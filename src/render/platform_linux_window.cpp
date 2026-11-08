// This file is part of libdesktop, a C++20 platform layer for desktop applications
// Copyright 2026  Zac Walker
// 
// This program is free software; you can redistribute it and / or modify it
// under the terms of the LGPL License either version 2.1 or later.
// License details are available at https://www.gnu.org/licenses/lgpl-2.1.html
// This program is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY

// Purpose: The Linux window layer, on SDL3 - the counterpart of platform_win_ui.cpp. The application's
// window, its dialogs and its popups are SDL windows; the frames inside a window are regions of it,
// drawn into its one surface, since Wayland and SDL give an application no child windows. A control
// host's controls and menus are the drawn ones every platform shares (render_controls.h), fed its input
// here. The event loop runs timers, idle work and folder notifications, and input arrives as the key
// codes and mouse calls the portable UI was written against. Every window draws through the portable
// software renderer with Pango's text. Linux's wWinMain, libdesktop_linux_main, is here, called from
// platform_linux_main.cpp.

#include "libdesktop/render.h"
#include "libdesktop/render_controls.h"
#include "libdesktop/render_software.h"

#include <SDL3/SDL.h>

#include <poll.h>
#include <sys/eventfd.h>
#include <sys/inotify.h>
#include <unistd.h>

#include <atomic>
#include <map>
#include <mutex>
#include <set>
#include <thread>

ui::app_ptr create_app(const ui::plat_app_ptr& pa);

namespace
{
	// The Windows layer's metrics, so a layout measured on one platform is spaced as on the other.
	constexpr auto ui_component_snap = 8;
	constexpr auto ui_baseline_snap = 4;
	constexpr auto ui_cx_resize_handle = 12;
	constexpr auto ui_scroll_width = 20;
	constexpr auto ui_base_icon_cxy = 18;
	constexpr auto ui_resize_side_thickness = 9;

	int global_base_font_size = ui::default_font_size;

	class sdl_app;
	class native_window;
	class node;

	sdl_app* the_app = nullptr;

	ui::key_state key_state_of(const SDL_Keymod mod)
	{
		ui::key_state ks;
		ks.control = (mod & SDL_KMOD_CTRL) != 0;
		ks.shift = (mod & SDL_KMOD_SHIFT) != 0;
		ks.alt = (mod & SDL_KMOD_ALT) != 0;
		return ks;
	}

	// SDL's key codes as the Win32 virtual keys the portable UI was written against: the keys:: table on
	// Linux holds those values, so the meaning of a key stays in one place.
	char32_t to_key(const SDL_Keycode k)
	{
		if (k >= SDLK_A && k <= SDLK_Z) return U'A' + static_cast<char32_t>(k - SDLK_A);
		if (k >= SDLK_0 && k <= SDLK_9) return U'0' + static_cast<char32_t>(k - SDLK_0);
		if (k >= SDLK_F1 && k <= SDLK_F12) return 0x70 + static_cast<char32_t>(k - SDLK_F1);
		if (k >= SDLK_F13 && k <= SDLK_F24) return 0x7C + static_cast<char32_t>(k - SDLK_F13);
		if (k >= SDLK_KP_1 && k <= SDLK_KP_9) return 0x61 + static_cast<char32_t>(k - SDLK_KP_1);

		switch (k)
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

	// The portable cursors as the system's own: Linux themes have no zoom or grab-hand cursor of the
	// Windows layer's kind, so those take the nearest standard shape.
	void apply_cursor(const ui::style::cursor cursor)
	{
		static std::map<SDL_SystemCursor, SDL_Cursor*> cursors;

		if (cursor == ui::style::cursor::none)
		{
			SDL_HideCursor();
			return;
		}

		auto id = SDL_SYSTEM_CURSOR_DEFAULT;

		switch (cursor)
		{
		case ui::style::cursor::link:
		case ui::style::cursor::hand_up:
		case ui::style::cursor::hand_down: id = SDL_SYSTEM_CURSOR_POINTER;
			break;
		case ui::style::cursor::text_select: id = SDL_SYSTEM_CURSOR_TEXT;
			break;
		case ui::style::cursor::move:
		case ui::style::cursor::size_all: id = SDL_SYSTEM_CURSOR_MOVE;
			break;
		case ui::style::cursor::left_right: id = SDL_SYSTEM_CURSOR_EW_RESIZE;
			break;
		case ui::style::cursor::up_down: id = SDL_SYSTEM_CURSOR_NS_RESIZE;
			break;
		case ui::style::cursor::zoom:
		case ui::style::cursor::select: id = SDL_SYSTEM_CURSOR_CROSSHAIR;
			break;
		default: break;
		}

		auto& c = cursors[id];
		if (!c) c = SDL_CreateSystemCursor(id);
		if (c) SDL_SetCursor(c);
		SDL_ShowCursor();
	}

	//////////////////////////////////////////////////////////////////////////////////////////////
	// sdl_present_target - the software renderer's tiles copied into the window's surface.
	//////////////////////////////////////////////////////////////////////////////////////////////

	class sdl_present_target final : public ui::software_present_target
	{
		SDL_Window* _window = nullptr;
		bool _layered = false;
		std::vector<uint8_t> _pixels;
		sizei _extent;
		SDL_Surface* _surface = nullptr;
		std::vector<SDL_Rect> _rects;

	public:
		sdl_present_target(SDL_Window* window, const bool layered) : _window(window), _layered(layered)
		{
		}

		bool is_layered() const override
		{
			return _layered;
		}

		ui::software_buffer acquire_buffer(const sizei extent) override
		{
			if (extent.cx < 1 || extent.cy < 1)
			{
				release_buffer();
				return {};
			}

			if (_extent != extent)
			{
				_pixels.assign(static_cast<size_t>(extent.cx) * extent.cy * 4, 0);
				_extent = extent;
			}

			return {_pixels.data(), _extent, _extent.cx * 4};
		}

		void release_buffer() override
		{
			_pixels.clear();
			_pixels.shrink_to_fit();
			_extent = {};
		}

		bool begin_present() override
		{
			_rects.clear();
			_surface = SDL_GetWindowSurface(_window);
			if (!_surface || _pixels.empty()) return false;
			if (SDL_MUSTLOCK(_surface)) SDL_LockSurface(_surface);
			return true;
		}

		void present_tile(const recti tile, const pointi buffer_origin) override
		{
			if (!_surface) return;

			const auto clipped = tile.intersection(recti(0, 0, _surface->w, _surface->h));
			if (clipped.is_empty()) return;

			const auto stride = _extent.cx * 4;
			const auto native = _surface->format == SDL_PIXELFORMAT_ARGB8888 ||
				_surface->format == SDL_PIXELFORMAT_XRGB8888;

			for (auto y = clipped.top; y < clipped.bottom; ++y)
			{
				const auto* const src = _pixels.data() + static_cast<ptrdiff_t>(y - buffer_origin.y) * stride +
					static_cast<ptrdiff_t>(clipped.left - buffer_origin.x) * 4;
				auto* const dst = static_cast<uint8_t*>(_surface->pixels) + static_cast<ptrdiff_t>(y) * _surface->pitch +
					static_cast<ptrdiff_t>(clipped.left) * SDL_BYTESPERPIXEL(_surface->format);

				if (_layered)
				{
					// A compositor takes a transparent window's pixels premultiplied.
					for (auto x = 0; x < clipped.width(); ++x)
					{
						const auto* const s = src + x * 4;
						auto* const d = dst + x * 4;
						const auto a = s[3];
						d[0] = static_cast<uint8_t>(s[0] * a / 255);
						d[1] = static_cast<uint8_t>(s[1] * a / 255);
						d[2] = static_cast<uint8_t>(s[2] * a / 255);
						d[3] = a;
					}
				}
				else if (native)
				{
					memcpy(dst, src, static_cast<size_t>(clipped.width()) * 4);
				}
				else
				{
					SDL_ConvertPixels(clipped.width(), 1, SDL_PIXELFORMAT_ARGB8888, src, stride, _surface->format, dst,
					                  _surface->pitch);
				}
			}

			_rects.push_back({clipped.left, clipped.top, clipped.width(), clipped.height()});
		}

		void end_present(int) override
		{
			if (!_surface) return;
			if (SDL_MUSTLOCK(_surface)) SDL_UnlockSurface(_surface);
			if (!_rects.empty()) SDL_UpdateWindowSurfaceRects(_window, _rects.data(), static_cast<int>(_rects.size()));
			_surface = nullptr;
		}
	};

	//////////////////////////////////////////////////////////////////////////////////////////////
	// node - something that paints and takes input: a window's root, or a frame that is a region of its
	// parent's window. Owned by the application's references; a parent keeps its children unowned, and
	// each child takes itself out of the tree when it goes.
	//////////////////////////////////////////////////////////////////////////////////////////////

	class node
	{
	public:
		node* _parent = nullptr;
		native_window* _window = nullptr;
		std::unique_ptr<native_window> _own_window;
		std::vector<node*> _children;
		recti _bounds;
		bool _visible = false;
		bool _no_focus = false;
		// Takes the keyboard with a press on its own ground, as a pane that asks to does on Windows.
		bool _can_focus = false;
		int _disable_count = 0;
		ui::frame_host_weak_ptr _host;
		ui::color_style _colors;
		// A popup drawn with per-pixel alpha, which shows only what its host draws.
		bool _layered = false;
		// Holds frames and draws nothing but its background where they leave it uncovered.
		bool _container = false;
		ui::style::cursor _cursor = ui::style::cursor::normal;
		std::string _text;
		uint64_t _timer = 0;
		std::shared_ptr<pointi> _origin_cell = std::make_shared<pointi>();
		// The pointer is over one of the controls this node draws, so its host has been told it left.
		bool _over_controls = false;

		virtual ~node();

		// This node as the application holds it, to keep it alive across a call that might release it.
		virtual std::shared_ptr<ui::frame> keep_alive() = 0;

		// The input manager that gives a frame's drawn controls its pointer and keyboard; none for any
		// other node.
		virtual ui::input_manager* controls() { return nullptr; }

		// Where the caret of a control here that takes typing is, in window pixels; empty when none is.
		virtual recti caret_area() const { return {}; }

		// The node took or lost the window's keyboard.
		virtual void keyboard_changed(bool has_keyboard);

		bool is_root() const
		{
			return _parent == nullptr;
		}

		pointi window_origin() const
		{
			auto p = _bounds.top_left();
			for (const auto* n = _parent; n; n = n->_parent) p = p + n->_bounds.top_left();
			return p;
		}

		recti window_rect() const
		{
			return {window_origin(), _bounds.extent()};
		}

		bool shown() const
		{
			for (const auto* n = this; n; n = n->_parent)
			{
				if (!n->_visible) return false;
			}

			return true;
		}

		bool enabled() const
		{
			for (const auto* n = this; n; n = n->_parent)
			{
				if (n->_disable_count > 0) return false;
			}

			return true;
		}

		void attach_child(node* parent)
		{
			_parent = parent;
			_window = parent->_window;
			parent->_children.push_back(this);
		}

		void detach()
		{
			if (_parent)
			{
				std::erase(_parent->_children, this);
				_parent = nullptr;
			}
		}

		void set_window(native_window* w)
		{
			_window = w;
			for (auto* c : _children) c->set_window(w);
		}

		void invalidate_rect(recti bounds) const;
		void layout_now(bool is_minimized = false);
		void paint_tree(ui::draw_context& dc, pointi origin, recti visible);
		// How a window's root fills it: its background, then whatever its host draws.
		void paint_root(ui::draw_context& dc);
		node* hit(pointi local);
		void set_bounds(recti bounds, bool visible);
		void start_timer(int milliseconds, std::function<void()> tick);
		void stop_timer();
	};

	//////////////////////////////////////////////////////////////////////////////////////////////
	// native_window - one SDL window, its draw context and what is happening in it.
	//////////////////////////////////////////////////////////////////////////////////////////////

	class native_window final
	{
	public:
		SDL_Window* sdl = nullptr;
		SDL_WindowID id = 0;
		node* root = nullptr;
		bool layered = false;
		std::shared_ptr<sdl_present_target> target;
		ui::draw_context_device_ptr ctx;
		double scale = 1.0;
		float density = 1.0f;
		sizei extent;
		recti damage;
		bool dirty = false;
		bool minimized = false;
		bool keyboard_focus = false;
		node* focus = nullptr;
		node* capture = nullptr;
		node* hover = nullptr;
		// What text input was last told: whether it runs, and where its candidates go.
		bool text_input = false;
		recti text_area;

		native_window(SDL_Window* window, node* r, bool is_layered);
		~native_window();

		int base_font_size() const
		{
			return df::round(scale * global_base_font_size);
		}

		void update_metrics()
		{
			scale = std::max(0.5, static_cast<double>(SDL_GetWindowDisplayScale(sdl)));
			density = std::max(0.5f, SDL_GetWindowPixelDensity(sdl));

			if (ctx)
			{
				// The large-font preference is folded in, as the Windows layer folds it, so spacing grows
				// with the text.
				const auto ui_scale = scale * global_base_font_size / static_cast<double>(ui::default_font_size);
				ctx->update_font_size(base_font_size());
				ctx->scale_factor = ui_scale;
				ctx->icon_cxy = df::round(ui_base_icon_cxy * ui_scale);
				ctx->padding2 = df::round(ui_component_snap * ui_scale);
				ctx->padding1 = df::round(ui_baseline_snap * ui_scale);
				ctx->handle_cxy = df::round(ui_cx_resize_handle * ui_scale);
				ctx->scroll_width = df::round(ui_scroll_width * ui_scale);
			}
		}

		void sync_size()
		{
			int cx = 0;
			int cy = 0;
			SDL_GetWindowSizeInPixels(sdl, &cx, &cy);
			extent = {cx, cy};
			if (ctx) ctx->resize(extent);
			if (root) root->_bounds = recti(0, 0, cx, cy);
		}

		void invalidate(const recti r)
		{
			const auto client = recti(0, 0, extent.cx, extent.cy);
			const auto area = r.is_empty() ? client : r.intersection(client);
			if (area.is_empty()) return;
			damage = damage.is_empty() ? area : damage.make_union(area);
			dirty = true;
		}

		pointi to_pixels(const float x, const float y) const
		{
			return {df::round(x * density), df::round(y * density)};
		}

		// Where this window's client sits in screen pixels. Wayland tells a client nothing of where its
		// windows are, so there the window's own corner is the origin, and popups placed against it land
		// where they belong.
		pointi screen_origin() const
		{
			int x = 0;
			int y = 0;
			SDL_GetWindowPosition(sdl, &x, &y);
			return to_pixels(static_cast<float>(x), static_cast<float>(y));
		}

		node* hit(const pointi p) const
		{
			return root ? root->hit(p) : nullptr;
		}

		void set_focus(node* n);
		void paint();
		void redraw();
	};

	//////////////////////////////////////////////////////////////////////////////////////////////
	// sdl_app - the event loop and the platform_app the application drives.
	//////////////////////////////////////////////////////////////////////////////////////////////

	class frame_node;

	class folder_watcher final
	{
		int _fd = -1;
		int _wake = -1;
		std::thread _thread;
		std::mutex _mutex;
		std::map<int, df::folder_path> _watches;
		std::set<std::string> _pending_text;
		std::vector<df::folder_path> _pending;
		std::function<void()> _notify;

		void run()
		{
			std::array<char, 16384> buffer{};

			while (true)
			{
				pollfd fds[2] = {{_fd, POLLIN, 0}, {_wake, POLLIN, 0}};
				if (poll(fds, 2, -1) < 0)
				{
					if (errno == EINTR) continue;
					return;
				}

				if (fds[1].revents) return;
				if (!(fds[0].revents & POLLIN)) continue;

				const auto n = read(_fd, buffer.data(), buffer.size());
				if (n <= 0) continue;

				auto first = false;
				{
					std::lock_guard lock(_mutex);

					for (auto i = 0; i < n;)
					{
						const auto* const e = reinterpret_cast<const inotify_event*>(buffer.data() + i);
						const auto found = _watches.find(e->wd);

						if (found != _watches.end())
						{
							const auto text = std::string(found->second.text());
							if (_pending_text.insert(text).second)
							{
								first = _pending.empty();
								_pending.push_back(found->second);
							}
						}

						i += static_cast<int>(sizeof(inotify_event) + e->len);
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
		}

		// Each folder alone, not its subtree, with the changes the Windows layer watches for: names,
		// attributes and writes.
		void watch(const std::vector<df::folder_path>& folders)
		{
			if (_fd < 0) return;
			std::lock_guard lock(_mutex);

			for (const auto& [wd, path] : _watches) inotify_rm_watch(_fd, wd);
			_watches.clear();

			constexpr auto mask = IN_ATTRIB | IN_CLOSE_WRITE | IN_CREATE | IN_DELETE | IN_MOVED_FROM | IN_MOVED_TO |
				IN_DELETE_SELF | IN_MOVE_SELF;

			for (const auto& folder : folders)
			{
				const auto text = std::string(folder.text());
				const auto wd = inotify_add_watch(_fd, text.c_str(), mask);
				if (wd >= 0) _watches[wd] = folder;
			}
		}

		std::vector<df::folder_path> take()
		{
			std::lock_guard lock(_mutex);
			_pending_text.clear();
			return std::exchange(_pending, {});
		}
	};

	class sdl_app final : public ui::platform_app
	{
		struct timer
		{
			uint64_t id = 0;
			uint64_t period = 0;
			uint64_t due = 0;
			std::function<void()> tick;
		};

		std::vector<timer> _timers;
		uint64_t _next_timer_id = 1;
		uint64_t _frame_timer = 0;
		int _frame_delay = 0;
		std::atomic_bool _idle_pending = false;
		folder_watcher _watcher;
		// The drawn menu open now, which takes every key and dismisses on a press elsewhere.
		ui::menu_session* _menu = nullptr;

	public:
		ui::app_ptr _app;
		std::shared_ptr<frame_node> _frame;
		std::map<SDL_WindowID, native_window*> _windows;
		bool _quit = false;
		int _modal_depth = 0;
		uint32_t _event_idle = 0;
		uint32_t _event_folder = 0;

		void init_events()
		{
			const auto base = SDL_RegisterEvents(2);
			_event_idle = base;
			_event_folder = base + 1;

			_watcher.start([this]
			{
				SDL_Event e{};
				e.type = _event_folder;
				SDL_PushEvent(&e);
			});
		}

		void shutdown()
		{
			_watcher.stop();
			_timers.clear();
		}

		uint64_t add_timer(const int milliseconds, std::function<void()> tick)
		{
			const auto period = static_cast<uint64_t>(std::max(1, milliseconds));
			const auto id = _next_timer_id++;
			_timers.push_back({id, period, SDL_GetTicks() + period, std::move(tick)});
			return id;
		}

		void remove_timer(const uint64_t id)
		{
			std::erase_if(_timers, [id](const timer& t) { return t.id == id; });
		}

		native_window* window(const SDL_WindowID id) const
		{
			const auto found = _windows.find(id);
			return found != _windows.end() ? found->second : nullptr;
		}

		// Runs the loop until done answers true, the application quits or the timeout passes. A dialog's
		// wait and a menu run it nested, as Windows runs a nested message loop.
		bool pump(const std::function<bool()>& done, uint32_t timeout_ms);

		void run();
		void dispatch(const SDL_Event& e);
		void request_app_close();

		void sys_command(ui::sys_command_type cmd) override;
		void full_screen(bool full) override;
		void frame_delay(int delay) override;

		void queue_idle() override
		{
			// Any thread. Collapsed, so a burst of queued work wakes the loop once.
			if (!_idle_pending.exchange(true))
			{
				SDL_Event e{};
				e.type = _event_idle;
				SDL_PushEvent(&e);
			}
		}

		ui::frame_ptr create_app_frame(const platform::setting_file_ptr& store, const ui::frame_host_weak_ptr& host,
		                               const ui::frame_style& app_style) override;

		void monitor_folders(const std::vector<df::folder_path>& folders) override
		{
			_watcher.watch(folders);
		}

		void enable_screen_saver(const bool enable) override
		{
			if (enable) SDL_EnableScreenSaver();
			else SDL_DisableScreenSaver();
		}

		void set_font_base_size(int size) override;
		void show_progress(int64_t done, int64_t total) override;
		void end_progress() override;

		// Text input follows the control or view that takes typing, from window to window.
		void update_text_input();
		void run_menu_loop(ui::menu_session& session);

	private:
		void fire_timers();
		void paint_windows();
		bool menu_event(const SDL_Event& e);
		void mouse_motion(native_window* w, pointi pt);
		void mouse_button(native_window* w, const SDL_MouseButtonEvent& e, bool down);
		void mouse_wheel(native_window* w, const SDL_MouseWheelEvent& e);
		void key_down(native_window* w, const SDL_KeyboardEvent& e);
	};

	//////////////////////////////////////////////////////////////////////////////////////////////
	// The ui:: frame interfaces over a node.
	//////////////////////////////////////////////////////////////////////////////////////////////

	template <class T, class TBase>
	class node_window : public TBase, public node
	{
	public:
		std::any handle() const override
		{
			return static_cast<node*>(const_cast<node_window*>(this));
		}

		void enable(const bool enable) override
		{
			if (enable)
			{
				if (_disable_count > 0) --_disable_count;
			}
			else
			{
				++_disable_count;
			}
		}

		std::string window_text() const override
		{
			return _text;
		}

		void window_text(const std::string_view text) override
		{
			_text = text;
			if (is_root() && _window) SDL_SetWindowTitle(_window->sdl, _text.c_str());
		}

		void focus() override
		{
			if (!_window) return;
			_window->set_focus(this);
			if (is_root()) SDL_RaiseWindow(_window->sdl);
		}

		bool is_visible() const override
		{
			return shown();
		}

		bool has_focus() const override
		{
			return _window && _window->keyboard_focus && _window->focus == this;
		}

		// In screen pixels, as the Windows layer answers, so a popup placed against it lands beside it.
		recti window_bounds() const override
		{
			if (!_window) return {};
			const auto origin = _window->screen_origin();
			return window_rect().offset(origin);
		}

		void window_bounds(const recti bounds, const bool visible) override
		{
			set_bounds(bounds, visible);
		}

		void show(const bool show) override
		{
			set_bounds(_bounds, show);
		}

		void options_changed() override
		{
			invalidate_rect({});
		}

		void invalidate(const recti bounds, const bool) override
		{
			df::bump(df::ui_perf.invalidates);
			invalidate_rect(bounds);
		}

		void layout() override
		{
			layout_now(_window && _window->minimized);
		}

		void redraw() override
		{
			df::bump(df::ui_perf.redraws);
			if (_window) _window->redraw();
		}

		void redraw_now() override
		{
			invalidate_rect({});
			if (_window) _window->paint();
		}

		// The scrolled region is drawn again; a host whose content holds controls or frames places them
		// again as well.
		void scroll(int, int, const recti bounds) override
		{
			invalidate_rect(bounds);
		}

		void track_menu(const recti button_bounds, const std::vector<ui::command_ptr>& commands) override;

		bool is_enabled() const override
		{
			return enabled();
		}

		bool is_maximized() const override
		{
			return is_root() && _window && (SDL_GetWindowFlags(_window->sdl) & SDL_WINDOW_MAXIMIZED) != 0;
		}

		bool is_occluded() const override
		{
			return !_window || _window->minimized || !shown() ||
				(SDL_GetWindowFlags(_window->sdl) & (SDL_WINDOW_HIDDEN | SDL_WINDOW_OCCLUDED)) != 0;
		}

		void set_cursor(const ui::style::cursor cursor) override
		{
			_cursor = cursor;
			if (_window && (_window->hover == this || _window->capture == this)) apply_cursor(cursor);
		}

		void reset_graphics() override
		{
			invalidate_rect({});
		}

		pointi cursor_location() override
		{
			if (!_window) return {-1, -1};

			float x = 0;
			float y = 0;

			if (SDL_GetMouseFocus() == _window->sdl)
			{
				SDL_GetMouseState(&x, &y);
				const auto p = _window->to_pixels(x, y);
				const auto origin = window_origin();
				return {p.x - origin.x, p.y - origin.y};
			}

			SDL_GetGlobalMouseState(&x, &y);
			const auto screen = _window->to_pixels(x, y);
			const auto bounds = window_bounds();
			return {screen.x - bounds.left, screen.y - bounds.top};
		}
	};

	// What a frame gives the controls it draws, the menus it opens and the bubbles it shows.
	class host_services final : public ui::control_host, public ui::menu_host, public ui::bubble_host
	{
	public:
		frame_node* owner = nullptr;
		bool typing = false;
		recti caret;
		uint64_t caret_timer_id = 0;

		~host_services() override;

		ui::measure_context* measure() override;
		void invalidate(recti bounds) override;
		bool has_keyboard_focus() const override;
		void take_keyboard_focus() override;
		void capture_pointer(bool capture) override;
		void set_cursor(ui::style::cursor cursor) override;
		pointi client_to_screen(pointi loc) const override;
		void track_menu(recti screen_bounds, const std::vector<ui::command_ptr>& commands) override;
		ui::frame_ptr create_popup(const ui::frame_host_weak_ptr& content, const ui::color_style& colors) override;
		std::shared_ptr<ui::frame_host> listener() const override;
		void keyboard_to_frame() override;
		void text_input(bool active, recti caret_bounds) override;
		void caret_timer(bool on, uint32_t interval_ms) override;
		bool is_dialog() const override;
		void close(bool cancel) override;

		ui::measure_context* menu_measure() override;
		ui::frame_ptr create_menu_popup(const ui::frame_host_weak_ptr& content, const ui::frame_ptr& parent) override;
		recti menu_work_area(recti bounds) override;
		void run_menu_loop(ui::menu_session& session) override;

		ui::frame_ptr create_bubble_popup(const ui::frame_host_weak_ptr& content) override;
		ui::measure_context* bubble_measure(const ui::frame_ptr& popup) override;
		recti bubble_work_area(recti anchor) override;
	};

	// A frame: the application's window, a dialog's window, a region of another window - a pane of any
	// style - or a popup, which a layered style makes a bubble's. The drawn controls in its host's tree get
	// its pointer and keyboard through its input manager, the same on every platform.
	class frame_node final : public node_window<frame_node, ui::frame>,
	                         public std::enable_shared_from_this<frame_node>
	{
	public:
		enum class kind
		{
			app,
			dialog,
			pane
		};

		const kind _kind;
		bool _close_signalled = false;
		ui::close_result _modal_result = ui::close_result::ok;
		host_services _services;
		ui::input_manager_ptr _controls;

		frame_node(const ui::frame_host_weak_ptr& host, const kind k, const ui::frame_style& style) : _kind(k)
		{
			_host = host;
			_colors = style.colors;
			_no_focus = style.no_focus;
			_can_focus = style.can_focus;
			_layered = style.layered;
			_container = style.container;
			_services.owner = this;
			_controls = std::make_shared<ui::input_manager>(&_services);
		}

		~frame_node() override
		{
			_controls->detach();
		}

		bool is_app_frame() const
		{
			return _kind == kind::app;
		}

		bool is_dialog() const
		{
			return _kind == kind::dialog;
		}

		std::shared_ptr<ui::frame> keep_alive() override
		{
			return weak_from_this().lock();
		}

		ui::input_manager* controls() override
		{
			return _controls.get();
		}

		recti caret_area() const override
		{
			if (!_services.typing || _services.caret.is_empty()) return {};
			return _services.caret.offset(window_origin());
		}

		void keyboard_changed(const bool has_keyboard) override
		{
			const auto keep = keep_alive();

			// The control that holds the focus is what the host hears about, as a native control's own
			// focus was; the window itself only when no control does.
			if (_controls->keyboard_focus(has_keyboard)) invalidate_rect({});
			else node::keyboard_changed(has_keyboard);
		}

		void destroy() override;

		// The application's window asks the application first, and a pane goes; a dialog answers its wait.
		void close(const bool is_cancel) override
		{
			if (is_app_frame())
			{
				the_app->request_app_close();
			}
			else if (_kind == kind::pane)
			{
				destroy();
			}
			else
			{
				_modal_result = is_cancel ? ui::close_result::cancel : ui::close_result::ok;
				_close_signalled = true;
			}
		}

		ui::frame_ptr create_dlg(ui::frame_host_weak_ptr host) override;
		ui::frame_ptr create_frame(ui::frame_host_weak_ptr host, const ui::frame_style& style) override;
		ui::bubble_window_ptr create_bubble() override;

		ui::close_result wait_for_close(uint32_t timeout_ms) override;

		void focus_first() override
		{
			if (!_controls->focus_first()) focus();
		}

		// The frame itself takes the keyboard, from any of its controls that held it: a node that has it
		// already hears of no change from its window, so the control lets it go here.
		void focus() override
		{
			_controls->clear_focus();
			node_window::focus();
		}

		ui::control_ptr focused_control() const override
		{
			return _controls->has_keyboard_focus() ? _controls->focused() : nullptr;
		}

		void position(recti bounds) override;

		void save_window_position(platform::setting_file_ptr& store) override;

		bool is_canceled() const override
		{
			return _modal_result == ui::close_result::cancel;
		}

		double scale_factor() const override
		{
			return _window ? _window->scale : 1.0;
		}
	};

	//////////////////////////////////////////////////////////////////////////////////////////////
	// node
	//////////////////////////////////////////////////////////////////////////////////////////////

	node::~node()
	{
		stop_timer();

		// Orphaned children draw nowhere until their owner lets them go.
		for (auto* c : _children)
		{
			c->_parent = nullptr;
			c->set_window(nullptr);
		}

		_children.clear();

		if (_window)
		{
			if (_window->focus == this) _window->focus = nullptr;
			if (_window->capture == this) _window->capture = nullptr;
			if (_window->hover == this) _window->hover = nullptr;
			if (_visible && _parent) _window->invalidate(window_rect());
		}

		detach();
		_own_window.reset();
	}

	void node::invalidate_rect(const recti bounds) const
	{
		if (!_window) return;
		const auto area = window_rect();
		_window->invalidate(bounds.is_empty() ? area : bounds.offset(area.top_left()).intersection(area));
	}

	void node::keyboard_changed(const bool has_keyboard)
	{
		const auto keep = keep_alive();
		const auto h = _host.lock();
		if (h) h->focus_changed(has_keyboard, nullptr);
		invalidate_rect({});
	}

	void node::layout_now(const bool is_minimized)
	{
		if (!_window || !_window->ctx || _bounds.is_empty()) return;
		const auto keep = keep_alive();
		const auto h = _host.lock();
		if (!h) return;

		h->on_window_layout(*_window->ctx, _bounds.extent(), is_minimized);
		if (is_minimized) return;

		// Then the frames its frame elements hold, where the layout put them.
		if (auto* const in = controls())
		{
			for (const auto& f : in->layout())
			{
				if (auto* const n = dynamic_cast<node*>(f.frame.get())) n->set_bounds(f.bounds, f.visible);
			}
		}
	}

	void node::set_bounds(const recti bounds, const bool visible)
	{
		if (!is_root())
		{
			const auto resized = bounds.extent() != _bounds.extent();
			const auto changed = resized || bounds.top_left() != _bounds.top_left() || visible != _visible;

			if (changed && _window && _visible) _window->invalidate(window_rect());
			_bounds = bounds;
			_visible = visible;
			if (changed && _window && _visible) _window->invalidate(window_rect());
			if (resized) layout_now();
			return;
		}

		if (!_window) return;

		// A popup sits against the window that owns it; a top-level window is placed where the system
		// allows, which on Wayland is nowhere the client chooses.
		const auto* const owner_window = _window;
		auto* const parent_sdl = SDL_GetWindowParent(owner_window->sdl);
		const auto density = owner_window->density;

		if (!bounds.is_empty())
		{
			if (parent_sdl && (SDL_GetWindowFlags(owner_window->sdl) & (SDL_WINDOW_POPUP_MENU | SDL_WINDOW_TOOLTIP)))
			{
				int px = 0;
				int py = 0;
				SDL_GetWindowPosition(parent_sdl, &px, &py);
				const auto parent_density = SDL_GetWindowPixelDensity(parent_sdl);
				const auto parent_origin = pointi(df::round(px * parent_density), df::round(py * parent_density));
				SDL_SetWindowPosition(owner_window->sdl, df::round((bounds.left - parent_origin.x) / density),
				                      df::round((bounds.top - parent_origin.y) / density));
			}
			else
			{
				SDL_SetWindowPosition(owner_window->sdl, df::round(bounds.left / density),
				                      df::round(bounds.top / density));
			}

			SDL_SetWindowSize(owner_window->sdl, std::max(1, df::round(bounds.width() / density)),
			                  std::max(1, df::round(bounds.height() / density)));
		}

		if (visible != _visible)
		{
			_visible = visible;
			if (visible) SDL_ShowWindow(owner_window->sdl);
			else SDL_HideWindow(owner_window->sdl);
		}

		// As on Windows, where WM_SIZE comes only with a new size: a window placed at the size it has is
		// not laid out again, which is what ends a layout that places its own window.
		const auto before = _window->extent;
		_window->sync_size();
		if (_window->extent != before) layout_now(_window->minimized);
		_window->invalidate({});
	}

	void node::paint_tree(ui::draw_context& dc, const pointi origin, const recti visible)
	{
		if (!_visible) return;

		const auto area = recti(origin, _bounds.extent()).intersection(visible);
		if (area.is_empty()) return;

		*_origin_cell = origin;
		const auto h = _host.lock();
		const auto has_focus = _window && _window->keyboard_focus && _window->focus == this;

		// Its controls are found again first, so each is drawn as enabled as the tree above it is now.
		if (auto* const in = controls()) in->refresh();

		if (is_root())
		{
			paint_root(dc);
		}
		else
		{
			ui::scoped_clip clip(dc, area);
			ui::offset_draw_context child_dc(dc, _origin_cell);
			child_dc.frame_has_focus = has_focus;
			child_dc.colors = _colors;
			child_dc.clear(ui::color(_colors.background, 1.0f));
			if (h && !_container) h->on_window_paint(child_dc);
		}

		const auto children = _children;

		for (auto* c : children)
		{
			c->paint_tree(dc, origin + c->_bounds.top_left(), area);
		}
	}

	void node::paint_root(ui::draw_context& dc)
	{
		const auto h = _host.lock();
		dc.frame_has_focus = _window && _window->keyboard_focus && _window->focus == this;
		dc.colors = _colors;

		// A layered window shows only what its host draws.
		if (_layered)
		{
			if (auto* const device = dynamic_cast<ui::draw_context_device*>(&dc)) device->set_layer_alpha(255);
		}
		else
		{
			dc.clear(ui::color(_colors.background, 1.0f));
		}

		if (h && !_container) h->on_window_paint(dc);
	}

	node* node::hit(const pointi local)
	{
		for (auto i = _children.rbegin(); i != _children.rend(); ++i)
		{
			auto* const c = *i;
			if (c->_visible && c->_bounds.contains(local))
			{
				return c->hit(local - c->_bounds.top_left());
			}
		}

		return this;
	}

	void node::start_timer(const int milliseconds, std::function<void()> tick)
	{
		stop_timer();
		if (the_app && milliseconds > 0) _timer = the_app->add_timer(milliseconds, std::move(tick));
	}

	void node::stop_timer()
	{
		if (_timer && the_app) the_app->remove_timer(_timer);
		_timer = 0;
	}

	//////////////////////////////////////////////////////////////////////////////////////////////
	// native_window
	//////////////////////////////////////////////////////////////////////////////////////////////

	native_window::native_window(SDL_Window* window, node* r, const bool is_layered) :
		sdl(window), id(SDL_GetWindowID(window)), root(r), layered(is_layered)
	{
		// Asked for while the window is still hidden, as every window here is made. A window's first
		// surface can move SDL to drawing it through the GPU, which on X11 recreates the window - hides
		// it and shows it again - and a menu takes its popup being hidden as being dismissed.
		SDL_GetWindowSurface(sdl);
		target = std::make_shared<sdl_present_target>(sdl, layered);
		scale = std::max(0.5, static_cast<double>(SDL_GetWindowDisplayScale(sdl)));
		ctx = ui::create_software_draw_context(ui::create_text_engine(base_font_size()), target, base_font_size());
		update_metrics();
		if (the_app) the_app->_windows[id] = this;
		sync_size();
	}

	native_window::~native_window()
	{
		if (the_app) the_app->_windows.erase(id);
		if (ctx) ctx->destroy();
		ctx.reset();
		target.reset();
		if (sdl) SDL_DestroyWindow(sdl);
	}

	void native_window::set_focus(node* n)
	{
		if (focus == n) return;

		auto* const old = focus;
		focus = n;

		if (keyboard_focus)
		{
			if (old) old->keyboard_changed(false);
			if (n) n->keyboard_changed(true);
		}

		// The keyboard went to another frame of the application, which takes it from the control that
		// had it, as on Windows: the host it left must not hand it back to that control when it next has it.
		if (old)
		{
			if (auto* const set = old->controls()) set->clear_focus();
		}
	}

	void native_window::paint()
	{
		if (!ctx || !root || minimized || extent.is_empty() || !root->_visible)
		{
			dirty = false;
			damage = {};
			return;
		}

		const auto area = damage.is_empty() ? recti(0, 0, extent.cx, extent.cy) : damage;
		damage = {};
		dirty = false;

		const auto keep = root->keep_alive();
		ctx->begin_draw(extent, base_font_size(), area);
		ctx->time_now = df::now();
		root->paint_tree(*ctx, {0, 0}, recti(0, 0, extent.cx, extent.cy));
		ctx->render();
	}

	void native_window::redraw()
	{
		if (!ctx || minimized || !root || !root->_visible) return;
		ctx->reset_damage();
		ctx->render();
	}

	//////////////////////////////////////////////////////////////////////////////////////////////
	// Frames
	//////////////////////////////////////////////////////////////////////////////////////////////

	// Wayland moves and resizes a window the client does not decorate only when asked from a press on
	// it, so the caption and the borders answer here, from the same questions the Windows layer asks.
	SDL_HitTestResult SDLCALL window_hit_test(SDL_Window* window, const SDL_Point* area, void* data)
	{
		auto* const w = static_cast<native_window*>(data);
		if (!w || !w->root) return SDL_HITTEST_NORMAL;

		const auto p = w->to_pixels(static_cast<float>(area->x), static_cast<float>(area->y));
		const auto flags = SDL_GetWindowFlags(window);
		const auto resizable = (flags & SDL_WINDOW_RESIZABLE) != 0 &&
			(flags & (SDL_WINDOW_MAXIMIZED | SDL_WINDOW_FULLSCREEN)) == 0;

		if (resizable)
		{
			const auto side = df::round(ui_resize_side_thickness * w->scale);
			const auto left = p.x < side;
			const auto right = p.x >= w->extent.cx - side;
			const auto top = p.y < side;
			const auto bottom = p.y >= w->extent.cy - side;

			if (top && left) return SDL_HITTEST_RESIZE_TOPLEFT;
			if (top && right) return SDL_HITTEST_RESIZE_TOPRIGHT;
			if (bottom && left) return SDL_HITTEST_RESIZE_BOTTOMLEFT;
			if (bottom && right) return SDL_HITTEST_RESIZE_BOTTOMRIGHT;
			if (left) return SDL_HITTEST_RESIZE_LEFT;
			if (right) return SDL_HITTEST_RESIZE_RIGHT;
			if (top) return SDL_HITTEST_RESIZE_TOP;
			if (bottom) return SDL_HITTEST_RESIZE_BOTTOM;
		}

		auto* const n = w->hit(p);
		if (!n) return SDL_HITTEST_NORMAL;

		const auto h = n->_host.lock();
		const auto origin = n->window_origin();
		const pointi local = {p.x - origin.x, p.y - origin.y};

		// A control in the caption is pressed, not dragged by.
		if (auto* const controls = n->controls(); controls && controls->hit(local)) return SDL_HITTEST_NORMAL;
		if (h && h->is_caption_area(local)) return SDL_HITTEST_DRAGGABLE;
		return SDL_HITTEST_NORMAL;
	}

	void frame_node::destroy()
	{
		const auto keep = weak_from_this().lock();
		const auto h = _host.lock();
		if (_visible && _window && !is_root()) _window->invalidate(window_rect());
		_visible = false;
		stop_timer();
		if (h) h->on_window_destroy();

		// Its owner may still hold its controls; they are inert from here.
		_services.caret_timer(false, 0);
		_controls->detach();

		detach();
		_own_window.reset();
		set_window(nullptr);
	}

	// A window ticks five times a second, as the Windows layer's do - the idle count the application
	// reads to hide its chrome, and its host's tick; a pane only as often as its style asks.
	void start_frame_timer(const std::shared_ptr<frame_node>& n, const int pane_milliseconds)
	{
		const auto is_pane = n->_kind == frame_node::kind::pane;
		const auto interval = is_pane ? pane_milliseconds : 1000 / ui::default_ticks_per_second;
		if (interval <= 0) return;

		const std::weak_ptr<frame_node> weak = n;
		n->start_timer(interval, [weak, is_pane]
		{
			const auto self = weak.lock();
			if (!self) return;
			if (!is_pane) ui::ticks_since_last_user_action += 1;
			self->_controls->tick();
			const auto h = self->_host.lock();
			if (h) h->tick();
		});
	}

	SDL_Window* make_top_level(const sizei points, const bool borderless, const bool resizable)
	{
		SDL_WindowFlags flags = SDL_WINDOW_HIDDEN | SDL_WINDOW_HIGH_PIXEL_DENSITY;
		if (borderless) flags |= SDL_WINDOW_BORDERLESS;
		if (resizable) flags |= SDL_WINDOW_RESIZABLE;
		return SDL_CreateWindow("", std::max(1, points.cx), std::max(1, points.cy), flags);
	}

	ui::frame_ptr frame_node::create_dlg(ui::frame_host_weak_ptr host)
	{
		ui::frame_style style;
		style.child = false;
		auto result = std::make_shared<frame_node>(host, kind::dialog, style);

		auto* const sdl = make_top_level({400, 300}, true, false);
		if (!sdl) return nullptr;

		if (_window)
		{
			SDL_SetWindowParent(sdl, _window->sdl);
			SDL_SetWindowModal(sdl, true);
		}

		result->_own_window = std::make_unique<native_window>(sdl, result.get(), false);
		result->set_window(result->_own_window.get());
		SDL_SetWindowHitTest(sdl, window_hit_test, result->_own_window.get());

		start_frame_timer(result, 0);
		return result;
	}

	// A popup of a window: a menu takes the keyboard while it is open, and anything else - a list of
	// suggestions, a tooltip, a bubble - leaves it with the window that owns it. A layered popup is
	// transparent where its host draws nothing.
	std::shared_ptr<frame_node> make_popup_frame(native_window* parent, const ui::frame_host_weak_ptr& host,
	                                             const ui::frame_style& style)
	{
		if (!parent) return nullptr;

		SDL_WindowFlags flags = SDL_WINDOW_HIDDEN | SDL_WINDOW_HIGH_PIXEL_DENSITY |
			(style.no_focus || style.layered ? SDL_WINDOW_TOOLTIP : SDL_WINDOW_POPUP_MENU);
		if (style.layered) flags |= SDL_WINDOW_TRANSPARENT;
		auto* const sdl = SDL_CreatePopupWindow(parent->sdl, 0, 0, 1, 1, flags);
		if (!sdl) return nullptr;

		auto result = std::make_shared<frame_node>(host, frame_node::kind::pane, style);
		result->_own_window = std::make_unique<native_window>(sdl, result.get(), style.layered);
		result->set_window(result->_own_window.get());
		start_frame_timer(result, style.timer_milliseconds);
		return result;
	}

	ui::frame_ptr frame_node::create_frame(ui::frame_host_weak_ptr host, const ui::frame_style& style)
	{
		if (!style.child || style.layered) return make_popup_frame(_window, host, style);

		auto result = std::make_shared<frame_node>(host, kind::pane, style);
		result->attach_child(this);
		start_frame_timer(result, style.timer_milliseconds);
		return result;
	}

	ui::bubble_window_ptr frame_node::create_bubble()
	{
		return ui::create_drawn_bubble(std::shared_ptr<ui::bubble_host>(shared_from_this(), &_services));
	}
	ui::close_result frame_node::wait_for_close(const uint32_t timeout_ms)
	{
		const auto keep = weak_from_this().lock();
		_close_signalled = false;
		++the_app->_modal_depth;
		// As on Windows, a timeout of zero waits for as long as the dialog is open.
		const auto reached = the_app->pump([this] { return _close_signalled; },
		                                   timeout_ms == 0 ? UINT32_MAX : timeout_ms);
		--the_app->_modal_depth;

		// The result holds ok until close() overwrites it, so a wait that never saw the close must not
		// be reported as a decision.
		return reached ? _modal_result : ui::close_result::cancel;
	}

	void frame_node::position(const recti bounds)
	{
		set_bounds(bounds, true);
		if (is_root() && _window) SDL_RaiseWindow(_window->sdl);
	}

	void frame_node::save_window_position(platform::setting_file_ptr& store)
	{
		if (!is_root() || !_window || !store) return;

		int x = 0;
		int y = 0;
		int cx = 0;
		int cy = 0;
		SDL_GetWindowPosition(_window->sdl, &x, &y);
		SDL_GetWindowSize(_window->sdl, &cx, &cy);
		const auto maximized = (SDL_GetWindowFlags(_window->sdl) & SDL_WINDOW_MAXIMIZED) != 0;
		store->write({}, "window_placement", std::format("{} {} {} {} {}", x, y, cx, cy, maximized ? 1 : 0));
	}

	template <class T, class TBase>
	void node_window<T, TBase>::track_menu(const recti button_bounds, const std::vector<ui::command_ptr>& commands)
	{
		// The nearest frame that is not a pane draws the menu, as on Windows.
		for (node* n = this; n; n = n->_parent)
		{
			auto* const frame = dynamic_cast<frame_node*>(n);

			if (frame && (frame->_kind != frame_node::kind::pane || !n->_parent))
			{
				const auto keep = frame->keep_alive();
				ui::track_drawn_menu(frame->_services, button_bounds, frame->window_bounds(), commands);
				return;
			}
		}
	}

	host_services::~host_services()
	{
		if (caret_timer_id && the_app) the_app->remove_timer(caret_timer_id);
	}

	ui::measure_context* host_services::measure()
	{
		return owner && owner->_window ? owner->_window->ctx.get() : nullptr;
	}

	void host_services::invalidate(const recti bounds)
	{
		if (owner) owner->invalidate_rect(bounds);
	}

	bool host_services::has_keyboard_focus() const
	{
		return owner && owner->_window && owner->_window->keyboard_focus && owner->_window->focus == owner;
	}

	void host_services::take_keyboard_focus()
	{
		if (owner && owner->_window) owner->_window->set_focus(owner);
	}

	void host_services::keyboard_to_frame()
	{
		if (owner) owner->node::keyboard_changed(true);
	}

	void host_services::capture_pointer(const bool capture)
	{
		if (!owner || !owner->_window) return;
		auto* const w = owner->_window;

		if (capture)
		{
			w->capture = owner;
			SDL_CaptureMouse(true);
		}
		else if (w->capture == owner)
		{
			w->capture = nullptr;
			SDL_CaptureMouse(false);
		}
	}

	void host_services::set_cursor(const ui::style::cursor cursor)
	{
		apply_cursor(cursor);
	}

	pointi host_services::client_to_screen(const pointi loc) const
	{
		return owner ? loc + owner->window_bounds().top_left() : loc;
	}

	void host_services::track_menu(const recti screen_bounds, const std::vector<ui::command_ptr>& commands)
	{
		if (owner) owner->track_menu(screen_bounds, commands);
	}

	ui::frame_ptr host_services::create_popup(const ui::frame_host_weak_ptr& content, const ui::color_style& colors)
	{
		if (!owner) return nullptr;

		ui::frame_style style;
		style.child = false;
		style.no_focus = true;
		style.colors = colors;
		return make_popup_frame(owner->_window, content, style);
	}

	std::shared_ptr<ui::frame_host> host_services::listener() const
	{
		return owner ? owner->_host.lock() : nullptr;
	}

	void host_services::text_input(const bool active, const recti caret_bounds)
	{
		typing = active;
		caret = caret_bounds;
		if (the_app) the_app->update_text_input();
	}

	void host_services::caret_timer(const bool on, const uint32_t interval_ms)
	{
		if (!the_app) return;
		if (caret_timer_id) the_app->remove_timer(caret_timer_id);
		caret_timer_id = 0;
		if (!on || !owner) return;

		const std::weak_ptr<frame_node> weak = owner->weak_from_this();
		caret_timer_id = the_app->add_timer(static_cast<int>(interval_ms), [weak]
		{
			if (const auto self = weak.lock()) self->_controls->blink();
		});
	}

	bool host_services::is_dialog() const
	{
		return owner && owner->is_dialog();
	}

	void host_services::close(const bool cancel)
	{
		if (owner) owner->close(cancel);
	}

	ui::measure_context* host_services::menu_measure()
	{
		return measure();
	}

	ui::frame_ptr host_services::create_menu_popup(const ui::frame_host_weak_ptr& content, const ui::frame_ptr& parent)
	{
		auto* parent_window = owner ? owner->_window : nullptr;

		if (parent)
		{
			const auto handle = parent->handle();
			if (const auto* const n = std::any_cast<node*>(&handle); n && *n && (*n)->_window)
			{
				parent_window = (*n)->_window;
			}
		}

		// The pointer resting on an item opens its submenu, so the menu ticks while it is open.
		ui::frame_style style;
		style.child = false;
		style.timer_milliseconds = 50;
		style.colors = {
			ui::style::colors.menu_background, ui::style::colors.menu_text,
			ui::style::colors.dialog_selected_background
		};
		return make_popup_frame(parent_window, content, style);
	}

	recti host_services::menu_work_area(recti)
	{
		return ui::desktop_bounds(true);
	}

	void host_services::run_menu_loop(ui::menu_session& session)
	{
		if (the_app) the_app->run_menu_loop(session);
	}

	ui::frame_ptr host_services::create_bubble_popup(const ui::frame_host_weak_ptr& content)
	{
		if (!owner) return nullptr;

		ui::frame_style style;
		style.child = false;
		style.no_focus = true;
		style.layered = true;
		style.colors = {
			ui::style::colors.bubble_background, ui::style::colors.view_text,
			ui::style::colors.view_selected_background
		};
		return owner->create_frame(content, style);
	}

	// Measured at the size its owner draws at now: the bubble sees no scale change of its own.
	ui::measure_context* host_services::bubble_measure(const ui::frame_ptr& popup)
	{
		const auto handle = popup ? popup->handle() : std::any{};
		const auto* const n = std::any_cast<node*>(&handle);
		if (!n || !*n || !(*n)->_window) return nullptr;
		(*n)->_window->update_metrics();
		return (*n)->_window->ctx.get();
	}

	recti host_services::bubble_work_area(recti)
	{
		return ui::desktop_bounds(true);
	}

	//////////////////////////////////////////////////////////////////////////////////////////////
	// sdl_app
	//////////////////////////////////////////////////////////////////////////////////////////////

	void sdl_app::fire_timers()
	{
		const auto now = SDL_GetTicks();
		std::vector<uint64_t> due;

		for (const auto& t : _timers)
		{
			if (t.due <= now) due.push_back(t.id);
		}

		// By id, since a tick can add or remove timers, its own among them.
		for (const auto id : due)
		{
			const auto found = std::ranges::find_if(_timers, [id](const timer& t) { return t.id == id; });
			if (found == _timers.end()) continue;

			// A loop that fell behind skips the ticks it missed rather than running them back to back.
			found->due = std::max(found->due + found->period, now + 1);
			const auto tick = found->tick;
			if (tick) tick();
		}
	}

	void sdl_app::paint_windows()
	{
		std::vector<native_window*> dirty;

		for (const auto& [id, w] : _windows)
		{
			if (w->dirty) dirty.push_back(w);
		}

		for (auto* w : dirty)
		{
			// A paint can close a window, so each is looked up again before it is drawn.
			if (window(w->id) == w) w->paint();
		}
	}

	// Text input runs in the window with the keyboard while a control there takes typing, or the
	// application has a text field focused, so an input method shows its candidates for typing and not
	// for shortcuts - beside the caret, when a control has one.
	void sdl_app::update_text_input()
	{
		const auto app_typing = _app && _app->focus_mode() == ui::focus_mode::text_edit;

		for (const auto& [id, w] : _windows)
		{
			auto want = false;
			recti area;

			if (w->keyboard_focus)
			{
				auto* const controls = w->focus ? w->focus->controls() : nullptr;

				if (controls && controls->is_typing())
				{
					want = true;
					area = w->focus->caret_area();
				}
				else
				{
					want = app_typing;
				}
			}

			if (want && !area.is_empty() && area != w->text_area)
			{
				w->text_area = area;
				const SDL_Rect r{
					df::round(area.left / w->density), df::round(area.top / w->density),
					std::max(1, df::round(area.width() / w->density)), std::max(1, df::round(area.height() / w->density))
				};
				SDL_SetTextInputArea(w->sdl, &r, 0);
			}

			if (want == w->text_input) continue;
			w->text_input = want;
			if (want) SDL_StartTextInput(w->sdl);
			else SDL_StopTextInput(w->sdl);
		}
	}

	// A menu runs the loop until it closes, as Windows tracks a popup menu in a loop of its own.
	void sdl_app::run_menu_loop(ui::menu_session& session)
	{
		auto* const outer = _menu;
		_menu = &session;
		pump([&session] { return session.done(); }, UINT32_MAX);
		_menu = outer;
	}

	// While a menu is open it takes every key, and a press on any window that is not one of its popups
	// dismisses it and goes no further. True when the event was the menu's.
	bool sdl_app::menu_event(const SDL_Event& e)
	{
		if (!_menu) return false;

		const auto is_popup = [this](const SDL_WindowID id)
		{
			for (const auto& f : _menu->popups())
			{
				const auto handle = f->handle();
				const auto* const n = std::any_cast<node*>(&handle);
				if (n && *n && (*n)->_window && (*n)->_window->id == id) return true;
			}

			return false;
		};

		switch (e.type)
		{
		case SDL_EVENT_KEY_DOWN:
			if (const auto key = to_key(e.key.key)) _menu->key_down(static_cast<int>(key), key_state_of(e.key.mod));
			return true;

		case SDL_EVENT_KEY_UP:
		case SDL_EVENT_TEXT_INPUT:
		case SDL_EVENT_TEXT_EDITING:
			return true;

		case SDL_EVENT_MOUSE_BUTTON_DOWN:
			if (is_popup(e.button.windowID)) return false;
			_menu->cancel();
			return true;

		case SDL_EVENT_WINDOW_FOCUS_LOST:
			// The keyboard moving to one of the menu's own popups keeps it open; leaving the application closes it.
			if (!SDL_GetKeyboardFocus()) _menu->cancel();
			return true;

		case SDL_EVENT_WINDOW_FOCUS_GAINED:
			return is_popup(e.window.windowID);

		case SDL_EVENT_WINDOW_HIDDEN:
			// A compositor dismisses a menu popup on a press outside it before the press is seen.
			if (is_popup(e.window.windowID)) _menu->cancel();
			return false;

		default:
			return false;
		}
	}

	bool sdl_app::pump(const std::function<bool()>& done, const uint32_t timeout_ms)
	{
		const auto deadline = timeout_ms == UINT32_MAX ? UINT64_MAX : SDL_GetTicks() + timeout_ms;

		while (!done())
		{
			if (_quit || df::is_closing) return false;

			paint_windows();
			update_text_input();

			const auto now = SDL_GetTicks();
			if (now >= deadline) return false;

			auto wait = deadline == UINT64_MAX ? UINT64_MAX : deadline - now;

			for (const auto& t : _timers)
			{
				wait = std::min(wait, t.due > now ? t.due - now : 0);
			}

			SDL_Event e;
			const auto wait_ms = wait == UINT64_MAX ? -1 : static_cast<Sint32>(std::min<uint64_t>(wait, INT32_MAX));

			if (SDL_WaitEventTimeout(&e, wait_ms))
			{
				dispatch(e);
				while (SDL_PollEvent(&e)) dispatch(e);
			}

			fire_timers();

			// After every wake, as the Windows loop does: the application drains the work its threads
			// queued for the UI thread.
			_idle_pending = false;
			if (_app) _app->idle();
		}

		return true;
	}

	void sdl_app::run()
	{
		if (!_frame || !_frame->_window) return;

		auto* const w = _frame->_window;
		_frame->_visible = true;
		SDL_ShowWindow(w->sdl);
		SDL_SyncWindow(w->sdl);
		w->sync_size();
		_frame->layout_now(false);
		w->invalidate({});

		frame_delay(1000 / ui::default_ticks_per_second);
		pump([this] { return _quit; }, UINT32_MAX);
	}

	void sdl_app::request_app_close()
	{
		if (!_app || _quit) return;

		if (_app->can_exit())
		{
			df::is_closing = true;
			platform::event_exit.set();

			if (_frame)
			{
				const auto h = _frame->_host.lock();
				if (h) h->on_window_destroy();
				if (_frame->_window) SDL_HideWindow(_frame->_window->sdl);
			}

			_quit = true;
		}
	}

	void sdl_app::mouse_motion(native_window* w, const pointi pt)
	{
		ui::ticks_since_last_user_action = 0;

		auto* const target = w->capture ? w->capture : w->hit(pt);

		if (!w->capture && target != w->hover)
		{
			if (auto* const old = w->hover)
			{
				const auto keep = old->keep_alive();
				const auto h = old->_host.lock();
				const auto origin = old->window_origin();
				if (auto* const controls = old->controls()) controls->mouse_leave();
				if (h && !old->_over_controls) h->on_mouse_leave({pt.x - origin.x, pt.y - origin.y});
				old->_over_controls = false;
				old->invalidate_rect({});
			}

			w->hover = target;
			if (target) apply_cursor(target->_cursor);
		}

		if (!target || !target->enabled()) return;

		const auto keep = target->keep_alive();
		const auto h = target->_host.lock();
		const auto origin = target->window_origin();
		const pointi local = {pt.x - origin.x, pt.y - origin.y};

		// Over one of its controls the host has the pointer no longer, as with a native child window.
		if (auto* const controls = target->controls())
		{
			if (controls->mouse_move(local))
			{
				if (!target->_over_controls)
				{
					target->_over_controls = true;
					if (h) h->on_mouse_leave(local);
				}

				return;
			}

			if (target->_over_controls)
			{
				target->_over_controls = false;
				apply_cursor(target->_cursor);
			}
		}

		if (h) h->on_mouse_move(local, w->capture == target);
	}

	void sdl_app::mouse_button(native_window* w, const SDL_MouseButtonEvent& e, const bool down)
	{
		const auto pt = w->to_pixels(e.x, e.y);
		const auto ks = key_state_of(SDL_GetModState());
		ui::ticks_since_last_user_action = 0;

		auto* const target = (!down && w->capture) ? w->capture : w->hit(pt);
		if (!target || !target->enabled()) return;

		const auto keep = target->keep_alive();
		const auto h = target->_host.lock();
		const auto origin = target->window_origin();
		const pointi local = {pt.x - origin.x, pt.y - origin.y};
		auto* const controls = target->controls();

		if (e.button == SDL_BUTTON_LEFT)
		{
			if (down)
			{
				w->capture = target;
				SDL_CaptureMouse(true);

				// A control takes the press, and the focus when it takes focus at all, before its host.
				if (controls && (e.clicks == 2 ? controls->mouse_double_click(local, ks) : controls->mouse_down(local, ks)))
				{
					return;
				}

				// A press on a frame's own ground gives it the keyboard where it takes the keyboard, for itself
				// rather than a control of its own that held it, unless its host keeps that ground for controls,
				// as on Windows; elsewhere the focus stays where it is.
				if (target->_can_focus && !target->_no_focus && (!h || h->takes_keyboard_at(local)))
				{
					if (controls) controls->clear_focus();
					w->set_focus(target);
				}

				// Windows delivers the second press of a double click as the double click alone.
				if (h)
				{
					if (e.clicks == 2) h->on_mouse_left_button_double_click(local, ks);
					else h->on_mouse_left_button_down(local, ks);
				}
			}
			else
			{
				w->capture = nullptr;
				SDL_CaptureMouse(false);
				if (controls && controls->mouse_up(local, ks)) return;
				target->invalidate_rect({});
				if (h) h->on_mouse_left_button_up(local, ks);
			}
		}
		else if (e.button == SDL_BUTTON_MIDDLE)
		{
			if (h)
			{
				if (down) h->on_mouse_middle_button_down(local, ks);
				else h->on_mouse_middle_button_up(local, ks);
			}
		}
		else if (e.button == SDL_BUTTON_RIGHT && !down)
		{
			// A control's own menu first; any other goes to the window, as Windows sends WM_CONTEXTMENU on
			// to the top-level window, in screen pixels.
			if (controls && controls->context_menu(local)) return;

			auto* const root = w->root;
			const auto root_host = root ? root->_host.lock() : nullptr;

			if (root_host && _app)
			{
				const auto screen = pt + w->screen_origin();
				const auto commands = root_host->menu(screen);

				if (!commands.empty())
				{
					const auto frame = std::dynamic_pointer_cast<ui::frame>(root->keep_alive());
					if (frame) _app->track_menu(frame, recti(screen, sizei{}), commands);
				}
			}
		}
		else if ((e.button == SDL_BUTTON_X1 || e.button == SDL_BUTTON_X2) && !down)
		{
			if (h)
			{
				h->on_mouse_other_button_up(e.button == SDL_BUTTON_X1 ? ui::other_mouse_button::xb1 : ui::other_mouse_button::xb2,
				                            local, ks);
			}
		}
	}

	void sdl_app::mouse_wheel(native_window* w, const SDL_MouseWheelEvent& e)
	{
		const auto pt = w->to_pixels(e.mouse_x, e.mouse_y);
		const auto ks = key_state_of(SDL_GetModState());
		const auto flip = e.direction == SDL_MOUSEWHEEL_FLIPPED ? -1.0f : 1.0f;

		// In the Windows layer's units, 120 to a detent, so the application accumulates one way.
		const auto dy = static_cast<int>(std::lround(e.y * flip * 120.0f));
		const auto dx = static_cast<int>(std::lround(e.x * flip * 120.0f));

		// Offered to the control or frame under the pointer and then to each frame around it, as Windows
		// passes an unhandled wheel to the parent window.
		for (auto* n = w->hit(pt); n; n = n->_parent)
		{
			if (!n->enabled()) continue;

			const auto keep = n->keep_alive();
			const auto origin = n->window_origin();
			const pointi local = {pt.x - origin.x, pt.y - origin.y};

			if (auto* const controls = n->controls(); controls && dy != 0 && controls->wheel(local, dy, ks)) break;

			const auto h = n->_host.lock();
			if (!h) continue;

			auto handled = false;

			if (dy != 0) h->on_mouse_wheel(local, dy, ks, handled);
			if (dx != 0) h->on_mouse_hwheel(local, dx, ks, handled);
			if (handled) break;
		}
	}

	void sdl_app::key_down(native_window* w, const SDL_KeyboardEvent& e)
	{
		const auto key = to_key(e.key);
		if (key == 0) return;

		const auto ks = key_state_of(e.mod);
		if (key != 0x25 && key != 0x27) ui::ticks_since_last_user_action = 0;

		auto* const focus = w ? w->focus : nullptr;
		const auto focus_keep = focus ? focus->keep_alive() : nullptr;
		auto* const controls = focus && focus->enabled() ? focus->controls() : nullptr;

		// The application's shortcuts first, as the Windows loop offers them, except inside a dialog's
		// own loop, where Windows hands the keys to the dialog.
		const auto offer_app = _modal_depth == 0 && _app && (!w || !w->root || w->root == _frame.get());
		auto handled = false;

		if (controls && controls->is_typing() && !ks.alt)
		{
			// A control taking typing keeps its keys from the application's shortcuts. The application
			// is offered only the keys that leave or confirm it, as the Windows loop treated an edit.
			const auto c = static_cast<char32_t>(key);
			const auto leaves = c == keys::ESCAPE || c == keys::UP || c == keys::DOWN || c == keys::TAB ||
				(c == keys::RETURN && !(controls->wants_return() && !ks.control));
			if (leaves && offer_app) handled = _app->key_down(key, ks);
			if (!handled) controls->key_down(static_cast<int>(key), ks);
			return;
		}

		if (offer_app) handled = _app->key_down(key, ks);
		if (!handled && controls) handled = controls->key_down(static_cast<int>(key), ks);

		if (!handled && focus && focus->enabled())
		{
			const auto h = focus->_host.lock();
			if (h) handled = h->key_down(static_cast<int>(key), ks);
		}

		// Escape cancels a dialog, as a dialog box's own handling of it does on Windows.
		if (!handled && key == 0x1B && w && w->root && w->root != _frame.get())
		{
			if (auto* const dialog = dynamic_cast<frame_node*>(w->root); dialog && dialog->is_dialog())
			{
				dialog->close(true);
			}
		}
	}

	void sdl_app::dispatch(const SDL_Event& e)
	{
		if (e.type == _event_idle)
		{
			_idle_pending = false;
			return;
		}

		if (menu_event(e)) return;

		if (e.type == _event_folder)
		{
			for (const auto& folder : _watcher.take())
			{
				if (_app) _app->folder_changed(folder);
			}

			return;
		}

		switch (e.type)
		{
		case SDL_EVENT_QUIT:
			request_app_close();
			break;

		case SDL_EVENT_WINDOW_CLOSE_REQUESTED:
			if (auto* const w = window(e.window.windowID); w && w->root)
			{
				if (w->root == _frame.get()) request_app_close();
				else if (auto* const dialog = dynamic_cast<frame_node*>(w->root); dialog && dialog->is_dialog())
					dialog->close(true);
			}
			break;

		case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED:
		case SDL_EVENT_WINDOW_RESIZED:
			if (auto* const w = window(e.window.windowID); w && w->root)
			{
				const auto before = w->extent;
				w->sync_size();

				if (w->extent != before)
				{
					w->root->layout_now(w->minimized);
					w->invalidate({});
				}
			}
			break;

		case SDL_EVENT_WINDOW_EXPOSED:
			if (auto* const w = window(e.window.windowID)) w->invalidate({});
			break;

		case SDL_EVENT_WINDOW_MINIMIZED:
			if (auto* const w = window(e.window.windowID); w && w->root)
			{
				w->minimized = true;
				w->root->layout_now(true);
			}
			break;

		case SDL_EVENT_WINDOW_MAXIMIZED:
		case SDL_EVENT_WINDOW_RESTORED:
			if (auto* const w = window(e.window.windowID); w && w->root)
			{
				w->minimized = false;
				w->sync_size();
				w->root->layout_now(false);
				w->invalidate({});
			}
			break;

		case SDL_EVENT_WINDOW_FOCUS_GAINED:
		case SDL_EVENT_WINDOW_FOCUS_LOST:
			if (auto* const w = window(e.window.windowID); w && w->root)
			{
				const auto gained = e.type == SDL_EVENT_WINDOW_FOCUS_GAINED;
				if (w->keyboard_focus == gained) break;

				w->keyboard_focus = gained;
				if (!w->focus) w->focus = w->root;

				const auto keep = w->root->keep_alive();
				const auto root_host = w->root->_host.lock();
				if (root_host) root_host->activate(gained);

				const auto focus_keep = w->focus->keep_alive();
				w->focus->keyboard_changed(gained);

				update_text_input();
				w->invalidate({});
			}
			break;

		case SDL_EVENT_WINDOW_DISPLAY_SCALE_CHANGED:
			if (auto* const w = window(e.window.windowID); w && w->root)
			{
				w->update_metrics();
				w->sync_size();
				const auto keep = w->root->keep_alive();
				const auto h = w->root->_host.lock();
				if (h) h->dpi_changed();
				w->root->layout_now(w->minimized);
				w->invalidate({});
			}
			break;

		case SDL_EVENT_WINDOW_MOUSE_LEAVE:
			if (auto* const w = window(e.window.windowID); w && !w->capture)
			{
				if (auto* const old = w->hover)
				{
					w->hover = nullptr;
					const auto keep = old->keep_alive();
					const auto h = old->_host.lock();
					if (h) h->on_mouse_leave({-1, -1});
					old->invalidate_rect({});
				}
			}
			break;

		case SDL_EVENT_MOUSE_MOTION:
			if (auto* const w = window(e.motion.windowID)) mouse_motion(w, w->to_pixels(e.motion.x, e.motion.y));
			break;

		case SDL_EVENT_MOUSE_BUTTON_DOWN:
		case SDL_EVENT_MOUSE_BUTTON_UP:
			if (auto* const w = window(e.button.windowID)) mouse_button(w, e.button, e.type == SDL_EVENT_MOUSE_BUTTON_DOWN);
			break;

		case SDL_EVENT_MOUSE_WHEEL:
			if (auto* const w = window(e.wheel.windowID)) mouse_wheel(w, e.wheel);
			break;

		case SDL_EVENT_KEY_DOWN:
			key_down(window(e.key.windowID), e.key);
			break;

		case SDL_EVENT_TEXT_INPUT:
			if (e.text.text)
			{
				auto* const w = window(e.text.windowID);
				auto* const focus = w ? w->focus : nullptr;
				auto* const controls = focus ? focus->controls() : nullptr;

				if (controls && controls->is_typing())
				{
					const auto keep = focus->keep_alive();
					controls->text_input(e.text.text);
				}
				else if (_app && _app->focus_mode() == ui::focus_mode::text_edit)
				{
					_app->text_input(e.text.text);
				}
			}
			break;

		default:
			break;
		}
	}

	void sdl_app::sys_command(const ui::sys_command_type cmd)
	{
		if (!_frame || !_frame->_window) return;
		auto* const sdl = _frame->_window->sdl;

		switch (cmd)
		{
		case ui::sys_command_type::MINIMIZE: SDL_MinimizeWindow(sdl);
			break;
		case ui::sys_command_type::MAXIMIZE: SDL_MaximizeWindow(sdl);
			break;
		case ui::sys_command_type::RESTORE: SDL_RestoreWindow(sdl);
			break;
		}
	}

	void sdl_app::full_screen(const bool full)
	{
		if (_frame && _frame->_window) SDL_SetWindowFullscreen(_frame->_window->sdl, full);
	}

	// The frame timer: the application prepares each frame on it.
	void sdl_app::frame_delay(const int delay)
	{
		if (delay == _frame_delay && _frame_timer) return;
		_frame_delay = delay;
		if (_frame_timer) remove_timer(_frame_timer);
		_frame_timer = add_timer(delay, [this] { if (_app) _app->prepare_frame(); });
	}

	void sdl_app::set_font_base_size(const int size)
	{
		if (global_base_font_size == size) return;
		global_base_font_size = size;

		std::vector<native_window*> windows;
		for (const auto& [id, w] : _windows) windows.push_back(w);

		for (auto* w : windows)
		{
			if (window(w->id) != w) continue;
			w->update_metrics();
			if (w->root) w->root->layout_now(w->minimized);
			w->invalidate({});
		}
	}

	void sdl_app::show_progress(const int64_t done, const int64_t total)
	{
		if (!_frame || !_frame->_window) return;
		auto* const sdl = _frame->_window->sdl;

		if (total > 0)
		{
			SDL_SetWindowProgressState(sdl, SDL_PROGRESS_STATE_NORMAL);
			SDL_SetWindowProgressValue(sdl, static_cast<float>(std::clamp<int64_t>(done, 0, total)) /
			                           static_cast<float>(total));
		}
		else
		{
			SDL_SetWindowProgressState(sdl, SDL_PROGRESS_STATE_INDETERMINATE);
		}
	}

	void sdl_app::end_progress()
	{
		if (_frame && _frame->_window) SDL_SetWindowProgressState(_frame->_window->sdl, SDL_PROGRESS_STATE_NONE);
	}

	ui::frame_ptr sdl_app::create_app_frame(const platform::setting_file_ptr& store, const ui::frame_host_weak_ptr& host,
	                                        const ui::frame_style& app_style)
	{
		// Where the window was, in the system's own units; Wayland keeps the size alone.
		int x = SDL_WINDOWPOS_UNDEFINED;
		int y = SDL_WINDOWPOS_UNDEFINED;
		int cx = 0;
		int cy = 0;
		auto maximized = 0;

		std::string placement;
		if (store && store->read({}, "window_placement", placement))
		{
			if (std::sscanf(placement.c_str(), "%d %d %d %d %d", &x, &y, &cx, &cy, &maximized) != 5)
			{
				x = y = SDL_WINDOWPOS_UNDEFINED;
				cx = cy = 0;
				maximized = 0;
			}
		}

		SDL_Rect usable{0, 0, 1280, 800};
		SDL_GetDisplayUsableBounds(SDL_GetPrimaryDisplay(), &usable);

		if (cx < 320 || cy < 200 || cx > usable.w || cy > usable.h)
		{
			cx = usable.w * 3 / 4;
			cy = usable.h * 3 / 4;
			x = y = SDL_WINDOWPOS_CENTERED;
		}

		auto* const sdl = make_top_level({cx, cy}, true, true);
		if (!sdl) return nullptr;
		if (x != SDL_WINDOWPOS_UNDEFINED) SDL_SetWindowPosition(sdl, x, y);
		SDL_SetWindowMinimumSize(sdl, 320, 200);

		auto style = app_style;
		style.child = false;

		auto result = std::make_shared<frame_node>(host, frame_node::kind::app, style);
		result->_own_window = std::make_unique<native_window>(sdl, result.get(), false);
		result->set_window(result->_own_window.get());
		result->_own_window->focus = result.get();
		SDL_SetWindowHitTest(sdl, window_hit_test, result->_own_window.get());
		if (maximized) SDL_MaximizeWindow(sdl);

		start_frame_timer(result, 0);
		_frame = result;
		return result;
	}

	std::string command_line_of(const int argc, char* argv[])
	{
		std::string result;

		for (auto i = 1; i < argc; ++i)
		{
			const std::string_view arg(argv[i]);
			if (!result.empty()) result += ' ';

			if (arg.find_first_of(" \t") != std::string_view::npos)
			{
				result += '"';
				result += arg;
				result += '"';
			}
			else
			{
				result += arg;
			}
		}

		return result;
	}
}

///////////////////////////////////////////////////////////////////////////////////////////////////
// The UI services the window layer answers.
///////////////////////////////////////////////////////////////////////////////////////////////////

recti ui::desktop_bounds(const bool work_area)
{
	SDL_DisplayID display = 0;
	float density = 1.0f;

	if (the_app && the_app->_frame && the_app->_frame->_window)
	{
		display = SDL_GetDisplayForWindow(the_app->_frame->_window->sdl);
		density = the_app->_frame->_window->density;
	}

	if (!display) display = SDL_GetPrimaryDisplay();

	SDL_Rect r{0, 0, 1920, 1080};
	if (!display || !(work_area ? SDL_GetDisplayUsableBounds(display, &r) : SDL_GetDisplayBounds(display, &r)))
	{
		r = {0, 0, 1920, 1080};
	}

	return {
		df::round(r.x * density), df::round(r.y * density), df::round((r.x + r.w) * density),
		df::round((r.y + r.h) * density)
	};
}

ui::focus_snapshot ui::save_focus()
{
	focus_snapshot result;
	if (!the_app) return result;

	for (const auto& [id, w] : the_app->_windows)
	{
		if (w->keyboard_focus && w->focus)
		{
			// The control that holds the focus, so a dialog closing gives it back to that control.
			if (auto* const controls = w->focus->controls())
			{
				if (controls->has_keyboard_focus()) result.focused = controls->focused();
			}

			result.window = w->focus->keep_alive();
			return result;
		}
	}

	return result;
}

void ui::restore_focus(const focus_snapshot& snapshot)
{
	if (const auto c = snapshot.focused.lock())
	{
		c->focus();
	}
	else if (const auto w = snapshot.window.lock())
	{
		w->focus();
	}
}

ui::key_state ui::current_key_state()
{
	return key_state_of(SDL_GetModState());
}

// What the window shows now, read back from its surface: every tile the renderer presents lands there.
ui::surface_ptr platform::capture_window_surface(const std::any& window_handle)
{
	const auto* const n = std::any_cast<node*>(&window_handle);
	if (!n || !*n || !(*n)->_window) return nullptr;

	auto* const w = (*n)->_window;
	w->paint();

	auto* const s = SDL_GetWindowSurface(w->sdl);
	if (!s || s->w < 1 || s->h < 1) return nullptr;

	auto result = std::make_shared<ui::surface>();
	if (!result->alloc(s->w, s->h, ui::texture_format::ARGB, ui::orientation::top_left)) return nullptr;

	if (SDL_MUSTLOCK(s)) SDL_LockSurface(s);

	for (auto y = 0; y < s->h; ++y)
	{
		const auto* const src = static_cast<const uint8_t*>(s->pixels) + static_cast<ptrdiff_t>(y) * s->pitch;
		auto* const dst = result->pixels_line(y);
		SDL_ConvertPixels(s->w, 1, s->format, src, s->pitch, SDL_PIXELFORMAT_ARGB8888, dst,
		                  static_cast<int>(result->stride()));
		auto* const pixels = reinterpret_cast<ui::color32*>(dst);
		for (auto x = 0; x < s->w; ++x) pixels[x] |= 0xff000000u;
	}

	if (SDL_MUSTLOCK(s)) SDL_UnlockSurface(s);
	return result;
}

///////////////////////////////////////////////////////////////////////////////////////////////////
// The entry point: the application's lifetime as wWinMain runs it on Windows.
///////////////////////////////////////////////////////////////////////////////////////////////////

int libdesktop_linux_main(const int argc, char* argv[])
{
	platform::set_ui_thread();

	// Filenames convert through the character locale, which must be UTF-8 whatever the shell set.
	if (!std::setlocale(LC_CTYPE, "C.UTF-8"))
	{
		std::fprintf(stderr, "Cannot initialize the UTF-8 character locale required for filenames.\n");
		return 1;
	}

	auto app_impl = std::make_shared<sdl_app>();
	the_app = app_impl.get();
	ui::app_ptr app;

	const df::scope_exit final_exit([&app]
	{
		if (app) app->final_exit();
	});

	try
	{
		app = app_impl->_app = create_app(app_impl);
		if (!app->pre_init()) return 0;

		// A request the application's own locale could not satisfy left the UTF-8 one in place; one it
		// could replaces it with another UTF-8 locale.
		const auto command_line = command_line_of(argc, argv);

		// Headless modes - the test suite among them - run without a display.
		if (app->parse_command_line(command_line))
		{
			df::start_time = platform::now();
			return app->run_console_mode();
		}

		if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS))
		{
			std::fprintf(stderr, "Cannot open a display: %s\n", SDL_GetError());
			return 1;
		}

		const df::scope_exit quit_sdl([] { SDL_Quit(); });

		// The clipboard is the display's, so it is reached through SDL, on this thread.
		platform::install_text_clipboard([]() -> std::optional<std::string>
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
		const df::scope_exit uninstall_clipboard([] { platform::install_text_clipboard({}, {}); });

		// Every window draws on the CPU here, where fades are not drawn.
		ui::animations_enabled = false;

		app_impl->init_events();
		df::start_time = platform::now();
		app->load_settings(platform::settings());

		if (app->init(command_line))
		{
			app_impl->run();
		}

		app->exit();
		app_impl->shutdown();
		app_impl->_frame.reset();
	}
	catch (const std::exception& e)
	{
		df::log(__FUNCTION__, e.what());
		std::fprintf(stderr, "%s\n", e.what());
		return 1;
	}

	return 0;
}
