// This file is part of libdesktop, a C++20 platform layer for desktop applications
// Copyright 2026  Zac Walker
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

// Purpose: The drawn bubble: content its owner measures and draws, shown beside what it points at, with
// its arrow and shadow, in a layered popup the window layer makes - which fades it in and out where
// animations run. Placing it inside the work area and drawing it are the same on every platform.

#include "libdesktop/ui.h"
#include "libdesktop/render/software.h"
#include "libdesktop/ui/controls.h"
#include "libdesktop/ui/elements.h"

namespace
{
	constexpr int shadow_padding = 16;
	constexpr int vert_padding = shadow_padding + 6;
	constexpr int horz_padding = shadow_padding + 10;
	constexpr int corner_radius = 8;

	class drawn_bubble final :
		public ui::bubble_frame,
		public ui::frame_host,
		public std::enable_shared_from_this<drawn_bubble>
	{
		std::weak_ptr<ui::bubble_host> _host;
		ui::frame_ptr _popup;
		ui::element_ptr _content;
		core::recti _bounds;
		core::pointi _focus;

	public:
		explicit drawn_bubble(std::weak_ptr<ui::bubble_host> host) : _host(std::move(host)) {}

		~drawn_bubble() override
		{
			if (_popup) _popup->destroy();
		}

		drawn_bubble(const drawn_bubble&) = delete;
		drawn_bubble& operator=(const drawn_bubble&) = delete;

		void show(const ui::element_ptr& content, const core::recti anchor, const int x_focus, const int preferred_size,
		          const bool horizontal) override
		{
			const auto host = _host.lock();
			if (!host || !content) return;
			if (!_popup) _popup = host->create_bubble_popup(weak_from_this());
			if (!_popup) return;

			auto* const mc = host->bubble_measure(_popup);
			if (!mc) return;

			const auto extent = content->measure(*mc, core::round(preferred_size * _popup->scale_factor()));
			const auto placement = ui::place_bubble(extent, anchor, x_focus, horizontal,
			                                        host->bubble_work_area(anchor));
			_bounds = placement.bounds;
			_focus = placement.focus;
			_content = content;

			_content->layout(*mc, core::center_rect(extent, _bounds.extent()));

			_popup->invalidate();
			_popup->window_bounds(_bounds, true);
		}

		void hide() override
		{
			if (_popup) _popup->show(false);
		}

		bool is_visible() const override { return _popup && _popup->is_visible(); }

		void on_window_layout(render::measure_context&, core::sizei, bool) override {}

		void on_window_paint(render::draw_context& dc) override
		{
			// A layered popup's draw context is local to the popup; _bounds and _focus remain screen
			// coordinates because placement and hit testing use them.
			dc.clear(core::color(0.0f, 0.0f, 0.0f, 0.0f));
			render::draw_surface(dc, background(core::color(dc.colors.background, 1.0f)),
			                     {0, 0, _bounds.width(), _bounds.height()}, 1.0f, render::texture_sampler::point);

			if (_content) _content->render(dc, {});
		}

		// The shadow, the body and the arrow pointing at the focus, rasterised on the CPU as an application draws a
		// chart of its own: the bubble is the same picture whichever backend presents it.
		core::surface_ptr background(const core::color clr) const
		{
			auto art = std::make_shared<core::surface>();
			if (!art->alloc(_bounds.extent(), core::texture_format::ARGB)) return {};
			art->make_blank();

			const render::software_canvas canvas(*art);
			canvas.fill_shadow({0, 0, _bounds.width(), _bounds.height()}, shadow_padding * 2, 0.7f);

			const auto l = static_cast<float>(shadow_padding);
			const auto t = static_cast<float>(shadow_padding);
			const auto r = static_cast<float>(_bounds.width() - shadow_padding);
			const auto b = static_cast<float>(_bounds.height() - shadow_padding);
			canvas.fill_rounded_rect(core::recti(shadow_padding, shadow_padding, _bounds.width() - shadow_padding,
			                                     _bounds.height() - shadow_padding),
			                         clr, corner_radius);

			const auto p = static_cast<float>(shadow_padding);
			std::array<core::pointd, 3> pointer;

			if (_focus.y <= _bounds.top)
			{
				const auto center = std::clamp(static_cast<float>(_focus.x - _bounds.left), l + 24.0f, r - 24.0f);
				pointer = {{{center - p, t}, {center, t - p}, {center + p, t}}};
			}
			else if (_focus.x >= _bounds.right)
			{
				const auto center = std::clamp(static_cast<float>(_focus.y - _bounds.top), t + 24.0f, b - 24.0f);
				pointer = {{{r, center - p}, {r + p, center}, {r, center + p}}};
			}
			else if (_focus.y >= _bounds.bottom)
			{
				const auto center = std::clamp(static_cast<float>(_focus.x - _bounds.left), l + 24.0f, r - 24.0f);
				pointer = {{{center + p, b}, {center, b + p}, {center - p, b}}};
			}
			else
			{
				const auto center = std::clamp(static_cast<float>(_focus.y - _bounds.top), t + 24.0f, b - 24.0f);
				pointer = {{{l, center + p}, {l - p, center}, {l, center - p}}};
			}

			canvas.fill_triangle(pointer[0], pointer[1], pointer[2], clr);
			return art;
		}
	};
}

ui::bubble_placement ui::place_bubble(const core::sizei content_extent, const core::recti anchor, const int x_focus,
                                      const bool horizontal, const core::recti work)
{
	const auto cx = std::max(80, content_extent.cx) + horz_padding * 2;
	const auto cy = std::max(36, content_extent.cy) + vert_padding * 2;

	const auto x_centered = (x_focus == -1 ? (anchor.left + anchor.right) / 2 : anchor.left + x_focus) - cx / 2;
	const auto y_centered = (anchor.top + anchor.bottom - cy) / 2;

	auto x = horizontal ? anchor.left - cx : x_centered;
	auto y = horizontal ? y_centered : anchor.top - cy;
	auto focus = core::recti(x, y, x + cx, y + cy).center();

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
	return {core::recti(x, y, x + cx, y + cy).clamp(work.inflate(shadow_padding)), focus};
}

ui::bubble_window_ptr ui::create_drawn_bubble(const std::shared_ptr<bubble_host>& host)
{
	return std::make_shared<drawn_bubble>(host);
}
