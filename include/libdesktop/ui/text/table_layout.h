// This file is part of libdesktop, a C++20 platform layer for desktop applications
// Copyright 2026  Zac Walker
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

// Purpose: Pure table parsing, sizing, wrapping, and drawing helpers.

#pragma once

#include "libdesktop/core.h"
#include "libdesktop/render/context.h"

#include <algorithm>
#include <array>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace ui
{
	// table_layout — Shared table helpers: the Markdown and CSV views' parsing and drawing, and the column sizing,
	// cell fitting and row shading every table uses.
	namespace table_layout
	{
		// Content widths and user widths share the same layout in document tables,
		// lists and element tables. User widths are logical units, not device pixels.
		class columns
		{
			std::vector<int> _natural;
			std::vector<int> _widths;
			std::vector<double> _user;
			std::vector<uint8_t> _fixed;
			int _available = 0;
			int _minimum = 1;
			double _scale = 1.0;
			bool _fill = true;

			void arrange();

		public:
			using sizes = std::vector<double>;

			void layout(std::span<const int> natural, int available, int minimum = 1, double scale = 1.0,
			            bool fill = true, std::span<const uint8_t> fixed = {});
			[[nodiscard]] const std::vector<int>& widths() const { return _widths; }
			[[nodiscard]] const std::vector<int>& natural_widths() const { return _natural; }
			[[nodiscard]] const sizes& user_sizes() const { return _user; }
			[[nodiscard]] bool user_sized(int column) const;
			[[nodiscard]] int maximum(int column) const;
			bool resize(int column, int width);
			bool fit(int column);
			void restore(sizes before);
			void reset();
			[[nodiscard]] core::recti divider_bounds(int column, core::recti band, int gap = 0, int grip = 4) const;
			[[nodiscard]] int divider_at(core::pointi point, core::recti band, int gap = 0, int grip = 4) const;
		};

		class resize_gesture
		{
			columns::sizes _before;
			int _column = -1;
			int _start_x = 0;
			int _start_width = 0;

		public:
			[[nodiscard]] bool active() const { return _column >= 0; }
			bool begin(const columns& layout, int column, int x);
			bool update(columns& layout, int x);
			void finish() { _column = -1; }
			bool cancel(columns& layout);
		};

		struct table_block
		{
			int start_line = -1;
			int end_line = -1; // exclusive
			int separator_line = -1;
			std::vector<int> col_widths; // display width per column (in codepoints, may be capped)
			columns sizing;
		};

		void size_table(table_block& table, int available_columns);
		[[nodiscard]] core::recti table_header_band(const table_block& table, int left, int top, int char_width,
		                                            int height);
		[[nodiscard]] int table_divider_at(const table_block& table, core::pointi point, core::recti header,
		                                   int char_width);

		std::string_view trim_cell(std::string_view s);

		// Split a markdown pipe row / CSV row into cell views over the caller's buffer.
		std::vector<std::string_view>& split_pipe_cells(std::vector<std::string_view>& cells, std::string_view line);

		// A quoted field's escaped quote ("") stands for one quote. Given storage, a
		// field that has one is rewritten there, unescaped; without it the field is
		// the text as it stands. A line that ends with a delimiter ends with an empty
		// field.
		std::vector<std::string_view>& split_csv_cells(std::vector<std::string_view>& cells, std::string_view line,
		                                               std::string* storage = nullptr);

		// True when a cell looks numeric (optional sign, currency, digits, separators, trailing %).
		bool is_right_align_cell(std::string_view text);

		// Narrow the widest columns so the row fits avail_cols character cells. A
		// column no wider than an even share of the room keeps its natural width.
		void cap_col_widths(std::vector<int>& col_widths, int avail_cols);

		inline int utf8_codepoint_count(const std::string_view text)
		{
			int count = 0;
			for (auto it = text.begin(); it != text.end();)
			{
				const auto before = it;
				(void)core::pop_utf8_char(it, text.end());
				if (it == before) break;
				++count;
			}
			return count;
		}

		inline void draw_cell_text(render::draw_context& draw, const int, const int, const core::recti clip,
		                           const std::string_view text, const render::font_face font, const core::color32 fg,
		                           const core::color32 bg)
		{
			draw.draw_rect(clip, core::color(bg));
			draw.draw_text(text, clip, font, render::text_style::single_line, core::color(fg),
			               core::color(core::rgba(0, 0, 0, 0)));
		}

		// Text cut to a width and ended with an ellipsis where it is wider, as a file browser shows a name too long for
		// its column: drawn whole it would be clipped with nothing to say so. Text that fits is as it is, and a
		// character is never split.
		[[nodiscard]] std::string fit_text(render::measure_context& mc, std::string_view text, int width,
		                                   render::font_face font = render::font_face::dialog);

		// Text broken into lines at its spaces, as many words to a line as fit the width, so a name of several words
		// can stand in a column no wider than its widest word. Nothing where a word alone is wider than the width:
		// such text is fitted to one line instead, ending in an ellipsis.
		[[nodiscard]] std::vector<std::string> wrap_words(render::measure_context& mc, std::string_view text, int width,
		                                                  render::font_face font = render::font_face::dialog);

		// How wide the widest word of the text is: the narrowest a column can be and still show it whole, wrapped.
		[[nodiscard]] int widest_word(render::measure_context& mc, std::string_view text,
		                              render::font_face font = render::font_face::dialog);

		// The shade every other row of a table is drawn on, so a row reads across from one column to the next.
		inline void draw_row_stripe(render::draw_context& draw, const core::recti row)
		{
			draw.draw_rounded_rect(row, core::color(0x000000, draw.colors.alpha / 11.11f), draw.padding1);
		}

		// The mark on a sorted column's heading: a chevron at the top of its cell, pointing up for an ascending sort
		// and down for a descending one. It takes none of the heading's width, so sorting never moves the columns.
		inline void draw_sort_mark(render::draw_context& draw, const core::recti cell, const bool ascending,
		                           const core::color c)
		{
			const auto scale = draw.scale_factor;
			const auto half = std::max(2.0, 4.0 * scale);
			const auto height = std::max(2.0, 3.0 * scale);
			const auto top = cell.top + std::max(1.0, scale);
			const auto x = (cell.left + cell.right) / 2.0;
			const std::array<core::pointd, 3> up = {core::pointd{x - half, top + height}, core::pointd{x, top},
			                                        core::pointd{x + half, top + height}};
			const std::array<core::pointd, 3> down = {core::pointd{x - half, top}, core::pointd{x, top + height},
			                                          core::pointd{x + half, top}};
			draw.draw_polyline(ascending ? up : down, c, std::max(1.0, scale));
		}

		// Draw pipe character(s) at x for the given number of sub-rows
		inline void draw_pipe(render::draw_context& draw, const int x, const int y, const int rows,
		                      const render::font_face& font, const int font_cx, const int font_cy,
		                      const core::color32 fg, const core::color32 bg)
		{
			for (int r = 0; r < rows; r++)
			{
				const core::recti pc{x, y + r * font_cy, x + font_cx, y + (r + 1) * font_cy};
				draw_cell_text(draw, x, y + r * font_cy, pc, "|", font, fg, bg);
			}
		}

		// Compute how many visual rows a cell needs when word-wrapped to col_w columns.
		// break_fn fills the caller's buffer so no allocation happens per cell.
		template <typename BreakFn>
		int cell_visual_rows(const std::string_view text, const int col_w, std::vector<int>& breaks, BreakFn&& break_fn)
		{
			breaks.clear();
			if (text.empty() || col_w <= 0) return 1;
			if (utf8_codepoint_count(text) <= col_w) return 1;
			break_fn(breaks, text, col_w);
			return static_cast<int>(breaks.size()) + 1;
		}

		// Draw a single table row with cells word-wrapped to their column widths.
		// Returns the total height in visual rows consumed by this table row.
		template <typename BreakFn>
		int draw_table_row(render::draw_context& draw, const int y, const int left_pad, const int right,
		                   const std::vector<std::string_view>& cells, const table_block& table,
		                   const render::font_face& font, const int font_cx, const int font_cy, const bool is_header,
		                   const core::color32 bg, const core::color32 pipe_color, const core::color32 text_color,
		                   std::vector<int>& breaks, BreakFn&& break_fn)
		{
			// first pass: compute max visual rows across all cells
			int max_rows = 1;
			for (size_t col = 0; col < table.col_widths.size(); col++)
			{
				const auto cell_raw = col < cells.size() ? trim_cell(cells[col]) : std::string_view{};
				const auto vr = cell_visual_rows(cell_raw, table.col_widths[col], breaks, break_fn);
				if (vr > max_rows) max_rows = vr;
			}

			const auto row_height = max_rows * font_cy;
			int x = left_pad;

			// leading pipe
			draw_pipe(draw, left_pad, y, max_rows, font, font_cx, font_cy, pipe_color, bg);
			x = left_pad + font_cx;

			for (size_t col = 0; col < table.col_widths.size(); col++)
			{
				const auto col_w = table.col_widths[col];
				const auto cell_raw = col < cells.size() ? trim_cell(cells[col]) : std::string_view{};
				const auto ralign = !is_header && is_right_align_cell(cell_raw);
				const auto cell_px = (col_w + 2) * font_cx;

				// background fill for entire cell area
				draw.draw_rect(core::recti{x, y, x + cell_px, y + row_height}, core::color(bg));

				// word-wrap the cell text
				break_fn(breaks, cell_raw, col_w);
				const auto num_sub = static_cast<int>(breaks.size()) + 1;
				const auto cell_len = static_cast<int>(cell_raw.size());

				for (int r = 0; r < num_sub; r++)
				{
					const auto rs = r == 0 ? 0 : breaks[r - 1];
					const auto re = r < static_cast<int>(breaks.size()) ? breaks[r] : cell_len;
					auto chunk = cell_raw.substr(rs, re - rs);

					// trim trailing spaces
					while (!chunk.empty() && chunk.back() == ' ') chunk = chunk.substr(0, chunk.size() - 1);

					const auto chunk_cps = utf8_codepoint_count(chunk);
					const auto sub_y = y + r * font_cy;

					const auto text_x = ralign ? x + (1 + col_w - chunk_cps) * font_cx : x + font_cx;
					const core::recti tc{text_x, sub_y, text_x + chunk_cps * font_cx, sub_y + font_cy};
					draw_cell_text(draw, text_x, sub_y, tc, chunk, font, text_color, bg);
				}

				x += cell_px;

				// trailing pipe
				draw_pipe(draw, x, y, max_rows, font, font_cx, font_cy, pipe_color, bg);
				x += font_cx;
			}

			// fill remainder
			if (x < right) draw.draw_rect(core::recti{x, y, right, y + row_height}, core::color(bg));

			return max_rows;
		}

		// Draw a horizontal separator row (dashes between pipes)
		inline void draw_separator_row(render::draw_context& draw, const int y, const int left_pad, const int right,
		                               const table_block& table, const render::font_face& font, const int font_cx,
		                               const int font_cy, const core::color32 bg, const core::color32 pipe_color)
		{
			int x = left_pad;

			draw_pipe(draw, x, y, 1, font, font_cx, font_cy, pipe_color, bg);
			x += font_cx;

			std::string dashes;

			for (const auto col_w : table.col_widths)
			{
				const auto dash_count = col_w + 2;
				dashes.assign(dash_count, '-');
				const core::recti dc{x, y, x + dash_count * font_cx, y + font_cy};
				draw_cell_text(draw, x, y, dc, dashes, font, pipe_color, bg);
				x += dash_count * font_cx;

				draw_pipe(draw, x, y, 1, font, font_cx, font_cy, pipe_color, bg);
				x += font_cx;
			}

			if (x < right) draw.draw_rect(core::recti{x, y, right, y + font_cy}, core::color(bg));
		}
	} // namespace table_layout
}
