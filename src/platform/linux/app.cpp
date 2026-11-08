// This file is part of libdesktop, a C++20 platform layer for desktop applications
// Copyright 2026  Zac Walker
//
// This program is free software; you can redistribute it and / or modify it
// under the terms of the LGPL License either version 2.1 or later.
// License details are available at https://www.gnu.org/licenses/lgpl-2.1.html
// This program is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY

// Purpose: SDL's Linux application lifetime and event pump. It deliberately knows only the
// platform::app and platform::window contracts; a UI frame is just a window_events implementation.

#include "window_internal.h"

namespace platform::detail
{
	void set_task_shutdown_bounded(bool bounded);
}

#include <SDL3/SDL.h>

#include <poll.h>
#include <sys/eventfd.h>
#include <sys/inotify.h>
#include <unistd.h>

#include <cerrno>
#include <clocale>
#include <cstdio>
#include <deque>
#include <condition_variable>
#include <future>
#include <mutex>

namespace
{
	constexpr uint32_t no_event = UINT32_MAX;

	std::map<SDL_WindowID, platform::linux_detail::sdl_window*> window_map;
	std::shared_ptr<platform::app> active_app;
	std::atomic_bool idle_pending = false;
	std::atomic_bool running = false;
	std::atomic_bool quit_requested = false;
	// What run returns when its loop ends, or when init declines to start: the code quit was given.
	int quit_code = 0;
	uint32_t event_idle = no_event;
	uint32_t event_folder = no_event;
	uint32_t event_ui = no_event;
	int frame_delay = 0;
	// Set once the application chooses a delay, so the loop's default never overrides that choice.
	bool frame_delay_chosen = false;
	uint64_t frame_due = UINT64_MAX;
	uint64_t user_action_due = UINT64_MAX;
	std::mutex ui_task_mutex;
	std::unordered_set<char32_t> event_keys;
	thread_local bool handling_key_event = false;
	std::condition_variable ui_task_ready;
	std::deque<std::function<void()>> ui_tasks;
	std::atomic_bool ui_accepting = true;
	std::vector<std::function<void()>> shutdown_hooks;
	constexpr uint32_t user_action_period_ms = 200;
	constexpr int default_frame_delay_ms = 200;

	void apply_frame_delay(const int milliseconds)
	{
		const auto delay = std::max(0, milliseconds);
		if (delay == frame_delay) return;
		frame_delay = delay;
		frame_due = frame_delay == 0 ? UINT64_MAX : SDL_GetTicks() + static_cast<uint64_t>(frame_delay);
	}

	class folder_watcher final
	{
		int _fd = -1;
		int _wake = -1;
		std::thread _thread;
		std::mutex _mutex;
		std::map<int, core::folder_path> _watches;
		std::set<std::string> _pending_text;
		std::vector<core::folder_path> _pending;
		std::function<void()> _notify;

		void run()
		{
			std::array<char, 16384> buffer{};
			for (;;)
			{
				pollfd fds[2] = {{_fd, POLLIN, 0}, {_wake, POLLIN, 0}};
				if (poll(fds, 2, -1) < 0)
				{
					if (errno == EINTR) continue;
					return;
				}

				if (fds[1].revents) return;
				if (!(fds[0].revents & POLLIN)) continue;

				const auto count = read(_fd, buffer.data(), buffer.size());
				if (count <= 0) continue;

				bool first = false;
				{
					std::lock_guard lock(_mutex);
					for (ssize_t offset = 0; offset < count;)
					{
						const auto* const event = reinterpret_cast<const inotify_event*>(buffer.data() + offset);
						const auto found = _watches.find(event->wd);
						if (found != _watches.end())
						{
							const auto text = std::string(found->second.text());
							if (_pending_text.insert(text).second)
							{
								first = _pending.empty();
								_pending.push_back(found->second);
							}
						}
						offset += static_cast<ssize_t>(sizeof(inotify_event) + event->len);
					}
				}
				if (first && _notify) _notify();
			}
		}

	public:
		void start(std::function<void()> notify)
		{
			_notify = std::move(notify);
			_fd = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
			_wake = eventfd(0, EFD_CLOEXEC);
			if (_fd >= 0 && _wake >= 0) _thread = std::thread([this] { run(); });
		}

		void stop()
		{
			if (_thread.joinable())
			{
				const uint64_t one = 1;
				[[maybe_unused]] const auto written = write(_wake, &one, sizeof(one));
				_thread.join();
			}
			if (_fd >= 0) close(_fd);
			if (_wake >= 0) close(_wake);
			_fd = _wake = -1;
			std::lock_guard lock(_mutex);
			_watches.clear();
			_pending.clear();
			_pending_text.clear();
		}

		void watch(const std::vector<core::folder_path>& folders)
		{
			if (_fd < 0) return;
			std::lock_guard lock(_mutex);
			for (const auto& entry : _watches) inotify_rm_watch(_fd, entry.first);
			_watches.clear();

			constexpr auto mask = IN_ATTRIB | IN_CLOSE_WRITE | IN_CREATE | IN_DELETE | IN_MOVED_FROM |
				IN_MOVED_TO | IN_DELETE_SELF | IN_MOVE_SELF;
			for (const auto& folder : folders)
			{
				const auto text = std::string(folder.text());
				const auto watch = inotify_add_watch(_fd, text.c_str(), mask);
				if (watch >= 0) _watches[watch] = folder;
			}
		}

		std::vector<core::folder_path> take()
		{
			std::lock_guard lock(_mutex);
			_pending_text.clear();
			auto result = std::move(_pending);
			_pending.clear();
			return result;
		}
	};

	folder_watcher watcher;

	std::string command_line_of(const int argc, char* argv[])
	{
		std::string result;
		for (auto i = 1; i < argc; ++i)
		{
			const std::string_view argument(argv[i] ? argv[i] : "");
			if (!result.empty()) result += ' ';
			if (argument.find_first_of(" \t") != std::string_view::npos)
			{
				result += '"';
				result += argument;
				result += '"';
			}
			else
			{
				result += argument;
			}
		}
		return result;
	}

	void reset_events()
	{
		event_idle = event_folder = event_ui = no_event;
		idle_pending = false;
		user_action_due = UINT64_MAX;
		{
			std::lock_guard lock(ui_task_mutex);
			ui_tasks.clear();
		}
		ui_task_ready.notify_all();
	}

	void wake_with_idle_event()
	{
		if (event_idle == no_event) return;
		SDL_Event event{};
		event.type = static_cast<SDL_EventType>(event_idle);
		SDL_PushEvent(&event);
	}

	void wake_with_ui_event()
	{
		if (event_ui == no_event) return;
		SDL_Event event{};
		event.type = static_cast<SDL_EventType>(event_ui);
		SDL_PushEvent(&event);
	}

	void run_ui_tasks()
	{
		for (;;)
		{
			std::function<void()> task;
			{
				std::lock_guard lock(ui_task_mutex);
				if (ui_tasks.empty()) return;
				task = std::move(ui_tasks.front());
				ui_tasks.pop_front();
			}
			if (task) task();
		}
	}

	void request_quit()
	{
		quit_requested = true;
		platform::is_closing = true;
		platform::event_exit.set();
		wake_with_idle_event();
	}

	void fire_frame_tick(const uint64_t now)
	{
		if (frame_delay <= 0 || frame_due > now) return;
		const auto period = frame_delay;
		frame_due = std::max(frame_due + static_cast<uint64_t>(period), now + 1);
		if (active_app) active_app->frame_tick();
		if (frame_delay != period && frame_delay <= 0) frame_due = UINT64_MAX;
	}

	void fire_user_action_tick(const uint64_t now)
	{
		if (user_action_due > now) return;
		platform::ticks_since_last_user_action += 1;
		user_action_due = std::max(user_action_due + user_action_period_ms, now + 1);
	}

	uint64_t next_due()
	{
		uint64_t result = frame_delay > 0 ? frame_due : UINT64_MAX;
		result = std::min(result, user_action_due);
		for (auto* const window : platform::linux_detail::windows())
			result = std::min(result, window->next_timer_due());
		return result;
	}

	void paint_dirty_windows()
	{
		for (auto* const window : platform::linux_detail::windows())
		{
			if (!window->dirty()) continue;
			const auto id = window->id();
			if (platform::linux_detail::window_for(id) == window) window->update_now();
		}
	}

	void fire_window_timers(const uint64_t now)
	{
		for (auto* const window : platform::linux_detail::windows())
		{
			const auto id = window->id();
			if (platform::linux_detail::window_for(id) == window) window->fire_timers(now);
		}
	}

	bool modal_event(const SDL_Event& event, platform::modal_filter* filter)
	{
		if (!filter) return false;
		switch (event.type)
		{
		case SDL_EVENT_KEY_DOWN:
		{
			const auto key = platform::linux_detail::key_from_sdl(event.key.key);
			if (key)
				filter->key_down(static_cast<int>(key), platform::linux_detail::key_state_from_sdl(event.key.mod));
			return true;
		}
		case SDL_EVENT_KEY_UP:
		case SDL_EVENT_TEXT_INPUT:
		case SDL_EVENT_TEXT_EDITING:
		case SDL_EVENT_TEXT_EDITING_CANDIDATES:
			return true;
		case SDL_EVENT_MOUSE_BUTTON_DOWN:
		{
			if (auto* const target = platform::linux_detail::window_for(event.button.windowID))
			{
				const auto loc = target->event_location(event.button.x, event.button.y);
				return filter->pointer_down(*target, target->client_to_screen(loc));
			}
			// A click on a foreign window is outside the menu just as a click on an owned non-popup is.
			filter->activation_lost();
			return true;
		}
		case SDL_EVENT_WINDOW_FOCUS_LOST:
			// Focus moving between a menu and one of its popup windows is not activation loss.
			// The old nested menu loop cancelled only when the application itself lost focus.
			if (!SDL_GetKeyboardFocus()) filter->activation_lost();
			return true;
		case SDL_EVENT_WINDOW_FOCUS_GAINED:
			// Popup-menu focus is consumed by the nested menu loop; it must not activate the
			// application's frame underneath it.
			if (const auto* const target = platform::linux_detail::window_for(event.window.windowID))
				return target->kind() == platform::window_kind::popup;
			return false;
		case SDL_EVENT_WINDOW_HIDDEN:
			// A compositor can hide a popup before its outside click reaches us. Treat that as menu
			// cancellation, but still let the window update its visible state.
			if (const auto* const target = platform::linux_detail::window_for(event.window.windowID);
				target && target->kind() == platform::window_kind::popup)
				filter->activation_lost();
			return false;
		default:
			return false;
		}
	}


	SDL_Keycode sdl_keycode_for(const char32_t key)
	{
		if (key >= U'A' && key <= U'Z') return static_cast<SDL_Keycode>(key);
		if (key >= U'0' && key <= U'9') return static_cast<SDL_Keycode>(key);
		if (key >= platform::keys::F1 && key <= platform::keys::F12)
			return static_cast<SDL_Keycode>(SDLK_F1 + key - platform::keys::F1);
		if (key >= platform::keys::NUMPAD0 && key <= platform::keys::NUMPAD9)
			return static_cast<SDL_Keycode>(SDLK_KP_0 + key - platform::keys::NUMPAD0);
		switch (key)
		{
		case 0x08: return SDLK_BACKSPACE;
		case 0x09: return SDLK_TAB;
		case 0x0D: return SDLK_RETURN;
		case 0x13: return SDLK_PAUSE;
		case 0x14: return SDLK_CAPSLOCK;
		case 0x1B: return SDLK_ESCAPE;
		case 0x20: return SDLK_SPACE;
		case 0x21: return SDLK_PAGEUP;
		case 0x22: return SDLK_PAGEDOWN;
		case 0x23: return SDLK_END;
		case 0x24: return SDLK_HOME;
		case 0x25: return SDLK_LEFT;
		case 0x26: return SDLK_UP;
		case 0x27: return SDLK_RIGHT;
		case 0x28: return SDLK_DOWN;
		case 0x2C: return SDLK_PRINTSCREEN;
		case 0x2D: return SDLK_INSERT;
		case 0x2E: return SDLK_DELETE;
		case 0x5B: return SDLK_LGUI;
		case 0x5C: return SDLK_RGUI;
		case 0x5D: return SDLK_APPLICATION;
		case 0x6A: return SDLK_KP_MULTIPLY;
		case 0x6B: return SDLK_KP_PLUS;
		case 0x6C: return SDLK_KP_COMMA;
		case 0x6D: return SDLK_KP_MINUS;
		case 0x6E: return SDLK_KP_PERIOD;
		case 0x6F: return SDLK_KP_DIVIDE;
		case 0x90: return SDLK_NUMLOCKCLEAR;
		case 0x91: return SDLK_SCROLLLOCK;
		case 0xA6: return SDLK_AC_BACK;
		case 0xA7: return SDLK_AC_FORWARD;
		case 0xA8: return SDLK_AC_REFRESH;
		case 0xA9: return SDLK_AC_STOP;
		case 0xAA: return SDLK_AC_SEARCH;
		case 0xAB: return SDLK_AC_BOOKMARKS;
		case 0xAC: return SDLK_AC_HOME;
		case 0xAD: return SDLK_MUTE;
		case 0xAE: return SDLK_VOLUMEDOWN;
		case 0xAF: return SDLK_VOLUMEUP;
		case 0xB0: return SDLK_MEDIA_NEXT_TRACK;
		case 0xB1: return SDLK_MEDIA_PREVIOUS_TRACK;
		case 0xB2: return SDLK_MEDIA_STOP;
		case 0xB3: return SDLK_MEDIA_PLAY_PAUSE;
		case 0xBA: return SDLK_SEMICOLON;
		case 0xBB: return SDLK_EQUALS;
		case 0xBC: return SDLK_COMMA;
		case 0xBD: return SDLK_MINUS;
		case 0xBE: return SDLK_PERIOD;
		case 0xBF: return SDLK_SLASH;
		case 0xC0: return SDLK_GRAVE;
		case 0xDB: return SDLK_LEFTBRACKET;
		case 0xDC: return SDLK_BACKSLASH;
		case 0xDD: return SDLK_RIGHTBRACKET;
		case 0xDE: return SDLK_APOSTROPHE;
		case 0xE2: return SDLK_BACKSLASH;
		default: break;
		}
		if (key == platform::keys::SHIFT) return SDLK_LSHIFT;
		if (key == platform::keys::LSHIFT) return SDLK_LSHIFT;
		if (key == platform::keys::RSHIFT) return SDLK_RSHIFT;
		if (key == platform::keys::CONTROL) return SDLK_LCTRL;
		if (key == platform::keys::LCONTROL) return SDLK_LCTRL;
		if (key == platform::keys::RCONTROL) return SDLK_RCTRL;
		if (key == platform::keys::ALT) return SDLK_LALT;
		if (key == platform::keys::LALT) return SDLK_LALT;
		if (key == platform::keys::RALT) return SDLK_RALT;
		return static_cast<SDL_Keycode>(key);
	}

	void dispatch_event(const SDL_Event& event, platform::modal_filter* filter)
	{
		const bool key_event = event.type == SDL_EVENT_KEY_DOWN || event.type == SDL_EVENT_KEY_UP;
		if (key_event)
		{
			const auto key = platform::linux_detail::key_from_sdl(event.key.key);
			const bool down = event.type == SDL_EVENT_KEY_DOWN;
			const auto set_key = [down](const char32_t value)
			{
				if (down) event_keys.insert(value);
				else event_keys.erase(value);
			};
			if (key) set_key(key);
			switch (event.key.key)
			{
			case SDLK_LSHIFT: set_key(platform::keys::LSHIFT); set_key(platform::keys::SHIFT); break;
			case SDLK_RSHIFT: set_key(platform::keys::RSHIFT); set_key(platform::keys::SHIFT); break;
			case SDLK_LCTRL: set_key(platform::keys::LCONTROL); set_key(platform::keys::CONTROL); break;
			case SDLK_RCTRL: set_key(platform::keys::RCONTROL); set_key(platform::keys::CONTROL); break;
			case SDLK_LALT: set_key(platform::keys::LALT); set_key(platform::keys::ALT); break;
			case SDLK_RALT: set_key(platform::keys::RALT); set_key(platform::keys::ALT); break;
			default: break;
			}
			handling_key_event = true;
		}
		const core::scope_exit clear_key_event([key_event] { if (key_event) handling_key_event = false; });
		if (modal_event(event, filter)) return;
		if (event_idle != no_event && event.type == static_cast<SDL_EventType>(event_idle))
		{
			idle_pending = false;
			return;
		}
		if (event_folder != no_event && event.type == static_cast<SDL_EventType>(event_folder))
		{
			for (const auto& folder : watcher.take())
				if (active_app) active_app->folder_changed(folder);
			return;
		}
		if (event_ui != no_event && event.type == static_cast<SDL_EventType>(event_ui))
		{
			run_ui_tasks();
			return;
		}

		switch (event.type)
		{
		case SDL_EVENT_QUIT:
			if (!quit_requested && active_app && active_app->can_exit()) request_quit();
			break;
		case SDL_EVENT_TERMINATING:
			if (active_app) active_app->system_event(platform::os_event_type::session_ending);
			if (!quit_requested && active_app && active_app->can_exit()) request_quit();
			break;
		case SDL_EVENT_WILL_ENTER_BACKGROUND:
			if (active_app) active_app->system_event(platform::os_event_type::system_suspending);
			break;
		case SDL_EVENT_DID_ENTER_FOREGROUND:
			if (active_app) active_app->system_event(platform::os_event_type::system_resumed);
			break;
		case SDL_EVENT_SYSTEM_THEME_CHANGED:
		case SDL_EVENT_LOCALE_CHANGED:
			if (active_app) active_app->system_event(platform::os_event_type::options_changed);
			break;
		case SDL_EVENT_DISPLAY_CONTENT_SCALE_CHANGED:
			if (active_app) active_app->system_event(platform::os_event_type::dpi_changed);
			break;
		case SDL_EVENT_DISPLAY_ORIENTATION:
		case SDL_EVENT_DISPLAY_USABLE_BOUNDS_CHANGED:
		case SDL_EVENT_DISPLAY_ADDED:
		case SDL_EVENT_DISPLAY_REMOVED:
		case SDL_EVENT_DISPLAY_MOVED:
		case SDL_EVENT_DISPLAY_DESKTOP_MODE_CHANGED:
		case SDL_EVENT_DISPLAY_CURRENT_MODE_CHANGED:
			if (active_app) active_app->system_event(platform::os_event_type::options_changed);
			break;
		default:
			if (auto* const window = platform::linux_detail::window_for(event.window.windowID))
				window->dispatch(event);
			break;
		}
	}

	bool pump(const std::function<bool()>& done, const uint32_t timeout_ms, platform::modal_filter* filter)
	{
		const auto start = SDL_GetTicks();
		const auto deadline = timeout_ms == UINT32_MAX ? UINT64_MAX : start + timeout_ms;
		while (!done())
		{
			if (quit_requested || platform::is_closing) return false;
			paint_dirty_windows();

			const auto now = SDL_GetTicks();
			if (now >= deadline) return false;
			const auto next = next_due();
			const auto event_deadline = std::min(deadline, next);
			const auto wait = event_deadline == UINT64_MAX ? UINT64_MAX :
				(event_deadline > now ? event_deadline - now : 0);
			const auto wait_ms = wait == UINT64_MAX ? -1 :
				static_cast<Sint32>(std::min<uint64_t>(wait, INT32_MAX));

			SDL_Event event{};
			if (SDL_WaitEventTimeout(&event, wait_ms))
			{
				dispatch_event(event, filter);
				while (SDL_PollEvent(&event)) dispatch_event(event, filter);
			}

			const auto after_wait = SDL_GetTicks();
			fire_window_timers(after_wait);
			fire_user_action_tick(after_wait);
			fire_frame_tick(after_wait);
			idle_pending = false;
			if (active_app) active_app->idle();
		}
		return true;
	}

	bool pump_pending_events()
	{
		if (!running) return false;
		SDL_Event event{};
		while (SDL_PollEvent(&event)) dispatch_event(event, nullptr);
		if (event_ui != no_event) run_ui_tasks();
		return !quit_requested && !platform::is_closing;
	}

	void shutdown_windows()
	{
		for (;;)
		{
			auto current = platform::linux_detail::windows();
			if (current.empty()) break;
			auto* selected = current.front();
			for (auto* const candidate : current)
			{
				const auto has_child = std::any_of(current.begin(), current.end(),
					[candidate](auto* child) { return child != candidate && child->parent_native() == candidate; });
				if (!has_child)
				{
					selected = candidate;
					break;
				}
			}
			selected->shutdown_native();
		}
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

	core::recti display_area(const core::recti requested, const bool work_area)
	{
		const auto all = displays();
		if (all.empty()) return {0, 0, 1920, 1080};

		const display_geometry* chosen = &all.front();
		if (!requested.is_empty())
		{
			int64_t largest = -1;
			for (const auto& display : all)
			{
				const auto scale = display.scale;
				const core::recti physical{
					core::round(display.bounds.x * scale), core::round(display.bounds.y * scale),
					core::round((display.bounds.x + display.bounds.w) * scale),
					core::round((display.bounds.y + display.bounds.h) * scale)};
				const auto overlap = requested.intersection(physical);
				const auto area = static_cast<int64_t>(std::max(0, overlap.width())) * std::max(0, overlap.height());
				if (area > largest)
				{
					largest = area;
					chosen = &display;
				}
			}
		}

		const auto& source = work_area ? chosen->usable : chosen->bounds;
		return {
			core::round(source.x * chosen->scale), core::round(source.y * chosen->scale),
			core::round((source.x + source.w) * chosen->scale),
			core::round((source.y + source.h) * chosen->scale)};
	}
}

namespace platform::linux_detail
{
	void register_window(sdl_window* window)
	{
		if (window && window->id() != 0) window_map[window->id()] = window;
	}

	void unregister_window(sdl_window* window)
	{
		if (!window) return;
		const auto found = window_map.find(window->id());
		if (found != window_map.end() && found->second == window) window_map.erase(found);
	}

	sdl_window* window_for(const SDL_WindowID id)
	{
		const auto found = window_map.find(id);
		return found == window_map.end() ? nullptr : found->second;
	}

	std::vector<sdl_window*> windows()
	{
		std::vector<sdl_window*> result;
		result.reserve(window_map.size());
		for (const auto& entry : window_map) result.push_back(entry.second);
		return result;
	}
}

///////////////////////////////////////////////////////////////////////////////////////////////////
// Platform input values that used to live beside the Linux UI implementation.
///////////////////////////////////////////////////////////////////////////////////////////////////

char32_t platform::keys::APPS = 0x5D;
char32_t platform::keys::BACK = 0x08;
char32_t platform::keys::BROWSER_BACK = 0xA6;
char32_t platform::keys::BROWSER_FAVORITES = 0xAB;
char32_t platform::keys::BROWSER_FORWARD = 0xA7;
char32_t platform::keys::BROWSER_HOME = 0xAC;
char32_t platform::keys::BROWSER_REFRESH = 0xA8;
char32_t platform::keys::BROWSER_SEARCH = 0xAA;
char32_t platform::keys::BROWSER_STOP = 0xA9;
char32_t platform::keys::DEL = 0x2E;
char32_t platform::keys::DOWN = 0x28;
char32_t platform::keys::END = 0x23;
char32_t platform::keys::ESCAPE = 0x1B;
char32_t platform::keys::F1 = 0x70;
char32_t platform::keys::F2 = 0x71;
char32_t platform::keys::F3 = 0x72;
char32_t platform::keys::F4 = 0x73;
char32_t platform::keys::F5 = 0x74;
char32_t platform::keys::F6 = 0x75;
char32_t platform::keys::F7 = 0x76;
char32_t platform::keys::F8 = 0x77;
char32_t platform::keys::F9 = 0x78;
char32_t platform::keys::F10 = 0x79;
char32_t platform::keys::F11 = 0x7A;
char32_t platform::keys::F12 = 0x7B;
char32_t platform::keys::HOME = 0x24;
char32_t platform::keys::INSERT = 0x2D;
char32_t platform::keys::LEFT = 0x25;
char32_t platform::keys::MEDIA_NEXT_TRACK = 0xB0;
char32_t platform::keys::MEDIA_PLAY_PAUSE = 0xB3;
char32_t platform::keys::MEDIA_PREV_TRACK = 0xB1;
char32_t platform::keys::MEDIA_STOP = 0xB2;
char32_t platform::keys::NEXT = 0x22;
char32_t platform::keys::OEM_1 = 0xBA;
char32_t platform::keys::OEM_2 = 0xBF;
char32_t platform::keys::OEM_3 = 0xC0;
char32_t platform::keys::OEM_4 = 0xDB;
char32_t platform::keys::OEM_5 = 0xDC;
char32_t platform::keys::OEM_6 = 0xDD;
char32_t platform::keys::OEM_7 = 0xDE;
char32_t platform::keys::OEM_102 = 0xE2;
char32_t platform::keys::OEM_COMMA = 0xBC;
char32_t platform::keys::OEM_PERIOD = 0xBE;
char32_t platform::keys::OEM_MINUS = 0xBD;
char32_t platform::keys::OEM_PLUS = 0xBB;
char32_t platform::keys::PRIOR = 0x21;
char32_t platform::keys::RETURN = 0x0D;
char32_t platform::keys::RIGHT = 0x27;
char32_t platform::keys::SPACE = 0x20;
char32_t platform::keys::TAB = 0x09;
char32_t platform::keys::UP = 0x26;
char32_t platform::keys::VOLUME_DOWN = 0xAE;
char32_t platform::keys::VOLUME_MUTE = 0xAD;
char32_t platform::keys::VOLUME_UP = 0xAF;
char32_t platform::keys::SHIFT = 0x10;
char32_t platform::keys::CONTROL = 0x11;
char32_t platform::keys::ALT = 0x12;
char32_t platform::keys::LSHIFT = 0xA0;
char32_t platform::keys::RSHIFT = 0xA1;
char32_t platform::keys::LCONTROL = 0xA2;
char32_t platform::keys::RCONTROL = 0xA3;
char32_t platform::keys::LALT = 0xA4;
char32_t platform::keys::RALT = 0xA5;
char32_t platform::keys::LWIN = 0x5B;
char32_t platform::keys::RWIN = 0x5C;
char32_t platform::keys::PAUSE = 0x13;
char32_t platform::keys::CAPITAL = 0x14;
char32_t platform::keys::NUMLOCK = 0x90;
char32_t platform::keys::SCROLL = 0x91;
char32_t platform::keys::SNAPSHOT = 0x2C;
char32_t platform::keys::NUMPAD0 = 0x60;
char32_t platform::keys::NUMPAD1 = 0x61;
char32_t platform::keys::NUMPAD2 = 0x62;
char32_t platform::keys::NUMPAD3 = 0x63;
char32_t platform::keys::NUMPAD4 = 0x64;
char32_t platform::keys::NUMPAD5 = 0x65;
char32_t platform::keys::NUMPAD6 = 0x66;
char32_t platform::keys::NUMPAD7 = 0x67;
char32_t platform::keys::NUMPAD8 = 0x68;
char32_t platform::keys::NUMPAD9 = 0x69;
char32_t platform::keys::MULTIPLY = 0x6A;
char32_t platform::keys::ADD = 0x6B;
char32_t platform::keys::SUBTRACT = 0x6D;
char32_t platform::keys::DECIMAL = 0x6E;
char32_t platform::keys::DIVIDE = 0x6F;
char32_t platform::keys::SEPARATOR = 0x6C;

int platform::ticks_since_last_user_action = 0;

namespace platform
{
	core::color32 system_color_value(const system_color c)
	{
		switch (c)
		{
		case system_color::window: return 0x00ffffff;
		case system_color::window_text: return 0x00000000;
		case system_color::desktop: return 0x00000000;
		}
		return 0;
	}

	platform::key_state current_key_state()
	{
		return linux_detail::key_state_from_sdl(SDL_GetModState());
	}

	bool is_key_down(const char32_t key)
	{
		if (handling_key_event) return event_keys.contains(key);
		return is_key_down_async(key);
	}

	bool is_key_down_async(const char32_t key)
	{
		const auto modifiers = SDL_GetModState();
		if (key == keys::SHIFT) return (modifiers & SDL_KMOD_SHIFT) != 0;
		if (key == keys::CONTROL) return (modifiers & SDL_KMOD_CTRL) != 0;
		if (key == keys::ALT) return (modifiers & SDL_KMOD_ALT) != 0;
		int count = 0;
		const auto* const state = SDL_GetKeyboardState(&count);
		SDL_Keymod ignored = SDL_KMOD_NONE;
		const auto scan = SDL_GetScancodeFromKey(sdl_keycode_for(key), &ignored);
		return state && scan != SDL_SCANCODE_UNKNOWN && static_cast<int>(scan) < count && state[scan];
	}

	core::recti desktop_bounds(const bool work_area)
	{
		return display_area({}, work_area);
	}

	namespace detail
	{
		bool pump_backend_ui_tasks(const uint32_t timeout_ms)
		{
			std::unique_lock lock(ui_task_mutex);
			if (ui_tasks.empty() && timeout_ms != 0)
				ui_task_ready.wait_for(lock, std::chrono::milliseconds(timeout_ms),
					[&] { return !ui_tasks.empty() || !ui_accepting; });
			lock.unlock();
			run_ui_tasks();
			return ui_accepting;
		}
	}

	bool pump_events()
	{
		return pump_pending_events();
	}

	void quit(const int exit_code)
	{
		if (!running) return;
		quit_code = exit_code;
		request_quit();
	}

	void queue_idle()
	{
		if (!running || event_idle == no_event) return;
		if (!idle_pending.exchange(true)) wake_with_idle_event();
	}

	void set_frame_delay(const int milliseconds)
	{
		frame_delay_chosen = true;
		apply_frame_delay(milliseconds);
	}

	void run_ui(std::function<void()> task)
	{
		if (!task) return;
		{
			std::lock_guard lock(ui_task_mutex);
			if (!ui_accepting) return;
			ui_tasks.push_back(std::move(task));
		}
		ui_task_ready.notify_one();
		if (running && event_ui != no_event) wake_with_ui_event();
	}

	void send_system_event(const os_event_type event)
	{
		// All callers that need ordering (notably device recovery) arrive through run_ui. Keep this
		// call synchronous so the application can release GPU resources before recovery continues.
		if (is_ui_thread())
		{
			if (active_app) active_app->system_event(event);
			return;
		}
		if (!running || event_ui == no_event) return;

		const auto complete = std::make_shared<std::promise<void>>();
		auto finished = complete->get_future();
		run_ui([event, complete]
		{
			const core::scope_exit signal([complete] { complete->set_value(); });
			if (active_app) active_app->system_event(event);
		});
		finished.wait();
	}

	void add_shutdown_hook(std::function<void()> hook)
	{
		if (hook) shutdown_hooks.push_back(std::move(hook));
	}

	void monitor_folders(const std::vector<core::folder_path>& folders)
	{
		watcher.watch(folders);
	}

	void enable_screen_saver(const bool enable)
	{
		if (enable) SDL_EnableScreenSaver();
		else SDL_DisableScreenSaver();
	}

	void show_progress(const int64_t done, const int64_t total)
	{
		for (auto* const window : platform::linux_detail::windows())
		{
			if (window->kind() != platform::window_kind::app || !window->native()) continue;
			if (total > 0)
			{
				SDL_SetWindowProgressState(window->native(), SDL_PROGRESS_STATE_NORMAL);
				SDL_SetWindowProgressValue(window->native(),
					static_cast<float>(std::clamp<int64_t>(done, 0, total)) / static_cast<float>(total));
			}
			else
			{
				SDL_SetWindowProgressState(window->native(), SDL_PROGRESS_STATE_INDETERMINATE);
			}
			break;
		}
	}

	void end_progress()
	{
		for (auto* const window : platform::linux_detail::windows())
		{
			if (window->kind() == platform::window_kind::app && window->native())
			{
				SDL_SetWindowProgressState(window->native(), SDL_PROGRESS_STATE_NONE);
				break;
			}
		}
	}

	bool run_modal_loop(const std::function<bool()>& done, modal_filter* filter, const uint32_t timeout_ms)
	{
		if (!running) return false;
		return pump(done, timeout_ms, filter);
	}

	core::recti monitor_work_area(const core::recti bounds)
	{
		return display_area(bounds, true);
	}

	core::surface_ptr capture_window_surface(const std::any& window_handle)
	{
		const auto* const handle = std::any_cast<SDL_Window*>(&window_handle);
		if (!handle || !*handle) return {};
		const auto id = SDL_GetWindowID(*handle);
		if (auto* const window = linux_detail::window_for(id))
		{
			window->update_now();
			if (linux_detail::window_for(id) != window) return {};
		}
		auto* const surface = SDL_GetWindowSurface(*handle);
		if (!surface || surface->w < 1 || surface->h < 1) return {};

		auto result = std::make_shared<core::surface>();
		result->alloc(surface->w, surface->h, core::texture_format::ARGB, core::orientation::top_left);
		if (SDL_MUSTLOCK(surface)) SDL_LockSurface(surface);
		for (auto y = 0; y < surface->h; ++y)
		{
			const auto* const source = static_cast<const uint8_t*>(surface->pixels) +
				static_cast<ptrdiff_t>(y) * surface->pitch;
			auto* const destination = result->pixels_line(y);
			SDL_ConvertPixels(surface->w, 1, surface->format, source, surface->pitch,
				SDL_PIXELFORMAT_ARGB8888, destination, static_cast<int>(result->stride()));
			auto* const pixels = reinterpret_cast<core::color32*>(destination);
			for (auto x = 0; x < surface->w; ++x) pixels[x] |= 0xff000000u;
		}
		if (SDL_MUSTLOCK(surface)) SDL_UnlockSurface(surface);
		return result;
	}

	int run(const std::shared_ptr<app> application, const int argc, char** argv)
	{
		if (!application) return 1;
		platform::detail::set_task_shutdown_bounded(true);
		set_ui_thread();
		start_log();
		const core::scope_exit close_log_at_end([] { close_log(); });
		const core::scope_exit final_exit([application]
		{
			{
				std::lock_guard lock(ui_task_mutex);
				ui_accepting = false;
				ui_tasks.clear();
			}
			ui_task_ready.notify_all();
			application->final_exit();
			const auto hooks = std::exchange(shutdown_hooks, {});
			for (const auto& hook : hooks)
				if (hook) hook();
		});
		active_app = application;
		is_closing = false;
		{
			std::lock_guard lock(ui_task_mutex);
			ui_accepting = true;
		}
		event_keys.clear();
		event_exit.reset();
		quit_requested = false;
		quit_code = 0;
		ticks_since_last_user_action = 0;
		frame_delay = 0;
		frame_delay_chosen = false;
		frame_due = UINT64_MAX;

		if (!std::setlocale(LC_CTYPE, "C.UTF-8"))
		{
			std::fprintf(stderr, "Cannot initialize the UTF-8 character locale required for filenames.\n");
			active_app.reset();
			return 1;
		}

		int result = 0;
		try
		{
			if (!application->pre_init())
			{
				active_app.reset();
				return 0;
			}

			const auto command_line = command_line_of(argc, argv);
			if (application->parse_command_line(command_line))
			{
				platform::detail::set_task_shutdown_bounded(false);
				result = application->run_console_mode();
				active_app.reset();
				return result;
			}

			if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS))
			{
				std::fprintf(stderr, "Cannot open a display: %s\n", SDL_GetError());
				active_app.reset();
				return 1;
			}
			const core::scope_exit quit_sdl([] { SDL_Quit(); });
			const core::scope_exit uninstall_clipboard([] { install_text_clipboard({}, {}); });

			install_text_clipboard(
				[]() -> std::optional<std::string>
				{
					if (!SDL_HasClipboardText()) return std::nullopt;
					auto* const text = SDL_GetClipboardText();
					if (!text) return std::nullopt;
					std::string result(text);
					SDL_free(text);
					return result;
				},
				[](const std::string_view text)
				{
					return SDL_SetClipboardText(std::string(text).c_str());
				});

			const auto events = SDL_RegisterEvents(3);
			if (events != 0 && events != no_event)
			{
				event_idle = events;
				event_folder = events + 1;
				event_ui = events + 2;
				watcher.start([]
				{
					if (event_folder == no_event) return;
					SDL_Event event{};
					event.type = static_cast<SDL_EventType>(event_folder);
					SDL_PushEvent(&event);
				});
			}
			running = true;
			user_action_due = SDL_GetTicks() + user_action_period_ms;
			application->load_settings(settings());
			if (application->init(command_line))
			{
				// Creation is hidden so init can finish laying out the tree. The old loop shows every app
				// window only after init, then paints it once before entering the event pump.
				const auto app_windows = platform::linux_detail::windows();
				for (auto* const window : app_windows)
				{
					if (window->kind() != platform::window_kind::app) continue;
					window->show(true);
					if (window->native()) SDL_SyncWindow(window->native());
				}
				for (auto* const window : app_windows)
					if (window->kind() == platform::window_kind::app) window->update_now();
				if (!frame_delay_chosen) apply_frame_delay(default_frame_delay_ms);
				if (application->owns_loop()) result = application->main_loop();
				else
				{
					pump([] { return quit_requested.load(); }, UINT32_MAX, nullptr);
					result = quit_code;
				}
			}
			else
			{
				result = quit_code;
			}
			{
				std::lock_guard lock(ui_task_mutex);
				ui_accepting = false;
			}
			application->exit();
			running = false;
			watcher.stop();
			shutdown_windows();
			reset_events();
		}
		catch (const std::exception& exception)
		{
			core::log(__FUNCTION__, exception.what());
			std::fprintf(stderr, "%s\n", exception.what());
			result = 1;
			if (running) running = false;
			watcher.stop();
			shutdown_windows();
			reset_events();
		}
		catch (...)
		{
			core::log(__FUNCTION__, "Unhandled exception");
			std::fprintf(stderr, "Unhandled exception\n");
			result = 1;
			running = false;
			watcher.stop();
			shutdown_windows();
			reset_events();
		}

		active_app.reset();
		return result;
	}
}