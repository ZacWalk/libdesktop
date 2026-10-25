// This file is part of libdesktop, a C++20 platform layer for desktop applications
// Copyright 2026  Zac Walker
// 
// This program is free software; you can redistribute it and / or modify it
// under the terms of the LGPL License either version 2.1 or later.
// License details are available at https://www.gnu.org/licenses/lgpl-2.1.html
// This program is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY

// Purpose: The UI layer's own definitions: its globals and hooks, and the theme's colours.

#include "libdesktop/render.h"

df_assert_pod(ui::color);
df_assert_movable(ui::const_image_ptr);
df_assert_movable(ui::const_surface_ptr);

/////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
/////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
/////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
/////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
/////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

std::unordered_map<void*, std::function<bool()>> ui::animations;
bool ui::animations_enabled = true;
// Off until a backend that can sample them says otherwise.
bool ui::yuv_textures_enabled = false;
bool ui::yuv_textures_allowed = true;
void (*ui::on_yuv_fault)() = nullptr;
bool ui::p010_textures_enabled = false;
// Matches alpha_fade_rate at 60Hz until the first frame recomputes it.
float ui::animation_step_factor = 0.333f;

std::function<ui::surface_ptr(df::cspan)> ui::decode_embedded_image;
std::function<std::unique_ptr<ui::surface_scaler>()> ui::make_surface_scaler;

df::ui_counters df::ui_perf;
df::gpu_counters df::gpu_perf;

ui::surface_ptr ui::decode_embedded(const df::cspan data)
{
	return decode_embedded_image ? decode_embedded_image(data) : nullptr;
}

ui::color32 ui::style::color::dialog_text = 0;
ui::color32 ui::style::color::dialog_selected_text = 0;
ui::color32 ui::style::color::dialog_background = 0;
ui::color32 ui::style::color::dialog_selected_background = 0;
ui::color32 ui::style::color::button_background = 0;
ui::color32 ui::style::color::edit_background = 0;
ui::color32 ui::style::color::edit_text = 0;

ui::color32 ui::style::color::toolbar_background = 0;
ui::color32 ui::style::color::bubble_background = 0;
ui::color32 ui::style::color::sidebar_background = 0;
ui::color32 ui::style::color::group_background = 0;
ui::color32 ui::style::color::view_background = 0;
ui::color32 ui::style::color::view_selected_background = 0;
ui::color32 ui::style::color::view_text = 0;

ui::color32 ui::style::color::menu_background = 0;
ui::color32 ui::style::color::menu_text = 0;
ui::color32 ui::style::color::menu_shortcut_text = 0;

ui::color32 ui::style::color::important_background = 0;
ui::color32 ui::style::color::warning_background = 0;
ui::color32 ui::style::color::success_background = 0;
ui::color32 ui::style::color::info_background = 0;
ui::color32 ui::style::color::desktop_background = 0;


void ui::style::color::install(const system_colors& system)
{
	constexpr color32 red = 0xaa2211;
	constexpr color32 green = 0x2E8B33;
	constexpr color32 orange = 0xCC6611;
	constexpr color32 blue = 0x0288D1;
	constexpr color32 blue2 = 0x117799;

	dialog_text = 0x00eeeeee;
	dialog_selected_text = 0x00ffffff;
	dialog_background = 0x00555555;
	dialog_selected_background = bgr(0x005588EE);
	button_background = 0x00444444;
	edit_background = system.edit_background;
	edit_text = system.edit_text;

	sidebar_background = 0x00333333;
	bubble_background = 0x00333333;
	group_background = 0x00444444;
	toolbar_background = 0x00666666;

	important_background = bgr(orange);
	warning_background = bgr(red);
	success_background = bgr(green);
	info_background = bgr(blue2);

	view_background = 0x00333333;
	view_selected_background = bgr(blue);
	view_text = 0x00eeeeee;

	menu_background = 0x00444444;
	menu_text = 0x00eeeeee;
	menu_shortcut_text = bgr(0x006699EE);

	desktop_background = system.desktop_background;

}
