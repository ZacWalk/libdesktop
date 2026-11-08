// This file is part of libdesktop, a C++20 platform layer for desktop applications
// Copyright 2026  Zac Walker
// 
// This program is free software; you can redistribute it and / or modify it
// under the terms of the LGPL License either version 2.1 or later.
// License details are available at https://www.gnu.org/licenses/lgpl-2.1.html
// This program is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY

// Purpose: The element tree's suite: flex layout, the scroller's track, thumb and anchors, and the
// controller an element host keeps - reused, escaped and bounded by the hit test that chose it. They
// began as Diffractor's, which keeps its own copies of them.

#include "libdesktop/ui.h"
#include "libdesktop/ui/elements.h"
#include "test.h"

using namespace std::literals;

namespace
{
	// Measures nothing, so a layout test observes only the arrangement the layout code chose rather than
	// the font metrics of whichever machine ran it.
	class flex_test_measure_context final : public render::measure_context
	{
	public:
		core::sizei measure_text(std::string_view text, render::font_face font, render::text_style style, int cx,
		                   int cy = 0) override
		{
			return {};
		}

		int text_line_height(render::font_face font) override
		{
			return 0;
		}

		render::text_layout_ptr create_text_layout(render::font_face font) override
		{
			return {};
		}
	};

	class flex_test_element final : public ui::element
	{
		core::sizei _desired;

	public:
		explicit flex_test_element(const core::sizei desired) : _desired(desired)
		{
			padding(0);
		}

		core::sizei measure(render::measure_context& mc, const int width_limit) const override
		{
			return {std::min(_desired.cx, width_limit), _desired.cy};
		}
	};

	// Behaves like text: content that does not fit the width limit wraps onto a second line.
	class flex_wrapping_test_element final : public ui::element
	{
		core::sizei _desired;

	public:
		explicit flex_wrapping_test_element(const core::sizei desired) : _desired(desired)
		{
			padding(0);
		}

		core::sizei measure(render::measure_context& mc, const int width_limit) const override
		{
			if (width_limit >= _desired.cx) return _desired;
			return {width_limit, _desired.cy * 2};
		}
	};

	// What every host below answers alike: no window, no commands and no menus.
	class windowless_test_host : public ui::element_host
	{
	public:
		const ui::frame_ptr frame() const override { return ui::no_frame(); }
		const ui::frame_ptr owner() override { return nullptr; }

		void on_window_layout(render::measure_context& mc, const core::sizei extent, bool is_minimized) override {}
		void on_window_paint(render::draw_context& dc) override {}
		void invoke(const commands cmd) override {}
		bool is_command_checked(const commands cmd) override { return false; }
		void track_menu(const core::recti bounds, const std::vector<ui::command_ptr>& commands) override {}
	};

	class scroller_test_host final : public std::enable_shared_from_this<scroller_test_host>,
	                                 public windowless_test_host
	{
	public:
		void controller_changed() override {}
		void invalidate_element(const ui::element_ptr& e) override {}
		void invalidate(const ui::host_invalid what) override {}

		ui::controller_ptr controller_from_location(core::pointi loc, ui::hit_test_context& ctx) override
		{
			return nullptr;
		}
	};

	// A host that has never been attached, or whose window has already been destroyed. Diffractor
	// 1.27.0 shipped a startup crash of exactly this shape: a panel borrowed another view's frame only
	// when it was visible, but kept populating, counting and invalidating while hidden.
	class detached_test_host final : public std::enable_shared_from_this<detached_test_host>,
	                                 public windowless_test_host
	{
	public:
		int controller_requests = 0;
		int controller_changes = 0;
		int invalidations = 0;

		void controller_changed() override { ++controller_changes; }
		void invalidate_element(const ui::element_ptr& e) override { ++invalidations; }
		void invalidate(const ui::host_invalid what) override { ++invalidations; }

		ui::controller_ptr controller_from_location(const core::pointi loc, ui::hit_test_context& ctx) override
		{
			++controller_requests;
			return nullptr;
		}
	};

	// A gesture that guards its own completion on having been cancelled, which every controller in the
	// tree does. What is under test is whether it is still there to be asked.
	class cancellable_test_controller final : public ui::controller
	{
	public:
		bool tracking = false;
		bool cancelled = false;
		int performed = 0;

		cancellable_test_controller(ui::element_host_ptr host, const core::recti bounds) :
			controller(std::move(host), bounds)
		{
		}

		void on_mouse_left_button_down(const core::pointi loc, const platform::key_state keys) override
		{
			controller::on_mouse_left_button_down(loc, keys);
			tracking = true;
		}

		void on_mouse_left_button_up(const core::pointi loc, const platform::key_state keys) override
		{
			if (!cancelled) ++performed;
			tracking = false;
		}

		bool escape() override
		{
			if (!tracking) return false;
			cancelled = true;
			return true;
		}
	};

	class cancelling_test_host final : public std::enable_shared_from_this<cancelling_test_host>,
	                                   public windowless_test_host
	{
	public:
		std::vector<std::shared_ptr<cancellable_test_controller>> built;

		void controller_changed() override {}
		void invalidate_element(const ui::element_ptr& e) override {}
		void invalidate(const ui::host_invalid what) override {}

		ui::controller_ptr controller_from_location(const core::pointi loc, ui::hit_test_context& ctx) override
		{
			auto controller = std::make_shared<cancellable_test_controller>(shared_from_this(), core::recti(0, 0, 200, 400));
			built.emplace_back(controller);
			return controller;
		}
	};

	class idle_escape_test_controller final : public ui::controller
	{
	public:
		bool escaped = false;

		idle_escape_test_controller(ui::element_host_ptr host, const core::recti bounds) :
			controller(std::move(host), bounds)
		{
		}

		bool escape() override
		{
			escaped = true;
			return true;
		}
	};

	class idle_escape_test_host final : public std::enable_shared_from_this<idle_escape_test_host>,
	                                    public windowless_test_host
	{
	public:
		std::shared_ptr<idle_escape_test_controller> built;

		void controller_changed() override {}
		void invalidate_element(const ui::element_ptr& e) override {}
		void invalidate(const ui::host_invalid what) override {}

		ui::controller_ptr controller_from_location(const core::pointi loc, ui::hit_test_context& ctx) override
		{
			built = std::make_shared<idle_escape_test_controller>(shared_from_this(), core::recti(0, 0, 200, 400));
			return built;
		}
	};

	// Answers one controller over a fixed region, having first recorded an occluder, so what the host
	// caches can be told apart from what the controller itself claims.
	class stability_test_host final : public std::enable_shared_from_this<stability_test_host>,
	                                  public windowless_test_host
	{
	public:
		core::recti answer_bounds{0, 0, 200, 400};
		core::recti occluder;
		int builds = 0;

		void controller_changed() override {}
		void invalidate_element(const ui::element_ptr& e) override {}
		void invalidate(const ui::host_invalid what) override {}

		ui::controller_ptr controller_from_location(const core::pointi loc, ui::hit_test_context& ctx) override
		{
			++builds;
			ctx.occluded(occluder);
			return std::make_shared<cancellable_test_controller>(shared_from_this(), answer_bounds);
		}
	};

	// An element that answers for part of itself. can_invoke is a style bit rather than a virtual, so
	// the bits are set rather than overridden.
	class invoke_counting_element final : public std::enable_shared_from_this<invoke_counting_element>,
	                                      public ui::element
	{
	public:
		int invokes = 0;

		invoke_counting_element()
		{
			bounds = core::recti(0, 0, 100, 100);
			is_visible(true);
			set_style_bit(ui::element_style::can_invoke, true);
		}

		void dispatch_event(const ui::element_event& event) override
		{
			if (event.type == ui::element_event_type::invoke) ++invokes;
		}

		ui::controller_ptr controller_from_location(const ui::element_host_ptr& host, const core::pointi loc,
		                                            const core::pointi element_offset, ui::hit_test_context& ctx) override
		{
			return ui::default_controller_from_location(*this, host, loc, element_offset, ctx);
		}
	};

	// Holds one element and moves it, as a scrolled panel moves what it holds; answers no pointer itself.
	class holding_element final : public ui::element
	{
	public:
		ui::element_ptr held;
		core::pointi moved;

		void visit_children(const std::function<void(const ui::element_ptr&)>& visit) const override
		{
			visit(held);
		}

		core::pointi children_offset() const override
		{
			return moved;
		}
	};
}

static void should_layout_flex_elements()
{
	using namespace ui;

	flex_test_measure_context mc;
	auto first = std::make_shared<flex_test_element>(core::sizei{40, 10});
	auto second = std::make_shared<flex_test_element>(core::sizei{40, 20});
	auto third = std::make_shared<flex_test_element>(core::sizei{40, 15});
	const std::vector<element_ptr> wrapping{first, second, third};
	const auto wrapped = calc_flex_layout(wrapping, mc, {100, 0}, {});
	assert_equal(5, wrapped.layout_bounds[0].top, "short item is centered on first flex line");
	assert_equal(0, wrapped.layout_bounds[1].top, "second item remains on first flex line");
	assert_equal(20, wrapped.layout_bounds[2].top, "third item wraps to second flex line");
	assert_equal(35, wrapped.extent.cy, "wrapped flex extent");

	first->flex.grow = 1.0f;
	second->flex.grow = 1.0f;
	const std::vector<element_ptr> growing{first, second};
	const auto grown = calc_flex_layout(growing, mc, {100, 0}, {});
	assert_equal(50, grown.layout_bounds[0].width(), "first flex item shares free space");
	assert_equal(50, grown.layout_bounds[1].width(), "second flex item shares free space");

	first->flex.grow = 0.0f;
	second->flex.grow = 0.0f;
	second->flex.main_start_auto = true;
	const auto justified = calc_flex_layout(growing, mc, {100, 0}, {});
	assert_equal(60, justified.layout_bounds[1].left, "auto margin right aligns trailing flex item");

	second->is_visible(false);
	const auto hidden = calc_flex_layout(growing, mc, {100, 0}, {});
	assert_equal(40, hidden.extent.cx, "hidden flex item consumes no space");
	assert_equal(true, hidden.layout_bounds[1] == core::recti{}, "hidden flex item has empty bounds");

	second->is_visible(true);
	first->flex.shrink = 1.0f;
	second->flex.shrink = 1.0f;
	first->flex.basis = 60;
	second->flex.basis = 60;
	first->flex.min_size.cx = 55;
	second->flex.min_size.cx = 40;
	flex_container_layout no_wrap;
	no_wrap.wrap = flex_wrap::no_wrap;
	const auto shrunk = calc_flex_layout(growing, mc, {100, 0}, no_wrap);
	assert_equal(55, shrunk.layout_bounds[0].width(), "flex shrink respects first minimum");
	assert_equal(45, shrunk.layout_bounds[1].width(), "flex shrink redistributes after minimum");

	auto column_first = std::make_shared<flex_test_element>(core::sizei{30, 10});
	auto column_second = std::make_shared<flex_test_element>(core::sizei{40, 20});
	const std::vector<element_ptr> column_elements{column_first, column_second};
	flex_container_layout column;
	column.direction = flex_direction::column;
	column.wrap = flex_wrap::no_wrap;
	const auto measured_column = calc_flex_layout(column_elements, mc, {100, -1}, column);
	assert_equal(30, measured_column.extent.cy, "unconstrained flex column measures intrinsic height");
	assert_equal(10, measured_column.layout_bounds[1].top, "unconstrained flex column stacks items");

	mc.padding2 = 8;
	auto divider = std::make_shared<divider_element>();
	divider->padding(0);
	const std::vector<element_ptr> divided_column{divider, column_second};
	const auto fixed_height_divider = calc_flex_layout(divided_column, mc, {100, 100}, column);
	assert_equal(8, fixed_height_divider.layout_bounds[0].height(),
	             "column divider keeps its intrinsic height instead of consuming free space");
	assert_equal(8, fixed_height_divider.layout_bounds[1].top,
	             "control after a column divider remains adjacent and visible");
	mc.padding2 = 0;

	column_first->flex.break_after = true;
	const auto broken_column = calc_flex_layout(column_elements, mc, {100, -1}, column);
	assert_equal(30, broken_column.layout_bounds[1].left,
	             "explicit break creates a new column when automatic wrapping is disabled");

	column_first->flex.break_after = false;
	column_first->flex.align_self = flex_align::stretch;
	column.padding = {5, 7};
	const auto padded_column = calc_flex_layout(column_elements, mc, {100, -1}, column);
	assert_equal(5, padded_column.layout_bounds[0].left, "column padding offsets the first item");
	assert_equal(90, padded_column.layout_bounds[0].width(), "explicit stretch fills the column cross axis");
	assert_equal(44, padded_column.extent.cy, "column padding contributes to intrinsic extent");

	mc.scale_factor = 2.0;
	const auto scaled_padded_column = calc_flex_layout(column_elements, mc, {200, -1}, column);
	assert_equal(10, scaled_padded_column.layout_bounds[0].left, "flex padding scales from logical units");
	assert_equal(58, scaled_padded_column.extent.cy, "scaled flex padding contributes once to intrinsic extent");
	mc.scale_factor = 1.0;

	const auto applied_extent = layout_flex_elements(column_elements, mc, {10, 20, 110, 80}, column);
	assert_equal(15, column_first->bounds.left, "applied flex layout offsets child bounds");
	assert_equal(padded_column.extent.cy, applied_extent.cy, "applied flex layout returns content extent");

	column.padding = {};
	column.justify = flex_justify::center;
	const auto centered_column = calc_flex_layout(column_elements, mc, {100, 100}, column);
	assert_equal(35, centered_column.layout_bounds[0].top, "flex column centers intrinsic content vertically");

	column.justify = flex_justify::start;
	column_first->flex.align_self = flex_align::automatic;
	column_first->flex = column_first->flex | flex_item::media;
	column_first->flex.basis = 100;
	const auto shrunk_column = calc_flex_layout(column_elements, mc, {100, 100}, column);
	assert_equal(80, shrunk_column.layout_bounds[0].height(), "flex column shrinks media to fit trailing controls");
	assert_equal(80, shrunk_column.layout_bounds[1].top, "trailing control remains visible after media shrink");

	auto fixed = std::make_shared<flex_test_element>(core::sizei{30, 10});
	auto flexible_first = std::make_shared<flex_test_element>(core::sizei{40, 10});
	auto flexible_second = std::make_shared<flex_test_element>(core::sizei{40, 10});
	fixed->flex.basis = 30;
	fixed->flex.shrink = 1.0f;
	flexible_first->flex.basis = 0;
	flexible_first->flex.grow = 1.0f;
	flexible_second->flex.basis = 0;
	flexible_second->flex.grow = 1.0f;
	flex_container_layout columns;
	columns.wrap = flex_wrap::no_wrap;
	columns.gap.cx = 5;
	const auto column_row = calc_flex_layout(
		std::vector<element_ptr>{fixed, flexible_first, flexible_second}, mc, {100, -1}, columns);
	assert_equal(30, column_row.layout_bounds[0].width(), "fixed flex column keeps its basis");
	assert_equal(30, column_row.layout_bounds[1].width(), "first flexible column shares remaining width");
	assert_equal(30, column_row.layout_bounds[2].width(), "second flexible column shares remaining width");

	auto capped = std::make_shared<flex_wrapping_test_element>(core::sizei{40, 10});
	capped->flex.max_size.cx = 25;
	capped->flex.align_self = flex_align::center;
	const std::vector<element_ptr> capped_elements{capped};
	const auto capped_column = calc_flex_layout(capped_elements, mc, {100, -1}, column);
	assert_equal(25, capped_column.layout_bounds[0].width(), "column item is capped by its maximum width");
	assert_equal(20, capped_column.extent.cy, "column item measures inside its maximum width");

	const auto capped_row = calc_flex_layout(capped_elements, mc, {100, -1}, {});
	assert_equal(25, capped_row.layout_bounds[0].width(), "row item is capped by its maximum width");
	assert_equal(20, capped_row.extent.cy, "row item measures inside its maximum width");

	auto padded_first = std::make_shared<flex_test_element>(core::sizei{20, 10});
	auto padded_second = std::make_shared<flex_test_element>(core::sizei{20, 10});
	flex_container_layout centered_row;
	centered_row.wrap = flex_wrap::no_wrap;
	centered_row.justify = flex_justify::center;
	centered_row.padding = {10, 0};
	const auto centered = calc_flex_layout(std::vector<element_ptr>{padded_first, padded_second}, mc,
	                                       {100, -1}, centered_row);
	assert_equal(30, centered.layout_bounds[0].left, "centred line keeps the container's leading padding");
	assert_equal(100, centered.extent.cx, "a justified line reports the whole main axis it positions within");

	auto capped_cross = std::make_shared<flex_test_element>(core::sizei{20, 10});
	capped_cross->flex.align_self = flex_align::stretch;
	capped_cross->flex.max_size.cy = 12;
	auto tall = std::make_shared<flex_test_element>(core::sizei{20, 40});
	flex_container_layout stretch_row;
	stretch_row.wrap = flex_wrap::no_wrap;
	const auto stretched = calc_flex_layout(std::vector<element_ptr>{capped_cross, tall}, mc, {100, -1},
	                                        stretch_row);
	assert_equal(12, stretched.layout_bounds[0].height(), "stretch respects the item's maximum cross size");

	auto only_child = std::make_shared<flex_test_element>(core::sizei{20, 10});
	only_child->is_visible(false);
	flex_container_layout hidden_column;
	hidden_column.direction = flex_direction::column;
	hidden_column.padding = {10, 10};
	const auto nothing_visible = calc_flex_layout(std::vector<element_ptr>{only_child}, mc, {100, -1},
	                                              hidden_column);
	assert_equal(0, nothing_visible.extent.cx, "a container with nothing visible occupies no width");
	assert_equal(0, nothing_visible.extent.cy, "a container with nothing visible occupies no height");
}

static void should_preserve_view_scroller_anchor_across_layout()
{
	ui::scroller scroller;
	constexpr core::recti client_bounds(0, 0, 100, 200);
	constexpr core::recti scroll_bounds(90, 0, 100, 200);
	const auto element = std::make_shared<ui::text_element>("anchor");
	element->bounds = {0, 350, 100, 450};

	scroller.layout({100, 1000}, client_bounds, scroll_bounds);
	scroller._offset.y = 300;
	const auto anchor = scroller.capture_anchor(element);
	assert_equal(50, anchor.device_top, "scroll anchor captures device position");

	element->bounds = {0, 500, 100, 600};
	scroller.layout({100, 1200}, client_bounds, scroll_bounds);
	assert_equal(450, scroller.anchor_offset(anchor, true), "current element preserves device position");
	assert_equal(375, scroller.anchor_offset(anchor, false), "replaced element preserves scroll ratio");

	scroller._offset.y = 0;
	const auto top_anchor = scroller.capture_anchor(element);
	element->bounds = {0, 700, 100, 800};
	assert_equal(0, scroller.anchor_offset(top_anchor, true), "top remains top after layout");
}

static void should_map_the_view_scroller_track_to_the_list()
{
	ui::scroller scroller;
	constexpr core::recti client_bounds(0, 0, 100, 200);
	constexpr core::recti scroll_bounds(90, 0, 100, 200);
	scroller.layout({100, 1000}, client_bounds, scroll_bounds);

	// The section bands are placed with logical_to_scrollbar_pos, so it must land on the track the
	// thumb actually travels rather than on the client height.
	const auto track = scroller.track_bounds();
	assert_equal(true, track == core::recti(90, 2, 100, 198), "scroll track excludes its own edge inset");
	assert_equal(track.height(), scroller.logical_to_scrollbar_pos(1000), "list end maps to track end");
	assert_equal(1000, scroller.scrollbar_pos_to_logical(track.bottom), "track end maps to list end");
	assert_equal(0, scroller.scrollbar_pos_to_logical(track.top), "track start maps to list start");
}

static void should_drag_view_scroller_thumb_from_grab_point()
{
	ui::scroller scroller;
	scroller.layout({100, 1000}, core::recti(0, 0, 100, 200), core::recti(90, 0, 100, 200));
	scroller._offset.y = 400;

	const auto track = scroller.track_bounds();
	const auto thumb = scroller.thumb_bounds();

	assert_equal(track.top + scroller.logical_to_scrollbar_pos(400), scroller.thumb_origin(),
	             "thumb origin follows the scroll position");
	assert_equal(true, thumb.top <= scroller.thumb_origin() && thumb.bottom > scroller.thumb_origin(),
	             "painted thumb covers its origin");

	// A drag holds the point it grabbed: releasing without moving must leave the thumb where it
	// was, rather than re-centring it on the cursor as an unheld press on bare track does.
	const auto host = std::make_shared<scroller_test_host>();
	const auto grab = scroller.thumb_origin() + 3;
	ui::scroll_controller held(host, scroller, scroller.scroll_bounds());
	held.on_mouse_left_button_down({95, grab}, {});
	held.on_mouse_left_button_up({95, grab}, {});
	assert_equal(400, scroller.scroll_offset().y, "grabbing the thumb does not move it");

	const auto expected = scroller.scrollbar_pos_to_logical(scroller.thumb_origin() + 10);
	ui::scroll_controller dragged(host, scroller, scroller.scroll_bounds());
	dragged.on_mouse_left_button_down({95, grab}, {});
	dragged.on_mouse_move({95, grab + 10});
	dragged.on_mouse_left_button_up({95, grab + 10}, {});
	assert_equal(expected, scroller.scroll_offset().y, "grab offset is preserved while dragging");
}

static void should_close_view_scroller_bands_over_track()
{
	ui::scroller scroller;
	scroller.layout({100, 1000}, core::recti(0, 0, 100, 200), core::recti(90, 0, 100, 200));
	scroller.sections({{"a", icon_index{}, 400}, {"b", icon_index{}, 700}});

	const auto track_height = scroller.track_bounds().height();
	std::vector<std::pair<int, int>> bands;
	auto labelled = 0;

	scroller.for_each_band([&](const int top, const int bottom, const ui::scroller_section* so)
	{
		bands.emplace_back(top, bottom);
		if (so) ++labelled;
	});

	assert_equal(3u, static_cast<unsigned>(bands.size()), "sections plus a closing band");
	assert_equal(2, labelled, "only the sections carry labels");
	assert_equal(0, bands.front().first, "bands start at the track");
	assert_equal(track_height, bands.back().second, "bands close off the track");
}

static void should_survive_a_host_with_no_window()
{
	const auto host = std::make_shared<detached_test_host>();
	host->_extent = {200, 400};

	// Every entry point element_host offers, in the order a real pointer session reaches them. Each
	// one dereferences frame() unconditionally, so a null there is an access violation.
	host->on_mouse_move({10, 10}, false);
	host->on_mouse_left_button_down({10, 10}, {});
	host->on_mouse_left_button_up({10, 10}, {});
	host->on_mouse_middle_button_down({10, 10}, {});
	host->on_mouse_middle_button_up({10, 10}, {});
	host->on_mouse_left_button_double_click({10, 10}, {});
	host->on_mouse_leave({10, 10});
	host->update_cursor();
	host->show_cursor(false);
	host->invalidate_element(nullptr);

	assert_equal(false, host->escape_controller(), "nothing to escape without a controller");
	assert_equal(false, host->key_down_controller(U'a', {}), "no controller claims a key");
	assert_equal(true, host->controller_requests > 0, "hit testing still ran");

	// Scrolling asks the host for its window twice: once to shift the pixels, once to repaint.
	ui::scroller scroller;
	scroller.layout({200, 4000}, core::recti(0, 0, 190, 400), core::recti(190, 0, 200, 400));
	scroller.scroll_offset(host, 0, 500);
	scroller.offset(host, 0, 120);
	assert_equal(true, scroller.scroll_offset().y > 0, "the scroller still tracks its position");
}

// Escape cancels a gesture the button is still holding, so the release belongs to the controller
// that was cancelled. Replacing it at the moment of the cancel handed the release to a fresh one
// with no memory of it, which then performed the gesture Escape had just refused.
static void should_not_perform_a_cancelled_gesture_on_release()
{
	const auto host = std::make_shared<cancelling_test_host>();
	host->_extent = {200, 400};

	host->on_mouse_move({10, 10}, false);
	host->on_mouse_left_button_down({10, 10}, {});
	assert_equal(1u, static_cast<unsigned>(host->built.size()), "the press latched one controller");

	assert_equal(true, host->escape_controller(), "escape cancelled the gesture");
	assert_equal(1u, static_cast<unsigned>(host->built.size()), "the cancelled controller was not replaced");
	assert_equal(true, host->built.front()->cancelled, "the cancel reached the controller holding the button");

	host->on_mouse_left_button_up({12, 12}, {});
	assert_equal(0, host->built.front()->performed, "the release did not perform the cancelled gesture");
}

static void should_offer_escape_to_an_idle_controller()
{
	const auto host = std::make_shared<idle_escape_test_host>();
	host->_extent = {200, 400};
	host->on_mouse_move({10, 10}, false);
	host->on_mouse_move({10, 10}, false);
	assert_equal(true, host->escape_controller(), "escape reaches the active controller");
	assert_equal(true, host->built->escaped, "controller consumed escape");
}

// A controller is cached for its own region narrowed by everything the hit test passed over on the
// way to it. Neither half of that is the controller's to decide, and a tool the walk stepped over
// stays reachable only because the pointer reaching it forces a fresh test.
static void should_bound_a_reused_controller_by_what_it_passed_over()
{
	const auto host = std::make_shared<stability_test_host>();
	host->_extent = {200, 400};
	host->occluder = core::recti(120, 0, 160, 400);

	host->on_mouse_move({40, 40}, false);
	assert_equal(1, host->builds, "the first move ran one hit test");
	assert_equal(true, host->_controller_bounds.contains(core::pointi(40, 40)), "the cached region covers the pointer");
	assert_equal(120, host->_controller_bounds.right, "the cached region stops at what was passed over");

	host->on_mouse_move({60, 60}, false);
	assert_equal(1, host->builds, "moving inside the cached region reused the controller");

	host->on_mouse_move({130, 60}, false);
	assert_equal(2, host->builds, "moving onto the passed-over region forced a fresh test");
}

// The controller's own region is never narrowed to describe what covers it: an element wider than
// the answer it is cached for still owns all of itself.
static void should_not_clip_a_controller_to_what_covers_it()
{
	const auto host = std::make_shared<stability_test_host>();
	host->_extent = {200, 400};
	host->answer_bounds = core::recti(0, 0, 200, 400);
	host->occluder = core::recti(120, 0, 160, 400);

	host->on_mouse_move({40, 40}, false);

	assert_equal(200, host->_active_controller->bounds().right, "the controller kept its own region");
	assert_equal(120, host->_controller_bounds.right, "only the cached region was narrowed");
}

// The regression the hit-test split fixes: the covering rect used to be subtracted from the
// control's own bounds, and a release is only a click when it lands inside those. Clicking the
// covered half of a control therefore did nothing at all.
static void should_keep_a_partly_covered_control_clickable()
{
	const auto host = std::make_shared<detached_test_host>();
	host->_extent = {200, 400};

	const auto element = std::make_shared<invoke_counting_element>();

	ui::hit_test_context ctx{{20, 20}, core::recti(0, 0, 200, 400)};
	ctx.occluded(core::recti(60, 0, 100, 100));

	const auto controller = element->controller_from_location(host, {20, 20}, {}, ctx);
	assert_equal(true, controller != nullptr, "the control answered");
	assert_equal(100, controller->bounds().right, "the controller kept the whole control");
	assert_equal(60, ctx.stability().right, "the cover narrowed only what the answer is reused for");

	controller->on_mouse_left_button_down({70, 50}, {});
	controller->on_mouse_left_button_up({70, 50}, {});
	assert_equal(1, element->invokes, "a click on the covered half still invoked");
}

// An ordinary clickable control had no escape of its own, so a press the user backed out of still
// invoked on release - and Escape is exactly the key that may have changed the view the click was
// made in, so the action landed somewhere the user was no longer looking.
static void should_not_invoke_an_escaped_click()
{
	const auto host = std::make_shared<detached_test_host>();
	host->_extent = {200, 400};

	const auto element = std::make_shared<invoke_counting_element>();
	ui::hit_test_context ctx{{20, 20}, core::recti(0, 0, 200, 400)};
	const auto controller = element->controller_from_location(host, {20, 20}, {}, ctx);
	assert_equal(true, controller != nullptr, "the control answered");

	controller->on_mouse_left_button_down({20, 20}, {});
	assert_equal(true, controller->escape(), "escape claimed the held press");

	controller->on_mouse_left_button_up({20, 20}, {});
	assert_equal(0, element->invokes, "the release did not invoke the escaped click");

	// The control is not left dead: the next press works as it always did.
	controller->on_mouse_left_button_down({20, 20}, {});
	controller->on_mouse_left_button_up({20, 20}, {});
	assert_equal(1, element->invokes, "a fresh press still invokes");

	assert_equal(false, controller->escape(), "escape claims nothing when no button is held");
}

// A region containing the pointer stops the walk; one that does not is cut away instead. Confining
// is neither: it narrows to a region already chosen, whichever side of the pointer it lies.
static void should_separate_occlusion_from_exclusion()
{
	ui::hit_test_context ctx{{50, 50}, core::recti(0, 0, 200, 400)};

	assert_equal(false, ctx.occluded(core::recti{}), "an empty region is not in the way");
	assert_equal(200, ctx.stability().right, "an empty region cuts nothing");

	assert_equal(false, ctx.occluded(core::recti(120, 0, 160, 400)), "a region beside the pointer is not in the way");
	assert_equal(120, ctx.stability().right, "a region beside the pointer is cut away");

	assert_equal(true, ctx.occluded(core::recti(40, 40, 60, 60)), "a region under the pointer is in the way");
	assert_equal(120, ctx.stability().right, "a region under the pointer cuts nothing, having stopped the walk");

	ctx.confine(core::recti(0, 0, 80, 400));
	assert_equal(80, ctx.stability().right, "confining narrows to the chosen region");
	assert_equal(true, ctx.stability().contains(core::pointi(50, 50)), "and never cuts the pointer out");
}

// An element that holds others hands the pointer to them, as it draws them and offers them the wheel, so a
// composite that adds nothing of its own - a search box over its results - needs no hit testing to pass it on.
static void should_hand_the_pointer_to_what_an_element_holds()
{
	const auto host = std::make_shared<detached_test_host>();
	host->_extent = {200, 400};

	const auto held = std::make_shared<invoke_counting_element>();
	const auto holder = std::make_shared<holding_element>();
	holder->held = held;
	holder->bounds = core::recti(0, 0, 200, 200);
	holder->moved = {10, 10};

	ui::hit_test_context ctx{{50, 50}, core::recti(0, 0, 200, 400)};
	const auto controller = holder->controller_from_location(host, {50, 50}, {}, ctx);
	assert_equal(true, controller != nullptr, "what it holds answers");
	controller->on_mouse_left_button_down({50, 50}, {});
	controller->on_mouse_left_button_up({50, 50}, {});
	assert_equal(1, held->invokes, "and is invoked");

	assert_equal(true, holder->is_control_area({50, 50}, {}), "a press there is a control's");
	assert_equal(false, holder->is_control_area({5, 5}, {}), "not before what it holds, moved as it moves it");
	assert_equal(false, holder->is_control_area({150, 150}, {}), "nor beside it");

	ui::hit_test_context moved_ctx{{5, 5}, core::recti(0, 0, 200, 400)};
	assert_equal(true, holder->controller_from_location(host, {5, 5}, {}, moved_ctx) == nullptr,
	             "where it moved what it holds away from, nothing answers");

	held->is_visible(false);
	ui::hit_test_context hidden_ctx{{50, 50}, core::recti(0, 0, 200, 400)};
	assert_equal(true, holder->controller_from_location(host, {50, 50}, {}, hidden_ctx) == nullptr,
	             "a hidden element answers nothing");
	assert_equal(false, holder->is_control_area({50, 50}, {}), "and is no control's ground");
}

void register_render_elements_tests(test_registry& tests)
{
	tests.add("Should layout flex elements"s, should_layout_flex_elements);
	tests.add("Should preserve view scroller anchor across layout"s, should_preserve_view_scroller_anchor_across_layout);
	tests.add("Should map the view scroller track to the list"s, should_map_the_view_scroller_track_to_the_list);
	tests.add("Should drag view scroller thumb from grab point"s, should_drag_view_scroller_thumb_from_grab_point);
	tests.add("Should close view scroller bands over track"s, should_close_view_scroller_bands_over_track);
	tests.add("Should survive a host with no window"s, should_survive_a_host_with_no_window);
	tests.add("Should not perform a cancelled gesture on release"s, should_not_perform_a_cancelled_gesture_on_release);
	tests.add("Should offer escape to an idle controller"s, should_offer_escape_to_an_idle_controller);
	tests.add("Should bound a reused hit test controller by what it passed over"s,
	          should_bound_a_reused_controller_by_what_it_passed_over);
	tests.add("Should not clip a hit test controller to what covers it"s, should_not_clip_a_controller_to_what_covers_it);
	tests.add("Should keep a partly covered control clickable"s, should_keep_a_partly_covered_control_clickable);
	tests.add("Should not invoke an escaped click"s, should_not_invoke_an_escaped_click);
	tests.add("Should separate hit test occlusion from exclusion"s, should_separate_occlusion_from_exclusion);
	tests.add("Should hand the pointer to what an element holds"s, should_hand_the_pointer_to_what_an_element_holds);
}
