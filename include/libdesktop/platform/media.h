// This file is part of libdesktop, a C++20 platform layer for desktop applications
// Copyright 2026  Zac Walker
// 
// This program is free software; you can redistribute it and / or modify it
// under the terms of the LGPL License either version 2.1 or later.
// License details are available at https://www.gnu.org/licenses/lgpl-2.1.html
// This program is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY

// Purpose: Platform movie, audio, and window-capture services.

#pragma once

namespace platform
{
		///////////////////////////////////////////////////////////////////////////////////////////////
	// Writing a movie.
	//
	// The intention is "take these finished frames and samples and leave one playable video file at
	// this path". The application composites and decodes; the platform owns the codec, the container
	// and whatever hardware is behind them, since a codec is the system's to supply rather than the
	// application's to ship.
	///////////////////////////////////////////////////////////////////////////////////////////////

	struct movie_writer_request
	{
		core::file_path path;
		core::sizei extent;
		int frame_rate = 30;
		int video_bitrate = 0;
		bool with_audio = false;
		int sample_rate = 48000;
		int channels = 2;
		int audio_bitrate = 192000;
	};

	class movie_writer
	{
	public:
		virtual ~movie_writer() = default;

		// One composited frame, at the request's extent, shown from `time` seconds until the next.
		virtual bool write_frame(const core::const_surface_ptr& surface, double time) = 0;
		// Interleaved 16-bit samples at the request's rate and channel count.
		virtual bool write_audio(const int16_t* samples, size_t sample_count, double time) = 0;
		// Finishes the container. Until this returns true there is no output file, only a partial
		// one the writer owns and will remove.
		virtual bool close() = 0;
		// Abandons the write and removes the partial file. Idempotent, and what the destructor does
		// when close was never called, so a cancelled or thrown-out render leaves nothing behind.
		virtual void abandon() = 0;
		virtual std::string last_error() const = 0;
	};

	using movie_writer_ptr = std::shared_ptr<movie_writer>;

	// Asked of the running system rather than fixed at build time: the same build offers Render on
	// a machine with the required encoders and does not offer it on one without. The answer is
	// cached after the first probe, which starts the system's media framework and enumerates its
	// encoders -- about half a second -- so the first call belongs on a worker, never the UI thread.
	bool can_write_movies();
	movie_writer_ptr create_movie_writer(const movie_writer_request& request);

	///////////////////////////////////////////////////////////////////////////////////////////////
	// Playing sound.
	//
	// An endpoint opened in its own mix format, which takes interleaved samples in that format and
	// keeps the clock of what it has played. The media code describes its buffers in these terms, so
	// nothing here names a decoder.
	///////////////////////////////////////////////////////////////////////////////////////////////

	enum class audio_sample_format
	{
		none,
		float32,
		int16,
		int32
	};

	// An endpoint's mix format. Each frame holds channels samples; channel_mask names the speakers they
	// feed in WAVEFORMATEXTENSIBLE's bits, and is zero where the endpoint names none.
	struct audio_output_format
	{
		uint32_t sample_rate = 0;
		audio_sample_format sample_format = audio_sample_format::none;
		uint32_t channels = 0;
		uint64_t channel_mask = 0;
	};

	class audio_output
	{
	public:
		virtual ~audio_output() = default;

		virtual std::string id() = 0;
		virtual audio_output_format format() = 0;
		virtual double time() const = 0;
		virtual bool is_stopped() const = 0;
		virtual bool is_device_lost() const = 0;

		virtual void reset() = 0;
		virtual void start() = 0;
		virtual void stop() = 0;
		virtual void wait_for_buffer(const core::thread_event& wake_event, uint32_t timeout_ms) = 0;
		// Queues as many whole frames from the front of samples, interleaved in the endpoint's format,
		// as it has room for, and answers how many bytes it took.
		virtual size_t write(core::cspan samples) = 0;
		// Queues a short run of silence so an underrun plays silence instead of looping
		// the last buffer (used to keep the tail clean at end of stream).
		virtual void write_silence() = 0;
		virtual void volume(double x) = 0;
	};

	using audio_output_ptr = std::shared_ptr<audio_output>;

	// A ring of zero seconds is the endpoint's minimum: a device period or two, which only a thread
	// waiting on wait_for_buffer can keep full. A caller that refills on its own schedule instead -- the
	// Movie preview, from the UI tick -- asks for a ring long enough to cover the gap between refills,
	// and has no buffer event to wait on.
	audio_output_ptr create_audio_output(std::string_view device_id, double ring_seconds = 0.0);

	// A decoded short sound held by the mixer. A voice remains attached to its output engine until
	// the last owner releases it, so shutdown does not truncate an already-created sound.
	struct sound_buffer
	{
		virtual ~sound_buffer() = default;

		virtual void play(bool loop = false) = 0;
		virtual void stop() = 0;
		virtual void set_frequency(uint32_t hz) = 0;
		virtual void set_volume(float gain) = 0;
		virtual void set_pan(float pan) = 0;
		[[nodiscard]] virtual std::optional<uint32_t> play_position() const = 0;
		virtual void set_play_position(uint32_t pos) = 0;
	};

	using sound_buffer_ptr = std::shared_ptr<sound_buffer>;

	// The default output engine. A missing endpoint is reported by false and leaves creation returning
	// null; Linux has no platform audio endpoint yet.
	bool sound_init();
	void sound_shutdown();

	sound_buffer_ptr create_sound_buffer(std::span<const uint8_t> wav);

	struct audio_stream
	{
		virtual ~audio_stream() = default;

		[[nodiscard]] virtual int queued_blocks() const = 0;
		[[nodiscard]] virtual bool can_write() const = 0;
		virtual bool write(std::span<const int16_t> samples) = 0;
		virtual void set_volume(float gain) = 0;
		virtual void stop() = 0;
	};

	using audio_stream_ptr = std::shared_ptr<audio_stream>;

	audio_stream_ptr create_audio_stream(uint32_t sample_rate, int channels, int max_blocks = 4);

	struct sound_device
	{
		std::string id;
		std::string name;
		bool is_current = false;
	};

	std::vector<sound_device> list_audio_playback_devices();

	// What a window shows now, as a surface. Windows answers in Debug builds alone; Linux always, from
	// the window's own pixels.
#if defined(_DEBUG) || !defined(_WIN32)
	core::surface_ptr capture_window_surface(const std::any& window_handle);
#endif

}
