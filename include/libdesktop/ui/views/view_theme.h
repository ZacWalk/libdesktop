// This file is part of libdesktop, a C++20 platform layer for desktop applications
// Copyright 2026  Zac Walker
//
// This program is free software; you can redistribute it and / or modify it
// under the terms of the LGPL License either version 2.1 or later.
// License details are available at https://www.gnu.org/licenses/lgpl-2.1.html
// This program is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY

// Purpose: Shared view fonts, spacing, and the dark text palette.

#pragma once

#include "libdesktop/core.h"
#include "libdesktop/render/context.h"
#include "libdesktop/render/text.h"
#include "libdesktop/ui/text_types.h"

namespace ui::view_colors
{
	constexpr core::color32 handle_color = core::rgba(0x44, 0x44, 0x44);
	constexpr core::color32 focus_handle_color = core::rgba(40, 100, 220);
	constexpr core::color32 main_wnd_clr = core::rgba(0x22, 0x22, 0x22);
	constexpr core::color32 tool_wnd_clr = core::rgba(0x33, 0x33, 0x33);
	constexpr core::color32 handle_hover_color = core::rgba(0x66, 0x66, 0x66);
	constexpr core::color32 handle_tracking_color = core::rgba(0x11, 0x66, 0xCC);
	constexpr core::color32 text_color = core::rgba(0xFF, 0xFF, 0xFF);
	constexpr core::color32 folder_text_color = core::rgba(0xEE, 0xCC, 0x22);
	constexpr core::color32 darker_text_color = core::rgba(0xCC, 0xCC, 0xCC);
	constexpr core::color32 line_color = core::rgba(0x22, 0x55, 0xAA);
	constexpr core::color32 window_background = core::rgba(33, 33, 33);
}

namespace ui
{
	// What a view is handed to draw itself. Metrics stay data so an application can
	// change spacing without replacing the parser or a view.
	class view_theme
	{
	public:
		virtual ~view_theme() = default;

		double dpi_scale = 1.0;

		render::font_desc list_font{"Calibri", 20};
		render::font_desc edit_font{"Calibri", 30};
		// Text views use a fixed-pitch face: a proportional face would put the caret
		// beside the text rather than on its byte-column grid.
		render::font_desc text_font{"Consolas", 24};

		int list_font_height = 20;
		int text_font_height = 24;

		int padding_x = 5;
		int padding_y = 5;
		int indent = 16;

		int edit_box_margin = 6;
		int edit_box_inner_pad = 4;

		int list_top_pad = 4;
		int list_scroll_pad = 64;

		core::color32 window_background = view_colors::window_background;
		core::color32 tool_background = view_colors::tool_wnd_clr;
		core::color32 text = view_colors::text_color;
		core::color32 dim_text = view_colors::darker_text_color;
		core::color32 group_text = view_colors::folder_text_color;
		core::color32 line = view_colors::line_color;
		core::color32 match_highlight = core::rgba(220, 140, 0);
		core::color32 header_background = core::rgba(43, 43, 43);
		core::color32 handle = view_colors::handle_color;
		core::color32 handle_hover = view_colors::handle_hover_color;
		core::color32 handle_tracking = view_colors::handle_tracking_color;
		core::color32 focus_handle = view_colors::focus_handle_color;

		[[nodiscard]] virtual core::color32 style_color(text_style s) const;
		[[nodiscard]] virtual render::font_desc heading_font(int level) const;
	};
}
