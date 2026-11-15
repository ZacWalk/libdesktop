// This file is part of libdesktop, a C++20 platform layer for desktop applications
// Copyright 2026  Zac Walker
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

// Purpose: UI application callbacks layered over platform::app.

#pragma once

namespace ui
{
	// Raised while a command runs, and while a drawn menu is open for input.
	extern std::atomic_int command_active;

	// Raised only while a drawn menu is open for input, so it is counted in command_active too. A menu shows
	// commands rather than running one: an application that makes its commands unavailable while one runs
	// counts the open menus out, or the menu's own items would dim under it.
	extern std::atomic_int menu_active;

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
				platform::system_color_value(platform::system_color::desktop),
			});
		}

		~app() override = default;

		virtual bool wants_gpu() const = 0;
		virtual bool key_down(char32_t c, platform::key_state keys) = 0;
		// A key let go, outside a dialog: what an application that reads held keys - a game, an emulator - needs
		// to know. True when it took the key.
		virtual bool key_up(char32_t /*c*/, platform::key_state /*keys*/) { return false; }
		virtual bool text_input(std::string_view text) = 0;
		virtual focus_mode focus_mode() const = 0;
		virtual void track_menu(const frame_ptr& parent, core::recti button_bounds,
		                        const std::vector<command_ptr>& buttons) = 0;
	};
}
