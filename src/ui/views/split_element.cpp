// This file is part of libdesktop, a C++20 platform layer for desktop applications
// Copyright 2026  Zac Walker
//
// This program is free software; you can redistribute it and / or modify it
// under the terms of the LGPL License either version 2.1 or later.
// License details are available at https://www.gnu.org/licenses/lgpl-2.1.html
// This program is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY

// Purpose: Layout and pointer interaction for the two-pane splitter element.

#include "libdesktop/ui/views/split_element.h"
#include "libdesktop/ui/views/view_theme.h"

#include <algorithm>
#include <utility>

namespace
{
	class split_controller final : public ui::controller
	{
		ui::split_element& _split;
		core::pointi _origin;
		double _start_ratio = 0.5;

		void relayout()
		{
			if (!_host) return;
			if (const auto frame = _host->frame())
			{
				frame->layout();
				frame->invalidate();
			}
		}

	public:
		split_controller(const ui::element_host_ptr& host, ui::split_element& split, const core::recti bounds,
		                 const core::pointi origin) : controller(host, bounds), _split(split), _origin(origin),
		                                             _start_ratio(split._ratio)
		{
			_split._hover = true;
		}

		~split_controller() override
		{
			if (!_split._tracking) _split._hover = false;
		}

		platform::cursor cursor() const override
		{
			return _split.cursor();
		}

		void on_mouse_left_button_down(const core::pointi loc, const platform::key_state keys) override
		{
			controller::on_mouse_left_button_down(loc, keys);
			_start_ratio = _split._ratio;
			_split._tracking = true;
			_split._hover = true;
			relayout();
		}

		void on_mouse_move(const core::pointi loc) override
		{
			_last_loc = loc;
			if (_split._tracking)
			{
				_split.update_ratio(_split.bounds, loc - _origin);
				relayout();
			}
			else if (!_split._hover)
			{
				_split._hover = true;
				relayout();
			}
		}

		void on_mouse_left_button_up(const core::pointi loc, const platform::key_state keys) override
		{
			if (_split._tracking)
			{
				_split.update_ratio(_split.bounds, loc - _origin);
				_split._tracking = false;
			}
			relayout();
		}

		void on_mouse_leave() override
		{
			if (!_split._tracking) _split._hover = false;
			relayout();
		}

		bool escape() override
		{
			if (!_split._tracking) return false;
			_split._ratio = _start_ratio;
			_split._tracking = false;
			relayout();
			return true;
		}
	};
}

ui::split_element::split_element(element_ptr first, element_ptr second, const orientation orient,
                                 const double initial_ratio, const view_theme* const theme) noexcept :
	elements(), _orient(orient), _ratio(initial_ratio), _first(std::move(first)), _second(std::move(second)), _theme(theme)
{
	kind = element_kind::content;
	if (_first) _children.push_back(_first);
	if (_second) _children.push_back(_second);
	ratio(initial_ratio);
}

ui::split_element::split_element(const orientation orient, const double initial_ratio, element_ptr first,
                                 element_ptr second, const view_theme* const theme) noexcept :
	split_element(std::move(first), std::move(second), orient, initial_ratio, theme)
{
}

ui::split_element::split_element(element_ptr first, element_ptr second, const orientation orient,
                                 const double initial_ratio, const view_theme& theme) noexcept :
	split_element(std::move(first), std::move(second), orient, initial_ratio, &theme)
{
}

ui::split_element::split_element(const orientation orient, const double initial_ratio, element_ptr first,
                                 element_ptr second, const view_theme& theme) noexcept :
	split_element(std::move(first), std::move(second), orient, initial_ratio, &theme)
{
}

ui::split_element::split_element(const orientation orient, const double initial_ratio,
                                 const view_theme& theme) noexcept :
	split_element(orient, initial_ratio, {}, {}, &theme)
{
}

int ui::split_element::bar_width() const noexcept
{
	return static_cast<int>(base_bar_width * _dpi_scale);
}

void ui::split_element::set_dpi_scale(const double scale) noexcept
{
	_dpi_scale = scale;
}

int ui::split_element::split_pos(const core::recti bounds_in) const noexcept
{
	if (_orient == orientation::vertical)
		return bounds_in.left + static_cast<int>(bounds_in.width() * _ratio);
	return bounds_in.top + static_cast<int>(bounds_in.height() * _ratio);
}

core::recti ui::split_element::bar_rect(const core::recti bounds_in) const noexcept
{
	const auto pos = split_pos(bounds_in);
	const auto bw = bar_width();
	if (_orient == orientation::vertical)
		return {pos - bw, bounds_in.top, pos + bw, bounds_in.bottom};
	return {bounds_in.left, pos - bw, bounds_in.right, pos + bw};
}

bool ui::split_element::hit_test(const core::recti bounds_in, const core::pointi point) const noexcept
{
	return bar_rect(bounds_in).contains(point);
}

void ui::split_element::ratio(const double value) noexcept
{
	_ratio = std::clamp(value, min_ratio, max_ratio);
}

void ui::split_element::update_ratio(const core::recti bounds_in, const core::pointi point) noexcept
{
	if (_orient == orientation::vertical)
	{
		if (bounds_in.width() > 0) _ratio = (point.x - bounds_in.left) / static_cast<double>(bounds_in.width());
	}
	else if (bounds_in.height() > 0)
	{
		_ratio = (point.y - bounds_in.top) / static_cast<double>(bounds_in.height());
	}
	ratio(_ratio);
}

bool ui::split_element::begin_tracking(const core::recti bounds_in, const core::pointi point) noexcept
{
	if (!hit_test(bounds_in, point)) return false;
	_tracking = true;
	_hover = true;
	return true;
}

bool ui::split_element::track_to(const core::recti bounds_in, const core::pointi point) noexcept
{
	if (!_tracking) return false;
	update_ratio(bounds_in, point);
	return true;
}

void ui::split_element::end_tracking() noexcept
{
	_tracking = false;
}

bool ui::split_element::update_hover(const core::recti bounds_in, const core::pointi point) noexcept
{
	const auto next = hit_test(bounds_in, point);
	if (_hover == next) return false;
	_hover = next;
	return true;
}

void ui::split_element::clear_hover() noexcept
{
	_hover = false;
}

core::color32 ui::split_element::color() const noexcept
{
	if (_theme)
	{
		if (_tracking) return _theme->handle_tracking;
		if (_hover) return _theme->handle_hover;
		return _theme->handle;
	}

	if (_tracking) return core::rgba(0x11, 0x66, 0xCC);
	if (_hover) return core::rgba(0x66, 0x66, 0x66);
	return core::rgba(0x44, 0x44, 0x44);
}

platform::cursor ui::split_element::cursor() const noexcept
{
	return _orient == orientation::vertical ? platform::cursor::left_right : platform::cursor::up_down;
}

void ui::split_element::draw(render::draw_context& dc, const core::recti bounds_in) const
{
	dc.draw_rect(bar_rect(bounds_in), core::color(color(), dc.colors.alpha));
}

void ui::split_element::children(element_ptr first, element_ptr second) noexcept
{
	_first = std::move(first);
	_second = std::move(second);
	_children.clear();
	if (_first) _children.push_back(_first);
	if (_second) _children.push_back(_second);
}

core::sizei ui::split_element::measure(render::measure_context& mc, const int width_limit) const
{
	const auto first = _first ? _first->measure(mc, width_limit) : core::sizei{};
	const auto second = _second ? _second->measure(mc, width_limit) : core::sizei{};
	const auto gap = bar_width() * 2;

	if (_orient == orientation::vertical)
		return {first.cx + second.cx + gap, std::max(first.cy, second.cy)};
	return {std::max(first.cx, second.cx), first.cy + second.cy + gap};
}

void ui::split_element::layout(render::measure_context& mc, const core::recti bounds_in)
{
	bounds = bounds_in;
	set_dpi_scale(mc.scale_factor);

	const auto bw = bar_width();
	const auto total = _orient == orientation::vertical ? bounds.width() : bounds.height();
	const auto available = std::max(0, total - 2 * bw);
	const auto first_requested = _first ? (_orient == orientation::vertical ? _first->flex.min_size.cx
	                                                                         : _first->flex.min_size.cy) : 0;
	const auto second_requested = _second ? (_orient == orientation::vertical ? _second->flex.min_size.cx
	                                                                          : _second->flex.min_size.cy) : 0;
	const auto first_min = std::max(core::round(min_pane_size * mc.scale_factor),
	                                core::round(first_requested * mc.scale_factor));
	const auto second_min = std::max(core::round(min_pane_size * mc.scale_factor),
	                                 core::round(second_requested * mc.scale_factor));

	if (total > 0 && available > 0)
	{
		const auto lo = std::max(min_ratio, static_cast<double>(first_min + bw) / total);
		const auto hi = std::min(max_ratio, 1.0 - static_cast<double>(second_min + bw) / total);
		if (lo <= hi) _ratio = std::clamp(_ratio, lo, hi);
	}

	const auto pos = split_pos(bounds);
	core::recti first_bounds = bounds;
	core::recti second_bounds = bounds;
	if (_orient == orientation::vertical)
	{
		first_bounds.right = std::clamp(pos - bw, bounds.left, bounds.right);
		second_bounds.left = std::clamp(pos + bw, bounds.left, bounds.right);
	}
	else
	{
		first_bounds.bottom = std::clamp(pos - bw, bounds.top, bounds.bottom);
		second_bounds.top = std::clamp(pos + bw, bounds.top, bounds.bottom);
	}

	if (_first) _first->layout(mc, first_bounds);
	if (_second) _second->layout(mc, second_bounds);
}

void ui::split_element::render(render::draw_context& dc, const core::pointi element_offset) const
{
	if (_first && _first->is_visible()) _first->render(dc, element_offset);
	if (_second && _second->is_visible()) _second->render(dc, element_offset);

	draw(dc, bounds.offset(element_offset));
}

void ui::split_element::visit_children(const std::function<void(const element_ptr&)>& visit) const
{
	if (_first) visit(_first);
	if (_second) visit(_second);
}

ui::controller_ptr ui::split_element::controller_from_location(const element_host_ptr& host, const core::pointi loc,
                                                               const core::pointi element_offset, hit_test_context& ctx)
{
	const auto bar = bar_rect(bounds).offset(element_offset);
	if (bar.contains(loc) && ctx.occluded(bar))
		return std::make_shared<split_controller>(host, *this, bar, element_offset);

	ctx.passed_over(bar);
	if (_first)
	{
		if (const auto result = _first->controller_from_location(host, loc, element_offset, ctx)) return result;
	}
	if (_second)
	{
		if (const auto result = _second->controller_from_location(host, loc, element_offset, ctx)) return result;
	}
	return {};
}