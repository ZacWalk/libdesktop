// This file is part of libdesktop, a C++20 platform layer for desktop applications
// Copyright 2026  Zac Walker
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

// Purpose: An element table with shared column sizing, optional headings that sort its rows and resize its
// columns, and normal tree-owned cells.

#pragma once

#include "libdesktop/ui/elements.h"
#include "libdesktop/ui/text/table_layout.h"

#include <array>
#include <numeric>
#include <type_traits>

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

	class table_element final : public element, public std::enable_shared_from_this<table_element>
	{
		struct cell
		{
			element_ptr element;
			core::pointi offset;
			core::sizei extent;
		};
		struct row
		{
			std::vector<cell> cells;
			std::vector<table_sort_key> keys;
			size_t order = 0;
			int y = 0;
			int height = 0;
		};
		mutable std::vector<row> _rows;
		mutable table_layout::columns _columns;
		mutable int _total_height = 0;
		mutable int _heading_height = 0;
		mutable size_t _added = 0;
		mutable bool _unsorted = false;
		std::vector<table_heading> _headings;
		int _sort_column = -1;
		bool _sort_ascending = true;
		int _pressed_heading = -1;
		int _active_divider = -1;
		scroller _scroller;
		double _scale = 1.0;
		std::weak_ptr<element_host> _host;

		static element_ptr make_cell(const element_ptr& value) { return value; }
		static element_ptr make_cell(const std::string_view value) { return std::make_shared<text_element>(value); }
		static core::sizei cell_porch(const element_ptr& e, const render::measure_context& mc)
		{
			auto porch = e ? e->porch() * mc.scale_factor : core::sizei{};
			porch.cx = std::max(porch.cx, core::round(4 * mc.scale_factor));
			return porch;
		}
		static int heading_porch(const render::measure_context& mc) { return core::round(4 * mc.scale_factor); }

		// Stable, so rows that tie keep the order they were added in, and keyless rows stay last both ways.
		void sort_rows() const
		{
			_unsorted = false;
			const auto column = _sort_column;
			const auto ascending = _sort_ascending;
			std::ranges::stable_sort(_rows, [column, ascending](const row& l, const row& r)
			{
				if (column < 0) return l.order < r.order;
				const auto* const lk = column < static_cast<int>(l.keys.size()) && !l.keys[column].empty()
				                           ? &l.keys[column]
				                           : nullptr;
				const auto* const rk = column < static_cast<int>(r.keys.size()) && !r.keys[column].empty()
				                           ? &r.keys[column]
				                           : nullptr;
				if (!lk || !rk) return lk && !rk;
				const auto c = lk->compare(*rk);
				return ascending ? c < 0 : c > 0;
			});
		}

		core::sizei measure_cells(render::measure_context& mc, const int width, const bool fill) const
		{
			if (_unsorted) sort_rows();

			std::vector<int> natural(_headings.size(), 0);
			const auto porch = heading_porch(mc);
			_heading_height = 0;

			for (size_t i = 0; i < _headings.size(); ++i)
			{
				const auto extent = mc.measure_text(_headings[i].title, render::font_face::dialog,
				                                    render::text_style::single_line, std::max(0, width), 0);
				natural[i] = extent.cx + porch * 2;
				_heading_height = std::max(_heading_height, extent.cy + porch * 2);
			}

			for (const auto& r : _rows)
			{
				natural.resize(std::max(natural.size(), r.cells.size()), 0);
				for (size_t i = 0; i < r.cells.size(); ++i)
				{
					const auto& e = r.cells[i].element;
					if (!e || !e->is_visible()) continue;
					const auto cell_porch_size = cell_porch(e, mc);
					natural[i] = std::max(natural[i], e->measure(mc, std::max(0, width)).cx + cell_porch_size.cx * 2);
				}
			}
			std::vector<uint8_t> fixed(natural.size());
			for (size_t i = 0; i < fixed.size() && i < no_shrink_col.size(); ++i) fixed[i] = no_shrink_col[i];
			_columns.layout(natural, width, std::max(1, core::round(24 * mc.scale_factor)), mc.scale_factor, fill,
			                fixed);
			_total_height = _heading_height;
			for (auto& r : _rows)
			{
				r.y = _total_height;
				r.height = 0;
				auto x = 0;
				for (size_t i = 0; i < r.cells.size(); ++i)
				{
					auto& c = r.cells[i];
					auto cp = cell_porch(c.element, mc);
					cp.cx = std::min(cp.cx, _columns.widths()[i] / 2);
					const auto cell_width = std::max(0, _columns.widths()[i] - cp.cx * 2);
					c.extent = c.element && c.element->is_visible() ? c.element->measure(mc, cell_width)
					                                                : core::sizei{};
					c.extent.cx = cell_width;
					c.offset = {x + cp.cx, _total_height + cp.cy};
					r.height = std::max(r.height, c.extent.cy + cp.cy * 2);
					x += _columns.widths()[i];
				}
				for (auto& c : r.cells)
				{
					const auto cp = cell_porch(c.element, mc);
					c.offset.y += std::max(0, (r.height - c.extent.cy - cp.cy * 2) / 2);
				}
				_total_height += r.height;
			}
			return {std::accumulate(_columns.widths().begin(), _columns.widths().end(), 0), _total_height};
		}

		void changed(const element_host_ptr& host) { host->invalidate(host_invalid::layout | host_invalid::redraw); }

		void user_changed(const element_host_ptr& host)
		{
			changed(host);
			if (on_user_change) on_user_change(*this);
		}

		class divider_controller final : public controller
		{
			std::shared_ptr<table_element> _table;
			int _column;
			table_layout::resize_gesture _drag;

		public:
			divider_controller(const element_host_ptr& host, const core::recti bounds,
			                   std::shared_ptr<table_element> table, const int column) :
				controller(host, bounds), _table(std::move(table)), _column(column)
			{
				_table->_active_divider = _column;
				_host->invalidate(host_invalid::redraw);
			}
			~divider_controller() override
			{
				if (_drag.active()) escape();
				_table->_active_divider = -1;
				_host->invalidate(host_invalid::redraw);
			}
			platform::cursor cursor() const override { return platform::cursor::left_right; }
			void on_mouse_left_button_down(const core::pointi point, platform::key_state) override
			{
				_drag.begin(_table->_columns, _column, point.x);
			}
			void on_mouse_move(const core::pointi point) override
			{
				if (_drag.update(_table->_columns, point.x)) _table->user_changed(_host);
			}
			void on_mouse_left_button_up(const core::pointi point, platform::key_state) override
			{
				on_mouse_move(point);
				_drag.finish();
			}
			void on_mouse_left_button_double_click(core::pointi, platform::key_state) override
			{
				_drag.finish();
				if (_table->_columns.fit(_column)) _table->user_changed(_host);
			}
			bool escape() override
			{
				if (!_drag.cancel(_table->_columns)) return false;
				_table->user_changed(_host);
				return true;
			}
		};

		// A click sorts; a press taken back by Escape, or released off the heading, does not.
		class heading_controller final : public controller
		{
			std::shared_ptr<table_element> _table;
			int _column;
			bool _tracking = false;

		public:
			heading_controller(const element_host_ptr& host, const core::recti bounds,
			                   std::shared_ptr<table_element> table, const int column) :
				controller(host, bounds), _table(std::move(table)), _column(column)
			{
			}
			~heading_controller() override { escape(); }
			platform::cursor cursor() const override { return platform::cursor::link; }
			void on_mouse_left_button_down(core::pointi, platform::key_state) override
			{
				_tracking = true;
				_table->_pressed_heading = _column;
				_host->invalidate(host_invalid::redraw);
			}
			void on_mouse_left_button_up(const core::pointi point, platform::key_state) override
			{
				const auto clicked = _tracking && _bounds.contains(point);
				escape();
				if (clicked) _table->click_heading(_column, _host);
			}
			bool escape() override
			{
				if (!_tracking) return false;
				_tracking = false;
				_table->_pressed_heading = -1;
				_host->invalidate(host_invalid::redraw);
				return true;
			}
		};

		void click_heading(const int column, const element_host_ptr& host)
		{
			if (column < 0 || column >= static_cast<int>(_headings.size()) || !_headings[column].sortable) return;
			sort(column, column == _sort_column ? !_sort_ascending : _headings[column].default_ascending);
			user_changed(host);
		}

	public:
		static constexpr int max_cols = 8;
		std::array<bool, max_cols> no_shrink_col{};
		bool can_scroll = false;
		bool frame_when_empty = false;
		bool resizable = true;

		// Told after the user sorts the rows or resizes a column, so an owner that rebuilds the table can give its
		// replacement the same sizes and order.
		std::function<void(const table_element&)> on_user_change;

		explicit table_element(const element_options& options = {}) :
			element(options | element_style::has_tooltip | element_style::can_invoke)
		{
			_scroller._moves_pixels = false;
			_scroller._scroll_child_controls = true;
		}

		// Clearing keeps the headings, the sort and the user's sizes, so refreshed rows arrive in the order
		// and the widths the user chose.
		void clear() const { _rows.clear(); }
		void reverse() const { std::ranges::reverse(_rows); }

		void headings(std::vector<table_heading> headings)
		{
			_headings = std::move(headings);
			if (_sort_column >= static_cast<int>(_headings.size())) sort(-1, true);
		}
		[[nodiscard]] const std::vector<table_heading>& headings() const { return _headings; }

		void add(const std::vector<element_ptr>& elements) const { add_row(elements, {}); }
		// The keys order the row when the heading over each is clicked; a row added without them keeps its place
		// among rows that tie.
		void add_row(const std::vector<element_ptr>& elements, std::vector<table_sort_key> keys) const
		{
			row r;
			for (const auto& e : elements) r.cells.push_back({e});
			r.keys = std::move(keys);
			r.order = _added++;
			_rows.push_back(std::move(r));
			_unsorted = _unsorted || _sort_column >= 0;
		}
		template <typename... Cells>
			requires((std::is_convertible_v<const Cells&, element_ptr> ||
		              std::is_convertible_v<const Cells&, std::string_view>) &&
		             ...)
		void add(const Cells&... cells) const
		{
			add(std::vector<element_ptr>{make_cell(cells)...});
		}
		void add(icon_index, const std::string_view label, const std::string_view value) const { add(label, value); }
		void add(const std::string_view label, const core::color32 color, const std::string_view value,
		         const std::string_view extra) const
		{
			auto text = std::make_shared<text_element>(label);
			if (color) text->foreground_color(color);
			add(std::vector<element_ptr>{text, make_cell(value), make_cell(extra)});
		}

		// Column -1 returns the rows to the order they were added in.
		void sort(const int column, const bool ascending)
		{
			_sort_column = column >= 0 && column < static_cast<int>(_headings.size()) ? column : -1;
			_sort_ascending = ascending;
			_unsorted = true;
			if (const auto host = _host.lock()) changed(host);
		}
		[[nodiscard]] int sort_column() const { return _sort_column; }
		[[nodiscard]] bool sort_ascending() const { return _sort_ascending; }

		[[nodiscard]] const std::vector<int>& column_widths() const { return _columns.widths(); }
		// Logical sizes survive empty row refreshes and can be restored before a replacement table is laid out.
		[[nodiscard]] table_layout::columns::sizes user_column_sizes() const { return _columns.user_sizes(); }
		void restore_column_sizes(table_layout::columns::sizes sizes)
		{
			_columns.restore(std::move(sizes));
			if (const auto host = _host.lock()) changed(host);
		}
		void reset_column_sizes()
		{
			_columns.reset();
			if (const auto host = _host.lock()) changed(host);
		}

		// The band the headings are drawn in, empty without headings. Pinned to the top while the rows scroll.
		[[nodiscard]] core::recti heading_bounds() const
		{
			if (_headings.empty() || _heading_height <= 0) return {};
			return core::recti(bounds.left, bounds.top, bounds.right, bounds.top + _heading_height)
			    .intersection(bounds);
		}
		[[nodiscard]] core::recti heading_cell_bounds(const int column) const
		{
			const auto band = heading_bounds();
			if (band.is_empty() || column < 0 || column >= static_cast<int>(_columns.widths().size())) return {};
			const auto left = band.left +
			                  std::accumulate(_columns.widths().begin(), _columns.widths().begin() + column, 0);
			return core::recti(left, band.top, left + _columns.widths()[column], band.bottom).intersection(band);
		}
		[[nodiscard]] core::recti divider_bounds(const int column) const
		{
			const auto band = heading_bounds();
			if (!resizable || band.is_empty()) return {};
			return _columns.divider_bounds(column, band, 0, std::max(2, core::round(4 * _scale)));
		}

		bool is_control_area(const core::pointi loc, const core::pointi offset) const override
		{
			if (!bounds.offset(offset).contains(loc)) return false;
			if (heading_bounds().offset(offset).contains(loc)) return true;
			if (can_scroll && _scroller.can_scroll() && _scroller.scroll_bounds().offset(offset).contains(loc))
				return true;
			auto found = false;
			visit_children([&](const element_ptr& child)
			{ found |= child->is_control_area(loc, offset + children_offset()); });
			return found;
		}

		core::sizei measure(render::measure_context& mc, const int width_limit) const override
		{
			const auto reserve = can_scroll && _scroller.can_scroll() ? mc.scroll_width : 0;
			auto extent = measure_cells(mc, std::max(0, width_limit - reserve), flex.grow > 0.0f);
			extent.cx += reserve;
			return extent;
		}

		void layout(render::measure_context& mc, const core::recti area) override
		{
			bounds = area;
			_scale = mc.scale_factor;
			measure_cells(mc, std::max(0, area.width()), true);
			const auto reserve = can_scroll && _total_height > area.height() ? mc.scroll_width : 0;
			if (reserve) measure_cells(mc, std::max(0, area.width() - reserve), true);
			for (const auto& r : _rows)
				for (const auto& c : r.cells)
					if (c.element) c.element->layout(mc, {area.top_left() + c.offset, c.extent});
			if (can_scroll)
				_scroller.layout({std::max(0, area.width() - reserve), _total_height - _heading_height},
				                 {area.left, area.top + _heading_height, area.right - reserve, area.bottom},
				                 {area.right - reserve, area.top + _heading_height, area.right, area.bottom});
			else _scroller.clear();
		}

		void render(render::draw_context& dc, const core::pointi offset) const override
		{
			const auto panel = bounds.offset(offset);
			if (panel.is_empty() || !dc.clip_bounds().intersects(panel)) return;
			dc.clip_bounds(panel);
			if (_rows.empty() && frame_when_empty)
			{
				const auto color = core::color(core::average(dc.colors.background, 0u), dc.colors.alpha);
				dc.draw_border(panel.inflate(-8), panel.inflate(-7), color, color);
			}

			// The rows scroll under the pinned headings.
			const auto band = heading_bounds().offset(offset);
			auto rows_area = panel;
			rows_area.top = std::max(rows_area.top, band.bottom);
			dc.clip_bounds(rows_area);
			const auto scroll = children_offset();
			for (size_t i = 1; i < _rows.size(); i += 2)
			{
				const auto& r = _rows[i];
				dc.draw_rounded_rect({panel.left, panel.top + scroll.y + r.y, panel.right,
				                      panel.top + scroll.y + r.y + r.height},
				                     core::color(0x000000, dc.colors.alpha / 11.11f), dc.padding1);
			}
			render_children(dc, offset);
			dc.restore_clip();

			if (!band.is_empty())
			{
				const auto porch = core::round(4 * dc.scale_factor);
				dc.draw_rounded_rect(band, core::color(0x000000, dc.colors.alpha * 0.2f), dc.padding1);

				for (int i = 0; i < static_cast<int>(_headings.size()); ++i)
				{
					const auto cell = heading_cell_bounds(i).offset(offset);
					if (cell.is_empty()) continue;
					if (i == _pressed_heading)
						dc.draw_rect(cell, core::color(dc.colors.foreground, dc.colors.alpha * 0.12f));
					const auto sorted = i == _sort_column;
					const auto& h = _headings[i];
					const auto color = core::color(dc.colors.foreground, dc.colors.alpha * (sorted ? 1.0f : 0.7f));
					dc.draw_text(h.title, cell.inflate(-porch, 0), render::font_face::dialog,
					             h.right_align ? render::text_style::single_line_far : render::text_style::single_line,
					             color, {});
					if (sorted) table_layout::draw_sort_mark(dc, cell, _sort_ascending, color);
				}

				// Faint, so the columns read as resizable, and plain while one is dragged.
				const auto line = std::max(1, core::round(dc.scale_factor));
				for (int i = 0; i < static_cast<int>(_columns.widths().size()) - 1; ++i)
				{
					const auto divider = divider_bounds(i).offset(offset);
					if (divider.height() <= 4) continue;
					const auto active = i == _active_divider;
					const auto width = active ? line * 2 : line;
					const auto x = divider.center().x - width / 2;
					dc.draw_rect({x, divider.top + 2, x + width, divider.bottom - 2},
					             core::color(dc.colors.foreground, dc.colors.alpha * (active ? 0.9f : 0.3f)));
				}
			}

			dc.restore_clip();
			if (can_scroll && _scroller.can_scroll())
			{
				offset_draw_context moved(dc, offset);
				_scroller.draw_scroll(moved);
			}
		}

		void visit_children(const std::function<void(const element_ptr&)>& visit) const override
		{
			for (const auto& r : _rows)
				for (const auto& c : r.cells)
					if (c.element) visit(c.element);
		}
		core::pointi children_offset() const override { return -_scroller.scroll_offset(); }
		void dispatch_event(const element_event& event) override
		{
			visit_children([&](const element_ptr& child) { child->dispatch_event(event); });
		}
		void hover(interaction_context& context) override
		{
			visit_children([&](const element_ptr& child) { child->hover(context); });
		}
		void tooltip(hover_bubble& hover, const core::pointi loc, const core::pointi offset) const override
		{
			if (heading_bounds().offset(offset).contains(loc)) return;
			const auto child_offset = offset + children_offset();
			visit_children([&](const element_ptr& child)
			{
				if (child->bounds.offset(child_offset).contains(loc)) child->tooltip(hover, loc, child_offset);
			});
		}
		bool wheel(const core::pointi loc, const core::pointi offset, const int delta) override
		{
			if (element::wheel(loc, offset, delta)) return true;
			if (can_scroll && _scroller.can_scroll() && bounds.offset(offset).contains(loc))
			{
				if (const auto host = _host.lock())
				{
					const auto before = _scroller.scroll_offset().y;
					_scroller.scroll_offset(host, 0, _scroller.scroll_offset().y - delta);
					return _scroller.scroll_offset().y != before;
				}
			}
			return false;
		}
		controller_ptr controller_from_location(const element_host_ptr& host, const core::pointi loc,
		                                        const core::pointi offset, hit_test_context& ctx) override
		{
			_host = host;
			if (!bounds.offset(offset).contains(loc)) return nullptr;

			// A divider sits over the edges of the headings either side of it, so it is found first.
			for (int i = 0; i < static_cast<int>(_columns.widths().size()) - 1; ++i)
			{
				const auto divider = divider_bounds(i).offset(offset);
				if (ctx.occluded(divider))
					return std::make_shared<divider_controller>(host, divider, shared_from_this(), i);
			}
			for (int i = 0; i < static_cast<int>(_headings.size()); ++i)
			{
				const auto cell = heading_cell_bounds(i).offset(offset);
				if (_headings[i].sortable && ctx.occluded(cell))
					return std::make_shared<heading_controller>(host, cell, shared_from_this(), i);
			}
			if (heading_bounds().offset(offset).contains(loc)) return nullptr;

			const auto scrollbar = _scroller.scroll_bounds().offset(offset);
			if (can_scroll && _scroller.can_scroll() && ctx.occluded(scrollbar))
				return std::make_shared<scroll_controller>(host, _scroller, scrollbar, offset, shared_from_this());
			ctx.confine(bounds.offset(offset));
			return element::controller_from_location(host, loc, offset, ctx);
		}
	};
}
