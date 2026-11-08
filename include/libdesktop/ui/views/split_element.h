// This file is part of libdesktop, a C++20 platform layer for desktop applications
// Copyright 2026  Zac Walker
//
// This program is free software; you can redistribute it and / or modify it
// under the terms of the LGPL License either version 2.1 or later.
// License details are available at https://www.gnu.org/licenses/lgpl-2.1.html
// This program is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY

// Purpose: A two-pane flex-tree element with a draggable splitter bar.

#pragma once

#include "libdesktop/ui/elements.h"

namespace ui
{
	class view_theme;

	class split_element final : public elements
	{
	public:
		enum class orientation
		{
			vertical,
			horizontal,
		};

		static constexpr int base_bar_width = 5;
		static constexpr double min_ratio = 0.05;
		static constexpr double max_ratio = 0.95;
		static constexpr int min_pane_size = 24;

		orientation _orient = orientation::vertical;
		double _ratio = 0.5;
		bool _tracking = false;
		bool _hover = false;
		double _dpi_scale = 1.0;

		// The panes remain ordinary elements, so controls beneath either side are
		// still found by the frame's normal tree walk.
		element_ptr _first;
		element_ptr _second;
		const view_theme* _theme = nullptr;

		split_element(element_ptr first, element_ptr second,
		              orientation orient = orientation::vertical, double initial_ratio = 0.5,
		              const view_theme* theme = nullptr) noexcept;
		split_element(orientation orient, double initial_ratio = 0.5,
		              element_ptr first = {}, element_ptr second = {},
		              const view_theme* theme = nullptr) noexcept;
		split_element(element_ptr first, element_ptr second,
		              orientation orient, double initial_ratio, const view_theme& theme) noexcept;
		split_element(orientation orient, double initial_ratio, element_ptr first,
		              element_ptr second, const view_theme& theme) noexcept;
		split_element(orientation orient, double initial_ratio, const view_theme& theme) noexcept;

		[[nodiscard]] int bar_width() const noexcept;
		void set_dpi_scale(double scale) noexcept;

		[[nodiscard]] int split_pos(core::recti bounds) const noexcept;
		[[nodiscard]] core::recti bar_rect(core::recti bounds) const noexcept;
		[[nodiscard]] bool hit_test(core::recti bounds, core::pointi point) const noexcept;
		void update_ratio(core::recti bounds, core::pointi point) noexcept;
		bool begin_tracking(core::recti bounds, core::pointi point) noexcept;
		bool track_to(core::recti bounds, core::pointi point) noexcept;
		void end_tracking() noexcept;
		bool update_hover(core::recti bounds, core::pointi point) noexcept;
		void clear_hover() noexcept;

		[[nodiscard]] core::color32 color() const noexcept;
		[[nodiscard]] platform::cursor cursor() const noexcept;
		void draw(render::draw_context& dc, core::recti bounds_in) const;

		[[nodiscard]] double ratio() const noexcept { return _ratio; }
		void ratio(double value) noexcept;
		[[nodiscard]] orientation split_orientation() const noexcept { return _orient; }
		[[nodiscard]] const element_ptr& first() const noexcept { return _first; }
		[[nodiscard]] const element_ptr& second() const noexcept { return _second; }
		void children(element_ptr first, element_ptr second) noexcept;

		core::sizei measure(render::measure_context& mc, int width_limit) const override;
		void layout(render::measure_context& mc, core::recti bounds_in) override;
		void render(render::draw_context& dc, core::pointi element_offset) const override;
		void visit_children(const std::function<void(const element_ptr&)>& visit) const override;
		controller_ptr controller_from_location(const element_host_ptr& host, core::pointi loc,
		                                        core::pointi element_offset, hit_test_context& ctx) override;
	};
}