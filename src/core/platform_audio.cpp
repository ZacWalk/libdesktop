// platform_audio.cpp — pf::'s sounds and streams: the WAV reader, the mixer, and the device thread that
// plays the mix through libdesktop-os's endpoint. Must NOT include OS headers: only the endpoint is per
// system, so this file goes wherever libdesktop-os does.

#include "platform_audio.h"

#include "libdesktop/os.h"

#include <algorithm>
#include <atomic>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstring>
#include <deque>
#include <future>
#include <thread>

namespace
{
	constexpr uint32_t four_cc(const char a, const char b, const char c, const char d)
	{
		return static_cast<uint8_t>(a) | static_cast<uint8_t>(b) << 8 |
			static_cast<uint8_t>(c) << 16 | static_cast<uint32_t>(static_cast<uint8_t>(d)) << 24;
	}

	uint16_t read_u16(const uint8_t* p)
	{
		return static_cast<uint16_t>(p[0] | p[1] << 8);
	}

	uint32_t read_u32(const uint8_t* p)
	{
		return p[0] | p[1] << 8 | p[2] << 16 | static_cast<uint32_t>(p[3]) << 24;
	}

	constexpr uint16_t wave_format_pcm = 1;
	constexpr uint16_t wave_format_ieee_float = 3;
	constexpr uint16_t wave_format_extensible = 0xFFFE;

	// KSDATAFORMAT_SUBTYPE_PCM and _IEEE_FLOAT begin with the format tag they stand for; these are the
	// fourteen bytes that follow it in both.
	constexpr uint8_t ks_subtype_tail[14] = {
		0x00, 0x00, 0x00, 0x00, 0x10, 0x00, 0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71
	};

	// The bounds XAudio2 put on a voice's frequency ratio, which pitching has always been held to.
	constexpr double min_ratio = 1.0 / 1024.0;
	constexpr double max_ratio = 8.0;

	float decode_sample(const uint8_t* p, const uint32_t bits, const bool is_float)
	{
		switch (bits)
		{
		case 8: return (static_cast<int>(p[0]) - 128) / 128.0f;
		case 16: return static_cast<int16_t>(read_u16(p)) / 32768.0f;
		case 24:
			return static_cast<int32_t>(static_cast<uint32_t>(p[0]) << 8 | static_cast<uint32_t>(p[1]) << 16 |
				static_cast<uint32_t>(p[2]) << 24) / 2147483648.0f;
		default:
			return is_float
				       ? std::bit_cast<float>(read_u32(p))
				       : static_cast<float>(static_cast<int32_t>(read_u32(p)) / 2147483648.0);
		}
	}

	class buffer_voice final : public pf::sound_buffer, public pf::audio::voice
	{
		// Declared first, so released last: what plays the mixer outlives the voice's hold on it.
		const std::shared_ptr<void> _owner;
		const pf::audio::mixer_ptr _mixer;
		const pf::audio::wav_sound _sound;
		const uint32_t _frames;

		// Guarded by the mixer's mutex.
		double _position = 0.0; // source frames into the sample, while playing
		uint32_t _start_frame = 0; // where the next play() begins
		double _ratio = 1.0;
		float _gain = 1.0f;
		// A mono sound goes to both sides at full scale until it is panned, as XAudio2 sent it.
		float _left = 1.0f;
		float _right = 1.0f;
		bool _looping = false;
		bool _playing = false;

	public:
		buffer_voice(std::shared_ptr<void> owner, pf::audio::mixer_ptr mixer, pf::audio::wav_sound sound)
			: _owner(std::move(owner)), _mixer(std::move(mixer)), _sound(std::move(sound)), _frames(_sound.frames())
		{
			_mixer->add(this);
		}

		~buffer_voice() override
		{
			_mixer->remove(this);
		}

		buffer_voice(const buffer_voice&) = delete;
		buffer_voice& operator=(const buffer_voice&) = delete;

		void play(const bool loop) override
		{
			std::lock_guard lock(_mixer->mutex());
			if (_playing) return; // re-triggering a running sound must not restart it

			_looping = loop;
			_position = _start_frame;
			_playing = true;
		}

		void stop() override
		{
			std::lock_guard lock(_mixer->mutex());
			_playing = false;
		}

		void set_frequency(const uint32_t hz) override
		{
			std::lock_guard lock(_mixer->mutex());
			_ratio = std::clamp(static_cast<double>(hz) / _sound.sample_rate, min_ratio, max_ratio);
		}

		void set_volume(const float gain) override
		{
			std::lock_guard lock(_mixer->mutex());
			_gain = gain;
		}

		void set_pan(const float pan) override
		{
			std::lock_guard lock(_mixer->mutex());

			// A stereo sound keeps its own sides: XAudio2 refused a one-channel pan matrix for it.
			if (_sound.channels != 1) return;

			// Matches DirectSound: panning attenuates one side rather than boosting the other.
			const auto clamped = std::clamp(pan, -1.0f, 1.0f);
			_left = std::min(1.0f, 1.0f - clamped);
			_right = std::min(1.0f, 1.0f + clamped);
		}

		[[nodiscard]] std::optional<uint32_t> play_position() const override
		{
			std::lock_guard lock(_mixer->mutex());
			if (_frames == 0) return 0u;

			const auto frame = _playing ? static_cast<uint32_t>(_position) : _start_frame;
			return frame % _frames * _sound.block_align;
		}

		void set_play_position(const uint32_t pos) override
		{
			std::lock_guard lock(_mixer->mutex());
			_start_frame = _frames == 0 ? 0 : pos / _sound.block_align % _frames;

			// A running sound carries on from there.
			if (_playing) _position = _start_frame;
		}

		void mix(float* const bus, const uint32_t frames, const uint32_t bus_channels,
		         const uint32_t output_rate) override
		{
			if (!_playing || _frames == 0 || output_rate == 0) return;

			const auto step = _ratio * _sound.sample_rate / output_rate;
			const auto* const samples = _sound.samples.data();
			const auto channels = _sound.channels;

			for (uint32_t i = 0; i < frames; ++i)
			{
				// Linear interpolation toward the next frame: the first again when looping, silence at the end.
				const auto index = static_cast<uint32_t>(_position);
				const auto fraction = static_cast<float>(_position - index);
				const auto has_next = index + 1 < _frames || _looping;
				const auto next = index + 1 < _frames ? index + 1 : 0u;

				const auto at = [&](const uint32_t channel)
				{
					const auto a = samples[static_cast<size_t>(index) * channels + channel];
					const auto b = has_next ? samples[static_cast<size_t>(next) * channels + channel] : 0.0f;
					return (a + (b - a) * fraction) * _gain;
				};

				auto* const out = bus + static_cast<size_t>(i) * bus_channels;

				if (channels == 1)
				{
					const auto v = at(0);

					if (bus_channels == 1)
					{
						out[0] += v;
					}
					else
					{
						out[0] += v * _left;
						out[1] += v * _right;
					}
				}
				else
				{
					const auto l = at(0);
					const auto r = at(1);

					if (bus_channels == 1)
					{
						out[0] += (l + r) * 0.5f;
					}
					else
					{
						out[0] += l;
						out[1] += r;
					}
				}

				_position += step;

				if (_position >= _frames)
				{
					if (!_looping)
					{
						_playing = false;
						return;
					}

					_position = std::fmod(_position, static_cast<double>(_frames));
				}
			}
		}
	};

	class stream_voice final : public pf::audio_stream, public pf::audio::voice
	{
		// Declared first, so released last: what plays the mixer outlives the voice's hold on it.
		const std::shared_ptr<void> _owner;
		const pf::audio::mixer_ptr _mixer;
		const uint32_t _rate;
		const uint32_t _channels;
		const size_t _max_blocks;

		// Guarded by the mixer's mutex.
		std::deque<std::vector<int16_t>> _blocks;
		// Blocks played out, kept to be written into again rather than freed on the device's thread.
		std::vector<std::vector<int16_t>> _spare;
		size_t _read = 0; // frames of the front block taken
		// The two frames the output lies between, and how far it is from the first toward the second. A
		// frame is taken when the output reaches it, so a block's frames are taken at the stream's rate
		// and a block counts as queued until its last frame has been.
		float _previous[2] = {};
		float _current[2] = {};
		double _phase = 1.0;
		float _gain = 1.0f;

		void take_frame()
		{
			_previous[0] = _current[0];
			_previous[1] = _current[1];

			if (_blocks.empty())
			{
				// Starved: silence until the producer catches up, which then plays on from where it was.
				_current[0] = _current[1] = 0.0f;
				return;
			}

			auto& block = _blocks.front();
			const auto* const frame = block.data() + _read * _channels;
			_current[0] = frame[0] / 32768.0f;
			_current[1] = _channels > 1 ? frame[1] / 32768.0f : _current[0];

			if (++_read * _channels >= block.size())
			{
				_read = 0;
				_spare.push_back(std::move(block));
				_blocks.pop_front();
			}
		}

		[[nodiscard]] bool is_silent() const
		{
			return _blocks.empty() && _previous[0] == 0.0f && _previous[1] == 0.0f &&
				_current[0] == 0.0f && _current[1] == 0.0f;
		}

	public:
		stream_voice(std::shared_ptr<void> owner, pf::audio::mixer_ptr mixer, const uint32_t rate,
		             const uint32_t channels, const size_t max_blocks)
			: _owner(std::move(owner)), _mixer(std::move(mixer)), _rate(rate), _channels(channels),
			  _max_blocks(max_blocks)
		{
			_spare.reserve(max_blocks + 1);
			_mixer->add(this);
		}

		~stream_voice() override
		{
			_mixer->remove(this);
		}

		stream_voice(const stream_voice&) = delete;
		stream_voice& operator=(const stream_voice&) = delete;

		[[nodiscard]] int queued_blocks() const override
		{
			std::lock_guard lock(_mixer->mutex());
			return static_cast<int>(_blocks.size());
		}

		[[nodiscard]] bool can_write() const override
		{
			std::lock_guard lock(_mixer->mutex());
			return _blocks.size() < _max_blocks;
		}

		bool write(const std::span<const int16_t> samples) override
		{
			// A block that ends part-way through a frame is refused whole, as XAudio2 refused it.
			if (samples.empty() || samples.size() % _channels != 0) return false;

			std::lock_guard lock(_mixer->mutex());
			if (_blocks.size() >= _max_blocks) return false;

			std::vector<int16_t> block;

			if (!_spare.empty())
			{
				block = std::move(_spare.back());
				_spare.pop_back();
			}

			block.assign(samples.begin(), samples.end());
			_blocks.push_back(std::move(block));
			return true;
		}

		void set_volume(const float gain) override
		{
			std::lock_guard lock(_mixer->mutex());
			_gain = gain;
		}

		void stop() override
		{
			std::lock_guard lock(_mixer->mutex());

			while (!_blocks.empty())
			{
				_spare.push_back(std::move(_blocks.front()));
				_blocks.pop_front();
			}

			_read = 0;
			_previous[0] = _previous[1] = 0.0f;
			_current[0] = _current[1] = 0.0f;
			_phase = 1.0;
		}

		void mix(float* const bus, const uint32_t frames, const uint32_t bus_channels,
		         const uint32_t output_rate) override
		{
			// Nothing queued and nothing still sounding: nothing to add, and no time to keep.
			if (output_rate == 0 || is_silent()) return;

			const auto step = static_cast<double>(_rate) / output_rate;

			for (uint32_t i = 0; i < frames; ++i)
			{
				while (_phase >= 1.0)
				{
					_phase -= 1.0;
					take_frame();
				}

				const auto t = static_cast<float>(_phase);
				const auto left = (_previous[0] + (_current[0] - _previous[0]) * t) * _gain;
				const auto right = (_previous[1] + (_current[1] - _previous[1]) * t) * _gain;
				auto* const out = bus + static_cast<size_t>(i) * bus_channels;

				if (bus_channels == 1)
				{
					out[0] += _channels == 1 ? left : (left + right) * 0.5f;
				}
				else
				{
					out[0] += left;
					out[1] += right;
				}

				_phase += step;
			}
		}
	};

	std::optional<pf::audio::output_format> output_format_of(const os::audio_format& format)
	{
		if (format.sample_rate == 0 || format.channels == 0) return std::nullopt;

		pf::audio::output_format result;
		result.sample_rate = format.sample_rate;
		result.channels = format.channels;

		switch (format.sample_format)
		{
		case os::audio_sample_format::float32: result.format = pf::audio::sample_format::float32;
			return result;
		case os::audio_sample_format::int16: result.format = pf::audio::sample_format::int16;
			return result;
		case os::audio_sample_format::int32: result.format = pf::audio::sample_format::int32;
			return result;
		case os::audio_sample_format::none: break;
		}

		return std::nullopt;
	}

	// The default endpoint, and the thread that keeps its ring full of the mix from sound_init until both
	// sound_shutdown and the last sound or stream made on it have let it go. Like XAudio2's engine it runs
	// throughout, silence and all, so a sound starts within a device period of play().
	class engine
	{
		const pf::audio::mixer_ptr _mixer = std::make_shared<pf::audio::mixer>();
		std::atomic<bool> _quit = false;
		std::thread _thread;

		// The default endpoint, in a format the mixer writes, or none.
		static os::audio_output_ptr open(std::optional<pf::audio::output_format>& format)
		{
			auto endpoint = os::create_audio_output({});
			format = endpoint ? output_format_of(endpoint->format()) : std::nullopt;
			if (!format) endpoint.reset();
			return endpoint;
		}

		void run(const std::shared_ptr<std::promise<bool>>& opened)
		{
			const os::audio_thread thread;
			std::optional<pf::audio::output_format> format;
			auto endpoint = open(format);
			opened->set_value(endpoint != nullptr);

			std::vector<uint8_t> frames;

			while (endpoint && !_quit)
			{
				if (endpoint->is_device_lost())
				{
					// The device went - unplugged, disabled, or its audio service restarted. Whichever
					// endpoint is the default now carries on once there is one; the voices hold still
					// meanwhile, so a stream's producer waits as it would for a slow device.
					endpoint.reset();

					while (!_quit && !endpoint)
					{
						std::this_thread::sleep_for(std::chrono::milliseconds(250));
						if (!_quit) endpoint = open(format);
					}

					continue;
				}

				if (const auto room = endpoint->writable_frames(); room > 0)
				{
					frames.resize(static_cast<size_t>(room) * format->frame_bytes());
					_mixer->render(frames, *format);
					endpoint->write(frames);
				}

				if (endpoint->is_stopped())
				{
					endpoint->start();
				}

				endpoint->wait_for_buffer(nullptr, 50);
			}
		}

	public:
		engine() = default;
		engine(const engine&) = delete;
		engine& operator=(const engine&) = delete;

		~engine()
		{
			_quit = true;
			if (_thread.joinable()) _thread.join();
		}

		[[nodiscard]] const pf::audio::mixer_ptr& mixer() const
		{
			return _mixer;
		}

		// False, and no thread left running, when there is no endpoint to play through.
		bool start()
		{
			const auto opened = std::make_shared<std::promise<bool>>();
			auto result = opened->get_future();
			_thread = std::thread([this, opened] { run(opened); });

			if (result.get()) return true;

			_thread.join();
			return false;
		}
	};

	// What a sound or stream holds keeps the device playing, so the last of sound_shutdown and the voices
	// it made closes it.
	std::shared_ptr<engine> s_engine;
}

std::optional<pf::audio::wav_sound> pf::audio::read_wav(const std::span<const uint8_t> wav)
{
	if (wav.size() < 12) return std::nullopt;

	const uint32_t riff_length = read_u32(wav.data() + 4);

	if (read_u32(wav.data()) != four_cc('R', 'I', 'F', 'F') || read_u32(wav.data() + 8) != four_cc('W', 'A', 'V', 'E'))
		return std::nullopt;
	if (riff_length < 4 || riff_length > wav.size() - 8)
		return std::nullopt;

	// Every length is checked against the declared RIFF size, so malformed input cannot read past the end.
	const uint8_t* format = nullptr;
	uint32_t format_length = 0;
	const uint8_t* data = nullptr;
	uint32_t data_size = 0;
	const size_t end = static_cast<size_t>(riff_length) + 8;
	size_t offset = 12;

	while (offset + 8 <= end && !(format && data))
	{
		const uint32_t type = read_u32(wav.data() + offset);
		const uint32_t length = read_u32(wav.data() + offset + 4);
		offset += 8;
		if (length > end - offset) return std::nullopt;

		if (type == four_cc('f', 'm', 't', ' ') && !format)
		{
			if (length < 16) return std::nullopt;
			format = wav.data() + offset;
			format_length = length;
		}
		else if (type == four_cc('d', 'a', 't', 'a') && !data)
		{
			data = wav.data() + offset;
			data_size = length;
		}

		if (format && data) break;

		const size_t padded = static_cast<size_t>(length) + (length & 1u);
		if (padded > end - offset) return std::nullopt;
		offset += padded;
	}

	if (!format || !data) return std::nullopt;

	const auto tag = read_u16(format);
	const auto channels = read_u16(format + 2);
	const auto sample_rate = read_u32(format + 4);
	const auto block_align = read_u16(format + 12);
	const auto bits = read_u16(format + 14);

	auto kind = tag;

	if (tag == wave_format_extensible)
	{
		// cbSize, then the valid bits, the channel mask and the sub-format: 40 bytes in all.
		if (format_length < 40 || read_u16(format + 16) < 22) return std::nullopt;

		const auto* const sub_format = format + 24;
		if (memcmp(sub_format + 2, ks_subtype_tail, sizeof(ks_subtype_tail)) != 0) return std::nullopt;
		kind = read_u16(sub_format);
	}

	const auto is_float = kind == wave_format_ieee_float && bits == 32;
	const auto is_pcm = kind == wave_format_pcm && (bits == 8 || bits == 16 || bits == 24 || bits == 32);
	if (!is_float && !is_pcm) return std::nullopt;
	if (channels < 1 || channels > 2 || sample_rate == 0) return std::nullopt;

	const uint32_t sample_bytes = bits / 8u;
	if (block_align != channels * sample_bytes) return std::nullopt;

	const auto frames = data_size / block_align;
	if (frames == 0) return std::nullopt;

	wav_sound result;
	result.sample_rate = sample_rate;
	result.channels = channels;
	result.block_align = block_align;
	result.samples.resize(static_cast<size_t>(frames) * channels);

	for (size_t i = 0; i < result.samples.size(); ++i)
	{
		result.samples[i] = decode_sample(data + i * sample_bytes, bits, is_float);
	}

	return result;
}

void pf::audio::mixer::add(voice* const v)
{
	std::lock_guard lock(_mutex);
	_voices.push_back(v);
}

void pf::audio::mixer::remove(voice* const v)
{
	std::lock_guard lock(_mutex);
	std::erase(_voices, v);
}

void pf::audio::mixer::render(const std::span<uint8_t> out, const output_format& format)
{
	const auto frame_bytes = format.frame_bytes();
	if (frame_bytes == 0) return;

	const auto frames = static_cast<uint32_t>(out.size() / frame_bytes);
	const auto bus_channels = std::min<uint32_t>(format.channels, 2);

	std::lock_guard lock(_mutex);
	_bus.assign(static_cast<size_t>(frames) * bus_channels, 0.0f);

	for (auto* const v : _voices)
	{
		v->mix(_bus.data(), frames, bus_channels, format.sample_rate);
	}

	auto* p = out.data();

	for (uint32_t i = 0; i < frames; ++i)
	{
		for (uint32_t c = 0; c < format.channels; ++c)
		{
			auto x = c < bus_channels ? _bus[static_cast<size_t>(i) * bus_channels + c] : 0.0f;
			x = std::isnan(x) ? 0.0f : std::clamp(x, -1.0f, 1.0f);

			switch (format.format)
			{
			case sample_format::float32:
				memcpy(p, &x, sizeof(x));
				p += sizeof(x);
				break;

			case sample_format::int16:
				{
					const auto s = static_cast<int16_t>(std::lrint(x * 32767.0f));
					memcpy(p, &s, sizeof(s));
					p += sizeof(s);
					break;
				}

			case sample_format::int32:
				{
					const auto s = static_cast<int32_t>(std::llrint(x * 2147483647.0));
					memcpy(p, &s, sizeof(s));
					p += sizeof(s);
					break;
				}
			}
		}
	}
}

pf::sound_buffer_ptr pf::audio::create_sound_buffer(const mixer_ptr& mixer, const std::span<const uint8_t> wav,
                                                    std::shared_ptr<void> owner)
{
	auto sound = read_wav(wav);
	if (!mixer || !sound) return nullptr;
	return std::make_shared<buffer_voice>(std::move(owner), mixer, std::move(*sound));
}

pf::audio_stream_ptr pf::audio::create_audio_stream(const mixer_ptr& mixer, const uint32_t sample_rate,
                                                    const int channels, const int max_blocks,
                                                    std::shared_ptr<void> owner)
{
	if (!mixer || sample_rate == 0 || channels < 1 || channels > 2 || max_blocks < 1) return nullptr;
	return std::make_shared<stream_voice>(std::move(owner), mixer, sample_rate, static_cast<uint32_t>(channels),
	                                      static_cast<size_t>(max_blocks));
}

bool pf::sound_init()
{
	if (s_engine) return true;

	auto started = std::make_shared<engine>();
	if (!started->start()) return false;

	s_engine = std::move(started);
	return true;
}

void pf::sound_shutdown()
{
	// A sound or stream still held keeps the device, and plays on until it goes, as XAudio2's did.
	s_engine.reset();
}

pf::sound_buffer_ptr pf::create_sound_buffer(const std::span<const uint8_t> wav)
{
	if (!s_engine) return nullptr;
	return audio::create_sound_buffer(s_engine->mixer(), wav, s_engine);
}

pf::audio_stream_ptr pf::create_audio_stream(const uint32_t sample_rate, const int channels, const int max_blocks)
{
	if (!s_engine) return nullptr;
	return audio::create_audio_stream(s_engine->mixer(), sample_rate, channels, max_blocks, s_engine);
}
