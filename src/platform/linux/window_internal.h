// This file is part of libdesktop, a C++20 platform layer for desktop applications
// Copyright 2026  Zac Walker
//
// This program is free software; you can redistribute it and / or modify it
// under the terms of the LGPL License either version 2.1 or later.
// License details are available at https://www.gnu.org/licenses/lgpl-2.1.html
// This program is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY

// Internal SDL window state shared by the Linux window and application-loop translation units.

#pragma once

#include "libdesktop/platform.h"

#include <SDL3/SDL.h>

namespace platform::linux_detail
{
	class sdl_window final : public platform::window, public std::enable_shared_from_this<sdl_window>
	{
		struct timer
		{
			uint32_t id = 0;
			uint64_t period = 0;
			uint64_t due = 0;
		};

		SDL_Window* _sdl = nullptr;
		SDL_WindowID _id = 0;
		platform::window_kind _kind = platform::window_kind::popup;
		std::weak_ptr<platform::window_events> _events;
		platform::window* _parent = nullptr;
		core::recti _bounds;
		core::sizei _extent;
		core::sizei _minimum_client;
		std::string _title;
		platform::cursor _cursor = platform::cursor::normal;
		std::map<uint32_t, timer> _timers;
		bool _visible = false;
		bool _enabled = true;
		bool _can_focus = true;
		bool _layered = false;
		bool _offscreen = false;
		bool _drawn_caption = false;
		bool _resizable = true;
		bool _container = false;
		bool _has_focus = false;
		bool _capture = false;
		bool _minimized = false;
		bool _maximized = false;
		bool _occluded = false;
		bool _full_screen = false;
		bool _text_input = false;
		bool _caption_hover = false;
		bool _caption_pressed = false;
		bool _dirty = false;
		bool _destroyed = false;
		core::recti _damage;
		core::recti _text_area;
		double _scale = 1.0;
		double _resize_scale = 1.0;
		float _density = 1.0f;

		void update_metrics(bool tell_events);
		void sync_geometry(bool tell_events);
		void notify_size(bool force = false);
		void paint_now();
		void update_caption_hover(core::pointi loc, platform::window_events& events);
		void handle_event(const SDL_Event& event);
		void native_destroyed();
		void release_capture(bool notify);
		core::pointi point_from_window(float x, float y) const;
		core::pointi screen_origin() const;
		SDL_HitTestResult hit_test(const SDL_Point* area) const;
		static SDL_HitTestResult SDLCALL hit_test_callback(SDL_Window* window, const SDL_Point* area, void* data);

		friend void register_window(sdl_window* window);
		friend void unregister_window(sdl_window* window);
		friend sdl_window* window_for(SDL_WindowID id);
		friend std::vector<sdl_window*> windows();

	public:
		sdl_window(const platform::window_options& options, std::weak_ptr<platform::window_events> events);
		~sdl_window() override;

		bool valid() const { return _sdl != nullptr; }
		SDL_Window* native() const { return _sdl; }
		sdl_window* parent_native() const;
		SDL_WindowID id() const { return _id; }
		bool dirty() const { return _dirty; }
		bool can_focus() const { return _can_focus; }
		bool has_capture() const { return _capture; }
		bool accepts_input() const { return _enabled && !_destroyed; }
		uint64_t next_timer_due() const;
		void fire_timers(uint64_t now);
		void dispatch(const SDL_Event& event) { handle_event(event); }
		core::pointi event_location(float x, float y) const { return point_from_window(x, y); }
		void shutdown_native();
		void notify_created() { if (const auto events = _events.lock()) events->created(*this); }

		std::any native_handle() const override;
		platform::window_kind kind() const override { return _kind; }
		void destroy() override;
		core::recti bounds() const override { return _bounds; }
		void set_bounds(core::recti bounds, bool visible) override;
		core::sizei client_extent() const override { return _extent; }
		void show(bool show) override;
		bool is_visible() const override { return _visible && !_destroyed; }
		void enable(bool enable) override { _enabled = enable; }
		bool is_enabled() const override { return _enabled && !_destroyed; }
		void set_title(std::string_view text) override;
		std::string title() const override { return _title; }
		void focus() override;
		bool has_focus() const override { return _has_focus; }
		void capture(bool capture) override;
		void set_cursor(platform::cursor cursor) override;
		core::pointi cursor_location() const override;
		core::pointi client_to_screen(core::pointi p) const override;
		core::pointi screen_to_client(core::pointi p) const override;
		double scale() const override { return _scale; }
		void set_resize_scale(double scale) override;
		void invalidate(core::recti bounds = {}) override;
		void update_now() override { paint_now(); }
		void scroll(int dx, int dy, core::recti bounds) override;
		void start_timer(uint32_t id, uint32_t milliseconds) override;
		void stop_timer(uint32_t id) override;
		platform::window* root() override;
		bool is_minimized() const override { return _minimized; }
		bool is_maximized() const override { return _maximized; }
		bool is_occluded() const override { return _offscreen ? !_sdl || _minimized : _occluded || _minimized || !_visible; }
		void sys_command(platform::sys_command_type command) override;
		void full_screen(bool full) override;
		bool is_full_screen() const override { return _full_screen; }
		void set_text_input(bool active, core::recti caret) override;
		void save_placement(const platform::setting_file_ptr& store, std::string_view name) override;
	};

	// The event loop owns the SDL-ID map; window.cpp uses these hooks without depending on its
	// application state or naming UI/render types.
	platform::key_state key_state_from_sdl(SDL_Keymod mod);
	char32_t key_from_sdl(SDL_Keycode key);

	void register_window(sdl_window* window);
	void unregister_window(sdl_window* window);
	sdl_window* window_for(SDL_WindowID id);
	std::vector<sdl_window*> windows();
}