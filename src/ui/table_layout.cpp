// table_layout.cpp — the pure cell-parsing helpers behind the table views.
//
// These are out of line because every view that renders a table includes the header,
// and because being pure makes them testable with no window.

#include "platform.h"
#include "ui/table_layout.h"

#include <algorithm>
#include <cstdint>
#include <numeric>

namespace pf::ui::table_layout
{
	std::string_view trim_cell(const std::string_view s)
	{
		const auto start = s.find_first_not_of(u8' ');
		if (start == std::string_view::npos) return {};
		const auto end = s.find_last_not_of(u8' ');
		return s.substr(start, end - start + 1);
	}

	std::vector<std::string_view>& split_pipe_cells(std::vector<std::string_view>& cells,
	                                                       const std::string_view line)
	{
		cells.clear();
		const auto len = static_cast<int>(line.size());
		int pos = 0;

		// skip leading whitespace
		while (pos < len && line[pos] == u8' ') pos++;
		// skip leading pipe
		if (pos < len && line[pos] == u8'|') pos++;

		while (pos < len)
		{
			const auto next = line.find(u8'|', pos);
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

	std::vector<std::string_view>& split_csv_cells(std::vector<std::string_view>& cells,
	                                                      const std::string_view line, std::string* const storage)
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
			if (line[pos] == u8'"')
			{
				// Quoted field: find matching close quote
				const auto quote_start = pos + 1;
				int end = quote_start;
				auto escaped = false;
				while (end < len)
				{
					if (line[end] == u8'"')
					{
						if (end + 1 < len && line[end + 1] == u8'"')
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
						if (field[i] == u8'"' && i + 1 < field.size() && field[i + 1] == u8'"') ++i;
					}
					cells.push_back(std::string_view(*storage).substr(start));
				}
				else
				{
					cells.push_back(field);
				}

				pos = (end < len) ? end + 1 : end; // skip closing quote
				after_delimiter = pos < len && line[pos] == u8',';
				if (after_delimiter) pos++; // skip delimiter
			}
			else
			{
				const auto next = line.find(u8',', pos);
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

		if (after_delimiter)
			cells.push_back(line.substr(line.size()));

		return cells;
	}

	bool is_right_align_cell(const std::string_view text)
	{
		const auto trimmed = trim_cell(text);
		if (trimmed.empty()) return false;

		const auto len = static_cast<int>(trimmed.size());
		int pos = 0;

		// optional leading sign
		if (pos < len && (trimmed[pos] == u8'+' || trimmed[pos] == u8'-')) pos++;

		// optional currency symbol
		if (pos < len && trimmed[pos] == u8'$') pos++;
		else if (pos + 2 < len && static_cast<uint8_t>(trimmed[pos]) == 0xE2
			&& static_cast<uint8_t>(trimmed[pos + 1]) == 0x82
			&& static_cast<uint8_t>(trimmed[pos + 2]) == 0xAC)
			pos += 3; // EUR
		else if (pos + 1 < len && static_cast<uint8_t>(trimmed[pos]) == 0xC2
			&& static_cast<uint8_t>(trimmed[pos + 1]) == 0xA3)
			pos += 2; // GBP

		bool has_digit = false;
		while (pos < len)
		{
			const auto ch = trimmed[pos];
			if (ch >= u8'0' && ch <= u8'9')
			{
				has_digit = true;
				pos++;
			}
			else if (ch == u8',' || ch == u8'.' || ch == u8' ') pos++;
			else if (ch == u8'%' && pos == len - 1) pos++;
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

}
