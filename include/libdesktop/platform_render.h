// This file is part of libdesktop, a C++20 platform layer for desktop applications
// Copyright 2026  Zac Walker
// 
// This program is free software; you can redistribute it and / or modify it
// under the terms of the LGPL License either version 2.1 or later.
// License details are available at https://www.gnu.org/licenses/lgpl-2.1.html
// This program is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY

// Purpose: The platform services the desktop UI layer implements rather than the system layer: icon
// glyphs as surfaces, movie writing, audio output, window capture, and the rendering probes the tests
// read, with the counters of what painting and Direct3D cost. The Windows UI, WIC, font, software,
// sound and encode backends implement them, and platform_linux_ui.cpp answers for Linux. platform.h
// holds what the system layer implements.

#pragma once

namespace ui
{
	class image;
	class surface;
	using surface_ptr = std::shared_ptr<surface>;
	using const_surface_ptr = std::shared_ptr<const surface>;
	using image_ptr = std::shared_ptr<image>;
	using const_image_ptr = std::shared_ptr<const image>;
};

namespace df
{
	// What painting and Direct3D cost this session: bumped by the window layer and the renderers, and
	// summarised at exit by the application with its own counters.

	// UI-thread cost. Everything here competes with input handling, so the maxima matter more than
	// the totals: one long drain or paint is a visible stutter.
	struct ui_counters
	{
		std::atomic_uint64_t idle_drains = 0;
		std::atomic_uint64_t idle_tasks = 0;
		std::atomic_uint64_t idle_us = 0;
		std::atomic_uint32_t idle_max_us = 0;
		std::atomic_uint64_t idle_wait_us = 0;
		std::atomic_uint32_t idle_wait_max_us = 0;
		std::atomic_uint32_t idle_batch_peak = 0;
		std::atomic_uint64_t paints = 0;
		std::atomic_uint64_t paint_us = 0;
		std::atomic_uint32_t paint_max_us = 0;
		latency_counters paint_latency;
		// on_render only: walking the view tree and issuing draw calls, with no GPU work in it. Paint
		// time is this plus submit plus present, so measuring it directly says which of the three to
		// attack rather than leaving it to subtraction.
		std::atomic_uint64_t scene_build_us = 0;
		std::atomic_uint32_t scene_build_max_us = 0;
		latency_counters scene_build_latency;
		// The backend half of a paint. D3D submits here; software rendering rasterises and presents
		// here, so this is the backend-parity timing that paint minus scene-build could only imply.
		std::atomic_uint64_t backend_render_us = 0;
		std::atomic_uint32_t backend_render_max_us = 0;
		latency_counters backend_render_latency;
		std::atomic_uint64_t texture_uploads = 0;
		// What asked for a frame. prepares is the display-frequency timer, invalidates is anything
		// that dirtied a window and so will rebuild the scene, and redraws is the re-present path that
		// replays the last scene instead. Paints far above prepares means frames are being driven by
		// something other than the pacing.
		std::atomic_uint64_t frame_prepares = 0;
		std::atomic_uint64_t invalidates = 0;
		std::atomic_uint64_t redraws = 0;
		// Paints and redraws skipped because the window could not be seen: minimised, or behind the
		// secure desktop. Playing video while minimised should raise this, not paints.
		std::atomic_uint64_t hidden_skips = 0;
		// Which source kept asking for the next frame. An idle app should leave all three at zero;
		// anything else is an animation that never settles.
		std::atomic_uint64_t prepares_registered_anim = 0;
		std::atomic_uint64_t prepares_display_anim = 0;
		std::atomic_uint64_t prepares_display_invalid = 0;
		std::atomic_uint32_t animations_peak = 0;
	};

	// Direct3D frame cost, all bumped from the UI thread by the hardware backend. A user's log is
	// the only place this is ever observable - a profiler cannot be attached to their machine - so
	// it records what a frame cost, how well it batched, and what it had to build to draw it.
	struct gpu_counters
	{
		std::atomic_uint64_t frames = 0; // draw_scene calls; a redraw replays a scene without a paint
		std::atomic_uint64_t submit_us = 0;
		std::atomic_uint32_t submit_max_us = 0;
		latency_counters submit_latency;
		std::atomic_uint64_t present_us = 0;
		std::atomic_uint32_t present_max_us = 0;
		latency_counters present_latency;

		// draws vs merged is the batching ratio: every merge is a draw call and a state block that
		// building the scene managed to avoid.
		std::atomic_uint64_t draws = 0;
		std::atomic_uint64_t merged = 0;
		std::atomic_uint32_t draws_peak = 0;
		std::atomic_uint64_t geometry_bytes = 0;

		std::atomic_uint64_t shader_binds = 0;
		std::atomic_uint64_t view_binds = 0;
		std::atomic_uint64_t sampler_binds = 0;
		std::atomic_uint64_t cbuffer_uploads = 0;

		// Driver object creation. These should flatten out early in a session; totals that keep pace
		// with the frame count are the signature of something being rebuilt every frame.
		std::atomic_uint64_t views_created = 0;
		std::atomic_uint64_t targets_created = 0;
		std::atomic_uint64_t textures_created = 0;
		// Textures overwritten from the pool instead of created, with their views. Scrolling a listing
		// should raise this far more than textures_created.
		std::atomic_uint64_t textures_reused = 0;
		std::atomic_uint64_t buffers_created = 0;

		// Gauge, sampled periodically: what the adapter reports this process is using locally.
		std::atomic_uint32_t vram_mb = 0;
		std::atomic_uint32_t vram_peak_mb = 0;
	};

	inline void set_gauge(std::atomic_uint32_t& gauge, const uint32_t value)
	{
		gauge.store(value, std::memory_order_relaxed);
	}

	extern ui_counters ui_perf;
	extern gpu_counters gpu_perf;
}

namespace platform
{
	class thread_event;

	// Rasterises one glyph of the bundled icon font to a surface, for the places that need an image
	// rather than drawn text. Takes a code point, so it does not depend on the size of wchar_t.
	ui::const_surface_ptr create_icon_surface(char32_t ch);

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
		df::file_path path;
		sizei extent;
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
		virtual bool write_frame(const ui::const_surface_ptr& surface, double time) = 0;
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
		virtual void wait_for_buffer(const thread_event& wake_event, uint32_t timeout_ms) = 0;
		// Queues as many whole frames from the front of samples, interleaved in the endpoint's format,
		// as it has room for, and answers how many bytes it took.
		virtual size_t write(df::cspan samples) = 0;
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

	struct sound_device
	{
		std::string id;
		std::string name;
		bool is_current = false;
	};

	std::vector<sound_device> list_audio_playback_devices();

#ifdef _DEBUG
	ui::surface_ptr capture_window_surface(const std::any& window_handle);
#endif

	// What the system font stack knows about one character in two named font families.
	// Exposed so tests can assert the glyph-fallback contract (diffractor#219) without
	// reaching into the platform text API themselves.
	struct glyph_fallback_probe
	{
		// False when the query could not run at all (no text engine, or a family is
		// not installed on this machine). Callers must treat the rest as meaningless.
		bool available = false;
		// Glyph index the primary family maps the character to; 0 means "not covered",
		// which is what makes fallback mandatory.
		uint16_t primary_glyph = 0;
		// Glyph index in the fallback family. Non-zero when that family covers it.
		uint16_t fallback_glyph = 0;
		// True when metrics for fallback_glyph read from the fallback face succeeded.
		bool fallback_metrics_ok = false;
		// True when reading metrics for fallback_glyph from the PRIMARY face either
		// fails or returns different metrics -- i.e. the face confusion is real.
		bool primary_metrics_differ = false;
	};

	glyph_fallback_probe probe_glyph_fallback(std::string_view primary_family, std::string_view fallback_family,
	                                          char32_t code_point);

	// What the font-face cache does with repeated requests. Exposed so tests can assert the
	// caching contract without reaching into the renderer: a repeat request must be served
	// from the cache rather than re-running the family lookup and re-logging its failures
	// (diffractor#232), and a font-size change must produce a distinct face so glyphs cached for
	// the old size cannot be reused (diffractor#189).
	struct font_cache_probe
	{
		// False when no text engine could be created; the rest is then meaningless.
		bool available = false;
		// Entries held after the first request. One request must add exactly one entry.
		int entries_after_first = 0;
		// True when the identical request returned the same face and added no entry.
		bool same_request_is_cached = false;
		// True when the same face at a different size returned a different instance.
		bool size_change_is_distinct = false;
		// True when a different face type at the same size returned a different instance.
		bool face_change_is_distinct = false;
		// True when resetting the fonts emptied the cache, so a settings change cannot
		// leave stale faces behind.
		bool reset_clears_cache = false;
	};

	font_cache_probe probe_font_cache(int base_font_size);

	// Test-only probe of the double-buffered paint applied to native common controls. The buffering
	// relies on the control drawing itself on demand into a supplied device context. If a control
	// class ignored that request the control would blit an empty buffer and appear blank, so the
	// probe reports what each class actually drew into a pre-filled buffer.
	struct control_paint_probe
	{
		int trackbar_painted_pixels = 0; // pixels the trackbar changed from the pre-fill
		int trackbar_colors = 0; // distinct colors the trackbar left in the buffer
		int toolbar_painted_pixels = 0;
		int toolbar_colors = 0;
		int button_painted_pixels = 0;
		int button_colors = 0;
	};

	control_paint_probe probe_buffered_control_paint();

	// Test-only probe of the software backend's scratch buffer. The scratch tile stops being
	// reallocated once it reaches its final size, so growing the client no longer reallocates
	// anything. These record how much of a grown client the canvas will actually accept writes for,
	// summed over the tiles the real loop walks, against the buffer that stayed behind - which must
	// not have grown with the window. Whether tiling itself is seamless is a question about the
	// rasterizer rather than the platform, and is ui::probe_software_rasterizer.
	struct software_tiling_probe
	{
		int grown_client_pixels = 0;
		int grown_writable_pixels = 0;
		int grown_buffer_pixels = 0;
	};

	software_tiling_probe probe_software_tiling();
}
