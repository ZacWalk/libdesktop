// This file is part of libdesktop, a C++20 platform layer for desktop applications
// Copyright 2026  Zac Walker
// 
// This program is free software; you can redistribute it and / or modify it
// under the terms of the LGPL License either version 2.1 or later.
// License details are available at https://www.gnu.org/licenses/lgpl-2.1.html
// This program is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY

// Purpose: The UI layer's own definitions: its globals and hooks, and the theme's colours.

#include "libdesktop/ui.h"

df_assert_pod(core::color);
df_assert_movable(core::const_image_ptr);
df_assert_movable(core::const_surface_ptr);

/////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
/////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
/////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
/////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
/////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

std::unordered_map<void*, std::function<bool()>> ui::animations;
// Matches alpha_fade_rate at 60Hz until the first frame recomputes it.
float ui::animation_step_factor = 0.333f;

std::atomic_int ui::command_active = 0;
core::date_t ui::start_time;

namespace
{
	ui::style::theme installed_theme;
	ui::chrome_icon_set installed_chrome_icons{};
}

void ui::set_chrome_icons(const chrome_icon_set& icons)
{
	installed_chrome_icons = icons;
}

const ui::chrome_icon_set& ui::chrome_icons()
{
	return installed_chrome_icons;
}

const ui::style::theme& ui::style::colors = installed_theme;

void ui::style::install(const system_colors& system)
{
	constexpr core::color32 red = 0xaa2211;
	constexpr core::color32 green = 0x2E8B33;
	constexpr core::color32 orange = 0xCC6611;
	constexpr core::color32 blue = 0x0288D1;
	constexpr core::color32 blue2 = 0x117799;

	auto& t = installed_theme;
	t.dialog_text = 0x00eeeeee;
	t.dialog_selected_text = 0x00ffffff;
	t.dialog_background = 0x00555555;
	t.dialog_selected_background = core::bgr(0x005588EE);
	t.button_background = 0x00444444;
	t.edit_background = system.edit_background;
	t.edit_text = system.edit_text;

	t.sidebar_background = 0x00333333;
	t.bubble_background = 0x00333333;
	t.group_background = 0x00444444;
	t.toolbar_background = 0x00666666;

	t.important_background = core::bgr(orange);
	t.warning_background = core::bgr(red);
	t.success_background = core::bgr(green);
	t.info_background = core::bgr(blue2);

	t.view_background = 0x00333333;
	t.view_selected_background = core::bgr(blue);
	t.view_text = 0x00eeeeee;

	t.menu_background = 0x00444444;
	t.menu_text = 0x00eeeeee;
	t.menu_shortcut_text = core::bgr(0x006699EE);

	t.desktop_background = system.desktop_background;
}
