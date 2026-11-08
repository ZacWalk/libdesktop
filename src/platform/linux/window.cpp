// This file is part of libdesktop, a C++20 platform layer for desktop applications
// Copyright 2026  Zac Walker
//
// This program is free software; you can redistribute it and / or modify it
// under the terms of the LGPL License either version 2.1 or later.
// License details are available at https://www.gnu.org/licenses/lgpl-2.1.html
// This program is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY

// Purpose: SDL's native window boundary. It owns no drawing surface: render binds a device to the
// SDL_Window* returned here, while this file translates SDL input and window state into neutral events.

#include "window_internal.h"

#include <SDL3/SDL.h>

#include <cstdio>

namespace
{
	std::map<SDL_SystemCursor, SDL_Cursor*> system_cursors;

	platform::key_state key_state_of(const SDL_Keymod mod)
	{
		platform::key_state result;
		result.control = (mod & SDL_KMOD_CTRL) != 0;
		result.shift = (mod & SDL_KMOD_SHIFT) != 0;
		result.alt = (mod & SDL_KMOD_ALT) != 0;
		return result;
	}

	char32_t to_key(const SDL_Keycode key)
	{
		if (key >= SDLK_A && key <= SDLK_Z) return U'A' + static_cast<char32_t>(key - SDLK_A);
		if (key >= SDLK_0 && key <= SDLK_9) return U'0' + static_cast<char32_t>(key - SDLK_0);
		if (key >= SDLK_F1 && key <= SDLK_F12) return 0x70 + static_cast<char32_t>(key - SDLK_F1);
		if (key >= SDLK_F13 && key <= SDLK_F24) return 0x7C + static_cast<char32_t>(key - SDLK_F13);
		if (key >= SDLK_KP_1 && key <= SDLK_KP_9) return 0x61 + static_cast<char32_t>(key - SDLK_KP_1);

		switch (key)
		{
		case SDLK_KP_0: return 0x60;
		case SDLK_BACKSPACE: return 0x08;
		case SDLK_TAB: return 0x09;
		case SDLK_RETURN:
		case SDLK_KP_ENTER: return 0x0D;
		case SDLK_LSHIFT:
		case SDLK_RSHIFT: return 0x10;
		case SDLK_LCTRL:
		case SDLK_RCTRL: return 0x11;
		case SDLK_LALT:
		case SDLK_RALT: return 0x12;
		case SDLK_PAUSE: return 0x13;
		case SDLK_CAPSLOCK: return 0x14;
		case SDLK_ESCAPE: return 0x1B;
		case SDLK_SPACE: return 0x20;
		case SDLK_PAGEUP: return 0x21;
		case SDLK_PAGEDOWN: return 0x22;
		case SDLK_END: return 0x23;
		case SDLK_HOME: return 0x24;
		case SDLK_LEFT: return 0x25;
		case SDLK_UP: return 0x26;
		case SDLK_RIGHT: return 0x27;
		case SDLK_DOWN: return 0x28;
		case SDLK_PRINTSCREEN: return 0x2C;
		case SDLK_INSERT: return 0x2D;
		case SDLK_DELETE: return 0x2E;
		case SDLK_APPLICATION: return 0x5D;
		case SDLK_KP_MULTIPLY: return 0x6A;
		case SDLK_KP_PLUS: return 0x6B;
		case SDLK_KP_MINUS: return 0x6D;
		case SDLK_KP_PERIOD: return 0x6E;
		case SDLK_KP_DIVIDE: return 0x6F;
		case SDLK_NUMLOCKCLEAR: return 0x90;
		case SDLK_SCROLLLOCK: return 0x91;
		case SDLK_AC_BACK: return 0xA6;
		case SDLK_AC_FORWARD: return 0xA7;
		case SDLK_AC_REFRESH: return 0xA8;
		case SDLK_AC_STOP: return 0xA9;
		case SDLK_AC_SEARCH: return 0xAA;
		case SDLK_AC_BOOKMARKS: return 0xAB;
		case SDLK_AC_HOME: return 0xAC;
		case SDLK_MUTE: return 0xAD;
		case SDLK_VOLUMEDOWN: return 0xAE;
		case SDLK_VOLUMEUP: return 0xAF;
		case SDLK_MEDIA_NEXT_TRACK: return 0xB0;
		case SDLK_MEDIA_PREVIOUS_TRACK: return 0xB1;
		case SDLK_MEDIA_STOP: return 0xB2;
		case SDLK_MEDIA_PLAY_PAUSE: return 0xB3;
		case SDLK_SEMICOLON: return 0xBA;
		case SDLK_EQUALS: return 0xBB;
		case SDLK_COMMA: return 0xBC;
		case SDLK_MINUS: return 0xBD;
		case SDLK_PERIOD: return 0xBE;
		case SDLK_SLASH: return 0xBF;
		case SDLK_GRAVE: return 0xC0;
		case SDLK_LEFTBRACKET: return 0xDB;
		case SDLK_BACKSLASH: return 0xDC;
		case SDLK_RIGHTBRACKET: return 0xDD;
		case SDLK_APOSTROPHE: return 0xDE;
		default: return 0;
		}
	}

	void apply_cursor(const platform::cursor cursor)
	{
		if (cursor == platform::cursor::none)
		{
			SDL_HideCursor();
			return;
		}

		auto system = SDL_SYSTEM_CURSOR_DEFAULT;
		switch (cursor)
		{
		case platform::cursor::link:
		case platform::cursor::hand_up:
		case platform::cursor::hand_down: system = SDL_SYSTEM_CURSOR_POINTER; break;
		case platform::cursor::text_select: system = SDL_SYSTEM_CURSOR_TEXT; break;
		case platform::cursor::move:
		case platform::cursor::size_all: system = SDL_SYSTEM_CURSOR_MOVE; break;
		case platform::cursor::left_right: system = SDL_SYSTEM_CURSOR_EW_RESIZE; break;
		case platform::cursor::up_down: system = SDL_SYSTEM_CURSOR_NS_RESIZE; break;
		case platform::cursor::zoom:
		case platform::cursor::select: system = SDL_SYSTEM_CURSOR_CROSSHAIR; break;
		default: break;
		}

		auto& native = system_cursors[system];
		if (!native) native = SDL_CreateSystemCursor(system);
		if (native) SDL_SetCursor(native);
		SDL_ShowCursor();
	}

	platform::pointer_button pointer_button_of(const uint8_t button)
	{
		switch (button)
		{
		case SDL_BUTTON_LEFT: return platform::pointer_button::left;
		case SDL_BUTTON_MIDDLE: return platform::pointer_button::middle;
		case SDL_BUTTON_RIGHT: return platform::pointer_button::right;
		case SDL_BUTTON_X1: return platform::pointer_button::x1;
		default: return platform::pointer_button::x2;
		}
	}

	bool is_pointer_button(const uint8_t button)
	{
		return button == SDL_BUTTON_LEFT || button == SDL_BUTTON_MIDDLE || button == SDL_BUTTON_RIGHT ||
		       button == SDL_BUTTON_X1 || button == SDL_BUTTON_X2;
	}

	struct display_geometry
	{
		SDL_DisplayID id = 0;
		SDL_Rect bounds{};
		SDL_Rect usable{};
		float scale = 1.0f;
	};

	std::vector<display_geometry> displays()
	{
		std::vector<display_geometry> result;
		int count = 0;
		auto* const ids = SDL_GetDisplays(&count);
		if (!ids) return result;
		for (auto i = 0; i < count; ++i)
		{
			display_geometry geometry;
			geometry.id = ids[i];
			geometry.scale = std::max(0.5f, SDL_GetDisplayContentScale(geometry.id));
			if (!SDL_GetDisplayBounds(geometry.id, &geometry.bounds)) continue;
			if (!SDL_GetDisplayUsableBounds(geometry.id, &geometry.usable)) geometry.usable = geometry.bounds;
			result.push_back(geometry);
		}
		SDL_free(ids);
		return result;
	}

	core::recti physical_rect(const SDL_Rect& rect, const float scale)
	{
		return {core::round(rect.x * scale), core::round(rect.y * scale),
			core::round((rect.x + rect.w) * scale), core::round((rect.y + rect.h) * scale)};
	}

	int64_t overlap_area(const SDL_Rect& a, const SDL_Rect& b)
	{
		const auto left = std::max(a.x, b.x);
		const auto top = std::max(a.y, b.y);
		const auto right = std::min(a.x + a.w, b.x + b.w);
		const auto bottom = std::min(a.y + a.h, b.y + b.h);
		return static_cast<int64_t>(std::max(0, right - left)) * std::max(0, bottom - top);
	}

	const display_geometry* display_for(const SDL_Rect& requested, const std::vector<display_geometry>& all)
	{
		if (all.empty()) return nullptr;
		const display_geometry* result = &all.front();
		int64_t largest = -1;
		for (const auto& display : all)
		{
			const auto area = overlap_area(requested, display.bounds);
			if (area > largest)
			{
				largest = area;
				result = &display;
			}
		}
		return result;
	}

	float scale_for_bounds(const core::recti bounds, const std::vector<display_geometry>& all)
	{
		if (all.empty()) return 1.0f;
		const auto physical = SDL_Rect{bounds.left, bounds.top, bounds.width(), bounds.height()};
		const display_geometry* result = &all.front();
		int64_t largest = -1;
		for (const auto& display : all)
		{
			const auto display_physical = physical_rect(display.bounds, display.scale);
			const auto requested = core::recti(physical.x, physical.y, physical.x + physical.w, physical.y + physical.h);
			const auto overlap = requested.intersection(display_physical);
			const auto area = static_cast<int64_t>(std::max(0, overlap.width())) *
				std::max(0, overlap.height());
			if (area > largest)
			{
				largest = area;
				result = &display;
			}
		}
		return result->scale;
	}

	core::recti default_window_bounds(const core::sizei client_size = {})
	{
		const auto all = displays();
		if (all.empty())
			return client_size.is_empty() ? core::recti{0, 0, 1440, 810} :
			       core::recti{0, 0, std::max(1, client_size.cx), std::max(1, client_size.cy)};
		const auto primary = SDL_GetPrimaryDisplay();
		const auto* chosen = &all.front();
		for (const auto& display : all)
			if (display.id == primary) { chosen = &display; break; }
		const auto width = client_size.is_empty() ? std::max(320, chosen->usable.w * 3 / 4) :
		                   std::max(1, core::round(client_size.cx / chosen->scale));
		const auto height = client_size.is_empty() ? std::max(200, chosen->usable.h * 3 / 4) :
		                    std::max(1, core::round(client_size.cy / chosen->scale));
		const SDL_Rect logical{chosen->usable.x + (chosen->usable.w - width) / 2,
			chosen->usable.y + (chosen->usable.h - height) / 2, width, height};
		return physical_rect(logical, chosen->scale);
	}
}

namespace platform
{
	std::string_view placement_setting_name()
	{
		return "window_placement";
	}

	std::optional<window_placement> read_placement(const setting_file_ptr& store, const std::string_view name)
	{
		if (!store) return std::nullopt;
		std::string value;
		if (!store->read({}, name.empty() ? placement_setting_name() : name, value)) return std::nullopt;

		int x = 0;
		int y = 0;
		int cx = 0;
		int cy = 0;
		int maximized = 0;
		if (std::sscanf(value.c_str(), "%d %d %d %d %d", &x, &y, &cx, &cy, &maximized) != 5)
			return std::nullopt;

		constexpr auto minimum_width = 320;
		constexpr auto minimum_height = 200;
		const auto all = displays();
		if (all.empty() || cx < minimum_width || cy < minimum_height) return std::nullopt;

		const SDL_Rect requested{x, y, cx, cy};
		const auto* const display = display_for(requested, all);
		if (!display || cx > display->usable.w || cy > display->usable.h) return std::nullopt;

		const auto on_display = overlap_area(requested, display->bounds) > 0;
		if (!on_display)
		{
			x = display->usable.x + (display->usable.w - cx) / 2;
			y = display->usable.y + (display->usable.h - cy) / 2;
		}
		else
		{
			x = std::clamp(x, display->usable.x, display->usable.x + display->usable.w - cx);
			y = std::clamp(y, display->usable.y, display->usable.y + display->usable.h - cy);
		}

		return window_placement{physical_rect({x, y, cx, cy}, display->scale), maximized != 0};
	}

	void defer_window_moves(const std::span<const window_move> moves)
	{
		const auto same_bounds = [](const core::recti a, const core::recti b)
		{
			return a.left == b.left && a.top == b.top && a.right == b.right && a.bottom == b.bottom;
		};
		for (const auto& move : moves)
		{
			if (!move.target) continue;
			if (same_bounds(move.target->bounds(), move.bounds) && move.target->is_visible() == move.visible) continue;
			move.target->set_bounds(move.bounds, move.visible);
		}
	}

	bool child_windows()
	{
		return false;
	}

	window_ptr create_window(const window_options& options, std::weak_ptr<window_events> events)
	{
		if (options.kind == window_kind::child || !SDL_WasInit(SDL_INIT_VIDEO)) return {};

		auto result = std::make_shared<linux_detail::sdl_window>(options, std::move(events));
		if (!result->valid()) return {};

		linux_detail::register_window(result.get());
		result->notify_created();
		return result;
	}
}

namespace platform::linux_detail
{
	platform::key_state key_state_from_sdl(const SDL_Keymod mod)
	{
		return ::key_state_of(mod);
	}

	char32_t key_from_sdl(const SDL_Keycode key)
	{
		return ::to_key(key);
	}


	sdl_window::sdl_window(const platform::window_options& options, std::weak_ptr<platform::window_events> events) :
		_kind(options.kind), _events(std::move(events)), _parent(options.parent),
		_minimum_client(options.minimum_client), _title(options.title), _can_focus(options.can_focus || options.menu),
		_layered(options.layered), _offscreen(options.offscreen), _drawn_caption(options.drawn_caption),
		_resizable(options.resizable), _container(options.container),
		_maximized(options.maximized && !options.offscreen)
	{
		_resize_scale = options.resize_scale;
		if (!SDL_WasInit(SDL_INIT_VIDEO)) return;

		const auto initial_bounds = options.bounds.is_empty() && options.kind == window_kind::app
			? default_window_bounds(options.client_size)
			: options.bounds;
		_bounds = initial_bounds;
		const auto initial_scale = scale_for_bounds(initial_bounds, displays());
		const auto requested_width = std::max(1, core::round(initial_bounds.width() / initial_scale));
		const auto requested_height = std::max(1, core::round(initial_bounds.height() / initial_scale));
		SDL_WindowFlags flags = SDL_WINDOW_HIDDEN | SDL_WINDOW_HIGH_PIXEL_DENSITY;

		if (options.kind == window_kind::app || options.kind == window_kind::dialog)
		{
			// A window that draws its own caption is borderless. Dialogs always are, as the old SDL layer
			// made them, even when one happens not to expose a caption.
			if (options.drawn_caption || options.kind == window_kind::dialog) flags |= SDL_WINDOW_BORDERLESS;
			if (options.resizable) flags |= SDL_WINDOW_RESIZABLE;
			if (!options.can_focus || options.offscreen) flags |= SDL_WINDOW_NOT_FOCUSABLE;
			_sdl = SDL_CreateWindow(_title.c_str(), requested_width, requested_height, flags);
			if (_sdl && options.parent)
			{
				if (auto* parent = dynamic_cast<sdl_window*>(options.parent); parent && parent->_sdl)
				{
					SDL_SetWindowParent(_sdl, parent->_sdl);
					if (options.kind == window_kind::dialog) SDL_SetWindowModal(_sdl, true);
				}
			}
		}
		else if (options.kind == window_kind::popup)
		{
			auto* parent = dynamic_cast<sdl_window*>(options.parent);
			if (!parent || !parent->_sdl) return;

			const auto no_focus_popup = (!options.menu && !options.can_focus) || options.layered;
			flags |= no_focus_popup ? SDL_WINDOW_TOOLTIP : SDL_WINDOW_POPUP_MENU;
			if (no_focus_popup) flags |= SDL_WINDOW_NOT_FOCUSABLE;
			if (options.layered) flags |= SDL_WINDOW_TRANSPARENT;

			const auto parent_density = std::max(0.5f, parent->_density);
			const auto origin = parent->screen_origin();
			const auto offset_x = options.bounds.is_empty() ? 0 :
				core::round((options.bounds.left - origin.x) / parent_density);
			const auto offset_y = options.bounds.is_empty() ? 0 :
				core::round((options.bounds.top - origin.y) / parent_density);
			_sdl = SDL_CreatePopupWindow(parent->_sdl, offset_x, offset_y,
				std::max(1, core::round(requested_width / parent_density)),
				std::max(1, core::round(requested_height / parent_density)), flags);
		}

		if (!_sdl) return;

		_id = SDL_GetWindowID(_sdl);
		SDL_SetWindowHitTest(_sdl, &sdl_window::hit_test_callback, this);
		if (!_can_focus || _offscreen) SDL_SetWindowFocusable(_sdl, false);

		// SDL may choose a GPU-backed surface when the first request arrives. Requesting it while
		// hidden preserves the old popup behaviour: creating a surface must not hide a shown menu.
		SDL_GetWindowSurface(_sdl);

		update_metrics(false);
		sync_geometry(false);

		auto minimum = _minimum_client;
		if (minimum.is_empty() && _kind == window_kind::app) minimum = {320, 200};
		if (!minimum.is_empty())
		{
			SDL_SetWindowMinimumSize(_sdl, std::max(1, core::round(minimum.cx / _density)),
				std::max(1, core::round(minimum.cy / _density)));
		}

		if (!initial_bounds.is_empty()) set_bounds(initial_bounds, options.offscreen ? false : options.visible);
		else if (options.visible && !options.offscreen) show(true);
		if (options.maximized && !options.offscreen)
		{
			SDL_MaximizeWindow(_sdl);
			_maximized = true;
		}
		if (options.layered) SDL_SetWindowOpacity(_sdl, 1.0f);
	}

	sdl_window::~sdl_window()
	{
		destroy();
	}

	sdl_window* sdl_window::parent_native() const
	{
		return dynamic_cast<sdl_window*>(_parent);
	}

	platform::window* sdl_window::root()
	{
		// SDL has no native child windows; mirror the Windows root contract by walking only
		// child kinds, not an owned dialog or popup's owner chain.
		if (_kind != window_kind::child) return this;
		auto* result = static_cast<platform::window*>(this);
		for (auto* parent = _parent; parent;)
		{
			result = parent;
			const auto native_parent = dynamic_cast<sdl_window*>(parent);
			parent = native_parent ? native_parent->_parent : nullptr;
		}
		return result;
	}

	std::any sdl_window::native_handle() const
	{
		return _sdl ? std::any(_sdl) : std::any{};
	}

	void sdl_window::update_metrics(const bool tell_events)
	{
		const auto old_scale = _scale;
		_scale = std::max(0.5, static_cast<double>(SDL_GetWindowDisplayScale(_sdl)));
		_density = std::max(0.5f, SDL_GetWindowPixelDensity(_sdl));
		if (tell_events && old_scale != _scale)
		{
			if (const auto events = _events.lock()) events->scale_changed(_scale);
		}
	}

	void sdl_window::sync_geometry(const bool tell_events)
	{
		if (!_sdl) return;

		int cx = 0;
		int cy = 0;
		SDL_GetWindowSizeInPixels(_sdl, &cx, &cy);
		_extent = {std::max(0, cx), std::max(0, cy)};

		const auto flags = SDL_GetWindowFlags(_sdl);
		_minimized = (flags & SDL_WINDOW_MINIMIZED) != 0;
		_maximized = (flags & SDL_WINDOW_MAXIMIZED) != 0;
		_occluded = (flags & SDL_WINDOW_OCCLUDED) != 0;
		_full_screen = (flags & SDL_WINDOW_FULLSCREEN) != 0;
		if ((flags & SDL_WINDOW_HIDDEN) != 0) _visible = false;

		int x = 0;
		int y = 0;
		SDL_GetWindowPosition(_sdl, &x, &y);
		_bounds = core::recti(core::pointi{core::round(x * _density), core::round(y * _density)}, _extent);

		if (tell_events) notify_size(true);
	}

	void sdl_window::notify_size(const bool force)
	{
		if (!force && !_extent.is_empty()) return;
		if (const auto events = _events.lock()) events->size_changed(_extent, _minimized);
	}

	core::pointi sdl_window::point_from_window(const float x, const float y) const
	{
		return {core::round(x * _density), core::round(y * _density)};
	}

	core::pointi sdl_window::screen_origin() const
	{
		if (!_sdl) return _bounds.top_left();
		int x = 0;
		int y = 0;
		SDL_GetWindowPosition(_sdl, &x, &y);
		return {core::round(x * _density), core::round(y * _density)};
	}

	SDL_HitTestResult sdl_window::hit_test(const SDL_Point* area) const
	{
		if (!_sdl || !area) return SDL_HITTEST_NORMAL;
		const auto p = point_from_window(static_cast<float>(area->x), static_cast<float>(area->y));
		const auto events = _events.lock();
		const auto region = events ? events->hit_test(p) : platform::hit_region::client;

		// The borderless SDL window has no other way to resize, so its band wins over drawn controls
		// beneath it, as the old Linux layer had it; Windows gives controls precedence instead.
		if (_drawn_caption && _resizable && !_maximized && !_full_screen)
		{
			const auto side = std::max(1, core::round(9.0 * _scale));
			const auto left = p.x < side;
			const auto right = p.x >= _extent.cx - side;
			const auto top = p.y < side;
			const auto bottom = p.y >= _extent.cy - side;
			if (top && left) return SDL_HITTEST_RESIZE_TOPLEFT;
			if (top && right) return SDL_HITTEST_RESIZE_TOPRIGHT;
			if (bottom && left) return SDL_HITTEST_RESIZE_BOTTOMLEFT;
			if (bottom && right) return SDL_HITTEST_RESIZE_BOTTOMRIGHT;
			if (left) return SDL_HITTEST_RESIZE_LEFT;
			if (right) return SDL_HITTEST_RESIZE_RIGHT;
			if (top) return SDL_HITTEST_RESIZE_TOP;
			if (bottom) return SDL_HITTEST_RESIZE_BOTTOM;
		}

		if (region == platform::hit_region::control) return SDL_HITTEST_NORMAL;
		if (!_drawn_caption && _kind != window_kind::dialog) return SDL_HITTEST_NORMAL;
		if (region == platform::hit_region::caption) return SDL_HITTEST_DRAGGABLE;
		return SDL_HITTEST_NORMAL;
	}

	SDL_HitTestResult SDLCALL sdl_window::hit_test_callback(SDL_Window*, const SDL_Point* area, void* data)
	{
		const auto* const window = static_cast<const sdl_window*>(data);
		return window ? window->hit_test(area) : SDL_HITTEST_NORMAL;
	}

	void sdl_window::update_caption_hover(const core::pointi loc, platform::window_events& events)
	{
		if (!_drawn_caption) return;
		const auto maximize = events.hit_test(loc) == platform::hit_region::maximize_button;
		if (maximize == _caption_hover) return;
		_caption_hover = maximize;
		events.caption_button(maximize ? platform::caption_button_action::hover : platform::caption_button_action::leave, loc);
	}

	void sdl_window::paint_now()
	{
		if (!_dirty || !_sdl || (!_visible && !_offscreen) || _minimized || (_occluded && !_offscreen)) return;
		const auto damage = _damage;
		_damage = {};
		_dirty = false;
		if (const auto events = _events.lock()) events->paint(damage);
	}

	void sdl_window::destroy()
	{
		if (_destroyed) return;
		[[maybe_unused]] const auto keep_alive = weak_from_this().lock();
		_destroyed = true;
		_timers.clear();
		release_capture(false);

		auto events = _events.lock();
		// Keep only weak references in the snapshot: destroying a child can release its frame and
		// recursively destroy grandchildren, so raw pointers left in the original list are stale.
		const auto current = windows();
		std::vector<std::weak_ptr<sdl_window>> children;
		children.reserve(current.size());
		for (auto* const candidate : current)
		{
			if (candidate != this && candidate->parent_native() == this)
				children.emplace_back(candidate->weak_from_this());
		}
		for (const auto& weak : children)
			if (const auto child = weak.lock()) child->destroy();

		if (_sdl)
		{
			unregister_window(this);
			SDL_DestroyWindow(_sdl);
			_sdl = nullptr;
		}
		_id = 0;
		_visible = false;
		if (events) events->destroyed();
	}

	void sdl_window::shutdown_native()
	{
		destroy();
	}

	void sdl_window::native_destroyed()
	{
		if (_destroyed) return;
		_destroyed = true;
		_timers.clear();
		_sdl = nullptr;
		unregister_window(this);
		_id = 0;
		_visible = false;
		if (const auto events = _events.lock()) events->destroyed();
	}

	void sdl_window::set_bounds(const core::recti bounds, const bool visible)
	{
		if (!_sdl) return;
		_bounds = bounds;

		if (!bounds.is_empty())
		{
			if (_kind == platform::window_kind::popup)
			{
				auto* parent = dynamic_cast<sdl_window*>(_parent);
				if (parent && parent->_sdl)
				{
					const auto density = std::max(0.5f, parent->_density);
					const auto origin = parent->screen_origin();
					SDL_SetWindowPosition(_sdl, core::round((bounds.left - origin.x) / density),
						core::round((bounds.top - origin.y) / density));
					SDL_SetWindowSize(_sdl, std::max(1, core::round(bounds.width() / density)),
						std::max(1, core::round(bounds.height() / density)));
				}
			}
			else
			{
				// Bounds are screen pixels. SDL positions and sizes are display points, so use the
				// density of the display the new rectangle belongs to rather than the old window
				// density; otherwise restoring a placement onto a scaled monitor doubles its size.
				const auto density = std::max(0.5f, scale_for_bounds(bounds, displays()));
				SDL_SetWindowPosition(_sdl, core::round(bounds.left / density), core::round(bounds.top / density));
				if (std::abs(density - _density) > 0.001f)
				{
					SDL_SyncWindow(_sdl);
					update_metrics(false);
				}
				SDL_SetWindowSize(_sdl, std::max(1, core::round(bounds.width() / density)),
					std::max(1, core::round(bounds.height() / density)));
			}
		}

		if (visible) show(true);
		else show(false);
		const auto old_extent = _extent;
		const auto id = _id;
		sync_geometry(false);
		if (old_extent != _extent)
		{
			notify_size(true);
			if (window_for(id) != this) return;
		}
		invalidate({});
	}

	void sdl_window::show(const bool show_window)
	{
		if (!_sdl) return;
		if (show_window)
		{
			if (_offscreen)
			{
				SDL_HideWindow(_sdl);
				_visible = false;
				invalidate({});
				return;
			}
			SDL_ShowWindow(_sdl);
			_visible = true;
			invalidate({});
		}
		else
		{
			SDL_HideWindow(_sdl);
			_visible = false;
		}
	}

	void sdl_window::set_resize_scale(const double scale)
	{
		_resize_scale = scale;
	}

	void sdl_window::set_title(const std::string_view text)
	{
		_title = text;
		if (_sdl) SDL_SetWindowTitle(_sdl, _title.c_str());
	}

	void sdl_window::focus()
	{
		if (_sdl && !_offscreen && _can_focus && _enabled)
		{
			SDL_RaiseWindow(_sdl);
		}
	}

	void sdl_window::capture(const bool capture_mouse)
	{
		if (!_sdl || _capture == capture_mouse) return;
		_capture = capture_mouse;
		SDL_CaptureMouse(capture_mouse);
	}

	void sdl_window::release_capture(const bool notify)
	{
		if (!_capture) return;
		_capture = false;
		SDL_CaptureMouse(false);
		if (notify)
		{
			if (const auto events = _events.lock()) events->cancel_mode();
		}
	}

	void sdl_window::set_cursor(const platform::cursor cursor)
	{
		_cursor = cursor;
		apply_cursor(cursor);
	}

	core::pointi sdl_window::cursor_location() const
	{
		if (!_sdl) return {-1, -1};
		float x = 0;
		float y = 0;
		if (SDL_GetMouseFocus() == _sdl)
		{
			SDL_GetMouseState(&x, &y);
			return point_from_window(x, y);
		}
		SDL_GetGlobalMouseState(&x, &y);
		return screen_to_client({core::round(x * _density), core::round(y * _density)});
	}

	core::pointi sdl_window::client_to_screen(const core::pointi p) const
	{
		return p + screen_origin();
	}

	core::pointi sdl_window::screen_to_client(const core::pointi p) const
	{
		return p - screen_origin();
	}

	void sdl_window::invalidate(const core::recti bounds)
	{
		if (!_sdl) return;
		const auto client = core::recti(0, 0, _extent.cx, _extent.cy);
		const auto area = bounds.is_empty() ? client : bounds.intersection(client);
		if (!bounds.is_empty() && area.is_empty()) return;
		_damage = bounds.is_empty() || _damage.is_empty() ? area : _damage.make_union(area);
		_dirty = true;
	}

	void sdl_window::scroll(const int, const int, const core::recti bounds)
	{
		invalidate(bounds);
	}

	uint64_t sdl_window::next_timer_due() const
	{
		uint64_t result = UINT64_MAX;
		for (const auto& [id, timer] : _timers) result = std::min(result, timer.due);
		return result;
	}

	void sdl_window::start_timer(const uint32_t id, const uint32_t milliseconds)
	{
		if (id == 0 || milliseconds == 0)
		{
			stop_timer(id);
			return;
		}
		const auto period = static_cast<uint64_t>(std::max<uint32_t>(1, milliseconds));
		_timers[id] = {id, period, SDL_GetTicks() + period};
	}

	void sdl_window::stop_timer(const uint32_t id)
	{
		_timers.erase(id);
	}

	void sdl_window::fire_timers(const uint64_t now)
	{
		std::vector<uint32_t> due;
		for (const auto& [id, timer] : _timers)
		{
			if (timer.due <= now) due.push_back(id);
		}

		for (const auto id : due)
		{
			auto found = _timers.find(id);
			if (found == _timers.end()) continue;
			found->second.due = std::max(found->second.due + found->second.period, now + 1);
			if (const auto events = _events.lock()) events->timer(id);
		}
	}

	void sdl_window::sys_command(const platform::sys_command_type command)
	{
		if (!_sdl) return;
		switch (command)
		{
		case platform::sys_command_type::MINIMIZE: SDL_MinimizeWindow(_sdl); break;
		case platform::sys_command_type::MAXIMIZE: SDL_MaximizeWindow(_sdl); break;
		case platform::sys_command_type::RESTORE: SDL_RestoreWindow(_sdl); break;
		}
	}

	void sdl_window::full_screen(const bool full)
	{
		if (_sdl) SDL_SetWindowFullscreen(_sdl, full);
		_full_screen = full;
	}

	void sdl_window::set_text_input(const bool active, const core::recti caret)
	{
		if (!_sdl) return;
		_text_area = caret;
		if (active && !caret.is_empty())
		{
			const SDL_Rect area{
				core::round(caret.left / _density), core::round(caret.top / _density),
				std::max(1, core::round(caret.width() / _density)),
				std::max(1, core::round(caret.height() / _density))};
			SDL_SetTextInputArea(_sdl, &area, 0);
		}

		if (_text_input == active) return;
		_text_input = active;
		if (active) SDL_StartTextInput(_sdl);
		else SDL_StopTextInput(_sdl);
	}


	void sdl_window::save_placement(const platform::setting_file_ptr& store, const std::string_view name)
	{
		if (!store || !_sdl || _kind == window_kind::child) return;
		int x = 0;
		int y = 0;
		int cx = 0;
		int cy = 0;
		SDL_GetWindowPosition(_sdl, &x, &y);
		SDL_GetWindowSize(_sdl, &cx, &cy);
		const auto maximized = (SDL_GetWindowFlags(_sdl) & SDL_WINDOW_MAXIMIZED) != 0;
		store->write({}, name.empty() ? placement_setting_name() : name,
			std::format("{} {} {} {} {}", x, y, cx, cy, maximized ? 1 : 0));
	}

	void sdl_window::handle_event(const SDL_Event& event)
	{
		if (_destroyed) return;
		auto events = _events.lock();
		if (!events) return;

		switch (event.type)
		{
		case SDL_EVENT_WINDOW_SHOWN:
			if (_offscreen)
			{
				SDL_HideWindow(_sdl);
				_visible = false;
			}
			else _visible = true;
			break;
		case SDL_EVENT_WINDOW_HIDDEN:
			_visible = false;
			break;
		case SDL_EVENT_WINDOW_MOVED:
			sync_geometry(false);
			break;
		case SDL_EVENT_WINDOW_RESIZED:
		case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED:
		case SDL_EVENT_WINDOW_METAL_VIEW_RESIZED:
		{
			const auto id = _id;
			sync_geometry(true);
			if (window_for(id) != this) break;
			invalidate({});
			break;
		}
		case SDL_EVENT_WINDOW_EXPOSED:
			_occluded = false;
			invalidate({});
			break;
		case SDL_EVENT_WINDOW_MINIMIZED:
			_minimized = true;
			sync_geometry(true);
			break;
		case SDL_EVENT_WINDOW_MAXIMIZED:
		{
			const auto id = _id;
			_maximized = true;
			_minimized = false;
			sync_geometry(true);
			if (window_for(id) != this) break;
			invalidate({});
			break;
		}
		case SDL_EVENT_WINDOW_RESTORED:
		{
			const auto id = _id;
			_minimized = false;
			_maximized = false;
			sync_geometry(true);
			if (window_for(id) != this) break;
			invalidate({});
			break;
		}
		case SDL_EVENT_WINDOW_OCCLUDED:
			_occluded = true;
			break;
		case SDL_EVENT_WINDOW_DISPLAY_SCALE_CHANGED:
		{
			const auto id = _id;
			update_metrics(true);
			if (window_for(id) != this) break;
			sync_geometry(true);
			invalidate({});
			break;
		}
		case SDL_EVENT_WINDOW_DISPLAY_CHANGED:
		case SDL_EVENT_WINDOW_ENTER_FULLSCREEN:
		case SDL_EVENT_WINDOW_LEAVE_FULLSCREEN:
		{
			const auto id = _id;
			update_metrics(true);
			if (window_for(id) != this) break;
			sync_geometry(true);
			invalidate({});
			break;
		}
		case SDL_EVENT_WINDOW_FOCUS_GAINED:
			if (!_can_focus) break;
			_has_focus = true;
			events->activated(true);
			events->focus_changed(true);
			break;
		case SDL_EVENT_WINDOW_FOCUS_LOST:
			_has_focus = false;
			_caption_pressed = false;
			if (_text_input)
			{
				SDL_StopTextInput(_sdl);
				_text_input = false;
			}
			release_capture(true);
			events->activated(false);
			events->focus_changed(false);
			break;
		case SDL_EVENT_WINDOW_MOUSE_LEAVE:
		{
			const auto captured = _capture;
			const auto caption_hover = _caption_hover;
			if (caption_hover)
			{
				_caption_hover = false;
				events->caption_button(platform::caption_button_action::leave, cursor_location());
			}
			if (!captured) events->pointer_leave();
			break;
		}
		case SDL_EVENT_WINDOW_CLOSE_REQUESTED:
			events->close_requested();
			break;
		case SDL_EVENT_WINDOW_DESTROYED:
			native_destroyed();
			break;
		case SDL_EVENT_MOUSE_MOTION:
		{
			platform::ticks_since_last_user_action = 0;
			const auto loc = point_from_window(event.motion.x, event.motion.y);
			update_caption_hover(loc, *events);
			events->pointer_move(loc, key_state_of(SDL_GetModState()));
			break;
		}
		case SDL_EVENT_MOUSE_BUTTON_DOWN:
		case SDL_EVENT_MOUSE_BUTTON_UP:
		{
			if (!is_pointer_button(event.button.button) || !_enabled) break;
			platform::ticks_since_last_user_action = 0;
			const auto loc = point_from_window(event.button.x, event.button.y);
			const auto button = pointer_button_of(event.button.button);
			const auto down = event.type == SDL_EVENT_MOUSE_BUTTON_DOWN;
			const auto caption_region = _drawn_caption ? events->hit_test(loc) : platform::hit_region::client;
			if (_drawn_caption && button == platform::pointer_button::left &&
				(caption_region == platform::hit_region::maximize_button || (!down && _caption_pressed)))
			{
				if (down)
				{
					_caption_pressed = true;
					events->caption_button(platform::caption_button_action::press, loc);
				}
				else if (_caption_pressed)
				{
					_caption_pressed = false;
					events->caption_button(platform::caption_button_action::release, loc);
				}
				break;
			}
			if (down && button == platform::pointer_button::left) capture(true);
			if (down)
			{
				if (event.button.clicks >= 2) events->double_click(button, loc, key_state_of(SDL_GetModState()));
				else events->pointer_down(button, loc, key_state_of(SDL_GetModState()));
			}
			else
			{
				const auto screen = client_to_screen(loc);
				const auto id = _id;
				events->pointer_up(button, loc, key_state_of(SDL_GetModState()));
				if (button == platform::pointer_button::left && window_for(id) == this) release_capture(false);
				if (button == platform::pointer_button::right)
					events->context_menu(screen, false);
			}
			break;
		}
		case SDL_EVENT_MOUSE_WHEEL:
		{
			if (!_enabled) break;
			const auto flip = event.wheel.direction == SDL_MOUSEWHEEL_FLIPPED ? -1.0f : 1.0f;
			const auto loc = point_from_window(event.wheel.mouse_x, event.wheel.mouse_y);
			const auto keys = key_state_of(SDL_GetModState());
			const auto dy = static_cast<int>(std::lround(event.wheel.y * flip * 120.0f));
			const auto dx = static_cast<int>(std::lround(event.wheel.x * flip * 120.0f));
			if (dy != 0) events->wheel(loc, dy, platform::wheel_axis::vertical, keys);
			if (dx != 0) events->wheel(loc, dx, platform::wheel_axis::horizontal, keys);
			break;
		}
		case SDL_EVENT_KEY_DOWN:
		case SDL_EVENT_KEY_UP:
		{
			const auto key = to_key(event.key.key);
			if (!key || !_enabled) break;
			auto keys = key_state_of(event.key.mod);
			keys.repeat = event.type == SDL_EVENT_KEY_DOWN && event.key.repeat;
			if (key != platform::keys::LEFT && key != platform::keys::RIGHT)
				platform::ticks_since_last_user_action = 0;
			const auto system = keys.alt || key == 0x79;
			if (event.type == SDL_EVENT_KEY_DOWN)
			{
				if (event.key.key == SDLK_APPLICATION ||
					(event.key.key == SDLK_F10 && (event.key.mod & SDL_KMOD_SHIFT)))
				{
					events->context_menu({-1, -1}, true);
					break;
				}
				events->key_down(static_cast<int>(key), keys, system);
			}
			else
			{
				events->key_up(static_cast<int>(key), keys, system);
			}
			break;
		}
		case SDL_EVENT_TEXT_INPUT:
			if (_enabled && event.text.text) events->text(event.text.text);
			break;
		case SDL_EVENT_PINCH_BEGIN:
		case SDL_EVENT_PINCH_UPDATE:
		case SDL_EVENT_PINCH_END:
		{
			const auto begins = event.type == SDL_EVENT_PINCH_BEGIN;
			const auto delta = begins || event.type == SDL_EVENT_PINCH_END ? 0 :
				static_cast<int>(std::lround((event.pinch.scale - 1.0f) * 120.0f));
			events->pinch(cursor_location(), delta, begins);
			break;
		}
		default:
			break;
		}
	}
}