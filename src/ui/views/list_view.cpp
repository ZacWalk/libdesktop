// This file is part of libdesktop, a C++20 platform layer for desktop applications
// Copyright 2026  Zac Walker
//
// This program is free software; you can redistribute it and / or modify it
// under the terms of the LGPL License either version 2.1 or later.
// License details are available at https://www.gnu.org/licenses/lgpl-2.1.html
// This program is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY

// Purpose: Implement the selectable list control and its column/row rendering.

#include "libdesktop/ui/views/list_view.h"
#include "libdesktop/ui/views/text_view.h"

#include <algorithm>
#include <array>
#include <limits>

namespace ui
{
	namespace
	{
		int utf8_truncate(const std::string_view text, const int codepoints)
		{
			if (codepoints <= 0) return 0;
			int count = 0;
			for (int i = 0; i < static_cast<int>(text.size()); ++i)
			{
				if (core::is_utf8_continuation(static_cast<unsigned char>(text[i]))) continue;
				if (count++ == codepoints) return i;
			}
			return static_cast<int>(text.size());
		}

		core::color transparent()
		{
			return {0.0f, 0.0f, 0.0f, 0.0f};
		}

		void draw_text_cell(render::draw_context& dc, const std::string_view text, const core::recti bounds,
		                    const render::font_face font, const core::color32 foreground,
		                    const core::color32 background)
		{
			if (bounds.is_empty() || text.empty()) return;
			dc.draw_rect(bounds, core::color(background));
			dc.draw_text(text, bounds, font, render::text_style::single_line,
			             core::color(foreground), transparent());
		}
	}

	list_view::list_view(const view_theme& theme, view_context* context, text_clipboard* clipboard)
		: view_base(theme), _context(context), _clipboard(clipboard)
	{
	}

	list_view::~list_view() = default;

	render::font_face list_view::list_font() const
	{
		if (!_list_face_valid)
		{
			_list_face = render::make_font(_theme.list_font);
			_list_face_valid = true;
		}
		return _list_face;
	}

	core::color list_view::text_color(const core::color32 value) const
	{
		return core::color(value);
	}

	core::sizei list_view::measure_content(render::measure_context& mc, const int width_limit) const
	{
		const auto extent = mc.measure_text("X", list_font(), render::text_style::single_line,
		                                   std::max(1, width_limit), 0);
		return {std::max(0, extent.cx + _theme.padding_x * 2),
		        std::max(0, extent.cy + _theme.padding_y * 2)};
	}

	void list_view::on_placed(render::measure_context& mc)
	{
		view_base::on_placed(mc);
		const auto extent = mc.measure_text("X", list_font(), render::text_style::single_line,
		                                   std::numeric_limits<int>::max() / 4, 0);
		_font_extent.cx = std::max(1, extent.cx);
		_font_extent.cy = std::max(1, extent.cy);
		layout_list();
	}

	void list_view::set_columns(std::vector<list_column> columns)
	{
		_columns = std::move(columns);
		if (_sort_column >= static_cast<int>(_columns.size())) _sort_column = -1;
		_header_height = _columns.empty() ? 0 : _font_extent.cy + _theme.padding_y * 2;
		layout_list();
	}

	void list_view::set_sort(const int column, const bool ascending)
	{
		_sort_column = column >= 0 && column < static_cast<int>(_columns.size()) ? column : -1;
		_sort_ascending = ascending;
	}

	int list_view::column_at(const core::pointi point) const
	{
		if (_columns.empty() || point.y < 0 || point.y >= _header_height) return -1;
		const auto rects = column_rects(_view_extent.cx);
		for (size_t i = 0; i < rects.size(); ++i)
			if (point.x >= rects[i].left && point.x < rects[i].right) return static_cast<int>(i);
		return -1;
	}

	std::vector<core::recti> list_view::column_rects(const int width) const
	{
		std::vector<core::recti> rects;
		if (_columns.empty()) return rects;

		int total = 0;
		for (const auto& column : _columns) total += std::max(1, column.weight);
		const auto available = std::max(0, width - scrollbar_reserve());
		int x = 0;
		for (size_t i = 0; i < _columns.size(); ++i)
		{
			const auto weight = std::max(1, _columns[i].weight);
			const auto w = i + 1 == _columns.size() ? available - x : available * weight / total;
			rects.emplace_back(x, 0, x + std::max(0, w), 0);
			x += std::max(0, w);
		}
		return rects;
	}

	int list_view::selected_index() const
	{
		if (!_selected_item) return -1;
		if (_selected_index >= 0 && _selected_index < static_cast<int>(_items.size()) &&
		    _items[_selected_index] == _selected_item)
			return _selected_index;
		for (int i = 0; i < static_cast<int>(_items.size()); ++i)
			if (_items[i] == _selected_item)
			{
				_selected_index = i;
				return i;
			}
		_selected_index = -1;
		return -1;
	}

	std::pair<int, int> list_view::selected_range() const
	{
		const auto current = selected_index();
		if (current < 0) return {-1, -1};
		if (_anchor_index < 0 || _anchor_index >= static_cast<int>(_items.size())) return {current, current};
		return {std::min(_anchor_index, current), std::max(_anchor_index, current)};
	}

	bool list_view::is_selected(const int index) const
	{
		const auto [lo, hi] = selected_range();
		return lo >= 0 && index >= lo && index <= hi;
	}

	void list_view::set_selected(const int index)
	{
		_selected_index = index;
		_anchor_index = index;
		_selected_item = index >= 0 && index < static_cast<int>(_items.size()) ? _items[index] : nullptr;
	}

	std::string list_view::row_text(const list_item& item) const
	{
		if (!item.cells.empty())
		{
			std::string result;
			for (const auto& cell : item.cells)
			{
				if (!result.empty()) result += '\t';
				result += cell.text;
			}
			return result;
		}
		return item.prefix.empty() ? item.text : item.prefix + item.text;
	}

	std::string list_view::selected_rows_text() const
	{
		const auto [lo, hi] = selected_range();
		if (lo < 0) return {};
		std::string result;
		for (int i = lo; i <= hi; ++i)
		{
			if (!result.empty()) result += "\r\n";
			result += row_text(*_items[i]);
		}
		return result;
	}

	bool list_view::set_clipboard(const std::string_view text) const
	{
		if (_clipboard) return _clipboard->set_text(text);
		return system_clipboard().set_text(text);
	}

	bool list_view::copy_rows() const
	{
		const auto text = selected_rows_text();
		return !text.empty() && set_clipboard(text);
	}

	std::vector<command_ptr> list_view::default_popup_menu()
	{
		auto copy = std::make_shared<command>();
		copy->text = "&Copy";
		copy->enable = can_copy_rows();
		copy->invoke = [this] { (void)copy_rows(); };
		return {std::move(copy)};
	}

	void list_view::navigate_next(const bool forward, const bool skip_groups, const bool extend)
	{
		if (_items.empty()) return;
		const auto count = static_cast<int>(_items.size());
		const auto current = selected_index();
		const auto step = forward ? 1 : -1;
		auto next = current < 0 ? (forward ? 0 : count - 1) : current + step;
		while (next >= 0 && next < count && skip_groups && _items[next]->is_group) next += step;
		if (next < 0 || next >= count) return;
		const auto anchor = extend && current >= 0 ? (_anchor_index < 0 ? current : _anchor_index) : next;
		set_selected(next);
		_anchor_index = anchor;
		ensure_visible(_selected_item);
		invalidate();
		on_item_selected(_selected_item, false);
	}

	bool list_view::can_scroll() const
	{
		return _content_extent.cy > std::max(0, _view_extent.cy - _header_height);
	}

	core::recti list_view::scrollbar_rect() const
	{
		return {0, _header_height, _view_extent.cx, _view_extent.cy};
	}

	int list_view::scrollbar_reserve() const
	{
		return can_scroll() ? _vscroll.thumb_thickness() + _vscroll.edge_margin() + 4 : 0;
	}

	void list_view::update_scrollbar()
	{
		const auto visible = std::max(0, _view_extent.cy - _header_height);
		_vscroll.update(_content_extent.cy, visible, _scroll_offset.y);
	}

	void list_view::scroll_to(int offset)
	{
		const auto max_y = std::max(0, _content_extent.cy - std::max(0, _view_extent.cy - _header_height));
		const auto clamped = std::clamp(offset, 0, max_y);
		if (_scroll_offset.y == clamped) return;
		_scroll_offset.y = clamped;
		update_scrollbar();
		invalidate();
	}

	void list_view::ensure_visible(const list_item_ptr& item)
	{
		if (!item) return;
		const auto visible_top = _scroll_offset.y + _header_height;
		const auto visible_bottom = _scroll_offset.y + _view_extent.cy;
		if (item->bounds.top < visible_top) scroll_to(item->bounds.top - _header_height);
		else if (item->bounds.bottom > visible_bottom) scroll_to(item->bounds.bottom - _view_extent.cy);
	}

	list_item_ptr list_view::selection_from_point(const core::pointi point) const
	{
		int lo = 0;
		int hi = static_cast<int>(_items.size()) - 1;
		while (lo <= hi)
		{
			const auto mid = lo + (hi - lo) / 2;
			const auto& bounds = _items[mid]->bounds;
			if (point.y < bounds.top) hi = mid - 1;
			else if (point.y >= bounds.bottom) lo = mid + 1;
			else return _items[mid];
		}
		return nullptr;
	}

	void list_view::select_list_item(const list_item_ptr& item, const bool activated)
	{
		for (int i = 0; i < static_cast<int>(_items.size()); ++i)
			if (_items[i] == item)
			{
				set_selected(i);
				ensure_visible(item);
				invalidate();
				on_item_selected(item, activated);
				return;
			}
	}

	void list_view::layout_list(render::measure_context& mc)
	{
		const auto extent = mc.measure_text("X", list_font(), render::text_style::single_line,
		                                   std::numeric_limits<int>::max() / 4, 0);
		_font_extent = {std::max(1, extent.cx), std::max(1, extent.cy)};
		_header_height = _columns.empty() ? 0 : _font_extent.cy + _theme.padding_y * 2;
		_vscroll.set_dpi_scale(mc.scale_factor);
		layout_list();
	}

	void list_view::layout_list()
	{
		int y = _header_height + _theme.list_top_pad;
		const auto item_height = _font_extent.cy + _theme.padding_y * 2;
		for (const auto& item : _items)
		{
			if (!item) continue;
			item->bounds = {0, y, _view_extent.cx, y + item_height};
			y += item_height;
		}
		_content_extent.cy = y + _theme.list_scroll_pad;
		_content_extent.cx = _view_extent.cx;
		_scroll_offset.y = std::clamp(_scroll_offset.y, 0,
		                              std::max(0, _content_extent.cy - std::max(0, _view_extent.cy - _header_height)));
		update_scrollbar();
		invalidate();
	}

	void list_view::paint(render::draw_context& dc, const bool focused) const
	{
		const auto view = core::recti(0, 0, _view_extent.cx, _view_extent.cy);
		dc.draw_rect(view, core::color(_theme.tool_background));
		if (_header_height > 0) draw_header(dc, {view.left, view.top, view.right, _header_height});

		const auto items_top = view.top + _header_height;
		for (int index = first_visible_item(); index < static_cast<int>(_items.size()); ++index)
		{
			const auto& item = _items[index];
			if (!item) continue;
			auto bounds = item->bounds.offset(0, -_scroll_offset.y);
			if (bounds.top >= view.bottom) break;
			if (bounds.bottom <= items_top) continue;
			bounds = bounds.crop({view.left, items_top, view.right, view.bottom});
			const bool selected = is_selected(index);
			const bool hovered = item == _hover_item;
			if (hovered) dc.draw_rect(bounds, core::color(_theme.handle_hover));
			if (selected) dc.draw_rect(bounds, core::color(focused ? _theme.focus_handle : _theme.handle));
			draw_item(dc, item, bounds, selected, hovered);
		}

		_vscroll.draw(dc, scrollbar_rect(), _theme);
	}

	void list_view::on_focus(const bool focused, const bool)
	{
		if (_focused == focused) return;
		_focused = focused;
		invalidate();
	}

	bool list_view::on_key(const int key, const platform::key_state keys)
	{
		const auto k = static_cast<char32_t>(key);
		if (keys.control && !keys.shift && k == U'C')
		{
			(void)copy_rows();
			return true;
		}
		if (k == platform::keys::DOWN)
		{
			navigate_next(true, false, keys.shift);
			return true;
		}
		if (k == platform::keys::UP)
		{
			navigate_next(false, false, keys.shift);
			return true;
		}
		if (k == platform::keys::RETURN)
		{
			on_item_activated();
			return true;
		}
		if (k == platform::keys::ESCAPE)
		{
			if (_context) _context->on_escape();
			return true;
		}
		return false;
	}

	bool list_view::on_text(std::string_view)
	{
		return false;
	}

	void list_view::on_mouse_down(const core::pointi loc, const platform::key_state keys)
	{
		take_focus(false);
		if (loc.y < _header_height)
		{
			if (const auto column = column_at(loc); column >= 0)
			{
				const auto ascending = column == _sort_column ? !_sort_ascending : _columns[column].default_ascending;
				set_sort(column, ascending);
				on_sort_changed(column, ascending);
				invalidate();
			}
			return;
		}

		const auto bar = scrollbar_rect();
		if (_vscroll.handle_mouse_down(loc, bar))
		{
			_scroll_tracking = true;
			if (host()) host()->capture_pointer(true);
			return;
		}

		if (const auto item = selection_from_point({loc.x, loc.y + _scroll_offset.y}))
		{
			if (keys.shift && _selected_item) extend_selection_to(item);
			else select_list_item(item);
		}
		invalidate();
	}

	void list_view::on_mouse_move(const core::pointi loc, const bool captured)
	{
		const auto bar = scrollbar_rect();
		if (_scroll_tracking && captured)
		{
			int new_position = _scroll_offset.y;
			if (_vscroll.handle_mouse_move(loc, bar, new_position))
			{
				scroll_to(new_position);
				return;
			}
		}

		if (_vscroll.set_hover(_vscroll.hit_test(loc, bar))) invalidate();
		if (!_scroll_tracking)
		{
			const auto item = loc.y >= _header_height
			                 ? selection_from_point({loc.x, loc.y + _scroll_offset.y})
			                 : nullptr;
			if (_hover_item != item)
			{
				_hover_item = item;
				invalidate();
			}
		}
	}

	void list_view::on_mouse_up(const core::pointi loc, const platform::key_state)
	{
		if (_scroll_tracking)
		{
			int new_position = _scroll_offset.y;
			_vscroll.handle_mouse_up(loc, scrollbar_rect(), new_position);
			scroll_to(new_position);
			_scroll_tracking = false;
			if (host()) host()->capture_pointer(false);
			invalidate();
		}
	}

	void list_view::on_mouse_leave()
	{
		if (_hover_item || _vscroll.set_hover(false))
		{
			_hover_item.reset();
			invalidate();
		}
	}

	bool list_view::on_wheel(const core::pointi, const int delta, const platform::key_state keys)
	{
		if (keys.control)
		{
			zoom(delta > 0 ? 1 : -1);
			return true;
		}
		scroll_to(_scroll_offset.y - delta / 2);
		return true;
	}

	std::vector<command_ptr> list_view::context_commands(const core::pointi loc)
	{
		if (loc.y >= _header_height && !_vscroll.hit_test(loc, scrollbar_rect()))
		{
			if (const auto item = selection_from_point({loc.x, loc.y + _scroll_offset.y}))
			{
				for (int i = 0; i < static_cast<int>(_items.size()); ++i)
					if (_items[i] == item && !is_selected(i))
					{
						select_list_item(item, false);
						break;
					}
			}
		}
		return default_popup_menu();
	}

	platform::cursor list_view::cursor_at(const core::pointi) const
	{
		return platform::cursor::normal;
	}

	void list_view::scale_changed()
	{
		view_base::scale_changed();
		if (auto* mc = metrics())
		{
			_vscroll.set_dpi_scale(mc->scale_factor);
			layout_list(*mc);
		}
	}

	void list_view::draw_header(render::draw_context& dc, const core::recti& header_rect) const
	{
		if (_columns.empty()) return;
		dc.draw_rect(header_rect, core::color(_theme.header_background));
		const auto rects = column_rects(header_rect.width());
		const auto font = list_font();
		const auto y = header_rect.top + _theme.padding_y;
		for (size_t i = 0; i < _columns.size() && i < rects.size(); ++i)
		{
			const auto& column = _columns[i];
			const bool sorted = static_cast<int>(i) == _sort_column;
			auto title = column.title;
			if (sorted) title += _sort_ascending ? " ▲" : " ▼";
			const auto size = dc.measure_text(title, font, render::text_style::single_line,
			                                 std::numeric_limits<int>::max() / 4, 0);
			const auto left = header_rect.left + rects[i].left;
			const auto right = header_rect.left + rects[i].right;
			const auto x = column.right_align ? std::max(left, right - _theme.padding_x - size.cx)
			                                : left + _theme.padding_x;
			const core::recti clip{x, y, std::min(x + size.cx, right), y + size.cy};
			if (!clip.is_empty()) draw_text_cell(dc, title, clip, font,
			                                   sorted ? _theme.text : _theme.dim_text,
			                                   _theme.header_background);
		}
	}

	void list_view::on_sort_changed(int, bool)
	{
	}

	void list_view::on_item_selected(const list_item_ptr&, bool)
	{
	}

	void list_view::on_item_activated()
	{
	}

	void list_view::zoom(const int delta)
	{
		if (_context) _context->on_zoom(delta);
	}

	core::color32 list_view::item_text_color(const list_item& item) const
	{
		return item.is_group ? _theme.group_text : _theme.text;
	}

	void list_view::draw_expand_icon(render::draw_context& dc, const int cx, const int cy, const bool expanded) const
	{
		constexpr int size = 4;
		std::array<core::pointd, 3> points;
		if (expanded)
			points = {core::pointd(cx - size, cy - size / 2), core::pointd(cx, cy + size / 2),
			          core::pointd(cx + size, cy - size / 2)};
		else
			points = {core::pointd(cx - size / 2, cy - size), core::pointd(cx + size / 2, cy),
			          core::pointd(cx - size / 2, cy + size)};
		dc.draw_polyline(points, core::color(_theme.line));
	}

	int list_view::centered_row_text_top(const core::recti& bounds) const
	{
		return bounds.top + std::max(0, (bounds.height() - _font_extent.cy) / 2);
	}

	int list_view::first_visible_item() const
	{
		int lo = 0;
		int hi = static_cast<int>(_items.size()) - 1;
		while (lo <= hi)
		{
			const auto mid = lo + (hi - lo) / 2;
			if (_items[mid]->bounds.bottom <= _scroll_offset.y) lo = mid + 1;
			else hi = mid - 1;
		}
		return lo;
	}

	void list_view::extend_selection_to(const list_item_ptr& item)
	{
		const auto anchor = _anchor_index >= 0 ? _anchor_index : selected_index();
		for (int i = 0; i < static_cast<int>(_items.size()); ++i)
			if (_items[i] == item)
			{
				set_selected(i);
				_anchor_index = anchor;
				ensure_visible(item);
				invalidate();
				on_item_selected(item, false);
				return;
			}
	}

	void list_view::draw_item(render::draw_context& dc, const list_item_ptr& item, const core::recti& bounds,
	                          const bool selected, const bool hovered) const
	{
		if (!item) return;
		const auto indent = _theme.padding_x + item->depth * _theme.indent;
		const auto font = list_font();
		const auto bg = selected ? (_focused ? _theme.focus_handle : _theme.handle)
		                        : (hovered ? _theme.handle_hover : _theme.tool_background);
		if (!_columns.empty())
		{
			draw_cells(dc, *item, bounds, font, bg);
			return;
		}
		if (item->is_group)
		{
			draw_expand_icon(dc, bounds.left + indent + 4, (bounds.top + bounds.bottom) / 2, item->expanded);
			auto text_bounds = bounds;
			text_bounds.left += indent + 14;
			text_bounds = text_bounds.inflate(-_theme.padding_x, -_theme.padding_y);
			draw_text_cell(dc, item->text, text_bounds, font, item_text_color(*item), bg);
			return;
		}
		auto text_bounds = bounds;
		text_bounds.left += indent + 4;
		text_bounds = text_bounds.inflate(-_theme.padding_x, -_theme.padding_y);
		auto content = text_bounds;
		if (!item->prefix.empty())
		{
			const auto prefix_size = dc.measure_text(item->prefix, font, render::text_style::single_line,
			                                       std::numeric_limits<int>::max() / 4, 0);
			auto prefix_bounds = text_bounds;
			prefix_bounds.right = prefix_bounds.left + prefix_size.cx;
			draw_text_cell(dc, item->prefix, prefix_bounds, font, _theme.dim_text, bg);
			content.left += prefix_size.cx;
		}
		if (item->has_match()) draw_matched_text(dc, *item, content, font, bg);
		else draw_plain_text(dc, *item, content, font, bg);
	}

	void list_view::draw_cells(render::draw_context& dc, const list_item& item, const core::recti& bounds,
	                           const render::font_face font, const core::color32 bg) const
	{
		const auto rects = column_rects(bounds.width());
		const auto row_color = item_text_color(item);
		const auto y = centered_row_text_top(bounds);
		for (size_t i = 0; i < _columns.size() && i < rects.size(); ++i)
		{
			const auto left = bounds.left + rects[i].left;
			const auto right = bounds.left + rects[i].right;
			if (right <= left || i >= item.cells.size() || item.cells[i].text.empty()) continue;
			const auto& cell = item.cells[i];
			const auto color = cell.has_color ? cell.color : row_color;
			const auto size = dc.measure_text(cell.text, font, render::text_style::single_line,
			                                 std::numeric_limits<int>::max() / 4, 0);
			const auto x = _columns[i].right_align ? std::max(left, right - _theme.padding_x - size.cx)
			                                    : left + _theme.padding_x;
			const core::recti clip{x, y, std::min(x + size.cx, right), y + size.cy};
			if (!clip.is_empty()) draw_text_cell(dc, cell.text, clip, font, color, bg);
		}
	}

	void list_view::draw_matched_text(render::draw_context& dc, const list_item& item,
	                                  const core::recti& content_rect, const render::font_face font,
	                                  const core::color32 bg) const
	{
		const std::string_view name = item.text;
		const auto name_len = static_cast<int>(name.size());
		const auto avail = content_rect.width();
		const auto full_width = dc.measure_text(name, font, render::text_style::single_line,
		                                       std::numeric_limits<int>::max() / 4, 0).cx;
		std::string_view display = name;
		int display_match_start = item.match_start;
		bool ellipsis_prefix = false;
		bool ellipsis_suffix = false;
		if (full_width > avail)
		{
			const auto ellipsis_width = dc.measure_text("...", font, render::text_style::single_line,
			                                           std::numeric_limits<int>::max() / 4, 0).cx;
			const auto match_start = std::clamp(item.match_start, 0, name_len);
			const auto match_end = std::clamp(match_start + item.match_length, match_start, name_len);
			int vis_start = match_start;
			int vis_end = match_end;
			const auto width_of_radius = [&](const int radius)
			{
				vis_start = std::max(0, match_start - radius);
				while (vis_start > 0 && core::is_utf8_continuation(static_cast<unsigned char>(name[vis_start]))) --vis_start;
				vis_end = std::min(name_len, match_end + radius);
				while (vis_end < name_len && core::is_utf8_continuation(static_cast<unsigned char>(name[vis_end]))) ++vis_end;
				auto width = dc.measure_text(name.substr(vis_start, vis_end - vis_start), font,
				                            render::text_style::single_line, std::numeric_limits<int>::max() / 4, 0).cx;
				if (vis_start > 0) width += ellipsis_width;
				if (vis_end < name_len) width += ellipsis_width;
				return width;
			};
			int lo = 0;
			int hi = name_len;
			while (lo < hi)
			{
				const auto mid = lo + (hi - lo + 1) / 2;
				if (width_of_radius(mid) <= avail) lo = mid;
				else hi = mid - 1;
			}
			width_of_radius(lo);
			ellipsis_prefix = vis_start > 0;
			ellipsis_suffix = vis_end < name_len;
			display = name.substr(vis_start, vis_end - vis_start);
			display_match_start = match_start - vis_start;
		}

		core::recti segment = content_rect;
		const auto foreground = item_text_color(item);
		const auto draw_segment = [&](const std::string_view text, const core::color32 segment_bg)
		{
			if (text.empty() || segment.left >= content_rect.right) return;
			const auto width = dc.measure_text(text, font, render::text_style::single_line,
			                                  std::numeric_limits<int>::max() / 4, 0).cx;
			segment.right = std::min(segment.left + width, content_rect.right);
			if (!segment.is_empty()) draw_text_cell(dc, text, segment, font, foreground, segment_bg);
			segment.left = segment.right;
		};
		if (ellipsis_prefix) draw_segment("...", bg);
		if (display_match_start >= 0 && display_match_start < static_cast<int>(display.size()))
		{
			const auto match_end = std::min(display_match_start + item.match_length, static_cast<int>(display.size()));
			draw_segment(display.substr(0, display_match_start), bg);
			draw_segment(display.substr(display_match_start, match_end - display_match_start), _theme.match_highlight);
			draw_segment(display.substr(match_end), bg);
		}
		else draw_segment(display, bg);
		if (ellipsis_suffix) draw_segment("...", bg);
		if (segment.left < content_rect.right)
			dc.draw_rect({segment.left, content_rect.top, content_rect.right, content_rect.bottom}, core::color(bg));
	}

	void list_view::draw_plain_text(render::draw_context& dc, const list_item& item,
	                                const core::recti& content_rect, const render::font_face font,
	                                const core::color32 bg) const
	{
		std::string ellipsized;
		std::string_view display = item.text;
		if (dc.measure_text(display, font, render::text_style::single_line,
		                    std::numeric_limits<int>::max() / 4, 0).cx > content_rect.width())
		{
			const auto ellipsis_width = dc.measure_text("...", font, render::text_style::single_line,
			                                           std::numeric_limits<int>::max() / 4, 0).cx;
			const auto available = content_rect.width() - ellipsis_width;
			int lo = 0;
			int hi = core::utf8_codepoint_count(display);
			while (lo < hi)
			{
				const auto mid = lo + (hi - lo + 1) / 2;
				const auto bytes = utf8_truncate(display, mid);
				if (dc.measure_text(display.substr(0, bytes), font, render::text_style::single_line,
				                   std::numeric_limits<int>::max() / 4, 0).cx <= available)
					lo = mid;
				else hi = mid - 1;
			}
			ellipsized.assign(display.substr(0, utf8_truncate(display, lo)));
			ellipsized += "...";
			display = ellipsized;
		}
		draw_text_cell(dc, display, content_rect, font, item_text_color(item), bg);
	}
}