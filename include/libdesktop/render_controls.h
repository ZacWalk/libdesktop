// This file is part of libdesktop, a C++20 platform layer for desktop applications
// Copyright 2026  Zac Walker
// 
// This program is free software; you can redistribute it and / or modify it
// under the terms of the LGPL License either version 2.1 or later.
// License details are available at https://www.gnu.org/licenses/lgpl-2.1.html
// This program is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY

// Purpose: The controls libdesktop-render draws itself - push, check and radio buttons, command links,
// the slider, the toolbar, the text edit and the date field, each an element of the tree drawn where it
// sits in it - the input manager that gives them a frame's pointer and keyboard, and the drawn menu. A
// window layer forwards its input to its input manager, which finds the controls in its host's tree, so
// one implementation serves every platform. The window-free models the controls edit with are here too,
// and are tested without a window.

#pragma once

#include "libdesktop/ui_elements.h"

#include <cctype>

namespace ui
{
	///////////////////////////////////////////////////////////////////////////////////////////////////
	// Window-free models
	///////////////////////////////////////////////////////////////////////////////////////////////////

	// Text, a caret and a selection, edited a UTF-8 code point at a time. Offsets are bytes and always
	// land on a code point boundary. A single-line model folds the line breaks it is given to spaces; a
	// multi-line one keeps them, as '\n' whatever they arrived as.
	class text_edit_model
	{
		std::string _text;
		std::string _original_text;
		size_t _caret = 0;
		size_t _anchor = 0;
		bool _multi_line = false;

		struct edit_state
		{
			std::string text;
			size_t caret = 0;
			size_t anchor = 0;
		};

		std::vector<edit_state> _undo;
		std::vector<edit_state> _redo;

		static bool is_continuation(const unsigned char c) noexcept
		{
			return (c & 0xc0) == 0x80;
		}

		static bool is_word_byte(const unsigned char c) noexcept
		{
			return c >= 0x80 || std::isalnum(c) != 0 || c == '_';
		}

		void record_undo()
		{
			_undo.push_back({_text, _caret, _anchor});
			if (_undo.size() > 100) _undo.erase(_undo.begin());
			_redo.clear();
		}

		void restore(std::vector<edit_state>& from, std::vector<edit_state>& to)
		{
			if (from.empty()) return;
			to.push_back({_text, _caret, _anchor});
			auto state = std::move(from.back());
			from.pop_back();
			_text = std::move(state.text);
			_caret = state.caret;
			_anchor = state.anchor;
		}

		void erase_selection_impl()
		{
			if (!has_selection()) return;
			const auto start = selection_start();
			_text.erase(start, selection_end() - start);
			_caret = _anchor = start;
		}

		void collapse_or_move(const bool right, const bool extend)
		{
			if (has_selection() && !extend)
			{
				_caret = _anchor = right ? selection_end() : selection_start();
				return;
			}

			_caret = right ? next_boundary(_caret) : previous_boundary(_caret);
			if (!extend) _anchor = _caret;
		}

		std::string normalize(const std::string_view value) const
		{
			std::string result;
			result.reserve(value.size());

			for (size_t i = 0; i < value.size(); ++i)
			{
				const auto c = value[i];

				if (c == '\r' || c == '\n')
				{
					if (_multi_line)
					{
						result.push_back('\n');
						if (c == '\r' && i + 1 < value.size() && value[i + 1] == '\n') ++i;
					}
					else if (result.empty() || result.back() != ' ')
					{
						result.push_back(' ');
					}
				}
				else
				{
					result.push_back(c);
				}
			}

			return result;
		}

	public:
		explicit text_edit_model(const bool multi_line = false) noexcept : _multi_line(multi_line)
		{
		}

		const std::string& text() const noexcept { return _text; }
		bool multi_line() const noexcept { return _multi_line; }
		size_t caret() const noexcept { return _caret; }
		size_t anchor() const noexcept { return _anchor; }
		bool has_selection() const noexcept { return _caret != _anchor; }
		size_t selection_start() const noexcept { return std::min(_caret, _anchor); }
		size_t selection_end() const noexcept { return std::max(_caret, _anchor); }
		bool can_undo() const noexcept { return !_undo.empty(); }

		size_t clamp_boundary(size_t pos) const noexcept
		{
			pos = std::min(pos, _text.size());
			while (pos > 0 && pos < _text.size() && is_continuation(static_cast<unsigned char>(_text[pos]))) --pos;
			return pos;
		}

		size_t previous_boundary(size_t pos) const noexcept
		{
			pos = clamp_boundary(pos);
			if (pos == 0) return 0;
			--pos;
			while (pos > 0 && is_continuation(static_cast<unsigned char>(_text[pos]))) --pos;
			return pos;
		}

		size_t next_boundary(size_t pos) const noexcept
		{
			pos = clamp_boundary(pos);
			if (pos >= _text.size()) return _text.size();
			++pos;
			while (pos < _text.size() && is_continuation(static_cast<unsigned char>(_text[pos]))) ++pos;
			return pos;
		}

		size_t previous_word(size_t pos) const noexcept
		{
			pos = clamp_boundary(pos);
			while (pos > 0 && !is_word_byte(static_cast<unsigned char>(_text[previous_boundary(pos)])))
				pos = previous_boundary(pos);
			while (pos > 0 && is_word_byte(static_cast<unsigned char>(_text[previous_boundary(pos)])))
				pos = previous_boundary(pos);
			return pos;
		}

		size_t next_word(size_t pos) const noexcept
		{
			pos = clamp_boundary(pos);
			while (pos < _text.size() && is_word_byte(static_cast<unsigned char>(_text[pos]))) pos = next_boundary(pos);
			while (pos < _text.size() && !is_word_byte(static_cast<unsigned char>(_text[pos]))) pos =
				next_boundary(pos);
			return pos;
		}

		// The start and end of the paragraph holding pos: the text between two line breaks.
		size_t paragraph_start(size_t pos) const noexcept
		{
			pos = clamp_boundary(pos);
			const auto found = pos == 0 ? std::string::npos : _text.rfind('\n', pos - 1);
			return found == std::string::npos ? 0 : found + 1;
		}

		size_t paragraph_end(size_t pos) const noexcept
		{
			pos = clamp_boundary(pos);
			const auto found = _text.find('\n', pos);
			return found == std::string::npos ? _text.size() : found;
		}

		void text(std::string value)
		{
			_text = _multi_line ? normalize(value) : std::move(value);
			_caret = _anchor = _text.size();
			_undo.clear();
			_redo.clear();
		}

		void begin_edit()
		{
			_original_text = _text;
		}

		void cancel_edit()
		{
			text(_original_text);
		}

		void select(const size_t anchor, const size_t caret)
		{
			_anchor = clamp_boundary(anchor);
			_caret = clamp_boundary(caret);
		}

		void select_all()
		{
			_anchor = 0;
			_caret = _text.size();
		}

		// Moves the caret to a position the view chose - a line above or below - extending the selection
		// when asked to.
		void move_to(const size_t pos, const bool extend = false)
		{
			_caret = clamp_boundary(pos);
			if (!extend) _anchor = _caret;
		}

		void move_left(const bool extend = false) { collapse_or_move(false, extend); }
		void move_right(const bool extend = false) { collapse_or_move(true, extend); }

		void move_word_left(const bool extend = false)
		{
			if (has_selection() && !extend)
			{
				_caret = _anchor = selection_start();
				return;
			}
			_caret = previous_word(_caret);
			if (!extend) _anchor = _caret;
		}

		void move_word_right(const bool extend = false)
		{
			if (has_selection() && !extend)
			{
				_caret = _anchor = selection_end();
				return;
			}
			_caret = next_word(_caret);
			if (!extend) _anchor = _caret;
		}

		void move_home(const bool extend = false)
		{
			_caret = 0;
			if (!extend) _anchor = _caret;
		}

		void move_end(const bool extend = false)
		{
			_caret = _text.size();
			if (!extend) _anchor = _caret;
		}

		void erase_selection()
		{
			if (!has_selection()) return;
			record_undo();
			erase_selection_impl();
		}

		std::string selected_text() const
		{
			return _text.substr(selection_start(), selection_end() - selection_start());
		}

		void select_word(const size_t pos)
		{
			auto start = clamp_boundary(pos);
			if (start == _text.size() && start > 0) start = previous_boundary(start);
			if (start >= _text.size()) return select(start, start);
			const auto word = is_word_byte(static_cast<unsigned char>(_text[start]));
			auto end = next_boundary(start);
			while (start > 0 && is_word_byte(static_cast<unsigned char>(_text[previous_boundary(start)])) == word)
				start = previous_boundary(start);
			while (end < _text.size() && is_word_byte(static_cast<unsigned char>(_text[end])) == word)
				end = next_boundary(end);
			select(start, end);
		}

		void insert(const std::string_view value)
		{
			const auto normalized = normalize(value);
			if (normalized.empty() && !has_selection()) return;
			record_undo();
			erase_selection_impl();
			_text.insert(_caret, normalized);
			_caret += normalized.size();
			_anchor = _caret;
		}

		void backspace()
		{
			if (has_selection())
			{
				record_undo();
				erase_selection_impl();
				return;
			}
			if (_caret == 0) return;
			record_undo();
			const auto previous = previous_boundary(_caret);
			_text.erase(previous, _caret - previous);
			_caret = _anchor = previous;
		}

		bool paste_from_clipboard(const std::function<std::optional<std::string>()>& read_text)
		{
			const auto value = read_text();
			if (!value) return false;
			insert(*value);
			return true;
		}

		bool cut_to_clipboard(const std::function<bool(std::string_view)>& write_text)
		{
			const auto selected = selected_text();
			if (selected.empty()) return false;
			if (!write_text(selected)) return false;
			erase_selection();
			return true;
		}

		void delete_forward()
		{
			if (has_selection())
			{
				record_undo();
				erase_selection_impl();
				return;
			}
			if (_caret >= _text.size()) return;
			record_undo();
			const auto next = next_boundary(_caret);
			_text.erase(_caret, next - _caret);
			_anchor = _caret;
		}

		void backspace_word()
		{
			if (has_selection()) return backspace();
			const auto previous = previous_word(_caret);
			if (previous == _caret) return;
			record_undo();
			_text.erase(previous, _caret - previous);
			_caret = _anchor = previous;
		}

		void delete_word()
		{
			if (has_selection()) return delete_forward();
			const auto next = next_word(_caret);
			if (next == _caret) return;
			record_undo();
			_text.erase(_caret, next - _caret);
			_anchor = _caret;
		}

		void undo() { restore(_undo, _redo); }
		void redo() { restore(_redo, _undo); }
	};

	enum class date_segment : uint8_t
	{
		year,
		month,
		day,
		hour,
		minute,
		second,
	};

	// The order the display locale writes a date in. Only the date part varies: every locale writes
	// a clock most-significant first.
	enum class date_field_order : uint8_t
	{
		ymd,
		dmy,
		mdy,
	};

	// The segmented date field's state. Segment order, typing, stepping and roll-over are arithmetic,
	// tested without a window.
	class date_edit_model
	{
		std::vector<date_segment> _segments;
		df::date_t _value;
		size_t _active = 0;
		// Digits typed into the active segment since it was entered. Typing runs left to right, so a
		// segment holds a partial value until it is full or the field is left.
		int _typed = 0;
		int _typed_digits = 0;

		static constexpr int days_in_month(const int year, const int month) noexcept
		{
			constexpr int lengths[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
			if (month < 1 || month > 12) return 31;
			if (month != 2) return lengths[month - 1];
			const auto leap = (year % 4 == 0 && year % 100 != 0) || year % 400 == 0;
			return leap ? 29 : 28;
		}

		df::day_t parts() const
		{
			return _value.date();
		}

		void assign(df::day_t d)
		{
			// A month or year step can land on a day the new month does not have, and 31 April is
			// not a date. Clamping keeps the segment the user is not editing where they left it.
			d.year = std::clamp<int>(d.year, min_year, max_year);
			d.month = std::clamp<int>(d.month, 1, 12);
			d.day = std::clamp<int>(d.day, 1, days_in_month(d.year, d.month));
			_value = df::date_t(d.year, d.month, d.day, d.hour, d.minute, d.second);
		}

		void step_ticks(const int64_t ticks)
		{
			const auto current = static_cast<int64_t>(_value.to_int64());
			if (ticks < 0 && current + ticks < 0) return;
			const auto stepped = df::date_t(static_cast<uint64_t>(current + ticks));
			const auto d = stepped.date();
			if (d.year < min_year || d.year > max_year) return;
			_value = stepped;
		}

	public:
		// df::date_t counts from 1601, so 1600 is not a date it can hold: on Windows the conversion
		// fails and takes the whole value with it, and on Linux it wraps to a far-future one.
		static constexpr int min_year = 1601;
		static constexpr int max_year = 9999;

		date_edit_model(const date_field_order order, const bool include_time)
		{
			switch (order)
			{
			case date_field_order::ymd:
				_segments = {date_segment::year, date_segment::month, date_segment::day};
				break;
			case date_field_order::mdy:
				_segments = {date_segment::month, date_segment::day, date_segment::year};
				break;
			default:
				_segments = {date_segment::day, date_segment::month, date_segment::year};
				break;
			}

			if (include_time)
			{
				_segments.push_back(date_segment::hour);
				_segments.push_back(date_segment::minute);
				_segments.push_back(date_segment::second);
			}
		}

		const std::vector<date_segment>& segments() const noexcept { return _segments; }
		size_t active() const noexcept { return _active; }
		date_segment active_segment() const noexcept { return _segments[_active]; }
		df::date_t value() const noexcept { return _value; }

		void value(const df::date_t v)
		{
			_value = v;
			commit_typing();
		}

		void active(const size_t index)
		{
			if (index >= _segments.size() || index == _active) return;
			commit_typing();
			_active = index;
		}

		// Left and Right walk the segments and stop at the ends, so the arrow keys stay inside the
		// field. Tab asks the same question and takes a false answer as "leave the control".
		bool move(const int direction)
		{
			if (direction == 0) return false;
			const auto next = static_cast<int>(_active) + (direction > 0 ? 1 : -1);
			if (next < 0 || next >= static_cast<int>(_segments.size())) return false;
			active(static_cast<size_t>(next));
			return true;
		}

		static int digit_count(const date_segment segment) noexcept
		{
			return segment == date_segment::year ? 4 : 2;
		}

		int maximum(const date_segment segment) const
		{
			const auto d = parts();

			switch (segment)
			{
			case date_segment::year: return max_year;
			case date_segment::month: return 12;
			case date_segment::day: return days_in_month(d.year, d.month);
			case date_segment::hour: return 23;
			case date_segment::minute:
			case date_segment::second: return 59;
			}

			return 0;
		}

		static int minimum(const date_segment segment) noexcept
		{
			switch (segment)
			{
			case date_segment::year: return min_year;
			case date_segment::month:
			case date_segment::day: return 1;
			default: return 0;
			}
		}

		int field(const date_segment segment) const
		{
			const auto d = parts();

			switch (segment)
			{
			case date_segment::year: return d.year;
			case date_segment::month: return d.month;
			case date_segment::day: return d.day;
			case date_segment::hour: return d.hour;
			case date_segment::minute: return d.minute;
			case date_segment::second: return d.second;
			}

			return 0;
		}

		void field(const date_segment segment, const int v)
		{
			auto d = parts();

			switch (segment)
			{
			case date_segment::year: d.year = v;
				break;
			case date_segment::month: d.month = v;
				break;
			case date_segment::day: d.day = v;
				break;
			case date_segment::hour: d.hour = v;
				break;
			case date_segment::minute: d.minute = v;
				break;
			case date_segment::second: d.second = v;
				break;
			}

			assign(d);
		}

		// Up, Down and the wheel. One step is one unit of the active segment applied to the whole
		// date, so 23:59:59 rolls into the next day and December rolls into January of the next
		// year - which is what a clock does, and is why this is not a per-field wrap.
		void step(const int direction)
		{
			if (direction == 0) return;
			commit_typing();

			const auto delta = direction > 0 ? 1 : -1;
			auto d = parts();

			switch (active_segment())
			{
			case date_segment::year:
				d.year += delta;
				assign(d);
				return;
			case date_segment::month:
				{
					auto month = d.month + delta;
					while (month > 12)
					{
						month -= 12;
						++d.year;
					}
					while (month < 1)
					{
						month += 12;
						--d.year;
					}
					d.month = month;
					assign(d);
					return;
				}
			case date_segment::day:
				step_ticks(delta * static_cast<int64_t>(df::date_t::intervals_per_day));
				return;
			case date_segment::hour:
				step_ticks(delta * static_cast<int64_t>(df::date_t::intervals_per_second) * 3600);
				return;
			case date_segment::minute:
				step_ticks(delta * static_cast<int64_t>(df::date_t::intervals_per_second) * 60);
				return;
			case date_segment::second:
				step_ticks(delta * static_cast<int64_t>(df::date_t::intervals_per_second));
				return;
			}
		}

		// Digits type left to right and the field advances when the segment can hold no more, so a
		// date is typed straight through without reaching for a separator key.
		bool type_digit(const char c)
		{
			if (c < '0' || c > '9') return false;

			const auto segment = active_segment();
			const auto digit = c - '0';
			const auto width = digit_count(segment);

			auto pending = _typed_digits > 0 ? _typed * 10 + digit : digit;
			auto digits = _typed_digits + 1;

			// A value the segment cannot hold starts the segment again from this digit rather than
			// being refused, which is what a user retyping a wrong month means by it.
			if (pending > maximum(segment))
			{
				pending = digit;
				digits = 1;
			}

			_typed = pending;
			_typed_digits = digits;

			// Applied as it is typed, so the field always shows what it holds. A partial value below
			// the minimum - a lone 0 in a month - is shown but not yet written.
			if (pending >= minimum(segment)) field(segment, pending);

			// Nothing further could be added, so the segment is finished.
			if (digits >= width || pending * 10 > maximum(segment))
			{
				commit_typing();
				move(1);
			}

			return true;
		}

		// What the segment displays, including a partial value that is still being typed.
		std::string text(const size_t index) const
		{
			if (index >= _segments.size()) return {};

			const auto segment = _segments[index];
			const auto width = digit_count(segment);
			const auto v = index == _active && _typed_digits > 0 ? _typed : field(segment);

			auto result = std::to_string(v);
			while (result.size() < static_cast<size_t>(width)) result.insert(result.begin(), '0');
			return result;
		}

		// The month is the one segment with a name, so it is the one segment worth a drop menu:
		// everything else is a number faster to type than to pick.
		bool active_has_menu() const noexcept
		{
			return active_segment() == date_segment::month;
		}

		void commit_typing()
		{
			_typed = 0;
			_typed_digits = 0;
		}
	};

	int slider_value_to_track_x(recti track_bounds, int value, int min_value, int max_value);

	// The value under x on the track. Pressing the thumb where it is drawn keeps current_value.
	int slider_track_x_to_value(recti track_bounds, int x, int min_value, int max_value, int current_value);

	// A word the dictionary does not know, as UTF-8 byte offsets into the text checked.
	struct spelling_range
	{
		std::string word;
		size_t start = 0;
		size_t end = 0;

		bool operator==(const spelling_range&) const = default;
	};

	// The words of text is_word_valid rejects. A word is a run of letters, digits, '_' and '\''.
	std::vector<spelling_range> spelling_ranges(std::string_view text,
	                                            const std::function<bool(std::string_view)>& is_word_valid);

	// What an edit checking against this dictionary underlines; nothing without one.
	std::vector<spelling_range> unknown_words(std::string_view text, const spelling_dictionary* dictionary);

	// A button's label as it is shown, and the character its '&' marks, which a dialog's keyboard
	// presses the button with. A marker comes before a word and not inside one, as the application's
	// labels are written ("&OK", "As &JPEG", "分析(&A)"); "R&B" and "Fitness & Workout" keep their
	// ampersand, and "&&" shows one. The key is an upper-case letter or a digit, as key codes name
	// them; zero when nothing is marked or what is marked has no key code.
	struct mnemonic_label
	{
		std::string text;
		char32_t key = 0;
	};

	mnemonic_label parse_mnemonic(std::string_view label);

	// The face the controls draw an icon in. The caption buttons' glyphs fill more of the icon font's
	// square than the rest of the set, so they are drawn a size down to sit level with the icons beside
	// them.
	style::font_face icon_face(icon_index icon, const chrome_icon_set& chrome = chrome_icons);

	// The hover and press of the toolbar button a frame reports as its maximize caption button.
	// Windows sends that button non-client mouse messages, so it is tracked apart from the pointer.
	// Ids are the toolbar's; zero is none.
	struct caption_button_tracker
	{
		int hover_id = 0;
		int pressed_id = 0;

		// Each answers whether what the toolbar draws changed.
		bool hover(int id);
		bool press(int id);
		bool leave();

		// The id to invoke: a release over the button its press began on, otherwise zero.
		int release(int id);
	};

	///////////////////////////////////////////////////////////////////////////////////////////////////
	// The drawn menu
	///////////////////////////////////////////////////////////////////////////////////////////////////

	class menu_session;

	// What a window layer gives the drawn menu: popups to draw it in, and its message loop.
	class menu_host
	{
	public:
		virtual ~menu_host() = default;

		// Fonts and metrics at the scale the menu is drawn at; null when there is none.
		virtual measure_context* menu_measure() = 0;
		// A popup drawn by content, placed with window_bounds in screen pixels. A submenu's names the popup
		// it opens from, since Wayland nests a menu's popups each in the one before.
		virtual frame_ptr create_menu_popup(const frame_host_weak_ptr& content, const frame_ptr& parent) = 0;
		// The work area of the display nearest bounds, in screen pixels.
		virtual recti menu_work_area(recti bounds) = 0;
		// Runs the message loop until the session is done. Keys go to session.key_down, not to the
		// windows they were meant for; a press outside the session's popups, or the application losing
		// the keyboard, cancels it.
		virtual void run_menu_loop(menu_session& session) = 0;
	};

	// One drawn menu, open until a command is chosen or it is dismissed.
	class menu_session
	{
	public:
		virtual ~menu_session() = default;
		virtual bool done() const = 0;
		virtual void cancel() = 0;
		virtual bool key_down(int key, key_state keys) = 0;
		// The popups open now, deepest last: a press on any other window dismisses the menu.
		virtual std::vector<frame_ptr> popups() const = 0;
	};

	// Shows commands as a drawn menu beside button_bounds - below or above it as owner_bounds leaves
	// room, or at its top left when it is a point - and invokes the command chosen once the menu has
	// closed. Bounds are screen pixels. Returns when the menu has closed.
	void track_drawn_menu(menu_host& host, recti button_bounds, recti owner_bounds,
	                      const std::vector<command_ptr>& commands);

	///////////////////////////////////////////////////////////////////////////////////////////////////
	// The drawn bubble
	///////////////////////////////////////////////////////////////////////////////////////////////////

	// What a window layer gives the drawn bubble: the layered popup it draws in - a frame with
	// frame_style::layered - and what that popup measures with, at the scale its owner draws at now,
	// since a bubble sees no scale change of its own.
	class bubble_host
	{
	public:
		virtual ~bubble_host() = default;
		virtual frame_ptr create_bubble_popup(const frame_host_weak_ptr& content) = 0;
		virtual measure_context* bubble_measure(const frame_ptr& popup) = 0;
		// The work area of the display a bubble beside anchor opens on, in screen pixels.
		virtual recti bubble_work_area(recti anchor) = 0;
	};

	// Where a bubble with content of content_extent goes beside anchor, in screen pixels: above it - or
	// to its left when horizontal - and on the other side when the work area leaves no room. The bounds
	// take the shadow and the padding around the content; focus is the point the bubble's arrow aims at.
	struct bubble_placement
	{
		recti bounds;
		pointi focus;
	};

	bubble_placement place_bubble(sizei content_extent, recti anchor, int x_focus, bool horizontal, recti work);

	// A bubble the module draws in a layered popup of its own, the same on every platform: placed beside
	// what it points at, with its arrow and shadow, faded in and out where animations run.
	bubble_window_ptr create_drawn_bubble(const std::shared_ptr<bubble_host>& host);

	///////////////////////////////////////////////////////////////////////////////////////////////////
	// The drawn controls
	///////////////////////////////////////////////////////////////////////////////////////////////////

	// What a window gives the drawn controls drawn in it. Coordinates are the window's client pixels.
	class control_host
	{
	public:
		virtual ~control_host() = default;

		// Fonts and metrics at the window's scale; null before the window can draw.
		virtual measure_context* measure() = 0;
		virtual void invalidate(recti bounds) = 0;
		virtual bool has_keyboard_focus() const = 0;
		virtual void take_keyboard_focus() = 0;
		virtual void capture_pointer(bool capture) = 0;
		virtual void set_cursor(style::cursor cursor) = 0;
		virtual pointi client_to_screen(pointi loc) const = 0;
		// Shows commands as a menu beside bounds, in screen pixels; returns once it has closed.
		virtual void track_menu(recti screen_bounds, const std::vector<command_ptr>& commands) = 0;
		// A popup that takes no focus, drawn by content: the suggestions an edit drops down.
		virtual frame_ptr create_popup(const frame_host_weak_ptr& content, const color_style& colors) = 0;
		// The window's host: the tree its controls are found in, told of focus moving between them and of
		// the command under the pointer.
		virtual std::shared_ptr<ui::frame_host> listener() const = 0;
		// The keyboard came back to the window itself from the control that held it - put down, disabled or
		// gone - while the window kept it. The window tells its frame host as it does when it gains the
		// keyboard with no control holding it.
		virtual void keyboard_to_frame() {}
		// Whether typing is expected, and where the caret is for an input method's candidates.
		virtual void text_input(bool active, recti caret) = 0;
		// Starts or stops the timer that calls input_manager::blink.
		virtual void caret_timer(bool on, uint32_t interval_ms) = 0;
		// Only a dialog closes on Escape and presses its default button on Return.
		virtual bool is_dialog() const = 0;
		virtual void close(bool cancel) = 0;
		// Whether the toolbar button for a maximize command may answer as the window's caption button.
		virtual bool presents_caption_button() const { return false; }
	};

	class input_manager;
	using input_manager_ptr = std::shared_ptr<input_manager>;

	// A control the module draws: an element of the tree, measured and laid out by flex like any other and
	// drawn by its own render where it sits in the tree, then given the pointer and the keyboard by the
	// input_manager of the frame whose host's tree it is found in. Made apart from any frame, it holds what
	// it is given until a frame finds it. Each kind - button, edit, trackbar, toolbar and date field - has
	// one implementation, private to the module, so what an owner asks of a control is answered directly;
	// the virtual methods here are the behaviour the kinds differ in.
	class control : public element, public std::enable_shared_from_this<control>
	{
	public:
		// What the input manager needs to know of a kind, fixed when it is made.
		struct control_traits
		{
			bool focus_on_click = true;
			// Typing goes to it while it has the focus, which it is given with its text selected.
			bool typing = false;
			bool wants_return = false;
			bool push_button = false;
			bool default_button = false;
			// Native focus_first went to the first enabled edit or button, so the rest are passed over.
			bool first_focus_candidate = false;
			// The radio group, or -1 for a control that is not a radio button.
			int radio_group = -1;
		};

		control_traits traits;
		// Whether it takes input and draws undimmed: enabled itself, and under no disabled element. The
		// frame that finds it keeps ancestors_enabled.
		bool enabled = true;
		bool ancestors_enabled = true;
		bool hovered = false;
		// Set before a press reaches the control: whether this press gave it the focus.
		bool click_took_focus = false;
		// The key its label marks, which a dialog's keyboard presses it with; zero for none.
		char32_t mnemonic = 0;

		// Added to what the control asks for, in logical units as flex lengths are; and the share of the
		// width it is measured in that it takes at least - 5 keeps a button a fifth of its dialog's width
		// however short its name - or 0 for no minimum.
		sizei room;
		int min_share = 0;

		control() noexcept
		{
			kind = element_kind::control;
			padding(0);
		}

		// What an owner asks of any control. Asked for the focus before its frame has found it, a control
		// takes it once found.
		void focus();
		bool has_focus() const;
		// Where it is on screen; where its frame last found or drew it.
		recti window_bounds() const;
		void options_changed();

		// As an element: as large as it asks with room added, at least its share, and drawn by itself where
		// its layout put it.
		sizei measure(measure_context& mc, int width_limit) const final;
		void layout(measure_context& mc, recti bounds_in) final;
		void render(draw_context& dc, pointi element_offset) const final;
		void enabled_changed() override;

		// The behaviour the kinds differ in. Locations are the control's content coordinates, the ones its
		// bounds are in.
		virtual sizei measure_content(measure_context& mc, int width_limit) const = 0;
		virtual void paint(draw_context& dc, bool focused) const = 0;
		virtual bool can_focus() const { return enabled; }
		virtual bool is_checked() const { return false; }
		virtual void check_from_group(bool checked) {}
		virtual void notify_changed() {}

		virtual void on_focus(bool focused, bool select_all) {}
		virtual void on_keyboard(bool has_keyboard) {}
		virtual bool on_key(int key, key_state keys) { return false; }
		virtual bool on_text(std::string_view text) { return false; }
		virtual void on_mouse_down(pointi loc, key_state keys) {}
		virtual void on_mouse_move(pointi loc, bool captured) {}
		virtual void on_mouse_up(pointi loc, key_state keys) {}
		virtual void on_double_click(pointi loc, const key_state keys) { on_mouse_down(loc, keys); }
		virtual void on_mouse_leave() {}
		virtual bool on_wheel(pointi loc, int delta, key_state keys) { return false; }
		virtual std::vector<command_ptr> context_commands(pointi loc) { return {}; }
		virtual style::cursor cursor_at(pointi loc) const { return style::cursor::normal; }
		virtual recti caret_bounds() const { return {}; }
		virtual void blink() {}
		virtual void tick() {}
		virtual void scale_changed() {}
		// Laid out at new bounds, measured by the context the layout was given: a control is laid out
		// before any frame has found it, so it cannot ask its frame.
		virtual void on_placed(measure_context& mc) {}
		virtual void click() {}

		virtual int caption_button_at(pointi loc) const { return 0; }
		virtual bool has_caption_button(int id) const { return false; }
		virtual void invoke_caption(int id) {}

		// The module's plumbing.
		input_manager_ptr input() const { return _input.lock(); }
		control_host* host() const;
		measure_context* metrics() const;
		double scale() const;
		void invalidate() const;
		void invalidate(recti r) const;
		bool is_focused() const;
		void take_focus(bool select_all);
		// Where its content coordinates sit in its frame's client pixels, and on screen.
		pointi origin() const { return _origin; }
		pointi screen_offset() const;

		// Told by the input manager that found it where it is, and whether everything above it is enabled.
		void found(const input_manager_ptr& input, pointi origin, bool ancestors_are_enabled);
		// The frame it was drawn in is going.
		void lost(const input_manager* input);
		// Taken by the input manager that has just found it: whether it asked for the focus first.
		bool take_focus_request();

	private:
		std::weak_ptr<input_manager> _input;
		mutable pointi _origin;
		bool _focus_requested = false;
	};

	// A push, check or radio button, or a command link.
	class button : public control
	{
	public:
		// As given, with its mnemonic marker.
		std::string window_text() const;
		void window_text(std::string_view text);
		void set_checked(bool checked);

	protected:
		button() = default;
	};

	// Text is UTF-8 with '\n' line breaks, and offsets count its bytes. window_text(text) from code puts
	// the caret at the end and, being the owner's own value, is not reported to changed. A negative
	// offset is the end of the text.
	class edit : public control
	{
	public:
		std::string window_text() const;
		void window_text(std::string_view text);
		void select(int start, int end);
		void select_all();
		void set_icon(icon_index icon);
		void set_background(color32 bg);
		void auto_completes(const std::vector<std::string>& texts);

	protected:
		edit() = default;
	};

	class trackbar : public control
	{
	public:
		int get_pos() const;
		void set_pos(int val);

	protected:
		trackbar() = default;
	};

	class toolbar : public control
	{
	public:
		// Its extent with every shown button, measured by mc: a toolbar is measured for the slot it is given
		// before a frame finds it, so it cannot take its text metrics from the frame.
		sizei measure_toolbar(measure_context& mc, int cx);
		void update_button_state(bool resize, bool text_changed);
		// Where a command's button is on screen; empty when it has none.
		recti button_bounds(const command_ptr& command) const;

	protected:
		toolbar() = default;
	};

	class date_time_control : public control
	{
	public:
		// The date as it is shown.
		std::string window_text() const;

	protected:
		date_time_control() = default;
	};

	// The controls, each made apart from any frame.
	edit_ptr make_edit(const edit_styles& styles, std::string_view text, std::function<void(const std::string&)> changed);
	trackbar_ptr make_slider(int min, int max, std::function<void(int, bool)> changed);
	toolbar_ptr make_toolbar(const toolbar_styles& styles, const std::vector<command_ptr>& buttons);
	button_ptr make_button(std::string_view text, std::function<void()> invoke, bool default_button = false);
	button_ptr make_button(icon_index icon, std::string_view title, std::string_view details,
	                       std::function<void()> invoke, bool default_button = false);
	button_ptr make_check_button(bool val, std::string_view text, bool is_radio, std::function<void(bool)> changed,
	                             int radio_group = radio_group_default);
	date_time_control_ptr make_date_time_control(df::date_t val, std::function<void(df::date_t)> changed,
	                                             bool include_time);

	// Calls f(element, offset, visible, ancestors_enabled) for root and everything under it, in the tree's
	// order: offset is where content coordinates sit in the frame, visible whether it and every element
	// above it are, and ancestors_enabled whether every element above it is enabled. Hidden subtrees are
	// skipped unless with_hidden.
	void walk_tree(const element_ptr& root, pointi offset, bool with_hidden,
	               const std::function<void(element&, pointi, bool, bool)>& f);

	// One frame's keyboard and pointer, for the drawn controls in its host's tree: which has the focus,
	// which the pointer is over and which holds it. It owns no controls. It finds them by walking the
	// elements the host offers (frame_host::visit_elements) as it needs them, so Tab follows the tree, a
	// control under a hidden element is passed over and one taken out of the tree is no longer reached.
	// The window forwards its input here before its frame host sees it.
	class input_manager final : public std::enable_shared_from_this<input_manager>
	{
	public:
		explicit input_manager(control_host* host);
		~input_manager();
		input_manager(const input_manager&) = delete;
		input_manager& operator=(const input_manager&) = delete;

		// The window is going; its controls are inert from here, though their owners may still hold them.
		void detach();
		control_host* host() const { return _host; }

		// Where a frame element's frame goes, in the client pixels of the frame that found it.
		struct frame_placement
		{
			frame_ptr frame;
			recti bounds;
			bool visible = false;
		};

		// After the host's layout: finds the controls and frame elements in the tree, tells each control
		// where it is and whether what is above it is enabled, lets go of a focus, hover or capture whose
		// control is no longer shown or has gone, and answers where each frame element's frame goes, for the
		// window to place them together.
		std::vector<frame_placement> layout();
		// Before each paint: finds the controls again, as layout does, so each is drawn as enabled as
		// what is above it in the tree is now, and lets go of a focused control that has gone.
		void refresh();

		// Whether a shown control is at loc.
		bool hit(pointi loc) const;
		// Each answers whether a control took the event, so the window keeps it from its frame host.
		bool mouse_move(pointi loc);
		bool mouse_down(pointi loc, key_state keys);
		bool mouse_up(pointi loc, key_state keys);
		bool mouse_double_click(pointi loc, key_state keys);
		void mouse_leave();
		bool wheel(pointi loc, int delta, key_state keys);
		bool context_menu(pointi loc);
		bool key_down(int key, key_state keys);
		bool text_input(std::string_view text);
		std::optional<style::cursor> cursor(pointi loc) const;

		// The window gained or lost the keyboard. True when a control holds the focus and has told the
		// window's frame host so, which then needs no word of its own. A window that loses the keyboard
		// keeps its control, which has it back when the window does, as a dialog box's does.
		bool keyboard_focus(bool has_focus);
		// No control holds the focus. For a window whose keyboard went to another of the application's
		// windows: the control lost it then, as a native control did, and is not given it back. A window that
		// keeps the keyboard has it itself.
		void clear_focus();
		void blink();
		void tick();
		void scale_changed();

		bool focus_first();
		bool focus(const control_ptr& c);
		control_ptr focused() const;
		// A control that takes typing has the focus, so the keys it edits with are its own.
		bool is_typing() const;
		bool wants_return() const;
		bool is_tracking() const;
		bool has_keyboard_focus() const { return _keyboard_focus; }

		// The toolbar button a frame reports as its maximize caption button, by id, or zero.
		int caption_button_at(pointi loc) const;
		void caption_hover(int id);
		void caption_press(int id);
		void caption_release(int id);
		void caption_leave();
		const caption_button_tracker& caption() const { return _caption; }

		// The controls' plumbing; not for an application.
		void invalidate(recti bounds) const;
		void focus(control* c, bool select_all);
		bool has_focus(const control& c) const;
		void pressed_radio(const control& c);
		void track_menu(recti screen_bounds, const std::vector<command_ptr>& commands);
		void caret_moved(const control& c);

	private:
		control_host* _host = nullptr;
		std::weak_ptr<control> _focused;
		std::weak_ptr<control> _hover;
		std::weak_ptr<control> _captured;
		bool _keyboard_focus = false;
		bool _caret_timer = false;
		caption_button_tracker _caption;

		// The controls in the tree, in its order, each found and told where it is: those shown, or every one.
		std::vector<control_ptr> find_controls(bool with_hidden) const;
		control_ptr control_at(pointi loc) const;
		void set_hover(const control_ptr& c);
		void release_capture();
		// A focused control that has gone - its owner let go of it - lets go of the focus with it.
		void release_gone_focus();
		bool move_focus(bool forward);
		void update_text_input();
	};

	// Where a word's underline is drawn in an edit, in its frame's client pixels; empty when the
	// range is not laid out. For the tests that pin the underline to the characters it marks.
	recti probe_edit_range_bounds(const edit_ptr& edit, size_t start, size_t end);
	// The commands an edit's context menu holds for a press at loc, in client pixels.
	std::vector<command_ptr> probe_edit_context_commands(const edit_ptr& edit, pointi loc);
}
