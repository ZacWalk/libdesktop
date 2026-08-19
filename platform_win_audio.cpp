// platform_win_audio.cpp — XAudio2 implementation of the pf:: audio API.
// Kept apart from platform_win.cpp so the audio dependency stays isolated.

#include "platform.h"

#include <algorithm>
#include <format>
#include <optional>
#include <vector>

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <mmreg.h>
#include <xaudio2.h>

#pragma comment(lib, "xaudio2.lib")

namespace
{
	// Source voices are created with this headroom because the default cap is
	// 2.0, which silently clamps anything pitched up more than an octave.
	constexpr float max_freq_ratio = 8.0f;

	constexpr uint32_t four_cc(const char a, const char b, const char c, const char d)
	{
		return static_cast<uint8_t>(a) | static_cast<uint8_t>(b) << 8 |
			static_cast<uint8_t>(c) << 16 | static_cast<uint8_t>(d) << 24;
	}

	// Owns the engine so a sound_buffer can outlive sound_shutdown() without
	// destroying its voice against a released engine.
	struct audio_device
	{
		IXAudio2* engine = nullptr;
		IXAudio2MasteringVoice* master = nullptr;
		uint32_t output_channels = 2;

		~audio_device()
		{
			if (master) master->DestroyVoice();
			if (engine) engine->Release();
		}
	};

	std::shared_ptr<audio_device> s_device;

	uint32_t read_dword(const uint8_t* data)
	{
		uint32_t value;
		memcpy(&value, data, sizeof(value));
		return value;
	}

	struct wav_contents
	{
		const WAVEFORMATEX* format = nullptr;
		const uint8_t* data = nullptr;
		uint32_t data_size = 0;
	};

	// Walks the RIFF chunks for 'fmt ' and 'data'. Every length is checked
	// against the declared RIFF size, so malformed input cannot read past the end.
	std::optional<wav_contents> unpack_wav(const std::span<const uint8_t> wav)
	{
		if (wav.size() < 12) return std::nullopt;

		const uint32_t chunk_id = read_dword(wav.data());
		const uint32_t riff_length = read_dword(wav.data() + 4);
		uint32_t type = read_dword(wav.data() + 8);

		if (chunk_id != four_cc('R', 'I', 'F', 'F') || type != four_cc('W', 'A', 'V', 'E'))
			return std::nullopt;
		if (riff_length < 4 || riff_length > wav.size() - 8)
			return std::nullopt;

		wav_contents result;
		const size_t end = static_cast<size_t>(riff_length) + 8;
		size_t offset = 12;

		while (offset + 8 <= end)
		{
			type = read_dword(wav.data() + offset);
			const uint32_t length = read_dword(wav.data() + offset + 4);
			offset += 8;
			if (length > end - offset) return std::nullopt;

			const uint8_t* chunk = wav.data() + offset;

			if (type == four_cc('f', 'm', 't', ' ') && !result.format)
			{
				if (length < sizeof(PCMWAVEFORMAT)) return std::nullopt;
				result.format = reinterpret_cast<const WAVEFORMATEX*>(chunk);
			}
			else if (type == four_cc('d', 'a', 't', 'a') && !result.data)
			{
				result.data = chunk;
				result.data_size = length;
			}

			if (result.format && result.data)
				return result;

			const size_t padded = static_cast<size_t>(length) + (length & 1u);
			if (padded > end - offset) return std::nullopt;
			offset += padded;
		}

		return std::nullopt;
	}

	class win_sound_buffer final : public pf::sound_buffer
	{
		std::shared_ptr<audio_device> _device;
		IXAudio2SourceVoice* _voice = nullptr;

		// XAudio2 never copies sample data; the voice reads from here.
		std::vector<uint8_t> _samples;

		uint32_t _block_align = 1;
		uint32_t _source_rate = 1;
		uint32_t _frames = 0;

		uint32_t _start_frame = 0; // where the next play() begins
		uint64_t _start_samples = 0; // SamplesPlayed when playback last began
		bool _looping = false;
		bool _started = false;

		// BuffersQueued is updated asynchronously, so it can only be trusted to
		// report a sound that has finished on its own — not one we just stopped.
		[[nodiscard]] bool running() const
		{
			if (!_started) return false;

			XAUDIO2_VOICE_STATE state;
			_voice->GetState(&state, XAUDIO2_VOICE_NOSAMPLESPLAYED);
			return state.BuffersQueued > 0;
		}

		void submit(const bool loop) const
		{
			XAUDIO2_BUFFER buffer = {};
			buffer.pAudioData = _samples.data();
			buffer.AudioBytes = static_cast<UINT32>(_samples.size());
			buffer.PlayBegin = _start_frame;

			if (!loop)
			{
				buffer.Flags = XAUDIO2_END_OF_STREAM;
				_voice->SubmitSourceBuffer(&buffer);
				return;
			}

			// Looping from a mid-sample start is expressed as a one-shot tail
			// followed by an endlessly looping whole sample, which avoids
			// XAudio2's constraints on LoopBegin relative to PlayBegin.
			if (_start_frame != 0)
				_voice->SubmitSourceBuffer(&buffer);

			XAUDIO2_BUFFER whole = {};
			whole.pAudioData = _samples.data();
			whole.AudioBytes = static_cast<UINT32>(_samples.size());
			whole.LoopCount = XAUDIO2_LOOP_INFINITE;
			_voice->SubmitSourceBuffer(&whole);
		}

	public:
		win_sound_buffer(std::shared_ptr<audio_device> device, IXAudio2SourceVoice* voice,
		                 std::vector<uint8_t> samples, const WAVEFORMATEX& format)
			: _device(std::move(device)), _voice(voice), _samples(std::move(samples)),
			  _block_align(std::max<uint32_t>(1, format.nBlockAlign)),
			  _source_rate(std::max<uint32_t>(1, format.nSamplesPerSec))
		{
			_frames = static_cast<uint32_t>(_samples.size()) / _block_align;
		}

		~win_sound_buffer() override
		{
			if (_voice)
			{
				_voice->Stop(0);
				_voice->FlushSourceBuffers();
				_voice->DestroyVoice();
			}
		}

		win_sound_buffer(const win_sound_buffer&) = delete;
		win_sound_buffer& operator=(const win_sound_buffer&) = delete;

		void play(const bool loop) override
		{
			if (running()) return; // re-triggering a running sound must not restart it

			_looping = loop;

			XAUDIO2_VOICE_STATE state;
			_voice->GetState(&state, 0);
			_start_samples = state.SamplesPlayed;

			_voice->FlushSourceBuffers();
			submit(loop);
			_voice->Start(0);
			_started = true;
		}

		void stop() override
		{
			_started = false;
			_voice->Stop(0);
			_voice->FlushSourceBuffers();
		}

		void set_frequency(const uint32_t hz) override
		{
			const auto ratio = static_cast<float>(hz) / static_cast<float>(_source_rate);
			_voice->SetFrequencyRatio(std::clamp(ratio, XAUDIO2_MIN_FREQ_RATIO, max_freq_ratio));
		}

		void set_volume(const float gain) override
		{
			_voice->SetVolume(gain); // XAudio2 volume is already a linear amplitude
		}

		void set_pan(const float pan) override
		{
			const auto clamped = std::clamp(pan, -1.0f, 1.0f);
			const uint32_t channels = _device->output_channels;

			// Matches DirectSound: panning attenuates one side rather than
			// boosting the other.
			float levels[8] = {};
			if (channels == 1)
			{
				levels[0] = 1.0f;
			}
			else
			{
				levels[0] = std::min(1.0f, 1.0f - clamped);
				levels[1] = std::min(1.0f, 1.0f + clamped);
			}

			_voice->SetOutputMatrix(nullptr, 1, std::min<uint32_t>(channels, 8), levels);
		}

		[[nodiscard]] std::optional<uint32_t> play_position() const override
		{
			if (_frames == 0) return 0u;

			// SamplesPlayed keeps its total across stops, so only a running
			// voice can be measured against the last start.
			if (!running()) return _start_frame * _block_align;

			XAUDIO2_VOICE_STATE state;
			_voice->GetState(&state, 0);

			const uint64_t played = state.SamplesPlayed - _start_samples;
			const auto frame = static_cast<uint32_t>((_start_frame + played) % _frames);
			return frame * _block_align;
		}

		void set_play_position(const uint32_t pos) override
		{
			const uint32_t frame = _frames == 0 ? 0 : pos / _block_align % _frames;
			const bool was_running = running();

			if (was_running) stop();
			_start_frame = frame;
			if (was_running) play(_looping);
		}
	};

	class win_audio_stream final : public pf::audio_stream
	{
		std::shared_ptr<audio_device> _device;
		IXAudio2SourceVoice* _voice = nullptr;

		// XAudio2 never copies sample data, so each queued block has to stay
		// put until the voice is done with it. A ring of exactly max_blocks
		// slots makes that automatic: while fewer than max are queued the
		// oldest slot — the next one in ring order — has already been played.
		std::vector<std::vector<int16_t>> _blocks;
		int _next = 0;
		bool _started = false;

	public:
		win_audio_stream(std::shared_ptr<audio_device> device, IXAudio2SourceVoice* voice,
		                 const int max_blocks)
			: _device(std::move(device)), _voice(voice), _blocks(max_blocks)
		{
		}

		~win_audio_stream() override
		{
			if (_voice)
			{
				_voice->Stop(0);
				_voice->FlushSourceBuffers();
				_voice->DestroyVoice();
			}
		}

		win_audio_stream(const win_audio_stream&) = delete;
		win_audio_stream& operator=(const win_audio_stream&) = delete;

		[[nodiscard]] int queued_blocks() const override
		{
			XAUDIO2_VOICE_STATE state;
			_voice->GetState(&state, XAUDIO2_VOICE_NOSAMPLESPLAYED);
			return static_cast<int>(state.BuffersQueued);
		}

		[[nodiscard]] bool can_write() const override
		{
			return queued_blocks() < static_cast<int>(_blocks.size());
		}

		bool write(const std::span<const int16_t> samples) override
		{
			if (samples.empty() || !can_write()) return false;

			auto& block = _blocks[_next];
			block.assign(samples.begin(), samples.end());

			XAUDIO2_BUFFER buffer = {};
			buffer.pAudioData = reinterpret_cast<const BYTE*>(block.data());
			buffer.AudioBytes = static_cast<UINT32>(block.size() * sizeof(int16_t));

			if (FAILED(_voice->SubmitSourceBuffer(&buffer))) return false;
			_next = (_next + 1) % static_cast<int>(_blocks.size());

			if (!_started)
			{
				_voice->Start(0);
				_started = true;
			}
			return true;
		}

		void set_volume(const float gain) override
		{
			_voice->SetVolume(gain);
		}

		void stop() override
		{
			_voice->Stop(0);
			_voice->FlushSourceBuffers();
			_started = false;
			_next = 0;
		}
	};
}

bool pf::sound_init()
{
	if (s_device) return true;

	// CreateMasteringVoice reaches WASAPI through COM and fails with
	// CO_E_NOTINITIALIZED otherwise, which a console-mode app has not
	// necessarily done. RPC_E_CHANGED_MODE means some other apartment model is
	// already in place, which is equally fine. Deliberately never balanced:
	// buffers can outlive sound_shutdown, and CoUninitialize is thread-bound.
	if (const auto hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
		FAILED(hr) && hr != RPC_E_CHANGED_MODE)
	{
		debug_trace(std::format("sound_init: CoInitializeEx failed (0x{:08X})\n",
		                        static_cast<uint32_t>(hr)));
		return false;
	}

	auto device = std::make_shared<audio_device>();

	if (const auto hr = XAudio2Create(&device->engine, 0, XAUDIO2_DEFAULT_PROCESSOR); FAILED(hr))
	{
		debug_trace(std::format("sound_init: XAudio2Create failed (0x{:08X})\n",
		                        static_cast<uint32_t>(hr)));
		return false;
	}

	if (const auto hr = device->engine->CreateMasteringVoice(&device->master); FAILED(hr))
	{
		debug_trace(std::format("sound_init: CreateMasteringVoice failed (0x{:08X})\n",
		                        static_cast<uint32_t>(hr)));
		return false;
	}

	XAUDIO2_VOICE_DETAILS details = {};
	device->master->GetVoiceDetails(&details);
	device->output_channels = std::max<uint32_t>(1, details.InputChannels);

	s_device = std::move(device);
	return true;
}

void pf::sound_shutdown()
{
	// Any buffer still alive holds its own reference, so the engine outlives it.
	s_device.reset();
}

pf::sound_buffer_ptr pf::create_sound_buffer(const std::span<const uint8_t> wav)
{
	if (!s_device) return nullptr;

	const auto contents = unpack_wav(wav);
	if (!contents || contents->data_size == 0) return nullptr;

	IXAudio2SourceVoice* voice = nullptr;
	if (FAILED(s_device->engine->CreateSourceVoice(&voice, contents->format, 0, max_freq_ratio)))
		return nullptr;

	std::vector<uint8_t> samples(contents->data, contents->data + contents->data_size);
	return std::make_shared<win_sound_buffer>(s_device, voice, std::move(samples), *contents->format);
}

pf::audio_stream_ptr pf::create_audio_stream(const uint32_t sample_rate, const int channels,
                                             const int max_blocks)
{
	if (!s_device || sample_rate == 0 || channels < 1 || max_blocks < 1) return nullptr;

	WAVEFORMATEX format = {};
	format.wFormatTag = WAVE_FORMAT_PCM;
	format.nChannels = static_cast<WORD>(channels);
	format.nSamplesPerSec = sample_rate;
	format.wBitsPerSample = 16;
	format.nBlockAlign = static_cast<WORD>(channels * 2);
	format.nAvgBytesPerSec = sample_rate * format.nBlockAlign;

	IXAudio2SourceVoice* voice = nullptr;
	if (FAILED(s_device->engine->CreateSourceVoice(&voice, &format)))
	{
		debug_trace("create_audio_stream: CreateSourceVoice failed\n");
		return nullptr;
	}

	return std::make_shared<win_audio_stream>(s_device, voice, max_blocks);
}
