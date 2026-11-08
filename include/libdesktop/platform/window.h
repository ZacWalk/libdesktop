// This file is part of libdesktop, a C++20 platform layer for desktop applications
// Copyright 2026  Zac Walker
//
// This program is free software; you can redistribute it and / or modify it
// under the terms of the LGPL License either version 2.1 or later.
// License details are available at https://www.gnu.org/licenses/lgpl-2.1.html
// This program is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY

// Purpose: A native window and the events it reports. The platform never draws into a window -
// render binds a device to it - and knows nothing of what is shown there: ui's frames and an
// application that draws for itself both implement window_events.

#pragma once

namespace platform
{
	enum class window_kind
	{
		// The application's top-level window: its caption, its place on the taskbar, and the system's
		// messages - power, session, theme and devices - reported as system_event.
		app,
		// An owned top-level window over another. A modal wait is the caller's (run_modal_loop).
		dialog,
		// An owned popup that never activates: a menu, a completion list, a tooltip or a bubble.
		popup,
		// A region of its parent with a native surface of its own, where child_windows() is true.
		child,
	};

	// Windows gives a child window a surface of its own; SDL does not, so there a caller composes
	// child regions into its parent's surface itself.
	bool child_windows();

	// What a point in a window with a drawn caption is, as its events answer it. The resize edges are
	// the platform's own, from the window's state and DPI, and are never asked.
	enum class hit_region
	{
		client,
		// A drawn control is under the pointer; it takes precedence over the resize band and caption.
		control,
		caption,
		// Reported to the system as the maximize button, which is what Windows 11 offers snap
		// layouts over; its hover, press and release arrive as caption_button events.
		maximize_button,
	};

	enum class pointer_button
	{
		left,
		middle,
		right,
		x1,
		x2
	};

	enum class caption_button_action
	{
		hover,
		leave,
		press,
		release
	};

	struct window_placement
	{
		core::recti bounds;
		bool maximized = false;
	};

	// The native placement setting name is platform-specific while the UI remains portable.
	std::string_view placement_setting_name();
	std::optional<window_placement> read_placement(const setting_file_ptr& store, std::string_view name);

	struct window_options
	{
		window_kind kind = window_kind::popup;
		// The owner of a dialog or popup, or the parent of a child; null for the application's window.
		class window* parent = nullptr;
		// Screen coordinates, or the parent's client coordinates for a child.
		core::recti bounds;
		// Initial client size in physical client pixels when bounds is empty.
		core::sizei client_size;
		bool visible = false;
		// A real window that paints without appearing or taking focus.
		bool offscreen = false;
		bool maximized = false;
		// An owned popup menu; each platform preserves its native menu focus behaviour.
		bool menu = false;
		// Additional UI scale (for example the large-text preference) used by the resize grab band.
		double resize_scale = 1.0;
		// Whether an ordinary popup may activate and take the keyboard; menu popups use native menu behaviour.
		bool can_focus = true;
		// A popup drawn with per-pixel alpha that the pointer passes through: a bubble.
		bool layered = false;
		bool accepts_drops = false;
		// The application's window draws its own caption; hit_test says where it is.
		bool drawn_caption = false;
		bool resizable = true;
		// Draws nothing itself: the platform fills it with background where its children leave it
		// uncovered, and it is never sent paint.
		bool container = false;
		core::color32 background = 0;
		// Minimum client size in physical client pixels, matching client_extent().
		core::sizei minimum_client;
		std::string title;
	};

	class window;

	// What a native window reports, on the UI thread, in client pixels unless named otherwise.
	class window_events
	{
	public:
		virtual ~window_events() = default;

		virtual void created(window& w) {}
		virtual void destroyed() {}

		virtual void size_changed(core::sizei client, bool minimized) {}
		// Empty damage is the whole client area.
		virtual void paint(core::recti damage) {}
		virtual void scale_changed(double scale) {}

		virtual void pointer_move(core::pointi loc, key_state keys) {}
		virtual void pointer_leave() {}
		virtual void pointer_down(pointer_button button, core::pointi loc, key_state keys) {}
		virtual void pointer_up(pointer_button button, core::pointi loc, key_state keys) {}
		virtual void double_click(pointer_button button, core::pointi loc, key_state keys) {}
		// True when handled; delta is in the platform's wheel units, 120 to a detent.
		virtual bool wheel(core::pointi loc, int delta, wheel_axis axis, key_state keys) { return false; }
		virtual bool pinch(core::pointi loc, int delta, bool begins) { return false; }
		virtual void pan_start(core::pointi loc) {}
		virtual void pan(core::pointi start, core::pointi current) {}
		virtual void pan_end(core::pointi start, core::pointi end) {}
		virtual bool touch_double_tap(core::pointi loc) { return false; }

		// Keys carry the platform's key codes (keys:: values). System keys - Alt and F10 combinations -
		// get the platform's default handling only when this answers false.
		virtual bool key_down(int key, key_state keys, bool system) { return false; }
		virtual bool key_up(int key, key_state keys, bool system) { return false; }
		// Typed text, already assembled from surrogate pairs and the input method.
		virtual void text(std::string_view utf8) {}

		virtual void focus_changed(bool has_focus) {}
		virtual void activated(bool active) {}
		// Closing is the handler's decision: it calls window::destroy when it agrees.
		virtual void close_requested() {}
		virtual void context_menu(core::pointi screen, bool from_keyboard) {}
		// The pointer capture was taken away.
		virtual void cancel_mode() {}
		virtual void timer(uint32_t id) {}

		virtual hit_region hit_test(core::pointi loc) { return hit_region::client; }
		virtual void caption_button(caption_button_action action, core::pointi loc) {}

		virtual drop_effect drag_over(const clipboard_data& data, key_state keys, core::pointi loc)
		{
			return drop_effect::none;
		}

		virtual drop_effect drag_drop(clipboard_data& data, key_state keys, core::pointi loc)
		{
			return drop_effect::none;
		}

		virtual void drag_leave() {}

		// The application's window only: power, session, theme, display and device changes.
		virtual void system_event(os_event_type e) {}
	};

	class window
	{
	public:
		virtual ~window() = default;

		// The native window - an HWND or an SDL_Window* - for an application's own backend.
		virtual std::any native_handle() const = 0;
		virtual window_kind kind() const = 0;
		virtual window* root() { return this; }
		virtual void destroy() = 0;

		// Screen coordinates, or the parent's client coordinates for a child.
		virtual core::recti bounds() const = 0;
		virtual void set_bounds(core::recti bounds, bool visible) = 0;
		virtual core::sizei client_extent() const = 0;
		virtual void show(bool show) = 0;
		virtual bool is_visible() const = 0;
		virtual void enable(bool enable) = 0;
		virtual bool is_enabled() const = 0;
		virtual void set_title(std::string_view text) = 0;
		virtual std::string title() const = 0;

		virtual void focus() = 0;
		virtual bool has_focus() const = 0;
		virtual void capture(bool capture) = 0;
		virtual void set_cursor(cursor c) = 0;
		virtual core::pointi cursor_location() const = 0;
		virtual core::pointi client_to_screen(core::pointi p) const = 0;
		virtual core::pointi screen_to_client(core::pointi p) const = 0;
		virtual double scale() const = 0;
		// The additional UI scale used by the top-level resize grab band.
		virtual void set_resize_scale(double scale) = 0;

		// Empty bounds is the whole client area.
		virtual void invalidate(core::recti bounds = {}) = 0;
		// Paints whatever is invalid now, rather than when the queue next gets to it.
		virtual void update_now() = 0;
		virtual void scroll(int dx, int dy, core::recti bounds) = 0;
		virtual void start_timer(uint32_t id, uint32_t milliseconds) = 0;
		virtual void stop_timer(uint32_t id) = 0;

		virtual bool is_minimized() const = 0;
		virtual bool is_maximized() const = 0;
		// Nothing drawn now could reach the screen: minimized, cloaked, or behind a secure desktop.
		virtual bool is_occluded() const = 0;
		virtual void sys_command(sys_command_type command) = 0;
		virtual void full_screen(bool full) = 0;
		// Whether this window's toolbar maximize command may be reported as its native caption button.
		virtual bool presents_caption_button() const { return false; }
		virtual bool is_full_screen() const = 0;

		// Where the input method places its window, while a control takes typing.
		virtual void set_text_input(bool active, core::recti caret) = 0;
		// The application's window and dialogs remember where they were: read_placement gives the saved
		// placement back for the bounds a window is made at.
		virtual void save_placement(const setting_file_ptr& store, std::string_view name) = 0;
	};

	using window_ptr = std::shared_ptr<window>;

	struct window_move
	{
		window* target = nullptr;
		core::recti bounds;
		bool visible = false;
	};

	// Places a host's native children as one operation, deferring redraw until the batch is complete.
	void defer_window_moves(std::span<const window_move> moves);

	// Null when the system refuses one - desktop heap exhaustion, or a session that is ending.
	window_ptr create_window(const window_options& options, std::weak_ptr<window_events> events);

	// The work area of the monitor that bounds, in screen coordinates, falls on most.
	core::recti monitor_work_area(core::recti bounds);
}
