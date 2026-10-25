// table_layout.h — table rendering helpers shared by the markdown and CSV views.

#pragma once

#include "platform.h"

namespace pf::ui
{
// table_layout — Shared table rendering helpers used by markdown and CSV views.
namespace table_layout
{
	struct table_block
	{
		int start_line = -1;
		int end_line = -1; // exclusive
		int separator_line = -1;
		std::vector<int> col_widths; // display width per column (in codepoints, may be capped)
	};

	std::string_view trim_cell(std::string_view s);

	// Split a markdown pipe row / CSV row into cell views over the caller's buffer.
	std::vector<std::string_view>& split_pipe_cells(std::vector<std::string_view>& cells,
	                                                std::string_view line);

	// A quoted field's escaped quote ("") stands for one quote. Given storage, a
	// field that has one is rewritten there, unescaped; without it the field is
	// the text as it stands. A line that ends with a delimiter ends with an empty
	// field.
	std::vector<std::string_view>& split_csv_cells(std::vector<std::string_view>& cells,
	                                               std::string_view line, std::string* storage = nullptr);

	// True when a cell looks numeric (optional sign, currency, digits, separators, trailing %).
	bool is_right_align_cell(std::string_view text);

	// Narrow the widest columns so the row fits avail_cols character cells. A
	// column no wider than an even share of the room keeps its natural width.
	void cap_col_widths(std::vector<int>& col_widths, int avail_cols);

	// Draw pipe character(s) at x for the given number of sub-rows
	inline void draw_pipe(pf::draw_context& draw, const int x, const int y, const int rows,
	                      const pf::font& font, const int font_cx, const int font_cy,
	                      const pf::color_t fg, const pf::color_t bg)
	{
		for (int r = 0; r < rows; r++)
		{
			const pf::irect pc{x, y + r * font_cy, x + font_cx, y + (r + 1) * font_cy};
			draw.draw_text(x, y + r * font_cy, pc, "|", font, fg, bg);
		}
	}

	// Compute how many visual rows a cell needs when word-wrapped to col_w columns.
	// break_fn fills the caller's buffer so no allocation happens per cell.
	template <typename BreakFn>
	int cell_visual_rows(const std::string_view text, const int col_w,
	                     std::vector<int>& breaks, BreakFn&& break_fn)
	{
		breaks.clear();
		if (text.empty() || col_w <= 0) return 1;
		if (pf::utf8_codepoint_count(text) <= col_w) return 1;
		break_fn(breaks, text, col_w);
		return static_cast<int>(breaks.size()) + 1;
	}

	// Draw a single table row with cells word-wrapped to their column widths.
	// Returns the total height in visual rows consumed by this table row.
	template <typename BreakFn>
	int draw_table_row(pf::draw_context& draw, const int y, const int left_pad, const int right,
	                   const std::vector<std::string_view>& cells, const table_block& table,
	                   const pf::font& font, const int font_cx, const int font_cy,
	                   const bool is_header, const pf::color_t bg, const pf::color_t pipe_color,
	                   const pf::color_t text_color, std::vector<int>& breaks, BreakFn&& break_fn)
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
			draw.fill_solid_rect(x, y, cell_px, row_height, bg);

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
				while (!chunk.empty() && chunk.back() == u8' ')
					chunk = chunk.substr(0, chunk.size() - 1);

				const auto chunk_cps = pf::utf8_codepoint_count(chunk);
				const auto sub_y = y + r * font_cy;

				const auto text_x = ralign
					                    ? x + (1 + col_w - chunk_cps) * font_cx
					                    : x + font_cx;
				const pf::irect tc{text_x, sub_y, text_x + chunk_cps * font_cx, sub_y + font_cy};
				draw.draw_text(text_x, sub_y, tc, chunk, font, text_color, bg);
			}

			x += cell_px;

			// trailing pipe
			draw_pipe(draw, x, y, max_rows, font, font_cx, font_cy, pipe_color, bg);
			x += font_cx;
		}

		// fill remainder
		if (x < right)
			draw.fill_solid_rect(x, y, right - x, row_height, bg);

		return max_rows;
	}

	// Draw a horizontal separator row (dashes between pipes)
	inline void draw_separator_row(pf::draw_context& draw, const int y, const int left_pad,
	                               const int right, const table_block& table,
	                               const pf::font& font, const int font_cx, const int font_cy,
	                               const pf::color_t bg, const pf::color_t pipe_color)
	{
		int x = left_pad;

		draw_pipe(draw, x, y, 1, font, font_cx, font_cy, pipe_color, bg);
		x += font_cx;

		std::string dashes;

		for (const auto col_w : table.col_widths)
		{
			const auto dash_count = col_w + 2;
			dashes.assign(dash_count, u8'-');
			const pf::irect dc{x, y, x + dash_count * font_cx, y + font_cy};
			draw.draw_text(x, y, dc, dashes, font, pipe_color, bg);
			x += dash_count * font_cx;

			draw_pipe(draw, x, y, 1, font, font_cx, font_cy, pipe_color, bg);
			x += font_cx;
		}

		if (x < right)
			draw.fill_solid_rect(x, y, right - x, font_cy, bg);
	}
} // namespace table_layout
}
