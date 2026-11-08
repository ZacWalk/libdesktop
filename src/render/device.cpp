// This file is part of libdesktop, a C++20 platform layer for desktop applications
// Copyright 2026  Zac Walker
//
// This program is free software; you can redistribute it and / or modify it
// under the terms of the LGPL License either version 2.1 or later.
// License details are available at https://www.gnu.org/licenses/lgpl-2.1.html
// This program is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY

// Purpose: Process-wide renderer selection, font sizing, the live-window device registry, and the
// hooks and counters the application sets and reads.

#include "libdesktop/render.h"

// Off until a backend that can sample them says otherwise.
bool render::yuv_textures_enabled = false;
bool render::yuv_textures_allowed = true;
void (*render::on_yuv_fault)() = nullptr;
bool render::p010_textures_enabled = false;

std::function<core::surface_ptr(core::cspan)> render::decode_embedded_image;
std::function<std::unique_ptr<render::surface_scaler>()> render::make_surface_scaler;

render::ui_counters render::ui_perf;
render::gpu_counters render::gpu_perf;

core::surface_ptr render::decode_embedded(const core::cspan data)
{
	return render::decode_embedded_image ? render::decode_embedded_image(data) : nullptr;
}

namespace render::detail
{
	// The platform backend owns its factories and supplies these three operations. Keeping their
	// seam here means the portable device-loss sequence is the same on every platform.
	bool backend_startup(bool use_gpu);
	void backend_shutdown();
	bool backend_downgrade();
	bool backend_gpu_active();
}

namespace
{
	struct device_registration
	{
		void* key = nullptr;
		std::function<void()> prepare_loss;
		std::function<void()> rebuild_loss;
		std::function<void()> update_font;
		std::function<void()> invalidate;
	};

	std::vector<device_registration> live_devices;
	int process_base_font_size = render::default_font_size;
	bool renderer_started = false;
	bool renderer_shutdown = false;
	bool renderer_initialised = false;
	bool renderer_gpu_active = false;
	bool handling_device_loss = false;
}

namespace render
{
	bool startup(const bool gpu)
	{
		if (renderer_shutdown) return false;
		if (renderer_started) return renderer_initialised;

		renderer_started = true;
		renderer_initialised = detail::backend_startup(gpu);
		renderer_gpu_active = renderer_initialised && detail::backend_gpu_active();
		if (renderer_initialised)
		{
			platform::add_shutdown_hook([] { render::shutdown(); });
		}
		return renderer_initialised;
	}

	void shutdown()
	{
		if (!renderer_started || renderer_shutdown) return;

		renderer_shutdown = true;
		renderer_gpu_active = false;
		detail::backend_shutdown();
	}

	bool gpu_active()
	{
		return renderer_gpu_active;
	}

	void set_base_font_size(const int size)
	{
		if (process_base_font_size == size) return;

		process_base_font_size = size;
		const auto devices = live_devices;

		for (const auto& device : devices)
		{
			if (device.update_font) device.update_font();
		}

		for (const auto& device : devices)
		{
			if (device.invalidate) device.invalidate();
		}
	}

	int base_font_size()
	{
		return process_base_font_size;
	}
}

namespace render::detail
{
	void register_window_device(void* key, std::function<void()> prepare_loss,
	                            std::function<void()> rebuild_loss, std::function<void()> update_font,
	                            std::function<void()> invalidate)
	{
		std::erase_if(live_devices, [key](const device_registration& device) { return device.key == key; });
		live_devices.push_back({key, std::move(prepare_loss), std::move(rebuild_loss), std::move(update_font),
		                        std::move(invalidate)});
	}

	void unregister_window_device(void* key)
	{
		std::erase_if(live_devices, [key](const device_registration& device) { return device.key == key; });
	}

	void device_lost()
	{
		if (!renderer_gpu_active || handling_device_loss) return;

		// Device recovery is deliberately queued: the failing draw/resize/present still owns live
		// contexts and swap-chain references on its stack. The UI loop runs the teardown after that
		// call has returned, including inside a modal loop.
		handling_device_loss = true;
		platform::run_ui([]
		{
			if (!renderer_gpu_active)
			{
				handling_device_loss = false;
				return;
			}

			// The application must release its GPU-backed resources before the factory drops the
			// device. This is synchronous because run_ui delivered this task on the UI thread.
			platform::send_system_event(platform::os_event_type::graphics_device_lost);
			const auto devices = live_devices;

			for (const auto& device : devices)
			{
				if (device.prepare_loss) device.prepare_loss();
			}

			if (!backend_downgrade())
			{
				handling_device_loss = false;
				return;
			}

			renderer_gpu_active = false;

			for (const auto& device : devices)
			{
				if (device.rebuild_loss) device.rebuild_loss();
			}

			for (const auto& device : devices)
			{
				if (device.invalidate) device.invalidate();
			}

			handling_device_loss = false;
		});
	}
}
