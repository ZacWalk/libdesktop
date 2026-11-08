// This file is part of libdesktop, a C++20 platform layer for desktop applications
// Copyright 2026  Zac Walker
//
// This program is free software; you can redistribute it and / or modify it
// under the terms of the LGPL License either version 2.1 or later.
// License details are available at https://www.gnu.org/licenses/lgpl-2.1.html
// This program is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY

// Purpose: Layout and paint the source-preserving Markdown document control.

#include "libdesktop/ui/views/markdown_view.h"

#include <algorithm>
#include <cmath>

#include <ranges>
#include <utility>

namespace
{


	int safe_columns(const int width, const int char_width) noexcept
	{
		return char_width > 0 ? std::max(1, width / char_width) : 1;
	}

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
			{
				column += advance;
			}
		}
	}

	core::sizei measured_font(render::measure_context& measure, const render::font_face face)
	{
		const auto metrics = render::metrics(measure, face);
		auto result = measure.measure_text("M", face, render::text_style::single_line, 100000);
		if (metrics.avg_char_width > 0) result.cx = metrics.avg_char_width;
		if (metrics.height > 0) result.cy = metrics.height;
		return {std::max(1, result.cx), std::max(1, result.cy)};
	}
}

namespace ui
{
	markdown_view::markdown_view(view_host& host, const view_theme& th, view_context* context,
		                               text_clipboard* clipboard)
		: read_only_doc_view(host, th, context, clipboard)
	{
		_sel_margin = false;
		_word_wrap = true;
	}

	markdown_view::markdown_view(const view_theme& th, view_context* context, text_clipboard* clipboard)
		: read_only_doc_view(th, context, clipboard)
	{
		_sel_margin = false;
		_word_wrap = true;
	}

	markdown_view::~markdown_view() = default;

	void markdown_view::set_link_handler(link_fn handler)
	{
		_on_link = std::move(handler);
	}

	bool markdown_view::has_link_handler() const
	{
		return static_cast<bool>(_on_link);
	}

	markdown_view::link_hit markdown_view::link_hit_at(const core::pointi& point) const
	{
		bool on_text = false;
		const auto location = hit_test(point, &on_text);
		if (!on_text) return {};

		(*_doc)[location.y].render(_link_buf);
		const std::string_view text(_link_buf);
		const auto in_fence = _lines[location.y].in_fence;
		bool fence_state = in_fence;
		md::parse_source_line(text, fence_state, _link_line);

		for (size_t i = 0; i < _link_line.spans.size(); ++i)
		{
			const auto& span = _link_line.spans[i];
			if (span.link.empty()) continue;
			if (offset_within(text, span.text, location.x) || offset_within(text, span.link, location.x))
				return {std::string(span.link), location.y, static_cast<int>(i)};
		}

		return {};
	}

	std::string markdown_view::link_at(const core::pointi& point) const
	{
		return link_hit_at(point).target;
	}

	bool markdown_view::allows_drag_selection() const
	{
		return true;
	}

	void markdown_view::set_buffer(const text_buffer_ptr& document, highlight_fn highlight)
	{
		_tables.clear();
		_lines.clear();
		_total_content_height = 0;
		read_only_doc_view::set_buffer(document, std::move(highlight));
	}

	void markdown_view::on_placed(render::measure_context& measure)
	{
		for (int level = 1; level <= heading_levels; ++level)
		{
			_heading_fonts[level - 1] = render::make_font(_theme.heading_font(level));
			_heading_extent[level - 1] = measured_font(measure, _heading_fonts[level - 1]);
		}

		read_only_doc_view::on_placed(measure);
	}

	void markdown_view::layout()
	{
		_wrap_dirty_all = false;
		_wrap_dirty_first = _wrap_dirty_last = -1;

		_wrap_breaks.clear();
		_wrap_offsets.clear();
		_wrap_line_y.clear();
		_tables.clear();
		_lines.clear();
		_total_visual_rows = 0;
		_total_content_height = 0;

		if (!_doc) return;

		const auto line_count = static_cast<int>(_doc->size());
		_wrap_offsets.assign(line_count + 1, 0);
		_wrap_line_y.assign(line_count + 1, 0);
		_lines.assign(line_count, line_metrics{});

		const auto base_height = std::max(1, _font_extent.cy);
		const auto available_width = std::max(1, _view_extent.cx - left_pad() - right_pad());
		const auto table_columns = safe_columns(available_width, _font_extent.cx);
		const auto line_padding = base_height / 4;

		int y = 0;
		int rows = 0;
		bool in_fence = false;
		int i = 0;

		while (i < line_count)
		{
			(*_doc)[i].render(_line_buf);

			if (!in_fence && is_table_row(_line_buf))
			{
				const auto table = find_table(i, table_columns);
				if (table.start_line >= 0 && table.start_line <= i)
				{
					const auto table_index = static_cast<int>(_tables.size());
					_tables.push_back(table);

					for (int table_line = i; table_line < table.end_line; ++table_line)
					{
						auto& metrics = _lines[table_line];
						metrics.y = y;
						metrics.row_height = base_height;
						metrics.char_width = std::max(1, _font_extent.cx);
						metrics.table = table_index;
						metrics.rows = table_line == table.separator_line ? 1 : table_row_visual_rows(table_line, table);
						metrics.height = metrics.rows * base_height;

						_wrap_offsets[table_line] = static_cast<int>(_wrap_breaks.size());
						_wrap_line_y[table_line] = rows;

						y += metrics.height;
						rows += metrics.rows;
					}

					i = table.end_line;
					continue;
				}
			}

			const auto fence_state = in_fence;
			md::parse_source_line(_line_buf, in_fence, _source_line);

			auto& metrics = _lines[i];
			metrics.in_fence = fence_state;
			metrics.heading = heading_level(_source_line.kind);

			const auto extent = font_extent_for(metrics.heading);
			metrics.char_width = std::max(1, extent.cx);
			metrics.row_height = std::max(1, extent.cy);
			metrics.indent = continuation_indent(_source_line, _line_buf);

			const auto columns = std::max(1, safe_columns(available_width, metrics.char_width) - metrics.indent);
			wrap_line_into(_line_breaks_buf, _line_buf, columns);

			_wrap_offsets[i] = static_cast<int>(_wrap_breaks.size());
			_wrap_breaks.insert(_wrap_breaks.end(), _line_breaks_buf.begin(), _line_breaks_buf.end());
			_wrap_line_y[i] = rows;

			metrics.rows = static_cast<int>(_line_breaks_buf.size()) + 1;
			metrics.lead = metrics.heading > 0 && i > 0 ? metrics.row_height / 2 : 0;
			metrics.trail = metrics.heading > 0 ? metrics.row_height / 3 : line_padding;
			metrics.y = y;
			metrics.height = metrics.lead + metrics.rows * metrics.row_height + metrics.trail;

			y += metrics.height;
			rows += (metrics.height + base_height - 1) / base_height;
			++i;
		}

		_wrap_offsets[line_count] = static_cast<int>(_wrap_breaks.size());
		_wrap_line_y[line_count] = rows;
		_total_visual_rows = rows;
		_total_content_height = y;
	}

	void markdown_view::recalc_vert_scrollbar()
	{
		_content_extent.cx = _view_extent.cx;
		_content_extent.cy = top_content_padding() + _total_content_height + bottom_content_padding();

		const auto max_y = std::max(0, _content_extent.cy - (_view_extent.cy - text_top()));
		if (_scroll_offset.y > max_y)
		{
			_scroll_offset.y = max_y;
			_host.invalidate_view();
		}

		const auto visible_height = std::max(0, _view_extent.cy - text_top());
		_vscroll.update(_content_extent.cy, visible_height, _scroll_offset.y);
	}

	int markdown_view::line_offset(const int line_index) const
	{
		const auto count = laid_out_lines();
		if (count <= 0) return top_content_padding();
		return top_content_padding() + _lines[std::clamp(line_index, 0, count - 1)].y;
	}

	int markdown_view::line_height(const int line_index) const
	{
		const auto count = laid_out_lines();
		if (count <= 0) return _font_extent.cy;
		return _lines[std::clamp(line_index, 0, count - 1)].height;
	}

	int markdown_view::client_to_line(const core::pointi& point) const
	{
		const auto count = laid_out_lines();
		if (count <= 0) return 0;

		const auto it = std::ranges::upper_bound(_lines.begin(), _lines.begin() + count, content_y_of(point.y), {},
		                                        &line_metrics::y);
		const auto index = static_cast<int>(it - _lines.begin()) - 1;
		return std::clamp(index, 0, count - 1);
	}

	text_location markdown_view::client_to_text(const core::pointi& point) const
	{
		return hit_test(point, nullptr);
	}

	core::pointi markdown_view::text_to_client(const text_location& point) const
	{
		const auto count = laid_out_lines();
		if (count <= 0) return {};

		const auto line_index = std::clamp(point.y, 0, count - 1);
		const auto& metrics = _lines[line_index];

		(*_doc)[line_index].render(_render_buf);
		const std::string_view line_view(_render_buf);

		const auto breaks = markdown_line_breaks(line_index);
		const auto row = std::clamp(markdown_char_to_sub_row(line_index, point.x), 0,
		                           static_cast<int>(breaks.size()));
		const auto row_start = row == 0 ? 0 : clamp_offset(breaks[row - 1], line_view);
		const auto indent = row > 0 ? metrics.indent : 0;

		core::pointi point_out;
		point_out.y = top_content_padding() + metrics.y + metrics.lead + row * metrics.row_height - _scroll_offset.y + text_top();
		point_out.x = left_pad() + (indent + column_of(line_view, row_start, point.x)) * metrics.char_width;
		return point_out;
	}

	void markdown_view::ensure_visible(const text_location& point)
	{
		const auto count = laid_out_lines();
		if (count <= 0) return;

		const auto line_index = std::clamp(point.y, 0, count - 1);
		const auto top = line_offset(line_index);
		const auto bottom = top + _lines[line_index].height;
		const auto visible_height = _view_extent.cy - text_top();

		if (bottom > _scroll_offset.y + visible_height)
			set_scroll_pixel(bottom - visible_height);
		else if (top < _scroll_offset.y)
			set_scroll_pixel(top);
	}

	void markdown_view::on_mouse_down(const core::pointi point, const platform::key_state keys)
	{
		_press_point = point;
		read_only_doc_view::on_mouse_down(point, keys);
	}

	void markdown_view::on_mouse_up(const core::pointi point, const platform::key_state keys)
	{
		const auto on_scrollbar = _vscroll._tracking || _hscroll._tracking;
		const auto slop = std::max(2, _font_extent.cx / 2);
		const auto moved = std::abs(point.x - _press_point.x) > slop || std::abs(point.y - _press_point.y) > slop;

		read_only_doc_view::on_mouse_up(point, keys);

		if (!_on_link || on_scrollbar || moved || !_doc || _doc->has_selection()) return;
		if (const auto target = link_at(point); !target.empty()) _on_link(target);
	}

	void markdown_view::on_mouse_move(const core::pointi point, const bool captured)
	{
		read_only_doc_view::on_mouse_move(point, captured);
	}

	platform::cursor markdown_view::cursor_at(const core::pointi point) const
	{
		if (_on_link && !_drag_selection)
		{
			const auto scrollbar = scrollbar_rect();
			if (!_vscroll.hit_test(point, scrollbar) && !_hscroll.hit_test(point, scrollbar) && !link_at(point).empty())
				return platform::cursor::hand_up;
		}

		return read_only_doc_view::cursor_at(point);
	}

	void markdown_view::paint(render::draw_context& draw, const bool focused) const
	{
		(void)focused;
		const auto client = client_rect();
		draw.draw_rect(client, core::color(_theme.style_color(text_style::normal_bkgnd)));

		if (_doc && laid_out_lines() > 0)
		{
			const auto line_count = laid_out_lines();
			int line_index = std::clamp(client_to_line({0, text_top()}), 0, line_count - 1);
			int y = line_offset(line_index) - _scroll_offset.y + text_top();

			while (y < client.bottom && line_index < line_count)
			{
				const auto& metrics = _lines[line_index];
				if (metrics.table >= 0 && metrics.table < static_cast<int>(_tables.size()))
				{
					const auto& table = _tables[metrics.table];
					const auto end = std::min(table.end_line, line_count);

					for (int table_line = line_index; table_line < end && y < client.bottom; ++table_line)
					{
						(*_doc)[table_line].render(_line_buf);
						y += draw_table_line(draw, y, client.right, table_line, table) * _font_extent.cy;
					}

					line_index = std::max(end, line_index + 1);
					continue;
				}

				draw_markdown_line(draw, y, client.right, line_index, metrics);
				y += metrics.height;
				++line_index;
			}
		}

		_vscroll.draw(draw, scrollbar_rect(), _theme);
		draw_message_bar(draw);
	}

	int markdown_view::left_pad() const
	{
		return _font_extent.cx * 2;
	}

	int markdown_view::right_pad() const
	{
		return _vscroll.thumb_thickness() + 2 * _vscroll.edge_margin();
	}

	int markdown_view::laid_out_lines() const
	{
		return _doc ? std::min(static_cast<int>(_doc->size()), static_cast<int>(_lines.size())) : 0;
	}

	int markdown_view::clamp_offset(const int offset, const std::string_view text)
	{
		return std::clamp(offset, 0, static_cast<int>(text.size()));
	}

	text_location markdown_view::hit_test(const core::pointi& point, bool* on_text) const
	{
		if (on_text) *on_text = false;

		const auto count = laid_out_lines();
		if (count <= 0) return {};

		text_location location{0, client_to_line(point)};
		const auto& metrics = _lines[location.y];

		(*_doc)[location.y].render(_render_buf);
		const std::string_view line_view(_render_buf);
		const auto line_size = static_cast<int>(line_view.size());

		const auto breaks = markdown_line_breaks(location.y);
		const auto y_in_line = content_y_of(point.y) - metrics.y;
		const auto row = std::clamp((y_in_line - metrics.lead) / std::max(1, metrics.row_height), 0,
		                            static_cast<int>(breaks.size()));
		const auto row_start = row == 0 ? 0 : clamp_offset(breaks[row - 1], line_view);
		const auto row_end = row < static_cast<int>(breaks.size())
			                    ? std::max(row_start, clamp_offset(breaks[row], line_view))
			                    : line_size;

		const auto indent = row > 0 ? metrics.indent : 0;
		const auto row_left = left_pad() + indent * metrics.char_width;
		const auto column = std::max(0, (point.x - row_left) / metrics.char_width);

		location.x = metrics.table >= 0 ? (point.x < _view_extent.cx / 2 ? 0 : line_size)
		                               : offset_at_column(line_view, row_start, row_end, column);

		if (on_text)
		{
			const auto rows_bottom = metrics.lead + metrics.rows * metrics.row_height;
			const auto on_row = point.y >= text_top() && y_in_line >= metrics.lead && y_in_line < rows_bottom;
			const auto on_column = point.x >= row_left && location.x < row_end &&
			                      column_of(line_view, row_start, location.x) >= column;
			*on_text = metrics.table < 0 && on_row && on_column;
		}

		return location;
	}

	int markdown_view::content_y_of(const int screen_y) const
	{
		return screen_y - text_top() + _scroll_offset.y - top_content_padding();
	}

	core::sizei markdown_view::font_extent_for(const uint8_t heading) const
	{
		if (heading >= 1 && heading <= heading_levels)
		{
			const auto& size = _heading_extent[heading - 1];
			if (size.cx > 0 && size.cy > 0) return size;
		}

		return _font_extent;
	}

	render::font_face markdown_view::font_for(const uint8_t heading) const
	{
		return heading >= 1 && heading <= heading_levels ? _heading_fonts[heading - 1] : body_font();
	}

	uint8_t markdown_view::heading_level(const md::block kind)
	{
		switch (kind)
		{
		case md::block::heading1: return 1;
		case md::block::heading2: return 2;
		case md::block::heading3: return 3;
		default: return 0;
		}
	}

	bool markdown_view::offset_within(const std::string_view line, const std::string_view span, const int offset)
	{
		const auto start = static_cast<int>(span.data() - line.data());
		return offset >= start && offset < start + static_cast<int>(span.size());
	}

	int markdown_view::continuation_indent(const md::source_line& parsed, const std::string_view text)
	{
		switch (parsed.kind)
		{
		case md::block::bullet:
		case md::block::numbered:
		case md::block::quote:
			return core::utf8_codepoint_count(text.substr(0, std::min(static_cast<size_t>(parsed.content_start), text.size())));
		default: return 0;
		}
	}

	text_style markdown_view::style_for_span(const md::span& span, const uint8_t heading, const md::block kind)
	{
		if (!span.link.empty()) return text_style::md_link_text;
		if (span.code) return text_style::md_code;
		if (heading == 1) return text_style::md_heading1;
		if (heading == 2) return text_style::md_heading2;
		if (heading == 3) return text_style::md_heading3;
		if (span.bold) return text_style::md_bold;
		if (span.italic) return text_style::md_italic;
		if (kind == md::block::quote) return text_style::md_quote;
		return text_style::normal_text;
	}

	bool markdown_view::spell_check_style(const text_style style)
	{
		switch (style)
		{
		case text_style::md_marker:
		case text_style::md_bullet:
		case text_style::md_link_url:
		case text_style::md_code:
			return false;
		default: return true;
		}
	}

	void markdown_view::build_style_runs(const std::string_view text, const md::source_line& parsed,
	                                     const uint8_t heading) const
	{
		const auto len = static_cast<int>(text.size());
		_byte_style.assign(len, static_cast<uint8_t>(text_style::md_marker));

		const auto fill = [&](const int start, const int length, const text_style style)
		{
			const auto from = std::clamp(start, 0, len);
			const auto to = std::clamp(start + length, from, len);
			std::fill(_byte_style.begin() + from, _byte_style.begin() + to, static_cast<uint8_t>(style));
		};

		if (parsed.kind == md::block::bullet || parsed.kind == md::block::numbered)
		{
			if (const auto first = text.find_first_not_of(" \t"); first != std::string_view::npos)
				fill(static_cast<int>(first), parsed.content_start - static_cast<int>(first), text_style::md_bullet);
		}

		for (const auto& span : parsed.spans)
		{
			if (!span.link.empty())
				fill(static_cast<int>(span.link.data() - text.data()), static_cast<int>(span.link.size()),
				     text_style::md_link_url);

			fill(static_cast<int>(span.text.data() - text.data()), static_cast<int>(span.text.size()),
			     style_for_span(span, heading, parsed.kind));
		}

		_style_runs.clear();
		for (int i = 0; i < len;)
		{
			int end = i + 1;
			while (end < len && _byte_style[end] == _byte_style[i]) ++end;
			_style_runs.push_back({i, end - i, static_cast<text_style>(_byte_style[i])});
			i = end;
		}
	}

	void markdown_view::build_text_runs(const std::string_view text, const text_style style,
	                                    const core::color32 base_color) const
	{
		_runs.clear();
		const auto enabled = _doc && _doc->spell_check() && spell_check_style(style);
		const auto error_color = _theme.style_color(text_style::error_text);

		const auto push_run = [&](const int start, const int length, const core::color32 color)
		{
			if (length <= 0) return;
			if (!_runs.empty() && _runs.back().start + _runs.back().length == start && _runs.back().color == color)
			{
				_runs.back().length += length;
				return;
			}
			_runs.push_back({start, length, color});
		};

		int position = 0;
		const auto len = static_cast<int>(text.size());
		while (position < len)
		{
			int next = position + 1;
			auto color = base_color;

			if (spell::is_word_byte(text[position]))
			{
				while (next < len && spell::is_word_byte(text[next])) ++next;
				if (enabled && !spell::check_word(text.substr(position, next - position))) color = error_color;
			}
			else
			{
				while (next < len && !spell::is_word_byte(text[next])) ++next;
			}

			push_run(position, next - position, color);
			position = next;
		}
	}

	void markdown_view::draw_run(render::draw_context& draw, const core::recti& rect, core::pointi& origin,
	                             const std::string_view text, const int absolute_start,
	                             const render::font_face font, const int font_cx, const core::color32 color,
	                             const core::color32 bg_color, const int selection_begin, const int selection_end,
	                             const core::color32 selection_text_color, const core::color32 selection_bg_color) const
	{
		const auto run_length = static_cast<int>(text.size());
		const auto selection_start = std::clamp(selection_begin - absolute_start, 0, run_length);
		const auto selection_end_in_run = std::clamp(selection_end - absolute_start, 0, run_length);
		auto clip = rect;

		const auto put = [&](const std::string_view part, const core::color32 foreground,
		                     const core::color32 background)
		{
			if (part.empty() || origin.x >= rect.right) return;
			clip.left = origin.x;
			draw.draw_rect(clip, core::color(background));
			draw.draw_text(part, {origin.x, origin.y, clip.right, clip.bottom}, font,
			               render::text_style::single_line, core::color(foreground), {});
			origin.x += font_cx * core::utf8_codepoint_count(part);
		};

		put(text.substr(0, selection_start), color, bg_color);
		put(text.substr(selection_start, selection_end_in_run - selection_start), selection_text_color,
		    selection_bg_color);
		put(text.substr(selection_end_in_run), color, bg_color);
	}

	void markdown_view::draw_markdown_line(render::draw_context& draw, const int top, const int right,
	                                       const int line_index, const line_metrics& metrics) const
	{
		(*_doc)[line_index].render(_line_buf);
		const std::string_view text(_line_buf);

		bool in_fence = metrics.in_fence;
		md::parse_source_line(text, in_fence, _source_line);
		build_style_runs(text, _source_line, metrics.heading);

		const auto font = font_for(metrics.heading);
		const auto bg = _theme.style_color(text_style::normal_bkgnd);
		const auto selection_text_color = _theme.style_color(text_style::sel_text);
		const auto selection_bg_color = _theme.style_color(text_style::sel_bkgnd);
		const auto length = static_cast<int>(text.size());

		int selection_begin = 0;
		int selection_end = 0;
		if (const auto selection = _doc->selection().normalize(); !selection.empty())
		{
			const auto edge = [&](const text_location& at)
			{
				if (at.y > line_index) return length;
				if (at.y == line_index) return std::clamp(at.x, 0, length);
				return 0;
			};
			selection_begin = edge(selection._start);
			selection_end = edge(selection._end);
		}

		const auto breaks = markdown_line_breaks(line_index);
		int y = top + metrics.lead;
		for (int row = 0; row <= static_cast<int>(breaks.size()); ++row)
		{
			const auto row_start = row == 0 ? 0 : clamp_offset(breaks[row - 1], text);
			const auto row_end = row < static_cast<int>(breaks.size())
			                    ? std::max(row_start, clamp_offset(breaks[row], text))
			                    : length;
			const auto row_left = left_pad() + (row > 0 ? metrics.indent * metrics.char_width : 0);
			const core::recti rect(row_left, y, right, y + metrics.row_height);
			core::pointi origin(rect.left, rect.top);

			for (const auto& run : _style_runs)
			{
				const auto run_end = run.start + run.length;
				if (run_end <= row_start || run.start >= row_end) continue;

				const auto from = std::max(run.start, row_start);
				const auto part = text.substr(from, std::min(run_end, row_end) - from);
				build_text_runs(part, run.style, _theme.style_color(run.style));

				for (const auto& piece : _runs)
					draw_run(draw, rect, origin, part.substr(piece.start, piece.length), from + piece.start, font,
					         metrics.char_width, piece.color, bg, selection_begin, selection_end,
					         selection_text_color, selection_bg_color);
			}

			y += metrics.row_height;
		}
	}

	bool markdown_view::is_table_row(const std::string_view line)
	{
		const auto position = line.find_first_not_of(' ');
		return position != std::string_view::npos && line[position] == '|';
	}

	bool markdown_view::is_table_separator(const std::string_view line)
	{
		bool has_dash = false;
		bool has_pipe = false;
		for (const auto ch : line)
		{
			if (ch == '-') has_dash = true;
			else if (ch == '|') has_pipe = true;
			else if (ch != ' ' && ch != ':') return false;
		}
		return has_dash && has_pipe;
	}

	void markdown_view::cell_breaks_fn(std::vector<int>& out, const std::string_view text, const int col_width)
	{
		calc_word_breaks_into(out, text, col_width, [&](const int i, int) -> int
		{
			return core::is_utf8_continuation(text[i]) ? 0 : 1;
		});
	}

	table_layout::table_block markdown_view::find_table(const int line_hint, const int available_columns) const
	{
		table_layout::table_block result;
		const auto line_count = static_cast<int>(_doc->size());
		if (line_hint < 0 || line_hint >= line_count) return result;

		auto& buffer = _table_buf;
		(*_doc)[line_hint].render(buffer);
		if (!is_table_row(buffer)) return result;

		int start = line_hint;
		while (start > 0)
		{
			(*_doc)[start - 1].render(buffer);
			if (!is_table_row(buffer)) break;
			--start;
		}

		int end = line_hint + 1;
		while (end < line_count)
		{
			(*_doc)[end].render(buffer);
			if (!is_table_row(buffer)) break;
			++end;
		}

		if (end - start < 2) return result;
		(*_doc)[start + 1].render(buffer);
		if (!is_table_separator(buffer)) return result;

		result.start_line = start;
		result.end_line = end;
		result.separator_line = start + 1;

		for (int i = start; i < end; ++i)
		{
			if (i == result.separator_line) continue;
			(*_doc)[i].render(buffer);
			table_layout::split_pipe_cells(_cells, buffer);

			while (result.col_widths.size() < _cells.size()) result.col_widths.push_back(0);
			for (size_t column = 0; column < _cells.size(); ++column)
			{
				const auto width = core::utf8_codepoint_count(table_layout::trim_cell(_cells[column]));
				if (width > result.col_widths[column]) result.col_widths[column] = width;
			}
		}

		table_layout::cap_col_widths(result.col_widths, available_columns);
		return result;
	}

	int markdown_view::table_row_visual_rows(const int line_index, const table_layout::table_block& table) const
	{
		(*_doc)[line_index].render(_table_buf);
		table_layout::split_pipe_cells(_cells, _table_buf);

		int rows = 1;
		for (size_t column = 0; column < table.col_widths.size() && column < _cells.size(); ++column)
		{
			const auto visual_rows = table_layout::cell_visual_rows(table_layout::trim_cell(_cells[column]),
			                                                       table.col_widths[column], _cell_breaks,
			                                                       cell_breaks_fn);
			if (visual_rows > rows) rows = visual_rows;
		}
		return rows;
	}

	int markdown_view::draw_table_line(render::draw_context& draw, const int y, const int right,
	                                   const int line_index, const table_layout::table_block& table) const
	{
		const auto font = body_font();
		const auto selected = line_fully_selected(line_index, static_cast<int>(_line_buf.size()));
		const auto bg = _theme.style_color(selected ? text_style::sel_bkgnd : text_style::normal_bkgnd);
		const auto marker = _theme.style_color(selected ? text_style::sel_text : text_style::md_marker);

		if (line_index == table.separator_line)
		{
			table_layout::draw_separator_row(draw, y, left_pad(), right, table, font,
			                                 _font_extent.cx, _font_extent.cy, bg, marker);
			return 1;
		}

		const auto is_header = line_index == table.start_line;
		const auto text_color = _theme.style_color(selected ? text_style::sel_text
		                                                  : is_header ? text_style::md_bold : text_style::normal_text);

		table_layout::split_pipe_cells(_cells, _line_buf);
		return table_layout::draw_table_row(draw, y, left_pad(), right, _cells, table, font,
		                                    _font_extent.cx, _font_extent.cy, is_header, bg, marker,
		                                    text_color, _cell_breaks, cell_breaks_fn);
	}

	void markdown_view::wrap_line_into(std::vector<int>& breaks, const std::string_view text, const int columns)
	{
		calc_word_breaks_into(breaks, text, columns, [&](const int i, int) -> int
		{
			return core::is_utf8_continuation(text[i]) ? 0 : 1;
		});
	}

	int markdown_view::offset_at_column(const std::string_view text, const int start, const int end, const int column)
	{
		int current = 0;
		for (int i = start; i < end; ++i)
		{
			// A display column names the code point that starts there; never return a
			// continuation byte when a UTF-8 character occupies more than one byte.
			if (current >= column && !core::is_utf8_continuation(text[i])) return i;
			if (!core::is_utf8_continuation(text[i])) ++current;
		}
		return std::clamp(end, start, end);
	}

	int markdown_view::column_of(const std::string_view text, const int start, const int offset)
	{
		const auto to = std::min(offset, static_cast<int>(text.size()));
		int columns = 0;
		for (int i = std::clamp(start, 0, to); i < to; ++i)
			if (!core::is_utf8_continuation(text[i])) ++columns;
		return columns;
	}

	std::span<const int> markdown_view::markdown_line_breaks(const int line_index) const
	{
		if (_wrap_offsets.empty() || line_index < 0 || line_index >= static_cast<int>(_wrap_offsets.size()) - 1)
			return {};

		return {_wrap_breaks.data() + _wrap_offsets[line_index],
		        _wrap_breaks.data() + _wrap_offsets[line_index + 1]};
	}

	int markdown_view::markdown_char_to_sub_row(const int line_index, const int byte_offset) const
	{
		const auto breaks = markdown_line_breaks(line_index);
		if (breaks.empty()) return 0;
		for (int i = 0; i < static_cast<int>(breaks.size()); ++i)
			if (byte_offset < breaks[i]) return i;
		return static_cast<int>(breaks.size());
	}

}
