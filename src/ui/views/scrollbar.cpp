// This file is part of libdesktop, a C++20 platform layer for desktop applications
// Copyright 2026  Zac Walker
//
// This program is free software; you can redistribute it and / or modify it
// under the terms of the LGPL License either version 2.1 or later.
// License details are available at https://www.gnu.org/licenses/lgpl-2.1.html
// This program is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY

// Purpose: Scrollbar geometry, capture state, and themed drawing.

#include "libdesktop/ui/views/scrollbar.h"
#include "libdesktop/ui/views/view_theme.h"

#include <algorithm>

namespace
{
	int axis_position(const ui::scrollbar::orientation orientation, const core::pointi point)
	{
		return orientation == ui::scrollbar::orientation::vertical ? point.y : point.x;
	}

	int axis_extent(const ui::scrollbar::orientation orientation, const core::recti client)
	{
		return orientation == ui::scrollbar::orientation::vertical ? client.height() : client.width();
	}
}

bool ui::scrollbar::begin_tracking(const core::pointi point, const core::recti client) noexcept
{
	if (!hit_test(point, client)) return false;

	_tracking = true;
	_hover_tracking = true;
	_tracking_start_mouse = axis_position(_orient, point);
	_tracking_start_pos = _position;
	return true;
}

int ui::scrollbar::track_to(const core::pointi point, const core::recti client) const noexcept
{
	if (!_tracking) return _position;

	const auto extent = axis_extent(_orient, client);
	if (extent <= 0) return _position;

	const auto delta_mouse = axis_position(_orient, point) - _tracking_start_mouse;
	const auto delta_content = core::mul_div(delta_mouse, _content_size, extent);
	return std::clamp(_tracking_start_pos + delta_content, 0, std::max(0, _content_size - _page_size));
}

bool ui::scrollbar::set_hover(const bool hover) noexcept
{
	if (_hover == hover) return false;
	_hover = hover;
	return true;
}

bool ui::scrollbar::handle_mouse_down(const core::pointi point, const core::recti client) noexcept
{
	return begin_tracking(point, client);
}

bool ui::scrollbar::handle_mouse_move(const core::pointi point, const core::recti client, int& new_position) noexcept
{
	if (_tracking)
	{
		new_position = track_to(point, client);
		return true;
	}

	set_hover(hit_test(point, client));
	_hover_tracking = true;
	return false;
}

bool ui::scrollbar::handle_mouse_up(const core::pointi point, const core::recti client, int& new_position) noexcept
{
	if (!_tracking) return false;
	new_position = track_to(point, client);
	end_tracking();
	_hover_tracking = false;
	return true;
}

void ui::scrollbar::draw(render::draw_context& dc, const core::recti client, const view_theme& theme) const
{
	if (!can_scroll() || client.is_empty()) return;

	const auto alpha = dc.colors.alpha;
	const auto track = core::color(theme.handle, alpha);
	const auto thumb_color = core::color(_tracking ? theme.handle_tracking : theme.handle_hover, alpha);
	const auto tt = thumb_thickness();
	const auto em = edge_margin();
	const auto htw = hover_track_width();
	const auto scaled_pad = std::max(0, core::round(10 * _dpi_scale));

	if (_orient == orientation::vertical)
	{
		const auto span = thumb(client.top, client.height());
		auto x_pad = 0;
		if (_hover || _tracking)
		{
			dc.draw_rect({client.right - htw, client.top, client.right, client.bottom}, track);
			x_pad = scaled_pad;
		}
		dc.draw_rect({client.right - tt - em - x_pad, span.start,
		              client.right - em, span.start + span.length}, thumb_color);
	}
	else
	{
		const auto span = thumb(client.left, client.width());
		auto y_pad = 0;
		if (_hover || _tracking)
		{
			dc.draw_rect({client.left, client.bottom - htw, client.right, client.bottom}, track);
			y_pad = scaled_pad;
		}
		dc.draw_rect({span.start, client.bottom - tt - em - y_pad,
		              span.start + span.length, client.bottom - em}, thumb_color);
	}
}

void ui::scrollbar::draw(render::draw_context& dc, const core::recti client) const
{
	static const ui::view_theme theme;
	draw(dc, client, theme);
}