// view_hex.h — bytes, as offset, hex and ASCII columns.
//
// Each line of the buffer is one row of sixteen bytes, which is how a binary file
// is loaded rather than something this view decides. It draws what is there and
// nothing else: no caret, no editing, and no interpretation of the bytes beyond
// "printable ASCII, or a dot".

#pragma once

#include "ui/view_doc_readonly.h"

#include <algorithm>
#include <string>

namespace pf::ui
{
	class hex_view : public read_only_doc_view
	{
	public:
		hex_view(view_host& host, const theme& th, view_context* context = nullptr)
			: read_only_doc_view(host, th, context)
		{
		}

		~hex_view() override = default;

		static constexpr int bytes_per_line = 16;

	protected:
		void draw_view(pf::window_frame_ptr& window, pf::draw_context& draw) const override
		{
			const auto rc_client = client_rect();
			const auto bg = _theme.style_color(text_style::normal_bkgnd);

			draw.fill_solid_rect(rc_client, bg);

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
				const auto left_margin = std::max(cx, (rc_client.right - content_chars * cx) / 2);

				auto line_index = std::max(0, cy > 0 ? (_scroll_offset.y - cy) / cy : 0);
				auto y = line_content_offset(line_index) - _scroll_offset.y + text_top();

				while (y < rc_client.bottom && line_index < line_count)
				{
					(*_doc)[line_index].render(_line_buf);
					build_row(static_cast<uint32_t>(line_index) * bytes_per_line);

					int x = left_margin;

					const auto draw_run = [&](const std::string_view text, const int chars, const pf::color_t color)
					{
						const pf::irect clip(x, y, x + chars * cx, y + cy);
						draw.draw_text(x, y, clip, text, font, color, bg);
						x += chars * cx;
					};

					draw_run(_offset_buf, offset_chars, offset_color);
					draw_run(_hex_buf, hex_chars, hex_color);
					draw_run("|", 1, separator_color);
					draw_run(_ascii_buf, bytes_per_line, ascii_color);
					draw_run("|", 1, separator_color);

					line_index++;
					y += cy;
				}
			}

			_vscroll.draw(draw, scrollbar_rect());
			draw_message_bar(draw);
		}

	private:
		// Column widths in characters: eight offset digits and two spaces; sixteen
		// bytes as "XX " with a gap after the eighth, then a trailing space.
		static constexpr int offset_chars = 10;
		static constexpr int hex_chars = 50;

		// Reused across lines and paints so drawing allocates nothing
		mutable std::string _line_buf;
		mutable std::string _offset_buf;
		mutable std::string _hex_buf;
		mutable std::string _ascii_buf;

		static void append_hex_byte(std::string& out, const uint8_t value)
		{
			static constexpr char digits[] = "0123456789ABCDEF";
			out += digits[value >> 4];
			out += digits[value & 0x0F];
		}

		void build_row(const uint32_t file_offset) const
		{
			_offset_buf.clear();
			append_hex_byte(_offset_buf, static_cast<uint8_t>(file_offset >> 24));
			append_hex_byte(_offset_buf, static_cast<uint8_t>(file_offset >> 16));
			append_hex_byte(_offset_buf, static_cast<uint8_t>(file_offset >> 8));
			append_hex_byte(_offset_buf, static_cast<uint8_t>(file_offset));

			_hex_buf.clear();
			_ascii_buf.clear();

			const auto num_bytes = std::min(static_cast<int>(_line_buf.size()), bytes_per_line);

			for (int i = 0; i < bytes_per_line; i++)
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
	};
}
