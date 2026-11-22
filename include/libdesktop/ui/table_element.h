// This file is part of libdesktop, a C++20 platform layer for desktop applications
// Copyright 2026  Zac Walker
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

// Purpose: An element table with shared column sizing, optional headings that sort its rows and resize its
// columns, and normal tree-owned cells. The headings' band is table_headings.h's, which every headed table shares.

#pragma once

#include "libdesktop/ui/elements.h"
#include "libdesktop/ui/table_headings.h"

#include <array>
#include <numeric>
#include <type_traits>

namespace ui
{
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
				const auto key = [column](const row& of)
				{
					return column < static_cast<int>(of.keys.size()) ? &of.keys[column] : nullptr;
				};
				return table_sorts_before(key(l), key(r), ascending);
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

		void user_changed()
		{
			if (const auto host = _host.lock()) user_changed(host);
		}

		void click_heading(const int column)
		{
			if (column < 0 || column >= static_cast<int>(_headings.size()) || !_headings[column].sortable) return;
			const auto next = next_table_sort({_sort_column, _sort_ascending}, column,
			                                  _headings[column].default_ascending);
			sort(next.column, next.ascending);
			user_changed();
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
				table_layout::draw_row_stripe(dc, {panel.left, panel.top + scroll.y + r.y, panel.right,
				                                   panel.top + scroll.y + r.y + r.height});
			}
			render_children(dc, offset);
			dc.restore_clip();

			if (!band.is_empty())
			{
				const auto porch = core::round(4 * dc.scale_factor);
				std::vector<table_heading_cell> cells;
				cells.reserve(_headings.size());

				for (int i = 0; i < static_cast<int>(_headings.size()); ++i)
				{
					const auto cell = heading_cell_bounds(i).offset(offset);
					cells.push_back({cell, cell.inflate(-porch, 0)});
				}

				std::vector<core::recti> dividers;
				for (int i = 0; i < static_cast<int>(_columns.widths().size()) - 1; ++i)
					dividers.push_back(divider_bounds(i).offset(offset));

				draw_table_headings(dc, band, _headings, cells, dividers, {_sort_column, _sort_ascending},
				                    _pressed_heading, _active_divider);
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
				{
					const auto self = shared_from_this();
					const auto changed = [self]
					{
						self->user_changed();
					};
					const auto active = [self, i](const bool held)
					{
						self->_active_divider = held ? i : -1;
					};
					auto actions = column_divider_actions(_columns, i, changed, active);
					return std::make_shared<table_divider_controller>(host, divider, std::move(actions));
				}
			}
			for (int i = 0; i < static_cast<int>(_headings.size()); ++i)
			{
				const auto cell = heading_cell_bounds(i).offset(offset);
				if (_headings[i].sortable && ctx.occluded(cell))
				{
					const auto self = shared_from_this();
					const auto pressed = [self, i](const bool down)
					{
						self->_pressed_heading = down ? i : -1;
					};
					const auto click = [self, i]
					{
						self->click_heading(i);
					};
					return std::make_shared<table_heading_controller>(host, cell, pressed, click);
				}
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
