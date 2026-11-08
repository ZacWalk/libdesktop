// This file is part of libdesktop, a C++20 platform layer for desktop applications
// Copyright 2026  Zac Walker
// 
// This program is free software; you can redistribute it and / or modify it
// under the terms of the LGPL License either version 2.1 or later.
// License details are available at https://www.gnu.org/licenses/lgpl-2.1.html
// This program is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY

// Purpose: Rendering and UI performance counters.

#pragma once

namespace render
{
	// GPU identity and memory budgets are renderer state, published by the active backend.
	extern std::string gpu_desc;
	extern std::string gpu_id;
	extern std::string d3d_info;
	extern int max_texture_dimension;
	extern int64_t max_texture_bytes;
	extern int64_t max_decode_bytes;

	// Written by whichever thread is inside the draw backend and read by diagnostics.
	extern std::atomic<const char*> rendering_func;

	class scope_rendering_func final : public core::no_copy
	{
		const char* _prev = "";

	public:
		scope_rendering_func(const char* f) : _prev(rendering_func.load(std::memory_order_relaxed))
		{
			rendering_func.store(f, std::memory_order_relaxed);
		}

		~scope_rendering_func() override
		{
			rendering_func.store(_prev, std::memory_order_relaxed);
		}
	};

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
		core::latency_counters paint_latency;
		// on_render only: walking the view tree and issuing draw calls, with no GPU work in it. Paint
		// time is this plus submit plus present, so measuring it directly says which of the three to
		// attack rather than leaving it to subtraction.
		std::atomic_uint64_t scene_build_us = 0;
		std::atomic_uint32_t scene_build_max_us = 0;
		core::latency_counters scene_build_latency;
		// The backend half of a paint. D3D submits here; software rendering rasterises and presents
		// here, so this is the backend-parity timing that paint minus scene-build could only imply.
		std::atomic_uint64_t backend_render_us = 0;
		std::atomic_uint32_t backend_render_max_us = 0;
		core::latency_counters backend_render_latency;
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
		core::latency_counters submit_latency;
		std::atomic_uint64_t present_us = 0;
		std::atomic_uint32_t present_max_us = 0;
		core::latency_counters present_latency;

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
