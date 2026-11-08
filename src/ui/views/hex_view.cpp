// This file is part of libdesktop, a C++20 platform layer for desktop applications
// Copyright 2026  Zac Walker
//
// This program is free software; you can redistribute it and / or modify it
// under the terms of the LGPL License either version 2.1 or later.
// License details are available at https://www.gnu.org/licenses/lgpl-2.1.html
// This program is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY

// Purpose: Render the byte-oriented hex document view.

#include "libdesktop/ui/views/hex_view.h"

#include <algorithm>
#include <limits>

namespace ui
{
	namespace
	{
		core::color transparent()
		{
			return {0.0f, 0.0f, 0.0f, 0.0f};
		}

		void draw_run(render::draw_context& draw, const int x, const int y, const int chars,
		              const int cell_width, const int cell_height, const std::string_view text,
		              const render::font_face font, const core::color32 foreground,
		              const core::color32 background)
		{
			const core::recti clip{x, y, x + chars * cell_width, y + cell_height};
			if (clip.is_empty()) return;
			draw.draw_rect(clip, core::color(background));
			draw.draw_text(text, clip, font, render::text_style::single_line,
			               core::color(foreground), transparent());
		}
	}

	hex_view::hex_view(view_host& host, const view_theme& theme, view_context* context, text_clipboard* clipboard)
		: read_only_doc_view(host, theme, context, clipboard)
	{
	}

	hex_view::~hex_view() = default;

	void hex_view::draw_view(render::draw_context& draw) const
	{
		const auto client = client_rect();
		const auto background = _theme.style_color(text_style::normal_bkgnd);
		draw.draw_rect(client, core::color(background));

		if (_doc)
		{
			const auto font = body_font();
			const auto cx = _font_extent.cx;
			const auto cy = _font_extent.cy;
			const auto line_count = static_cast<int>(_doc->size());
			const auto offset_color = _theme.style_color(text_style::code_number);
			const auto hex_color = _theme.style_color(text_style::code_keyword);
			const auto ascii_color = _theme.style_color(text_style::code_string);
			const auto separator_color = _theme.style_color(text_style::code_comment);
			constexpr int content_chars = offset_chars + hex_chars + 1 + bytes_per_line + 1;
			const auto left_margin = std::max(cx, (client.right - content_chars * cx) / 2);
			const auto first_line = cy > 0
			                       ? std::max(0, (_scroll_offset.y - top_content_padding()) / cy)
			                       : 0;
			int line_index = first_line;
			int y = line_offset(line_index) - _scroll_offset.y + text_top();
			while (y < client.bottom && line_index < line_count)
			{
				(*_doc)[line_index].render(_line_buf);
				build_row(static_cast<uint32_t>(line_index) * bytes_per_line);
				int x = left_margin;
				draw_run(draw, x, y, offset_chars, cx, cy, _offset_buf, font, offset_color, background);
				x += offset_chars * cx;
				draw_run(draw, x, y, hex_chars, cx, cy, _hex_buf, font, hex_color, background);
				x += hex_chars * cx;
				draw_run(draw, x, y, 1, cx, cy, "|", font, separator_color, background);
				x += cx;
				draw_run(draw, x, y, bytes_per_line, cx, cy, _ascii_buf, font, ascii_color, background);
				x += bytes_per_line * cx;
				draw_run(draw, x, y, 1, cx, cy, "|", font, separator_color, background);
				++line_index;
				y += cy;
			}
		}

		_vscroll.draw(draw, scrollbar_rect(), _theme);
		draw_message_bar(draw);
	}

	void hex_view::append_hex_byte(std::string& out, const uint8_t value)
	{
		static constexpr char digits[] = "0123456789ABCDEF";
		out += digits[value >> 4];
		out += digits[value & 0x0F];
	}

	void hex_view::build_row(const uint32_t file_offset) const
	{
		_offset_buf.clear();
		append_hex_byte(_offset_buf, static_cast<uint8_t>(file_offset >> 24));
		append_hex_byte(_offset_buf, static_cast<uint8_t>(file_offset >> 16));
		append_hex_byte(_offset_buf, static_cast<uint8_t>(file_offset >> 8));
		append_hex_byte(_offset_buf, static_cast<uint8_t>(file_offset));

		_hex_buf.clear();
		_ascii_buf.clear();
		const auto num_bytes = std::min(static_cast<int>(_line_buf.size()), bytes_per_line);
		for (int i = 0; i < bytes_per_line; ++i)
		{
			if (i == 8) _hex_buf += ' ';
			if (i < num_bytes)
			{
				const auto value = static_cast<uint8_t>(_line_buf[i]);
				append_hex_byte(_hex_buf, value);
				_hex_buf += ' ';
				_ascii_buf += value >= 32 && value < 127 ? static_cast<char>(value) : '.';
			}
			else
			{
				_hex_buf += "   ";
				_ascii_buf += ' ';
			}
		}
	}
}