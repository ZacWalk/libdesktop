// This file is part of libdesktop, a C++20 platform layer for desktop applications
// Copyright 2026  Zac Walker
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

// Purpose: The element tree's suite: flex layout, the scroller's track, thumb and anchors, and the
// controller an element host keeps - reused, escaped and bounded by the hit test that chose it. They
// began as Diffractor's, which keeps its own copies of them.

#include "libdesktop/ui.h"
#include "libdesktop/ui/elements.h"
#include "libdesktop/ui/table_element.h"
#include "libdesktop/ui/test_support.h"
#include "test.h"

using namespace std::literals;

namespace
{
	// Measures nothing, so a layout test observes only the arrangement the layout code chose rather than
	// the font metrics of whichever machine ran it.
	class flex_test_measure_context final : public render::measure_context
	{
	public:
		core::sizei measure_text(std::string_view /*text*/, render::font_face /*font*/, render::text_style /*style*/,
		                         int /*cx*/, int /*cy*/ = 0) override
		{
			return {};
		}

		int text_line_height(render::font_face /*font*/) override { return 0; }

		render::text_layout_ptr create_text_layout(render::font_face /*font*/) override { return {}; }
	};

	class flex_test_element final : public ui::element
	{
		core::sizei _desired;

	public:
		mutable int measure_calls = 0;

		explicit flex_test_element(const core::sizei desired) : _desired(desired) { padding(0); }

		core::sizei measure(render::measure_context& /*mc*/, const int width_limit) const override
		{
			++measure_calls;
			return {std::min(_desired.cx, width_limit), _desired.cy};
		}
	};

	// Behaves like text: content that does not fit the width limit wraps onto a second line.
	class flex_wrapping_test_element final : public ui::element
	{
		core::sizei _desired;

	public:
		explicit flex_wrapping_test_element(const core::sizei desired) : _desired(desired) { padding(0); }

		core::sizei measure(render::measure_context& /*mc*/, const int width_limit) const override
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

		void on_window_layout(render::measure_context& /*mc*/, const core::sizei /*extent*/,
		                      bool /*is_minimized*/) override
		{
		}
		void on_window_paint(render::draw_context& /*dc*/) override {}
		void invoke(const commands /*cmd*/) override {}
		bool is_command_checked(const commands /*cmd*/) override { return false; }
		void track_menu(const core::recti /*bounds*/, const std::vector<ui::command_ptr>& /*commands*/) override {}
	};

	class scroller_test_host final :
		public std::enable_shared_from_this<scroller_test_host>,
		public windowless_test_host
	{
	public:
		void controller_changed() override {}
		void invalidate_element(const ui::element_ptr& /*e*/) override {}
		void invalidate(const ui::host_invalid /*what*/) override {}

		ui::controller_ptr controller_from_location(core::pointi /*loc*/, ui::hit_test_context& /*ctx*/) override
		{
			return nullptr;
		}
	};

	// A host that has never been attached, or whose window has already been destroyed. Diffractor
	// 1.27.0 shipped a startup crash of exactly this shape: a panel borrowed another view's frame only
	// when it was visible, but kept populating, counting and invalidating while hidden.
	class detached_test_host final :
		public std::enable_shared_from_this<detached_test_host>,
		public windowless_test_host
	{
	public:
		int controller_requests = 0;
		int controller_changes = 0;
		int invalidations = 0;

		void controller_changed() override { ++controller_changes; }
		void invalidate_element(const ui::element_ptr& /*e*/) override { ++invalidations; }
		void invalidate(const ui::host_invalid /*what*/) override { ++invalidations; }

		ui::controller_ptr controller_from_location(const core::pointi /*loc*/, ui::hit_test_context& /*ctx*/) override
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

		void on_mouse_left_button_up(const core::pointi /*loc*/, const platform::key_state /*keys*/) override
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

	class cancelling_test_host final :
		public std::enable_shared_from_this<cancelling_test_host>,
		public windowless_test_host
	{
	public:
		std::vector<std::shared_ptr<cancellable_test_controller>> built;

		void controller_changed() override {}
		void invalidate_element(const ui::element_ptr& /*e*/) override {}
		void invalidate(const ui::host_invalid /*what*/) override {}

		ui::controller_ptr controller_from_location(const core::pointi /*loc*/, ui::hit_test_context& /*ctx*/) override
		{
			auto controller = std::make_shared<cancellable_test_controller>(shared_from_this(),
			                                                                core::recti(0, 0, 200, 400));
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

	class idle_escape_test_host final :
		public std::enable_shared_from_this<idle_escape_test_host>,
		public windowless_test_host
	{
	public:
		std::shared_ptr<idle_escape_test_controller> built;

		void controller_changed() override {}
		void invalidate_element(const ui::element_ptr& /*e*/) override {}
		void invalidate(const ui::host_invalid /*what*/) override {}

		ui::controller_ptr controller_from_location(const core::pointi /*loc*/, ui::hit_test_context& /*ctx*/) override
		{
			built = std::make_shared<idle_escape_test_controller>(shared_from_this(), core::recti(0, 0, 200, 400));
			return built;
		}
	};

	// Answers one controller over a fixed region, having first recorded an occluder, so what the host
	// caches can be told apart from what the controller itself claims.
	class stability_test_host final :
		public std::enable_shared_from_this<stability_test_host>,
		public windowless_test_host
	{
	public:
		core::recti answer_bounds{0, 0, 200, 400};
		core::recti occluder;
		int builds = 0;

		void controller_changed() override {}
		void invalidate_element(const ui::element_ptr& /*e*/) override {}
		void invalidate(const ui::host_invalid /*what*/) override {}

		ui::controller_ptr controller_from_location(const core::pointi /*loc*/, ui::hit_test_context& ctx) override
		{
			++builds;
			ctx.occluded(occluder);
			return std::make_shared<cancellable_test_controller>(shared_from_this(), answer_bounds);
		}
	};

	// An element that answers for part of itself. can_invoke is a style bit rather than a virtual, so
	// the bits are set rather than overridden.
	class invoke_counting_element final :
		public std::enable_shared_from_this<invoke_counting_element>,
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
		                                            const core::pointi element_offset,
		                                            ui::hit_test_context& ctx) override
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

		void visit_children(const std::function<void(const ui::element_ptr&)>& visit) const override { visit(held); }

		core::pointi children_offset() const override { return moved; }
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
	const auto column_row = calc_flex_layout(std::vector<element_ptr>{fixed, flexible_first, flexible_second}, mc,
	                                         {100, -1}, columns);
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
	const auto centered = calc_flex_layout(std::vector<element_ptr>{padded_first, padded_second}, mc, {100, -1},
	                                       centered_row);
	assert_equal(30, centered.layout_bounds[0].left, "centred line keeps the container's leading padding");
	assert_equal(100, centered.extent.cx, "a justified line reports the whole main axis it positions within");

	auto capped_cross = std::make_shared<flex_test_element>(core::sizei{20, 10});
	capped_cross->flex.align_self = flex_align::stretch;
	capped_cross->flex.max_size.cy = 12;
	auto tall = std::make_shared<flex_test_element>(core::sizei{20, 40});
	flex_container_layout stretch_row;
	stretch_row.wrap = flex_wrap::no_wrap;
	const auto stretched = calc_flex_layout(std::vector<element_ptr>{capped_cross, tall}, mc, {100, -1}, stretch_row);
	assert_equal(12, stretched.layout_bounds[0].height(), "stretch respects the item's maximum cross size");

	auto only_child = std::make_shared<flex_test_element>(core::sizei{20, 10});
	only_child->is_visible(false);
	flex_container_layout hidden_column;
	hidden_column.direction = flex_direction::column;
	hidden_column.padding = {10, 10};
	const auto nothing_visible = calc_flex_layout(std::vector<element_ptr>{only_child}, mc, {100, -1}, hidden_column);
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

static void should_resize_shared_element_tables()
{
	ui::test::fake_measure_context mc;
	const auto table = std::make_shared<ui::table_element>();
	table->headings({{"A"}, {"B"}, {"C"}});
	auto first = std::make_shared<flex_test_element>(core::sizei{50, 20});
	auto second = std::make_shared<flex_test_element>(core::sizei{100, 20});
	auto third = std::make_shared<flex_test_element>(core::sizei{100, 20});
	table->add(first, second, third);
	table->add("name", "value", "other");
	table->layout(mc, {20, 30, 420, 120});
	const auto widths = table->column_widths();
	assert_equal(400, std::accumulate(widths.begin(), widths.end(), 0), "element table fills its allocated width");
	const auto grip = table->divider_bounds(1);
	assert_equal(true, table->heading_bounds().contains(grip.center()), "a column is resized from its heading");
	const auto host = std::make_shared<detached_test_host>();
	const core::pointi offset{7, 11};
	const auto point = grip.center() + offset;
	ui::hit_test_context ctx(point, {0, 0, 500, 300});
	const auto controller = table->controller_from_location(host, point, offset, ctx);
	assert_equal(true, controller != nullptr && controller->cursor() == platform::cursor::left_right,
	             "element table exposes a resize controller");
	assert_equal(true, controller->bounds() == grip.offset(offset), "divider bounds include the element offset once");
	controller->on_mouse_left_button_down(point, {});
	controller->on_mouse_move({point.x + 40, point.y + 500});
	table->layout(mc, {20, 30, 420, 120});
	assert_equal(widths[0], table->column_widths()[0], "earlier columns stay where they were");
	assert_equal(widths[1] + 40, table->column_widths()[1],
	             "element table follows a captured pointer outside its bounds");
	assert_equal(widths[2] - 40, table->column_widths()[2], "last column makes the room");
	auto replacement = std::make_shared<ui::table_element>();
	const auto refill = [&]
	{
		replacement->add(std::make_shared<flex_test_element>(core::sizei{50, 20}),
		                 std::make_shared<flex_test_element>(core::sizei{100, 20}),
		                 std::make_shared<flex_test_element>(core::sizei{100, 20}));
	};
	replacement->restore_column_sizes(table->user_column_sizes());
	replacement->measure(mc, 400);
	refill();
	replacement->layout(mc, {20, 30, 420, 120});
	assert_equal(widths[1] + 40, replacement->column_widths()[1], "column sizes can be restored before rows arrive");
	replacement->clear();
	replacement->measure(mc, 400);
	refill();
	replacement->layout(mc, {20, 30, 420, 120});
	assert_equal(widths[1] + 40, replacement->column_widths()[1], "clearing and refreshing rows preserves user sizes");
	mc.scale_factor = 2.0;
	replacement->layout(mc, {20, 30, 820, 120});
	assert_equal((widths[1] + 40) * 2, replacement->column_widths()[1], "restored widths are logical units");
	mc.scale_factor = 1.0;
	replacement->reset_column_sizes();
	replacement->layout(mc, {20, 30, 420, 120});
	assert_equal(widths[1], replacement->column_widths()[1], "an explicit reset returns to content sizing");
	assert_equal(true, controller->escape(), "element table cancels a drag");
	controller->on_mouse_left_button_up({point.x + 40, point.y}, {});
	table->layout(mc, {20, 30, 420, 120});
	assert_equal(widths[1], table->column_widths()[1], "cancelled release cannot change the size");
	assert_equal(true, host->invalidations > 0, "table asks its host to lay out and repaint");
	assert_equal(true, table->divider_bounds(2).is_empty(), "last column has no resize grip");
	table->layout(mc, {0, 0, 1, 50});
	assert_equal(1, std::accumulate(table->column_widths().begin(), table->column_widths().end(), 0),
	             "tiny element table stays within its bounds");
	assert_equal(true, first->bounds.left >= 0 && third->bounds.right <= 1, "tiny cells never acquire negative widths");
}

static void should_keep_table_cells_in_the_element_tree()
{
	struct painted_cell : ui::element
	{
		core::sizei measure(render::measure_context&, int) const override { return {80, 20}; }
		void render(render::draw_context& dc, const core::pointi offset) const override
		{
			dc.draw_text("child", bounds.offset(offset), render::font_face::dialog, render::text_style::single_line, {},
			             {});
		}
	};
	ui::test::fake_measure_context mc;
	const auto table = std::make_shared<ui::table_element>();
	auto edit = ui::make_edit({}, "editable", [](const std::string&) {});
	table->headings({{"Label"}, {"Edit"}, {"Drawn"}});
	table->add("Name", edit, std::make_shared<painted_cell>());
	table->layout(mc, {10, 20, 410, 100});
	int found = 0;
	table->visit_children([&](const ui::element_ptr& child)
	{
		if (child == edit) ++found;
	});
	assert_equal(1, found, "a table keeps its drawn controls discoverable for focus and keyboard input");
	const auto grip = table->divider_bounds(1);
	assert_equal(true, edit->bounds.right < grip.center().x, "resize grip does not overlap the editable cell");
	edit->select(0, -1);
	edit->on_text("changed");
	assert_equal_strict("changed", edit->window_text(), "table layout does not take editing away from its cell");
	ui::test::fake_draw_context dc({0, 0, 500, 200});
	table->render(dc, {});
	assert_equal(true, dc.drawn_text().find("child") != std::string::npos,
	             "table renders the children its tree exposes");
	table->clear();
	table->add(std::vector<ui::element_ptr>(10, std::make_shared<flex_test_element>(core::sizei{30, 20})));
	table->layout(mc, {0, 0, 300, 100});
	assert_equal(10u, static_cast<unsigned>(table->column_widths().size()),
	             "tables safely handle more columns than fixed flags");
	table->clear();
	table->add(core::text_t{"Label"}, std::string("value"), "source");
	table->add(icon_index{}, core::text_t{"Keys"}, core::text_t{"Description"});
	table->add("Colour", core::rgba(1, 2, 3), "value", "extra");
	int cells = 0;
	table->visit_children([&](const ui::element_ptr&) { ++cells; });
	assert_equal(8, cells, "translated labels, strings, icon rows and coloured labels use the common table API");
}

// A table's headings sort its rows - by each row's key for the column, a first click the column's own way and a
// second the other - and resize its columns. A table without them draws no band and no dividers, so its first
// row never reads as one.
static void should_sort_a_table_from_its_headings()
{
	ui::test::fake_measure_context mc;
	const auto table = std::make_shared<ui::table_element>();
	table->headings({{"Name"}, {"Size", true, true, false}, {"Note", false, false}});
	const auto name = [](const std::string_view text)
	{
		return std::make_shared<ui::text_element>(text);
	};
	const auto b = name("b");
	const auto a = name("a10");
	const auto c = name("c");
	const auto plain = name("plain");
	table->add_row({b, name("2"), name("x")}, {"b", 2});
	table->add_row({a, name("10"), name("y")}, {"a10", 10});
	table->add_row({c, name(""), name("z")}, {"c"});
	table->add(plain, name(""), name(""));
	auto changes = 0;
	table->on_user_change = [&changes](const ui::table_element&)
	{
		++changes;
	};

	const core::recti area{0, 0, 400, 300};
	table->layout(mc, area);
	const auto band = table->heading_bounds();
	assert_equal(true, !band.is_empty() && band.top == area.top, "the headings are a band over the rows");
	assert_equal(true, b->bounds.top >= band.bottom, "with the rows below it");
	const auto order = [&]
	{
		table->layout(mc, area);
		return std::vector{b->bounds.top, a->bounds.top, c->bounds.top, plain->bounds.top};
	};
	const auto in_order = [](const std::vector<int>& tops, const std::array<int, 4> ranks)
	{
		for (auto i = 0; i < 4; ++i)
			for (auto j = 0; j < 4; ++j)
				if ((ranks[i] < ranks[j]) != (tops[i] < tops[j]) && i != j) return false;
		return true;
	};
	assert_equal(true, in_order(order(), {0, 1, 2, 3}), "rows start in the order they were added");

	const auto host = std::make_shared<detached_test_host>();
	const auto click = [&](const int column, const core::pointi release)
	{
		const auto at = table->heading_cell_bounds(column).center();
		ui::hit_test_context ctx(at, area);
		const auto controller = table->controller_from_location(host, at, {}, ctx);
		if (!controller) return false;
		controller->on_mouse_left_button_down(at, {});
		controller->on_mouse_left_button_up(release.x < 0 ? at : release, {});
		return controller->cursor() == platform::cursor::link;
	};

	assert_equal(true, click(1, {-1, -1}), "a heading answers a click");
	assert_equal(1, table->sort_column(), "and sorts by its column");
	assert_equal(false, table->sort_ascending(), "the way its column sorts first");
	assert_equal(true, in_order(order(), {1, 0, 2, 3}), "largest first, and rows without a key last");
	assert_equal(1, changes, "the owner is told");
	click(1, {-1, -1});
	assert_equal(true, in_order(order(), {0, 1, 2, 3}), "a second click sorts the other way, keyless rows still last");
	click(0, {-1, -1});
	assert_equal(true, table->sort_ascending() && in_order(order(), {1, 0, 2, 3}), "text sorts as it is read");

	click(1, {area.right + 50, area.bottom + 50});
	assert_equal(0, table->sort_column(), "a press released away from its heading sorts nothing");
	const auto unsortable = table->heading_cell_bounds(2).center();
	ui::hit_test_context unsortable_ctx(unsortable, area);
	assert_equal(true, table->controller_from_location(host, unsortable, {}, unsortable_ctx) == nullptr,
	             "a heading that cannot sort is not a button");

	table->clear();
	table->add_row({b, name("2"), name("x")}, {"b", 2});
	table->add_row({a, name("10"), name("y")}, {"a10", 10});
	table->layout(mc, area);
	assert_equal(true, a->bounds.top < b->bounds.top, "refreshed rows arrive in the order the user chose");
	table->sort(-1, true);
	table->layout(mc, area);
	assert_equal(true, b->bounds.top < a->bounds.top, "and no sort is the order they were added in");

	const auto bare = std::make_shared<ui::table_element>();
	const auto first = name("first");
	bare->add(first, name("second"));
	bare->layout(mc, area);
	assert_equal(true, bare->heading_bounds().is_empty(), "a table without headings has no band");
	assert_equal(true, bare->divider_bounds(0).is_empty(), "and no dividers on its first row");
	assert_equal(true, first->bounds.top < area.top + 8, "which starts at its top");
}

// A last column that may not shrink and is stretched to fill the table makes the room a resized column takes,
// down to what it holds, as any column after the divider does.
static void should_widen_a_column_beside_a_filled_fixed_one()
{
	ui::test::fake_measure_context mc;
	const auto table = std::make_shared<ui::table_element>();
	table->headings({{"#"}, {"Name"}, {"Notes", true}});
	table->no_shrink_col[0] = true;
	table->no_shrink_col[2] = true;
	table->add(std::make_shared<flex_test_element>(core::sizei{20, 20}),
	           std::make_shared<flex_test_element>(core::sizei{80, 20}),
	           std::make_shared<flex_test_element>(core::sizei{40, 20}));
	const core::recti area{0, 0, 400, 100};
	table->layout(mc, area);
	const auto widths = table->column_widths();
	const auto grip = table->divider_bounds(1);
	const auto host = std::make_shared<detached_test_host>();
	ui::hit_test_context ctx(grip.center(), area);
	const auto controller = table->controller_from_location(host, grip.center(), {}, ctx);
	assert_equal(true, controller != nullptr, "the name column's divider is reachable");
	controller->on_mouse_left_button_down(grip.center(), {});
	controller->on_mouse_move({grip.center().x + 60, grip.center().y});
	controller->on_mouse_left_button_up({grip.center().x + 60, grip.center().y}, {});
	table->layout(mc, area);
	assert_equal(widths[1] + 60, table->column_widths()[1], "a column widens into the room the last one was given");
	assert_equal(widths[2] - 60, table->column_widths()[2], "which the last column gives back");
}

static void should_keep_a_scrolled_table_alive_during_capture()
{
	ui::test::fake_measure_context mc;
	mc.scroll_width = 16;
	auto table = std::make_shared<ui::table_element>();
	table->can_scroll = true;
	for (int i = 0; i < 8; ++i)
		table->add(std::make_shared<flex_test_element>(core::sizei{50, 20}),
		           std::make_shared<flex_test_element>(core::sizei{50, 20}));
	table->layout(mc, {20, 30, 320, 90});
	const auto host = std::make_shared<detached_test_host>();
	const core::pointi offset{40, 100};
	const core::pointi point{352, 155};
	ui::hit_test_context ctx(point, {0, 0, 500, 300});
	auto controller = table->controller_from_location(host, point, offset, ctx);
	const auto scroll = std::dynamic_pointer_cast<ui::scroll_controller>(controller);
	assert_equal(true, scroll != nullptr, "a table exposes its shared scrollbar controller");
	assert_equal(true, scroll->_origin == offset, "scroll capture uses the same origin as rendering");
	assert_equal(false, scroll->_parent._moves_pixels, "an offset element never scrolls unrelated frame pixels");
	assert_equal(true, scroll->_parent._scroll_child_controls, "scrolling relocates editable cells in the input tree");
	controller->on_mouse_left_button_down(point, {});
	controller->on_mouse_move({point.x, point.y + 20});
	assert_equal(true, scroll->_parent.scroll_offset().y > 0, "thumb capture scrolls the table");
	assert_equal(true, controller->escape(), "scroll capture can be cancelled");
	assert_equal(0, scroll->_parent.scroll_offset().y, "cancel restores the table scroll");
	std::weak_ptr<ui::table_element> lifetime = table;
	table.reset();
	assert_equal(false, lifetime.expired(), "a captured scrollbar keeps its table alive");
	controller->on_mouse_left_button_up(point, {});
}

static void should_measure_flex_children_only_as_needed()
{
	flex_test_measure_context mc;
	std::vector<ui::element_ptr> children;
	std::vector<std::shared_ptr<flex_test_element>> measured;
	for (auto i = 0; i < 1000; ++i)
	{
		auto child = std::make_shared<flex_test_element>(core::sizei{20, 16});
		children.emplace_back(child);
		measured.emplace_back(std::move(child));
	}
	const auto result = ui::calc_flex_layout(children, mc, {400, -1}, {});
	auto calls = 0;
	for (const auto& child : measured) calls += child->measure_calls;
	std::printf("Width-sensitive flex: %d calls for %zu children\n", calls, children.size());
	assert_equal(2000, calls, "width-sensitive children are measured at their resolved width");
	assert_equal(400, result.extent.cx, "twenty children fill a line");
	assert_equal(800, result.extent.cy, "fifty lines retain their complete height");
	for (const auto& child : measured)
	{
		child->measure_calls = 0;
		child->set_style_bit(ui::element_style::fixed_measure_height, true);
	}
	const auto fixed = ui::calc_flex_layout(children, mc, {400, -1}, {});
	calls = 0;
	for (const auto& child : measured) calls += child->measure_calls;
	std::printf("Fixed-height flex: %d calls for %zu children\n", calls, children.size());
	assert_equal(1000, calls, "fixed-height leaves need one virtual measurement, not two");
	assert_equal(true, result.layout_bounds == fixed.layout_bounds, "avoiding remeasurement preserves every rectangle");
	auto line = std::make_shared<ui::text_element>("single", render::text_style::single_line);
	auto paragraph = std::make_shared<ui::text_element>("wrapped", render::text_style::multiline);
	assert_equal(true, line->is_style_bit_set(ui::element_style::fixed_measure_height),
	             "single-line text declares its stable height");
	assert_equal(false, paragraph->is_style_bit_set(ui::element_style::fixed_measure_height),
	             "wrapped text still reflows");
	ui::elements container(ui::element_options{line->style});
	assert_equal(false, container.is_style_bit_set(ui::element_style::fixed_measure_height),
	             "a container cannot inherit a leaf's fixed-height measurement promise");
}

static void should_keep_elements_and_layout_records_compact()
{
	assert_equal(1_z, sizeof(ui::flex_direction), "layout enums use one byte");
	if constexpr (sizeof(void*) == 8)
	{
		std::printf("UI sizes: element=%zu, container=%zu, item=%zu, layout=%zu\n", sizeof(ui::element),
		            sizeof(ui::elements), sizeof(ui::flex_item_layout), sizeof(ui::flex_container_layout));
		assert_equal(true, sizeof(ui::element) <= 88, "inactive elements do not carry a second background colour");
		assert_equal(true, sizeof(ui::elements) <= 128 + sizeof(std::vector<ui::element_ptr>),
		             "containers keep compact layout data alongside the standard library's iterator bookkeeping");
		assert_equal(true, sizeof(ui::panel_element) == sizeof(ui::elements),
		             "shared panel chrome adds no per-panel fields");
		assert_equal(true, sizeof(ui::flex_item_layout) <= 32,
		             "item flags retain full-width dimensions without extra words");
		assert_equal(true, sizeof(ui::flex_container_layout) <= 20, "container layout is compact");
	}
}

static void should_keep_background_targets_with_active_animations()
{
	auto saved = std::move(ui::animations);
	const auto factor = ui::animation_step_factor;
	const auto enabled = render::animations_enabled;
	const core::scope_exit restore([&]
	{
		ui::animations = std::move(saved);
		ui::animation_step_factor = factor;
		render::animations_enabled = enabled;
	});
	ui::animation_step_factor = 0.5f;
	render::animations_enabled = true;
	auto item = std::make_shared<ui::element>();
	const auto host = std::make_shared<detached_test_host>();
	item->_bg_color = core::color(0.2f, 0.3f, 0.4f, 0.5f);
	auto expected = item->_bg_color;
	auto target = core::color(ui::style::colors.important_background, 1.0f);
	item->set_style_bit(ui::element_style::important, true, host, item);
	assert_equal(true, ui::animations.contains(item.get()), "a changing background owns an active transition");
	ui::animations.at(item.get())();
	expected += (target - expected) * 0.5f;
	assert_equal(true, (item->_bg_color - expected).abs_sum() < core::color::color_epsilon,
	             "the current colour follows its target");
	item->set_style_bit(ui::element_style::important, false, host, item);
	target.a = 0;
	ui::animations.at(item.get())();
	expected += (target - expected) * 0.5f;
	assert_equal(true, (item->_bg_color - expected).abs_sum() < core::color::color_epsilon,
	             "fading out retains the prior target's RGB");
	item->set_style_bit(ui::element_style::has_tooltip, true, host, item);
	ui::animations.at(item.get())();
	expected += (target - expected) * 0.5f;
	assert_equal(true, (item->_bg_color - expected).abs_sum() < core::color::color_epsilon,
	             "an unrelated flag does not change a fade's colour");
	item->update_background_color();
	ui::animations.at(item.get())();
	assert_equal(true, item->_bg_color.a == 0, "an immediate style update also updates an active target");
	render::animations_enabled = false;
	item->set_style_bit(ui::element_style::info, true, host, item);
	ui::animations.at(item.get())();
	assert_equal(true,
	             (item->_bg_color - core::color(ui::style::colors.info_background, 1.0f)).abs_sum() <
	                 core::color::color_epsilon,
	             "software rendering still takes the target immediately");
	item->cancel_background_animation();
	assert_equal(false, ui::animations.contains(item.get()),
	             "custom animation can cancel the automatic background fade");
	ui::animations[item.get()] = []
	{
		return false;
	};
	item->cancel_background_animation();
	assert_equal(true, ui::animations.contains(item.get()),
	             "cancelling a background does not erase another kind of animation");
}

// A fade holds its element and view weakly: a dialog closed while one of its backgrounds fades goes at once, and the
// fade ends, rather than keeping it until the fade has run - or, where nothing steps the animations, until exit.
static void should_let_an_element_and_its_view_go_while_its_background_fades()
{
	auto saved = std::move(ui::animations);
	const core::scope_exit restore([&] { ui::animations = std::move(saved); });

	auto item = std::make_shared<ui::element>();
	auto host = std::make_shared<detached_test_host>();
	const std::weak_ptr<ui::element> item_left = item;
	const std::weak_ptr<detached_test_host> host_left = host;
	auto* const key = static_cast<void*>(item.get());

	item->set_style_bit(ui::element_style::important, true, host, item);
	assert_equal(true, ui::animations.contains(key), "a changing background fades");

	item.reset();
	host.reset();
	assert_equal(true, item_left.expired(), "the fade does not keep its element");
	assert_equal(true, host_left.expired(), "or its view");
	assert_equal(false, ui::animations.at(key)(), "and ends once they have gone");
}

void register_render_elements_tests(test_registry& tests)
{
	tests.add("Should keep background targets with active animations"s,
	          should_keep_background_targets_with_active_animations);
	tests.add("Should let an element and its view go while its background fades"s,
	          should_let_an_element_and_its_view_go_while_its_background_fades);
	tests.add("Should measure flex children only as needed"s, should_measure_flex_children_only_as_needed);
	tests.add("Should keep elements and layout records compact"s, should_keep_elements_and_layout_records_compact);
	tests.add("Should resize shared element tables"s, should_resize_shared_element_tables);
	tests.add("Should keep table cells in the element tree"s, should_keep_table_cells_in_the_element_tree);
	tests.add("Should sort a table from its headings"s, should_sort_a_table_from_its_headings);
	tests.add("Should widen a column beside a filled fixed one"s, should_widen_a_column_beside_a_filled_fixed_one);
	tests.add("Should keep a scrolled table alive during capture"s, should_keep_a_scrolled_table_alive_during_capture);
	tests.add("Should layout flex elements"s, should_layout_flex_elements);
	tests.add("Should preserve view scroller anchor across layout"s,
	          should_preserve_view_scroller_anchor_across_layout);
	tests.add("Should map the view scroller track to the list"s, should_map_the_view_scroller_track_to_the_list);
	tests.add("Should drag view scroller thumb from grab point"s, should_drag_view_scroller_thumb_from_grab_point);
	tests.add("Should close view scroller bands over track"s, should_close_view_scroller_bands_over_track);
	tests.add("Should survive a host with no window"s, should_survive_a_host_with_no_window);
	tests.add("Should not perform a cancelled gesture on release"s, should_not_perform_a_cancelled_gesture_on_release);
	tests.add("Should offer escape to an idle controller"s, should_offer_escape_to_an_idle_controller);
	tests.add("Should bound a reused hit test controller by what it passed over"s,
	          should_bound_a_reused_controller_by_what_it_passed_over);
	tests.add("Should not clip a hit test controller to what covers it"s,
	          should_not_clip_a_controller_to_what_covers_it);
	tests.add("Should keep a partly covered control clickable"s, should_keep_a_partly_covered_control_clickable);
	tests.add("Should not invoke an escaped click"s, should_not_invoke_an_escaped_click);
	tests.add("Should separate hit test occlusion from exclusion"s, should_separate_occlusion_from_exclusion);
	tests.add("Should hand the pointer to what an element holds"s, should_hand_the_pointer_to_what_an_element_holds);
}
