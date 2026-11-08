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
df::cspan ui::icon_font_data;

df::ui_counters df::ui_perf;
df::gpu_counters df::gpu_perf;

ui::surface_ptr ui::decode_embedded(const df::cspan data)
{
	return decode_embedded_image ? decode_embedded_image(data) : nullptr;
}

namespace
{
	ui::style::theme installed_theme;
}

const ui::style::theme& ui::style::colors = installed_theme;

void ui::style::install(const system_colors& system)
{
	constexpr color32 red = 0xaa2211;
	constexpr color32 green = 0x2E8B33;
	constexpr color32 orange = 0xCC6611;
	constexpr color32 blue = 0x0288D1;
	constexpr color32 blue2 = 0x117799;

	auto& t = installed_theme;
	t.dialog_text = 0x00eeeeee;
	t.dialog_selected_text = 0x00ffffff;
	t.dialog_background = 0x00555555;
	t.dialog_selected_background = bgr(0x005588EE);
	t.button_background = 0x00444444;
	t.edit_background = system.edit_background;
	t.edit_text = system.edit_text;

	t.sidebar_background = 0x00333333;
	t.bubble_background = 0x00333333;
	t.group_background = 0x00444444;
	t.toolbar_background = 0x00666666;

	t.important_background = bgr(orange);
	t.warning_background = bgr(red);
	t.success_background = bgr(green);
	t.info_background = bgr(blue2);

	t.view_background = 0x00333333;
	t.view_selected_background = bgr(blue);
	t.view_text = 0x00eeeeee;

	t.menu_background = 0x00444444;
	t.menu_text = 0x00eeeeee;
	t.menu_shortcut_text = bgr(0x006699EE);

	t.desktop_background = system.desktop_background;
}
