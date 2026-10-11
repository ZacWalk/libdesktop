// view_csv.h — comma-separated values as an aligned table.
//
// The backing store is the source text, one record per line, and the view draws it
// through the same table_layout the markdown view uses: a brighter header row, a
// separator under it, columns capped to the width available and numeric cells
// right-aligned.
//
// A cell is drawn padded into its column, so its pixels are not where the source
// says they are. Rather than guess, a drag over the table selects whole records —
// which is what someone copying rows out of a CSV wanted anyway.

#pragma once

#include "ui/table_layout.h"
#include "ui/view_doc_readonly.h"

#include <algorithm>
#include <string>
#include <vector>

namespace pf::ui
{
	class csv_view : public read_only_doc_view
	{
	public:
		csv_view(view_host& host, const theme& th, view_context* context = nullptr)
			: read_only_doc_view(host, th, context)
		{
			_sel_margin = false;
			_word_wrap = true;
		}

		~csv_view() override = default;

		// Records are selectable whole, so a drag is honest even though a cell's
		// pixels are not where its bytes are.
		[[nodiscard]] bool allows_drag_selection() const override { return true; }

		[[nodiscard]] const table_layout::table_block& table() const { return _table; }

		void set_buffer(const text_buffer_ptr& d, highlight_fn highlight) override
		{
			_table = {};
			doc_view::set_buffer(d, std::move(highlight));
			rebuild_table();
		}

		void recalc_vert_scrollbar() override
		{
			_content_extent.cy = top_content_padding() + _total_visual_rows * _font_extent.cy +
				bottom_content_padding();

			const int max_y = std::max(0, _content_extent.cy - (_view_extent.cy - text_top()));

			if (_scroll_offset.y > max_y)
			{
				_scroll_offset.y = max_y;
				_host.invalidate_view();
			}

			const int visible_height = std::max(0, _view_extent.cy - text_top());
			_vscroll.update(_content_extent.cy, visible_height, _scroll_offset.y);
		}

		void layout() override
		{
			rebuild_table();

			_wrap_breaks.clear();
			_wrap_offsets.clear();
			_wrap_line_y.clear();
			_total_visual_rows = 0;

			if (!_doc || _table.col_widths.empty()) return;

			const auto line_count = static_cast<int>(_doc->size());
			_wrap_line_y.assign(line_count + 1, 0);

			int cumulative = 0;

			for (int i = 0; i < line_count; i++)
			{
				_wrap_line_y[i] = cumulative;
				cumulative += record_visual_rows(i);

				// The separator under the header is drawn, not stored, so it has to
				// be counted here or every row below it would be a row too high.
				if (i == 0 && line_count > 1) cumulative += 1;
			}

			_wrap_line_y[line_count] = cumulative;
			_total_visual_rows = cumulative;
		}

		// A record has no columns of its own once it is drawn padded, so a point in
		// one resolves to the record rather than to a byte inside it.
		text_location client_to_text(const pf::ipoint& point) const override
		{
			if (!_doc || _doc->empty()) return {};

			text_location pt{0, client_to_line(point)};
			if (point.x >= _view_extent.cx / 2) pt.x = static_cast<int>((*_doc)[pt.y].size());
			return pt;
		}

	protected:
		void draw_view(pf::window_frame_ptr& window, pf::draw_context& draw) const override
		{
			const auto rc_client = client_rect();
			const auto bg = _theme.style_color(text_style::normal_bkgnd);

			draw.fill_solid_rect(rc_client, bg);

			if (_doc && !_table.col_widths.empty())
			{
				const auto font = body_font();
				const auto font_cx = _font_extent.cx;
				const auto font_cy = _font_extent.cy;
				const auto line_count = static_cast<int>(_doc->size());

				const auto first_vrow = font_cy > 0
					                        ? std::max(0, (_scroll_offset.y - top_content_padding()) / font_cy)
					                        : 0;
				auto line_index = _wrap_line_y.empty() ? 0 : visual_row_to_line_index(first_vrow);
				line_index = std::clamp(line_index, 0, std::max(0, line_count - 1));

				auto y = line_offset(line_index) - _scroll_offset.y + text_top();

				while (y < rc_client.bottom && line_index < line_count)
				{
					(*_doc)[line_index].render(_line_buf);
					table_layout::split_csv_cells(_cells, _line_buf, &_cell_text);

					const auto is_header = line_index == 0;
					const auto selected = line_fully_selected(line_index, static_cast<int>(_line_buf.size()));

					// A record is selected whole or not at all, for the same reason it
					// is hit-tested whole.
					const auto row_bg = selected ? _theme.style_color(text_style::sel_bkgnd) : bg;
					const auto pipe = _theme.style_color(selected ? text_style::sel_text : text_style::md_marker);
					const auto text_color = _theme.style_color(selected
						                                           ? text_style::sel_text
						                                           : is_header
						                                           ? text_style::md_bold
						                                           : text_style::normal_text);

					y += table_layout::draw_table_row(draw, y, left_pad(), rc_client.right, _cells, _table,
					                                  font, font_cx, font_cy, is_header, row_bg, pipe,
					                                  text_color, _cell_breaks, cell_breaks_fn) * font_cy;

					if (is_header && line_count > 1 && y < rc_client.bottom)
					{
						table_layout::draw_separator_row(draw, y, left_pad(), rc_client.right, _table,
						                                 font, font_cx, font_cy, bg,
						                                 _theme.style_color(text_style::md_marker));
						y += font_cy;
					}

					line_index++;
				}
			}

			_vscroll.draw(draw, scrollbar_rect());
			draw_message_bar(draw);
		}

	private:
		table_layout::table_block _table; // the column layout of the whole document

		// Paint and layout scratch, reused across every row
		mutable std::string _line_buf;
		mutable std::vector<std::string_view> _cells;
		mutable std::string _cell_text; // unescaped quoted fields the cells may point into
		mutable std::vector<int> _cell_breaks;

		[[nodiscard]] int left_pad() const { return _font_extent.cx * 2; }

		// Fills the caller's buffer, so wrapping a cell costs no allocation. A
		// continuation byte advances nothing, or a break could land inside a
		// character and draw half of one.
		static void cell_breaks_fn(std::vector<int>& out, const std::string_view text, const int col_w)
		{
			calc_word_breaks_into(out, text, col_w, [&](const int i, int) -> int
			{
				return pf::is_utf8_continuation(text[i]) ? 0 : 1;
			});
		}

		[[nodiscard]] int record_visual_rows(const int line_index) const
		{
			(*_doc)[line_index].render(_line_buf);
			table_layout::split_csv_cells(_cells, _line_buf, &_cell_text);

			int rows = 1;

			for (size_t c = 0; c < _table.col_widths.size() && c < _cells.size(); c++)
			{
				const auto vr = table_layout::cell_visual_rows(
					table_layout::trim_cell(_cells[c]), _table.col_widths[c], _cell_breaks, cell_breaks_fn);
				if (vr > rows) rows = vr;
			}

			return rows;
		}

		void rebuild_table()
		{
			_table = {};

			if (!_doc || _doc->empty()) return;

			const auto line_count = static_cast<int>(_doc->size());
			const auto avail_width = std::max(1, _view_extent.cx - left_pad());
			const auto avail_cols = safe_cols(avail_width, _font_extent.cx);

			_table.start_line = 0;
			_table.end_line = line_count;
			_table.separator_line = -1; // the separator is drawn, not read from a record

			for (int i = 0; i < line_count; i++)
			{
				(*_doc)[i].render(_line_buf);
				table_layout::split_csv_cells(_cells, _line_buf, &_cell_text);

				while (_table.col_widths.size() < _cells.size())
					_table.col_widths.push_back(0);

				for (size_t c = 0; c < _cells.size(); c++)
				{
					const auto w = static_cast<int>(pf::utf8_codepoint_count(table_layout::trim_cell(_cells[c])));
					if (w > _table.col_widths[c]) _table.col_widths[c] = w;
				}
			}

			table_layout::cap_col_widths(_table.col_widths, avail_cols);
		}
	};
}
