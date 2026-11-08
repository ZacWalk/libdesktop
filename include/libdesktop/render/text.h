// This file is part of libdesktop, a C++20 platform layer for desktop applications
// Copyright 2026  Zac Walker
// 
// This program is free software; you can redistribute it and / or modify it
// under the terms of the LGPL License either version 2.1 or later.
// License details are available at https://www.gnu.org/licenses/lgpl-2.1.html
// This program is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY

// Purpose: Font descriptors, metrics, and platform text/icon-surface rendering services.

#pragma once

namespace render
{
	enum class font_face : int;
	class measure_context;

	struct font_desc
	{
		std::string family;
		int size = 16;
		int weight = 400;
		bool italic = false;
		bool underline = false;
		bool strikeout = false;
		bool cell_size = false;

		bool operator==(const font_desc&) const = default;
	};

	struct font_metrics
	{
		int height = 0;
		int ascent = 0;
		int descent = 0;
		int x_height = 0;
		int avg_char_width = 0;
	};

	// Custom face ids are process values, so every backend can resolve the same descriptor.
	font_face make_font(const font_desc& desc);
	font_metrics metrics(measure_context& mc, font_face face);

	namespace detail
	{
		constexpr int custom_font_base = 0x1000;
		bool custom_font_description(font_face face, font_desc& desc);
	}

	// Registers the application-owned icon font. The bytes must outlive the process.
	void set_icon_font(core::cspan font_bytes);
	// Or names an installed icon font whose code points the application's icon_index values are:
	// "Segoe Fluent Icons", which Windows 10 has as Segoe MDL2 Assets. The later call wins.
	void set_icon_font_family(std::string_view family);

	// Rasterises one glyph of the bundled icon font to a surface, for the places that need an image
	// rather than drawn text. Takes a code point, so it does not depend on the size of wchar_t.
	core::const_surface_ptr create_icon_surface(char32_t ch);

	// A stretch of text with its own weight, slant and colour, inside a styled_text block.
	struct text_run
	{
		// UTF-8. A '\n' breaks the line.
		std::string text;
		bool bold = false;
		bool italic = false;
		// As core::color32 holds one: 0xAABBGGRR.
		uint32_t color = 0xFFFFFFFF;
	};

	// Text set as one block, as a player draws a subtitle: its runs laid out together at a size in
	// pixels, wrapped to a width, aligned, and outlined so it reads on any picture.
	struct styled_text
	{
		std::vector<text_run> runs;
		// A family the system has, or the text engine's own fallback for one it has not.
		std::string font_family = "Arial";
		float font_px = 32.0f;
		// Lines longer than this wrap at a word; zero never wraps.
		int wrap_width = 0;
		// 0 aligns the lines left, 1 centres them and 2 aligns them right.
		int alignment = 1;
		float outline_px = 2.0f;
		// 0xAABBGGRR, as text_run::color.
		uint32_t outline_color = 0xFF000000;
	};

	// The block rasterised into a straight-alpha BGRA surface just large enough to hold it and its
	// outline, for drawing as a texture at any scale in either renderer. Null when there is nothing to
	// draw, or no text stack to draw it with. Safe on any thread.
	core::surface_ptr render_styled_text(const styled_text& text);
}
