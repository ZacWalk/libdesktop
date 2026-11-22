// This file is part of libdesktop, a C++20 platform layer for desktop applications
// Copyright 2026  Zac Walker
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

// Purpose: The band of headings over a table's rows, whoever keeps the rows: how it is drawn, the order a click on a
// heading leaves the rows in, and the controllers a heading and a divider answer the pointer with. Element tables and
// an application's own row lists share it, so every headed table looks and answers alike.

#pragma once

#include "libdesktop/ui/elements.h"
#include "libdesktop/ui/text/table_layout.h"

#include <functional>
#include <memory>
#include <span>
#include <type_traits>
#include <utility>

namespace ui
{
	// A column's heading. A table with headings draws them in a band over its rows: a click on one sorts the rows
	// by its column, and the divider after one resizes its column. A table without headings draws neither and
	// fits its columns to what they hold, so its first row never reads as a heading.
	struct table_heading
	{
		std::string title;
		bool right_align = false;
		bool sortable = true;
		// The order a first click sorts in: text from A, a size or a date often largest or latest first.
		bool default_ascending = true;
		// Centred over its column, where the column's values are centred under it.
		bool centred = false;
	};

	// What orders a row in one column: a number, or text compared as people read it - regardless of case, and
	// with the numbers in it by value. A row with no key for the column follows those that have one.
	class table_sort_key
	{
		std::string _text;
		double _number = 0.0;
		uint8_t _kind = 0;

	public:
		table_sort_key() = default;

		template <typename T>
			requires std::is_arithmetic_v<T>
		table_sort_key(const T number) : _number(static_cast<double>(number)), _kind(1)
		{
		}

		table_sort_key(const std::string_view text) : _text(text), _kind(2) {}

		table_sort_key(const char* text) : table_sort_key(std::string_view(text)) {}

		[[nodiscard]] bool empty() const { return _kind == 0; }

		// Numbers before text, so a column that mixes them keeps each kind together.
		[[nodiscard]] int compare(const table_sort_key& other) const
		{
			if (_kind != other._kind) return _kind < other._kind ? -1 : 1;
			if (_kind == 1) return _number < other._number ? -1 : (other._number < _number ? 1 : 0);
			if (_kind == 2) return core::icmp_natural(_text, other._text);
			return 0;
		}
	};

	// What a table's rows are sorted by: a column and which way, or, for column -1, the rows' own order.
	struct table_sort
	{
		int column = -1;
		bool ascending = true;

		[[nodiscard]] bool operator==(const table_sort&) const = default;
	};

	// The sort a click on a heading leaves: a first click sorts by its column the column's own way, a second the other
	// way, and a third gives the rows back their own order.
	[[nodiscard]] constexpr table_sort next_table_sort(const table_sort current, const int column,
	                                                   const bool first_ascending)
	{
		if (current.column != column) return {column, first_ascending};
		if (current.ascending == first_ascending) return {column, !first_ascending};
		return {};
	}

	// Whether one row comes before another by their keys for the sorted column. Rows without a key follow those
	// with one either way, and two that tie answer false, so a stable sort keeps them in the order they were in.
	[[nodiscard]] inline bool table_sorts_before(const table_sort_key* const left, const table_sort_key* const right,
	                                             const bool ascending)
	{
		const auto* const l = left && !left->empty() ? left : nullptr;
		const auto* const r = right && !right->empty() ? right : nullptr;
		if (!l || !r) return l && !r;
		const auto c = l->compare(*r);
		return ascending ? c < 0 : c > 0;
	}

	// A heading as its band places it: the cell a press finds and shades, and the room its title is drawn in.
	struct table_heading_cell
	{
		core::recti bounds;
		core::recti label;
	};

	// Draws a band of headings: shaded; each title in its cell, bright with a chevron over the column the rows are
	// sorted by and dimmed over the rest; the pressed cell shaded; and a faint line at each divider, plain while it
	// is dragged. The owner places the band, the cells and the dividers in the coordinates it draws in, since the
	// rows they head are its own, and draws the band opaque first where rows scroll under it.
	inline void draw_table_headings(render::draw_context& dc, const core::recti band,
	                                const std::span<const table_heading> headings,
	                                const std::span<const table_heading_cell> cells,
	                                const std::span<const core::recti> dividers, const table_sort sort,
	                                const int pressed, const int active_divider)
	{
		if (band.is_empty()) return;
		dc.draw_rounded_rect(band, core::color(0x000000, dc.colors.alpha * 0.2f), dc.padding1);

		const auto count = std::min(headings.size(), cells.size());

		for (size_t i = 0; i < count; ++i)
		{
			const auto& cell = cells[i];
			if (cell.bounds.is_empty()) continue;

			const auto column = static_cast<int>(i);
			if (column == pressed)
				dc.draw_rect(cell.bounds, core::color(dc.colors.foreground, dc.colors.alpha * 0.12f));

			const auto& h = headings[i];
			const auto sorted = column == sort.column;
			const auto color = core::color(dc.colors.foreground, dc.colors.alpha * (sorted ? 1.0f : 0.7f));
			const auto style = h.right_align ? render::text_style::single_line_far
			                   : h.centred   ? render::text_style::single_line_center
			                                 : render::text_style::single_line;
			if (!h.title.empty()) dc.draw_text(h.title, cell.label, render::font_face::dialog, style, color, {});
			if (sorted) table_layout::draw_sort_mark(dc, cell.bounds, sort.ascending, color);
		}

		// Faint, so the columns read as resizable, and plain while one is dragged.
		const auto line = std::max(1, core::round(dc.scale_factor));

		for (size_t i = 0; i < dividers.size(); ++i)
		{
			const auto& divider = dividers[i];
			if (divider.height() <= 4) continue;

			const auto active = static_cast<int>(i) == active_divider;
			const auto width = active ? line * 2 : line;
			const auto x = divider.center().x - width / 2;
			dc.draw_rect({x, divider.top + 2, x + width, divider.bottom - 2},
			             core::color(dc.colors.foreground, dc.colors.alpha * (active ? 0.9f : 0.3f)));
		}
	}

	// A click on a heading: a press shades it, and a release over it clicks it unless Escape took the press back.
	class table_heading_controller final : public controller
	{
		std::function<void(bool)> _pressed;
		std::function<void()> _click;
		bool _tracking = false;

	public:
		table_heading_controller(const element_host_ptr& host, const core::recti bounds,
		                         std::function<void(bool)> pressed, std::function<void()> click) :
			controller(host, bounds), _pressed(std::move(pressed)), _click(std::move(click))
		{
		}

		~table_heading_controller() override { escape(); }

		platform::cursor cursor() const override { return platform::cursor::link; }

		void on_mouse_left_button_down(core::pointi, platform::key_state) override
		{
			_tracking = true;
			if (_pressed) _pressed(true);
			_host->invalidate(host_invalid::redraw);
		}

		void on_mouse_left_button_up(const core::pointi point, platform::key_state) override
		{
			const auto clicked = _tracking && _bounds.contains(point);
			escape();
			if (clicked && _click) _click();
		}

		bool escape() override
		{
			if (!_tracking) return false;
			_tracking = false;
			if (_pressed) _pressed(false);
			_host->invalidate(host_invalid::redraw);
			return true;
		}
	};

	// What a drag on a divider does to the columns it sizes, which only the owner of the columns knows. Begin
	// answers whether there is a column to size there; cancel whether there was a drag to take back.
	struct table_divider_actions
	{
		std::function<void(bool)> active;
		std::function<bool(int)> begin;
		std::function<void(int)> move;
		std::function<void()> finish;
		std::function<bool()> cancel;
		std::function<void()> fit;
	};

	// A drag on a divider: it sizes a column as the pointer moves, Escape takes it back, and a double-click fits the
	// column to what it holds again. A drag still held when the controller goes - the pointer taken elsewhere - is
	// taken back too.
	class table_divider_controller final : public controller
	{
		table_divider_actions _actions;
		bool _dragging = false;

	public:
		table_divider_controller(const element_host_ptr& host, const core::recti bounds,
		                         table_divider_actions actions) : controller(host, bounds), _actions(std::move(actions))
		{
			if (_actions.active) _actions.active(true);
			_host->invalidate(host_invalid::redraw);
		}

		~table_divider_controller() override
		{
			escape();
			if (_actions.active) _actions.active(false);
			_host->invalidate(host_invalid::redraw);
		}

		platform::cursor cursor() const override { return platform::cursor::left_right; }

		void on_mouse_left_button_down(const core::pointi point, platform::key_state) override
		{
			_dragging = _actions.begin && _actions.begin(point.x);
		}

		void on_mouse_move(const core::pointi point) override
		{
			if (_dragging && _actions.move) _actions.move(point.x);
		}

		void on_mouse_left_button_up(const core::pointi point, platform::key_state) override
		{
			on_mouse_move(point);
			if (!std::exchange(_dragging, false)) return;
			if (_actions.finish) _actions.finish();
		}

		void on_mouse_left_button_double_click(core::pointi, platform::key_state) override
		{
			if (std::exchange(_dragging, false) && _actions.finish) _actions.finish();
			if (_actions.fit) _actions.fit();
		}

		bool escape() override
		{
			if (!std::exchange(_dragging, false)) return false;
			return !_actions.cancel || _actions.cancel();
		}
	};

	// The actions of a divider over table_layout's columns: the columns before it stay put and the last makes the
	// room, Escape gives every column back the width it had, and a double-click fits the column to what it holds.
	// Changed is told after each change. The columns must outlive the controller, which an owner whose changed
	// holds it alive sees to.
	[[nodiscard]] inline table_divider_actions column_divider_actions(table_layout::columns& columns, const int column,
	                                                                  const std::function<void()>& changed,
	                                                                  std::function<void(bool)> active = {})
	{
		const auto drag = std::make_shared<table_layout::resize_gesture>();
		auto* const layout = &columns;

		table_divider_actions actions;
		actions.active = std::move(active);
		actions.begin = [drag, layout, column](const int x)
		{
			return drag->begin(*layout, column, x);
		};
		actions.move = [drag, layout, changed](const int x)
		{
			if (drag->update(*layout, x) && changed) changed();
		};
		actions.finish = [drag]
		{
			drag->finish();
		};
		actions.cancel = [drag, layout, changed]
		{
			if (!drag->cancel(*layout)) return false;
			if (changed) changed();
			return true;
		};
		actions.fit = [layout, column, changed]
		{
			if (layout->fit(column) && changed) changed();
		};
		return actions;
	}
}
