// This file is part of libdesktop, a C++20 platform layer for desktop applications
// Copyright 2026  Zac Walker
//
// This program is free software; you can redistribute it and / or modify it
// under the terms of the LGPL License either version 2.1 or later.
// License details are available at https://www.gnu.org/licenses/lgpl-2.1.html
// This program is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY

// Purpose: Native Windows windows. The class below reports the window's native events without
// knowing what is drawn in it; the portable owner receives all painting and input decisions.

#include "libdesktop/platform.h"
#include "libdesktop/platform/win32.h"

#include <Dbt.h>
#include <dwmapi.h>
#include <imm.h>
#include <shobjidl.h>
#include <shellapi.h>
#include <wtsapi32.h>
#include <windowsx.h>

#include <cmath>

#ifndef GET_X_LPARAM
#define GET_X_LPARAM(lp) static_cast<int>(static_cast<short>(LOWORD(lp)))
#endif
#ifndef GET_Y_LPARAM
#define GET_Y_LPARAM(lp) static_cast<int>(static_cast<short>(HIWORD(lp)))
#endif

extern std::weak_ptr<platform::app> g_platform_app;
extern std::atomic_bool number_format_invalid;
extern void dispatch_frame_tick_if_due();

extern const unsigned char libdesktop_cursor_hand[];
extern const std::size_t libdesktop_cursor_hand_size;
extern const unsigned char libdesktop_cursor_hand_down[];
extern const std::size_t libdesktop_cursor_hand_down_size;
extern const unsigned char libdesktop_cursor_hand_up[];
extern const std::size_t libdesktop_cursor_hand_up_size;
extern const unsigned char libdesktop_cursor_move[];
extern const std::size_t libdesktop_cursor_move_size;
extern const unsigned char libdesktop_cursor_select[];
extern const std::size_t libdesktop_cursor_select_size;
extern const unsigned char libdesktop_cursor_zoom[];
extern const std::size_t libdesktop_cursor_zoom_size;

namespace
{
	// The executable's icon is the shell's conventional resource ID. Unlike the desktop UI assets,
	// this is deliberately supplied by the executable rather than by libdesktop-platform.
	constexpr int icon_id = 1;

	constexpr UINT wm_ncuah_draw_caption = 0xae;
	constexpr UINT wm_ncuah_draw_frame = 0xaf;
	constexpr UINT wm_pointer_wheel = 0x024e;
	constexpr UINT wm_pointer_hwheel = 0x024f;

	constexpr int resize_side = 9;
	constexpr int resize_bottom = 16;
	constexpr int resize_corner = 24;

	uint16_t read_u16(const unsigned char* const data)
	{
		return static_cast<uint16_t>(data[0]) | static_cast<uint16_t>(data[1]) << 8;
	}

	uint32_t read_u32(const unsigned char* const data)
	{
		return static_cast<uint32_t>(data[0]) | static_cast<uint32_t>(data[1]) << 8 |
			static_cast<uint32_t>(data[2]) << 16 | static_cast<uint32_t>(data[3]) << 24;
	}

	HCURSOR create_cursor(const unsigned char* const file, const std::size_t file_size)
	{
		// A .cur file has an ICONDIR followed by one or more image entries. CreateIconFromResourceEx
		// expects the image resource instead: a LOCALHEADER containing the hotspot, then the image.
		if (!file || file_size < 22 || read_u16(file) != 0 || read_u16(file + 2) != 2 || read_u16(file + 4) == 0)
			return nullptr;

		const auto image_size = static_cast<std::size_t>(read_u32(file + 14));
		const auto image_offset = static_cast<std::size_t>(read_u32(file + 18));
		if (image_size == 0 || image_offset < 22 || image_offset > file_size || image_size > file_size - image_offset)
			return nullptr;

		std::vector<unsigned char> resource(4 + image_size);
		resource[0] = file[10];
		resource[1] = file[11];
		resource[2] = file[12];
		resource[3] = file[13];
		std::memcpy(resource.data() + 4, file + image_offset, image_size);

		// Zero desired dimensions plus LR_DEFAULTSIZE is the same system-metric scaling LoadCursor
		// applies, while the LOCALHEADER above preserves the .cur file's hotspot.
		return static_cast<HCURSOR>(CreateIconFromResourceEx(resource.data(), static_cast<DWORD>(resource.size()),
			FALSE, 0x00030000, 0, 0, LR_DEFAULTSIZE));
	}

	struct resources_t
	{
		HCURSOR link = nullptr;
		HCURSOR normal = nullptr;
		HCURSOR zoom = nullptr;
		HCURSOR move = nullptr;
		HCURSOR size_all = nullptr;
		HCURSOR left_right = nullptr;
		HCURSOR up_down = nullptr;
		HCURSOR hand_up = nullptr;
		HCURSOR hand_down = nullptr;
		HCURSOR select = nullptr;
		HCURSOR text_select = nullptr;
		HICON app_icon_32 = nullptr;
		HICON app_icon_64 = nullptr;
		bool initialized = false;

		void init()
		{
			if (initialized) return;
			initialized = true;
			const auto instance = get_resource_instance ? get_resource_instance : GetModuleHandleW(nullptr);
			link = create_cursor(libdesktop_cursor_hand, libdesktop_cursor_hand_size);
			normal = LoadCursorW(nullptr, IDC_ARROW);
			zoom = create_cursor(libdesktop_cursor_zoom, libdesktop_cursor_zoom_size);
			move = create_cursor(libdesktop_cursor_move, libdesktop_cursor_move_size);
			size_all = LoadCursorW(nullptr, IDC_SIZEALL);
			left_right = LoadCursorW(nullptr, IDC_SIZEWE);
			up_down = LoadCursorW(nullptr, IDC_SIZENS);
			hand_up = create_cursor(libdesktop_cursor_hand_up, libdesktop_cursor_hand_up_size);
			hand_down = create_cursor(libdesktop_cursor_hand_down, libdesktop_cursor_hand_down_size);
			select = create_cursor(libdesktop_cursor_select, libdesktop_cursor_select_size);
			text_select = LoadCursorW(nullptr, IDC_IBEAM);
			app_icon_32 = static_cast<HICON>(LoadImageW(instance, MAKEINTRESOURCEW(icon_id), IMAGE_ICON, 32, 32, 0));
			app_icon_64 = static_cast<HICON>(LoadImageW(instance, MAKEINTRESOURCEW(icon_id), IMAGE_ICON, 64, 64, 0));
		}
	};

	resources_t resources;

	HCURSOR cursor_for(const platform::cursor cursor)
	{
		resources.init();
		switch (cursor)
		{
		case platform::cursor::none: return nullptr;
		case platform::cursor::normal: return resources.normal;
		case platform::cursor::link: return resources.link;
		case platform::cursor::zoom: return resources.zoom;
		case platform::cursor::select: return resources.select;
		case platform::cursor::text_select: return resources.text_select;
		case platform::cursor::move: return resources.move;
		case platform::cursor::size_all: return resources.size_all;
		case platform::cursor::left_right: return resources.left_right;
		case platform::cursor::up_down: return resources.up_down;
		case platform::cursor::hand_up: return resources.hand_up;
		case platform::cursor::hand_down: return resources.hand_down;
		default: return resources.normal;
		}
	}

	void track_mouse_leave(const HWND hwnd)
	{
		TRACKMOUSEEVENT tme{sizeof(tme), TME_LEAVE, hwnd, HOVER_DEFAULT};
		TrackMouseEvent(&tme);
	}

	void track_nonclient_leave(const HWND hwnd)
	{
		TRACKMOUSEEVENT tme{sizeof(tme), TME_LEAVE | TME_NONCLIENT, hwnd, HOVER_DEFAULT};
		TrackMouseEvent(&tme);
	}

	core::recti rect_from(const RECT& r)
	{
		return {r.left, r.top, r.right, r.bottom};
	}

	RECT rect_to(const core::recti& r)
	{
		return {r.left, r.top, r.right, r.bottom};
	}

	core::recti monitor_rect(const core::recti bounds, const bool work)
	{
		const auto r = rect_to(bounds);
		const auto monitor = MonitorFromRect(&r, MONITOR_DEFAULTTONEAREST);
		MONITORINFO info{sizeof(info)};
		if (monitor && GetMonitorInfoW(monitor, &info)) return rect_from(work ? info.rcWork : info.rcMonitor);

		RECT fallback{};
		if (work && SystemParametersInfoW(SPI_GETWORKAREA, 0, &fallback, 0)) return rect_from(fallback);
		fallback = {0, 0, GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN)};
		return rect_from(fallback);
	}

	platform::key_state pointer_keys(const UINT message, const WPARAM parameter)
	{
		if (message == wm_pointer_wheel || message == wm_pointer_hwheel) return platform::current_key_state();
		platform::key_state result;
		result.control = (parameter & MK_CONTROL) != 0 || GetAsyncKeyState(VK_CONTROL) < 0;
		result.shift = (parameter & MK_SHIFT) != 0 || GetAsyncKeyState(VK_SHIFT) < 0;
		result.alt = GetAsyncKeyState(VK_MENU) < 0;
		return result;
	}

	platform::key_state drop_keys(const DWORD state)
	{
		return {
			(state & MK_CONTROL) != 0 || GetAsyncKeyState(VK_CONTROL) < 0,
			(state & MK_SHIFT) != 0 || GetAsyncKeyState(VK_SHIFT) < 0,
			(state & MK_ALT) != 0 || GetAsyncKeyState(VK_MENU) < 0
		};
	}

	UINT window_dpi(const HWND hwnd)
	{
		using get_dpi_for_window = UINT(WINAPI*)(HWND);
		static const auto proc = reinterpret_cast<get_dpi_for_window>(
			GetProcAddress(GetModuleHandleW(L"user32.dll"), "GetDpiForWindow"));
		if (proc && hwnd) return std::max<UINT>(96, proc(hwnd));
		const auto dc = GetDC(hwnd);
		const auto dpi = dc ? static_cast<UINT>(GetDeviceCaps(dc, LOGPIXELSX)) : 96u;
		if (dc) ReleaseDC(hwnd, dc);
		return std::max<UINT>(96, dpi);
	}

	core::sizei window_size_for_client(const core::sizei client, const DWORD style, const DWORD ex_style, const UINT dpi)
	{
		RECT rect{0, 0, std::max<LONG>(0, client.cx), std::max<LONG>(0, client.cy)};
		using adjust_for_dpi = BOOL(WINAPI*)(LPRECT, DWORD, BOOL, DWORD, UINT);
		static const auto adjust = reinterpret_cast<adjust_for_dpi>(
			GetProcAddress(GetModuleHandleW(L"user32.dll"), "AdjustWindowRectExForDpi"));
		const auto adjusted = adjust ? adjust(&rect, style, FALSE, ex_style, dpi) != FALSE
			                         : AdjustWindowRectEx(&rect, style, FALSE, ex_style) != FALSE;
		return adjusted ? core::sizei{rect.right - rect.left, rect.bottom - rect.top} :
		                   core::sizei{std::max<LONG>(0, rect.right), std::max<LONG>(0, rect.bottom)};
	}

	std::shared_ptr<platform::app> active_app();

	class win_window final : public platform::window,
	                         public IDropTarget,
	                         public std::enable_shared_from_this<win_window>
	{
	public:
		platform::window_options _options;
		std::weak_ptr<platform::window_events> _events;
		HWND _hwnd = nullptr;
		HCURSOR _cursor = nullptr;
		uint32_t _disable_count = 0;
		bool _nonclient_tracking = false;
		bool _first_nccalc = true;
		bool _composition = false;
		bool _full_screen = false;
		bool _window_region_initialized = false;
		core::recti _window_region;
		bool _ime_caret = false;
		wchar_t _pending_high_surrogate = 0;
		WINDOWPLACEMENT _restore_placement{sizeof(WINDOWPLACEMENT)};
		LONG_PTR _restore_style = 0;
		LONG_PTR _restore_ex_style = 0;
		ULONGLONG _zoom_distance = 0;
		core::pointi _pan_start;
		ComPtr<IDataObject> _drag_data;
		double _scale = 1.0;
		bool _caption_tracked = false;

		win_window(const platform::window_options& options, std::weak_ptr<platform::window_events> events) :
			_options(options), _events(std::move(events)), _cursor(cursor_for(platform::cursor::normal))
		{
		}

		~win_window() override
		{
			if (_hwnd && IsWindow(_hwnd))
			{
				SetWindowLongPtrW(_hwnd, GWLP_USERDATA, 0);
				DestroyWindow(_hwnd);
			}
			_hwnd = nullptr;
		}

		std::shared_ptr<platform::window_events> events() const
		{
			return _events.lock();
		}

		bool is_child() const
		{
			return _options.kind == platform::window_kind::child;
		}

		bool is_drawn_caption() const
		{
			return _options.drawn_caption && !is_child();
		}

		win_window* root_window()
		{
			auto* result = this;
			while (result->is_child() && result->_options.parent)
			{
				auto* parent = dynamic_cast<win_window*>(result->_options.parent);
				if (!parent || parent == result) break;
				result = parent;
			}
			return result;
		}

		LRESULT resize_hit(core::pointi loc) const;

		void sync_enabled()
		{
			if (_hwnd) EnableWindow(_hwnd, _disable_count == 0);
		}

		void created();
		void destroyed();
		void update_composition();
		void update_region();
		LRESULT nonclient_hit(LPARAM lparam);
		bool caption_point(LPARAM lparam, core::pointi& loc) const;
		LRESULT caption_message(UINT message, WPARAM wparam, LPARAM lparam, bool& handled);
		void send_system_event(platform::os_event_type event);
		void handle_dpi_changed(LPARAM lparam, WPARAM wparam);
		void handle_char(wchar_t value);
		void handle_gesture(LPARAM lparam);
		LRESULT paint();
		LRESULT message(UINT message, WPARAM wparam, LPARAM lparam);

		static std::optional<platform::pointer_button> button_for(UINT message, WPARAM wparam);

		// IDropTarget is kept alive by the HWND until RevokeDragDrop in WM_DESTROY.
		STDMETHOD(QueryInterface)(REFIID iid, void** object) noexcept override;
		STDMETHOD_(ULONG, AddRef)() noexcept override { return 1; }
		STDMETHOD_(ULONG, Release)() noexcept override { return 1; }
		STDMETHODIMP DragEnter(IDataObject* object, DWORD state, POINTL point, DWORD* effect) override;
		STDMETHODIMP DragOver(DWORD state, POINTL point, DWORD* effect) override;
		STDMETHODIMP DragLeave() override;
		STDMETHODIMP Drop(IDataObject* object, DWORD state, POINTL point, DWORD* effect) override;

		std::any native_handle() const override;
		platform::window_kind kind() const override;
		platform::window* root() override;
		void destroy() override;
		core::recti bounds() const override;
		void set_bounds(core::recti value, bool visible) override;
		core::sizei client_extent() const override;
		void show(bool show_window) override;
		bool is_visible() const override;
		void enable(bool enable_window) override;
		bool is_enabled() const override;
		void set_title(std::string_view text) override;
		std::string title() const override;
		void focus() override;
		bool has_focus() const override;
		void capture(bool capture_window) override;
		void set_cursor(platform::cursor cursor) override;
		core::pointi cursor_location() const override;
		core::pointi client_to_screen(core::pointi point) const override;
		core::pointi screen_to_client(core::pointi point) const override;
		double scale() const override;
		void set_resize_scale(double scale) override;
		void invalidate(core::recti value) override;
		void update_now() override;
		void scroll(int dx, int dy, core::recti value) override;
		void start_timer(uint32_t id, uint32_t milliseconds) override;
		void stop_timer(uint32_t id) override;
		bool is_minimized() const override;
		bool is_maximized() const override;
		bool is_occluded() const override;
		void sys_command(platform::sys_command_type command) override;
		void full_screen(bool full) override;
		bool is_full_screen() const override;
		bool presents_caption_button() const override;
		void set_text_input(bool active, core::recti caret) override;
		void save_placement(const platform::setting_file_ptr& store, std::string_view name) override;
	};

	std::vector<std::weak_ptr<win_window>> app_windows;

	void win_window::created()
	{
		_scale = window_dpi(_hwnd) / 96.0;
		if (_options.kind == platform::window_kind::app)
		{
			set_app_wnd(_hwnd);
			if (resources.app_icon_64) SendMessageW(_hwnd, WM_SETICON, ICON_BIG, reinterpret_cast<LPARAM>(resources.app_icon_64));
			if (resources.app_icon_32) SendMessageW(_hwnd, WM_SETICON, ICON_SMALL, reinterpret_cast<LPARAM>(resources.app_icon_32));
			WTSRegisterSessionNotification(_hwnd, NOTIFY_FOR_THIS_SESSION);
			update_composition();
		}
		if (auto e = events()) e->created(*this);
	}

	void win_window::destroyed()
	{
		if (_options.kind == platform::window_kind::app)
		{
			WTSUnRegisterSessionNotification(_hwnd);
			if (app_wnd() == _hwnd) set_app_wnd(nullptr);
		}
		if (_ime_caret) DestroyCaret();
		_ime_caret = false;
		if (_options.accepts_drops) RevokeDragDrop(_hwnd);
		if (auto e = events()) e->destroyed();
	}

	void win_window::update_composition()
	{
		BOOL enabled = FALSE;
		DwmIsCompositionEnabled(&enabled);
		_composition = enabled != FALSE;
		if (is_drawn_caption() && _composition)
		{
			const auto style = static_cast<uint32_t>(GetWindowLongPtrW(_hwnd, GWL_STYLE));
			const auto shadow = _full_screen ? 0 : 1;
			const auto shadow_at_bottom = platform::can_present_maximize_caption_button(
				::windows_release(), style);
			const MARGINS margins{0, 0, shadow_at_bottom ? 0 : shadow, shadow_at_bottom ? shadow : 0};
			DwmExtendFrameIntoClientArea(_hwnd, &margins);
			const auto policy = DWMNCRP_ENABLED;
			DwmSetWindowAttribute(_hwnd, DWMWA_NCRENDERING_POLICY, &policy, sizeof(policy));
		}
		update_region();
	}

	void win_window::update_region()
	{
		if (!is_drawn_caption()) return;
		RECT region{};
		bool use_region = false;
		if (IsZoomed(_hwnd))
		{
			WINDOWINFO info{sizeof(info)};
			if (GetWindowInfo(_hwnd, &info))
			{
				region = {info.rcClient.left - info.rcWindow.left, info.rcClient.top - info.rcWindow.top,
					info.rcClient.right - info.rcWindow.left, info.rcClient.bottom - info.rcWindow.top};
				use_region = true;
			}
		}
		else if (!_composition)
		{
			region = {0, 0, 32767, 32767};
			use_region = true;
		}

		const core::recti wanted = use_region ? rect_from(region) : core::recti{};
		if (_window_region_initialized && wanted == _window_region) return;
		_window_region_initialized = true;
		_window_region = wanted;
		HRGN rgn = use_region ? CreateRectRgnIndirect(&region) : nullptr;
		if (use_region && !rgn) return;
		if (!SetWindowRgn(_hwnd, rgn, TRUE) && rgn) DeleteObject(rgn);
	}

	bool monitor_has_autohide_taskbar_uncached(const UINT edge, const HMONITOR monitor)
	{
		APPBARDATA taskbar_data_for_getautohidebar = {sizeof(APPBARDATA), nullptr, 0, edge};
		taskbar_data_for_getautohidebar.hWnd = GetForegroundWindow();
		// ABM_GETAUTOHIDEBAR is the precise query, but it can fail on a secondary monitor.
		// Fall back to the shell's global autohide state and its reported edge in that case.
		auto taskbar = reinterpret_cast<HWND>(
			SHAppBarMessage(ABM_GETAUTOHIDEBAR, &taskbar_data_for_getautohidebar));
		if (!IsWindow(taskbar))
		{
			APPBARDATA taskbar_data = {sizeof(APPBARDATA), nullptr, 0, 0};
			const auto taskbar_state = SHAppBarMessage(ABM_GETSTATE, &taskbar_data);
			if (!(taskbar_state & ABS_AUTOHIDE)) return false;

			taskbar_data.hWnd = FindWindowW(L"Shell_TrayWnd", nullptr);
			if (!IsWindow(taskbar_data.hWnd)) return false;
			SHAppBarMessage(ABM_GETTASKBARPOS, &taskbar_data);
			if (taskbar_data.uEdge == edge) taskbar = taskbar_data.hWnd;
		}

		if (IsWindow(taskbar))
		{
			if (MonitorFromWindow(taskbar, MONITOR_DEFAULTTONEAREST) == monitor) return true;
			// MonitorFromWindow can report the wrong monitor for a taskbar on a secondary edge;
			// the cursor monitor is the reliable fallback used by the old platform layer.
			POINT cursor_pos{0, 0};
			GetCursorPos(&cursor_pos);
			if (MonitorFromPoint(cursor_pos, MONITOR_DEFAULTTONEAREST) == monitor) return true;
		}
		return false;
	}

	static bool monitor_has_autohide_taskbar_cached(const UINT edge, const HMONITOR monitor)
	{
		constexpr uint64_t cache_ms = 250;
		struct cache_entry
		{
			HMONITOR monitor = nullptr;
			uint64_t when = 0;
			bool value = false;
		};
		static cache_entry entries[4];
		static_assert(ABE_LEFT == 0 && ABE_TOP == 1 && ABE_RIGHT == 2 && ABE_BOTTOM == 3);

		if (edge > ABE_BOTTOM) return monitor_has_autohide_taskbar_uncached(edge, monitor);
		auto& entry = entries[edge];
		const auto now = GetTickCount64();
		if (entry.when != 0 && entry.monitor == monitor && (now - entry.when) < cache_ms)
			return entry.value;

		entry.monitor = monitor;
		entry.value = monitor_has_autohide_taskbar_uncached(edge, monitor);
		entry.when = GetTickCount64();
		return entry.value;
	}

	bool monitor_has_autohide_taskbar(const HMONITOR monitor, const UINT edge)
	{
		return monitor_has_autohide_taskbar_cached(edge, monitor);
	}

	LRESULT win_window::resize_hit(const core::pointi loc) const
	{
		if (!is_drawn_caption() || _full_screen || IsZoomed(_hwnd) || !_options.resizable) return HTCLIENT;
		RECT rc{};
		GetClientRect(_hwnd, &rc);
		const auto ui_scale = _scale * std::max(1.0, _options.resize_scale);
		const auto side = std::max(1, core::round(resize_side * ui_scale));
		const auto bottom = std::max(1, core::round(resize_bottom * ui_scale));
		const auto corner = std::max(side, core::round(resize_corner * ui_scale));
		const auto left = loc.x < rc.left + side;
		const auto right = loc.x >= rc.right - side;
		const auto top = loc.y < rc.top + side;
		const auto bottom_edge = loc.y >= rc.bottom - bottom;
		const auto near_left = loc.x < rc.left + corner;
		const auto near_right = loc.x >= rc.right - corner;
		const auto near_top = loc.y < rc.top + corner;
		const auto near_bottom = loc.y >= rc.bottom - corner;
		if (top)
		{
			if (near_left) return HTTOPLEFT;
			if (near_right) return HTTOPRIGHT;
			return HTTOP;
		}
		if (bottom_edge)
		{
			if (near_left) return HTBOTTOMLEFT;
			if (near_right) return HTBOTTOMRIGHT;
			return HTBOTTOM;
		}
		if (left)
		{
			if (near_top) return HTTOPLEFT;
			if (near_bottom) return HTBOTTOMLEFT;
			return HTLEFT;
		}
		if (right)
		{
			if (near_top) return HTTOPRIGHT;
			if (near_bottom) return HTBOTTOMRIGHT;
			return HTRIGHT;
		}
		return HTCLIENT;
	}

	LRESULT win_window::nonclient_hit(const LPARAM lparam)
	{
		if (_options.layered) return HTTRANSPARENT;
		POINT screen{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
		POINT root_client = screen;
		auto* root = root_window();
		if (!root || !root->_hwnd) return DefWindowProcW(_hwnd, WM_NCHITTEST, 0, lparam);
		ScreenToClient(root->_hwnd, &root_client);
		const auto loc = core::pointi{root_client.x, root_client.y};
		const auto root_events = root->events();
		const auto hit = root_events ? root_events->hit_test(loc) : platform::hit_region::client;

		// Drawn controls own their pixels even when those pixels overlap the native resize band.
		if (hit == platform::hit_region::control) return HTCLIENT;
		const auto edge = root->resize_hit(loc);

		if (root != this)
		{
			// A child surface must let its top-level owner answer for the drawn caption and resize band.
			if (edge != HTCLIENT || hit == platform::hit_region::caption ||
				hit == platform::hit_region::maximize_button) return HTTRANSPARENT;
			return DefWindowProcW(_hwnd, WM_NCHITTEST, 0, lparam);
		}

		if (is_drawn_caption())
		{
			if (edge != HTCLIENT) return edge;
			if (hit == platform::hit_region::maximize_button && presents_caption_button()) return HTMAXBUTTON;
			if (hit == platform::hit_region::caption) return HTCAPTION;
			return HTCLIENT;
		}

		const auto result = DefWindowProcW(_hwnd, WM_NCHITTEST, 0, lparam);
		if (result == HTCLIENT)
		{
			if (hit == platform::hit_region::caption) return HTCAPTION;
			if (hit == platform::hit_region::maximize_button && presents_caption_button()) return HTMAXBUTTON;
		}
		return result;
	}

	bool win_window::caption_point(const LPARAM lparam, core::pointi& loc) const
	{
		POINT point{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
		ScreenToClient(_hwnd, &point);
		loc = {point.x, point.y};
		if (!presents_caption_button()) return false;
		if (auto e = events()) return e->hit_test(loc) == platform::hit_region::maximize_button;
		return false;
	}

	LRESULT win_window::caption_message(const UINT message, const WPARAM wparam, const LPARAM lparam, bool& handled)
	{
		handled = false;
		if (!is_drawn_caption()) return 0;
		core::pointi loc;
		if (message == WM_NCHITTEST)
		{
			if (caption_point(lparam, loc))
			{
				handled = true;
				return HTMAXBUTTON;
			}
			return 0;
		}

		if (message == WM_NCMOUSEMOVE)
		{
			const auto hit = wparam == HTMAXBUTTON && caption_point(lparam, loc);
			if (hit && !_nonclient_tracking)
			{
				track_nonclient_leave(_hwnd);
				_nonclient_tracking = true;
			}
			if (auto e = events())
			{
				if (hit) e->caption_button(platform::caption_button_action::hover, loc);
				else if (_caption_tracked) e->caption_button(platform::caption_button_action::leave, loc);
			}
			_caption_tracked = hit;
			return 0;
		}
		if (message == WM_NCMOUSELEAVE)
		{
			_nonclient_tracking = false;
			if (_caption_tracked)
			{
				if (auto e = events()) e->caption_button(platform::caption_button_action::leave, {});
			}
			_caption_tracked = false;
			return 0;
		}
		if ((message == WM_NCLBUTTONDOWN || message == WM_NCLBUTTONDBLCLK) && wparam == HTMAXBUTTON)
		{
			caption_point(lparam, loc);
			if (auto e = events()) e->caption_button(platform::caption_button_action::press, loc);
			_caption_tracked = true;
			handled = true;
			return 0;
		}
		if (message == WM_NCLBUTTONUP && (_caption_tracked || wparam == HTMAXBUTTON))
		{
			const auto hit = caption_point(lparam, loc);
			if (auto e = events()) e->caption_button(platform::caption_button_action::release, loc);
			_caption_tracked = false;
			handled = true;
			return 0;
		}
		return 0;
	}

	void win_window::send_system_event(const platform::os_event_type event)
	{
		if (auto e = events()) e->system_event(event);
	}

	void win_window::handle_dpi_changed(const LPARAM lparam, const WPARAM wparam)
	{
		_scale = std::max<UINT>(1u, LOWORD(wparam)) / 96.0;
		if (auto e = events()) e->scale_changed(_scale);
		if (lparam && _options.kind != platform::window_kind::popup)
		{
			const auto suggested = reinterpret_cast<const RECT*>(lparam);
			SetWindowPos(_hwnd, nullptr, suggested->left, suggested->top,
			             suggested->right - suggested->left, suggested->bottom - suggested->top,
			             SWP_NOZORDER | SWP_NOACTIVATE);
		}
	}

	void win_window::handle_char(const wchar_t value)
	{
		std::wstring text;
		if (value >= 0xd800 && value <= 0xdbff)
		{
			_pending_high_surrogate = value;
			return;
		}
		if (value >= 0xdc00 && value <= 0xdfff)
		{
			if (_pending_high_surrogate)
			{
				text.push_back(_pending_high_surrogate);
				text.push_back(value);
			}
			_pending_high_surrogate = 0;
		}
		else
		{
			_pending_high_surrogate = 0;
			if (value >= L' ') text.push_back(value);
		}
		if (!text.empty())
		{
			if (auto e = events()) e->text(core::utf16_to_utf8(text));
		}
	}
	void win_window::handle_gesture(const LPARAM lparam)
	{
		GESTUREINFO info{sizeof(info)};
		const auto gesture = reinterpret_cast<HGESTUREINFO>(lparam);
		if (!GetGestureInfo(gesture, &info)) return;
		POINT point{info.ptsLocation.x, info.ptsLocation.y};
		ScreenToClient(_hwnd, &point);
		const core::pointi loc{point.x, point.y};
		if (info.dwID == GID_PAN)
		{
			if (info.dwFlags & GF_BEGIN) _pan_start = loc;
			if (auto e = events())
			{
				if (info.dwFlags & GF_BEGIN) e->pan_start(loc);
				else if (info.dwFlags & GF_END) e->pan_end(_pan_start, loc);
				else e->pan(_pan_start, loc);
			}
			CloseGestureInfoHandle(gesture);
			return;
		}
		if (info.dwID == GID_ZOOM)
		{
			if (info.dwFlags & GF_BEGIN)
			{
				_zoom_distance = info.ullArguments;
				if (auto e = events()) e->pinch(loc, 0, true);
			}
			else if (_zoom_distance && info.ullArguments)
			{
				const auto delta = core::round(std::log(static_cast<double>(info.ullArguments) /
					static_cast<double>(_zoom_distance)) * 480.0);
				if (delta)
				{
					if (auto e = events()) e->pinch(loc, delta, false);
				}
				_zoom_distance = info.ullArguments;
			}
			if (info.dwFlags & GF_END) _zoom_distance = 0;
			CloseGestureInfoHandle(gesture);
			return;
		}
		CloseGestureInfoHandle(gesture);
	}

	LRESULT win_window::paint()
	{
		PAINTSTRUCT ps{};
		const auto dc = BeginPaint(_hwnd, &ps);
		if (dc)
		{
			if (_options.container)
			{
				const auto c = _options.background;
				const auto brush = CreateSolidBrush(RGB(core::get_r(c), core::get_g(c), core::get_b(c)));
				FillRect(dc, &ps.rcPaint, brush);
				DeleteObject(brush);
			}
			else if (auto e = events())
			{
				e->paint(rect_from(ps.rcPaint));
			}
			EndPaint(_hwnd, &ps);
		}
		return 0;
	}

	LRESULT win_window::message(const UINT message, const WPARAM wparam, const LPARAM lparam)
	{
		const auto keep_alive = weak_from_this().lock();
		bool caption_handled = false;
		if (is_drawn_caption() && (message == WM_NCHITTEST || message == WM_NCMOUSEMOVE ||
		                    message == WM_NCMOUSELEAVE || message == WM_NCLBUTTONDOWN ||
		                    message == WM_NCLBUTTONDBLCLK || message == WM_NCLBUTTONUP))
		{
			const auto result = caption_message(message, wparam, lparam, caption_handled);
			if (caption_handled) return result;
			if (message == WM_NCMOUSEMOVE || message == WM_NCMOUSELEAVE)
				return DefWindowProcW(_hwnd, message, wparam, lparam);
		}

		switch (message)
		{
		case WM_NCCREATE:
			return DefWindowProcW(_hwnd, message, wparam, lparam);
		case WM_CREATE:
			created();
			return DefWindowProcW(_hwnd, message, wparam, lparam);
		case WM_DESTROY:
			KillTimer(_hwnd, 0);
			destroyed();
			return DefWindowProcW(_hwnd, message, wparam, lparam);
		case WM_NCDESTROY:
			{
				const auto hwnd = _hwnd;
				SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
				_hwnd = nullptr;
				return DefWindowProcW(hwnd, message, wparam, lparam);
			}
		case WM_PAINT:
			return paint();
		case WM_PRINTCLIENT:
			return 0;
		case WM_ERASEBKGND:
			return 1;
		case WM_DISPLAYCHANGE:
			InvalidateRect(_hwnd, nullptr, FALSE);
			return 0;
		case WM_SIZE:
			{
				update_region();
				const auto minimized = wparam == SIZE_MINIMIZED;
				if (auto e = events()) e->size_changed({static_cast<int>(LOWORD(lparam)), static_cast<int>(HIWORD(lparam))}, minimized);
				if (!minimized && LOWORD(lparam) != 0 && HIWORD(lparam) != 0)
				{
					InvalidateRect(_hwnd, nullptr, FALSE);
					RedrawWindow(_hwnd, nullptr, nullptr, RDW_ALLCHILDREN | RDW_UPDATENOW);
				}
				return DefWindowProcW(_hwnd, message, wparam, lparam);
			}
		case WM_DPICHANGED:
			handle_dpi_changed(lparam, wparam);
			return 0;
		case WM_SETCURSOR:
			if (reinterpret_cast<HWND>(wparam) == _hwnd && LOWORD(lparam) == HTCLIENT)
			{
				SetCursor(_cursor);
				return TRUE;
			}
			return DefWindowProcW(_hwnd, message, wparam, lparam);
		case WM_MOUSEACTIVATE:
			if (_options.menu || !_options.can_focus || _options.layered || _options.offscreen) return MA_NOACTIVATE;
			return DefWindowProcW(_hwnd, message, wparam, lparam);
		case WM_ACTIVATE:
			{
				const auto result = DefWindowProcW(_hwnd, message, wparam, lparam);
				if (auto e = events()) e->activated(wparam == WA_ACTIVE || wparam == WA_CLICKACTIVE);
				return result;
			}
		case WM_SETFOCUS:
			if (auto e = events()) e->focus_changed(true);
			return 0;
		case WM_KILLFOCUS:
			if (_ime_caret) DestroyCaret();
			_ime_caret = false;
			_zoom_distance = 0;
			if (auto e = events()) e->focus_changed(false);
			return 0;
		case WM_KEYDOWN:
		case WM_SYSKEYDOWN:
			{
				const auto system = message == WM_SYSKEYDOWN;
				const auto e = events();
				auto keys = platform::current_key_state();
				// Bit 30 is the key's previous state: down already means the keyboard is repeating it.
				keys.repeat = (lparam & (LPARAM{1} << 30)) != 0;
				const auto handled = e && e->key_down(static_cast<int>(wparam), keys, system);
				if (handled) return 0;
				// Translation is deliberately delayed until after the events have declined the key.
				// The application loop therefore must not call TranslateMessage for platform windows.
				MSG translated{};
				translated.hwnd = _hwnd;
				translated.message = message;
				translated.wParam = wparam;
				translated.lParam = lparam;
				TranslateMessage(&translated);
				return DefWindowProcW(_hwnd, message, wparam, lparam);
			}
		case WM_KEYUP:
		case WM_SYSKEYUP:
			{
				const auto system = message == WM_SYSKEYUP;
				const auto e = events();
				const auto handled = e && e->key_up(static_cast<int>(wparam), platform::current_key_state(), system);
				return handled ? 0 : DefWindowProcW(_hwnd, message, wparam, lparam);
			}
		case WM_CHAR:
			handle_char(static_cast<wchar_t>(wparam));
			return 0;
		case WM_SYSCHAR:
			return DefWindowProcW(_hwnd, message, wparam, lparam);
		case WM_CLOSE:
			if (auto e = events()) e->close_requested();
			return 0;
		case WM_CONTEXTMENU:
			{
				const auto from_keyboard = lparam == static_cast<LPARAM>(-1);
				if (auto e = events())
				{
					if (from_keyboard) e->context_menu({-1, -1}, true);
					else e->context_menu({GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)}, false);
				}
				return 0;
			}
		case WM_CANCELMODE:
			if (auto e = events()) e->cancel_mode();
			return DefWindowProcW(_hwnd, message, wparam, lparam);
		case WM_CAPTURECHANGED:
			if (wparam && reinterpret_cast<HWND>(wparam) != _hwnd)
				if (auto e = events()) e->cancel_mode();
			return DefWindowProcW(_hwnd, message, wparam, lparam);
		case WM_MOUSEMOVE:
			track_mouse_leave(_hwnd);
			if (auto e = events()) e->pointer_move({GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)}, pointer_keys(message, wparam));
			return 0;
		case WM_MOUSELEAVE:
			if (auto e = events()) e->pointer_leave();
			return 0;
		case WM_LBUTTONDOWN:
		case WM_MBUTTONDOWN:
		case WM_RBUTTONDOWN:
		case WM_XBUTTONDOWN:
			{
				const auto button = button_for(message, wparam);
				if (button)
				{
					if (auto e = events())
						e->pointer_down(
							*button, {GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)}, pointer_keys(message, wparam));
				}
				return message == WM_XBUTTONDOWN ? TRUE : 0;
			}
		case WM_LBUTTONUP:
		case WM_MBUTTONUP:
		case WM_RBUTTONUP:
		case WM_XBUTTONUP:
			{
				const auto button = button_for(message, wparam);
				if (button && events())
					events()->pointer_up(
						*button, {GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)}, pointer_keys(message, wparam));
				// Let DefWindowProc synthesize WM_CONTEXTMENU for a mouse right-button release.
				if (message == WM_RBUTTONUP) return DefWindowProcW(_hwnd, message, wparam, lparam);
				return message == WM_XBUTTONUP ? TRUE : 0;
			}
		case WM_LBUTTONDBLCLK:
		case WM_MBUTTONDBLCLK:
		case WM_RBUTTONDBLCLK:
		case WM_XBUTTONDBLCLK:
			{
				const auto button = button_for(message, wparam);
				if (button)
				{
					const auto extra = static_cast<ULONG_PTR>(GetMessageExtraInfo());
					const auto touch = (extra & 0xffffff00u) == 0xff515700u && (extra & 0x80u) != 0;
					if (touch)
					{
						if (auto e = events(); e && e->touch_double_tap({GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)})) return 0;
					}
					if (auto e = events())
						e->double_click(
							*button, {GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)}, pointer_keys(message, wparam));
				}
				return message == WM_XBUTTONDBLCLK ? TRUE : 0;
			}
		case WM_MOUSEWHEEL:
		case WM_MOUSEHWHEEL:
		case wm_pointer_wheel:
		case wm_pointer_hwheel:
			{
				POINT point{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
				ScreenToClient(_hwnd, &point);
				const auto axis = message == WM_MOUSEHWHEEL || message == wm_pointer_hwheel
					? platform::wheel_axis::horizontal
					: platform::wheel_axis::vertical;
				const auto delta = static_cast<short>(HIWORD(wparam));
				const auto e = events();
				const auto handled = e && e->wheel({point.x, point.y}, delta, axis, pointer_keys(message, wparam));
				if (handled) return 0;
				if (axis == platform::wheel_axis::vertical && GetParent(_hwnd))
					return SendMessageW(GetParent(_hwnd), message, wparam, lparam);
				return DefWindowProcW(_hwnd, message, wparam, lparam);
			}
		case WM_GESTURENOTIFY:
			if (_options.kind == platform::window_kind::app || _options.kind == platform::window_kind::dialog)
				return DefWindowProcW(_hwnd, message, wparam, lparam);
			{
				constexpr auto wanted = GC_PAN_WITH_SINGLE_FINGER_VERTICALLY | GC_PAN_WITH_SINGLE_FINGER_HORIZONTALLY;
				GESTURECONFIG config[] = {{GID_PAN, wanted, 0}, {GID_ZOOM, GC_ZOOM, 0}};
				SetGestureConfig(_hwnd, 0, std::size(config), config, sizeof(GESTURECONFIG));
				return DefWindowProcW(_hwnd, message, wparam, lparam);
			}
		case WM_GESTURE:
			if (_options.kind == platform::window_kind::app || _options.kind == platform::window_kind::dialog)
				return DefWindowProcW(_hwnd, message, wparam, lparam);
			handle_gesture(lparam);
			return 0;
		case WM_TIMER:
			if (_options.kind == platform::window_kind::app && wparam == platform_activity_timer_id)
			{
				++platform::ticks_since_last_user_action;
				dispatch_frame_tick_if_due();
				return 0;
			}
			if (auto e = events()) e->timer(static_cast<uint32_t>(wparam));
			return 0;
		case WM_NCHITTEST:
			return nonclient_hit(lparam);
		case WM_NCACTIVATE:
			if (is_drawn_caption()) return DefWindowProcW(_hwnd, message, wparam, static_cast<LPARAM>(-1));
			return DefWindowProcW(_hwnd, message, wparam, lparam);
		case WM_NCCALCSIZE:
		{
			if (!is_drawn_caption()) return DefWindowProcW(_hwnd, message, wparam, lparam);
			if (_first_nccalc)
			{
				_first_nccalc = false;
				return DefWindowProcW(_hwnd, message, wparam, lparam);
			}
			if (!wparam || !lparam) return DefWindowProcW(_hwnd, message, wparam, lparam);
			auto* params = reinterpret_cast<NCCALCSIZE_PARAMS*>(lparam);
			RECT* client = &params->rgrc[0];
			auto monitor = MonitorFromWindow(_hwnd, MONITOR_DEFAULTTONULL);
			if (!monitor) monitor = MonitorFromRect(client, MONITOR_DEFAULTTONULL);
			if (!monitor) return 0;

			if (!_full_screen && IsZoomed(_hwnd))
			{
				MONITORINFO info{sizeof(info)};
				if (GetMonitorInfoW(monitor, &info)) *client = info.rcWork;
				constexpr int auto_hide_taskbar_thickness = 2;
				if (monitor_has_autohide_taskbar(monitor, ABE_LEFT)) client->left += auto_hide_taskbar_thickness;
				if (monitor_has_autohide_taskbar(monitor, ABE_TOP)) client->top += auto_hide_taskbar_thickness;
				if (monitor_has_autohide_taskbar(monitor, ABE_RIGHT)) client->right -= auto_hide_taskbar_thickness;
				if (monitor_has_autohide_taskbar(monitor, ABE_BOTTOM)) client->bottom -= auto_hide_taskbar_thickness;
			}

			const auto& old_client = params->rgrc[2];
			const auto changed = client->right - client->left != old_client.right - old_client.left ||
				client->bottom - client->top != old_client.bottom - old_client.top;
			// Returning WVR_REDRAW instead of DefWindowProc keeps the drawn caption's normal window
			// entirely client-area and prevents stale pixels being copied during resize.
			return changed ? WVR_REDRAW : 0;
		}
		case WM_NCPAINT:
			if (is_drawn_caption() && !_composition) return 0;
			return DefWindowProcW(_hwnd, message, wparam, lparam);
		case wm_ncuah_draw_caption:
		case wm_ncuah_draw_frame:
			if (is_drawn_caption()) return 0;
			break;
		case WM_DWMCOMPOSITIONCHANGED:
			if (is_drawn_caption()) update_composition();
			return 0;
		case WM_GETMINMAXINFO:
			if (lparam && (_options.minimum_client.cx > 0 || _options.minimum_client.cy > 0 ||
				_options.kind == platform::window_kind::app))
			{
				const auto style = static_cast<DWORD>(GetWindowLongPtrW(_hwnd, GWL_STYLE));
				const auto ex_style = static_cast<DWORD>(GetWindowLongPtrW(_hwnd, GWL_EXSTYLE));
				const auto minimum = _options.kind == platform::window_kind::app &&
					_options.minimum_client.cx == 0 && _options.minimum_client.cy == 0
					? core::sizei{640, 320} : _options.minimum_client;
				RECT r{0, 0, std::max(0, minimum.cx), std::max(0, minimum.cy)};
				AdjustWindowRectEx(&r, style, FALSE, ex_style);
				auto* info = reinterpret_cast<MINMAXINFO*>(lparam);
				info->ptMinTrackSize.x = std::max<LONG>(info->ptMinTrackSize.x, static_cast<LONG>(r.right - r.left));
				info->ptMinTrackSize.y = std::max<LONG>(info->ptMinTrackSize.y, static_cast<LONG>(r.bottom - r.top));
				return 0;
			}
			break;
		case WM_WTSSESSION_CHANGE:
			if (_options.kind == platform::window_kind::app && wparam == WTS_SESSION_LOCK)
				send_system_event(platform::os_event_type::screen_locked);
			return 0;
		case WM_POWERBROADCAST:
			if (_options.kind == platform::window_kind::app)
			{
				if (wparam == PBT_APMSUSPEND) send_system_event(platform::os_event_type::system_suspending);
				else if (wparam == PBT_APMRESUMEAUTOMATIC || wparam == PBT_APMRESUMESUSPEND)
					send_system_event(platform::os_event_type::system_resumed);
			}
			return TRUE;
		case WM_QUERYENDSESSION:
			if (_options.kind == platform::window_kind::app)
			{
				send_system_event(platform::os_event_type::session_ending);
				if (lparam & ENDSESSION_CRITICAL) return TRUE;
				const auto app = active_app();
				return !app || app->can_exit();
			}
			break;
		case WM_ENDSESSION:
			if (_options.kind == platform::window_kind::app && wparam)
			{
				send_system_event(platform::os_event_type::session_ending);
				platform::is_closing = true;
				platform::event_exit.set();
				platform::close_log();
			}
			return 0;
		case WM_SETTINGCHANGE:
			if (_options.kind == platform::window_kind::app)
			{
				number_format_invalid = true;
				send_system_event(platform::os_event_type::options_changed);
			}
			return 0;
		case WM_DEVICECHANGE:
			if (_options.kind == platform::window_kind::app && wparam == DBT_DEVNODES_CHANGED)
				send_system_event(platform::os_event_type::system_device_change);
			return 1;
		case WM_IME_SETCONTEXT:
			return DefWindowProcW(_hwnd, message, wparam, lparam);
		default:
			break;
		}
		return DefWindowProcW(_hwnd, message, wparam, lparam);
	}

	std::optional<platform::pointer_button> win_window::button_for(const UINT message, const WPARAM wparam)
	{
		switch (message)
		{
		case WM_LBUTTONDOWN:
		case WM_LBUTTONUP:
		case WM_LBUTTONDBLCLK: return platform::pointer_button::left;
		case WM_MBUTTONDOWN:
		case WM_MBUTTONUP:
		case WM_MBUTTONDBLCLK: return platform::pointer_button::middle;
		case WM_RBUTTONDOWN:
		case WM_RBUTTONUP:
		case WM_RBUTTONDBLCLK: return platform::pointer_button::right;
		case WM_XBUTTONDOWN:
		case WM_XBUTTONUP:
		case WM_XBUTTONDBLCLK: return GET_XBUTTON_WPARAM(wparam) == XBUTTON1 ? platform::pointer_button::x1 : platform::pointer_button::x2;
		default: return std::nullopt;
		}
	}

	STDMETHODIMP win_window::QueryInterface(REFIID iid, void** object) noexcept
	{
		if (!object) return E_POINTER;
		if (iid == IID_IUnknown || iid == IID_IDropTarget)
		{
			*object = static_cast<IDropTarget*>(this);
			return S_OK;
		}
		*object = nullptr;
		return E_NOINTERFACE;
	}

	void set_drop_effect(const platform::drop_effect effect, DWORD* result)
	{
		if (!result) return;
		switch (effect)
		{
		case platform::drop_effect::copy: *result = DROPEFFECT_COPY; break;
		case platform::drop_effect::move: *result = DROPEFFECT_MOVE; break;
		case platform::drop_effect::link: *result = DROPEFFECT_LINK; break;
		default: *result = DROPEFFECT_NONE; break;
		}
	}

	core::pointi drop_location(const HWND hwnd, const POINTL& point)
	{
		POINT p{point.x, point.y};
		ScreenToClient(hwnd, &p);
		return {p.x, p.y};
	}

	STDMETHODIMP win_window::DragEnter(IDataObject* object, DWORD state, POINTL point, DWORD* effect)
	{
		_drag_data = object;
		return DragOver(state, point, effect);
	}

	STDMETHODIMP win_window::DragOver(DWORD state, POINTL point, DWORD* effect)
	{
		if (auto e = events())
		{
			data_object_client data(_drag_data.Get());
			set_drop_effect(e->drag_over(data, drop_keys(state), drop_location(_hwnd, point)), effect);
		}
		else if (effect) *effect = DROPEFFECT_NONE;
		return S_OK;
	}

	STDMETHODIMP win_window::DragLeave()
	{
		_drag_data.Reset();
		if (auto e = events()) e->drag_leave();
		return S_OK;
	}

	STDMETHODIMP win_window::Drop(IDataObject* object, DWORD state, POINTL point, DWORD* effect)
	{
		_drag_data.Reset();
		if (auto e = events())
		{
			data_object_client data(object);
			set_drop_effect(e->drag_drop(data, drop_keys(state), drop_location(_hwnd, point)), effect);
		}
		else if (effect) *effect = DROPEFFECT_NONE;
		return S_OK;
	}
	std::any win_window::native_handle() const { return _hwnd; }
	platform::window_kind win_window::kind() const { return _options.kind; }
	platform::window* win_window::root() { return root_window(); }

	void win_window::destroy()
	{
		if (_hwnd) DestroyWindow(_hwnd);
	}

	core::recti win_window::bounds() const
	{
		RECT r{};
		if (!_hwnd) return {};
		if (is_child())
		{
			GetWindowRect(_hwnd, &r);
			const auto parent = GetParent(_hwnd);
			if (parent) MapWindowPoints(HWND_DESKTOP, parent, reinterpret_cast<POINT*>(&r), 2);
		}
		else GetWindowRect(_hwnd, &r);
		return rect_from(r);
	}

	void win_window::set_bounds(const core::recti value, const bool visible)
	{
		if (!_hwnd) return;
		const auto old = bounds();
		const auto was_visible = is_visible();
		if (old == value && was_visible == visible) return;
		const auto offscreen = _options.offscreen && !is_child();
		const auto insert_after = offscreen ? HWND_BOTTOM : nullptr;
		UINT flags = SWP_NOACTIVATE;
		if (!offscreen) flags |= SWP_NOZORDER;
		if (old.width() != value.width() || old.height() != value.height()) flags |= SWP_NOCOPYBITS;
		if (was_visible != visible) flags |= visible ? SWP_SHOWWINDOW : SWP_HIDEWINDOW;
		SetWindowPos(_hwnd, insert_after, offscreen ? -32000 : value.left, offscreen ? -32000 : value.top,
		             value.width(), value.height(), flags);
	}

	core::sizei win_window::client_extent() const
	{
		RECT r{};
		if (_hwnd) GetClientRect(_hwnd, &r);
		return {r.right - r.left, r.bottom - r.top};
	}

	void win_window::show(const bool show_window)
	{
		if (!_hwnd) return;
		if (!show_window)
		{
			ShowWindow(_hwnd, SW_HIDE);
			return;
		}
		if (_options.offscreen && !is_child())
		{
			SetWindowPos(_hwnd, HWND_BOTTOM, -32000, -32000, 0, 0, SWP_NOSIZE | SWP_NOACTIVATE);
			ShowWindow(_hwnd, SW_SHOWNOACTIVATE);
			UpdateWindow(_hwnd);
			return;
		}
		ShowWindow(_hwnd, _options.maximized ? SW_SHOWMAXIMIZED : SW_SHOWNOACTIVATE);
	}

	bool win_window::is_visible() const { return _hwnd && IsWindowVisible(_hwnd) != FALSE; }

	void win_window::enable(const bool enable_window)
	{
		if (enable_window)
		{
			if (_disable_count) --_disable_count;
		}
		else ++_disable_count;
		sync_enabled();
	}

	bool win_window::is_enabled() const { return _disable_count == 0 && _hwnd && IsWindowEnabled(_hwnd) != FALSE; }

	void win_window::set_title(const std::string_view text)
	{
		if (_hwnd)
		{
			const auto wide = core::utf8_to_utf16(text);
			SetWindowTextW(_hwnd, wide.c_str());
		}
	}

	std::string win_window::title() const
	{
		if (!_hwnd) return {};
		const auto length = GetWindowTextLengthW(_hwnd);
		std::wstring value(static_cast<size_t>(length) + 1, L'\0');
		GetWindowTextW(_hwnd, value.data(), length + 1);
		value.resize(static_cast<size_t>(length));
		return core::utf16_to_utf8(value);
	}

	void win_window::focus()
	{
		if (_hwnd && !_options.offscreen) SetFocus(_hwnd);
	}
	bool win_window::has_focus() const { return _hwnd && GetFocus() == _hwnd; }

	void win_window::capture(const bool capture_window)
	{
		if (!_hwnd) return;
		if (capture_window) SetCapture(_hwnd);
		else if (GetCapture() == _hwnd) ReleaseCapture();
	}

	void win_window::set_cursor(const platform::cursor cursor)
	{
		_cursor = cursor_for(cursor);
		SetCursor(_cursor);
	}

	core::pointi win_window::cursor_location() const
	{
		POINT point{};
		if (GetCursorPos(&point) && _hwnd) ScreenToClient(_hwnd, &point);
		return {point.x, point.y};
	}

	core::pointi win_window::client_to_screen(const core::pointi point) const
	{
		POINT p{point.x, point.y};
		if (_hwnd) ClientToScreen(_hwnd, &p);
		return {p.x, p.y};
	}

	core::pointi win_window::screen_to_client(const core::pointi point) const
	{
		POINT p{point.x, point.y};
		if (_hwnd) ScreenToClient(_hwnd, &p);
		return {p.x, p.y};
	}

	double win_window::scale() const { return _scale; }

	void win_window::set_resize_scale(const double scale)
	{
		_options.resize_scale = scale;
	}

	void win_window::invalidate(const core::recti value)
	{
		if (!_hwnd) return;
		if (value.is_empty()) InvalidateRect(_hwnd, nullptr, FALSE);
		else { const auto r = rect_to(value); InvalidateRect(_hwnd, &r, FALSE); }
	}

	void win_window::update_now()
	{
		if (_hwnd) RedrawWindow(_hwnd, nullptr, nullptr, RDW_INVALIDATE | RDW_UPDATENOW);
	}

	void win_window::scroll(const int dx, const int dy, const core::recti value)
	{
		if (!_hwnd) return;
		const auto r = rect_to(value);
		ScrollWindowEx(_hwnd, dx, dy, value.is_empty() ? nullptr : &r, nullptr, nullptr, nullptr,
		              SW_INVALIDATE | SW_ERASE);
	}

	void win_window::start_timer(const uint32_t id, const uint32_t milliseconds)
	{
		if (_hwnd) SetTimer(_hwnd, id, milliseconds, nullptr);
	}

	void win_window::stop_timer(const uint32_t id)
	{
		if (_hwnd) KillTimer(_hwnd, id);
	}

	bool win_window::is_minimized() const { return _hwnd && IsIconic(_hwnd) != FALSE; }
	bool win_window::is_maximized() const { return _hwnd && IsZoomed(_hwnd) != FALSE; }

	bool win_window::is_occluded() const
	{
		if (_options.offscreen && !is_child()) return !_hwnd || IsIconic(_hwnd) != FALSE;
		if (!_hwnd || !IsWindowVisible(_hwnd) || IsIconic(_hwnd)) return true;
		BOOL cloaked = FALSE;
		return SUCCEEDED(DwmGetWindowAttribute(_hwnd, DWMWA_CLOAKED, &cloaked, sizeof(cloaked))) && cloaked;
	}

	void win_window::sys_command(const platform::sys_command_type command)
	{
		if (!_hwnd) return;
		UINT value = 0;
		switch (command)
		{
		case platform::sys_command_type::MINIMIZE: value = SC_MINIMIZE; break;
		case platform::sys_command_type::MAXIMIZE: value = SC_MAXIMIZE; break;
		case platform::sys_command_type::RESTORE: value = SC_RESTORE; break;
		}
		if (value) PostMessageW(_hwnd, WM_SYSCOMMAND, value, 0);
	}

	void win_window::full_screen(const bool full)
	{
		if (!_hwnd || is_child() || full == _full_screen) return;
		_full_screen = full;
		if (full)
		{
			_restore_style = GetWindowLongPtrW(_hwnd, GWL_STYLE);
			_restore_ex_style = GetWindowLongPtrW(_hwnd, GWL_EXSTYLE);
			_restore_placement.length = sizeof(_restore_placement);
			GetWindowPlacement(_hwnd, &_restore_placement);
			const auto style = static_cast<DWORD>(_restore_style) & ~(WS_CAPTION | WS_THICKFRAME);
			const auto ex_style = static_cast<DWORD>(_restore_ex_style) &
				~(WS_EX_DLGMODALFRAME | WS_EX_WINDOWEDGE | WS_EX_CLIENTEDGE | WS_EX_STATICEDGE);
			SetWindowLongPtrW(_hwnd, GWL_STYLE, style);
			SetWindowLongPtrW(_hwnd, GWL_EXSTYLE, ex_style);

			const auto area = monitor_rect(bounds(), false);
			const auto monitor = monitor_rect(area, false);
			const auto work = monitor_rect(area, true);
			auto placement = _restore_placement;
			placement.showCmd = SW_SHOWNORMAL;
			placement.rcNormalPosition = rect_to(platform::screen_to_placement(area, monitor, work));
			SetWindowPlacement(_hwnd, &placement);
			SetWindowPos(_hwnd, nullptr, area.left, area.top, area.width(), area.height(),
			             SWP_NOZORDER | SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
		}
		else
		{
			SetWindowLongPtrW(_hwnd, GWL_STYLE, _restore_style);
			SetWindowLongPtrW(_hwnd, GWL_EXSTYLE, _restore_ex_style);
			SetWindowPlacement(_hwnd, &_restore_placement);
			SetWindowPos(_hwnd, nullptr, 0, 0, 0, 0,
			             SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_FRAMECHANGED);
		}
		reset_mouse_move_baseline();
		update_composition();
	}

	bool win_window::is_full_screen() const { return _full_screen; }
	bool win_window::presents_caption_button() const
	{
		if (!is_drawn_caption() || !_hwnd) return false;
		const auto style = static_cast<uint32_t>(GetWindowLongPtrW(_hwnd, GWL_STYLE));
		return platform::presents_maximize_caption_button(::windows_release(), style);
	}

	void win_window::set_text_input(const bool active, const core::recti caret)
	{
		if (!_hwnd) return;
		if (!active || caret.is_empty())
		{
			if (_ime_caret) DestroyCaret();
			_ime_caret = false;
			return;
		}
		if (!_ime_caret && GetFocus() == _hwnd)
		{
			_ime_caret = CreateCaret(_hwnd, nullptr, std::max(1, caret.width()), std::max(1, caret.height())) != FALSE;
		}
		if (_ime_caret) SetCaretPos(caret.left, caret.top);
		if (const auto context = ImmGetContext(_hwnd))
		{
			COMPOSITIONFORM composition{};
			composition.dwStyle = CFS_POINT;
			composition.ptCurrentPos = {caret.left, caret.top};
			ImmSetCompositionWindow(context, &composition);
			CANDIDATEFORM candidate{};
			candidate.dwIndex = 0;
			candidate.dwStyle = CFS_CANDIDATEPOS;
			candidate.ptCurrentPos = {caret.left, caret.bottom};
			ImmSetCandidateWindow(context, &candidate);
			ImmReleaseContext(_hwnd, context);
		}
	}

	void win_window::save_placement(const platform::setting_file_ptr& store, const std::string_view name)
	{
		if (!store || !_hwnd || is_child()) return;
		WINDOWPLACEMENT placement{sizeof(placement)};
		if (GetWindowPlacement(_hwnd, &placement))
		{
			store->write({}, name, core::cspan(reinterpret_cast<const uint8_t*>(&placement), sizeof(placement)));
		}
	}

	std::shared_ptr<platform::app> active_app()
	{
		return ::g_platform_app.lock();
	}

	LRESULT CALLBACK window_proc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam)
	{
		if (message == WM_NCCREATE)
		{
			const auto create = reinterpret_cast<const CREATESTRUCTW*>(lparam);
			if (create && create->lpCreateParams)
			{
				auto* window = static_cast<win_window*>(create->lpCreateParams);
				SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(window));
				window->_hwnd = hwnd;
			}
		}
		auto* window = reinterpret_cast<win_window*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
		if (window) return window->message(message, wparam, lparam);
		return DefWindowProcW(hwnd, message, wparam, lparam);
	}

	bool register_window_class(const wchar_t* name, const HICON icon)
	{
		resources.init();
		const auto instance = get_resource_instance ? get_resource_instance : GetModuleHandleW(nullptr);
		WNDCLASSEXW klass{sizeof(klass)};
		klass.style = CS_DBLCLKS;
		klass.lpfnWndProc = window_proc;
		klass.hInstance = instance;
		klass.hIcon = icon;
		klass.hCursor = resources.normal;
		klass.lpszClassName = name;
		if (RegisterClassExW(&klass) == 0 && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) return false;
		return true;
	}

	HWND parent_handle(const platform::window_options& options)
	{
		if (!options.parent) return nullptr;
		const auto handle = options.parent->native_handle();
		if (const auto value = std::any_cast<HWND>(&handle)) return *value;
		return nullptr;
	}

	bool create_native(win_window& window)
	{
		resources.init();
		const auto app_class = window._options.kind == platform::window_kind::app;
		const auto class_name = app_class ? L"LIBDESKTOP_MAIN" : L"LIBDESKTOP_FRAME";
		if (!register_window_class(class_name, app_class ? resources.app_icon_32 : nullptr)) return false;

		DWORD style = 0;
		DWORD ex_style = 0;
		if (app_class)
		{
			style = WS_CAPTION | WS_OVERLAPPED | WS_CLIPCHILDREN | WS_CLIPSIBLINGS | WS_SYSMENU;
			if (window._options.resizable) style |= WS_THICKFRAME | WS_MINIMIZEBOX | WS_MAXIMIZEBOX;
			ex_style = WS_EX_WINDOWEDGE | WS_EX_APPWINDOW;
		}
		else if (window._options.kind == platform::window_kind::dialog)
		{
			style = WS_CLIPCHILDREN | WS_CLIPSIBLINGS | WS_POPUP;
			if (!window._options.can_focus || window._options.layered) ex_style |= WS_EX_NOACTIVATE;
			if (window._options.layered) ex_style |= WS_EX_LAYERED;
		}
		else
		{
			style = window._options.kind == platform::window_kind::child && !window._options.layered ? WS_CHILD : WS_POPUP;
			if (window._options.can_focus && !window._options.menu) style |= WS_TABSTOP;
			if (window._options.menu || !window._options.can_focus || window._options.layered) ex_style |= WS_EX_NOACTIVATE;
			if (window._options.layered) ex_style |= WS_EX_LAYERED;
		}

		const auto parent = parent_handle(window._options);
		const auto bounds = window._options.bounds;
		const auto has_client_size = bounds.is_empty() && !window._options.client_size.is_empty();
		int x = bounds.left;
		int y = bounds.top;
		int cx = bounds.width();
		int cy = bounds.height();
		if (bounds.is_empty())
		{
			if (window._options.kind == platform::window_kind::child)
			{
				x = 0;
				y = 0;
				cx = 0;
				cy = 0;
			}
			else
			{
				x = CW_USEDEFAULT;
				y = CW_USEDEFAULT;
				cx = CW_USEDEFAULT;
				cy = CW_USEDEFAULT;
			}
			if (has_client_size)
			{
				const auto size = window_size_for_client(window._options.client_size, style, ex_style, window_dpi(nullptr));
				cx = size.cx;
				cy = size.cy;
			}
		}
		if (window._options.offscreen && window._options.kind != platform::window_kind::child &&
		    (has_client_size || !bounds.is_empty()))
		{
			x = -32000;
			y = -32000;
		}
		const auto instance = get_resource_instance ? get_resource_instance : GetModuleHandleW(nullptr);
		const auto title = core::utf8_to_utf16(window._options.title);
		const auto created = CreateWindowExW(ex_style, class_name, title.c_str(), style, x, y, cx, cy,
		                                    parent, nullptr, instance, &window);
		if (!created) return false;
		if (has_client_size)
		{
			const auto size = window_size_for_client(window._options.client_size, style, ex_style, window_dpi(created));
			RECT current{};
			if (GetWindowRect(created, &current) &&
				(current.right - current.left != size.cx || current.bottom - current.top != size.cy))
			{
				int left = current.left;
				int top = current.top;
				if (window._options.kind == platform::window_kind::child && parent)
				{
					POINT point{left, top};
					ScreenToClient(parent, &point);
					left = point.x;
					top = point.y;
				}
				const auto insert_after = window._options.offscreen ? HWND_BOTTOM : nullptr;
				UINT flags = SWP_NOACTIVATE | SWP_NOCOPYBITS;
				if (!window._options.offscreen) flags |= SWP_NOZORDER;
				SetWindowPos(created, insert_after, left, top, size.cx, size.cy, flags);
			}
		}
		if (window._options.accepts_drops && FAILED(RegisterDragDrop(created, &window)))
			window._options.accepts_drops = false;
		if (app_class) app_windows.emplace_back(window.weak_from_this());
		window.show(window._options.offscreen ? true : window._options.visible);
		return true;
	}

	void show_native_app_windows()
	{
		std::erase_if(app_windows, [](const auto& weak) { return weak.expired(); });
		for (const auto& weak : app_windows)
		{
			const auto window = weak.lock();
			if (!window || !window->_hwnd || !IsWindow(window->_hwnd)) continue;
			if (window->_options.offscreen)
			{
				window->show(true);
			}
			else
			{
				// A window restored maximized stays hidden until ShowWindow maximizes it, so it is never
				// seen at its restored size first.
				const auto maximized = window->_options.maximized;
				SetWindowPos(window->_hwnd, nullptr, 0, 0, 0, 0,
				             SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_FRAMECHANGED | (maximized ? 0 : SWP_SHOWWINDOW));
				ShowWindow(window->_hwnd, maximized ? SW_SHOWMAXIMIZED : SW_SHOWNORMAL);
			}
			RedrawWindow(window->_hwnd, nullptr, nullptr, RDW_INVALIDATE | RDW_ALLCHILDREN | RDW_UPDATENOW);
		}
	}

	void defer_native_window_moves(const std::span<const platform::window_move> moves)
	{
		struct pending
		{
			win_window* window = nullptr;
			HWND hwnd = nullptr;
			HWND parent = nullptr;
			core::recti bounds;
			core::recti old_bounds;
			bool visible = false;
			bool was_visible = false;
		};

		std::vector<pending> pending_moves;
		pending_moves.reserve(moves.size());
		for (const auto& move : moves)
		{
			auto* window = dynamic_cast<win_window*>(move.target);
			if (!window || !window->_hwnd || !IsWindow(window->_hwnd)) continue;
			const auto old_bounds = window->bounds();
			const auto was_visible = window->is_visible();
			if (old_bounds == move.bounds && was_visible == move.visible) continue;
			pending_moves.push_back({window, window->_hwnd, GetParent(window->_hwnd), move.bounds, old_bounds,
			                         move.visible, was_visible});
		}
		if (pending_moves.empty()) return;

		for (size_t first = 0; first < pending_moves.size();)
		{
			const auto parent = pending_moves[first].parent;
			size_t last = first + 1;
			while (last < pending_moves.size() && pending_moves[last].parent == parent) ++last;
			const auto count = static_cast<int>(last - first);
			HDWP hdwp = BeginDeferWindowPos(count);
			if (hdwp)
			{
				for (size_t i = first; i < last && hdwp; ++i)
				{
					const auto& move = pending_moves[i];
					UINT flags = SWP_NOACTIVATE | SWP_NOZORDER | SWP_NOCOPYBITS | SWP_NOREDRAW;
					if (move.visible != move.was_visible) flags |= move.visible ? SWP_SHOWWINDOW : SWP_HIDEWINDOW;
					hdwp = DeferWindowPos(hdwp, move.hwnd, nullptr, move.bounds.left, move.bounds.top,
					                      move.bounds.width(), move.bounds.height(), flags);
				}
				if (hdwp) EndDeferWindowPos(hdwp);
			}
			if (!hdwp)
			{
				for (size_t i = first; i < last; ++i)
				{
					const auto& move = pending_moves[i];
					UINT flags = SWP_NOACTIVATE | SWP_NOZORDER | SWP_NOCOPYBITS | SWP_NOREDRAW;
					if (move.visible != move.was_visible) flags |= move.visible ? SWP_SHOWWINDOW : SWP_HIDEWINDOW;
					SetWindowPos(move.hwnd, nullptr, move.bounds.left, move.bounds.top, move.bounds.width(),
					             move.bounds.height(), flags);
				}
			}

			core::recti vacated;
			for (size_t i = first; i < last; ++i)
			{
				const auto& move = pending_moves[i];
				if (move.visible) RedrawWindow(move.hwnd, nullptr, nullptr, RDW_INVALIDATE | RDW_ERASE | RDW_FRAME);
				if (move.parent && move.was_visible && !move.old_bounds.is_empty())
					vacated = vacated.make_union(move.old_bounds);
			}
			if (parent && !vacated.is_empty())
			{
				const auto rect = rect_to(vacated);
				RedrawWindow(parent, &rect, nullptr, RDW_INVALIDATE);
			}
			first = last;
		}
	}
}

bool platform::child_windows()
{
	return true;
}

void show_app_windows()
{
	show_native_app_windows();
}

void platform::defer_window_moves(const std::span<const platform::window_move> moves)
{
	defer_native_window_moves(moves);
}

platform::window_ptr platform::create_window(const window_options& options,
                                             std::weak_ptr<window_events> events)
{
	if (!platform::is_ui_thread()) return nullptr;
	try
	{
		auto result = std::make_shared<win_window>(options, std::move(events));
		if (!create_native(*result)) return nullptr;
		return result;
	}
	catch (...)
	{
		return nullptr;
	}
}

core::recti platform::monitor_work_area(const core::recti bounds)
{
	return monitor_rect(bounds, true);
}

std::string_view platform::placement_setting_name()
{
	return "window_rect";
}

std::optional<platform::window_placement> platform::read_placement(
	const platform::setting_file_ptr& store, const std::string_view name)
{
	if (!store) return std::nullopt;
	WINDOWPLACEMENT placement{sizeof(placement)};
	size_t length = sizeof(placement);
	if (!store->read({}, name.empty() ? placement_setting_name() : name,
	                reinterpret_cast<uint8_t*>(&placement), length) || length != sizeof(placement))
		return std::nullopt;
	if (placement.showCmd != SW_SHOW && placement.showCmd != SW_SHOWNORMAL &&
		placement.showCmd != SW_SHOWMAXIMIZED) placement.showCmd = SW_SHOWNORMAL;
	if (IsRectEmpty(&placement.rcNormalPosition)) return std::nullopt;

	const auto nearest = MonitorFromRect(&placement.rcNormalPosition, MONITOR_DEFAULTTONEAREST);
	MONITORINFO info{sizeof(info)};
	if (!nearest || !GetMonitorInfoW(nearest, &info)) return std::nullopt;
	const auto monitor = rect_from(info.rcMonitor);
	const auto work = rect_from(info.rcWork);
	const auto saved = platform::placement_to_screen(rect_from(placement.rcNormalPosition), monitor, work);
	const auto saved_rect = rect_to(saved);
	const auto reaches_a_display = MonitorFromRect(&saved_rect, MONITOR_DEFAULTTONULL) != nullptr;
	const auto fitted = platform::window_needs_refit(saved, work, reaches_a_display)
		? platform::fit_window_to_work_area(saved, work)
		: saved;
	return platform::window_placement{fitted, placement.showCmd == SW_SHOWMAXIMIZED};
}

void sync_app_window_enabled()
{
	const auto hwnd = app_wnd();
	if (!hwnd) return;
	auto* window = reinterpret_cast<win_window*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
	if (window) window->sync_enabled();
}
platform::window* platform_window_from_native(const HWND hwnd)
{
	if (!hwnd) return nullptr;
	auto* window = reinterpret_cast<win_window*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
	return window;
}

namespace platform
{
	core::recti fit_window_to_work_area(const core::recti saved, const core::recti work_area)
	{
		const auto width = std::min(saved.width(), work_area.width());
		const auto height = std::min(saved.height(), work_area.height());
		const auto x = std::clamp(saved.left, work_area.left, std::max(work_area.left, work_area.right - width));
		const auto y = std::clamp(saved.top, work_area.top, std::max(work_area.top, work_area.bottom - height));
		return {x, y, x + width, y + height};
	}

	core::recti placement_to_screen(const core::recti saved, const core::recti monitor,
	                               const core::recti work_area)
	{
		const auto x = work_area.left - monitor.left;
		const auto y = work_area.top - monitor.top;
		return {saved.left + x, saved.top + y, saved.right + x, saved.bottom + y};
	}

	core::recti screen_to_placement(const core::recti screen, const core::recti monitor,
	                               const core::recti work_area)
	{
		const auto x = work_area.left - monitor.left;
		const auto y = work_area.top - monitor.top;
		return {screen.left - x, screen.top - y, screen.right - x, screen.bottom - y};
	}

	bool window_needs_refit(const core::recti saved, const core::recti work_area, const bool reaches_a_display)
	{
		if (!reaches_a_display) return true;
		if (saved.width() > work_area.width() || saved.height() > work_area.height()) return true;

		constexpr auto min_visible_cx = 96;
		constexpr auto min_visible_cy = 32;
		const auto overlap_cx = std::min(saved.right, work_area.right) - std::max(saved.left, work_area.left);
		const auto overlap_cy = std::min(saved.bottom, work_area.bottom) - std::max(saved.top, work_area.top);
		return overlap_cx < min_visible_cx || overlap_cy < min_visible_cy;
	}

	bool can_present_maximize_caption_button(const os_release release, const uint32_t root_style)
	{
		const auto has_snap_layouts = release == os_release::windows_11 || release == os_release::windows_later;
		constexpr uint32_t resizable_with_maximize = WS_THICKFRAME | WS_MAXIMIZEBOX;
		return has_snap_layouts && (root_style & resizable_with_maximize) == resizable_with_maximize;
	}

	bool presents_maximize_caption_button(const os_release release, const uint32_t root_style)
	{
		return can_present_maximize_caption_button(release, root_style) && (root_style & WS_MAXIMIZE) == 0;
	}
}

namespace
{
	// A native-only probe keeps the platform test seam independent of the portable control tree.
	class caption_probe_events final : public platform::window_events
	{
		const core::recti _maximize{100, 10, 140, 50};
		bool _pressed = false;
		int _hover = 0;
		int _invocations = 0;

		bool in_maximize(const core::pointi loc) const
		{
			return !_maximize.intersection({loc.x, loc.y, loc.x + 1, loc.y + 1}).is_empty();
		}

	public:
		platform::hit_region hit_test(const core::pointi loc) override
		{
			return in_maximize(loc) ? platform::hit_region::maximize_button : platform::hit_region::client;
		}

		void caption_button(const platform::caption_button_action action, const core::pointi loc) override
		{
			switch (action)
			{
			case platform::caption_button_action::hover: _hover = in_maximize(loc) ? 1 : 0; break;
			case platform::caption_button_action::leave: _hover = 0; break;
			case platform::caption_button_action::press: _pressed = in_maximize(loc); break;
			case platform::caption_button_action::release:
				if (_pressed && in_maximize(loc)) ++_invocations;
				_pressed = false;
				break;
			}
		}

		int hover() const { return _hover; }
		int invocations() const { return _invocations; }
	};
}

platform::caption_button_probe platform::probe_toolbar_caption_button()
{
	caption_button_probe result;
	const auto events = std::make_shared<caption_probe_events>();
	window_options options;
	options.kind = window_kind::app;
	options.bounds = {0, 0, 400, 200};
	options.drawn_caption = true;
	options.resizable = true;
	options.visible = false;

	const auto window = create_window(options, events);
	if (!window) return result;

	result.offered = window->presents_caption_button();
	const auto native = std::any_cast<HWND>(window->native_handle());
	const auto screen_point = [native](const core::pointi client)
	{
		POINT point{client.x, client.y};
		ClientToScreen(native, &point);
		return MAKELPARAM(point.x, point.y);
	};
	const auto send_hit = [native, &screen_point](const core::pointi client)
	{
		return static_cast<int>(SendMessageW(native, WM_NCHITTEST, 0, screen_point(client)));
	};

	result.maximize_hit = send_hit({120, 30});
	result.other_hit = send_hit({40, 30});

	if (result.offered)
	{
		const auto point = screen_point({120, 30});
		SendMessageW(native, WM_NCMOUSEMOVE, HTMAXBUTTON, point);
		result.hovered_id = events->hover();
		SendMessageW(native, WM_NCLBUTTONDOWN, HTMAXBUTTON, point);
		SendMessageW(native, WM_NCLBUTTONUP, HTMAXBUTTON, point);
		result.maximize_id = 1;
		result.invocations = events->invocations();
		SendMessageW(native, WM_NCMOUSELEAVE, 0, 0);
		result.hover_after_leave = events->hover();
	}

	const auto style = GetWindowLongPtrW(native, GWL_STYLE);
	SetWindowLongPtrW(native, GWL_STYLE, style | WS_MAXIMIZE);
	SetWindowPos(native, nullptr, 0, 0, 0, 0,
	             SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_FRAMECHANGED);
	result.maximized_hit = send_hit({120, 30});

	SetWindowLongPtrW(native, GWL_STYLE, style & ~(WS_CAPTION | WS_THICKFRAME));
	SetWindowPos(native, nullptr, 0, 0, 0, 0,
	             SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_FRAMECHANGED);
	result.frameless_hit = send_hit({120, 30});
	return result;
}

#if defined(_DEBUG)
core::surface_ptr platform::capture_window_surface(const std::any& window_handle)
{
	const auto hwnd = std::any_cast<HWND>(window_handle);
	RECT bounds{};
	if (!hwnd || !GetWindowRect(hwnd, &bounds)) return nullptr;

	const auto width = bounds.right - bounds.left;
	const auto height = bounds.bottom - bounds.top;
	if (width < 1 || height < 1) return nullptr;
	const auto failed = [](const std::string_view step) -> core::surface_ptr
	{
		core::log(__FUNCTION__, std::format("Window capture {} failed: {}", step, GetLastError()));
		return {};
	};

	const auto screen_dc = GetDC(nullptr);
	if (!screen_dc) return failed("GetDC");
	const core::scope_exit release_screen([screen_dc] { ReleaseDC(nullptr, screen_dc); });
	const auto memory_dc = CreateCompatibleDC(screen_dc);
	if (!memory_dc) return failed("CreateCompatibleDC");
	const core::scope_exit release_memory([memory_dc] { DeleteDC(memory_dc); });
	const auto bitmap = CreateCompatibleBitmap(screen_dc, width, height);
	if (!bitmap) return failed("CreateCompatibleBitmap");
	const core::scope_exit release_bitmap([bitmap] { DeleteObject(bitmap); });
	const auto previous = SelectObject(memory_dc, bitmap);
	if (!previous || previous == HGDI_ERROR) return failed("SelectObject");
	const auto captured = BitBlt(memory_dc, 0, 0, width, height, screen_dc, bounds.left, bounds.top,
	                             SRCCOPY | CAPTUREBLT) != FALSE;
	const auto capture_error = captured ? ERROR_SUCCESS : GetLastError();
	SelectObject(memory_dc, previous);
	if (!captured)
	{
		core::log(__FUNCTION__, std::format("Window capture BitBlt failed: {}", capture_error));
		return nullptr;
	}

	auto surface = std::make_shared<core::surface>();
	if (!surface->alloc(width, height, core::texture_format::ARGB, core::orientation::top_left))
	{
		core::log(__FUNCTION__, "Cannot allocate window capture surface");
		return nullptr;
	}
	BITMAPINFO bitmap_info{};
	bitmap_info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
	bitmap_info.bmiHeader.biWidth = width;
	bitmap_info.bmiHeader.biHeight = -height;
	bitmap_info.bmiHeader.biPlanes = 1;
	bitmap_info.bmiHeader.biBitCount = 32;
	bitmap_info.bmiHeader.biCompression = BI_RGB;
	const auto row_bytes = static_cast<size_t>(width) * 4;
	std::vector<uint8_t> bitmap_pixels(row_bytes * height);
	const auto copied = GetDIBits(memory_dc, bitmap, 0, height, bitmap_pixels.data(), &bitmap_info, DIB_RGB_COLORS) == height;
	if (!copied) return failed("GetDIBits");
	for (auto y = 0; y < height; ++y)
	{
		memcpy_s(surface->pixels_line(y), surface->stride(), bitmap_pixels.data() + y * row_bytes, row_bytes);
		auto* pixels = std::bit_cast<core::color32*>(surface->pixels_line(y));
		for (auto x = 0; x < width; ++x) pixels[x] |= 0xff000000u;
	}
	return surface;
}
#endif
