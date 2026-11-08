// This file is part of libdesktop, a C++20 platform layer for desktop applications
// Copyright 2026  Zac Walker
//
// This program is free software; you can redistribute it and / or modify it
// under the terms of the LGPL License either version 2.1 or later.
// License details are available at https://www.gnu.org/licenses/lgpl-2.1.html
// This program is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY

// Purpose: Keyboard, pointer, cursor, and input-state abstractions.

#pragma once

namespace platform::keys
{
	extern char32_t APPS;
	extern char32_t BACK;
	extern char32_t BROWSER_BACK;
	extern char32_t BROWSER_FAVORITES;
	extern char32_t BROWSER_FORWARD;
	extern char32_t BROWSER_HOME;
	extern char32_t BROWSER_REFRESH;
	extern char32_t BROWSER_SEARCH;
	extern char32_t BROWSER_STOP;
	extern char32_t DEL;
	extern char32_t DOWN;
	extern char32_t ESCAPE;
	extern char32_t F1;
	extern char32_t F2;
	extern char32_t F3;
	extern char32_t F4;
	extern char32_t F5;
	extern char32_t F6;
	extern char32_t F7;
	extern char32_t F8;
	extern char32_t F9;
	extern char32_t F10;
	extern char32_t F11;
	extern char32_t F12;
	extern char32_t INSERT;
	extern char32_t LEFT;
	extern char32_t MEDIA_NEXT_TRACK;
	extern char32_t MEDIA_PLAY_PAUSE;
	extern char32_t MEDIA_PREV_TRACK;
	extern char32_t MEDIA_STOP;
	extern char32_t NEXT;
	extern char32_t OEM_1;
	extern char32_t OEM_2;
	extern char32_t OEM_3;
	extern char32_t OEM_4;
	extern char32_t OEM_5;
	extern char32_t OEM_6;
	extern char32_t OEM_7;
	extern char32_t OEM_102;
	extern char32_t OEM_COMMA;
	extern char32_t OEM_PERIOD;
	extern char32_t OEM_MINUS;
	extern char32_t OEM_PLUS;
	extern char32_t PRIOR;
	extern char32_t RETURN;
	extern char32_t RIGHT;
	extern char32_t SPACE;
	extern char32_t TAB;
	extern char32_t UP;
	extern char32_t VOLUME_DOWN;
	extern char32_t VOLUME_MUTE;
	extern char32_t VOLUME_UP;
	extern char32_t HOME;
	extern char32_t END;
	extern char32_t SHIFT;
	extern char32_t CONTROL;
	extern char32_t ALT;
	extern char32_t LSHIFT;
	extern char32_t RSHIFT;
	extern char32_t LCONTROL;
	extern char32_t RCONTROL;
	extern char32_t LALT;
	extern char32_t RALT;
	extern char32_t LWIN;
	extern char32_t RWIN;
	extern char32_t PAUSE;
	extern char32_t CAPITAL;
	extern char32_t NUMLOCK;
	extern char32_t SCROLL;
	extern char32_t SNAPSHOT;
	extern char32_t NUMPAD0;
	extern char32_t NUMPAD1;
	extern char32_t NUMPAD2;
	extern char32_t NUMPAD3;
	extern char32_t NUMPAD4;
	extern char32_t NUMPAD5;
	extern char32_t NUMPAD6;
	extern char32_t NUMPAD7;
	extern char32_t NUMPAD8;
	extern char32_t NUMPAD9;
	extern char32_t MULTIPLY;
	extern char32_t ADD;
	extern char32_t SUBTRACT;
	extern char32_t DECIMAL;
	extern char32_t DIVIDE;
	extern char32_t SEPARATOR;

	std::string_view format(char32_t key);

	// Numeric keypad digit virtual-key codes (VK_NUMPAD0..VK_NUMPAD9 = 0x60..0x69) are
	// distinct from the top-row digit codes ('0'..'9' = 0x30..0x39). With NumLock on, the
	// keypad sends the VK_NUMPAD* codes, so keyboard accelerators bound to '0'..'9' (rating
	// and label shortcuts) never fired from the numeric keypad (diffractor#135). Map the keypad
	// digits onto the equivalent top-row digit; all other keys pass through unchanged.
	constexpr char32_t numpad0 = 0x60;
	constexpr char32_t numpad9 = 0x69;
	constexpr char32_t normalize_numpad(const char32_t key)
	{
		return (key >= numpad0 && key <= numpad9) ? U'0' + (key - numpad0) : key;
	}
};

namespace platform
{
	struct keyboard_accelerator_t
	{
		char32_t key = 0;
		uint32_t key_state = 0;
		static constexpr auto shift = 0x04;
		static constexpr auto control = 0x08;
		static constexpr auto alt = 0x10;
	};

	std::string format_keyboard_accelerator(const std::vector<keyboard_accelerator_t>& keyboard_accelerators);
	extern int ticks_since_last_user_action;

	enum class cursor
	{
		none,
		normal,
		link,
		zoom,
		select,
		text_select,
		move,
		size_all,
		left_right,
		up_down,
		hand_up,
		hand_down
	};

	struct key_state
	{
		bool control = false;
		bool shift = false;
		bool alt = false;
		// A key-down the keyboard repeated while the key was held, rather than a fresh press.
		bool repeat = false;
	};

	enum class wheel_axis
	{
		vertical,
		horizontal
	};

	// Whole detents from a possibly-fractional delta. A precision wheel or a touchpad emits many
	// small deltas that a consumer dividing by the detent would round away entirely, so the
	// remainder is carried rather than discarded.
	inline int accumulate_wheel_steps(double& pending, const double delta, const double detent = 120.0) noexcept
	{
		if (detent <= 0.0) return 0;
		pending += delta;
		const auto steps = static_cast<int>(pending / detent);
		pending -= steps * detent;
		return steps;
	}

	// One wheel event, accumulated once at the frame boundary. `steps` is whole detents, for the
	// surfaces that move by detent; `delta` is the same movement in logical units, for the ones that
	// scroll smoothly. Accumulating in one place is what stops each consumer inventing its own
	// rounding, and what keeps a touchpad from being divided down to nothing on the way through.
	struct wheel_notch
	{
		wheel_axis axis = wheel_axis::vertical;
		int steps = 0;
		int delta = 0;
		key_state keys;
		bool is_vertical() const noexcept { return axis == wheel_axis::vertical; }
		bool is_horizontal() const noexcept { return axis == wheel_axis::horizontal; }
	};

	enum class other_mouse_button
	{
		xb1,
		xb2
	};

	key_state current_key_state();
	bool is_key_down(char32_t key);
	bool is_key_down_async(char32_t key);
	core::pointi cursor_position();
	core::sizei primary_screen_size();
	int primary_screen_dpi();
}