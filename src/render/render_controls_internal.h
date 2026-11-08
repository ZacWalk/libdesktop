// This file is part of libdesktop, a C++20 platform layer for desktop applications
// Copyright 2026  Zac Walker
// 
// This program is free software; you can redistribute it and / or modify it
// under the terms of the LGPL License either version 2.1 or later.
// License details are available at https://www.gnu.org/licenses/lgpl-2.1.html
// This program is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY

// Purpose: What the drawn controls share inside libdesktop-render: the metrics and drawing helpers
// every control uses, and the edit render_edit.cpp makes. Internal to render_controls.cpp,
// render_edit.cpp and render_menu.cpp.

#pragma once

#include "libdesktop/render_controls.h"

namespace ui
{
	namespace drawn
	{
		// The native controls' metrics, in device-independent pixels, so the drawn ones keep their size.
		constexpr int base_icon_cxy = 18;
		constexpr int element_padding = 8;
		constexpr int focus_padding = 2;
		constexpr int corner_radius = 6;
		constexpr int button_padding = 8;

		inline int scaled(const int v, const double scale)
		{
			return df::round(v * scale);
		}

		inline int icon_cxy(const double scale)
		{
			return scaled(base_icon_cxy, scale);
		}

		inline color solid(const color32 c, const float alpha = 1.0f)
		{
			return {c, alpha};
		}

		void draw_icon(draw_context& dc, icon_index icon, recti bounds, color c);
		void draw_outline(draw_context& dc, recti bounds, color c, int width = 1);
		void fill_gradient(draw_context& dc, recti bounds, color32 top, color32 bottom, float alpha);
		bool is_word_char(uint32_t cp);
	}

	// The edit, which render_edit.cpp makes.
	edit_ptr make_drawn_edit(const edit_styles& styles, std::string_view text,
	                         std::function<void(const std::string&)> changed);
	recti drawn_edit_range_bounds(const edit& e, size_t start, size_t end);
	std::vector<command_ptr> drawn_edit_context_commands(edit& e, pointi loc);
}