// This file is part of libdesktop, a C++20 platform layer for desktop applications
// Copyright 2026  Zac Walker
// 
// This program is free software; you can redistribute it and / or modify it
// under the terms of the LGPL License either version 2.1 or later.
// License details are available at https://www.gnu.org/licenses/lgpl-2.1.html
// This program is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY

// Purpose: UI frame hosts, windows, modal bubbles, and focus snapshots.

#pragma once

namespace ui
{
	enum class close_result
	{
		ok,
		cancel
	};

	class frame_host
	{
	public:
		virtual ~frame_host() = default;

		virtual void on_window_layout(render::measure_context& mc, core::sizei extent, bool is_minimized) = 0;
		virtual void on_window_paint(render::draw_context& dc) = 0;

		virtual void on_window_destroy()
		{
		}

		virtual void on_mouse_move(const core::pointi loc, bool is_tracking)
		{
		}

		virtual void on_mouse_left_button_down(const core::pointi loc, const platform::key_state keys)
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

		// The right button's press and release, for a host that uses the button itself (an emulated
		// mouse). A host's context menu still comes from menu(), which the platform raises separately.
		virtual void on_mouse_right_button_down(const core::pointi loc, const platform::key_state keys)
		{
		}

		virtual void on_mouse_right_button_up(const core::pointi loc, const platform::key_state keys)
		{
		}

		virtual void on_mouse_leave(const core::pointi loc)
		{
		}

		virtual void on_mouse_wheel(const core::pointi loc, const int delta, const platform::key_state keys, bool& was_handled)
		{
		}

		virtual void on_mouse_hwheel(const core::pointi loc, const int delta, const platform::key_state keys, bool& was_handled)
		{
		}

		// A pinch is unambiguous, so it arrives as itself rather than as a wheel wearing a modifier:
		// the modifier then had to be disambiguated by what the pointer was over, and over a grid it
		// meant something else entirely. `begins` opens a gesture, so the accumulator carrying its
		// sub-detent remainder belongs to that gesture rather than to the window.
		virtual void on_pinch(const core::pointi loc, const int delta, const bool begins, bool& was_handled)
		{
		}

		virtual void on_mouse_left_button_double_click(const core::pointi loc, const platform::key_state keys)
		{
		}

		virtual void on_mouse_other_button_up(
			const platform::other_mouse_button& button, const core::pointi loc, const platform::key_state keys)
		{
		}

		virtual void pan_start(const core::pointi start_loc)
		{
		}

		virtual void pan(const core::pointi start_loc, const core::pointi current_loc)
		{
		}

		virtual void pan_end(const core::pointi start_loc, const core::pointi final_loc)
		{
		}

		// Returns false when the tap has no touch-specific meaning, so the caller runs the normal double-click.
		virtual bool touch_double_tap(const core::pointi location)
		{
			return false;
		}

		// Told of the keyboard coming to or leaving one of its controls.
		virtual void focus_changed(const bool has_focus, const control_ptr& child)
		{
		}

		// Told of the keyboard coming to or leaving a pane, a frame of its own; as any other change of
		// focus unless the host needs to know which.
		virtual void pane_focus_changed(const bool has_focus, const frame_ptr& pane)
		{
			focus_changed(has_focus, nullptr);
		}

		virtual void tick()
		{
		}

		virtual void activate(bool is_active)
		{
		}

		virtual bool key_down(const int c, const platform::key_state keys)
		{
			return false;
		}

		virtual void command_hover(const command_ptr& c, const core::recti window_bounds)
		{
		}

		virtual std::vector<command_ptr> menu(const core::pointi loc)
		{
			return {};
		}

		virtual platform::drop_effect drag_over(const platform::clipboard_data& data, const platform::key_state keys,
		                                        const core::pointi loc)
		{
			return platform::drop_effect::none;
		}

		virtual platform::drop_effect drag_drop(platform::clipboard_data& data, const platform::key_state keys, const core::pointi loc)
		{
			return platform::drop_effect::none;
		}

		virtual void drag_leave()
		{
		}

		virtual bool is_control_area(const core::pointi loc)
		{
			return true;
		}

		virtual bool is_caption_area(const core::pointi loc) const
		{
			return false;
		}

		// Whether a press at loc on the frame's own ground - not on a control - gives the frame itself the
		// keyboard, where its style says a press does: a region of controls drawn beside the frame's content
		// answers false, so a press between its controls leaves the keyboard with them.
		virtual bool takes_keyboard_at(const core::pointi loc) const
		{
			return true;
		}

		virtual void dpi_changed()
		{
		}

		// The elements this host draws, each with where its content coordinates sit in the frame - a
		// scrolled region's offset included. The frame finds what it serves among them, and their
		// descendants: the drawn controls, which take the pointer under them, the keyboard when focused
		// and Tab in this order, and the frame elements, whose frames it places after each layout.
		virtual void visit_elements(const std::function<void(const element_ptr&, core::pointi offset)>& visit)
		{
		}
	};

	struct frame_style
	{
		bool hardware_accelerated = false;
		bool child = true;
		bool no_focus = false;
		// Takes the keyboard with a press on its own ground, as a view does; otherwise only its controls do.
		bool can_focus = false;
		bool can_drop = false;
		// A popup drawn with per-pixel alpha that the pointer passes through, faded in and out where
		// animations run: a bubble.
		bool layered = false;
		// Holds frames and draws nothing of its own, so no surface is made for it: where its frames leave
		// it uncovered it shows its background colour, as a window's border.
		bool container = false;
		// The application draws its caption; false leaves the platform title bar and system menu.
		bool drawn_caption = true;
		// Initial client size in physical client pixels when the placement is empty.
		core::sizei client_size;
		// Minimum client size in physical client pixels, matching platform::window_options.
		core::sizei minimum_client;
		// A real application window that paints without appearing or taking focus.
		bool offscreen = false;
		bool resizable = true;

		// How often a pane ticks; 0 never. A window ticks default_ticks_per_second times a second
		// whatever this says, because that is the idle count its host reads: an application that wants
		// faster frames sets platform::set_frame_delay and does its work in frame_tick.
		int timer_milliseconds = 0;

		render::color_style colors = {
			style::colors.dialog_background, style::colors.dialog_text, style::colors.dialog_selected_background
		};
	};


	// The radio buttons a frame shows check one per group. An application with more than one set in a
	// frame gives each its own id.
	constexpr int radio_group_default = 0;

	// A window, or a region of one, that draws what its host draws: the host's elements, the drawn controls
	// among them, and the frames the frame elements among them place. The application's window, a dialog,
	// a view's pane, a menu's popup and a bubble are all frames, the same on every platform, and any of them
	// can make frames of its own.
	class frame
	{
	public:
		virtual ~frame() = default;

		// The platform's window, where there is one.
		virtual std::any handle() const = 0;
		virtual void destroy() = 0;
		virtual void enable(bool enable) = 0;
		virtual std::string window_text() const = 0;
		virtual void window_text(std::string_view val) = 0;
		virtual void focus() = 0;
		virtual bool is_visible() const = 0;
		virtual bool has_focus() const = 0;
		virtual core::recti window_bounds() const = 0;
		virtual void window_bounds(core::recti bounds, bool visible) = 0;
		virtual void show(bool show) = 0;
		virtual void options_changed() = 0;

		virtual void invalidate(core::recti bounds = {}, bool erase = false) = 0;
		virtual void layout() = 0;
		virtual void redraw() = 0;
		virtual void redraw_now() = 0;
		virtual void scroll(int dx, int dy, core::recti bounds) = 0;
		virtual void track_menu(core::recti button_bounds, const std::vector<command_ptr>& buttons) = 0;
		virtual void close(bool is_cancel = false) = 0;

		virtual bool is_enabled() const = 0;
		virtual bool is_maximized() const = 0;
		virtual bool is_occluded() const = 0;
		virtual void set_cursor(platform::cursor cursor) = 0;
		virtual void reset_graphics() = 0;
		virtual core::pointi cursor_location() = 0;
		virtual double scale_factor() const = 0;
		virtual void sys_command(platform::sys_command_type command) = 0;
		virtual void full_screen(bool full) = 0;

		// Gives the keyboard to the first edit or button among the host's controls.
		virtual void focus_first() = 0;
		// The drawn control that holds this frame's keyboard focus, or null.
		virtual control_ptr focused_control() const = 0;

		// Frames of its own: a dialog, a window of its own over this one; a frame of any style; and a bubble.
		virtual frame_ptr create_dlg(frame_host_weak_ptr host) = 0;
		virtual frame_ptr create_frame(frame_host_weak_ptr host, const frame_style& ft) = 0;
		virtual bubble_window_ptr create_bubble() = 0;

		// A dialog's window: its modal wait, its answer and its place.
		virtual close_result wait_for_close(uint32_t timeout_ms) = 0;
		virtual bool is_canceled() const = 0;
		virtual void position(core::recti bounds) = 0;
		virtual void save_window_position(platform::setting_file_ptr& store) = 0;
	};

	frame_ptr create_app_frame(const std::shared_ptr<app>& app, const platform::setting_file_ptr& store,
	                           const frame_host_weak_ptr& host, const frame_style& style);

	class bubble_frame
	{
	public:
		virtual ~bubble_frame() = default;
		// Shows content, an element of the tree in ui_elements.h, beside bounds.
		virtual void show(const element_ptr& content, core::recti bounds, int x_center, int preferred_size,
		                  bool horizontal) = 0;
		virtual void hide() = 0;
		virtual bool is_visible() const = 0;
	};

	// A frame that does not exist: a host's before it is attached or after it is destroyed, or one whose
	// creation failed - desktop heap exhaustion, or a request made while the session is ending. Its host
	// is still populated, laid out, counted and invalidated, and the dozens of call sites that open a dialog
	// cannot each be asked to test for that, so this stands in for the absent window in one place. The
	// controls its host lays out belong to no window, so they hold what they are given and show nothing,
	// and a dialog on it reports itself cancelled, which is what a dialog that never appeared means. A test
	// overrides what it records.
	class null_frame : public frame
	{
		class null_bubble final : public bubble_frame
		{
		public:
			void show(const element_ptr&, core::recti, int, int, bool) override {}
			void hide() override {}
			bool is_visible() const override { return false; }
		};

	public:
		std::any handle() const override { return {}; }
		void destroy() override {}
		void enable(bool) override {}
		std::string window_text() const override { return {}; }
		void window_text(std::string_view) override {}
		void focus() override {}
		bool is_visible() const override { return false; }
		bool has_focus() const override { return false; }
		core::recti window_bounds() const override { return {}; }
		void window_bounds(core::recti, bool) override {}
		void show(bool) override {}
		void options_changed() override {}

		void invalidate(core::recti = {}, bool = false) override {}
		void layout() override {}
		void redraw() override {}
		void redraw_now() override {}
		void scroll(int, int, core::recti) override {}
		void track_menu(core::recti, const std::vector<command_ptr>&) override {}
		void close(bool = false) override {}

		bool is_enabled() const override { return false; }
		bool is_maximized() const override { return false; }
		// Callers use this to skip drawing, and nothing drawn into an absent window can be seen.
		bool is_occluded() const override { return true; }
		void set_cursor(platform::cursor) override {}
		void reset_graphics() override {}
		void sys_command(platform::sys_command_type) override {}
		void full_screen(bool) override {}
		// Outside every client rect, so hit testing against it resolves to no controller.
		core::pointi cursor_location() override { return {-1, -1}; }
		double scale_factor() const override { return 1.0; }

		void focus_first() override {}
		control_ptr focused_control() const override { return nullptr; }

		frame_ptr create_dlg(frame_host_weak_ptr) override;
		frame_ptr create_frame(frame_host_weak_ptr, const frame_style&) override;
		bubble_window_ptr create_bubble() override { return std::make_shared<null_bubble>(); }

		// A dialog that could not be shown is one the user did not agree to.
		close_result wait_for_close(uint32_t) override { return close_result::cancel; }
		bool is_canceled() const override { return true; }
		void position(core::recti) override {}
		void save_window_position(platform::setting_file_ptr&) override {}
	};

	// Stateless, so one instance serves every host that has no window.
	inline const frame_ptr& no_frame()
	{
		static const frame_ptr instance = std::make_shared<null_frame>();
		return instance;
	}

	inline frame_ptr null_frame::create_dlg(frame_host_weak_ptr)
	{
		return no_frame();
	}

	inline frame_ptr null_frame::create_frame(frame_host_weak_ptr, const frame_style&)
	{
		return no_frame();
	}

	// Where the keyboard is, kept to give it back once a dialog closes: the drawn control that holds it,
	// or the frame that does when no control does.
	struct focus_snapshot
	{
		std::weak_ptr<control> focused;
		frame_weak_ptr window;
	};

	focus_snapshot save_focus();
	void restore_focus(const focus_snapshot& snapshot);
}
