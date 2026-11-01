// platform_audio.h - the mixer pf::sound_buffer and pf::audio_stream play through: standard C++ over
// libdesktop-os's audio endpoint, the one engine libdesktop-system's platform::audio_output is too.
// Internal to libdesktop-core; its suite renders through the mixer here, without a device.

#pragma once

#include "platform.h"

#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <vector>

namespace pf::audio
{
	// A RIFF/WAVE file's frames as the mixer plays them, decoded to float.
	struct wav_sound
	{
		uint32_t sample_rate = 0;
		uint32_t channels = 0;
		// The bytes a frame takes in the file. Play positions count these, as they did under XAudio2.
		uint32_t block_align = 0;
		// Interleaved, from -1 to 1.
		std::vector<float> samples;

		[[nodiscard]] uint32_t frames() const
		{
			return channels == 0 ? 0 : static_cast<uint32_t>(samples.size() / channels);
		}
	};

	// PCM of 8 (unsigned), 16, 24 or 32 bits, or 32-bit float, in one channel or two, described by a
	// WAVEFORMATEX or a WAVEFORMATEXTENSIBLE. Anything else - compressed, more channels, malformed, or
	// without a whole frame - is empty.
	std::optional<wav_sound> read_wav(std::span<const uint8_t> wav);

	enum class sample_format
	{
		float32,
		int16,
		int32
	};

	// An endpoint's frames: what render writes.
	struct output_format
	{
		uint32_t sample_rate = 0;
		uint32_t channels = 0;
		sample_format format = sample_format::float32;

		[[nodiscard]] uint32_t frame_bytes() const
		{
			return channels * (format == sample_format::int16 ? 2u : 4u);
		}
	};

	// A sound or a stream in the mix. A voice's state is guarded by its mixer's mutex, which render holds
	// while it mixes, so a voice changes between two renders and never during one.
	class voice
	{
	public:
		virtual ~voice() = default;

		// Adds this voice's next frames into bus - frames frames of bus_channels, one or two, interleaved -
		// at output_rate, and moves the voice on by as much.
		virtual void mix(float* bus, uint32_t frames, uint32_t bus_channels, uint32_t output_rate) = 0;
	};

	class mixer
	{
		std::mutex _mutex;
		std::vector<voice*> _voices;
		std::vector<float> _bus;

	public:
		// Fills out with as many whole frames in format as it holds: every voice mixed, clipped, and moved
		// on by as many frames. A one-channel endpoint takes a mono mix; any other, stereo in its first two
		// channels and silence in the rest.
		void render(std::span<uint8_t> out, const output_format& format);

		std::mutex& mutex() { return _mutex; }
		void add(voice* v);
		void remove(voice* v);
	};

	using mixer_ptr = std::shared_ptr<mixer>;

	// A sound or a stream in mixer's mix. It keeps the mixer alive, and owner with it: pf::create_sound_buffer
	// and pf::create_audio_stream make these on the device's mixer, with the device as owner, so one held
	// past sound_shutdown plays on, as it did under XAudio2.
	sound_buffer_ptr create_sound_buffer(const mixer_ptr& mixer, std::span<const uint8_t> wav,
	                                     std::shared_ptr<void> owner = {});
	audio_stream_ptr create_audio_stream(const mixer_ptr& mixer, uint32_t sample_rate, int channels, int max_blocks,
	                                     std::shared_ptr<void> owner = {});
}
