// text_buffer.h — the editable text model behind every text view.
//
// Holds lines of UTF-8, a selection, a cursor and an undo history, and nothing else.
// It does not open files, know a path, choose a syntax highlighter or decide whether
// a document should be spell checked: those are the application's, and keeping them
// out is what lets three applications share one editor.
//
// Positions are UTF-8 byte offsets. Step them with pf::utf8_next / pf::utf8_prev,
// never with ++ or --.

#pragma once

#include "platform.h"
#include "ui/spell.h"
#include "ui/text_line.h"
#include "ui/text_types.h"
#include "ui/view_host.h"

#include <cassert>
#include <memory>
#include <string>
#include <vector>

namespace pf::ui
{
	class undo_group;

	constexpr uint32_t invalid_cookie = UINT32_MAX;

	enum class undo_action { insert, erase };

	struct undo_step
	{
		text_selection _selection;
		undo_action _action = undo_action::insert;
		std::string _text;

		undo_step() = default;

		undo_step(const text_location& location, const char& c, const undo_action action) :
			_selection(location, location), _action(action), _text(1, c)
		{
		}

		undo_step(const text_selection& selection, std::string text, const undo_action action) :
			_selection(selection), _action(action), _text(std::move(text))
		{
		}

		undo_step(const text_selection& selection, const std::string_view text, const undo_action action) :
			_selection(selection), _action(action), _text(text)
		{
		}

		[[nodiscard]] bool is_insert() const { return _action == undo_action::insert; }
		[[nodiscard]] bool is_erase() const { return _action == undo_action::erase; }
		[[nodiscard]] bool is_single_char() const { return _text.size() == 1; }

		// For a single-char step, compute the position just past the inserted character.
		// Newline -> (0, y+1), other char -> (x+1, y).
		[[nodiscard]] text_location char_end_location() const
		{
			assert(!_text.empty());
			if (_text[0] == '\n')
				return text_location(0, _selection._start.y + 1);
			return text_location(_selection._start.x + 1, _selection._start.y);
		}
	};

	struct undo_item
	{
		void add_insert(const text_location& location, const char& c)
		{
			_steps.emplace_back(location, c, undo_action::insert);
		}

		void add_insert(const text_selection& selection, std::string_view text)
		{
			_steps.emplace_back(selection, text, undo_action::insert);
		}

		void add_erase(const text_location& location, const char& c)
		{
			_steps.emplace_back(location, c, undo_action::erase);
		}

		void add_erase(const text_selection& selection, std::string_view text)
		{
			_steps.emplace_back(selection, text, undo_action::erase);
		}

		[[nodiscard]] bool empty() const { return _steps.empty(); }
		[[nodiscard]] auto begin() const { return _steps.begin(); }
		[[nodiscard]] auto end() const { return _steps.end(); }
		[[nodiscard]] auto rbegin() const { return _steps.rbegin(); }
		[[nodiscard]] auto rend() const { return _steps.rend(); }

	private:
		std::vector<undo_step> _steps;
	};

	// --- Document ---


	class text_buffer
	{
	protected:
		// Protected rather than private: an application subclasses this to add
		// loading and saving, and replacing the lines is exactly what loading does.
		view_host& _host;

		text_location _anchor_loc;
		text_location _cursor_loc;
		text_selection _selection;
		bool _read_only = false;
		bool _spell_check = false;
		int _ideal_char_pos = 0;
		int _tab_size = 4;
		mutable int _max_line_len = -1;

		mutable bool _modified = false;
		std::vector<text_line> _lines;
		std::vector<undo_item> _undo;

		// Reused by the measurement helpers below; they run per line on every layout and paint
		mutable std::string _line_scratch;

		// Reused by the edit paths so a keystroke does not allocate
		std::string _edit_scratch;
		std::string _edit_scratch2;

		size_t _undo_pos = 0;
		mutable size_t _saved_undo_pos = 0;

		text_location apply_undo_step(const undo_step& step);
		text_location apply_redo_step(const undo_step& step);
		text_location apply_undo(const undo_item& item);
		text_location apply_redo(const undo_item& item);

		text_location insert_text(const text_location& location, std::string_view text);
		text_location insert_text(const text_location& location, const char& c);
		text_location delete_text(const text_selection& selection);
		text_location delete_text(const text_location& location);
		void clamp_locations();

		struct block_range
		{
			int start_line;
			int end_line;
		};

		block_range prepare_block_selection(text_selection& sel);

	public:
		// Loading or saving decides a document is clean; that judgement belongs to
		// whatever owns the file, so it says so here.
		void set_modified(const bool modified) const { _modified = modified; }

		// The host outlives the buffer: it is the view or application that owns it.
		explicit text_buffer(view_host& host) : _host(host)
		{
			append_line("");
		}

		virtual ~text_buffer() = default;

		text_buffer(const text_buffer&) = delete;
		text_buffer& operator=(const text_buffer&) = delete;

		// Replace the whole document with UTF-8 text, splitting it into lines on
		// either CRLF or LF. This is how content gets in: the buffer never reads a
		// file, so whatever loaded the bytes hands them over here.
		void set_text(std::string_view text);

		void clear();

		[[nodiscard]] bool is_modified() const
		{
			return _modified;
		}

		[[nodiscard]] bool is_read_only() const
		{
			return _read_only;
		}

		void read_only(const bool ro)
		{
			_read_only = ro;
		}

		[[nodiscard]] bool empty() const
		{
			return _lines.empty();
		}

		[[nodiscard]] size_t size() const
		{
			return _lines.size();
		}

		const text_line& operator[](const int n) const
		{
			return _lines[n];
		}

		text_line& operator[](const int n)
		{
			return _lines[n];
		}

		std::vector<std::string> text(const text_selection& selection) const;

		void append_line(std::string_view text);

		text_selection replace_text(undo_group& ug, const text_selection& selection, std::string_view text);

		text_location insert_text(undo_group& ug, const text_location& location, std::string_view text);
		text_location insert_text(undo_group& ug, const text_location& location, const char& c);
		text_location delete_text(undo_group& ug, const text_selection& selection);
		text_location delete_text(undo_group& ug, const text_location& location);


		[[nodiscard]] bool can_undo() const;
		[[nodiscard]] bool can_redo() const;
		text_location undo();
		text_location redo();
		void record_undo(undo_item ui);

		[[nodiscard]] const std::vector<text_line>& lines() const
		{
			return _lines;
		}

		static bool can_paste();

		[[nodiscard]] bool has_selection() const
		{
			return !_selection.empty();
		}

		[[nodiscard]] bool query_editable() const;

		std::string edit_cut();
		std::string copy() const;
		void edit_delete();
		void edit_delete_back();
		void edit_redo();
		void edit_tab();
		void edit_undo();
		void edit_untab();
		void edit_paste(std::string_view text);

		[[nodiscard]] text_selection selection() const
		{
			return _selection.normalize();
		}

		void move_doc_end(bool selecting);
		void move_doc_home(bool selecting);
		void move_line_end(bool selecting);
		void move_line_home(bool selecting);
		void move_char_left(bool selecting);
		void move_char_right(bool selecting);
		void move_word_left(bool selecting);
		void move_word_right(bool selecting);
		void move_lines(int lines_to_move, bool selecting);

		bool is_inside_selection(const text_location& loc) const;
		void reset();
		void finalize_move(bool selecting);

		std::string str() const;

		[[nodiscard]] text_location end() const
		{
			const auto last_line = static_cast<int>(_lines.size()) - 1;
			return text_location(static_cast<int>(_lines[last_line].size()), last_line);
		}

		[[nodiscard]] text_selection all() const
		{
			return text_selection(text_location(0, 0), end());
		}

		[[nodiscard]] int tab_size() const
		{
			return _tab_size;
		}

		int max_line_length() const;
		text_location word_to_left(text_location pt) const;
		text_location word_to_right(text_location pt) const;

		int calc_offset(int lineIndex, int nCharIndex) const;
		int calc_offset_approx(int lineIndex, int nOffset) const;
		int expanded_line_length(int line_index) const;
		void expanded_chars(std::string_view text, int offset_in, int count_in, std::string& result) const;

		[[nodiscard]] const text_location& cursor_pos() const
		{
			return _cursor_loc;
		}

		[[nodiscard]] const text_location& anchor_pos() const
		{
			return _anchor_loc;
		}

		void anchor_pos(const text_location& ptNewAnchor);
		void cursor_pos(const text_location& ptCursorPos);

		void update_max_line_length(int lineIndex) const;

		void move_to(text_location pos, const bool selecting)
		{
			const auto limit = static_cast<int>(_lines[pos.y].size());

			if (pos.x > limit)
			{
				pos.x = limit;
			}

			_cursor_loc = pos;

			if (!selecting)
			{
				_anchor_loc = _cursor_loc;
			}

			select(text_selection(_anchor_loc, _cursor_loc));
		}

		text_selection word_selection(const text_location& pos, const bool from_anchor) const
		{
			const auto ptStart = from_anchor ? _anchor_loc : pos;
			const auto ptEnd = pos;

			if (ptStart < ptEnd || ptStart == ptEnd)
			{
				return text_selection(word_to_left(ptStart), word_to_right(ptEnd));
			}
			return text_selection(word_to_right(ptStart), word_to_left(ptEnd));
		}

		text_selection word_selection() const
		{
			if (_cursor_loc < _anchor_loc)
			{
				return text_selection(word_to_left(_cursor_loc), word_to_right(_anchor_loc));
			}
			return text_selection(word_to_left(_anchor_loc), word_to_right(_cursor_loc));
		}

		text_selection line_selection(const text_location& pos, const bool from_anchor) const
		{
			auto ptStart = from_anchor ? _anchor_loc : pos;
			auto ptEnd = pos;

			ptEnd.x = 0; //	Force beginning of the line

			if (ptStart.y < static_cast<int>(_lines.size()))
			{
				ptStart.x = static_cast<int>(_lines[ptStart.y].size());
			}
			else
			{
				ptStart.y = static_cast<int>(_lines.size()) - 1;
				ptStart.x = static_cast<int>(_lines[ptStart.y].size());
			}

			return text_selection(ptStart, ptEnd);
		}

		text_selection pos_selection(const text_location& pos, const bool from_anchor) const
		{
			return text_selection(from_anchor ? _anchor_loc : pos, pos);
		}

		void select(const text_selection& selection);

		void invalidate_line(int index);

		[[nodiscard]] bool spell_check() const
		{
			return _spell_check;
		}

		void set_spell_check(bool enabled);
		void toggle_spell_check();

	};

		using text_buffer_ptr = std::shared_ptr<text_buffer>;

	class undo_group
	{
		text_buffer& _doc;
		undo_item _undo;

	public:
		undo_group(const undo_group&) = delete;
		undo_group(undo_group&&) = delete;
		undo_group& operator=(const undo_group&) = delete;
		undo_group& operator=(undo_group&&) = delete;

		undo_group(text_buffer& d) : _doc(d)
		{
		}

		undo_group(const text_buffer_ptr& d) : _doc(*d)
		{
		}

		~undo_group()
		{
			if (!_undo.empty())
				_doc.record_undo(std::move(_undo));
		}

		void insert(const text_location& location, const char& c)
		{
			_undo.add_insert(location, c);
		}

		void insert(const text_selection& selection, const std::string_view text)
		{
			_undo.add_insert(selection, text);
		}

		void erase(const text_location& location, const char& c)
		{
			_undo.add_erase(location, c);
		}

		void erase(const text_selection& selection, const std::string_view text)
		{
			_undo.add_erase(selection, text);
		}
	};

	// --- Highlighting and spell checking utilities (used by views) ---

}
