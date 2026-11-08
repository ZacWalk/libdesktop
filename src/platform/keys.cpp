// This file is part of libdesktop, a C++20 platform layer for desktop applications
// Copyright 2026  Zac Walker
//
// This program is free software; you can redistribute it and / or modify it
// under the terms of the LGPL License either version 2.1 or later.
// License details are available at https://www.gnu.org/licenses/lgpl-2.1.html
// This program is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY

// Purpose: Portable keyboard label and accelerator formatting.

#include "libdesktop/platform.h"

namespace
{
	bool is_not_virt_key(const int key)
	{
		return (key >= '0' && key <= '9') ||
			(key >= 'A' && key <= 'Z');
	}
}

std::string_view platform::keys::format(const char32_t key)
{
	const auto& words = platform::text();
	if (key == BACK) return words.keyboard_back;
	if (key == BROWSER_BACK) return words.keyboard_browser_back;
	if (key == BROWSER_FAVORITES) return words.keyboard_browser_favorites;
	if (key == BROWSER_FORWARD) return words.keyboard_browser_forward;
	if (key == BROWSER_HOME) return words.keyboard_browser_home;
	if (key == BROWSER_REFRESH) return words.keyboard_browser_refresh;
	if (key == BROWSER_SEARCH) return words.keyboard_browser_search;
	if (key == BROWSER_STOP) return words.keyboard_browser_stop;
	if (key == DEL) return words.keyboard_del;
	if (key == DOWN) return words.keyboard_down;
	if (key == END) return words.keyboard_end;
	if (key == ESCAPE) return words.keyboard_escape;
	if (key == F1) return words.keyboard_f1;
	if (key == F10) return words.keyboard_f10;
	if (key == F11) return words.keyboard_f11;
	if (key == F12) return words.keyboard_f12;
	if (key == F2) return words.keyboard_f2;
	if (key == F3) return words.keyboard_f3;
	if (key == F4) return words.keyboard_f4;
	if (key == F5) return words.keyboard_f5;
	if (key == F6) return words.keyboard_f6;
	if (key == F7) return words.keyboard_f7;
	if (key == F8) return words.keyboard_f8;
	if (key == F9) return words.keyboard_f9;
	if (key == HOME) return words.keyboard_home;
	if (key == INSERT) return words.keyboard_insert;
	if (key == LEFT) return words.keyboard_left;
	if (key == MEDIA_NEXT_TRACK) return words.keyboard_media_next_track;
	if (key == MEDIA_PLAY_PAUSE) return words.keyboard_media_play_pause;
	if (key == MEDIA_PREV_TRACK) return words.keyboard_media_prev_track;
	if (key == MEDIA_STOP) return words.keyboard_media_stop;
	if (key == NEXT) return words.keyboard_next;
	if (key == OEM_4) return words.keyboard_oem_4;
	if (key == OEM_6) return words.keyboard_oem_6;
	if (key == OEM_MINUS) return words.keyboard_oem_minus;
	if (key == OEM_PLUS) return words.keyboard_oem_plus;
	if (key == PRIOR) return words.keyboard_prior;
	if (key == RETURN) return words.keyboard_enter;
	if (key == RIGHT) return words.keyboard_right;
	if (key == SPACE) return words.keyboard_space;
	if (key == TAB) return words.keyboard_tab;
	if (key == UP) return words.keyboard_up;
	if (key == VOLUME_DOWN) return words.keyboard_volume_down;
	if (key == VOLUME_MUTE) return words.keyboard_volume_mute;
	if (key == VOLUME_UP) return words.keyboard_volume_up;
	return "?";
}

std::string platform::format_keyboard_accelerator(const std::vector<platform::keyboard_accelerator_t>& keyboard_accelerators)
{
	constexpr auto control = platform::keyboard_accelerator_t::control;
	constexpr auto shift = platform::keyboard_accelerator_t::shift;
	constexpr auto alt = platform::keyboard_accelerator_t::alt;
	const auto& words = platform::text();

	std::string result;

	for (const auto& ac : keyboard_accelerators)
	{
		// Add Accelerator
		if (!result.empty())
		{
			result += std::format(" {} ", words.keyboard_or);
		}

		if (ac.key_state & alt)
		{
			result += std::format("{}+", words.keyboard_alt);
		}
		if (ac.key_state & control)
		{
			result += std::format("{}+", words.keyboard_control);
		}
		if (ac.key_state & shift)
		{
			result += std::format("{}+", words.keyboard_shift);
		}

		if (ac.key == platform::keys::RETURN)
		{
			result += words.keyboard_enter;
		}
		else if (ac.key == platform::keys::BACK)
		{
			result += words.keyboard_backspace;
		}
		else if (ac.key == platform::keys::OEM_PLUS || ac.key == platform::keys::OEM_MINUS)
		{
			// Quoted so the key is not read as the modifier separator.
			result += std::format("'{}'", platform::keys::format(ac.key));
		}
		else if (is_not_virt_key(ac.key))
		{
			const char szTemp[2] = {static_cast<char>(ac.key), 0};
			result += szTemp;
		}
		else
		{
			result += platform::keys::format(ac.key);
		}
	}

	return result;
}
