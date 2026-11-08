// This file is part of libdesktop, a C++20 platform layer for desktop applications
// Copyright 2026  Zac Walker
// 
// This program is free software; you can redistribute it and / or modify it
// under the terms of the LGPL License either version 2.1 or later.
// License details are available at https://www.gnu.org/licenses/lgpl-2.1.html
// This program is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY

// Purpose: The element tree an application builds its UI from: elements that are measured, laid out by
// flex, drawn and hit tested; the controllers a hit test hands the pointer to; the host a tree is drawn
// in, with what an element asks of it; the scroller; and the elements every UI needs - text, links,
// icons, dividers and padding. A frame finds the drawn controls and frame elements among them by walking
// what each element holds.

#pragma once

#include "libdesktop/ui.h"

namespace ui
{
	class controller;
	class elements;
	class element_host;
	class element_host_base;
	struct hit_test_context;
	struct hover_bubble;

	using controller_ptr = std::shared_ptr<controller>;
	using elements_ptr = std::shared_ptr<elements>;
	using element_host_ptr = std::shared_ptr<element_host>;
	using element_host_base_ptr = std::shared_ptr<element_host_base>;

	enum class element_event_type
	{
		invoke,
		click,
		double_click,
		tick,
		populate,
		dpi_changed,
		initialise,
		update_command_state,
		free_graphics_resources,
	};

	// What an element or controller asks of the host it is drawn in. The host may coalesce the requests
	// with its own work, so they say what is needed rather than doing it.
	enum class host_invalid : uint32_t
	{
		none = 0,
		redraw = 1 << 0,
		layout = 1 << 1,
		// What is under the pointer may have changed, so the hit test runs again.
		controller = 1 << 2,
		tooltip = 1 << 3,
		// An animation is running, so the host keeps ticking.
		animations = 1 << 4,
	};

	constexpr host_invalid operator|(const host_invalid a, const host_invalid b)
	{
		return static_cast<host_invalid>(static_cast<uint32_t>(a) | static_cast<uint32_t>(b));
	}

	constexpr bool operator&&(const host_invalid a, const host_invalid b)
	{
		return (static_cast<uint32_t>(a) & static_cast<uint32_t>(b)) != 0;
	}

	struct element_event
	{
		element_event_type type;
		element_host_ptr host;
	};

	struct interaction_context
	{
		const core::pointi loc;
		const core::pointi element_offset;
		const bool tracking;
		// Set by an element whose look changed, so its bounds are drawn again.
		bool redraw = false;
	};

	// Threaded through one hit test. It accumulates the region within which element_host may reuse the
	// controller the test produces: anything tested at higher precedence and passed over is cut out of
	// it, so moving the pointer onto that region forces a fresh test. Recording is a side effect of
	// testing, which is what stops a new tool from being hit tested but forgotten here.
	//
	// Every rect is device space, the space _loc is in.
	struct hit_test_context
	{
		hit_test_context(const core::pointi loc, const core::recti extent) : _loc(loc), _stability(extent)
		{
		}

		// The region a controller found at this location remains the right answer for.
		core::recti stability() const
		{
			return _stability;
		}

		// Answers whether the pointer is inside a region that takes precedence, so nothing beneath it
		// may claim the pointer. When it is not, the region is cut away instead.
		bool occluded(const core::recti bounds)
		{
			if (bounds.is_empty()) return false;
			if (bounds.contains(_loc)) return true;
			_stability.exclude(_loc, bounds);
			return false;
		}

		// For a region that was tested and missed by some other means, so it still has to be cut away.
		// Prefer occluded(): a caller that tests one way and records another can disagree with itself.
		void passed_over(const core::recti bounds)
		{
			if (!bounds.is_empty()) _stability.exclude(_loc, bounds);
		}

		// Narrows the region to one the answer provably cannot travel outside of, for a caller that has
		// already chosen between panes or regions rather than between overlapping targets.
		void confine(const core::recti bounds)
		{
			if (!bounds.is_empty()) _stability = _stability.intersection(bounds);
		}

	private:
		core::pointi _loc;
		core::recti _stability;
	};

	inline core::color handle_color(const bool selected, const bool hover, const bool tracking,
	                              const bool view_has_focus, const bool text_over,
	                              const core::color bg_clr = core::color(ui::style::colors.group_background))
	{
		if (tracking)
		{
			const auto clr = selected
				                 ? core::color(ui::style::colors.view_selected_background)
				                 : bg_clr.average(core::color(ui::style::colors.view_text));

			return clr.scale(hover ? 1.22f : 1.0f).aa(0.9f);
		}

		if (selected)
		{
			const auto clr = view_has_focus
				                 ? core::color(ui::style::colors.view_selected_background).scale(hover ? 1.22f : 1.0f)
				                 : bg_clr.average(core::color(ui::style::colors.view_selected_background)).scale(
					                 hover ? 1.33f : 1.0f);
			return clr.aa(0.9f);
		}

		if (hover)
		{
			const auto clr = bg_clr.average(core::color(ui::style::colors.view_text));
			return text_over ? clr.scale(0.8f).aa(0.9f) : clr.aa(0.9f);
		}

		return bg_clr.scale(text_over ? 1.22f : 1.44f).aa(0.9f);
	}

	// Every draggable splitter draws through this so the handle and its hover and drag highlights
	// are identical wherever a pane can be resized.
	inline void draw_splitter_handle(render::draw_context& dc, const core::recti bounds, const int splitter_width,
	                                 const bool active, const bool tracking)
	{
		const auto scale1 = core::round(1 * dc.scale_factor);
		const auto visual_width = std::max(scale1, splitter_width);
		const auto visual_left = bounds.left + (bounds.width() - visual_width) / 2;
		const auto handle_margin = tracking || active ? scale1 : std::max(scale1, core::mul_div(visual_width, 2, 9));

		core::recti draw_bounds;
		draw_bounds.left = visual_left + handle_margin;
		draw_bounds.right = visual_left + visual_width - handle_margin;
		draw_bounds.top = bounds.top + dc.handle_cxy;
		draw_bounds.bottom = bounds.bottom - dc.handle_cxy;

		if (draw_bounds.height() > 8)
		{
			dc.draw_rounded_rect(draw_bounds, core::color(0x000000, dc.colors.alpha * dc.colors.bg_alpha), dc.padding1);
		}

		if (active)
		{
			const auto clr = handle_color(false, active, tracking, dc.frame_has_focus, false).aa(dc.colors.alpha);
			dc.draw_rounded_rect(draw_bounds.inflate(-scale1), clr, dc.padding1);
		}
	}

	class controller
	{
	public:
		controller(element_host_ptr host, const core::recti bounds) : _host(std::move(host)), _bounds(bounds)
		{
		}

		virtual ~controller() = default;

		element_host_ptr _host;
		// The controller's own region: what it paints over, and what a release has to land in for the
		// gesture to count. Never the region the host caches it for -- that is hit_test_context's job,
		// and clipping this to it silently drops clicks and hover highlights on a partly covered target.
		core::recti _bounds;
		core::pointi _last_loc;
		core::pointi _start_loc;
		float _alpha = 1.0f;

		const core::recti bounds() const
		{
			return _bounds;
		}

		virtual void draw(render::draw_context& rc)
		{
		}

		virtual platform::cursor cursor() const
		{
			return platform::cursor::normal;
		}

		virtual void on_mouse_left_button_down(const core::pointi loc, const platform::key_state keys)
		{
			_last_loc = _start_loc = loc;
		}

		virtual void on_mouse_move(const core::pointi loc)
		{
		}

		virtual void on_mouse_left_button_up(const core::pointi loc, const platform::key_state keys)
		{
		}

		virtual void on_mouse_middle_button_down(const core::pointi loc, const platform::key_state keys)
		{
		}

		virtual void on_mouse_middle_button_up(const core::pointi loc, const platform::key_state keys)
		{
		}

		virtual void on_mouse_left_button_double_click(const core::pointi loc, const platform::key_state keys)
		{
		}

		virtual bool key_down(const char32_t key, const platform::key_state keys)
		{
			return false;
		}

		virtual void on_mouse_leave()
		{
		}

		// True only when something in progress was actually cancelled. Escape is shared with the view
		// and the app, so a controller that merely sits under the pointer must not consume it.
		virtual bool escape()
		{
			return false;
		}

		virtual void popup_from_location(hover_bubble& hover)
		{
		}

		virtual void tick()
		{
		}
	};

	// What an element reports its own changes to: the host it is drawn in, or a model standing in
	// for one.
	class element_host_base
	{
	public:
		virtual void invalidate_element(const element_ptr& e) = 0;
		virtual void invalidate(ui::host_invalid what) = 0;
	};

	// A frame host whose content is a tree of elements. It hit tests the pointer into a controller and
	// keeps that controller for as long as the hit test says it remains the answer.
	class element_host : public ui::frame_host, public element_host_base
	{
	public:
		core::sizei _extent;

		controller_ptr _active_controller;

		bool _show_cursor = true;
		bool _hover = false;
		bool _tracking = false;
		bool _controller_invalid = false;
		core::recti _controller_bounds;
		core::recti _tooltip_bounds;

		platform::cursor _cursor = platform::cursor::normal;

		// The application's command ids: the type is the UI's, its values the application's, as
		// icon_index's are, and a value-initialized id is none.
		virtual void invoke(commands cmd) = 0;
		virtual bool is_command_checked(commands cmd) = 0;
		virtual void track_menu(core::recti bounds, const std::vector<ui::command_ptr>& commands) = 0;
		virtual void controller_changed() = 0;
		// Never null: a host with no window yet, or none any more, answers ui::no_frame(). Everything
		// below dereferences this without checking, and so may every caller.
		virtual const ui::frame_ptr frame() const = 0;
		virtual const ui::frame_ptr owner() = 0;
		virtual controller_ptr controller_from_location(core::pointi loc, hit_test_context& ctx) = 0;

		// After a scroll of content holding controls or frames: they are found again where they now are.
		virtual void scroll_controls()
		{
			frame()->layout();
		}

		// The modal drag of files out of the window. Virtual so that a test host can stand in for it.
		virtual platform::drop_effect perform_drag(const std::vector<core::file_path>& files,
		                                           const std::vector<core::folder_path>& folders)
		{
			return platform::perform_drag(frame()->handle(), files, folders);
		}

		void on_window_destroy() override
		{
			_active_controller.reset();
		}

		void update_controller(const core::pointi loc)
		{
			if (!_tooltip_bounds.is_empty() && !_tooltip_bounds.contains(loc))
			{
				_tooltip_bounds.clear();
				invalidate(ui::host_invalid::tooltip);
			}

			if (!_tracking && (_controller_invalid || !_controller_bounds.contains(loc)))
			{
				_active_controller.reset();

				hit_test_context ctx{loc, core::recti(_extent)};
				_active_controller = controller_from_location(loc, ctx);

				auto cursor = platform::cursor::normal;

				if (_active_controller)
				{
					_active_controller->on_mouse_move(loc);
					cursor = _active_controller->cursor();
				}

				_cursor = cursor;
				// The controller answers for its own region; the hit test answers for how far that answer
				// travels. An empty rect here is the idiom for re-testing on every move, which is what a
				// controller standing in for whatever happens to be under the pointer wants.
				_controller_bounds = _active_controller
					                     ? _active_controller->bounds().intersection(ctx.stability())
					                     : core::recti{};
				_controller_invalid = false;

				controller_changed();
				frame()->invalidate();
			}
		}

		void on_mouse_left_button_down(const core::pointi loc, const platform::key_state keys) override
		{
			update_controller(loc);
			update_tracking(loc, true);

			const auto c = _active_controller;

			if (c)
			{
				c->on_mouse_left_button_down(loc, keys);
			}

			update_cursor();
		}

		void on_mouse_left_button_up(const core::pointi loc, const platform::key_state keys) override
		{
			update_tracking(loc, false);

			// The copy keeps the controller alive for the duration of its own handler, and is released
			// before the controller is re-tested: a copy still held there would destroy the outgoing
			// controller after its replacement was built, so the replacement's hover state was undone.
			if (const auto c = _active_controller)
			{
				c->on_mouse_left_button_up(loc, keys);
			}

			// on_mouse_left_button_up sometimes shows a dialog
			// it is best to fetch the current cursor location
			update_controller(frame()->cursor_location());
		}

		void on_mouse_middle_button_down(const core::pointi loc, const platform::key_state keys) override
		{
			update_controller(loc);
			if (_active_controller) _active_controller->on_mouse_middle_button_down(loc, keys);
			update_cursor();
		}

		void on_mouse_middle_button_up(const core::pointi loc, const platform::key_state keys) override
		{
			if (_active_controller) _active_controller->on_mouse_middle_button_up(loc, keys);
		}

		void on_mouse_left_button_double_click(const core::pointi loc, const platform::key_state keys) override
		{
			update_controller(loc);
			const auto c = _active_controller;

			if (c)
			{
				c->on_mouse_left_button_double_click(loc, keys);
			}
		}

		void update_tracking(const core::pointi loc, const bool is_tracking)
		{
			_hover = core::recti(_extent).contains(loc);
			_tracking = is_tracking;
		}

		bool escape_controller()
		{
			if (!_active_controller) return false;
			if (!_active_controller->escape()) return false;

			if (_tracking)
			{
				// The button is still down -- _tracking is what says so -- and the cancelled controller is
				// the one that must receive its own release. Replacing it here handed that release to a
				// fresh controller with no memory of the cancel, which then performed the very gesture
				// Escape had just refused: a rubber-band selection cancelled mid-drag was replaced on
				// release by the single item under the pointer. The re-test happens once the button is up.
				_controller_invalid = true;
			}
			update_cursor();
			return true;
		}

		bool key_down_controller(const char32_t key, const platform::key_state keys) const
		{
			return _tracking && _active_controller && _active_controller->key_down(key, keys);
		}

		void on_mouse_move(const core::pointi loc, const bool is_tracking) override
		{
			update_tracking(loc, is_tracking);
			update_controller(loc);

			const auto c = _active_controller;

			if (c)
			{
				c->on_mouse_move(loc);
			}

			update_cursor();
		}

		void on_mouse_leave(const core::pointi loc) override
		{
			update_controller({-1, -1});
			_hover = false;
		}

		void update_cursor()
		{
			if (_active_controller)
			{
				_cursor = _active_controller->cursor();
			}

			frame()->set_cursor(_show_cursor ? _cursor : platform::cursor::none);
		}

		void show_cursor(const bool show)
		{
			if (_show_cursor != show)
			{
				_show_cursor = show;
				update_cursor();
			}
		}

		bool show_cursor() const
		{
			return _show_cursor;
		}
	};

	struct scroller_section
	{
		std::string text;
		icon_index icon = {};
		int y = 0;
	};

	struct scroll_anchor
	{
		element_ptr element;
		int device_top = 0;
		double scroll_ratio = 0.0;
		bool was_at_start = true;
		bool valid = false;
	};

	class scroller
	{
	public:
		core::pointi _offset;
		core::sizei _scroll_extent;
		core::recti _client_bounds;
		core::recti _scroll_bounds;
		std::vector<scroller_section> _sections;
		std::function<void(hover_bubble&, core::pointi)> popup_func;
		std::function<void()> changed_func;

		bool _active = false;
		bool _tracking = false;
		// The scrolled content holds controls or frames, which its host places again after a scroll.
		bool _scroll_child_controls = false;
		// Its client bounds are where its frame shows it, so a scroll moves the pixels there and draws only
		// what it uncovers; false for a scroller drawn among other elements, at an offset only its host
		// knows, whose scroll draws the frame again.
		bool _moves_pixels = true;

		const core::recti scroll_bounds() const
		{
			return _scroll_bounds;
		}

		// The painted track. Every mapping between list position and scrollbar position goes through
		// it, so the thumb, the section bands and the hit tests cannot disagree.
		core::recti track_bounds() const
		{
			constexpr auto inset = 2;
			return _scroll_bounds.height() > inset * 2 ? _scroll_bounds.inflate(0, -inset) : _scroll_bounds;
		}

		// Shared so anything drawn in line with the track sits in the same column as it.
		int track_inset() const
		{
			return !_sections.empty() || _active ? 1 : core::mul_div(_scroll_bounds.width(), 2, 9);
		}

		// Section bands in track-relative coordinates, in paint order.
		template <class F>
		void for_each_band(F&& f) const
		{
			const auto track_height = track_bounds().height();
			if (track_height <= 0) return;

			if (_sections.empty())
			{
				f(0, track_height, static_cast<const scroller_section*>(nullptr));
				return;
			}

			constexpr auto band_gap = 2;
			// Matches the minimum the sections are generated against. A band below it cannot be read
			// or aimed at, and drawn against the band above it it reads as a second overlapping band.
			constexpr auto min_band_height = 8;

			const auto count = _sections.size();
			auto top = 0;

			for (size_t i = 0; i < count; ++i)
			{
				const auto& so = _sections[i];
				// Not std::clamp: top can already have run past the track, and clamp with lo > hi is
				// undefined.
				auto bottom = std::min(std::max(logical_to_scrollbar_pos(so.y), top), track_height);

				// The list runs past its last group -- footer, actions, trailing space -- so the track
				// is closed off rather than stopping short of the position the thumb can reach. What is
				// left over only earns its own band if it is big enough to be one; otherwise the last
				// section keeps it.
				if (i + 1 == count && track_height - bottom - band_gap < min_band_height)
				{
					bottom = track_height;
				}

				if (bottom > top) f(top, bottom, &so);
				top = bottom + band_gap;
			}

			if (track_height - top >= min_band_height)
			{
				f(top, track_height, static_cast<const scroller_section*>(nullptr));
			}
		}

		// Device y the thumb starts at before it is padded out to a usable size. A drag measures its
		// grab from here so scrollbar_pos_to_logical inverts it exactly.
		int thumb_origin() const
		{
			return track_bounds().top + logical_to_scrollbar_pos(_offset.y);
		}

		// The painted thumb. Hit testing shares it so a press lands on what the user aimed at.
		core::recti thumb_bounds() const
		{
			const auto track = track_bounds();
			if (!can_scroll() || track.height() <= 0) return {};

			const auto cy = core::mul_div(track.height(), _client_bounds.height(), _scroll_extent.cy);
			const auto pad = cy >= 20 ? 1 : 10;
			const auto origin = thumb_origin();
			const auto inset = track_inset() + 1;

			return {
				track.left + inset, std::clamp(origin - pad, track.top, track.bottom),
				track.right - inset, std::clamp(origin + cy + pad, track.top, track.bottom)
			};
		}

		void scroll_offset(const element_host_ptr& host, const int x, const int y)
		{
			const auto cx = _scroll_extent.cx - _client_bounds.width();
			const auto cy = _scroll_extent.cy - _client_bounds.height();
			const auto xx = cx > 0 ? std::clamp(x, 0, cx) : 0;
			const auto yy = cy > 0 ? std::clamp(y, 0, cy) : 0;

			if ((_offset.x != xx || _offset.y != yy) && _scroll_extent.cy > 0)
			{
				const auto delta = _offset.y - yy;
				_offset.x = xx;
				_offset.y = yy;

				if (delta != 0)
				{
					if (_moves_pixels) host->frame()->scroll(0, delta, _client_bounds);
					else host->frame()->invalidate();

					// What is drawn in the scrolled region moved with its pixels; the controls and frames
					// in it are told where they are now.
					if (_scroll_child_controls) host->scroll_controls();
				}

				host->frame()->invalidate(_moves_pixels ? _scroll_bounds : core::recti{});

				if (changed_func)
				{
					changed_func();
				}
			}
		}

		void layout(const core::sizei scroll_extent, const core::recti client_bounds, const core::recti scroll_bounds)
		{
			_scroll_extent = scroll_extent;
			_client_bounds = client_bounds;
			_scroll_bounds = scroll_bounds;

			const auto cx = _scroll_extent.cx - _client_bounds.width();
			const auto cy = _scroll_extent.cy - _client_bounds.height();

			_offset.x = cx > 0 ? std::clamp(_offset.x, 0, cx) : 0;
			_offset.y = cy > 0 ? std::clamp(_offset.y, 0, cy) : 0;
		}

		void sections(std::vector<scroller_section> section_offsets)
		{
			_sections = std::move(section_offsets);
		}

		const core::pointi scroll_offset() const
		{
			return _offset;
		}

		const core::recti client_bounds() const
		{
			return _client_bounds;
		}

		scroll_anchor capture_anchor(const element_ptr& element) const;
		int anchor_offset(const scroll_anchor& anchor, bool element_is_current) const;
		void restore_anchor(const element_host_ptr& host, const scroll_anchor& anchor, bool element_is_current);

		void reset()
		{
			_offset.x = 0;
			_offset.y = 0;
		}

		// Centres the thumb on a device y. Takes a device y so the caller never has to know where the
		// track begins.
		void scrollbar_to(const element_host_ptr& host, const int device_y)
		{
			const auto track = track_bounds();
			if (track.height() <= 0 || _scroll_extent.cy <= 0) return;
			const auto half_handle = core::mul_div(_client_bounds.height(), track.height(), _scroll_extent.cy) / 2;
			scroll_offset(host, 0, scrollbar_pos_to_logical(device_y - half_handle));
		}

		// Puts the thumb origin on a device y, so a drag keeps the point it was grabbed by.
		void scrollbar_thumb_to(const element_host_ptr& host, const int device_y)
		{
			if (track_bounds().height() <= 0 || _scroll_extent.cy <= 0) return;
			scroll_offset(host, 0, scrollbar_pos_to_logical(device_y));
		}

		core::pointi device_to_logical(const core::pointi loc) const
		{
			return {loc.x, loc.y + _offset.y};
		}

		core::pointi logical_to_device(const core::pointi loc) const
		{
			return {loc.x, loc.y - _offset.y};
		}

		core::recti logical_to_device(const core::recti bounds) const
		{
			return {{bounds.left, bounds.top - _offset.y}, bounds.extent()};
		}

		void offset(const element_host_ptr& host, const int x, const int y)
		{
			scroll_offset(host, _offset.x + x, _offset.y + y);
		}

		void draw_scroll(render::draw_context& dc) const;

		void scroll_popup(hover_bubble& hover, const core::pointi loc) const
		{
			if (popup_func)
			{
				popup_func(hover, loc);
			}
		}

		bool can_scroll() const
		{
			return _scroll_extent.cy > _client_bounds.height() && _client_bounds.height() > 0;
		}

		void clear()
		{
			_offset = {};
			_scroll_extent = {};
			_client_bounds.clear();
			_scroll_bounds.clear();
			_sections.clear();
			popup_func = {};
		}

		int logical_to_scrollbar_pos(const int y) const
		{
			return _scroll_extent.cy > 0 ? core::mul_div(y, track_bounds().height(), _scroll_extent.cy) : 0;
		}

		int scrollbar_pos_to_logical(const int device_y) const
		{
			const auto track = track_bounds();
			return track.height() > 0 ? core::mul_div(device_y - track.top, _scroll_extent.cy, track.height()) : 0;
		}
	};

	class scroll_controller final : public controller
	{
	public:
		scroller& _parent;
		// Where the scroller's coordinates sit in the frame: nothing for a host's own, an element's offset
		// for one drawn among other elements.
		core::pointi _origin;
		core::pointi _start;
		// Where in the thumb the drag took hold, so the list does not jump under the pointer.
		int _grab_offset = 0;


		scroll_controller(const element_host_ptr& host, scroller& parent, const core::recti bounds,
		                  const core::pointi origin = {}) :
			controller(host, bounds), _parent(parent), _origin(origin)
		{
			_parent._active = true;
		}

		~scroll_controller() override
		{
			if (_parent._tracking)
			{
				escape();
			}

			_parent._active = false;
		}

		// The scroll bar is painted by the view that owns it, never by this controller. Splitting the
		// paint between the two made the bar and its section bands vanish whenever the flag and the
		// active controller disagreed.

		platform::cursor cursor() const override
		{
			return platform::cursor::up_down;
		}

		void update_pos(const int y) const
		{
			_parent.scrollbar_thumb_to(_host, y - _origin.y - _grab_offset);
		}

		void on_mouse_left_button_down(const core::pointi loc, const platform::key_state keys) override
		{
			_last_loc = loc;
			_parent._tracking = true;
			_start = _parent.scroll_offset();

			// Only a press on bare track jumps. A press on the thumb drags it from where it was held.
			const auto thumb = _parent.thumb_bounds();
			const auto y = loc.y - _origin.y;

			if (thumb.is_empty() || y < thumb.top || y >= thumb.bottom)
			{
				_parent.scrollbar_to(_host, y);
			}

			_grab_offset = y - _parent.thumb_origin();
			_host->frame()->invalidate();
		}

		void on_mouse_move(const core::pointi loc) override
		{
			_last_loc = loc;

			if (_parent._tracking)
			{
				update_pos(loc.y);
			}
		}

		void on_mouse_left_button_up(const core::pointi loc, const platform::key_state keys) override
		{
			const auto moved = loc != _last_loc;
			_last_loc = loc;

			// Escape may already have ended the drag and restored the position; do not re-apply it.
			if (_parent._tracking)
			{
				if (moved) update_pos(loc.y);
				_parent._tracking = false;
			}

			_host->frame()->invalidate();
		}

		bool escape() override
		{
			if (!_parent._tracking) return false;
			_parent.scroll_offset(_host, _start.x, _start.y);
			_parent._tracking = false;
			return true;
		}

		void popup_from_location(hover_bubble& hover) override
		{
			if (_parent._active)
			{
				_parent.scroll_popup(hover, _last_loc);
			}
		}
	};

	enum class element_style : uint32_t
	{
		none = 0,
		checked = 1 << 16,

		visible = 1 << 17,
		can_invoke = 1 << 18,
		has_tooltip = 1 << 19,

		hover = 1 << 20,
		tracking = 1 << 21,
		selected = 1 << 22,
		error = 1 << 23,
		highlight = 1 << 24,

		important = 1 << 25,
		info = 1 << 26,
		background = 1 << 27,
		dark_background = 1 << 28,
		shaded_background = 1 << 29,
		// Takes no input, and neither does anything it holds: a control under it draws dimmed.
		disabled = 1 << 30,
	};

	constexpr element_style operator|(const element_style a, const element_style b)
	{
		return static_cast<element_style>(static_cast<uint32_t>(a) | static_cast<uint32_t>(b));
	}

	constexpr element_style operator&(const element_style a, const element_style b)
	{
		return static_cast<element_style>(static_cast<uint32_t>(a) & static_cast<uint32_t>(b));
	}

	constexpr element_style operator^(const element_style a, const element_style b)
	{
		return static_cast<element_style>(static_cast<uint32_t>(a) ^ static_cast<uint32_t>(b));
	}

	constexpr element_style operator~(const element_style a)
	{
		return static_cast<element_style>(~static_cast<uint32_t>(a));
	}

	constexpr bool operator&&(const element_style a, const element_style b)
	{
		return (static_cast<uint32_t>(a) & static_cast<uint32_t>(b)) != 0;
	}

	constexpr element_style& operator|=(element_style& a, const element_style b)
	{
		a = a | b;
		return a;
	}

	constexpr element_style& operator&=(element_style& a, const element_style b)
	{
		a = a & b;
		return a;
	}

	constexpr element_style& operator^=(element_style& a, const element_style b)
	{
		a = a ^ b;
		return a;
	}

	struct element_padding
	{
		int16_t cx = 0;
		int16_t cy = 0;

		void operator()(const int16_t xy)
		{
			cx = cy = xy;
		}

		void operator()(const int16_t x, const int16_t y)
		{
			cx = x;
			cy = y;
		}

		operator core::sizei() const
		{
			return {cx, cy};
		}

		core::sizei operator *(const double d) const noexcept
		{
			return {core::round(cx * d), core::round(cy * d)};
		}
	};

	enum class flex_direction
	{
		row,
		column
	};

	enum class flex_wrap
	{
		no_wrap,
		wrap
	};

	enum class flex_justify
	{
		start,
		center,
		end,
		space_between
	};

	enum class flex_align
	{
		automatic,
		start,
		center,
		end,
		stretch
	};

	struct flex_item_layout
	{
		// All lengths are logical units; calc_flex_layout applies the measure-context scale once.
		float grow = 0.0f;
		float shrink = 0.0f;
		int basis = -1;
		core::sizei min_size{};
		core::sizei max_size{INT_MAX, INT_MAX};
		flex_align align_self = flex_align::automatic;
		bool main_start_auto = false;
		bool break_before = false;
		bool break_after = false;
		bool keep_with_next = false;
		bool center_line = false;
	};

	struct flex_container_layout
	{
		// Gap and padding use the same logical-unit contract as item basis/min/max sizes.
		flex_direction direction = flex_direction::row;
		flex_wrap wrap = flex_wrap::wrap;
		flex_justify justify = flex_justify::start;
		flex_align align_items = flex_align::center;
		core::sizei gap{};
		core::sizei padding{};
	};

	constexpr flex_item_layout operator|(flex_item_layout left, const flex_item_layout& right)
	{
		left.grow = std::max(left.grow, right.grow);
		left.shrink = std::max(left.shrink, right.shrink);
		if (right.basis >= 0) left.basis = right.basis;
		left.min_size.cx = std::max(left.min_size.cx, right.min_size.cx);
		left.min_size.cy = std::max(left.min_size.cy, right.min_size.cy);
		left.max_size.cx = std::min(left.max_size.cx, right.max_size.cx);
		left.max_size.cy = std::min(left.max_size.cy, right.max_size.cy);
		if (right.align_self != flex_align::automatic) left.align_self = right.align_self;
		left.main_start_auto |= right.main_start_auto;
		left.break_before |= right.break_before;
		left.break_after |= right.break_after;
		left.keep_with_next |= right.keep_with_next;
		left.center_line |= right.center_line;
		return left;
	}

	namespace flex_item
	{
		inline constexpr auto grow = []
		{
			flex_item_layout result;
			result.grow = 1.0f;
			return result;
		}();
		inline constexpr auto shrink = []
		{
			flex_item_layout result;
			result.shrink = 1.0f;
			return result;
		}();
		// A media pane yields space to the controls beside it, but never below a height at which the
		// image or video stops being recognisable.
		inline constexpr auto media = []
		{
			flex_item_layout result;
			result.shrink = 1.0f;
			result.min_size.cy = 64;
			return result;
		}();
		inline constexpr auto stretch = []
		{
			flex_item_layout result;
			result.align_self = flex_align::stretch;
			return result;
		}();
		inline constexpr auto center = []
		{
			flex_item_layout result;
			result.align_self = flex_align::center;
			result.center_line = true;
			return result;
		}();
		inline constexpr auto right_justified = []
		{
			flex_item_layout result;
			result.main_start_auto = true;
			return result;
		}();
		inline constexpr auto line_break = []
		{
			flex_item_layout result;
			result.break_after = true;
			return result;
		}();
		inline constexpr auto no_break = []
		{
			flex_item_layout result;
			result.keep_with_next = true;
			return result;
		}();
		inline constexpr auto new_line = []
		{
			flex_item_layout result;
			result.break_before = true;
			return result;
		}();
	}

	struct element_options
	{
		element_style style = element_style::none;
		flex_item_layout flex;

		constexpr element_options() = default;

		constexpr element_options(const element_style style_in) : style(style_in)
		{
		}

		constexpr element_options(const flex_item_layout& flex_in) : flex(flex_in)
		{
		}
	};

	constexpr element_options operator|(element_options left, const element_options& right)
	{
		left.style |= right.style;
		left.flex = left.flex | right.flex;
		return left;
	}

	constexpr element_options operator|(const element_style left, const flex_item_layout& right)
	{
		return element_options(left) | element_options(right);
	}

	constexpr element_options operator|(const flex_item_layout& left, const element_style right)
	{
		return element_options(left) | element_options(right);
	}

	constexpr element_options operator|(const element_options& left, const element_style right)
	{
		return left | element_options(right);
	}

	constexpr element_options operator|(const element_style left, const element_options& right)
	{
		return element_options(left) | right;
	}

	constexpr element_options operator|(const element_options& left, const flex_item_layout& right)
	{
		return left | element_options(right);
	}

	constexpr element_options operator|(const flex_item_layout& left, const element_options& right)
	{
		return element_options(left) | right;
	}

	// What a frame does with an element it finds in its host's tree: nothing, for content its host draws
	// and hit tests; give it the pointer and the keyboard, for a drawn control; or place its frame, for a
	// frame element.
	enum class element_kind : uint8_t
	{
		content,
		control,
		frame,
	};

	class element
	{
	public:
		// invalidate_slack value meaning "not known yet, or wider than a byte can describe", which makes
		// invalidate_bounds ask for the whole frame.
		static constexpr uint8_t unknown_invalidate_slack = 255;

		core::recti bounds;
		element_padding padding{3, 3};
		element_padding margin{0, 0};
		flex_item_layout flex;
		element_style style = element_style::visible;
		element_kind kind = element_kind::content;

		// How far outside bounds this element paints, in device pixels. Maintained at draw time because
		// padding is logical and only becomes device pixels once the scale factor is known.
		mutable uint8_t invalidate_slack = unknown_invalidate_slack;

		core::color _bg_color;
		core::color _bg_target;

		element() noexcept = default;

		constexpr explicit element(const element_style style_in) noexcept : style(
			style_in | element_style::visible)
		{
		}

		constexpr explicit element(const element_options& options) noexcept :
			flex(options.flex), style(options.style | element_style::visible)
		{
		}

		element(const element&) noexcept = delete;
		element& operator=(const element&) noexcept = delete;
		element(element&&) noexcept = delete;
		element& operator=(element&&) noexcept = delete;
		virtual ~element() noexcept = default;

		core::color calc_background_color(const render::draw_context& dc) const
		{
			auto c = _bg_color;
			c.a = _bg_color.a * dc.colors.alpha;
			return c;
		}

		// The rectangle a partial repaint must cover to redraw this element completely. Empty means the
		// whole frame, which is what an unknown slack falls back to.
		core::recti invalidate_bounds(const core::pointi element_offset = {}) const
		{
			if (invalidate_slack == unknown_invalidate_slack) return {};
			return bounds.offset(element_offset).inflate(invalidate_slack);
		}

		void render_background(render::draw_context& dc, const core::pointi element_offset) const
		{
			const auto pad = padding * dc.scale_factor;

			// draw_rounded_rect grows its fill by round(0.8333 * (radius + 2) - radius), which is at most
			// 2 for any radius. Recorded even when nothing is drawn, so the first hover already knows it.
			const auto slack = std::max(pad.cx, pad.cy) + 2;
			invalidate_slack = slack < unknown_invalidate_slack
				                   ? static_cast<uint8_t>(slack)
				                   : unknown_invalidate_slack;

			const auto bg = calc_background_color(dc);

			if (bg.a > 0.0f)
			{
				dc.draw_rounded_rect(bounds.offset(element_offset).inflate(pad.cx, pad.cy), bg, dc.padding1);
			}
		}

		core::sizei porch() const
		{
			return {padding.cx + margin.cx, padding.cy + margin.cy};
		}

		// Whether a press at loc is the element's or what it holds acts on, so a window that moves by the rest of
		// its ground leaves it alone.
		virtual bool is_control_area(const core::pointi loc, const core::pointi element_offset) const
		{
			if (!is_visible()) return false;
			if (bounds.contains(loc - element_offset) && (can_invoke() || has_tooltip())) return true;

			auto found = false;
			const auto offset = element_offset + children_offset();

			visit_children([&](const element_ptr& c)
			{
				if (!found && c) found = c->is_control_area(loc, offset);
			});

			return found;
		}

		// Draws the element at its bounds moved by element_offset, where its content coordinates sit in the
		// frame, and what it holds over that: an element that draws something of its own as well calls
		// render_children for what visit_children names.
		virtual void render(render::draw_context& dc, const core::pointi element_offset) const
		{
			render_children(dc, element_offset);
		}

		virtual core::sizei measure(render::measure_context& mc, const int width_limit) const
		{
			return {};
		}

		// Places the element, and whatever it holds, in content coordinates.
		virtual void layout(render::measure_context& mc, const core::recti bounds_in)
		{
			bounds = bounds_in;
		}

		virtual void tooltip(hover_bubble& hover, const core::pointi loc, const core::pointi element_offset) const
		{
		}

		virtual void hover(interaction_context& ic)
		{
		}

		virtual void dispatch_event(const element_event& event)
		{
		}

		// What the element holds, in the order it draws them, which is the order Tab visits the controls
		// among them. A frame finds its drawn controls and frame elements through this, so an element that
		// holds either names it here.
		virtual void visit_children(const std::function<void(const element_ptr&)>& visit) const
		{
		}

		// Where what it holds sits against the element's own content coordinates: an element that scrolls
		// what it holds moves it by its scroll.
		virtual core::pointi children_offset() const
		{
			return {};
		}

		// Offered a wheel turned over loc: an element that scrolls what it shows takes it, and answers true.
		// One that holds others offers it to them.
		virtual bool wheel(const core::pointi loc, const core::pointi element_offset, const int delta)
		{
			auto taken = false;
			const auto offset = element_offset + children_offset();

			visit_children([&](const element_ptr& c)
			{
				if (!taken && c && c->is_visible()) taken = c->wheel(loc, offset, delta);
			});

			return taken;
		}

		// Draws the visible elements visit_children names.
		void render_children(render::draw_context& dc, core::pointi element_offset) const;

		// What a press, a hover or a tooltip at loc goes to: the element's own controller, or by default the
		// first that a visible element it holds offers.
		virtual controller_ptr controller_from_location(const element_host_ptr& host, core::pointi loc, core::pointi element_offset,
		                                                hit_test_context& ctx);

		bool is_style_bit_set(const element_style mask) const
		{
			return style && mask;
		}

		void set_style_bit(element_style mask, bool state, const element_host_base_ptr& view, const element_ptr& e);
		void update_background_color();
		void set_style_bit(element_style mask, bool state);

		bool can_invoke() const
		{
			return is_style_bit_set(element_style::can_invoke);
		}

		bool has_tooltip() const
		{
			return is_style_bit_set(element_style::has_tooltip);
		}

		bool is_visible() const
		{
			return is_style_bit_set(element_style::visible);
		}

		void is_visible(const bool visible)
		{
			set_style_bit(element_style::visible, visible);
		}

		// Whether the element itself takes input. One that does not disables everything it holds.
		bool is_enabled() const
		{
			return !is_style_bit_set(element_style::disabled);
		}

		void enable(const bool enable)
		{
			if (is_enabled() == enable) return;
			set_style_bit(element_style::disabled, !enable);
			enabled_changed();
		}

		virtual void enabled_changed()
		{
		}
	};

	struct flex_layout_result
	{
		std::vector<core::recti> layout_bounds;
		core::sizei extent;
	};

	flex_layout_result calc_flex_layout(const std::vector<element_ptr>& elements, render::measure_context& mc,
	                                    core::sizei available, const flex_container_layout& container);

	core::sizei layout_flex_elements(const std::vector<element_ptr>& elements, render::measure_context& mc, core::recti bounds,
	                           const flex_container_layout& container);

	class elements : public element, public std::enable_shared_from_this<elements>
	{
	protected:
		std::vector<element_ptr> _children;

	public:
		flex_container_layout flex_container;

		elements() noexcept = default;

		explicit elements(const element_options& options) noexcept : element(options)
		{
		}

		explicit elements(std::vector<element_ptr> children) noexcept : _children(std::move(children))
		{
		}

		bool is_empty() const
		{
			return _children.empty();
		}

		void clear()
		{
			_children.clear();
		}

		size_t size() const
		{
			return _children.size();
		}

		void render(render::draw_context& dc, const core::pointi offset) const override
		{
			if (std::ranges::any_of(_children, [](const element_ptr& e) { return e->is_visible(); }))
			{
				render_background(dc, offset);

				for (const auto& e : _children)
				{
					if (e->is_visible())
					{
						e->render(dc, offset);
					}
				}
			}
		}

		void add(const element_ptr& v)
		{
			_children.emplace_back(v);
		}

		void add(const std::vector<element_ptr>& vals)
		{
			if (!vals.empty())
			{
				_children.insert(_children.end(), vals.begin(), vals.end());
			}
		}

		core::sizei measure(render::measure_context& mc, const int width_limit) const override
		{
			return calc_flex_layout(_children, mc, {width_limit, -1}, container_layout(mc)).extent;
		}

		void layout(render::measure_context& mc, const core::recti bounds_in) override
		{
			update_style_from_children();
			bounds = bounds_in;
			layout_flex_elements(_children, mc, bounds, container_layout(mc));
		}

		void update_style_from_children()
		{
			auto has_tooltip = false;
			auto can_invoke = false;

			for (const auto& c : _children)
			{
				has_tooltip |= c->has_tooltip();
				can_invoke |= c->can_invoke();
			}

			set_style_bit(element_style::has_tooltip, has_tooltip);
			set_style_bit(element_style::can_invoke, can_invoke);
		}

		void tooltip(hover_bubble& hover, const core::pointi loc, const core::pointi element_offset) const override
		{
			for (const auto& c : _children)
			{
				if (c->is_visible() && c->bounds.offset(element_offset).contains(loc))
				{
					c->tooltip(hover, loc, element_offset);
				}
			}
		}

		void hover(interaction_context& ic) override
		{
			for (const auto& c : _children)
			{
				c->hover(ic);
			}
		}


		void dispatch_event(const element_event& event) override
		{
			for (const auto& c : _children)
			{
				c->dispatch_event(event);
			}
		}

		void visit_children(const std::function<void(const element_ptr&)>& visit) const override
		{
			for (const auto& c : _children)
			{
				visit(c);
			}
		}

	protected:
		// Metrics such as a gap in device pixels are only known once there is a measure context, so a
		// derived container folds them in here rather than caching a scale-dependent value.
		virtual flex_container_layout container_layout(const render::measure_context& mc) const
		{
			return flex_container;
		}
	};

	// An element that owns a frame of its own - a pane of results, a map, one of the regions an application's
	// window holds - which its frame places where the element is laid out after each layout, and hides with
	// it. Measured as flex sizes it unless a derived element says otherwise.
	class frame_element : public element
	{
	public:
		// The frame placed where this element is; null until there is one.
		frame_ptr child;

		frame_element() noexcept
		{
			kind = element_kind::frame;
		}

		explicit frame_element(const element_options& options) noexcept : element(options)
		{
			kind = element_kind::frame;
		}

		virtual frame_ptr placed_frame() const
		{
			return child;
		}
	};

	using frame_element_ptr = std::shared_ptr<frame_element>;

	// Draws into another context with every coordinate moved by origin, and clips in its own terms: what an
	// element draws at the content coordinates of a region of its frame, or a frame that is a region of its
	// window. The origin is shared so that vertices made through it move with it.
	class offset_draw_context final : public render::draw_context
	{
		render::draw_context& _dc;
		std::shared_ptr<core::pointi> _origin;

		core::recti to_dc(const core::recti r) const
		{
			return r.offset(*_origin);
		}

	public:
		offset_draw_context(render::draw_context& dc, std::shared_ptr<core::pointi> origin);

		offset_draw_context(render::draw_context& dc, const core::pointi origin) :
			offset_draw_context(dc, std::make_shared<core::pointi>(origin))
		{
		}

		void clear(core::color c) override;
		void draw_rounded_rect(core::recti bounds, core::color c, int radius) override;
		void fill_polygon(std::span<const core::pointd> points, core::color c) override;
		void draw_shadow(core::recti bounds, int width, float alpha) override;
		void draw_rect(core::recti bounds, core::color c) override;
		void draw_line(core::pointd a, core::pointd b, core::color c, double width = 1.0) override;
		void draw_polyline(std::span<const core::pointd> points, core::color c, double width = 1.0) override;
		void draw_ellipse(core::recti bounds, core::color c, double width = 1.0) override;
		void fill_ellipse(core::recti bounds, core::color c) override;
		void draw_text(std::string_view text, core::recti bounds, render::font_face font, render::text_style style, core::color c,
		               core::color bg) override;
		void draw_text(std::string_view text, const std::vector<render::text_highlight_t>& highlights, core::recti bounds,
		               render::font_face font, render::text_style style, core::color clr, core::color bg) override;
		void draw_text(const render::text_layout_ptr& tl, core::recti bounds, core::color clr, core::color bg) override;
		void draw_border(core::recti inside, core::recti outside, core::color c_inside, core::color c_outside) override;
		void draw_texture(const render::texture_ptr& t, core::recti dst, float alpha, render::texture_sampler sampler) override;
		void draw_texture(const render::texture_ptr& t, core::recti dst, core::recti src, float alpha, render::texture_sampler sampler,
		                  float radius) override;
		void draw_texture(const render::texture_ptr& t, const core::quadd& dst, core::recti src, float alpha,
		                  render::texture_sampler sampler) override;
		void draw_texture(const render::texture_ptr& t, const core::quadd& dst, core::recti src, float alpha,
		                  render::texture_sampler sampler, const render::texture_transform& transform) override;
		bool draw_panorama(const render::texture_ptr& t, core::recti dst, const render::panorama_params& params, float alpha) override;
		void draw_vertices(const render::vertices_ptr& v) override;
		core::sizei measure_text(std::string_view text, render::font_face font, render::text_style style, int width,
		                   int height) override;
		int text_line_height(render::font_face font) override;
		int text_baseline(render::font_face font) override;
		render::texture_ptr create_texture() override;
		render::vertices_ptr create_vertices() override;
		render::text_layout_ptr create_text_layout(render::font_face font) override;
		core::recti clip_bounds() const override;
		void clip_bounds(core::recti r) override;
		void restore_clip() override;
	};

	// What a hover shows in a bubble and where: the content, the bounds it points at, the region
	// it stays valid for, and the command it describes, whose tooltip the host supplies.
	struct hover_bubble
	{
		static constexpr int default_preferred_size = 300;

		elements_ptr elements;
		core::recti window_bounds;
		core::recti active_bounds;
		uint32_t active_delay = 0;
		int x_focus = -1;
		int preferred_size = default_preferred_size;
		bool horizontal = false;
		commands id = {};

		hover_bubble() = default;

		bool is_empty() const
		{
			return elements == nullptr || elements->is_empty();
		}

		void clear()
		{
			elements = std::make_shared<ui::elements>();
			window_bounds.clear();
			active_bounds.clear();
			x_focus = -1;
			preferred_size = default_preferred_size;
			horizontal = false;
			id = {};
		}
	};

	class clickable_controller final : public controller
	{
		const element_ptr _element;
		const core::pointi _element_offset;
		bool _tracking = false;
		bool _hover = false;
		bool _can_click = false;

	public:
		clickable_controller(const element_host_ptr& host, element_ptr e, const core::pointi element_offset) :
			controller(host, e->bounds.offset(element_offset)), _element(std::move(e)),
			_element_offset(element_offset), _can_click(_element->can_invoke())
		{
			update_highlight();
		}

		// For an element the controller owns only part of, such as one tile of a grid. The bounds are
		// the part, not the element clipped by what covers it -- clipping here drops the click.
		clickable_controller(const element_host_ptr& host, element_ptr e, const core::pointi element_offset,
		                     const core::recti bounds) : controller(host, bounds), _element(std::move(e)),
		                                           _element_offset(element_offset), _can_click(_element->can_invoke())
		{
			update_highlight();
		}

		~clickable_controller() override
		{
			if (_tracking)
			{
				_tracking = false;
			}

			if (_element)
			{
				interaction_context ic{{-1, -1}, _element_offset, _tracking};
				_element->hover(ic);
			}

			_hover = false;

			update_highlight();
		}

		void on_mouse_left_button_down(const core::pointi loc, const platform::key_state keys) override
		{
			_last_loc = loc;
			_hover = _bounds.contains(loc);
			_tracking = true;
			interaction_context ic{loc, _element_offset, _tracking};
			update_highlight();
			_element->hover(ic);

			if (ic.redraw)
			{
				_host->frame()->invalidate(_element->invalidate_bounds(_element_offset));
			}
		}

		void on_mouse_move(const core::pointi loc) override
		{
			_last_loc = loc;
			_hover = _bounds.contains(loc);
			interaction_context ic{loc, _element_offset, _tracking};
			update_highlight();
			_element->hover(ic);

			if (ic.redraw)
			{
				_host->frame()->invalidate(_element->invalidate_bounds(_element_offset));
			}
		}

		void on_mouse_left_button_up(const core::pointi loc, const platform::key_state keys) override
		{
			_last_loc = loc;
			_hover = _bounds.contains(loc);
			const auto can_invoke = _tracking && _hover && _can_click;
			_tracking = false;
			interaction_context ic{loc, _element_offset, _tracking};
			update_highlight();
			_element->hover(ic);

			if (ic.redraw)
			{
				_host->frame()->invalidate(_element->invalidate_bounds(_element_offset));
			}

			if (can_invoke)
			{
				const element_event e1{element_event_type::click, _host};
				const element_event e2{element_event_type::invoke, _host};

				_element->dispatch_event(e1);
				_element->dispatch_event(e2);
			}
		}

		void on_mouse_left_button_double_click(const core::pointi loc, const platform::key_state keys) override
		{
			if (_can_click)
			{
				const element_event e{element_event_type::double_click, _host};
				_element->dispatch_event(e);
			}
		}

		// Escape cancels a press the same way it cancels a drag. The button is still down, and the
		// release that follows must not invoke an action the user has just backed out of - by then
		// Escape may have changed the view the click was made in.
		bool escape() override
		{
			if (!_tracking) return false;

			_tracking = false;
			_hover = false;
			interaction_context ic{{-1, -1}, _element_offset, _tracking};
			update_highlight();
			_element->hover(ic);

			if (ic.redraw)
			{
				_host->frame()->invalidate(_element->invalidate_bounds(_element_offset));
			}

			return true;
		}

		platform::cursor cursor() const override
		{
			return _can_click ? platform::cursor::link : platform::cursor::normal;
		}

		void popup_from_location(hover_bubble& hover) override
		{
			_element->tooltip(hover, _last_loc, _element_offset);
		}

		void update_highlight() const
		{
			if (_element)
			{
				const auto hover = _element->is_style_bit_set(element_style::hover);

				if (hover != _hover) // || _tracking != tracking)
				{
					_element->set_style_bit(element_style::hover, _hover, _host, _element);
					_host->frame()->invalidate(_element->invalidate_bounds(_element_offset));
				}
			}
		}
	};

	template <class T>
	static controller_ptr default_controller_from_location(T& this_element, const element_host_ptr& host,
	                                                       const core::pointi loc, const core::pointi element_offset,
	                                                       hit_test_context& ctx)
	{
		if (this_element.is_visible() && (this_element.can_invoke() || this_element.has_tooltip()) &&
			this_element.bounds.contains(loc - element_offset))
		{
			return std::make_shared<clickable_controller>(host, this_element.shared_from_this(), element_offset);
		}

		return nullptr;
	}

	class text_element_base : public element
	{
	protected:
		std::string _text;
		render::font_face _font = render::font_face::dialog;
		render::text_style _text_style = render::text_style::multiline;
		core::color32 _foreground_clr = 0;

		mutable render::text_layout_ptr _tl;

	public:
		text_element_base(const std::string_view text, const element_options& options = {}) noexcept :
			element(options), _text(text)
		{
		}

		void foreground_color(const core::color32 clr)
		{
			_foreground_clr = clr;
		}

		void text(const std::string_view t)
		{
			_text = t;

			if (_tl)
			{
				_tl->update(_text, _text_style);
			}
		}

		void render(render::draw_context& dc, const core::pointi element_offset) const override
		{
			render_background(dc, element_offset);
			draw_text_content(dc, bounds.offset(element_offset));
		}

		core::sizei measure(render::measure_context& mc, const int width_limit) const override
		{
			const auto& tl = text_layout(mc);
			return tl ? tl->measure_text(width_limit) : core::sizei{};
		}

		void dispatch_event(const element_event& event) override
		{
			if (event.type == element_event_type::dpi_changed)
			{
				_tl.reset();
			}
		}

	protected:
		// Text layout creation can fail when the graphics device is lost, so every caller checks the result.
		const render::text_layout_ptr& text_layout(render::measure_context& mc) const
		{
			if (!_tl)
			{
				_tl = mc.create_text_layout(_font);
				if (_tl) _tl->update(_text, _text_style);
			}

			return _tl;
		}

		void draw_text_content(render::draw_context& dc, const core::recti logical_bounds) const
		{
			const auto& tl = text_layout(dc);

			if (tl)
			{
				const auto clr = _foreground_clr
					                 ? core::color(_foreground_clr, dc.colors.alpha)
					                 : core::color(dc.colors.foreground, dc.colors.alpha);

				dc.draw_text(tl, logical_bounds, clr, {});
			}
		}
	};

	class text_element final : public std::enable_shared_from_this<text_element>, public text_element_base
	{
	public:
		text_element(const std::string_view text, const render::font_face font, const render::text_style text_style,
		             const element_options& options) noexcept : text_element_base(text, options)
		{
			_font = font;
			_text_style = text_style;

			update_background_color();
		}

		text_element(const std::string_view text, const render::text_style text_style) noexcept : text_element_base(
			text)
		{
			_text_style = text_style;
		}

		text_element(const std::string_view text, const render::font_face font) noexcept : text_element_base(text)
		{
			_font = font;
		}

		text_element(const std::string_view text, const element_options& options) noexcept : text_element_base(
			text, options)
		{
		}

		text_element(const std::string_view text) noexcept : text_element_base(text)
		{
		}
	};

	inline core::color32 link_foreground_color()
	{
		return core::average(ui::style::colors.dialog_text, ui::style::colors.dialog_selected_background);
	}

	class link_element final : public std::enable_shared_from_this<link_element>, public text_element_base
	{
		commands _cmd = {};
		std::function<void()> _invoke;
		std::function<void(hover_bubble&)> _tooltip;
		bool _full_background = false;

	public:
		link_element(const std::string_view text, const commands cmd, const render::font_face font,
		             const render::text_style text_style,
		             const element_options& options = {},
		             const bool full_background = false) noexcept : text_element_base(
			                                                            text, options), _cmd(cmd),
		                                                            _full_background(full_background)
		{
			_font = font;
			_text_style = text_style;
			update_style();
		}

		link_element(const std::string_view text, std::function<void()> func, const render::font_face font,
		             const render::text_style text_style,
		             const element_options& options = {},
		             const bool full_background = false) noexcept :
			text_element_base(text, options), _invoke(std::move(func)), _full_background(full_background)
		{
			_font = font;
			_text_style = text_style;
			update_style();
		}

		link_element(const std::string_view text, std::function<void()> func,
		             std::function<void(hover_bubble&)> tooltip,
		             const render::font_face font, const render::text_style text_style,
		             const element_options& options = {}) noexcept :
			text_element_base(text, options), _invoke(std::move(func)), _tooltip(std::move(tooltip))
		{
			_font = font;
			_text_style = text_style;
			update_style();
		}

		link_element(const std::string_view text, const commands cmd) noexcept : text_element_base(text), _cmd(cmd)
		{
			update_style();
		}

		link_element(const std::string_view text, std::function<void()> func) noexcept : text_element_base(text),
			_invoke(std::move(func))
		{
			update_style();
		}

		link_element(const std::string_view text) noexcept : text_element_base(text)
		{
			update_style();
		}

		void update_style()
		{
			set_style_bit(element_style::has_tooltip, _cmd != commands{} || _tooltip);
			set_style_bit(element_style::can_invoke, _cmd != commands{} || _invoke);

			_foreground_clr = link_foreground_color();
		}

		// A link may be asked to fill its own background - a box rather than a run of underlined words -
		// while still carrying a hover bubble.
		void full_background(const bool v)
		{
			_full_background = v;
		}

		void render(render::draw_context& dc, const core::pointi element_offset) const override
		{
			if (_full_background)
			{
				const auto logical_bounds = bounds.offset(element_offset);
				dc.draw_rounded_rect(logical_bounds, calc_background_color(dc), dc.padding1);
				draw_text_content(dc, logical_bounds);
			}
			else
			{
				text_element_base::render(dc, element_offset);
			}
		}

		void update_check(const element_event& event);
		void dispatch_event(const element_event& event) override;
		void tooltip(hover_bubble& hover, core::pointi loc, core::pointi element_offset) const override;

		controller_ptr controller_from_location(const element_host_ptr& host, core::pointi loc, core::pointi element_offset,
		                                        hit_test_context& ctx) override;
	};

	inline element_ptr make_icon_element(const icon_index i, const element_options& options)
	{
		return std::make_shared<text_element>(icon_to_utf8(i), render::font_face::icons,
		                                      render::text_style::single_line_center, options);
	}

	inline element_ptr make_icon_element(const icon_index i, const size_t repeat, const element_options& options)
	{
		return std::make_shared<text_element>(icon_to_utf8(i, repeat), render::font_face::icons,
		                                      render::text_style::single_line_center, options);
	}

	inline element_ptr make_icon_link_element(const icon_index i, commands cmd, const element_options& options)
	{
		return std::make_shared<link_element>(icon_to_utf8(i), cmd, render::font_face::icons,
		                                      render::text_style::single_line_center, options);
	}

	inline element_ptr make_icon_link_element(const icon_index i, const size_t repeat,
	                                          const std::function<void()>& func,
	                                          const std::function<void(hover_bubble&)>& tooltip)
	{
		return std::make_shared<link_element>(icon_to_utf8(i, repeat), func, tooltip, render::font_face::icons,
		                                      render::text_style::single_line_center, element_style::none);
	}

	inline void draw_icon(render::draw_context& dc, const icon_index i, const core::recti bounds, const core::color c,
	                      const core::color bg, const render::font_face font = render::font_face::icons)
	{
		dc.draw_text(icon_to_utf8(i), bounds, font, render::text_style::single_line_center, c, bg);
	}

	class divider_element final : public std::enable_shared_from_this<divider_element>, public element
	{
	public:
		divider_element() noexcept : element(flex_item::stretch)
		{
		}

		void render(render::draw_context& dc, const core::pointi element_offset) const override
		{
			const auto line_height = core::round(1.1 * dc.scale_factor);
			auto logical_bounds = bounds.offset(element_offset);
			logical_bounds.top = (logical_bounds.top + logical_bounds.bottom - line_height) / 2;
			logical_bounds.bottom = logical_bounds.top + line_height;

			const auto clr = core::color(0x000000, dc.colors.alpha / 4.44f);
			dc.draw_rect(logical_bounds, clr);
		}

		core::sizei measure(render::measure_context& mc, const int width_limit) const override
		{
			return {width_limit, mc.padding2};
		}
	};

	class padding_element final : public std::enable_shared_from_this<padding_element>, public element
	{
	public:
		padding_element(const int height = 0) noexcept : element(flex_item::grow | flex_item::stretch), _height(height)
		{
		}

		core::sizei measure(render::measure_context& mc, const int width_limit) const override
		{
			return {width_limit, _height ? _height : mc.padding2};
		}

		int _height;
	};
}
