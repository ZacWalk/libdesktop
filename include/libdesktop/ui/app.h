// This file is part of libdesktop, a C++20 platform layer for desktop applications
// Copyright 2026  Zac Walker
//
// This program is free software; you can redistribute it and / or modify it
// under the terms of the LGPL License either version 2.1 or later.
// License details are available at https://www.gnu.org/licenses/lgpl-2.1.html
// This program is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY

// Purpose: UI application callbacks layered over platform::app.

#pragma once

namespace ui
{
	extern std::atomic_int command_active;
	extern core::date_t start_time;
}

namespace ui
{
	enum class focus_mode
	{
		none,
		view,
		text_edit
	};

	// The UI part of an application's lifecycle. Platform setup, process events and the main loop
	// belong to platform::app; these callbacks are the only ones a frame needs from the UI.
	class app : public platform::app
	{
	public:
		// Applications construct their UI app before platform::run. Install the system palette and
		// session start timestamp here so windowed and console modes observe the same initialization.
		app()
		{
			start_time = core::utc_now();
			style::install({
				platform::system_color_value(platform::system_color::window),
				platform::system_color_value(platform::system_color::window_text),
				platform::system_color_value(platform::system_color::desktop)
			});
		}

		~app() override = default;

		virtual bool wants_gpu() const = 0;
		virtual bool key_down(char32_t c, platform::key_state keys) = 0;
		virtual bool text_input(std::string_view text) = 0;
		virtual focus_mode focus_mode() const = 0;
		virtual void track_menu(const frame_ptr& parent, core::recti button_bounds,
		                        const std::vector<command_ptr>& buttons) = 0;
	};
}
