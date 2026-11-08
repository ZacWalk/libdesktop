// This file is part of libdesktop, a C++20 platform layer for desktop applications
// Copyright 2026  Zac Walker
//
// This program is free software; you can redistribute it and / or modify it
// under the terms of the LGPL License either version 2.1 or later.
// License details are available at https://www.gnu.org/licenses/lgpl-2.1.html
// This program is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY

// Purpose: Default dark view palette and heading font sizes.

#include "libdesktop/ui/views/view_theme.h"

namespace ui
{
	core::color32 view_theme::style_color(const text_style s) const
	{
		switch (s)
		{
		case text_style::white_space:
		case text_style::main_wnd_clr:
			return view_colors::main_wnd_clr;
		case text_style::tool_wnd_clr:
			return view_colors::tool_wnd_clr;
		case text_style::normal_bkgnd:
			return core::rgba(30, 30, 30);
		case text_style::normal_text:
			return core::rgba(222, 222, 222);
		case text_style::sel_margin:
			return core::rgba(44, 44, 44);
		case text_style::code_preprocessor:
			return core::rgba(133, 133, 211);
		case text_style::code_comment:
			return core::rgba(128, 222, 128);
		case text_style::code_number:
		case text_style::code_string:
			return core::rgba(244, 244, 144);
		case text_style::code_operator:
			return core::rgba(128, 255, 128);
		case text_style::code_keyword:
			return core::rgba(128, 128, 255);
		case text_style::sel_bkgnd:
			return core::rgba(88, 88, 88);
		case text_style::sel_text:
			return core::rgba(255, 255, 255);
		case text_style::error_bkgnd:
			return core::rgba(128, 0, 0);
		case text_style::error_text:
			return core::rgba(255, 100, 100);
		case text_style::md_heading1:
			return core::rgba(100, 200, 255);
		case text_style::md_heading2:
			return core::rgba(140, 180, 255);
		case text_style::md_heading3:
			return core::rgba(180, 160, 255);
		case text_style::md_bold:
			return core::rgba(255, 255, 255);
		case text_style::md_italic:
			return core::rgba(180, 220, 180);
		case text_style::md_code:
			return core::rgba(215, 186, 125);
		case text_style::md_link_text:
			return core::rgba(100, 180, 255);
		case text_style::md_link_url:
			return core::rgba(120, 120, 120);
		case text_style::md_marker:
			return core::rgba(80, 80, 80);
		case text_style::md_bullet:
			return core::rgba(200, 200, 100);
		case text_style::md_quote:
			// Quoted text reads as text by default; applications can repalette it.
			return core::rgba(222, 222, 222);
		}

		return core::rgba(222, 222, 222);
	}

	render::font_desc view_theme::heading_font(const int level) const
	{
		auto result = text_font;
		switch (level)
		{
		case 1: result.size = text_font_height + 12; return result;
		case 2: result.size = text_font_height + 8; return result;
		case 3: result.size = text_font_height + 4; return result;
		default: return text_font;
		}
	}
}
