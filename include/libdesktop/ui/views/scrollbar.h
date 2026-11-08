// This file is part of libdesktop, a C++20 platform layer for desktop applications
// Copyright 2026  Zac Walker
//
// This program is free software; you can redistribute it and / or modify it
// under the terms of the LGPL License either version 2.1 or later.
// License details are available at https://www.gnu.org/licenses/lgpl-2.1.html
// This program is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY

// Purpose: The pixel scrollbar used by portable text views and split controls.

#pragma once

#include "libdesktop/ui.h"

#include <algorithm>

namespace ui
{
	class view_theme;

	// A view-local scrollbar.  It owns only geometry and interaction state; its owner
	// supplies the client rectangle and applies the position through scroll_offset().
	class scrollbar
	{
	public:
		enum class orientation
		{
			vertical,
			horizontal,
		};

		orientation _orient = orientation::vertical;
		int _content_size = 0;
		int _page_size = 0;
		int _position = 0;

		bool _hover = false;
		bool _tracking = false;
		bool _hover_tracking = false;
		int _tracking_start_mouse = 0;
		int _tracking_start_pos = 0;

		static constexpr int base_thumb_thickness = 8;
		static constexpr int base_edge_margin = 4;
		static constexpr int base_hover_track_width = 26;
		static constexpr int base_hit_margin = 32;

		double _dpi_scale = 1.0;

		constexpr scrollbar() noexcept = default;
		constexpr explicit scrollbar(const orientation o) noexcept : _orient(o)
		{
		}

		[[nodiscard]] int thumb_thickness() const noexcept
		{
			return static_cast<int>(base_thumb_thickness * _dpi_scale);
		}

		[[nodiscard]] int edge_margin() const noexcept
		{
			return static_cast<int>(base_edge_margin * _dpi_scale);
		}

		[[nodiscard]] int hover_track_width() const noexcept
		{
			return static_cast<int>(base_hover_track_width * _dpi_scale);
		}

		[[nodiscard]] int hit_margin() const noexcept
		{
			return static_cast<int>(base_hit_margin * _dpi_scale);
		}

		void set_dpi_scale(double scale) noexcept
		{
			_dpi_scale = scale;
		}

		[[nodiscard]] bool can_scroll() const noexcept
		{
			return _content_size > _page_size && _page_size > 0;
		}

		void update(int content_size, int page_size, int position) noexcept
		{
			_content_size = content_size;
			_page_size = page_size;
			_position = position;
		}

		struct thumb_span
		{
			int start = 0;
			int length = 0;
		};

		// The minimum thumb length must be included before the position is mapped: otherwise
		// the last page can put the thumb beyond the end of a short track.
		[[nodiscard]] thumb_span thumb(int track_start, int track_extent) const noexcept
		{
			if (!can_scroll() || track_extent <= 0) return {track_start, 0};

			const auto length = std::max(core::mul_div(_page_size, track_extent, _content_size), thumb_thickness());
			const auto limit = std::max(track_start, track_start + track_extent - length);
			const auto start = std::clamp(core::mul_div(_position, track_extent, _content_size) + track_start,
			                             track_start, limit);
			return {start, std::min(length, track_extent)};
		}

		[[nodiscard]] core::recti thumb_bounds(core::recti client) const noexcept
		{
			if (_orient == orientation::vertical)
			{
				const auto span = thumb(client.top, client.height());
				return {client.right - thumb_thickness() - edge_margin(), span.start,
				        client.right - edge_margin(), span.start + span.length};
			}

			const auto span = thumb(client.left, client.width());
			return {span.start, client.bottom - thumb_thickness() - edge_margin(),
			        span.start + span.length, client.bottom - edge_margin()};
		}

		[[nodiscard]] bool hit_test(core::pointi point, core::recti client) const noexcept
		{
			if (!can_scroll() || client.is_empty()) return false;
			if (_orient == orientation::vertical)
				return point.x >= client.right - hit_margin() && point.y >= client.top && point.y < client.bottom;
			return point.y >= client.bottom - hit_margin() && point.x >= client.left && point.x < client.right;
		}

		bool begin_tracking(core::pointi point, core::recti client) noexcept;
		int track_to(core::pointi point, core::recti client) const noexcept;
		void end_tracking() noexcept { _tracking = false; }
		bool set_hover(bool hover) noexcept;

		// These helpers are deliberately window-free: view_base owns capture and invalidation, and the
		// scrollbar only the arithmetic.
		bool handle_mouse_down(core::pointi point, core::recti client) noexcept;
		bool handle_mouse_move(core::pointi point, core::recti client, int& new_position) noexcept;
		bool handle_mouse_up(core::pointi point, core::recti client, int& new_position) noexcept;

		void draw(render::draw_context& dc, core::recti client, const view_theme& theme) const;
		void draw(render::draw_context& dc, core::recti client) const;
	};
}