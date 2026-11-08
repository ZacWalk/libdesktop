// This file is part of libdesktop, a C++20 platform layer for desktop applications
// Copyright 2026  Zac Walker
// 
// This program is free software; you can redistribute it and / or modify it
// under the terms of the LGPL License either version 2.1 or later.
// License details are available at https://www.gnu.org/licenses/lgpl-2.1.html
// This program is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY

// Purpose: The drawn bubble: content its owner measures and draws, shown beside what it points at, with
// its arrow and shadow, in a layered popup the window layer makes - which fades it in and out where
// animations run. Placing it inside the work area and drawing it are the same on every platform.

#include "libdesktop/render.h"
#include "libdesktop/render_controls.h"
#include "libdesktop/ui_elements.h"

namespace
{
	constexpr int shadow_padding = 16;
	constexpr int vert_padding = shadow_padding + 6;
	constexpr int horz_padding = shadow_padding + 10;
	constexpr float corner_radius = 8.0f;

	class drawn_bubble final : public ui::bubble_frame,
	                           public ui::frame_host,
	                           public std::enable_shared_from_this<drawn_bubble>
	{
		std::weak_ptr<ui::bubble_host> _host;
		ui::frame_ptr _popup;
		ui::element_ptr _content;
		recti _bounds;
		pointi _focus;

	public:
		explicit drawn_bubble(std::weak_ptr<ui::bubble_host> host) : _host(std::move(host))
		{
		}

		~drawn_bubble() override
		{
			if (_popup) _popup->destroy();
		}

		drawn_bubble(const drawn_bubble&) = delete;
		drawn_bubble& operator=(const drawn_bubble&) = delete;

		void show(const ui::element_ptr& content, const recti anchor, const int x_focus,
		          const int preferred_size, const bool horizontal) override
		{
			const auto host = _host.lock();
			if (!host || !content) return;
			if (!_popup) _popup = host->create_bubble_popup(weak_from_this());
			if (!_popup) return;

			auto* const mc = host->bubble_measure(_popup);
			if (!mc) return;

			const auto extent = content->measure(*mc, df::round(preferred_size * _popup->scale_factor()));
			const auto placement = ui::place_bubble(extent, anchor, x_focus, horizontal, host->bubble_work_area(anchor));
			_bounds = placement.bounds;
			_focus = placement.focus;
			_content = content;

			_content->layout(*mc, center_rect(extent, _bounds.extent()));

			_popup->invalidate();
			_popup->window_bounds(_bounds, true);
		}

		void hide() override
		{
			if (_popup) _popup->show(false);
		}

		bool is_visible() const override
		{
			return _popup && _popup->is_visible();
		}

		void on_window_layout(ui::measure_context&, sizei, bool) override
		{
		}

		void on_window_paint(ui::draw_context& dc) override
		{
			// The background is in screen pixels, as the bubble was placed.
			if (auto* const device = dynamic_cast<ui::draw_context_device*>(&dc))
			{
				device->draw_bubble_background(_bounds, _focus, shadow_padding, corner_radius);
			}

			if (_content) _content->render(dc, {});
		}
	};
}

ui::bubble_placement ui::place_bubble(const sizei content_extent, const recti anchor, const int x_focus,
                                      const bool horizontal, const recti work)
{
	const auto cx = std::max(80, content_extent.cx) + horz_padding * 2;
	const auto cy = std::max(36, content_extent.cy) + vert_padding * 2;

	const auto x_centered = (x_focus == -1 ? (anchor.left + anchor.right) / 2 : anchor.left + x_focus) - cx / 2;
	const auto y_centered = (anchor.top + anchor.bottom - cy) / 2;

	auto x = horizontal ? anchor.left - cx : x_centered;
	auto y = horizontal ? y_centered : anchor.top - cy;
	auto focus = recti(x, y, x + cx, y + cy).center();

	if (horizontal)
	{
		if (anchor.left - cx < work.left)
		{
			x = anchor.right;
			focus.x = anchor.right - 5;
		}
		else
		{
			focus.x = anchor.left + 5;
		}
	}
	else
	{
		if (anchor.top - cy < work.top)
		{
			y = anchor.bottom;
			focus.y = anchor.bottom - 5;
		}
		else
		{
			focus.y = anchor.top + 5;
		}
	}

	// The shadow may hang off the work area, but not the bubble.
	return {recti(x, y, x + cx, y + cy).clamp(work.inflate(shadow_padding)), focus};
}

ui::bubble_window_ptr ui::create_drawn_bubble(const std::shared_ptr<bubble_host>& host)
{
	return std::make_shared<drawn_bubble>(host);
}
