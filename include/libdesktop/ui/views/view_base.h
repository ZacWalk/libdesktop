// This file is part of libdesktop, a C++20 platform layer for desktop applications
// Copyright 2026  Zac Walker
//
// This program is free software; you can redistribute it and / or modify it
// under the terms of the LGPL License either version 2.1 or later.
// License details are available at https://www.gnu.org/licenses/lgpl-2.1.html
// This program is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY

// Purpose: Control-backed viewport state and scrollbar input shared by text views.

#pragma once

#include "libdesktop/ui/controls.h"
#include "libdesktop/ui/views/scrollbar.h"
#include "libdesktop/ui/views/view_theme.h"

#include <algorithm>

namespace ui
{
	class view_base : public control
	{
	protected:
		scrollbar _hscroll{scrollbar::orientation::horizontal};
		scrollbar _vscroll{scrollbar::orientation::vertical};

		core::sizei _view_extent = {};
		core::sizei _content_extent = {};
		core::pointi _scroll_offset = {};
		const view_theme& _theme;

		explicit view_base(const view_theme& theme) noexcept : _theme(theme)
		{
			traits.focus_on_click = true;
			// A view lays out its document from its own corner, wherever the tree puts it.
			traits.local_coordinates = true;
		}

		void clamp_scroll_offset() noexcept
		{
			_scroll_offset.x = std::clamp(_scroll_offset.x, 0, std::max(0, _content_extent.cx - _view_extent.cx));
			_scroll_offset.y = std::clamp(_scroll_offset.y, 0, std::max(0, _content_extent.cy - _view_extent.cy));
			_hscroll.update(_content_extent.cx, _view_extent.cx, _scroll_offset.x);
			_vscroll.update(_content_extent.cy, _view_extent.cy, _scroll_offset.y);
		}

		[[nodiscard]] core::recti client_rect() const noexcept
		{
			return {0, 0, _view_extent.cx, _view_extent.cy};
		}

		[[nodiscard]] int max_scroll_y() const noexcept
		{
			return std::max(0, _content_extent.cy - _view_extent.cy);
		}

		void scroll_to(core::pointi offset)
		{
			const auto old = _scroll_offset;
			_scroll_offset = offset;
			clamp_scroll_offset();
			if (_scroll_offset != old) invalidate();
		}

		void draw_scrollbars(render::draw_context& dc) const
		{
			const auto client = client_rect();
			_vscroll.draw(dc, client, _theme);
			_hscroll.draw(dc, client, _theme);
		}

		// Derived views put document pixels here.  Keeping the hook in the base lets
		// a view remain a normal control while its scroll chrome stays consistent.
		virtual void draw_view(render::draw_context& dc) const
		{
			(void)dc;
		}

	public:
		[[nodiscard]] const scrollbar& vert_scrollbar() const noexcept { return _vscroll; }
		[[nodiscard]] scrollbar& vert_scrollbar() noexcept { return _vscroll; }
		[[nodiscard]] const scrollbar& horiz_scrollbar() const noexcept { return _hscroll; }
		[[nodiscard]] scrollbar& horiz_scrollbar() noexcept { return _hscroll; }
		[[nodiscard]] core::pointi scroll_offset() const noexcept { return _scroll_offset; }
		void scroll_offset(const core::pointi offset) { scroll_to(offset); }
		[[nodiscard]] core::sizei view_extent() const noexcept { return _view_extent; }
		[[nodiscard]] core::sizei content_extent() const noexcept { return _content_extent; }

		core::sizei measure_content(render::measure_context& mc, int width_limit) const override
		{
			(void)mc;
			(void)width_limit;
			return {};
		}

		void paint(render::draw_context& dc, const bool focused) const override
		{
			(void)focused;
			if (_theme.window_background != 0)
				dc.draw_rect(client_rect(), core::color(_theme.window_background, dc.colors.alpha));
			draw_view(dc);
			draw_scrollbars(dc);
		}

		void on_placed(render::measure_context& mc) override
		{
			_view_extent = bounds.extent();
			_hscroll.set_dpi_scale(mc.scale_factor);
			_vscroll.set_dpi_scale(mc.scale_factor);
			clamp_scroll_offset();
		}

		void on_mouse_down(const core::pointi loc, const platform::key_state keys) override
		{
			(void)keys;
			const auto client = client_rect();
			if (_vscroll.handle_mouse_down(loc, client) || _hscroll.handle_mouse_down(loc, client))
			{
				if (auto* const h = host()) h->capture_pointer(true);
				invalidate();
			}
		}

		void on_mouse_move(const core::pointi loc, const bool captured) override
		{
			const auto client = client_rect();
			int position = 0;
			if (_vscroll._tracking)
			{
				if (_vscroll.handle_mouse_move(loc, client, position))
				{
					_scroll_offset.y = position;
					_vscroll.update(_content_extent.cy, _view_extent.cy, position);
					invalidate();
				}
				return;
			}
			if (_hscroll._tracking)
			{
				if (_hscroll.handle_mouse_move(loc, client, position))
				{
					_scroll_offset.x = position;
					_hscroll.update(_content_extent.cx, _view_extent.cx, position);
					invalidate();
				}
				return;
			}

			const auto changed = _vscroll.set_hover(_vscroll.hit_test(loc, client)) |
			                     _hscroll.set_hover(_hscroll.hit_test(loc, client));
			if (changed || captured) invalidate();
		}

		void on_mouse_up(const core::pointi loc, const platform::key_state keys) override
		{
			(void)keys;
			const auto client = client_rect();
			int position = 0;
			bool handled = false;
			if (_vscroll._tracking)
			{
				handled = _vscroll.handle_mouse_up(loc, client, position);
				_scroll_offset.y = position;
				_vscroll.update(_content_extent.cy, _view_extent.cy, position);
			}
			else if (_hscroll._tracking)
			{
				handled = _hscroll.handle_mouse_up(loc, client, position);
				_scroll_offset.x = position;
				_hscroll.update(_content_extent.cx, _view_extent.cx, position);
			}
			if (handled)
			{
				if (auto* const h = host()) h->capture_pointer(false);
				invalidate();
			}
		}

		void on_mouse_leave() override
		{
			_vscroll._hover_tracking = false;
			_hscroll._hover_tracking = false;
			if (_vscroll.set_hover(false) | _hscroll.set_hover(false)) invalidate();
		}

		bool on_wheel(const core::pointi loc, const int delta, const platform::key_state keys) override
		{
			(void)loc;
			if (delta == 0) return false;
			const auto before = _scroll_offset;
			if (keys.shift)
				_scroll_offset.x += delta;
			else
				_scroll_offset.y -= delta;
			clamp_scroll_offset();
			if (_scroll_offset == before) return false;
			invalidate();
			return true;
		}

		platform::cursor cursor_at(const core::pointi loc) const override
		{
			const auto client = client_rect();
			if (_vscroll.hit_test(loc, client)) return platform::cursor::up_down;
			if (_hscroll.hit_test(loc, client)) return platform::cursor::left_right;
			return platform::cursor::normal;
		}

		void scale_changed() override
		{
			const auto s = scale();
			_hscroll.set_dpi_scale(s);
			_vscroll.set_dpi_scale(s);
			invalidate();
		}
	};
}