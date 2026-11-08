// This file is part of libdesktop, a C++20 platform layer for desktop applications
// Copyright 2026  Zac Walker
// 
// This program is free software; you can redistribute it and / or modify it
// under the terms of the LGPL License either version 2.1 or later.
// License details are available at https://www.gnu.org/licenses/lgpl-2.1.html
// This program is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY

// Purpose: Windows audio output. platform::audio_output is the os:: services' WASAPI endpoint, the one
// engine libdesktop's own sounds and streams play through as well; this wraps it in the platform::
// types the media code is written in.

#include "libdesktop/platform.h"
#include "libdesktop/platform/win32.h"
#include "os.h"

#include <mmreg.h>

namespace
{
	platform::audio_sample_format to_platform(const os::audio_sample_format format)
	{
		switch (format)
		{
		case os::audio_sample_format::float32: return platform::audio_sample_format::float32;
		case os::audio_sample_format::int16: return platform::audio_sample_format::int16;
		case os::audio_sample_format::int32: return platform::audio_sample_format::int32;
		case os::audio_sample_format::none: break;
		}

		return platform::audio_sample_format::none;
	}

	platform::audio_output_format to_platform(const os::audio_format& format)
	{
		platform::audio_output_format result;
		result.sample_rate = format.sample_rate;
		result.sample_format = to_platform(format.sample_format);
		result.channels = format.channels;
		result.channel_mask = format.channel_mask;
		return result;
	}

	class os_audio_output final : public platform::audio_output
	{
		const os::audio_output_ptr _native;

	public:
		explicit os_audio_output(os::audio_output_ptr native) : _native(std::move(native))
		{
		}

		std::string id() override { return _native->id(); }
		platform::audio_output_format format() override { return to_platform(_native->format()); }
		double time() const override { return _native->time(); }
		bool is_stopped() const override { return _native->is_stopped(); }
		bool is_device_lost() const override { return _native->is_device_lost(); }

		void reset() override { _native->reset(); }
		void start() override { _native->start(); }
		void stop() override { _native->stop(); }

		void wait_for_buffer(const core::thread_event& wake_event, const uint32_t timeout_ms) override
		{
			_native->wait_for_buffer(wake_event._h, timeout_ms);
		}

		size_t write(const core::cspan samples) override
		{
			return _native->write({samples.data, samples.data ? samples.size : 0});
		}

		void write_silence() override { _native->write_silence(); }
		void volume(const double x) override { _native->volume(x); }
	};
}

// The endpoint's mix format as the platform describes it. Not a member of the device, so the tests can
// describe a format without an endpoint.
platform::audio_output_format wasapi_mix_format(const WAVEFORMATEX* format)
{
	return to_platform(os::describe_wave_format(format));
}

platform::audio_output_ptr platform::create_audio_output(const std::string_view device_id, const double ring_seconds)
{
	auto native = os::create_audio_output(device_id, ring_seconds);

	if (!native)
	{
		return nullptr;
	}

	return std::make_shared<os_audio_output>(std::move(native));
}

std::vector<platform::sound_device> platform::list_audio_playback_devices()
{
	std::vector<sound_device> result;

	for (auto& device : os::list_audio_playback_devices())
	{
		sound_device d;
		d.id = std::move(device.id);
		d.name = std::move(device.name);
		result.emplace_back(std::move(d));
	}

	if (result.empty())
	{
		core::log("list_audio_playback_devices", "No sound endpoints found.");
	}

	return result;
}