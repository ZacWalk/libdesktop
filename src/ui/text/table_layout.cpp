// This file is part of libdesktop, a C++20 platform layer for desktop applications
// Copyright 2026  Zac Walker
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

// Purpose: Table cell parsing, alignment classification, width capping, and fitting a cell's text to its width, on one
// line or wrapped at its words.

// table_layout.cpp — the pure cell-parsing helpers behind the table views.
//
// These are out of line because every view that renders a table includes the header,
// and because being pure makes them testable with no window.

#include "libdesktop/core.h"
#include "libdesktop/ui/text/table_layout.h"

#include <algorithm>
#include <cstdint>
#include <limits>
#include <numeric>

namespace ui::table_layout
{
	void columns::layout(const std::span<const int> natural, const int available, const int minimum, const double scale,
	                     const bool fill, const std::span<const uint8_t> fixed)
	{
		_natural.assign(natural.begin(), natural.end());
		if (!natural.empty()) _user.resize(natural.size(), 0.0);
		_fixed.assign(natural.size(), 0);
		std::copy_n(fixed.begin(), std::min(fixed.size(), _fixed.size()), _fixed.begin());
		_available = std::max(0, available);
		_minimum = std::max(1, minimum);
		_scale = scale > 0.0 ? scale : 1.0;
		_fill = fill;
		arrange();
	}

	bool columns::user_sized(const int column) const
	{
		return column >= 0 && column < static_cast<int>(_user.size()) && _user[column] > 0.0;
	}

	void columns::arrange()
	{
		_widths.resize(_natural.size());
		int64_t total = 0;
		int64_t fixed = 0;
		for (size_t i = 0; i < _widths.size(); ++i)
		{
			_widths[i] = std::max(0, user_sized(static_cast<int>(i)) ? core::round(_user[i] * _scale) : _natural[i]);
			total += _widths[i];
			if (_fixed[i] || user_sized(static_cast<int>(i))) fixed += _widths[i];
		}
		if (total > _available)
		{
			const auto all = fixed >= _available;
			const auto budget = all ? _available : _available - static_cast<int>(fixed);
			const auto denominator = all ? total : total - fixed;
			int64_t consumed = 0;
			auto assigned = 0;
			for (size_t i = 0; i < _widths.size(); ++i)
			{
				if (!all && (_fixed[i] || user_sized(static_cast<int>(i)))) continue;
				consumed += _widths[i];
				const auto end = denominator > 0 ? static_cast<int>(consumed * budget / denominator) : 0;
				_widths[i] = end - assigned;
				assigned = end;
			}
		}
		if (_fill && !_widths.empty())
		{
			const auto others = std::accumulate(_widths.begin(), _widths.end() - 1, 0);
			_widths.back() = std::max(0, _available - others);
		}
	}

	int columns::maximum(const int column) const
	{
		if (column < 0 || column >= static_cast<int>(_widths.size()) - 1) return 0;
		const auto last = static_cast<int>(_widths.size()) - 1;
		auto taken = 0;
		for (int i = 0; i < static_cast<int>(_widths.size()); ++i)
		{
			if (i == column) continue;
			if (i < column || user_sized(i)) taken += _widths[i];
			// A filled last column is the one that makes the room: it gives back what filling gave it, down
			// to what it holds when it may not shrink, as any column after the divider gives back its slack.
			else if (_fixed[i]) taken += i == last && _fill ? std::min(_widths[i], _natural[i]) : _widths[i];
			else taken += std::min(_widths[i], _minimum);
		}
		return std::max(0, _available - taken);
	}

	bool columns::resize(const int column, const int width)
	{
		if (column < 0 || column >= static_cast<int>(_widths.size()) - 1) return false;
		const auto before = _user;
		for (int i = 0; i < column; ++i)
			if (!user_sized(i)) _user[i] = _widths[i] / _scale;
		const auto max = maximum(column);
		_user[column] = std::clamp(width, std::min(_minimum, max), max) / _scale;
		arrange();
		return before != _user;
	}

	bool columns::fit(const int column)
	{
		return column >= 0 && column < static_cast<int>(_natural.size()) && resize(column, _natural[column]);
	}

	void columns::restore(sizes before)
	{
		if (!_natural.empty()) before.resize(_natural.size(), 0.0);
		_user = std::move(before);
		arrange();
	}

	void columns::reset()
	{
		_user.assign(_natural.size(), 0.0);
		arrange();
	}

	core::recti columns::divider_bounds(const int column, const core::recti band, const int gap, const int grip) const
	{
		if (column < 0 || column >= static_cast<int>(_widths.size()) - 1 || band.is_empty()) return {};
		const auto x = band.left + std::accumulate(_widths.begin(), _widths.begin() + column + 1, 0) + column * gap +
		               gap / 2;
		return core::recti(x - std::max(1, grip), band.top, x + std::max(1, grip), band.bottom).intersection(band);
	}

	int columns::divider_at(const core::pointi point, const core::recti band, const int gap, const int grip) const
	{
		for (int i = 0; i < static_cast<int>(_widths.size()) - 1; ++i)
			if (divider_bounds(i, band, gap, grip).contains(point)) return i;
		return -1;
	}

	bool resize_gesture::begin(const columns& layout, const int column, const int x)
	{
		if (active() || column < 0 || column >= static_cast<int>(layout.widths().size()) - 1) return false;
		_column = column;
		_start_x = x;
		_start_width = layout.widths()[column];
		_before = layout.user_sizes();
		return true;
	}

	bool resize_gesture::update(columns& layout, const int x)
	{
		if (!active() || (x == _start_x && layout.user_sizes() == _before)) return false;
		return layout.resize(_column, _start_width + x - _start_x);
	}

	bool resize_gesture::cancel(columns& layout)
	{
		if (!active()) return false;
		layout.restore(std::move(_before));
		finish();
		return true;
	}

	void size_table(table_block& table, const int available_columns)
	{
		for (auto& width : table.col_widths) width = std::max(1, width);
		cap_col_widths(table.col_widths, available_columns);
		const auto count = static_cast<int>(table.col_widths.size());
		table.sizing.layout(table.col_widths, std::max(count, available_columns - 1 - count * 3));
		table.col_widths = table.sizing.widths();
	}

	core::recti table_header_band(const table_block& table, const int left, const int top, const int char_width,
	                              const int height)
	{
		const auto width = 1 + std::accumulate(table.col_widths.begin(), table.col_widths.end(), 0) +
		                   static_cast<int>(table.col_widths.size()) * 3;
		return {left, top, left + width * char_width, top + height};
	}

	int table_divider_at(const table_block& table, const core::pointi point, const core::recti header,
	                     const int char_width)
	{
		if (char_width <= 0 || !header.contains(point)) return -1;
		auto x = header.left + char_width;
		for (int i = 0; i < static_cast<int>(table.col_widths.size()) - 1; ++i)
		{
			x += (table.col_widths[i] + 2) * char_width;
			if (std::abs(point.x - x) <= std::max(2, char_width / 2)) return i;
			x += char_width;
		}
		return -1;
	}

	std::string_view trim_cell(const std::string_view s)
	{
		const auto start = s.find_first_not_of(' ');
		if (start == std::string_view::npos) return {};
		const auto end = s.find_last_not_of(' ');
		return s.substr(start, end - start + 1);
	}

	std::vector<std::string_view>& split_pipe_cells(std::vector<std::string_view>& cells, const std::string_view line)
	{
		cells.clear();
		const auto len = static_cast<int>(line.size());
		int pos = 0;

		// skip leading whitespace
		while (pos < len && line[pos] == ' ') pos++;
		// skip leading pipe
		if (pos < len && line[pos] == '|') pos++;

		while (pos < len)
		{
			const auto next = line.find('|', pos);
			if (next == std::string_view::npos)
			{
				const auto tail = trim_cell(line.substr(pos));
				if (!tail.empty()) cells.push_back(line.substr(pos, len - pos));
				break;
			}
			cells.push_back(line.substr(pos, next - pos));
			pos = static_cast<int>(next) + 1;
		}
		return cells;
	}

	std::vector<std::string_view>& split_csv_cells(std::vector<std::string_view>& cells, const std::string_view line,
	                                               std::string* const storage)
	{
		cells.clear();
		const auto len = static_cast<int>(line.size());
		int pos = 0;
		auto after_delimiter = false;

		// Unescaping never lengthens a field, so this is all the room it needs, and
		// the views already taken into storage stay valid while it fills.
		if (storage)
		{
			storage->clear();
			storage->reserve(line.size());
		}

		while (pos < len)
		{
			if (line[pos] == '"')
			{
				// Quoted field: find matching close quote
				const auto quote_start = pos + 1;
				int end = quote_start;
				auto escaped = false;
				while (end < len)
				{
					if (line[end] == '"')
					{
						if (end + 1 < len && line[end + 1] == '"')
						{
							end += 2; // escaped quote
							escaped = true;
							continue;
						}
						break;
					}
					end++;
				}

				const auto field = line.substr(quote_start, end - quote_start);
				if (escaped && storage)
				{
					const auto start = storage->size();
					for (size_t i = 0; i < field.size(); ++i)
					{
						storage->push_back(field[i]);
						if (field[i] == '"' && i + 1 < field.size() && field[i + 1] == '"') ++i;
					}
					cells.push_back(std::string_view(*storage).substr(start));
				}
				else
				{
					cells.push_back(field);
				}

				pos = (end < len) ? end + 1 : end; // skip closing quote
				after_delimiter = pos < len && line[pos] == ',';
				if (after_delimiter) pos++; // skip delimiter
			}
			else
			{
				const auto next = line.find(',', pos);
				if (next == std::string_view::npos)
				{
					cells.push_back(line.substr(pos));
					after_delimiter = false;
					break;
				}
				cells.push_back(line.substr(pos, next - pos));
				pos = static_cast<int>(next) + 1;
				after_delimiter = true;
			}
		}

		if (after_delimiter) cells.push_back(line.substr(line.size()));

		return cells;
	}

	bool is_right_align_cell(const std::string_view text)
	{
		const auto trimmed = trim_cell(text);
		if (trimmed.empty()) return false;

		const auto len = static_cast<int>(trimmed.size());
		int pos = 0;

		// optional leading sign
		if (pos < len && (trimmed[pos] == '+' || trimmed[pos] == '-')) pos++;

		// optional currency symbol
		if (pos < len && trimmed[pos] == '$') pos++;
		else if (pos + 2 < len && static_cast<uint8_t>(trimmed[pos]) == 0xE2 &&
		         static_cast<uint8_t>(trimmed[pos + 1]) == 0x82 && static_cast<uint8_t>(trimmed[pos + 2]) == 0xAC)
			pos += 3; // EUR
		else if (pos + 1 < len && static_cast<uint8_t>(trimmed[pos]) == 0xC2 &&
		         static_cast<uint8_t>(trimmed[pos + 1]) == 0xA3)
			pos += 2; // GBP

		bool has_digit = false;
		while (pos < len)
		{
			const auto ch = trimmed[pos];
			if (ch >= '0' && ch <= '9')
			{
				has_digit = true;
				pos++;
			}
			else if (ch == ',' || ch == '.' || ch == ' ') pos++;
			else if (ch == '%' && pos == len - 1) pos++;
			else return false;
		}
		return has_digit;
	}

	void cap_col_widths(std::vector<int>& col_widths, const int avail_cols)
	{
		if (col_widths.empty()) return;

		const auto num_cols = static_cast<int>(col_widths.size());
		const auto overhead = 1 + num_cols * 3; // leading pipe + per-col: space+space+pipe
		const auto budget = std::max(num_cols, avail_cols - overhead);

		int total_natural = 0;
		for (const auto w : col_widths) total_natural += w;

		if (total_natural <= budget || total_natural <= 0) return;

		// A column no wider than an even share of the room left keeps its natural
		// width, and only the wider ones are narrowed, splitting what remains. A
		// proportional cut squeezed a four-character price column to one character,
		// stacking it down the row, because one long text column shared the table.
		std::vector<int> order(col_widths.size());
		std::iota(order.begin(), order.end(), 0);
		std::stable_sort(order.begin(), order.end(),
		                 [&col_widths](const int a, const int b) { return col_widths[a] < col_widths[b]; });

		auto remaining = budget;
		auto left = num_cols;
		size_t kept = 0;
		for (; kept < order.size(); ++kept)
		{
			const auto w = col_widths[order[kept]];
			if (static_cast<int64_t>(w) * left > remaining) break;
			remaining -= w;
			--left;
		}

		// Any cells over go to the widest columns, so a column that needed less
		// never ends up wider than one that needed more.
		const auto base = std::max(1, remaining / std::max(1, left));
		const auto extra = left > 0 ? remaining % left : 0;
		for (size_t j = kept; j < order.size(); ++j)
		{
			const auto from_end = static_cast<int>(order.size() - j);
			col_widths[order[j]] = base + (from_end <= extra ? 1 : 0);
		}
	}

	std::string fit_text(render::measure_context& mc, const std::string_view text, const int width,
	                     const render::font_face font)
	{
		constexpr auto unbounded = std::numeric_limits<int>::max() / 4;
		const auto measure = [&mc, font](const std::string_view s)
		{
			return mc.measure_text(s, font, render::text_style::single_line, unbounded).cx;
		};

		if (text.empty() || measure(text) <= width) return std::string(text);

		constexpr std::string_view ellipsis = "\xE2\x80\xA6";
		const auto available = width - measure(ellipsis);
		// The most characters that fit before the ellipsis: as many as `fits` always do, as many as `unfit` never.
		auto fits = 0;
		auto unfit = core::utf8_codepoint_count(text);

		while (fits + 1 < unfit)
		{
			const auto mid = fits + (unfit - fits) / 2;
			if (measure(text.substr(0, core::utf8_truncate(text, mid))) <= available) fits = mid;
			else unfit = mid;
		}

		auto result = std::string(text.substr(0, core::utf8_truncate(text, fits)));
		result += ellipsis;
		return result;
	}

	namespace
	{
		// Each word of the text, in order: what lies between its spaces.
		template <class F> void for_each_word(const std::string_view text, F&& f)
		{
			size_t pos = 0;
			while (pos < text.size())
			{
				while (pos < text.size() && text[pos] == ' ') ++pos;
				if (pos >= text.size()) break;
				const auto end = std::min(text.find(' ', pos), text.size());
				f(text.substr(pos, end - pos));
				pos = end;
			}
		}
	}

	std::vector<std::string> wrap_words(render::measure_context& mc, const std::string_view text, const int width,
	                                    const render::font_face font)
	{
		constexpr auto unbounded = std::numeric_limits<int>::max() / 4;
		const auto measure = [&mc, font](const std::string_view s)
		{
			return mc.measure_text(s, font, render::text_style::single_line, unbounded).cx;
		};

		std::vector<std::string> lines;
		std::string line;
		auto fits = true;

		for_each_word(text, [&](const std::string_view word)
		{
			if (!fits) return;
			if (measure(word) > width)
			{
				fits = false;
				return;
			}

			if (line.empty())
			{
				line = word;
				return;
			}

			auto longer = line;
			longer += ' ';
			longer += word;

			if (measure(longer) <= width)
			{
				line = std::move(longer);
			}
			else
			{
				lines.push_back(std::move(line));
				line = word;
			}
		});

		if (!fits) return {};
		if (!line.empty()) lines.push_back(std::move(line));
		return lines;
	}

	int widest_word(render::measure_context& mc, const std::string_view text, const render::font_face font)
	{
		constexpr auto unbounded = std::numeric_limits<int>::max() / 4;
		auto widest = 0;
		for_each_word(text, [&](const std::string_view word)
		{ widest = std::max(widest, mc.measure_text(word, font, render::text_style::single_line, unbounded).cx); });
		return widest;
	}

}
