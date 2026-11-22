// This file is part of libdesktop, a C++20 platform layer for desktop applications
// Copyright 2026  Zac Walker
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

// Purpose: Which entries of a list are selected, which one the keyboard is on and where a range starts from, by the
// one set of rules every list that selects follows - a click selects one, Ctrl toggles one, Shift takes the range
// from the anchor and Ctrl with Shift adds it, and the keys move the cursor alike - and when a press becomes a drag
// and how fast a drag held at an end carries the list along, so selecting and dragging feel the same wherever they
// are done.

#pragma once

#include "libdesktop/core/geometry.h"
#include "libdesktop/core/primitives.h"
#include "libdesktop/platform/input.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <iterator>
#include <optional>
#include <set>
#include <span>
#include <vector>

namespace ui
{
	// Where the keys move a cursor along a list of count entries laid out in a line, page entries showing at a time:
	// one step for the arrows that run along it - Up and Down down a column, Left and Right along a row - a page for
	// Page Up and Page Down, and either end for Home and End. From no entry (at -1) any of them starts at the first,
	// End at the last. Answers -1 for any other key, and for an empty list.
	[[nodiscard]] inline int step_list_cursor(const int at, const int count, const char32_t key, const int page,
	                                          const bool horizontal)
	{
		if (count <= 0) return -1;

		const auto back = horizontal ? platform::keys::LEFT : platform::keys::UP;
		const auto on = horizontal ? platform::keys::RIGHT : platform::keys::DOWN;
		const auto jump = std::max(1, page);
		int to;

		if (key == platform::keys::HOME) to = 0;
		else if (key == platform::keys::END) to = count - 1;
		else if (key != back && key != on && key != platform::keys::PRIOR && key != platform::keys::NEXT) return -1;
		else if (at < 0) to = 0;
		else if (key == back) to = at - 1;
		else if (key == on) to = at + 1;
		else if (key == platform::keys::PRIOR) to = at - jump;
		else to = at + jump;

		return std::clamp(to, 0, count - 1);
	}

	// How far a pressed pointer moves before the press is a drag - a rubber band, a move - rather than a click with a
	// shaky hand: the same few pixels whichever way it goes, so every list answers a drag alike.
	constexpr int drag_distance = 4;

	[[nodiscard]] inline bool is_drag(const core::pointi start, const core::pointi loc)
	{
		return std::abs(loc.x - start.x) >= drag_distance || std::abs(loc.y - start.y) >= drag_distance;
	}

	// Carries a list along while a drag - a rubber band, a move - is held near either end of it, so what lies beyond
	// can be reached, at one pace everywhere: from nothing at the inner edge of a zone at each end up to half the
	// list's length a second at its very end, and as fast past it. Timed rather than counted, so the pace does not
	// hang on how often the pointer reports or the window ticks; and the part of a pixel a step falls short by is
	// carried to the next, or slow steps would each round to nothing.
	class edge_scroll
	{
		bool _running = false;
		uint64_t _last = 0;
		double _carry = 0.0;

	public:
		// The zone at each end: 48 pixels at 100% scale, but no more than a quarter of the list.
		[[nodiscard]] static int zone(const int length, const double scale)
		{
			return std::max(1, std::min(static_cast<int>(std::lround(48.0 * scale)), length / 4));
		}

		// How far to scroll a list running from start to end along the drag, for a pointer at `at` along it and the
		// time `now` in milliseconds: less than 0 back towards start, more on towards end. Out of both zones it is 0,
		// and the clock stops, so the first step back in a zone starts it again rather than leaping.
		[[nodiscard]] int step(const int at, const int start, const int end, const double scale, const uint64_t now)
		{
			const auto length = std::max(1, end - start);
			const auto depth_zone = zone(length, scale);
			const auto into_start = start + depth_zone - at;
			const auto into_end = at - (end - depth_zone);
			const auto depth = into_start > 0 ? -std::min(into_start, depth_zone)
			                   : into_end > 0 ? std::min(into_end, depth_zone)
			                                  : 0;

			if (depth == 0)
			{
				reset();
				return 0;
			}

			// Bounded, so a stall between two steps does not throw the list to its end.
			const auto elapsed = _running ? std::min(0.25, static_cast<double>(now - _last) / 1000.0) : 0.0;
			_running = true;
			_last = now;

			const auto travel = length / 2.0 * depth / depth_zone * elapsed + _carry;
			const auto step = static_cast<int>(travel);
			_carry = travel - step;
			return step;
		}

		[[nodiscard]] int step(const int at, const int start, const int end, const double scale)
		{
			return step(at, start, end, scale, core::tick_count64());
		}

		void reset()
		{
			_running = false;
			_last = 0;
			_carry = 0.0;
		}
	};

	// Entries are named by a key that survives the list being sorted - a pointer to a row, an id - and a range is
	// taken in the order the list shows its entries, which each call that needs one is given.
	template <typename Key> class list_selection
	{
		std::set<Key> _selected;
		std::optional<Key> _cursor;
		std::optional<Key> _anchor;

		[[nodiscard]] static ptrdiff_t index_of(const std::span<const Key> order, const Key& key)
		{
			const auto found = std::ranges::find(order, key);
			return found == order.end() ? -1 : std::distance(order.begin(), found);
		}

		// From the anchor, or with none from the cursor, to the entry given.
		void select_range(const std::span<const Key> order, const Key& to, const bool add)
		{
			if (!add) _selected.clear();

			const auto& from = _anchor ? _anchor : _cursor;
			const auto to_at = index_of(order, to);
			if (to_at < 0) return;

			const auto from_at = from ? index_of(order, *from) : -1;
			const auto first = from_at < 0 ? to_at : std::min(from_at, to_at);
			const auto last = from_at < 0 ? to_at : std::max(from_at, to_at);
			for (auto i = first; i <= last; ++i) _selected.insert(order[static_cast<size_t>(i)]);
			if (!_anchor) _anchor = from_at < 0 ? to : *from;
		}

	public:
		[[nodiscard]] bool empty() const { return _selected.empty(); }
		[[nodiscard]] size_t size() const { return _selected.size(); }
		[[nodiscard]] bool is_selected(const Key& key) const { return _selected.contains(key); }
		// Every key selected, in key order rather than the list's: for a list that keeps its entries under keys that
		// change, to name them anew.
		[[nodiscard]] const std::set<Key>& selected() const { return _selected; }
		[[nodiscard]] const std::optional<Key>& cursor() const { return _cursor; }
		[[nodiscard]] const std::optional<Key>& anchor() const { return _anchor; }

		void clear()
		{
			_selected.clear();
			_cursor.reset();
			_anchor.reset();
		}

		// A click on an entry: alone it selects that one alone; with Ctrl it toggles it; with Shift it selects the
		// range from the anchor to it, and with Ctrl too adds that range. A click without Shift moves the anchor.
		void click(const Key& key, const bool control, const bool shift, const std::span<const Key> order)
		{
			if (shift)
			{
				select_range(order, key, control);
			}
			else if (control)
			{
				if (!_selected.erase(key)) _selected.insert(key);
				_anchor = key;
			}
			else
			{
				_selected = {key};
				_anchor = key;
			}

			_cursor = key;
		}

		// The keys moving the cursor to an entry: alone they select that one alone, with Shift the range from the
		// anchor, and with Ctrl they move the cursor and select nothing more.
		void move_to(const Key& key, const bool control, const bool shift, const std::span<const Key> order)
		{
			if (shift)
			{
				select_range(order, key, control);
			}
			else if (!control)
			{
				_selected = {key};
				_anchor = key;
			}

			_cursor = key;
		}

		// Ctrl+Space: the entry the cursor is on joins the selection, or leaves it.
		void toggle_cursor()
		{
			if (!_cursor) return;
			if (!_selected.erase(*_cursor)) _selected.insert(*_cursor);
			_anchor = _cursor;
		}

		// How entries taken together change the selection.
		enum class choice
		{
			replace,
			add,
			toggle,
		};

		// Entries taken together - those a rubber band touches, those under a heading, those a command picks: they
		// become the selection, join it, or each goes in or out of it, and the cursor and the anchor move to the first
		// of them, as they do in Items. Taking none changes nothing.
		void choose(const std::span<const Key> keys, const choice how)
		{
			if (keys.empty()) return;
			if (how == choice::replace) _selected.clear();

			for (const auto& key : keys)
			{
				if (how != choice::toggle || !_selected.erase(key)) _selected.insert(key);
			}

			_cursor = _anchor = keys.front();
		}

		// An entry known by a new key - saved under another name, or folded into the entry that stands for it: what
		// held of the old key holds of the new.
		void rename(const Key& from, const Key& to)
		{
			if (_selected.erase(from)) _selected.insert(to);
			if (_cursor == from) _cursor = to;
			if (_anchor == from) _anchor = to;
		}

		void select_all(const std::span<const Key> order)
		{
			_selected = std::set<Key>(order.begin(), order.end());
			if (!_cursor && !order.empty()) _cursor = order.front();
			if (!_anchor) _anchor = _cursor;
		}

		// Lets go of what the list no longer shows, once it filters or rebuilds its entries.
		void retain(const std::span<const Key> order)
		{
			const std::set<Key> shown(order.begin(), order.end());
			std::erase_if(_selected, [&shown](const Key& key) { return !shown.contains(key); });
			if (_cursor && !shown.contains(*_cursor)) _cursor.reset();
			if (_anchor && !shown.contains(*_anchor)) _anchor.reset();
		}

		// The selected entries, in the order the list shows them.
		[[nodiscard]] std::vector<Key> selected_in(const std::span<const Key> order) const
		{
			std::vector<Key> result;
			for (const auto& key : order)
			{
				if (_selected.contains(key)) result.push_back(key);
			}
			return result;
		}
	};
}
