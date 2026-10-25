// text_buffer.cpp — the editable text model: movement, editing, undo and redo.
//
// Everything here works on lines of UTF-8 and a selection. Nothing opens a file or
// knows a path; see text_buffer.h for why.

#include "platform.h"
#include "ui/text_buffer.h"

#include <algorithm>
#include <cassert>

namespace pf::ui
{
	static bool is_word_byte(const char c);
	static bool is_space_byte(const char c);

	namespace
	{
		// Join lines with a separator. The application's own string helpers stay in
		// the application; this is the only joining the buffer needs.
		std::string combine(const std::vector<std::string>& lines, const std::string_view endl = "\n")
		{
			std::string result;
			size_t total = 0;
			for (const auto& l : lines) total += l.size() + endl.size();
			result.reserve(total);

			auto first = true;
			for (const auto& l : lines)
			{
				if (!first) result.append(endl);
				result.append(l);
				first = false;
			}
			return result;
		}

		std::string combine_line_text(const std::vector<text_line>& lines)
		{
			std::string result;
			std::string line_text;
			for (const auto& line : lines)
			{
				line.render(line_text);
				result += line_text;
				result += '\n';
			}
			if (!result.empty())
				result.pop_back();
			return result;
		}
	}

	void text_buffer::set_text(const std::string_view text)
	{
		_lines.clear();
		_undo.clear();
		_undo_pos = 0;
		_saved_undo_pos = 0;
		_max_line_len = -1;

		if (text.empty())
		{
			append_line("");
		}
		else
		{
			size_t start = 0;
			for (size_t i = 0; i < text.size(); i++)
			{
				if (text[i] == '\n')
				{
					auto end = i;
					if (end > start && text[end - 1] == '\r')
						end--;
					append_line(text.substr(start, end - start));
					start = i + 1;
				}
			}

			auto end = text.size();
			if (end > start && text[end - 1] == '\r')
				end--;
			append_line(text.substr(start, end - start));
		}

		clamp_locations();
		_ideal_char_pos = -1;
	}

	// The caret, anchor and selection must name places in the text: every accessor
	// indexes _lines with them, so an edit that shortens the text past them leaves a
	// crash for whoever reads them next. Copying a selection that a replaced
	// transcript had outlived read past the last line.
	void text_buffer::clamp_locations()
	{
		const auto inside = [this](text_location at)
		{
			at.y = std::clamp(at.y, 0, static_cast<int>(_lines.size()) - 1);
			at.x = std::clamp(at.x, 0, static_cast<int>(_lines[at.y].size()));
			return at;
		};
		_cursor_loc = inside(_cursor_loc);
		_anchor_loc = inside(_anchor_loc);
		_selection = text_selection(inside(_selection._start), inside(_selection._end));
	}
	bool text_buffer::is_inside_selection(const text_location& ptTextPos) const
	{
		const auto sel = _selection.normalize();

		if (ptTextPos.y < sel._start.y)
			return false;
		if (ptTextPos.y > sel._end.y)
			return false;
		if (ptTextPos.y < sel._end.y && ptTextPos.y > sel._start.y)
			return true;
		if (sel._start.y < sel._end.y)
		{
			if (ptTextPos.y == sel._end.y)
				return ptTextPos.x < sel._end.x;
			assert(ptTextPos.y == sel._start.y);
			return ptTextPos.x >= sel._start.x;
		}
		assert(sel._start.y == sel._end.y);
		return ptTextPos.x >= sel._start.x && ptTextPos.x < sel._end.x;
	}

	void text_buffer::reset()
	{
		_tab_size = 4;
		_max_line_len = -1;
		_ideal_char_pos = -1;
		_anchor_loc.x = 0;
		_anchor_loc.y = 0;

		for (const auto& line : _lines)
		{
			line.invalidate_expanded_length();
		}

		_cursor_loc = {};
		_selection._start = _selection._end = _cursor_loc;
	}

	int text_buffer::max_line_length() const
	{
		if (_max_line_len == -1)
		{
			_max_line_len = 0;
			const auto line_count = std::ssize(_lines);

			for (int i = 0; i < line_count; i++)
			{
				update_max_line_length(i);
			}
		}

		return _max_line_len;
	}

	void text_buffer::update_max_line_length(const int i) const
	{
		const int len = expanded_line_length(i);

		if (_max_line_len < len)
			_max_line_len = len;
	}

	int text_buffer::expanded_line_length(const int line_index) const
	{
		const auto& line = _lines[line_index];

		if (line.expanded_length_cache() == invalid_length)
		{
			auto expanded_len = 0;

			if (!line.empty())
			{
				const auto tab_len = tab_size();
				line.render(_line_scratch);

				for (const auto ch : _line_scratch)
				{
					if (ch == u8'\t')
						expanded_len += tab_len - expanded_len % tab_len;
					else if (!pf::is_utf8_continuation(ch))
						expanded_len++;
				}
			}

			line.set_expanded_length(expanded_len);
		}

		return line.expanded_length_cache();
	}

	void text_buffer::expanded_chars(const std::string_view text, const int offset_in, const int count_in,
	                              std::string& result) const
	{
		result.clear();

		if (count_in <= 0)
			return;

		const auto text_len = static_cast<int>(text.size());
		if (offset_in < 0 || offset_in + count_in > text_len)
			return;

		const auto* const chars = text.data() + offset_in;

		if (memchr(chars, '\t', count_in) == nullptr)
		{
			result.assign(chars, count_in);
			return;
		}

		const auto tab_len = tab_size();
		auto actual_offset = 0;

		for (auto i = 0; i < offset_in; i++)
		{
			if (text[i] == u8'\t')
				actual_offset += tab_len - actual_offset % tab_len;
			else if (!pf::is_utf8_continuation(text[i]))
				actual_offset++;
		}

		auto cur_pos = 0;

		for (auto i = 0; i < count_in; i++)
		{
			const auto c = chars[i];

			if (c == u8'\t')
			{
				auto space_count = tab_len - (actual_offset + cur_pos) % tab_len;
				result.append(space_count, u8' ');
				cur_pos += space_count;
			}
			else
			{
				result += c;
				if (!pf::is_utf8_continuation(c))
					cur_pos++;
			}
		}
	}

	int text_buffer::calc_offset(const int lineIndex, const int nCharIndex) const
	{
		auto result = 0;

		if (lineIndex >= 0 && lineIndex < std::ssize(_lines))
		{
			const auto& line = _lines[lineIndex];
			line.render(_line_scratch);

			const auto tabSize = tab_size();
			const auto charLimit = std::min(nCharIndex, static_cast<int>(_line_scratch.size()));

			for (auto i = 0; i < charLimit; i++)
			{
				if (_line_scratch[i] == u8'\t')
					result += tabSize - result % tabSize;
				else if (!pf::is_utf8_continuation(_line_scratch[i]))
					result++;
			}
		}

		return result;
	}

	int text_buffer::calc_offset_approx(const int lineIndex, const int nOffset) const
	{
		if (nOffset == 0)
			return 0;

		if (lineIndex < 0 || lineIndex >= std::ssize(_lines))
			return 0;

		const auto& line = _lines[lineIndex];
		line.render(_line_scratch);

		const auto nLength = static_cast<int>(_line_scratch.size());
		int nCurrentOffset = 0;
		const int tabSize = tab_size();

		for (int i = 0; i < nLength; i++)
		{
			if (_line_scratch[i] == u8'\t')
				nCurrentOffset += tabSize - nCurrentOffset % tabSize;
			else if (!pf::is_utf8_continuation(_line_scratch[i]))
				nCurrentOffset++;

			if (nCurrentOffset >= nOffset)
			{
				if (nOffset <= nCurrentOffset - tabSize / 2)
					return i;
				return i + 1;
			}
		}
		return nLength;
	}


	void text_buffer::anchor_pos(const text_location& ptNewAnchor)
	{
		_anchor_loc = ptNewAnchor;
	}

	void text_buffer::cursor_pos(const text_location& pos)
	{
		if (_cursor_loc != pos)
		{
			_cursor_loc = pos;
			_ideal_char_pos = calc_offset(_cursor_loc.y, _cursor_loc.x);
			_host.invalidate_caret();
		}
	}

	void text_buffer::finalize_move(const bool selecting)
	{
		_ideal_char_pos = calc_offset(_cursor_loc.y, _cursor_loc.x);
		_host.ensure_visible(_cursor_loc);
		if (!selecting)
			_anchor_loc = _cursor_loc;
		select(text_selection(_anchor_loc, _cursor_loc));
	}

	void text_buffer::move_char_left(const bool selecting)
	{
		const auto sel = _selection.normalize();

		if (!sel.empty() && !selecting)
		{
			_cursor_loc = sel._start;
		}
		else
		{
			if (_cursor_loc.x == 0)
			{
				if (_cursor_loc.y > 0)
				{
					_cursor_loc.y--;
					_cursor_loc.x = static_cast<int>(_lines[_cursor_loc.y].size());
				}
			}
			else
			{
				std::string line_text;
				_lines[_cursor_loc.y].render(line_text);
				_cursor_loc.x = pf::utf8_prev(line_text, _cursor_loc.x);
			}
		}
		finalize_move(selecting);
	}

	void text_buffer::move_char_right(const bool selecting)
	{
		const auto sel = _selection.normalize();

		if (!sel.empty() && !selecting)
		{
			_cursor_loc = sel._end;
		}
		else
		{
			if (_cursor_loc.x == static_cast<int>(_lines[_cursor_loc.y].size()))
			{
				if (_cursor_loc.y < static_cast<int>(_lines.size()) - 1)
				{
					_cursor_loc.y++;
					_cursor_loc.x = 0;
				}
			}
			else
			{
				std::string line_text;
				_lines[_cursor_loc.y].render(line_text);
				_cursor_loc.x = pf::utf8_next(line_text, _cursor_loc.x);
			}
		}
		finalize_move(selecting);
	}

	// Word/space classifiers for navigation. Every non-ASCII byte counts as a word byte, so a
	// scan can never stop between the lead and continuation bytes of one codepoint.
	static bool is_word_byte(const char c)
	{
		const auto b = static_cast<unsigned char>(c);
		return b >= 0x80 || isalnum(b) != 0 || b == '_';
	}

	static bool is_space_byte(const char c)
	{
		const auto b = static_cast<unsigned char>(c);
		return b < 0x80 && isspace(b) != 0;
	}

	void text_buffer::move_word_left(const bool selecting)
	{
		const auto sel = _selection.normalize();

		if (!sel.empty() && !selecting)
		{
			move_char_left(selecting);
			return;
		}

		if (_cursor_loc.x == 0)
		{
			if (_cursor_loc.y == 0)
				return;

			_cursor_loc.y--;
			_cursor_loc.x = static_cast<int>(_lines[_cursor_loc.y].size());
		}

		const auto& line = _lines[_cursor_loc.y];
		std::string line_view;
		line.render(line_view);
		auto nPos = _cursor_loc.x;

		while (nPos > 0 && is_space_byte(line_view[nPos - 1]))
			nPos--;

		if (nPos > 0)
		{
			nPos--;
			if (is_word_byte(line_view[nPos]))
			{
				while (nPos > 0 && is_word_byte(line_view[nPos - 1]))
					nPos--;
			}
			else
			{
				while (nPos > 0 && !is_word_byte(line_view[nPos - 1]) && !is_space_byte(line_view[nPos - 1]))
					nPos--;
			}
		}

		_cursor_loc.x = nPos;
		finalize_move(selecting);
	}

	void text_buffer::move_word_right(const bool selecting)
	{
		const auto sel = _selection.normalize();

		if (!sel.empty() && !selecting)
		{
			move_char_right(selecting);
			return;
		}

		const auto line_len = static_cast<int>(_lines[_cursor_loc.y].size());

		if (_cursor_loc.x == line_len)
		{
			if (_cursor_loc.y == static_cast<int>(_lines.size()) - 1)
				return;
			_cursor_loc.y++;
			_cursor_loc.x = 0;
		}

		const auto nLength = static_cast<int>(_lines[_cursor_loc.y].size());

		if (_cursor_loc.x == nLength)
		{
			move_char_right(selecting);
			return;
		}

		const auto& line = _lines[_cursor_loc.y];
		std::string line_text;
		line.render(line_text);

		int nPos = _cursor_loc.x;

		if (is_word_byte(line_text[nPos]))
		{
			while (nPos < nLength && is_word_byte(line_text[nPos]))
				nPos++;
		}
		else
		{
			while (nPos < nLength && !is_word_byte(line_text[nPos]) && !is_space_byte(line_text[nPos]))
				nPos++;
		}

		while (nPos < nLength && is_space_byte(line_text[nPos]))
			nPos++;

		_cursor_loc.x = nPos;
		finalize_move(selecting);
	}

	void text_buffer::move_lines(const int lines_to_move, const bool selecting)
	{
		const auto sel = _selection.normalize();
		const auto line_count = static_cast<int>(_lines.size());

		if (!sel.empty() && !selecting)
			_cursor_loc = lines_to_move > 0 ? sel._end : sel._start;

		int yy = _cursor_loc.y + lines_to_move;
		if (yy < 0) yy = 0;
		if (yy >= line_count) yy = line_count - 1;

		if (yy != _cursor_loc.y)
		{
			if (_ideal_char_pos == -1)
				_ideal_char_pos = calc_offset(_cursor_loc.y, _cursor_loc.x);

			_cursor_loc.y = yy;
			_cursor_loc.x = calc_offset_approx(_cursor_loc.y, _ideal_char_pos);

			const auto size = static_cast<int>(_lines[_cursor_loc.y].size());

			if (_cursor_loc.x > size)
				_cursor_loc.x = size;
		}
		_host.ensure_visible(_cursor_loc);
		if (!selecting)
			_anchor_loc = _cursor_loc;
		select(text_selection(_anchor_loc, _cursor_loc));
	}

	void text_buffer::move_line_home(const bool selecting)
	{
		const auto& line = _lines[_cursor_loc.y];

		std::string line_text;
		line.render(line_text);
		const auto line_len = static_cast<int>(line_text.size());

		int nHomePos = 0;
		while (nHomePos < line_len && is_space_byte(line_text[nHomePos]))
			nHomePos++;
		if (nHomePos == line_len || _cursor_loc.x == nHomePos)
			_cursor_loc.x = 0;
		else
			_cursor_loc.x = nHomePos;
		finalize_move(selecting);
	}

	void text_buffer::move_line_end(const bool selecting)
	{
		_cursor_loc.x = static_cast<int>(_lines[_cursor_loc.y].size());
		finalize_move(selecting);
	}

	void text_buffer::move_doc_home(const bool selecting)
	{
		_cursor_loc.x = 0;
		_cursor_loc.y = 0;
		finalize_move(selecting);
	}

	void text_buffer::move_doc_end(const bool selecting)
	{
		_cursor_loc.y = static_cast<int>(_lines.size()) - 1;
		_cursor_loc.x = static_cast<int>(_lines[_cursor_loc.y].size());
		finalize_move(selecting);
	}

	text_location text_buffer::word_to_right(text_location pt) const
	{
		const auto& line = _lines[pt.y];
		std::string line_text;
		line.render(line_text);
		const auto line_len = static_cast<int>(line_text.size());

		while (pt.x < line_len && is_word_byte(line_text[pt.x]))
			pt.x++;

		return pt;
	}

	text_location text_buffer::word_to_left(text_location pt) const
	{
		const auto& line = _lines[pt.y];
		std::string line_text;
		line.render(line_text);

		while (pt.x > 0 && is_word_byte(line_text[pt.x - 1]))
			pt.x--;

		return pt;
	}

	std::string text_buffer::copy() const
	{
		if (_selection._start == _selection._end)
			return {};

		const auto sel = _selection.normalize();
		return combine(text(sel), "\r\n");
	}

	bool text_buffer::can_paste()
	{
		return pf::platform_clipboard_has_text();
	}

	bool text_buffer::query_editable() const
	{
		return !_read_only;
	}

	void text_buffer::edit_paste(const std::string_view text)
	{
		if (query_editable() && !text.empty())
		{
			undo_group ug(*this);
			const auto pos = delete_text(ug, selection());
			select(insert_text(ug, pos, text));
		}
	}

	std::string text_buffer::edit_cut()
	{
		if (query_editable() && has_selection())
		{
			const auto sel = selection();
			auto result = combine(text(sel), "\r\n");

			undo_group ug(*this);
			select(delete_text(ug, sel));
			return result;
		}
		return {};
	}

	void text_buffer::edit_delete()
	{
		if (query_editable())
		{
			auto sel = selection();

			if (sel.empty())
			{
				if (sel._end.x == static_cast<int>(_lines[sel._end.y].size()))
				{
					if (sel._end.y == static_cast<int>(_lines.size()) - 1)
						return;

					sel._end.y++;
					sel._end.x = 0;
				}
				else
				{
					// Advance past the full UTF-8 codepoint
					std::string line_text;
					_lines[sel._end.y].render(line_text);
					auto pos = sel._end.x;
					if (pos < static_cast<int>(line_text.size()))
					{
						pos++;
						while (pos < static_cast<int>(line_text.size()) && pf::is_utf8_continuation(line_text[pos]))
							pos++;
					}
					sel._end.x = pos;
				}
			}

			undo_group ug(*this);
			select(delete_text(ug, sel));
		}
	}

	void text_buffer::edit_delete_back()
	{
		if (query_editable())
		{
			if (has_selection())
			{
				edit_delete();
			}
			else
			{
				undo_group ug(*this);
				select(delete_text(ug, cursor_pos()));
			}
		}
	}

	text_buffer::block_range text_buffer::prepare_block_selection(text_selection& sel)
	{
		const int nStartLine = sel._start.y;
		int nEndLine = sel._end.y;
		sel._start.x = 0;

		if (sel._end.x > 0)
		{
			if (sel._end.y == static_cast<int>(_lines.size()) - 1)
			{
				sel._end.x = static_cast<int>(_lines[sel._end.y].size());
			}
			else
			{
				sel._end.x = 0;
				sel._end.y++;
			}
		}
		else
		{
			nEndLine--;
		}

		select(sel);
		cursor_pos(sel._end);
		_host.ensure_visible(sel._end);

		return {nStartLine, nEndLine};
	}

	void text_buffer::edit_tab()
	{
		if (query_editable())
		{
			auto sel = selection();

			if (sel._end.y > sel._start.y)
			{
				undo_group ug(*this);
				const auto [nStartLine, nEndLine] = prepare_block_selection(sel);

				static constexpr char pszText[] = "\t";

				for (int i = nStartLine; i <= nEndLine; i++)
				{
					insert_text(ug, text_location(0, i), pszText);
				}

				_host.invalidate_scrollbar();
			}
			else
			{
				undo_group ug(*this);
				const auto pos = delete_text(ug, selection());
				select(insert_text(ug, pos, L'\t'));
			}
		}
	}

	void text_buffer::edit_untab()
	{
		if (query_editable())
		{
			auto sel = selection();

			if (sel._end.y > sel._start.y)
			{
				std::string line_text;
				undo_group ug(*this);
				const auto [nStartLine, nEndLine] = prepare_block_selection(sel);

				for (int i = nStartLine; i <= nEndLine; i++)
				{
					const auto& line = _lines[i];
					line.render(line_text);

					if (!line.empty())
					{
						const auto line_len = static_cast<int>(line_text.size());
						int nPos = 0, nOffset = 0;

						while (nPos < line_len)
						{
							if (line_text[nPos] == L' ')
							{
								nPos++;
								if (++nOffset >= tab_size())
									break;
							}
							else
							{
								if (line_text[nPos] == L'\t')
									nPos++;
								break;
							}
						}

						if (nPos > 0)
						{
							delete_text(ug, text_selection(0, i, nPos, i));
						}
					}
				}

				_host.invalidate_scrollbar();
			}
			else
			{
				auto ptCursorPos = cursor_pos();

				if (ptCursorPos.x > 0)
				{
					const int tabSize = tab_size();
					if (tabSize == 0) return;
					const int nOffset = calc_offset(ptCursorPos.y, ptCursorPos.x);
					int nNewOffset = nOffset / tabSize * tabSize;
					if (nOffset == nNewOffset && nNewOffset > 0)
						nNewOffset -= tabSize;
					assert(nNewOffset >= 0);

					const auto& line = _lines[ptCursorPos.y];
					std::string line_text;
					line.render(line_text);
					int nCurrentOffset = 0;
					int i = 0;

					while (nCurrentOffset < nNewOffset)
					{
						if (line_text[i] == L'\t')
						{
							nCurrentOffset = nCurrentOffset / tabSize * tabSize + tabSize;
						}
						else
						{
							nCurrentOffset++;
						}

						i++;
					}

					assert(nCurrentOffset == nNewOffset);

					ptCursorPos.x = i;
					select(ptCursorPos);
				}
			}
		}
	}

	void text_buffer::edit_undo()
	{
		if (can_undo())
		{
			select(undo());
		}
	}

	void text_buffer::edit_redo()
	{
		if (can_redo())
		{
			select(redo());
		}
	}



	void text_buffer::select(const text_selection& selection)
	{
		if (_selection != selection)
		{
			assert(selection.is_valid());

			anchor_pos(selection._start);
			cursor_pos(selection._end);

			_host.ensure_visible(selection._end);
			_host.invalidate_caret();

			_host.invalidate_lines(selection._start.y, selection._end.y);
			_host.invalidate_lines(_selection._start.y, _selection._end.y);
			_selection = selection;
		}
	}

	void text_buffer::invalidate_line(const int index)
	{
		const auto& line = _lines[index];
		const auto old_len = line.expanded_length_cache();
		line.invalidate_expanded_length();

		_host.lines_changed(index, index);

		const int new_len = expanded_line_length(index);

		if (_max_line_len >= 0 && new_len >= _max_line_len)
		{
			_max_line_len = new_len;
		}
		else if (old_len == _max_line_len)
		{
			// The previously longest line got shorter — must rescan
			_max_line_len = -1;
		}

		_host.invalidate_scrollbar();
	}

	void text_buffer::set_spell_check(const bool enabled)
	{
		if (_spell_check == enabled)
			return;

		_spell_check = enabled;
		_host.invalidate_view();
	}

	void text_buffer::toggle_spell_check()
	{
		set_spell_check(!_spell_check);
	}

	void text_buffer::append_line(std::string_view text)
	{
		_lines.emplace_back(text);
	}

	void text_buffer::clear()
	{
		_lines.clear();
		_undo.clear();
		_undo_pos = 0;
		_read_only = false;
		append_line("");
		reset();
	}

	// BOM byte signatures:
	//   FF FE 00 00  UTF-32, little-endian
	//   00 00 FE FF  UTF-32, big-endian
	//   EF BB BF     UTF-8
	//   FF FE        UTF-16, little-endian
	//   FE FF        UTF-16, big-endian
	//
	std::vector<std::string> text_buffer::text(const text_selection& selection_in) const
	{
		std::vector<std::string> result;
		const auto selection = selection_in.normalize();

		if (!selection.empty())
		{
			std::string line_text;

			if (selection._start.y == selection._end.y)
			{
				_lines[selection._start.y].render(line_text);
				const auto len = static_cast<int>(line_text.size());
				const auto start_x = std::clamp(selection._start.x, 0, len);
				const auto end_x = std::clamp(selection._end.x, start_x, len);
				result.emplace_back(line_text.substr(start_x, end_x - start_x));
			}
			else
			{
				for (int y = selection._start.y; y <= selection._end.y; y++)
				{
					_lines[y].render(line_text);
					const auto len = static_cast<int>(line_text.size());

					if (y == selection._start.y)
					{
						result.emplace_back(line_text.substr(std::clamp(selection._start.x, 0, len)));
					}
					else if (y == selection._end.y)
					{
						result.emplace_back(line_text.substr(0, std::clamp(selection._end.x, 0, len)));
					}
					else
					{
						result.emplace_back(line_text);
					}
				}
			}
		}

		return result;
	}

	bool text_buffer::can_undo() const
	{
		assert(_undo_pos >= 0 && _undo_pos <= _undo.size());
		return _undo_pos > 0;
	}

	bool text_buffer::can_redo() const
	{
		assert(_undo_pos >= 0 && _undo_pos <= _undo.size());
		return _undo_pos < _undo.size();
	}

	text_location text_buffer::apply_undo_step(const undo_step& step)
	{
		if (step.is_insert())
		{
			if (step.is_single_char())
				return delete_text(step.char_end_location());
			return delete_text(step._selection);
		}
		return insert_text(step._selection._start, step._text);
	}

	text_location text_buffer::apply_redo_step(const undo_step& step)
	{
		if (step.is_insert())
			return insert_text(step._selection._start, step._text);
		if (step.is_single_char())
			return delete_text(step.char_end_location());
		return delete_text(step._selection);
	}

	text_location text_buffer::apply_undo(const undo_item& item)
	{
		text_location location;
		for (auto i = item.rbegin(); i != item.rend(); ++i)
			location = apply_undo_step(*i);
		return location;
	}

	text_location text_buffer::apply_redo(const undo_item& item)
	{
		text_location location;
		for (const auto& step : item)
			location = apply_redo_step(step);
		return location;
	}

	text_location text_buffer::undo()
	{
		assert(can_undo());
		_undo_pos--;
		const auto result = apply_undo(_undo[_undo_pos]);
		_modified = (_undo_pos != _saved_undo_pos);
		return result;
	}

	text_location text_buffer::redo()
	{
		assert(can_redo());
		_modified = true;
		const auto result = apply_redo(_undo[_undo_pos]);
		_undo_pos++;
		_modified = (_undo_pos != _saved_undo_pos);
		return result;
	}

	void text_buffer::record_undo(undo_item ui)
	{
		_undo.erase(_undo.begin() + _undo_pos, _undo.end());
		_undo.push_back(std::move(ui));
		_undo_pos = _undo.size();
	}

	text_selection text_buffer::replace_text(undo_group& ug, const text_selection& selection, const std::string_view text)
	{
		if (!query_editable())
			return selection;

		text_selection result;
		result._start = delete_text(ug, selection);
		result._end = insert_text(ug, result._start, text);
		return result;
	}

	text_location text_buffer::insert_text(const text_location& location, const std::string_view text)
	{
		if (text.empty())
			return location;

		// Split input into lines
		std::vector<std::string_view> input_lines;
		size_t start = 0;
		for (size_t i = 0; i < text.size(); i++)
		{
			if (text[i] == u8'\n')
			{
				auto end = i;
				if (end > start && text[end - 1] == u8'\r')
					end--;
				input_lines.push_back(text.substr(start, end - start));
				start = i + 1;
			}
		}
		auto end = text.size();
		if (end > start && text[end - 1] == u8'\r')
			end--;
		input_lines.push_back(std::string_view(text).substr(start, end - start));

		text_location result = location;

		if (input_lines.size() == 1)
		{
			// Single-line insert: splice into existing line
			auto& li = _lines[location.y];
			auto& line_text = _edit_scratch;
			li.render(line_text);
			line_text.insert(location.x, input_lines[0]);
			result.x = location.x + static_cast<int>(input_lines[0].size());
			result.y = location.y;
			li.update(line_text);
			invalidate_line(location.y);
		}
		else
		{
			// Multi-line insert: split current line, bulk-insert new lines
			auto& first_line = _lines[location.y];
			auto& line_text = _edit_scratch;
			first_line.render(line_text);
			const auto tail = line_text.substr(location.x);
			line_text.erase(location.x);
			line_text.append(input_lines[0]);
			first_line.update(line_text);

			// Build new intermediate and last lines
			const auto count = static_cast<int>(input_lines.size());
			std::vector<text_line> new_lines;
			new_lines.reserve(count - 1);

			for (int i = 1; i < count - 1; i++)
				new_lines.emplace_back(input_lines[i]);

			// Last input line gets the tail appended
			new_lines.emplace_back(std::string(input_lines[count - 1]) + tail);

			_lines.insert(_lines.begin() + location.y + 1,
			              std::make_move_iterator(new_lines.begin()),
			              std::make_move_iterator(new_lines.end()));

			result.y = location.y + count - 1;
			result.x = static_cast<int>(input_lines[count - 1].size());

			_max_line_len = -1;
			_host.line_count_changed(location.y, count - 1);
		}

		_modified = true;
		return result;
	}

	text_location text_buffer::insert_text(undo_group& ug, const text_location& location, const std::string_view text)
	{
		if (text.empty())
			return location;

		const auto result_location = insert_text(location, text);
		ug.insert(text_selection(location, result_location), text);
		_modified = true;
		return result_location;
	}

	text_location text_buffer::insert_text(const text_location& location, const char& c)
	{
		text_location resultLocation = location;
		auto& li = _lines[location.y];

		if (c == L'\n')
		{
			// Split - create new line from the tail of the current line
			auto& line_text = _edit_scratch;
			li.render(line_text);
			_edit_scratch2.assign(line_text, location.x);
			line_text.erase(location.x);
			li.update(line_text);

			// Insert after updating li — _lines.insert may invalidate the li reference
			_lines.insert(_lines.begin() + location.y + 1, text_line(_edit_scratch2));

			resultLocation.y = location.y + 1;
			resultLocation.x = 0;

			_max_line_len = -1;
			_host.line_count_changed(location.y, 1);
		}
		else if (c != '\r')
		{
			auto& line_text = _edit_scratch;
			li.render(line_text);
			line_text.insert(line_text.begin() + location.x, c);
			li.update(line_text);

			resultLocation.y = location.y;
			resultLocation.x = location.x + 1;

			invalidate_line(location.y);
		}

		return resultLocation;
	}

	text_location text_buffer::insert_text(undo_group& ug, const text_location& location, const char& c)
	{
		const auto result_location = insert_text(location, c);

		if (result_location != location)
		{
			ug.insert(location, c);
			_modified = true;
		}

		return result_location;
	}

	text_location text_buffer::delete_text(undo_group& ug, const text_location& location)
	{
		if (location.x == 0)
		{
			if (location.y > 0)
			{
				ug.erase(text_location(static_cast<int>(_lines[location.y - 1].size()), location.y - 1), L'\n');
			}
		}
		else
		{
			auto& line_text = _edit_scratch2;
			_lines[location.y].render(line_text);
			const auto start_x = pf::utf8_prev(line_text, location.x);
			ug.erase(text_selection(text_location(start_x, location.y), text_location(location.x, location.y)),
			         std::string_view(line_text).substr(start_x, location.x - start_x));
		}

		_modified = true;
		return delete_text(location);
	}

	text_location text_buffer::delete_text(const text_selection& selection)
	{
		if (!selection.empty())
		{
			if (selection._start.y == selection._end.y)
			{
				auto& li = _lines[selection._start.y];
				auto& line_text = _edit_scratch;
				li.render(line_text);

				line_text.erase(line_text.begin() + selection._start.x, line_text.begin() + selection._end.x);
				li.update(line_text);

				clamp_locations();

				invalidate_line(selection._start.y);
			}
			else
			{
				auto& line_text_start = _edit_scratch;
				auto& line_text_end = _edit_scratch2;
				_lines[selection._start.y].render(line_text_start);
				_lines[selection._end.y].render(line_text_end);

				// Combine: keep prefix of start line + suffix of end line
				line_text_start.erase(selection._start.x);
				line_text_end.erase(0, selection._end.x);
				line_text_start += line_text_end;
				_lines[selection._start.y].update(line_text_start);

				// Erase intermediate lines and the end line
				_lines.erase(_lines.begin() + selection._start.y + 1, _lines.begin() + selection._end.y + 1);

				clamp_locations();

				_max_line_len = -1;
				_host.line_count_changed(selection._start.y, selection._start.y - selection._end.y);
			}
		}


		return selection._start;
	}

	text_location text_buffer::delete_text(undo_group& ug, const text_selection& selection)
	{
		if (selection.empty())
			return selection._start;

		ug.erase(selection, combine(text(selection)));
		_modified = true;
		return delete_text(selection);
	}

	text_location text_buffer::delete_text(const text_location& location)
	{
		const auto& line = _lines[location.y];
		auto resultPos = location;

		if (location.x == 0)
		{
			if (location.y > 0)
			{
				auto& previous = _lines[location.y - 1];

				auto& line_text = _edit_scratch;
				auto& previous_text = _edit_scratch2;
				line.render(line_text);
				previous.render(previous_text);

				resultPos.x = static_cast<int>(previous.size());
				resultPos.y = location.y - 1;

				previous_text.insert(previous_text.end(), line_text.begin(), line_text.end());
				previous.update(previous_text);

				_lines.erase(_lines.begin() + location.y);
				_max_line_len = -1;
				_host.line_count_changed(location.y - 1, -1);
			}
		}
		else
		{
			auto& li = _lines[location.y];
			auto& line_text = _edit_scratch;
			li.render(line_text);

			// Erase the whole codepoint, not a single byte
			const auto start_x = pf::utf8_prev(line_text, location.x);
			line_text.erase(line_text.begin() + start_x, line_text.begin() + location.x);
			li.update(line_text);

			resultPos.x = start_x;
			resultPos.y = location.y;

			invalidate_line(location.y);
		}

		clamp_locations();
		return resultPos;
	}

	std::string text_buffer::str() const
	{
		return combine_line_text(_lines);
	}

}
