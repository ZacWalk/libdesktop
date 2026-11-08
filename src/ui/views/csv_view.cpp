// This file is part of libdesktop, a C++20 platform layer for desktop applications
// Copyright 2026  Zac Walker
//
// This program is free software; you can redistribute it and / or modify it
// under the terms of the LGPL License either version 2.1 or later.
// License details are available at https://www.gnu.org/licenses/lgpl-2.1.html
// This program is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY

// Purpose: Implement the source-preserving CSV table control

#include "libdesktop/ui/views/csv_view.h"

#include <algorithm>
#include <cctype>
#include <utility>

namespace
{
	template <typename AdvanceFn>
	void calc_word_breaks_into(std::vector<int>& breaks, const std::string_view text, const int max_columns,
	                           AdvanceFn&& char_advance)
	{
		breaks.clear();
		const auto len = static_cast<int>(text.size());
		int column = 0;
		int last_break = -1;
		int columns_at_break = 0;
		for (int i = 0; i < len; ++i)
		{
			const auto advance = char_advance(i, column);
			if (text[i] == ' ' || text[i] == '\t')
			{
				last_break = i + 1;
				columns_at_break = column + advance;
			}
			if (column > 0 && column + advance > max_columns)
			{
				if (last_break > (breaks.empty() ? 0 : breaks.back()))
				{
					breaks.push_back(last_break);
					column = column - columns_at_break + advance;
				}
				else
				{
					breaks.push_back(i);
					column = advance;
				}
				last_break = -1;
				columns_at_break = 0;
			}
			else
				column += advance;
		}
	}

	int safe_columns(const int width, const int char_width) noexcept
	{
		return char_width > 0 ? std::max(1, width / char_width) : 1;
	}
}

namespace ui
{
	csv_view::csv_view(view_host& host, const view_theme& theme, view_context* context, text_clipboard* clipboard)
		: read_only_doc_view(host, theme, context, clipboard)
	{
		_sel_margin = false;
		_word_wrap = true;
	}

	csv_view::~csv_view() = default;

	void csv_view::set_buffer(const text_buffer_ptr& document, highlight_fn highlight)
	{
		_table = {};
		read_only_doc_view::set_buffer(document, std::move(highlight));
		rebuild_table();
	}

	void csv_view::recalc_vert_scrollbar()
	{
		_content_extent.cy = top_content_padding() + _total_visual_rows * _font_extent.cy + bottom_content_padding();
		const auto max_y = std::max(0, _content_extent.cy - (_view_extent.cy - text_top()));
		if (_scroll_offset.y > max_y)
		{
			_scroll_offset.y = max_y;
			_host.invalidate_view();
		}
		const auto visible_height = std::max(0, _view_extent.cy - text_top());
		_vscroll.update(_content_extent.cy, visible_height, _scroll_offset.y);
	}

	void csv_view::layout()
	{
		rebuild_table();
		_wrap_line_y.clear();
		_total_visual_rows = 0;
		if (!_doc || _table.col_widths.empty())
		{
			recalc_vert_scrollbar();
			return;
		}

		const auto line_count = static_cast<int>(_doc->size());
		_wrap_line_y.assign(line_count + 1, 0);
		int cumulative = 0;
		for (int i = 0; i < line_count; ++i)
		{
			_wrap_line_y[i] = cumulative;
			cumulative += record_visual_rows(i);
			if (i == 0 && line_count > 1) ++cumulative;
		}
		_wrap_line_y[line_count] = cumulative;
		_total_visual_rows = cumulative;
		recalc_vert_scrollbar();
	}

	text_location csv_view::client_to_text(const core::pointi& point) const
	{
		if (!_doc || _doc->empty()) return {};
		const auto line = std::clamp(client_to_line(point), 0, static_cast<int>(_doc->size()) - 1);
		text_location result{0, line};
		if (point.x >= _view_extent.cx / 2) result.x = static_cast<int>((*_doc)[line].size());
		return result;
	}

	void csv_view::draw_view(render::draw_context& draw) const
	{
		const auto client = client_rect();
		const auto background = _theme.style_color(text_style::normal_bkgnd);
		draw.draw_rect(client, core::color(background));
		if (_doc && !_table.col_widths.empty())
		{
			const auto font = body_font();
			const auto font_cx = _font_extent.cx;
			const auto font_cy = _font_extent.cy;
			const auto line_count = static_cast<int>(_doc->size());
			const auto first_visual_row = font_cy > 0
			                              ? std::max(0, (_scroll_offset.y - top_content_padding()) / font_cy)
			                              : 0;
			int line_index = _wrap_line_y.empty() ? 0 : visual_row_to_line_index(first_visual_row);
			line_index = std::clamp(line_index, 0, std::max(0, line_count - 1));
			int y = line_offset(line_index) - _scroll_offset.y + text_top();
			while (y < client.bottom && line_index < line_count)
			{
				(*_doc)[line_index].render(_line_buf);
				table_layout::split_csv_cells(_cells, _line_buf, &_cell_text);
				const bool header = line_index == 0;
				const bool selected = line_fully_selected(line_index, static_cast<int>(_line_buf.size()));
				const auto row_background = selected ? _theme.style_color(text_style::sel_bkgnd) : background;
				const auto pipe = _theme.style_color(selected ? text_style::sel_text : text_style::md_marker);
				const auto text_color = _theme.style_color(selected ? text_style::sel_text
			                                             : header ? text_style::md_bold : text_style::normal_text);
				y += table_layout::draw_table_row(draw, y, left_pad(), client.right, _cells, _table, font,
				                                  font_cx, font_cy, header, row_background, pipe, text_color,
				                                  _cell_breaks, cell_breaks_fn) * font_cy;
				if (header && line_count > 1 && y < client.bottom)
				{
					table_layout::draw_separator_row(draw, y, left_pad(), client.right, _table, font,
					                                  font_cx, font_cy, background,
					                                  _theme.style_color(text_style::md_marker));
					y += font_cy;
				}
				++line_index;
			}
		}
		_vscroll.draw(draw, scrollbar_rect(), _theme);
		draw_message_bar(draw);
	}

	void csv_view::cell_breaks_fn(std::vector<int>& out, const std::string_view text, const int col_width)
	{
		calc_word_breaks_into(out, text, col_width, [&](const int i, int) -> int
		{
			return core::is_utf8_continuation(text[i]) ? 0 : 1;
		});
	}

	int csv_view::record_visual_rows(const int line_index) const
	{
		(*_doc)[line_index].render(_line_buf);
		table_layout::split_csv_cells(_cells, _line_buf, &_cell_text);
		int rows = 1;
		for (size_t column = 0; column < _table.col_widths.size() && column < _cells.size(); ++column)
		{
			const auto visual = table_layout::cell_visual_rows(table_layout::trim_cell(_cells[column]),
			                                                 _table.col_widths[column], _cell_breaks,
			                                                 cell_breaks_fn);
			rows = std::max(rows, visual);
		}
		return rows;
	}

	void csv_view::rebuild_table()
	{
		_table = {};
		if (!_doc || _doc->empty()) return;
		const auto line_count = static_cast<int>(_doc->size());
		const auto available_width = std::max(1, _view_extent.cx - left_pad());
		const auto available_columns = safe_columns(available_width, _font_extent.cx);
		_table.start_line = 0;
		_table.end_line = line_count;
		_table.separator_line = -1;
		for (int i = 0; i < line_count; ++i)
		{
			(*_doc)[i].render(_line_buf);
			table_layout::split_csv_cells(_cells, _line_buf, &_cell_text);
			while (_table.col_widths.size() < _cells.size()) _table.col_widths.push_back(0);
			for (size_t column = 0; column < _cells.size(); ++column)
			{
				const auto width = table_layout::utf8_codepoint_count(table_layout::trim_cell(_cells[column]));
				_table.col_widths[column] = std::max(_table.col_widths[column], width);
			}
		}
		table_layout::cap_col_widths(_table.col_widths, available_columns);
	}
}