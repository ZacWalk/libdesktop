// platform_win.cpp â€” Win32 platform layer: entry point, windowing, timers,
// resources, menus, device context, file dialogs, and spell checking

#include "platform.h"

#include <algorithm>
#include <cassert>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <format>
#include <fstream>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <vector>

#define WIN32_LEAN_AND_MEAN
#define ISOLATION_AWARE_ENABLED 1
#define NOMINMAX
#include <windows.h>
#include <commdlg.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shlwapi.h>
#include <sddl.h>
#include <commctrl.h>
#include <objidl.h>
#include <winhttp.h>
#include <spellcheck.h>

#include <iostream>

#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "comdlg32.lib")
#pragma comment(lib, "shlwapi.lib")
#pragma comment(lib, "msimg32.lib")
#pragma comment(lib, "winhttp.lib")

#include <functional>
#include <map>
#include <vector>
#include <string>
#include <optional>

#include "platform.h"
#include "libdesktop/os.h"

using namespace std::string_view_literals;

// pf::irect and RECT have identical binary layout (4 x int32_t)
inline RECT& as_rect(pf::irect& r) { return reinterpret_cast<RECT&>(r); }
inline const RECT& as_rect(const pf::irect& r) { return reinterpret_cast<const RECT&>(r); }

// Shared clipboard helpers used by both win_impl and the free pf:: functions.
static std::string clipboard_get_text(HWND owner)
{
	return os::read_clipboard_text(reinterpret_cast<uintptr_t>(owner)).text.value_or(std::string());
}

static bool clipboard_set_text(HWND owner, std::string_view text)
{
	return os::write_clipboard_text(reinterpret_cast<uintptr_t>(owner), text) == os::clipboard_write::ok;
}

constexpr uint32_t xrgb(const uint32_t r, const uint32_t g, const uint32_t b)
{
	return (r & 0xff) | ((g & 0xff) << 8) | ((b & 0xff) << 16) | 0xff000000;
}


bool pf::file_path::exists() const
{
	const auto attributes = os::query_file(_path);
	return attributes.exists() && !attributes.directory;
}

pf::file_path pf::file_path::module_folder()
{
	return module_path().folder();
}

pf::file_path pf::file_path::module_path()
{
	wchar_t raw_path[MAX_PATH];
	GetModuleFileNameW(nullptr, raw_path, MAX_PATH);
	return file_path(utf16_to_utf8(raw_path));
}

pf::file_path pf::local_app_data_path()
{
	uint32_t status = 0;
	const auto path = os::known_folder_path(os::known_folder::local_app_data, false, &status);
	if (path.empty())
	{
		debug_trace(std::format("SHGetKnownFolderPath(LocalAppData) failed ({:#x})", status));
		return {};
	}
	return file_path(path);
}

pf::file_path pf::roaming_app_data_path()
{
	uint32_t status = 0;
	const auto path = os::known_folder_path(os::known_folder::roaming_app_data, false, &status);
	if (path.empty())
	{
		debug_trace(std::format("SHGetKnownFolderPath(RoamingAppData) failed ({:#x})", status));
		return {};
	}
	return file_path(path);
}

static pf::file_operation_result file_failure(DWORD error, std::string message = {});

std::optional<double> pf::file_age_seconds(const file_path& path)
{
	const auto attributes = os::query_file(path.view());
	if (!attributes.exists()) return {};

	FILETIME now = {};
	GetSystemTimeAsFileTime(&now);
	const auto current = (static_cast<uint64_t>(now.dwHighDateTime) << 32) | now.dwLowDateTime;
	if (current <= attributes.modified) return 0.0;
	return static_cast<double>(current - attributes.modified) / 10'000'000.0;
}

pf::file_query_result pf::query_file(const file_path& path)
{
	const auto source = os::query_file(path.view());
	file_query_result result;
	result.attributes.is_readonly = source.readonly;
	result.attributes.is_offline = source.offline;
	result.attributes.is_hidden = source.hidden;
	result.attributes.is_reparse = source.reparse;
	result.attributes.modified = source.modified;
	result.attributes.created = source.created;
	result.attributes.size = source.size;
	result.attributes.identity.volume = source.identity.volume;
	result.attributes.identity.index = source.identity.index;
	result.attributes.identity.valid = source.identity.valid;
	if (source.exists())
	{
		result.status = file_query_status::found;
		return result;
	}

	result.status = source.confirmed_missing() ? file_query_status::no_such_file : file_query_status::failed;
	result.error = file_failure(source.error);
	return result;
}

namespace
{
	struct win_instance_lock final : pf::instance_lock
	{
		HANDLE handle = nullptr;
		~win_instance_lock() override
		{
			if (handle) CloseHandle(handle);
		}
	};

	pf::instance_lock_result instance_lock_error(const std::string_view operation, const DWORD error)
	{
		auto message = std::format("{} failed (error {})", operation, error);
		pf::debug_trace(message);
		return {nullptr, false, std::move(message)};
	}
}

pf::instance_lock_result pf::try_lock_instance(const std::string_view name)
{
	if (name.empty() || name.size() > 128 ||
		!std::all_of(name.begin(), name.end(), [](const char c)
		{
			return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
				(c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-';
		}))
		return instance_lock_error("Instance lock name validation", ERROR_INVALID_NAME);

	HANDLE raw_token = nullptr;
	if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &raw_token))
		return instance_lock_error("OpenProcessToken", GetLastError());
	const auto close_token = [](void* handle) { CloseHandle(handle); };
	const std::unique_ptr<void, decltype(close_token)> token(raw_token, close_token);

	DWORD bytes = 0;
	if (!GetTokenInformation(token.get(), TokenUser, nullptr, 0, &bytes))
	{
		const auto error = GetLastError();
		if (error != ERROR_INSUFFICIENT_BUFFER)
			return instance_lock_error("GetTokenInformation(size)", error);
	}
	if (bytes < sizeof(TOKEN_USER))
		return instance_lock_error("GetTokenInformation(size)", ERROR_INVALID_DATA);
	std::vector<uint8_t> user(bytes);
	if (!GetTokenInformation(token.get(), TokenUser, user.data(), bytes, &bytes))
		return instance_lock_error("GetTokenInformation", GetLastError());

	PWSTR raw_sid = nullptr;
	if (!ConvertSidToStringSidW(reinterpret_cast<const TOKEN_USER*>(user.data())->User.Sid, &raw_sid))
		return instance_lock_error("ConvertSidToStringSid", GetLastError());
	const auto free_sid = [](wchar_t* sid) { LocalFree(sid); };
	const std::unique_ptr<wchar_t, decltype(free_sid)> sid(raw_sid, free_sid);
	const auto qualified_name = std::wstring(L"Global\\pf.instance.") + sid.get() + L"." + utf8_to_utf16(name);

	auto lock = std::make_unique<win_instance_lock>();
	// Object lifetime, not thread ownership, is the lock; a second handle must never escape.
	SetLastError(ERROR_SUCCESS);
	lock->handle = CreateMutexW(nullptr, FALSE, qualified_name.c_str());
	const auto error = GetLastError();
	if (!lock->handle)
		return instance_lock_error("CreateMutex", error);
	if (error == ERROR_ALREADY_EXISTS)
		return {nullptr, true, {}};
	return {std::move(lock), false, {}};
}

pf::instance_lock_result pf::try_lock_named_instance(const std::string_view name)
{
	if (name.empty() || name.size() > 256 || name.find('\0') != std::string_view::npos)
		return instance_lock_error("Named instance lock validation", ERROR_INVALID_NAME);

	auto lock = std::make_unique<win_instance_lock>();
	SetLastError(ERROR_SUCCESS);
	lock->handle = CreateMutexW(nullptr, FALSE, utf8_to_utf16(name).c_str());
	const auto error = GetLastError();
	if (!lock->handle) return instance_lock_error("CreateMutex", error);
	if (error == ERROR_ALREADY_EXISTS) return {nullptr, true, {}};
	return {std::move(lock), false, {}};
}

pf::file_path tmp_folder()
{
	wchar_t raw_path[MAX_PATH];
	//SHGetSpecialFolderPathW(GetActiveWindow(), raw_path, CSIDL_LOCAL_APPDATA | CSIDL_FLAG_CREATE, TRUE);
	GetTempPathW(MAX_PATH, raw_path);
	const auto result = pf::utf16_to_utf8(raw_path);
	return pf::file_path{result};
}

//  Globals 

static HINSTANCE resource_instance = nullptr;
HWND g_hWnd = nullptr; // shared with platform_win_audio.cpp
static LARGE_INTEGER g_perfFreq;
static LARGE_INTEGER g_perfStart;
static HMENU g_hMenu = nullptr;
static HACCEL g_hAccel = nullptr;
static std::vector<pf::menu_command> g_menuDef;
static int g_nCmdShow = SW_SHOW;
static std::atomic_bool g_test_tray_publication_failure = false;
static std::unordered_map<UINT, std::function<void()>> g_instance_signals;
static std::atomic<pf::atomic_file_test_failure> g_test_atomic_file_failure{
	pf::atomic_file_test_failure::none};

static pf::file_operation_result file_failure(const DWORD error, std::string message)
{
	pf::file_operation_result result;
	result.native_code = error;
	result.code = error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND
		              ? pf::file_error::no_such_file
		              : error == ERROR_FILE_EXISTS || error == ERROR_ALREADY_EXISTS
		                    ? pf::file_error::already_exists
		                    : error == ERROR_ACCESS_DENIED
		                          ? pf::file_error::access_denied
		                          : error == ERROR_SHARING_VIOLATION || error == ERROR_LOCK_VIOLATION
		                                ? pf::file_error::sharing_violation
		                                : error == ERROR_INVALID_NAME || error == ERROR_BAD_PATHNAME ||
		                                      error == ERROR_FILENAME_EXCED_RANGE
		                                      ? pf::file_error::invalid_path
		                                      : pf::file_error::io_error;
	if (message.empty())
	{
		SetLastError(error);
		message = pf::platform_last_error_message();
	}
	if (message.empty()) message = std::format("Windows error {}", error);
	result.message = std::move(message);
	return result;
}

static pf::file_operation_result os_failure(const os::file_operation& operation)
{
	return file_failure(operation.error, operation.diagnostic);
}

static UINT instance_signal_message(const std::string_view name)
{
	if (name.empty() || name.size() > 128 ||
		!std::all_of(name.begin(), name.end(), [](const char c)
		{
			return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
				(c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-';
		}))
		return 0;
	const auto message_name = std::wstring(L"libdesktop.instance.") + pf::utf8_to_utf16(name);
	return RegisterWindowMessageW(message_name.c_str());
}

// Look up a pf::menu_command by ID and invoke its action
static bool dispatch_menu_command(const std::vector<pf::menu_command>& items, const int cmd_id)
{
	for (const auto& item : items)
	{
		if (item.id == cmd_id && item.action)
		{
			if (item.is_enabled && !item.is_enabled())
				return true; // consumed but not executed
			item.action();
			return true;
		}
		if (!item.children.empty() && dispatch_menu_command(item.children, cmd_id))
			return true;
	}
	return false;
}

static bool dispatch_instance_signal(const UINT message)
{
	const auto it = g_instance_signals.find(message);
	if (it == g_instance_signals.end()) return false;
	if (it->second) it->second();
	return true;
}

bool pf::register_instance_signal(const std::string_view name, std::function<void()> callback)
{
	const auto message = instance_signal_message(name);
	if (!message || !callback) return false;
	g_instance_signals[message] = std::move(callback);
	return true;
}

bool pf::signal_existing_instance(const std::string_view name)
{
	const auto message = instance_signal_message(name);
	return message != 0 && PostMessageW(HWND_BROADCAST, message, 0, 0) != FALSE;
}

// Find pf::menu_command by ID in the global menu definition tree
static const pf::menu_command* find_menu_command(const std::vector<pf::menu_command>& items,
                                                 const int cmd_id)
{
	for (const auto& item : items)
	{
		if (item.id == cmd_id)
			return &item;
		if (!item.children.empty())
		{
			if (const auto* found = find_menu_command(item.children, cmd_id))
				return found;
		}
	}
	return nullptr;
}

// Apply is_enabled/is_checked from pf::menu_command callbacks to HMENU items
static void apply_menu_state(const HMENU hMenu)
{
	const auto count = GetMenuItemCount(hMenu);
	for (int i = 0; i < count; i++)
	{
		const auto id = GetMenuItemID(hMenu, i);
		if (id == static_cast<UINT>(-1))
			continue; // separator or submenu

		if (const auto* cmd = find_menu_command(g_menuDef, static_cast<int>(id)))
		{
			if (cmd->is_enabled)
				EnableMenuItem(hMenu, i, MF_BYPOSITION | (cmd->is_enabled() ? MF_ENABLED : MF_DISABLED));
			if (cmd->is_checked)
				CheckMenuItem(hMenu, i, MF_BYPOSITION | (cmd->is_checked() ? MF_CHECKED : MF_UNCHECKED));
		}
	}
}

// Build a Win32 accelerator table at runtime from menu key bindings
static void build_runtime_accelerators()
{
	std::vector<ACCEL> accels;

	const auto add = [&accels](const pf::key_binding& kb, const int id)
	{
		if (kb.empty() || id == 0) return;

		ACCEL a = {};
		a.cmd = static_cast<WORD>(id);
		a.fVirt = FVIRTKEY | FNOINVERT;
		if (kb.modifiers & pf::key_mod::ctrl) a.fVirt |= FCONTROL;
		if (kb.modifiers & pf::key_mod::shift) a.fVirt |= FSHIFT;
		if (kb.modifiers & pf::key_mod::alt) a.fVirt |= FALT;
		a.key = static_cast<WORD>(kb.key);
		accels.push_back(a);
	};

	std::function<void(const std::vector<pf::menu_command>&)> collect;
	collect = [&](const std::vector<pf::menu_command>& items)
	{
		for (const auto& item : items)
		{
			add(item.accel, item.id);
			add(item.accel_alt, item.id);

			if (!item.children.empty())
				collect(item.children);
		}
	};

	collect(g_menuDef);

	if (g_hAccel)
	{
		DestroyAcceleratorTable(g_hAccel);
		g_hAccel = nullptr;
	}

	if (!accels.empty())
		g_hAccel = CreateAcceleratorTable(accels.data(), static_cast<int>(accels.size()));
}

class win
{
public:
	HWND m_hWnd = nullptr;

	pf::irect get_client_rect() const
	{
		pf::irect result;
		GetClientRect(m_hWnd, &as_rect(result));
		return result;
	}

	void move_window(const pf::irect& bounds) const
	{
		MoveWindow(m_hWnd, bounds.left, bounds.top, bounds.width(), bounds.height(), TRUE);
	}


	void create_control(const LPCWSTR class_name, const HWND parent, const uint32_t style,
	                    const uint32_t exstyle = 0,
	                    const uintptr_t id = 0)
	{
		m_hWnd = CreateWindowEx(
			exstyle,
			class_name,
			nullptr,
			style,
			0, 0, 0, 0,
			parent,
			std::bit_cast<HMENU>(id),
			resource_instance,
			this);
	}
};

// Map Win32 WM_* to pf::message_type (non-mouse, non-keyboard messages)
static std::optional<pf::message_type> map_message(const UINT uMsg)
{
	switch (uMsg)
	{
	case WM_CREATE: return pf::message_type::create;
	case WM_DESTROY: return pf::message_type::destroy;
	case WM_SETFOCUS: return pf::message_type::set_focus;
	case WM_KILLFOCUS: return pf::message_type::kill_focus;
	case WM_ERASEBKGND: return pf::message_type::erase_background;
	case WM_TIMER: return pf::message_type::timer;
	case WM_SYSCOLORCHANGE: return pf::message_type::sys_color_change;
	case WM_COMMAND: return pf::message_type::command;
	case WM_CLOSE: return pf::message_type::close;
	case WM_DPICHANGED: return pf::message_type::dpi_changed;
	case WM_INITDIALOG: return pf::message_type::init_dialog;
	case WM_DROPFILES: return pf::message_type::drop_files;
	case WM_CAPTURECHANGED: return pf::message_type::capture_changed;
	case WM_ENDSESSION: return pf::message_type::end_session;
	default: return std::nullopt;
	}
}

// Map Win32 WM_* to pf::keyboard_message_type
static std::optional<pf::keyboard_message_type> map_keyboard_message(const UINT uMsg)
{
	switch (uMsg)
	{
	case WM_KEYDOWN: return pf::keyboard_message_type::key_down;
	case WM_KEYUP: return pf::keyboard_message_type::key_up;
	case WM_CHAR: return pf::keyboard_message_type::char_input;
	default: return std::nullopt;
	}
}

// Map Win32 WM_* to pf::mouse_message_type
static std::optional<pf::mouse_message_type> map_mouse_message(const UINT uMsg)
{
	switch (uMsg)
	{
	case WM_LBUTTONDBLCLK: return pf::mouse_message_type::left_button_dbl_clk;
	case WM_LBUTTONDOWN: return pf::mouse_message_type::left_button_down;
	case WM_RBUTTONDOWN: return pf::mouse_message_type::right_button_down;
	case WM_RBUTTONUP: return pf::mouse_message_type::right_button_up;
	case WM_MBUTTONDOWN: return pf::mouse_message_type::middle_button_down;
	case WM_MBUTTONUP: return pf::mouse_message_type::middle_button_up;
	case WM_LBUTTONUP: return pf::mouse_message_type::left_button_up;
	case WM_MOUSEMOVE: return pf::mouse_message_type::mouse_move;
	case WM_MOUSEWHEEL: return pf::mouse_message_type::mouse_wheel;
	case WM_MOUSELEAVE: return pf::mouse_message_type::mouse_leave;
	case WM_MOUSEACTIVATE: return pf::mouse_message_type::mouse_activate;
	case WM_CONTEXTMENU: return pf::mouse_message_type::context_menu;
	case WM_SETCURSOR: return pf::mouse_message_type::set_cursor;
	default: return std::nullopt;
	}
}

// Map platform window styles to Win32 WS_* styles
static DWORD map_window_style(const uint32_t style)
{
	DWORD ws = 0;
	if (style & pf::window_style::child) ws |= WS_CHILD;
	if (style & pf::window_style::visible) ws |= WS_VISIBLE;
	if (style & pf::window_style::clip_children) ws |= WS_CLIPCHILDREN;
	return ws;
}

static DWORD map_window_ex_style(const uint32_t style)
{
	DWORD ws = 0;
	if (style & pf::window_style::composited) ws |= WS_EX_COMPOSITED;
	return ws;
}

// Map font_name enum to Win32 font family name
static const wchar_t* map_font_name(const pf::font_name name)
{
	switch (name)
	{
	case pf::font_name::consolas: return L"Consolas";
	case pf::font_name::arial: return L"Arial";
	case pf::font_name::calibri: return L"Calibri";
	case pf::font_name::segoe_icons: return L"Segoe Fluent Icons";
	}
	return L"Consolas";
}

static HFONT create_platform_font(const pf::font& f)
{
	const auto family = map_font_name(f.name);
	const bool is_icon = f.name == pf::font_name::segoe_icons;
	return ::CreateFont(
		f.size_mode == pf::font_size_mode::cell ? f.size : -f.size,
		0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
		DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
		CLEARTYPE_QUALITY,
		is_icon ? (DEFAULT_PITCH | FF_DONTCARE) : (FIXED_PITCH | FF_MODERN),
		family);
}

// Font cache — keyed on (size, name, size mode), HFONTs live until process exit
static std::map<std::tuple<int, pf::font_name, pf::font_size_mode>, HFONT> s_font_cache;

static HFONT get_cached_font(const pf::font& f)
{
	const auto key = std::make_tuple(f.size, f.name, f.size_mode);
	const auto it = s_font_cache.find(key);
	if (it != s_font_cache.end())
		return it->second;
	const auto hfont = create_platform_font(f);
	s_font_cache[key] = hfont;
	return hfont;
}

// Win32 draw_context implementation
// Caches fonts and DC state â€” only issues GDI calls when values actually change.
// Original DC state is restored in the destructor.
class win_draw_context final : public pf::draw_context
{
	HDC _hdc;
	pf::irect _clip;

	// Current DC state
	COLORREF _text_color;
	COLORREF _bk_color;
	mutable HFONT _font = nullptr;
	HBRUSH _fill_brush = nullptr;
	uint32_t _fill_color = 0;

	// Original DC state (restored in destructor)
	COLORREF _orig_text_color;
	COLORREF _orig_bk_color;
	HFONT _orig_font;

	void select_font(const pf::font& f) const
	{
		const auto hfont = get_cached_font(f);
		if (hfont != _font)
		{
			SelectObject(_hdc, hfont);
			_font = hfont;
		}
	}

	void select_text_color(const uint32_t color)
	{
		if (color != _text_color)
		{
			SetTextColor(_hdc, color);
			_text_color = color;
		}
	}

	void select_bk_color(const uint32_t color)
	{
		if (color != _bk_color)
		{
			SetBkColor(_hdc, color);
			_bk_color = color;
		}
	}

public:
	explicit win_draw_context(const HDC hdc, const pf::irect& clip) : _hdc(hdc), _clip(clip)
	{
		_orig_text_color = _text_color = GetTextColor(_hdc);
		_orig_bk_color = _bk_color = GetBkColor(_hdc);
		_orig_font = _font = static_cast<HFONT>(GetCurrentObject(_hdc, OBJ_FONT));
	}

	~win_draw_context() override
	{
		if (_fill_brush) DeleteObject(_fill_brush);
		if (_text_color != _orig_text_color) SetTextColor(_hdc, _orig_text_color);
		if (_bk_color != _orig_bk_color) SetBkColor(_hdc, _orig_bk_color);
		if (_font != _orig_font) SelectObject(_hdc, _orig_font);
	}

	pf::irect clip_rect() const override { return _clip; }

	void fill_solid_rect(const pf::irect& rc, const pf::color_t color) override
	{
		const auto c = color.rgb();
		if (!_fill_brush || c != _fill_color)
		{
			if (_fill_brush) DeleteObject(_fill_brush);
			_fill_brush = CreateSolidBrush(c);
			_fill_color = c;
		}
		FillRect(_hdc, &as_rect(rc), _fill_brush);
	}

	void fill_solid_rect(const int x, const int y, const int cx, const int cy, const pf::color_t color) override
	{
		fill_solid_rect(pf::irect(x, y, x + cx, y + cy), color);
	}

	void draw_text(const int x, const int y, const pf::irect& clip, const std::string_view text,
	               const pf::font& f, const pf::color_t text_color, const pf::color_t bg_color) override
	{
		select_font(f);
		select_text_color(text_color.rgb());
		select_bk_color(bg_color.rgb());
		const RECT rc = {clip.left, clip.top, clip.right, clip.bottom};
		const auto wtext = pf::utf8_to_utf16(text);
		ExtTextOutW(_hdc, x, y, ETO_CLIPPED | ETO_OPAQUE, &rc, wtext.c_str(), static_cast<UINT>(wtext.size()), nullptr);
	}

	pf::isize measure_text(const std::string_view text, const pf::font& f) const override
	{
		select_font(f);
		const auto wtext = pf::utf8_to_utf16(text);
		SIZE sz;
		GetTextExtentPoint32W(_hdc, wtext.c_str(), static_cast<int>(wtext.size()), &sz);
		return {sz.cx, sz.cy};
	}

	void draw_lines(const std::span<const pf::ipoint> points, const pf::color_t color) override
	{
		if (points.size() < 2) return;
		const auto pen = CreatePen(PS_SOLID, 1, color.rgb());
		const auto old_pen = SelectObject(_hdc, pen);
		MoveToEx(_hdc, points[0].x, points[0].y, nullptr);
		for (size_t i = 1; i < points.size(); ++i)
		{
			LineTo(_hdc, points[i].x, points[i].y);
		}
		SelectObject(_hdc, old_pen);
		DeleteObject(pen);
	}

	void draw_bitmap(const int x, const int y, const pf::bitmap& bmp) override
	{
		if (bmp.empty()) return;
		BITMAPINFO bi = {};
		bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
		bi.bmiHeader.biWidth = bmp.width;
		bi.bmiHeader.biHeight = -bmp.height; // top-down
		bi.bmiHeader.biPlanes = 1;
		bi.bmiHeader.biBitCount = 32;
		bi.bmiHeader.biCompression = BI_RGB;
		SetDIBitsToDevice(_hdc,
		                  x, y,
		                  bmp.width, bmp.height,
		                  0, 0,
		                  0, bmp.height,
		                  bmp.pixels.data(),
		                  &bi,
		                  DIB_RGB_COLORS);
	}

	void draw_bitmap(const pf::irect& dest, const pf::bitmap& bmp) override
	{
		if (bmp.empty()) return;
		BITMAPINFO bi = {};
		bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
		bi.bmiHeader.biWidth = bmp.width;
		bi.bmiHeader.biHeight = -bmp.height; // top-down
		bi.bmiHeader.biPlanes = 1;
		bi.bmiHeader.biBitCount = 32;
		bi.bmiHeader.biCompression = BI_RGB;
		const int prev_mode = SetStretchBltMode(_hdc, HALFTONE);
		SetBrushOrgEx(_hdc, 0, 0, nullptr);
		StretchDIBits(_hdc,
		              dest.left, dest.top,
		              dest.width(), dest.height(),
		              0, 0,
		              bmp.width, bmp.height,
		              bmp.pixels.data(),
		              &bi,
		              DIB_RGB_COLORS,
		              SRCCOPY);
		SetStretchBltMode(_hdc, prev_mode);
	}

	void fill_blend_rect(const pf::irect& rc, const pf::color_t color) override
	{
		if (rc.width() <= 0 || rc.height() <= 0 || color.a == 0) return;
		BITMAPINFO info = {};
		info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
		info.bmiHeader.biWidth = 1;
		info.bmiHeader.biHeight = -1;
		info.bmiHeader.biPlanes = 1;
		info.bmiHeader.biBitCount = 32;
		info.bmiHeader.biCompression = BI_RGB;
		void* bits = nullptr;
		const auto bitmap = CreateDIBSection(_hdc, &info, DIB_RGB_COLORS, &bits, nullptr, 0);
		if (!bitmap || !bits)
		{
			if (bitmap) DeleteObject(bitmap);
			return;
		}
		const auto premultiply = [color](const uint8_t c)
		{
			return static_cast<uint8_t>((static_cast<uint16_t>(c) * color.a + 127) / 255);
		};
		*static_cast<uint32_t*>(bits) = premultiply(color.b) |
			(static_cast<uint32_t>(premultiply(color.g)) << 8) |
			(static_cast<uint32_t>(premultiply(color.r)) << 16) |
			(static_cast<uint32_t>(color.a) << 24);
		const auto source = CreateCompatibleDC(_hdc);
		const auto old = SelectObject(source, bitmap);
		BLENDFUNCTION blend = {AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};
		AlphaBlend(_hdc, rc.left, rc.top, rc.width(), rc.height(),
		           source, 0, 0, 1, 1, blend);
		SelectObject(source, old);
		DeleteDC(source);
		DeleteObject(bitmap);
	}

	void draw_bitmap(const pf::irect& dest, const pf::bitmap& bmp, const bool alpha) override
	{
		if (!alpha)
		{
			draw_bitmap(dest, bmp);
			return;
		}
		if (bmp.empty() || dest.width() <= 0 || dest.height() <= 0) return;
		BITMAPINFO info = {};
		info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
		info.bmiHeader.biWidth = bmp.width;
		info.bmiHeader.biHeight = -bmp.height;
		info.bmiHeader.biPlanes = 1;
		info.bmiHeader.biBitCount = 32;
		info.bmiHeader.biCompression = BI_RGB;
		void* bits = nullptr;
		const auto bitmap = CreateDIBSection(_hdc, &info, DIB_RGB_COLORS, &bits, nullptr, 0);
		if (!bitmap || !bits)
		{
			if (bitmap) DeleteObject(bitmap);
			return;
		}
		auto* output = static_cast<uint32_t*>(bits);
		for (size_t i = 0; i != bmp.pixels.size(); ++i)
		{
			const auto pixel = bmp.pixels[i];
			const auto a = static_cast<uint8_t>(pixel >> 24);
			output[i] = (static_cast<uint32_t>((static_cast<uint16_t>(pixel & 0xff) * a + 127) / 255)) |
			            (static_cast<uint32_t>((static_cast<uint16_t>(pixel >> 8 & 0xff) * a + 127) / 255) << 8) |
			            (static_cast<uint32_t>((static_cast<uint16_t>(pixel >> 16 & 0xff) * a + 127) / 255) << 16) |
			            (static_cast<uint32_t>(a) << 24);
		}
		const auto source = CreateCompatibleDC(_hdc);
		const auto old = SelectObject(source, bitmap);
		BLENDFUNCTION blend = {AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};
		AlphaBlend(_hdc, dest.left, dest.top, dest.width(), dest.height(),
		           source, 0, 0, bmp.width, bmp.height, blend);
		SelectObject(source, old);
		DeleteDC(source);
		DeleteObject(bitmap);
	}

	void draw_text_h(const int x, const int y, const std::string_view text,
	                 const pf::font_handle handle, const pf::color_t text_color) override
	{
		const auto hfont = std::bit_cast<HFONT>(handle);
		const auto old_font = static_cast<HFONT>(SelectObject(_hdc, hfont));
		_font = hfont;
		SetBkMode(_hdc, TRANSPARENT);
		SetTextColor(_hdc, text_color.rgb());
		_text_color = text_color.rgb();
		const auto wtext = pf::utf8_to_utf16(text);
		ExtTextOutW(_hdc, x, y, 0, nullptr, wtext.c_str(), static_cast<UINT>(wtext.size()), nullptr);
		SelectObject(_hdc, old_font);
		_font = old_font;
	}

	pf::isize measure_text_h(const std::string_view text, const pf::font_handle handle) const override
	{
		const auto hfont = std::bit_cast<HFONT>(handle);
		const auto old_font = static_cast<HFONT>(SelectObject(_hdc, hfont));
		const auto wtext = pf::utf8_to_utf16(text);
		SIZE sz = {0, 0};
		GetTextExtentPoint32W(_hdc, wtext.c_str(), static_cast<int>(wtext.size()), &sz);
		SelectObject(_hdc, old_font);
		return {sz.cx, sz.cy};
	}

	void draw_solid_line(const pf::ipoint a, const pf::ipoint b, const pf::color_t color, const int width) override
	{
		const auto pen = CreatePen(PS_SOLID, width > 0 ? width : 1, color.rgb());
		const auto old_pen = SelectObject(_hdc, pen);
		MoveToEx(_hdc, a.x, a.y, nullptr);
		LineTo(_hdc, b.x, b.y);
		SelectObject(_hdc, old_pen);
		DeleteObject(pen);
	}

	void draw_ellipse(const int x, const int y, const int w, const int h,
	                  const pf::color_t color, const int line_width) override
	{
		const auto pen = CreatePen(PS_SOLID, line_width > 0 ? line_width : 1, color.rgb());
		const auto old_pen = SelectObject(_hdc, pen);
		const auto old_brush = SelectObject(_hdc, GetStockObject(NULL_BRUSH));
		Ellipse(_hdc, x, y, x + w, y + h);
		SelectObject(_hdc, old_brush);
		SelectObject(_hdc, old_pen);
		DeleteObject(pen);
	}

	void fill_ellipse(const int x, const int y, const int w, const int h, const pf::color_t color) override
	{
		const auto brush = CreateSolidBrush(color.rgb() & 0x00FFFFFF);
		const auto old_brush = SelectObject(_hdc, brush);
		const auto old_pen = SelectObject(_hdc, GetStockObject(NULL_PEN));
		Ellipse(_hdc, x, y, x + w, y + h);
		SelectObject(_hdc, old_pen);
		SelectObject(_hdc, old_brush);
		DeleteObject(brush);
	}

	void set_clip_rect(const pf::irect& rc) override
	{
		POINT view = {0, 0};
		GetWindowOrgEx(_hdc, &view);
		const auto rgn = CreateRectRgn(rc.left - view.x, rc.top - view.y,
		                               rc.right - view.x, rc.bottom - view.y);
		SelectClipRgn(_hdc, rgn);
		DeleteObject(rgn);
	}

	void clear_clip_rect() override
	{
		SelectClipRgn(_hdc, nullptr);
	}
};

// Win32 measure_context implementation
class win_measure_context : public pf::measure_context
{
	HDC _hdc;
	mutable HFONT _font = nullptr;
	HFONT _orig_font;

	void select_font(const pf::font& f) const
	{
		const auto hfont = get_cached_font(f);
		if (hfont != _font)
		{
			SelectObject(_hdc, hfont);
			_font = hfont;
		}
	}

public:
	explicit win_measure_context(const HDC hdc) : _hdc(hdc)
	{
		_orig_font = _font = static_cast<HFONT>(GetCurrentObject(_hdc, OBJ_FONT));
	}

	~win_measure_context() override
	{
		if (_font != _orig_font) SelectObject(_hdc, _orig_font);
	}

	pf::isize measure_text(const std::string_view text, const pf::font& f) const override
	{
		select_font(f);
		const auto wtext = pf::utf8_to_utf16(text);
		SIZE sz;
		GetTextExtentPoint32W(_hdc, wtext.c_str(), static_cast<int>(wtext.size()), &sz);
		return {sz.cx, sz.cy};
	}

	pf::isize measure_char(const pf::font& f) const override
	{
		select_font(f);
		TEXTMETRIC tm;
		::GetTextMetrics(_hdc, &tm);
		return {tm.tmAveCharWidth, tm.tmHeight + tm.tmExternalLeading};
	}

protected:
	// Used by win_measure_context_owned to restore the font before its
	// owned HDC is released, so this base destructor has nothing to do.
	void restore_font_for_owned()
	{
		if (_font != _orig_font)
		{
			SelectObject(_hdc, _orig_font);
			_font = _orig_font;
		}
	}
};

// Win32 measure_context that owns its HDC (for use outside paint).
// NOTE: the base ~win_measure_context() runs *after* this destructor, so
// the HDC must still be valid at that point. We release it via a
// member-scoped guard that destructs after the base.
class win_measure_context_owned final : public win_measure_context
{
	struct dc_holder
	{
		HWND hwnd;
		HDC hdc;
		~dc_holder() { if (hdc) ReleaseDC(hwnd, hdc); }
	};

	// Declared before the base subobject would be destroyed? No — bases
	// destruct after members of the most-derived class. So instead we
	// restore the original font in *this* destructor (before the base
	// runs SelectObject on a freed DC) and then release.
	HWND _hwnd;
	HDC _owned_hdc;

public:
	win_measure_context_owned(const HWND hwnd, const HDC hdc)
		: win_measure_context(hdc), _hwnd(hwnd), _owned_hdc(hdc)
	{
	}

	~win_measure_context_owned() override
	{
		// Release happens here; base destructor will run after but only
		// touches the HDC if the font changed. Pre-restore the font so
		// the base sees no work to do.
		restore_font_for_owned();
		ReleaseDC(_hwnd, _owned_hdc);
		_owned_hdc = nullptr;
	}
};


#ifndef GET_X_LPARAM
#define GET_X_LPARAM(lParam)	((int)(short)LOWORD(lParam))
#endif
#ifndef GET_Y_LPARAM
#define GET_Y_LPARAM(lParam)	((int)(short)HIWORD(lParam))
#endif

static std::vector<pf::file_path> decode_dropped_files(HDROP hDrop);

class win_impl final : public win, public pf::window_frame
{
	pf::frame_reactor_ptr _reactor;
	pf::window_frame_ptr _self_ref; // cleared on WM_DESTROY
	uint16_t _pending_lead = 0; // first half of a WM_CHAR surrogate pair
	pf::isize _minimum_client_size;
	WINDOWPLACEMENT _windowed_placement = {};
	LONG _windowed_style = 0;
	LONG _windowed_exstyle = 0;
	bool _fullscreen = false;

	// Cached back buffer for flicker-free painting
	HDC _hdc_back = nullptr;
	HBITMAP _hbm_back = nullptr;
	HGDIOBJ _hbm_back_old = nullptr;
	int _back_cx = 0;
	int _back_cy = 0;

	void ensure_back_buffer(const HDC hdc, const int cx, const int cy)
	{
		if (_hdc_back && _back_cx == cx && _back_cy == cy)
			return;

		discard_back_buffer();

		if (cx <= 0 || cy <= 0)
			return;

		_hdc_back = CreateCompatibleDC(hdc);
		if (!_hdc_back)
			return;

		_hbm_back = CreateCompatibleBitmap(hdc, cx, cy);
		if (!_hbm_back)
		{
			DeleteDC(_hdc_back);
			_hdc_back = nullptr;
			return;
		}
		_hbm_back_old = SelectObject(_hdc_back, _hbm_back);
		_back_cx = cx;
		_back_cy = cy;
	}

	void discard_back_buffer()
	{
		if (_hdc_back)
		{
			SelectObject(_hdc_back, _hbm_back_old);
			DeleteObject(_hbm_back);
			DeleteDC(_hdc_back);
			_hdc_back = nullptr;
			_hbm_back = nullptr;
			_hbm_back_old = nullptr;
			_back_cx = _back_cy = 0;
		}
	}

public:
	~win_impl() override
	{
		discard_back_buffer();

		if (IsWindow(m_hWnd))
		{
			SetWindowLongPtr(m_hWnd, GWLP_USERDATA, 0);
		}
	}

	void set_self_ref(const pf::window_frame_ptr& ref) { _self_ref = ref; }
	pf::window_frame_ptr self() { return _self_ref; }

	//  window_frame implementation 

	void set_reactor(pf::frame_reactor_ptr reactor) override
	{
		_reactor = std::move(reactor);
	}

	void notify_size() override
	{
		if (!_reactor || !IsWindow(m_hWnd)) return;
		RECT rc;
		GetClientRect(m_hWnd, &rc);
		const pf::isize extent(rc.right - rc.left, rc.bottom - rc.top);
		const auto hdc = GetDC(m_hWnd);
		win_measure_context measure_ctx(hdc);
		auto self = _self_ref;
		_reactor->handle_size(self, extent, measure_ctx);
		ReleaseDC(m_hWnd, hdc);
	}

	pf::irect get_client_rect() const override
	{
		pf::irect result;
		GetClientRect(m_hWnd, &as_rect(result));
		return result;
	}

	void invalidate() override
	{
		InvalidateRect(m_hWnd, nullptr, FALSE);
	}

	void invalidate_rect(const pf::irect& rect) override
	{
		InvalidateRect(m_hWnd, &as_rect(const_cast<pf::irect&>(rect)), FALSE);
	}

	void set_focus() override
	{
		SetFocus(m_hWnd);
	}

	bool has_focus() const override
	{
		return GetFocus() == m_hWnd;
	}

	void set_capture() override
	{
		SetCapture(m_hWnd);
	}

	void release_capture() override
	{
		if (GetCapture() == m_hWnd)
			ReleaseCapture();
	}

	uint32_t set_timer(const uint32_t id, const uint32_t ms) override
	{
		return static_cast<uint32_t>(SetTimer(m_hWnd, id, ms, nullptr));
	}

	void kill_timer(const uint32_t id) override
	{
		KillTimer(m_hWnd, id);
	}

	pf::ipoint screen_to_client(const pf::ipoint pt) const override
	{
		POINT p = {pt.x, pt.y};
		ScreenToClient(m_hWnd, &p);
		return {p.x, p.y};
	}

	pf::ipoint client_to_screen(const pf::ipoint pt) const override
	{
		POINT p = {pt.x, pt.y};
		ClientToScreen(m_hWnd, &p);
		return {p.x, p.y};
	}

	void set_cursor_shape(const pf::cursor_shape shape) override
	{
		LPCWSTR id = IDC_ARROW;
		switch (shape)
		{
		case pf::cursor_shape::arrow: id = IDC_ARROW;
			break;
		case pf::cursor_shape::ibeam: id = IDC_IBEAM;
			break;
		case pf::cursor_shape::size_we: id = IDC_SIZEWE;
			break;
		case pf::cursor_shape::size_ns: id = IDC_SIZENS;
			break;
		case pf::cursor_shape::hand: id = IDC_HAND;
			break;
		case pf::cursor_shape::wait: id = IDC_WAIT;
			break;
		}
		SetCursor(::LoadCursor(nullptr, id));
	}

	void move_window(const pf::irect& bounds) override
	{
		MoveWindow(m_hWnd, bounds.left, bounds.top, bounds.width(), bounds.height(), TRUE);
	}

	void show(const bool visible) override
	{
		ShowWindow(m_hWnd, visible ? SW_SHOW : SW_HIDE);
	}

	bool is_visible() const override
	{
		return IsWindowVisible(m_hWnd) != 0;
	}

	void set_text(const std::string_view text) override
	{
		SetWindowTextW(m_hWnd, pf::utf8_to_utf16(text).c_str());
	}

	std::string text_from_clipboard() override
	{
		return clipboard_get_text(m_hWnd);
	}

	bool text_to_clipboard(const std::string_view text) override
	{
		return clipboard_set_text(m_hWnd, text);
	}

	placement get_placement() const override
	{
		WINDOWPLACEMENT wp = {};
		wp.length = sizeof(wp);
		GetWindowPlacement(m_hWnd, &wp);
		placement p;
		std::memcpy(&p.normal_bounds, &wp.rcNormalPosition, sizeof(RECT));
		p.maximized = wp.showCmd == SW_SHOWMAXIMIZED || IsZoomed(m_hWnd);
		return p;
	}

	void set_placement(const placement& p) override
	{
		WINDOWPLACEMENT wp = {};
		wp.length = sizeof(wp);
		wp.showCmd = p.maximized ? SW_SHOWMAXIMIZED : SW_SHOWNORMAL;
		std::memcpy(&wp.rcNormalPosition, &p.normal_bounds, sizeof(RECT));
		SetWindowPlacement(m_hWnd, &wp);
	}

	void set_minimum_client_size(const pf::isize size) override
	{
		_minimum_client_size = {std::max(0, size.cx), std::max(0, size.cy)};
		if (IsWindow(m_hWnd)) SetWindowPos(m_hWnd, nullptr, 0, 0, 0, 0,
		                                   SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
	}

	void set_fullscreen(const bool fullscreen) override
	{
		if (!IsWindow(m_hWnd) || fullscreen == _fullscreen) return;

		if (fullscreen)
		{
			_windowed_style = GetWindowLongW(m_hWnd, GWL_STYLE);
			_windowed_exstyle = GetWindowLongW(m_hWnd, GWL_EXSTYLE);
			_windowed_placement.length = sizeof(_windowed_placement);
			GetWindowPlacement(m_hWnd, &_windowed_placement);

			MONITORINFO monitor = {sizeof(monitor)};
			GetMonitorInfoW(MonitorFromWindow(m_hWnd, MONITOR_DEFAULTTONEAREST), &monitor);
			SetWindowLongW(m_hWnd, GWL_STYLE,
			              _windowed_style & ~(WS_CAPTION | WS_THICKFRAME | WS_MINIMIZEBOX |
			                                  WS_MAXIMIZEBOX | WS_SYSMENU));
			SetWindowLongW(m_hWnd, GWL_EXSTYLE, _windowed_exstyle & ~WS_EX_DLGMODALFRAME);
			SetWindowPos(m_hWnd, HWND_TOP,
			             monitor.rcMonitor.left, monitor.rcMonitor.top,
			             monitor.rcMonitor.right - monitor.rcMonitor.left,
			             monitor.rcMonitor.bottom - monitor.rcMonitor.top,
			             SWP_NOOWNERZORDER | SWP_FRAMECHANGED | SWP_SHOWWINDOW);
			_fullscreen = true;
		}
		else
		{
			SetWindowLongW(m_hWnd, GWL_STYLE, _windowed_style);
			SetWindowLongW(m_hWnd, GWL_EXSTYLE, _windowed_exstyle);
			SetWindowPlacement(m_hWnd, &_windowed_placement);
			SetWindowPos(m_hWnd, nullptr, 0, 0, 0, 0,
			             SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
			_fullscreen = false;
		}
	}

	bool is_fullscreen() const override
	{
		return _fullscreen;
	}

	void track_mouse_leave() override
	{
		TRACKMOUSEEVENT tme = {};
		tme.cbSize = sizeof(tme);
		tme.dwFlags = TME_LEAVE;
		tme.hwndTrack = m_hWnd;
		TrackMouseEvent(&tme);
	}

	bool is_key_down(const unsigned int vk) const override
	{
		return (GetKeyState(vk) & 0x8000) != 0;
	}

	bool is_key_down_async(const unsigned int vk) const override
	{
		return (GetAsyncKeyState(vk) & 0x8000) != 0;
	}

	pf::window_frame_ptr create_child(const std::string_view class_name,
	                                  const uint32_t style, const pf::color_t background) const & override
	{
		auto child = std::make_shared<win_impl>();
		child->create_window(pf::utf8_to_utf16(class_name).c_str(), m_hWnd, background,
		                     map_window_style(style), map_window_ex_style(style));
		child->set_self_ref(child);
		return child;
	}

	void close() override
	{
		DestroyWindow(m_hWnd);
	}

	int message_box(const std::string_view text, const std::string_view title, const uint32_t style) override
	{
		return MessageBoxW(m_hWnd, pf::utf8_to_utf16(text).c_str(), pf::utf8_to_utf16(title).c_str(), style);
	}

	void present_pixels(const uint32_t* pixels, const int cx, const int cy) override
	{
		if (!pixels || cx <= 0 || cy <= 0) return;

		BITMAPINFO bmi = {};
		bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
		bmi.bmiHeader.biWidth = cx;
		bmi.bmiHeader.biHeight = -cy; // top-down
		bmi.bmiHeader.biPlanes = 1;
		bmi.bmiHeader.biBitCount = 32;
		bmi.bmiHeader.biCompression = BI_RGB;

		const HDC hdc = GetDC(m_hWnd);
		SetDIBitsToDevice(hdc, 0, 0, cx, cy, 0, 0, 0, cy, pixels, &bmi, DIB_RGB_COLORS);
		ReleaseDC(m_hWnd, hdc);
	}

	void set_menu(std::vector<pf::menu_command> menu_def) override
	{
		pf::platform_set_menu(std::move(menu_def));
	}


	std::unique_ptr<pf::measure_context> create_measure_context() const override
	{
		HDC hdc = GetDC(m_hWnd);
		auto ctx = std::make_unique<win_measure_context_owned>(m_hWnd, hdc);
		return ctx;
	}

	void show_popup_menu(const std::vector<pf::menu_command>& items, const pf::ipoint& screen_pt) override
	{
		if (items.empty()) return;

		auto display_text = [](const pf::menu_command& item)
		{
			auto text = pf::utf8_to_utf16(item.text);
			if (!item.accel.empty())
			{
				text += L'\t';
				text += pf::utf8_to_utf16(pf::format_key_binding(item.accel));
			}
			return text;
		};

		// Assign temporary IDs and build a lookup table
		std::unordered_map<int, const pf::menu_command*> id_map;
		int next_id = 30000;

		std::function<HMENU(const std::vector<pf::menu_command>&)> build;
		build = [&](const std::vector<pf::menu_command>& cmds) -> HMENU
		{
			const HMENU hMenu = CreatePopupMenu();
			for (const auto& cmd : cmds)
			{
				if (cmd.text.empty() && cmd.children.empty())
				{
					AppendMenuW(hMenu, MF_SEPARATOR, 0, nullptr);
				}
				else if (!cmd.children.empty())
				{
					AppendMenuW(hMenu, MF_POPUP, reinterpret_cast<UINT_PTR>(build(cmd.children)),
					            display_text(cmd).c_str());
				}
				else
				{
					const int id = next_id++;
					id_map[id] = &cmd;

					UINT flags = MF_STRING;
					if (cmd.is_enabled && !cmd.is_enabled()) flags |= MF_GRAYED;
					if (cmd.is_checked && cmd.is_checked()) flags |= MF_CHECKED;

					AppendMenuW(hMenu, flags, id, display_text(cmd).c_str());
				}
			}
			return hMenu;
		};

		const HMENU hMenu = build(items);
		const int sel = TrackPopupMenu(hMenu, TPM_RETURNCMD | TPM_RIGHTBUTTON,
		                               screen_pt.x, screen_pt.y, 0, m_hWnd, nullptr);
		if (sel != 0)
		{
			const auto it = id_map.find(sel);
			if (it != id_map.end() && it->second->action)
				it->second->action();
		}
		DestroyMenu(hMenu);
	}

	double get_dpi_scale() const override
	{
		const auto dpi = GetDpiForWindow(m_hWnd);
		return dpi > 0 ? dpi / 96.0 : 1.0;
	}

	void accept_drop_files(const bool accept) override
	{
		DragAcceptFiles(m_hWnd, accept ? TRUE : FALSE);
	}

	void configure_gestures(const bool zoom, const bool pan) override
	{
		const auto flags = zoom || pan ? TWF_WANTPALM : 0;
		if (zoom || pan)
			RegisterTouchWindow(m_hWnd, flags);
		else
			UnregisterTouchWindow(m_hWnd);
	}

	pf::toolbar_frame_ptr create_address_bar(const pf::address_bar_config& cfg) override;

	//  Win32 message handling 

	virtual LRESULT handle_message(const HWND hWnd, const UINT uMsg, const WPARAM wParam, const LPARAM lParam)
	{
		if (dispatch_instance_signal(uMsg)) return 0;
		const auto reactor = _reactor;
		if (!reactor)
		{
			if (uMsg == WM_DESTROY)
			{
				_self_ref.reset();
				if (GetParent(hWnd) == nullptr) PostQuitMessage(0);
			}
			return DefWindowProc(hWnd, uMsg, wParam, lParam);

		if (uMsg == WM_GESTURE)
		{
			GESTUREINFO info = {};
			info.cbSize = sizeof(info);
			if (GetGestureInfo(reinterpret_cast<HGESTUREINFO>(lParam), &info))
			{
				const auto kind = info.dwID == GID_ZOOM ? pf::gesture_kind::zoom
				                                       : info.dwID == GID_PAN ? pf::gesture_kind::pan
				                                                             : pf::gesture_kind::pan;
				if ((info.dwID == GID_ZOOM || info.dwID == GID_PAN) && self())
				{
					POINT point = {info.ptsLocation.x, info.ptsLocation.y};
					ScreenToClient(hWnd, &point);
					pf::gesture_params params;
					params.kind = kind;
					params.point = {point.x, point.y};
					params.argument = info.ullArguments;
					params.begin = (info.dwFlags & GF_BEGIN) != 0;
					params.end = (info.dwFlags & GF_END) != 0;
					reactor->handle_gesture(_self_ref, params);
				}
				CloseGestureInfoHandle(reinterpret_cast<HGESTUREINFO>(lParam));
				return 0;
			}
		}
		}

		if (uMsg == WM_GETMINMAXINFO && (_minimum_client_size.cx > 0 || _minimum_client_size.cy > 0))
		{
			auto* const info = reinterpret_cast<MINMAXINFO*>(lParam);
			RECT client = {
				0, 0,
				std::max<LONG>(0, _minimum_client_size.cx),
				std::max<LONG>(0, _minimum_client_size.cy)
			};
			const auto dpi = GetDpiForWindow(hWnd);
			if (!AdjustWindowRectExForDpi(&client, static_cast<DWORD>(GetWindowLongW(hWnd, GWL_STYLE)),
			                              FALSE, static_cast<DWORD>(GetWindowLongW(hWnd, GWL_EXSTYLE)), dpi))
			{
				AdjustWindowRectEx(&client, static_cast<DWORD>(GetWindowLongW(hWnd, GWL_STYLE)),
				                   FALSE, static_cast<DWORD>(GetWindowLongW(hWnd, GWL_EXSTYLE)));
			}
			info->ptMinTrackSize.x = std::max(info->ptMinTrackSize.x, client.right - client.left);
			info->ptMinTrackSize.y = std::max(info->ptMinTrackSize.y, client.bottom - client.top);
			return 0;
		}

		// Special-case WM_NCHITTEST: expand resize border for easier grabbing
		if (uMsg == WM_NCHITTEST && GetWindowLong(hWnd, GWL_STYLE) & WS_THICKFRAME)
		{
			const auto result = DefWindowProc(hWnd, uMsg, wParam, lParam);
			if (result == HTCLIENT)
			{
				RECT rc;
				GetWindowRect(hWnd, &rc);
				const int x = GET_X_LPARAM(lParam);
				const int y = GET_Y_LPARAM(lParam);
				constexpr int border = 6;

				const bool near_left = x < rc.left + border;
				const bool near_right = x >= rc.right - border;
				const bool near_top = y < rc.top + border;
				const bool near_bottom = y >= rc.bottom - border;

				if (near_top && near_left) return HTTOPLEFT;
				if (near_top && near_right) return HTTOPRIGHT;
				if (near_bottom && near_left) return HTBOTTOMLEFT;
				if (near_bottom && near_right) return HTBOTTOMRIGHT;
				if (near_left) return HTLEFT;
				if (near_right) return HTRIGHT;
				if (near_top) return HTTOP;
				if (near_bottom) return HTBOTTOM;
			}
			return result;
		}

		// Special-case WM_SETCURSOR: let DefWindowProc handle non-client cursors (resize arrows)
		if (uMsg == WM_SETCURSOR)
		{
			const auto hitTest = LOWORD(lParam);
			if (hitTest != HTCLIENT)
				return DefWindowProc(hWnd, uMsg, wParam, lParam);
		}

		// Special-case WM_PAINT: double-buffered paint via cached offscreen bitmap
		if (uMsg == WM_PAINT)
		{
			PAINTSTRUCT ps;
			const HDC hdc = BeginPaint(m_hWnd, &ps);

			RECT rc;
			GetClientRect(m_hWnd, &rc);
			const int cx = rc.right - rc.left;
			const int cy = rc.bottom - rc.top;

			const pf::irect clip(ps.rcPaint.left, ps.rcPaint.top, ps.rcPaint.right, ps.rcPaint.bottom);

			ensure_back_buffer(hdc, cx, cy);

			if (_hdc_back)
			{
				// Clip to the dirty rect so a partial repaint (a blinking caret) costs
				// only the pixels it touches, even though views redraw their whole viewport.
				const int saved = SaveDC(_hdc_back);
				IntersectClipRect(_hdc_back, clip.left, clip.top, clip.right, clip.bottom);

				{
					win_draw_context draw_ctx(_hdc_back, clip);
					auto self = _self_ref;
					reactor->handle_paint(self, draw_ctx);
				}

				RestoreDC(_hdc_back, saved);

				BitBlt(hdc, clip.left, clip.top, clip.width(), clip.height(),
				       _hdc_back, clip.left, clip.top, SRCCOPY);
			}
			else
			{
				// Fall back to painting directly to the window DC if the back
				// buffer could not be created.
				win_draw_context draw_ctx(hdc, clip);
				auto self = _self_ref;
				reactor->handle_paint(self, draw_ctx);
			}

			EndPaint(m_hWnd, &ps);
			return 0;
		}

		// Special-case WM_SIZE: extract dimensions and call handle_size
		if (uMsg == WM_SIZE)
		{
			const pf::isize extent(LOWORD(lParam), HIWORD(lParam));
			const auto hdc = GetDC(hWnd);
			win_measure_context measure_ctx(hdc);
			auto self = _self_ref;
			reactor->handle_size(self, extent, measure_ctx);
			ReleaseDC(hWnd, hdc);
			return 0;
		}

		// Apply enable/check state when a menu is about to be shown
		if (uMsg == WM_INITMENUPOPUP)
		{
			apply_menu_state(reinterpret_cast<HMENU>(wParam));
			return 0;
		}

		// On per-monitor-v2 DPI change, resize the window to the rect
		// suggested by Windows so child controls and layout follow the
		// new DPI. Notify the reactor first so it can refresh metrics.
		if (uMsg == WM_DPICHANGED)
		{
			if (const auto* const rc = reinterpret_cast<const RECT*>(lParam))
			{
				const auto self = _self_ref;
				if (self)
				{
					pf::message_params params;
					params.dpi_scale = LOWORD(wParam) / 96.0;
					params.suggested_bounds = {rc->left, rc->top, rc->right, rc->bottom};
					reactor->handle_message(self, pf::message_type::dpi_changed, params);
				}
				SetWindowPos(hWnd, nullptr,
				             rc->left, rc->top,
				             rc->right - rc->left, rc->bottom - rc->top,
				             SWP_NOZORDER | SWP_NOACTIVATE);
				return 0;
			}
		}

		// Dispatch menu/accelerator WM_COMMAND via pf::menu_command actions
		if (uMsg == WM_COMMAND && lParam == 0)
		{
			const int cmdId = LOWORD(wParam);
			if (dispatch_menu_command(g_menuDef, cmdId))
				return 0;
		}

		// Map to platform message_type and delegate to reactor
		const auto mt = map_message(uMsg);
		if (mt)
		{
			const auto self = _self_ref;
			if (self)
			{
				pf::message_params params;
				std::vector<pf::file_path> dropped;

				switch (uMsg)
				{
				case WM_TIMER:
					params.timer_id = static_cast<uint32_t>(wParam);
					break;
				case WM_DROPFILES:
					dropped = decode_dropped_files(reinterpret_cast<HDROP>(wParam));
					params.dropped_paths = dropped;
					break;
				case WM_ENDSESSION:
					params.session_ending = wParam != 0;
					break;
				default:
					break;
				}

				const auto result = reactor->handle_message(self, *mt, params);

				if (uMsg == WM_DESTROY)
				{
					// Keep the callback alive for the duration of dispatch, then break the
					// frame -> reactor -> application ownership cycle before app_destroy.
					_reactor.reset();
					_self_ref.reset();
					if (GetParent(hWnd) == nullptr)
						PostQuitMessage(0);
				}

				return result;
			}
			if (uMsg == WM_DESTROY)
			{
				_reactor.reset();
				_self_ref.reset();
				if (GetParent(hWnd) == nullptr) PostQuitMessage(0);
			}
		}

		// Map to keyboard_message_type and delegate to handle_keyboard
		const auto kmt = map_keyboard_message(uMsg);
		if (kmt)
		{
			const auto self = _self_ref;
			if (self)
			{
				pf::keyboard_params params;

				if (*kmt == pf::keyboard_message_type::key_down)
				{
					params.vk = static_cast<unsigned int>(wParam);
					params.repeat = (lParam & 1LL << 30) != 0; // previous key state
				}
				else if (*kmt == pf::keyboard_message_type::key_up)
				{
					params.vk = static_cast<unsigned int>(wParam);
				}
				else
				{
					// WM_CHAR delivers UTF-16 code units; a supplementary character arrives as two messages.
					const auto unit = static_cast<uint16_t>(wParam);

					if (pf::is_lead_surrogate(unit))
					{
						_pending_lead = unit;
						return 0;
					}

					if (pf::is_trail_surrogate(unit))
					{
						if (_pending_lead == 0)
							return 0;
						params.ch = static_cast<char32_t>(0x10000u + ((_pending_lead - 0xD800u) << 10) + (unit - 0xDC00u));
						_pending_lead = 0;
					}
					else
					{
						_pending_lead = 0;
						params.ch = static_cast<char32_t>(unit);
					}
				}

				return reactor->handle_keyboard(self, *kmt, params);
			}
		}

		// Map to mouse_message_type and delegate to handle_mouse
		const auto mmt = map_mouse_message(uMsg);
		if (mmt)
		{
			const auto self = _self_ref;
			if (self)
			{
				pf::mouse_params params;
				params.point = pf::point_from_lparam(lParam);

				const auto key_flags = static_cast<uint32_t>(wParam & 0xFFFF);
				params.left_button = (key_flags & 0x0001) != 0; // MK_LBUTTON
				params.right_button = (key_flags & 0x0002) != 0; // MK_RBUTTON
				params.middle_button = (key_flags & 0x0010) != 0; // MK_MBUTTON
				params.control = (key_flags & 0x0008) != 0; // MK_CONTROL
				params.shift = (key_flags & 0x0004) != 0; // MK_SHIFT

				if (*mmt == pf::mouse_message_type::mouse_wheel)
				{
					params.wheel_delta = static_cast<int16_t>(wParam >> 16 & 0xFFFF);
					// WM_MOUSEWHEEL delivers screen coords — convert to client.
					POINT sp = {params.point.x, params.point.y};
					ScreenToClient(m_hWnd, &sp);
					params.point = {sp.x, sp.y};
				}

				if (*mmt == pf::mouse_message_type::set_cursor)
					params.hit_test = static_cast<uint32_t>(lParam & 0xFFFF);

				const auto result = reactor->handle_mouse(self, *mmt, params);

				// DefWindowProc turns a right-button release into WM_CONTEXTMENU, which
				// is how every context menu here opens; the release is reported as well
				// as that, never instead of it.
				if (uMsg == WM_RBUTTONUP)
					return DefWindowProc(hWnd, uMsg, wParam, lParam);

				return result;
			}
		}

		return DefWindowProc(hWnd, uMsg, wParam, lParam);
	}

	//  Win32 window procedure 

	static LRESULT CALLBACK win_proc(const HWND hwnd, const UINT uMsg, const WPARAM wParam, const LPARAM lParam)
	{
		if (uMsg == WM_NCCREATE)
		{
			const auto pt = std::bit_cast<win_impl*>(std::bit_cast<LPCREATESTRUCT>(lParam)->lpCreateParams);
			const auto ptr = std::bit_cast<LONG_PTR>(std::bit_cast<LPCREATESTRUCT>(lParam)->lpCreateParams);
			SetWindowLongPtr(hwnd, GWLP_USERDATA, ptr);

			if (pt)
			{
				pt->m_hWnd = hwnd;
			}
		}

		if (uMsg == WM_INITDIALOG)
		{
			const auto pt = std::bit_cast<win_impl*>(lParam);
			const auto ptr = std::bit_cast<LONG_PTR>(lParam);
			SetWindowLongPtr(hwnd, GWLP_USERDATA, ptr);

			if (pt)
			{
				pt->m_hWnd = hwnd;
			}
		}

		// get the pointer to the window
		const auto ptr = std::bit_cast<win_impl*>(GetWindowLongPtr(hwnd, GWLP_USERDATA));

		if (ptr)
		{
			return ptr->handle_message(hwnd, uMsg, wParam, lParam);
		}
		return DefWindowProc(hwnd, uMsg, wParam, lParam);
	}

	//  Window class registration & creation 

	static bool register_class(const UINT style, const HICON hIcon, const HCURSOR hCursor,
	                           const uint32_t clr_background,
	                           const LPCWSTR lpszMenuName, const LPCWSTR lpszClassName, const HICON hIconSm)
	{
		WNDCLASSEX wcx;
		wcx.cbSize = sizeof(WNDCLASSEX);
		wcx.style = style;
		wcx.lpfnWndProc = win_proc;
		wcx.cbClsExtra = 0;
		wcx.cbWndExtra = 0;
		wcx.hInstance = resource_instance;
		wcx.hIcon = hIcon;
		wcx.hCursor = hCursor;
		wcx.hbrBackground = CreateSolidBrush(clr_background);
		wcx.lpszMenuName = lpszMenuName;
		wcx.lpszClassName = lpszClassName;
		wcx.hIconSm = hIconSm;

		if (RegisterClassEx(&wcx) == 0)
		{
			if (GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
			{
				DeleteObject(wcx.hbrBackground);
				return false;
			}
			DeleteObject(wcx.hbrBackground);
		}

		return true;
	}

	void create_window(const LPCWSTR class_name, const HWND parent, const pf::color_t wnd_clr, const uint32_t style,
	                   const uint32_t exstyle = 0,
	                   const uintptr_t id = 0,
	                   const pf::window_options& options = {})
	{
		const auto default_cursor = LoadCursor(nullptr, IDC_ARROW);

		// platform_add_app embeds the application's icon as resource 1. Without it
		// on the class a top-level window shows the generic icon in its caption and
		// in Alt+Tab, although the exe carries its own. An application with no icon
		// gets null back and keeps that default.
		HICON icon = nullptr;
		HICON small_icon = nullptr;
		if (!parent)
		{
			icon = static_cast<HICON>(LoadImageW(resource_instance, MAKEINTRESOURCEW(1), IMAGE_ICON, 0, 0,
			                                     LR_DEFAULTSIZE | LR_SHARED));
			small_icon = static_cast<HICON>(LoadImageW(resource_instance, MAKEINTRESOURCEW(1), IMAGE_ICON,
			                                           GetSystemMetrics(SM_CXSMICON),
			                                           GetSystemMetrics(SM_CYSMICON), LR_SHARED));
		}

		if (register_class(CS_HREDRAW | CS_VREDRAW | CS_DBLCLKS,
		                   icon, default_cursor,
		                   wnd_clr.rgb(),
		                   nullptr, class_name, small_icon))
		{
			const bool is_child = (style & WS_CHILD) != 0;
			const int x = is_child ? 0 : CW_USEDEFAULT;
			const int y = is_child ? 0 : CW_USEDEFAULT;
			const auto dpi = GetDpiForSystem();
			auto window_style = static_cast<DWORD>(style);
			if (!is_child && !options.resizable)
				window_style &= ~(WS_THICKFRAME | WS_MAXIMIZEBOX);

			int w = is_child ? 0 : CW_USEDEFAULT;
			int h = is_child ? 0 : CW_USEDEFAULT;
			if (!is_child && options.initial_client_size.cx > 0 && options.initial_client_size.cy > 0)
			{
				RECT client = {0, 0, options.initial_client_size.cx, options.initial_client_size.cy};
				if (AdjustWindowRectExForDpi(&client, window_style, FALSE, exstyle, dpi))
				{
					w = client.right - client.left;
					h = client.bottom - client.top;
				}
			}

			m_hWnd = CreateWindowEx(
				exstyle,
				class_name,
				nullptr,
				window_style,
				x, y, w, h,
				parent, std::bit_cast<HMENU>(id),
				resource_instance,
				this);
			if (m_hWnd)
			{
				_minimum_client_size = options.minimum_client_size;
				if (options.fullscreen) set_fullscreen(true);
			}
		}
	}
};

namespace pf::platform_win
{
	uintptr_t native_window_handle(const pf::window_frame_ptr& frame)
	{
		const auto* const native = dynamic_cast<const win_impl*>(frame.get());
		return native ? reinterpret_cast<uintptr_t>(native->m_hWnd) : 0;
	}

	bool set_fullscreen(const pf::window_frame_ptr& frame, const bool fullscreen)
	{
		auto* const native = dynamic_cast<win_impl*>(frame.get());
		if (!native) return false;
		native->set_fullscreen(fullscreen);
		return native->is_fullscreen() == fullscreen;
	}
}

namespace
{
	class win_tray_service final : public pf::tray_service
	{
		static constexpr UINT callback_message = WM_APP + 0x34;

		HWND _window = nullptr;
		HICON _icon = nullptr;
		UINT _id = 0;
		UINT _taskbar_created = 0;
		bool _visible = false;
		std::wstring _tooltip;
		pf::tray_callbacks _callbacks;
		std::string _diagnostics;

		static ATOM register_window_class()
		{
			static const ATOM atom = []
			{
				WNDCLASSW wc = {};
				wc.lpfnWndProc = &window_proc;
				wc.hInstance = GetModuleHandleW(nullptr);
				wc.lpszClassName = L"LibdesktopTrayWindow";
				return RegisterClassW(&wc);
			}();
			return atom;
		}

		static std::string last_error_text()
		{
			const auto error = GetLastError();
			return error ? std::format("Windows error {}", error) : "The tray operation failed.";
		}

		static LRESULT CALLBACK window_proc(const HWND hwnd, const UINT message,
		                                   const WPARAM wParam, const LPARAM lParam)
		{
			auto* self = reinterpret_cast<win_tray_service*>(
				GetWindowLongPtrW(hwnd, GWLP_USERDATA));
			if (message == WM_NCCREATE)
			{
				const auto* const create = reinterpret_cast<const CREATESTRUCTW*>(lParam);
				self = static_cast<win_tray_service*>(create->lpCreateParams);
				SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
			}
			if (!self) return DefWindowProcW(hwnd, message, wParam, lParam);

			if (message == self->_taskbar_created)
			{
				self->republish();
				return 0;
			}
			if (message == WM_ENDSESSION)
			{
				if (self->_callbacks.on_end_session)
					self->_callbacks.on_end_session(wParam != 0);
				return 0;
			}
			if (message != callback_message) return DefWindowProcW(hwnd, message, wParam, lParam);

			switch (static_cast<UINT>(lParam))
			{
			case WM_LBUTTONUP:
			case WM_LBUTTONDBLCLK:
			case NIN_SELECT:
				if (self->_callbacks.on_activate) self->_callbacks.on_activate();
				break;
			case WM_CONTEXTMENU:
			case WM_RBUTTONUP:
			case NIN_KEYSELECT:
				self->context_menu(static_cast<UINT>(lParam) == NIN_KEYSELECT);
				break;
			case NIN_BALLOONUSERCLICK:
				if (self->_callbacks.on_balloon_click) self->_callbacks.on_balloon_click();
				break;
			default:
				break;
			}
			return 0;
		}

		void context_menu(const bool keyboard_anchor)
		{
			POINT point = {};
			if (keyboard_anchor)
			{
				NOTIFYICONIDENTIFIER identifier = {};
				identifier.cbSize = sizeof(identifier);
				identifier.hWnd = _window;
				identifier.uID = _id;
				RECT rect = {};
				if (FAILED(Shell_NotifyIconGetRect(&identifier, &rect)))
				{
					GetCursorPos(&point);
				}
				else
				{
					point.x = rect.left;
					point.y = rect.bottom;
				}
			}
			else
			{
				GetCursorPos(&point);
			}
			if (_callbacks.on_context_menu)
				_callbacks.on_context_menu({point.x, point.y}, keyboard_anchor);
		}

		HICON make_icon(const pf::tray_icon_image image)
		{
			if (image.extent.cx <= 0 || image.extent.cy <= 0 ||
				image.extent.cx > 256 || image.extent.cy > 256 ||
				image.bgra_pixels.size() < static_cast<size_t>(image.extent.cx) * image.extent.cy)
			{
				_diagnostics = "The tray icon dimensions or pixel buffer are invalid.";
				return nullptr;
			}

			BITMAPINFO info = {};
			info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
			info.bmiHeader.biWidth = image.extent.cx;
			info.bmiHeader.biHeight = -image.extent.cy;
			info.bmiHeader.biPlanes = 1;
			info.bmiHeader.biBitCount = 32;
			info.bmiHeader.biCompression = BI_RGB;

			void* bits = nullptr;
			const auto dc = GetDC(nullptr);
			const auto color = CreateDIBSection(dc, &info, DIB_RGB_COLORS, &bits, nullptr, 0);
			ReleaseDC(nullptr, dc);
			if (!color || !bits)
			{
				if (color) DeleteObject(color);
				_diagnostics = "Unable to allocate the tray icon bitmap.";
				return nullptr;
			}
			std::memcpy(bits, image.bgra_pixels.data(),
			            static_cast<size_t>(image.extent.cx) * image.extent.cy * sizeof(uint32_t));

			const auto mask_bytes = (image.extent.cx + 7) / 8 * image.extent.cy;
			std::vector<uint8_t> mask(mask_bytes, 0);
			const auto mask_bitmap = CreateBitmap(image.extent.cx, image.extent.cy, 1, 1, mask.data());
			if (!mask_bitmap)
			{
				DeleteObject(color);
				_diagnostics = "Unable to allocate the tray icon mask.";
				return nullptr;
			}

			ICONINFO icon_info = {};
			icon_info.fIcon = TRUE;
			icon_info.hbmColor = color;
			icon_info.hbmMask = mask_bitmap;
			const auto icon = CreateIconIndirect(&icon_info);
			DeleteObject(color);
			DeleteObject(mask_bitmap);
			if (!icon) _diagnostics = "Unable to create the tray icon.";
			return icon;
		}

		NOTIFYICONDATAW notification() const
		{
			NOTIFYICONDATAW result = {};
			result.cbSize = sizeof(result);
			result.hWnd = _window;
			result.uID = _id;
			result.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
			result.uCallbackMessage = callback_message;
			result.hIcon = _icon;
			wcsncpy_s(result.szTip, _tooltip.c_str(), _TRUNCATE);
			return result;
		}

		bool publish()
		{
			if (g_test_tray_publication_failure.load(std::memory_order_relaxed))
			{
				_diagnostics = "Tray publication was rejected by the test seam.";
				return false;
			}
			auto notification_data = notification();
			if (!Shell_NotifyIconW(NIM_ADD, &notification_data))
			{
				_diagnostics = last_error_text();
				return false;
			}
			notification_data.uVersion = NOTIFYICON_VERSION_4;
			if (!Shell_NotifyIconW(NIM_SETVERSION, &notification_data))
			{
				_diagnostics = last_error_text();
				Shell_NotifyIconW(NIM_DELETE, &notification_data);
				return false;
			}
			_visible = true;
			_diagnostics.clear();
			return true;
		}

		void republish()
		{
			if (_icon && _visible) publish();
		}

	public:
		win_tray_service(const pf::tray_icon_image image, const std::string_view tooltip,
		                 pf::tray_callbacks callbacks)
			: _id([] { static std::atomic_uint32_t next = 1; return next.fetch_add(1); }()),
			  _taskbar_created(RegisterWindowMessageW(L"TaskbarCreated")),
			  _callbacks(std::move(callbacks))
		{
			if (!register_window_class())
			{
				_diagnostics = last_error_text();
				return;
			}
			_window = CreateWindowExW(0, L"LibdesktopTrayWindow", L"", 0, 0, 0, 0, 0,
			                          HWND_MESSAGE, nullptr, GetModuleHandleW(nullptr), this);
			if (!_window)
			{
				_diagnostics = last_error_text();
				return;
			}
			set_tooltip(tooltip);
			set_icon(image);
		}

		~win_tray_service() override
		{
			if (_window && _visible)
			{
				auto notification_data = notification();
				Shell_NotifyIconW(NIM_DELETE, &notification_data);
			}
			if (_icon) DestroyIcon(_icon);
			if (_window) DestroyWindow(_window);
		}

		bool available() const override
		{
			return _window != nullptr && _icon != nullptr;
		}

		std::string diagnostics() const override { return _diagnostics; }

		bool set_icon(const pf::tray_icon_image image) override
		{
			const auto icon = make_icon(image);
			if (!icon) return false;
			if (_icon) DestroyIcon(_icon);
			_icon = icon;
			if (!_visible) return true;
			auto data = notification();
			if (Shell_NotifyIconW(NIM_MODIFY, &data) != FALSE) return true;
			_diagnostics = last_error_text();
			return false;
		}

		bool set_tooltip(const std::string_view text) override
		{
			_tooltip = pf::utf8_to_utf16(text.substr(0, 127));
			if (!_visible) return true;
			auto data = notification();
			if (Shell_NotifyIconW(NIM_MODIFY, &data) != FALSE) return true;
			_diagnostics = last_error_text();
			return false;
		}

		bool show() override
		{
			if (_visible) return true;
			if (!_window || !_icon) return false;
			return publish();
		}

		bool hide() override
		{
			if (!_visible) return true;
			auto data = notification();
			if (!Shell_NotifyIconW(NIM_DELETE, &data))
			{
				_diagnostics = last_error_text();
				return false;
			}
			_visible = false;
			return true;
		}

		bool show_balloon(const std::string_view title, const std::string_view text,
		                 const pf::tray_balloon_kind kind) override
		{
			if (!_visible) return false;
			auto data = notification();
			data.uFlags = NIF_INFO;
			wcsncpy_s(data.szInfoTitle, pf::utf8_to_utf16(title).c_str(), _TRUNCATE);
			wcsncpy_s(data.szInfo, pf::utf8_to_utf16(text).c_str(), _TRUNCATE);
			data.dwInfoFlags = kind == pf::tray_balloon_kind::warning
				                   ? NIIF_WARNING
				                   : kind == pf::tray_balloon_kind::error ? NIIF_ERROR : NIIF_INFO;
			if (Shell_NotifyIconW(NIM_MODIFY, &data) != FALSE) return true;
			_diagnostics = last_error_text();
			return false;
		}
	};
}

pf::tray_service_ptr pf::create_tray_service(const pf::tray_icon_image image,
                                              const std::string_view tooltip,
                                              pf::tray_callbacks callbacks)
{
	auto result = std::make_shared<win_tray_service>(image, tooltip, std::move(callbacks));
	result->show();
	return result;
}

void pf::test_set_tray_publication_failure(const bool fail)
{
	g_test_tray_publication_failure.store(fail, std::memory_order_relaxed);
}

bool pf::post_key_event(const pf::window_frame_ptr& window, const unsigned int key, const bool down)
{
	auto* const native = dynamic_cast<win_impl*>(window.get());
	if (!native || !IsWindow(native->m_hWnd)) return false;
	const LPARAM parameters = down ? 0 : (static_cast<LPARAM>(1) << 30) | (static_cast<LPARAM>(1) << 31);
	return PostMessageW(native->m_hWnd, down ? WM_KEYDOWN : WM_KEYUP, key, parameters) != FALSE;
}

bool pf::post_key_events(const pf::window_frame_ptr& window, const unsigned int key, const bool down)
{
	return post_key_event(window, key, down);
}

inline void set_font(const HWND h, HFONT f)
{
	SendMessage(h, WM_SETFONT, (WPARAM)f, 1);
}

inline void set_icon(const HWND h, HICON i)
{
	SendMessage(h, WM_SETICON, ICON_BIG, (LPARAM)i);
	SendMessage(h, WM_SETICON, ICON_SMALL, (LPARAM)i);
}

inline DWORD get_style(const HWND m_hWnd)
{
	return static_cast<DWORD>(::GetWindowLong(m_hWnd, GWL_STYLE));
}

inline BOOL center_window(const HWND m_hWnd, HWND hWndCenter = nullptr) noexcept
{
	// determine owner window to center against
	const DWORD dwStyle = get_style(m_hWnd);
	if (hWndCenter == nullptr)
	{
		if (dwStyle & WS_CHILD)
			hWndCenter = GetParent(m_hWnd);
		else
			hWndCenter = GetWindow(m_hWnd, GW_OWNER);
	}

	// get coordinates of the window relative to its parent
	RECT rcDlg;
	GetWindowRect(m_hWnd, &rcDlg);
	RECT rcArea;
	RECT rcCenter;
	const HWND hWndParent = hWndCenter;
	if (!(dwStyle & WS_CHILD))
	{
		// don't center against invisible or minimized windows
		if (hWndCenter != nullptr)
		{
			const DWORD dwStyleCenter = ::GetWindowLong(hWndCenter, GWL_STYLE);
			if (!(dwStyleCenter & WS_VISIBLE) || dwStyleCenter & WS_MINIMIZE)
				hWndCenter = nullptr;
		}

		// center within screen coordinates
		HMONITOR hMonitor = nullptr;
		if (hWndCenter != nullptr)
		{
			hMonitor = MonitorFromWindow(hWndCenter, MONITOR_DEFAULTTONEAREST);
		}
		else
		{
			hMonitor = MonitorFromWindow(m_hWnd, MONITOR_DEFAULTTONEAREST);
		}

		MONITORINFO minfo;
		minfo.cbSize = sizeof(MONITORINFO);
		BOOL bResult = ::GetMonitorInfo(hMonitor, &minfo);

		rcArea = minfo.rcWork;

		if (hWndCenter == nullptr)
			rcCenter = rcArea;
		else
			GetWindowRect(hWndCenter, &rcCenter);
	}
	else
	{
		// center within parent client coordinates
		GetClientRect(hWndParent, &rcArea);
		GetClientRect(hWndCenter, &rcCenter);
		MapWindowPoints(hWndCenter, hWndParent, (POINT*)&rcCenter, 2);
	}

	const int DlgWidth = rcDlg.right - rcDlg.left;
	const int DlgHeight = rcDlg.bottom - rcDlg.top;

	// find dialog's upper left based on rcCenter
	int xLeft = (rcCenter.left + rcCenter.right) / 2 - DlgWidth / 2;
	int yTop = (rcCenter.top + rcCenter.bottom) / 2 - DlgHeight / 2;

	// if the dialog is outside the screen, move it inside
	if (xLeft + DlgWidth > rcArea.right)
		xLeft = rcArea.right - DlgWidth;
	if (xLeft < rcArea.left)
		xLeft = rcArea.left;

	if (yTop + DlgHeight > rcArea.bottom)
		yTop = rcArea.bottom - DlgHeight;
	if (yTop < rcArea.top)
		yTop = rcArea.top;

	// map screen coordinates to child coordinates
	return SetWindowPos(m_hWnd, nullptr, xLeft, yTop, -1, -1,
	                    SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
}

static std::string window_text(const HWND h)
{
	const auto len = GetWindowTextLengthW(h);
	if (len == 0) return {};
	std::wstring wresult(len + 1, 0);
	GetWindowTextW(h, wresult.data(), len + 1);
	wresult.resize(len);
	return pf::utf16_to_utf8(wresult);
}

//  Platform API implementations â”€

//  Key binding formatting â”€

std::string pf::format_key_binding(const key_binding& kb)
{
	if (kb.empty()) return {};

	std::string result;

	if (kb.modifiers & key_mod::ctrl)
		result += "Ctrl+";
	if (kb.modifiers & key_mod::alt)
		result += "Alt+";
	if (kb.modifiers & key_mod::shift)
		result += "Shift+";

	switch (kb.key)
	{
	case platform_key::Back: result += "Backspace";
		break;
	case platform_key::Tab: result += "Tab";
		break;
	case platform_key::Return: result += "Enter";
		break;
	case platform_key::Escape: result += "Esc";
		break;
	case platform_key::Space: result += "Space";
		break;
	case platform_key::Prior: result += "Page Up";
		break;
	case platform_key::Next: result += "Page Down";
		break;
	case platform_key::End: result += "End";
		break;
	case platform_key::Home: result += "Home";
		break;
	case platform_key::Left: result += "Left";
		break;
	case platform_key::Up: result += "Up";
		break;
	case platform_key::Right: result += "Right";
		break;
	case platform_key::Down: result += "Down";
		break;
	case platform_key::Insert: result += "Ins";
		break;
	case platform_key::Delete: result += "Del";
		break;
	default:
		if (kb.key >= platform_key::F1 && kb.key <= platform_key::F12)
			result += std::format("F{}", kb.key - platform_key::F1 + 1);
		else if (kb.key >= 'A' && kb.key <= 'Z')
			result += static_cast<char>(kb.key);
		else if (kb.key >= '0' && kb.key <= '9')
			result += static_cast<char>(kb.key);
		else
			result += std::format("0x{:02X}", kb.key);
		break;
	}

	return result;
}

//  Font handles, native DC wrapper, URL resolver 

// A screen-compatible DC kept alive per thread. Acquiring a DC and realizing a
// font dominated text measurement, which runs once per text node per style pass.
struct measure_dc
{
	HDC hdc = CreateCompatibleDC(nullptr);
	HFONT original = nullptr;
	HFONT current = nullptr;
	std::wstring buffer;

	~measure_dc()
	{
		if (hdc)
		{
			if (original) SelectObject(hdc, original);
			DeleteDC(hdc);
		}
	}

	HDC with_font(const HFONT f)
	{
		if (!hdc) return nullptr;
		if (f != current)
		{
			const auto prev = static_cast<HFONT>(SelectObject(hdc, f));
			if (!original) original = prev;
			current = f;
		}
		return hdc;
	}

	void release_font(const HFONT f)
	{
		if (hdc && f == current)
		{
			SelectObject(hdc, original);
			current = nullptr;
		}
	}
};

static measure_dc& shared_measure_dc()
{
	thread_local measure_dc instance;
	return instance;
}

pf::font_handle pf::create_font_handle(const font_def& def, font_metrics_data* out_metrics)
{
	const auto wface = utf8_to_utf16(def.face);
	LOGFONTW lf = {};
	lf.lfHeight = def.size_mode == font_size_mode::cell ? def.size : -def.size;
	lf.lfWeight = def.weight;
	lf.lfItalic = def.italic ? TRUE : FALSE;
	lf.lfUnderline = def.underline ? TRUE : FALSE;
	lf.lfStrikeOut = def.strikeout ? TRUE : FALSE;
	lf.lfCharSet = DEFAULT_CHARSET;
	lf.lfOutPrecision = OUT_DEFAULT_PRECIS;
	lf.lfClipPrecision = CLIP_DEFAULT_PRECIS;
	lf.lfQuality = CLEARTYPE_QUALITY;
	lf.lfPitchAndFamily = DEFAULT_PITCH | FF_DONTCARE;
	wcsncpy_s(lf.lfFaceName, wface.c_str(), _TRUNCATE);

	const auto hfont = CreateFontIndirectW(&lf);
	if (!hfont) return 0;

	if (out_metrics)
	{
		const HDC hdc = GetDC(nullptr);
		const auto old = static_cast<HFONT>(SelectObject(hdc, hfont));
		TEXTMETRICW tm = {};
		GetTextMetricsW(hdc, &tm);
		out_metrics->height = tm.tmHeight;
		out_metrics->ascent = tm.tmAscent;
		out_metrics->descent = tm.tmDescent;
		out_metrics->x_height = tm.tmHeight;
		out_metrics->avg_char_width = tm.tmAveCharWidth;
		SelectObject(hdc, old);
		ReleaseDC(nullptr, hdc);
	}

	return std::bit_cast<font_handle>(hfont);
}

void pf::delete_font_handle(const font_handle h)
{
	if (!h) return;
	// GDI refuses to delete a font that is still selected into a DC.
	shared_measure_dc().release_font(std::bit_cast<HFONT>(h));
	DeleteObject(std::bit_cast<HFONT>(h));
}

pf::isize pf::measure_text_with_font(const font_handle h, const std::string_view text)
{
	if (!h) return {0, 0};
	auto& m = shared_measure_dc();
	const HDC hdc = m.with_font(std::bit_cast<HFONT>(h));
	if (!hdc) return {0, 0};
	utf8_to_utf16(text, m.buffer);
	SIZE sz = {0, 0};
	GetTextExtentPoint32W(hdc, m.buffer.c_str(), static_cast<int>(m.buffer.size()), &sz);
	return {sz.cx, sz.cy};
}

int pf::line_height_for_font(const font_handle h)
{
	if (!h) return 0;
	auto& m = shared_measure_dc();
	const HDC hdc = m.with_font(std::bit_cast<HFONT>(h));
	if (!hdc) return 0;
	TEXTMETRICW tm = {};
	GetTextMetricsW(hdc, &tm);
	return tm.tmHeight;
}

namespace pf
{
class pixel_surface_draw_context final : public pf::draw_context
{
	pf::pixel_surface& _surface;
	HDC _dc = nullptr;
	HBITMAP _bitmap = nullptr;
	HGDIOBJ _old_bitmap = nullptr;
	void* _bits = nullptr;
	std::unique_ptr<win_draw_context> _draw;

public:
	pixel_surface_draw_context(pf::pixel_surface& surface, const pf::irect clip)
		: _surface(surface)
	{
		BITMAPINFO info = {};
		info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
		info.bmiHeader.biWidth = surface.width;
		info.bmiHeader.biHeight = -surface.height;
		info.bmiHeader.biPlanes = 1;
		info.bmiHeader.biBitCount = 32;
		info.bmiHeader.biCompression = BI_RGB;

		_dc = CreateCompatibleDC(nullptr);
		if (!_dc) return;
		_bitmap = CreateDIBSection(_dc, &info, DIB_RGB_COLORS, &_bits, nullptr, 0);
		if (!_bitmap || !_bits) return;
		_old_bitmap = SelectObject(_dc, _bitmap);
		std::memcpy(_bits, surface.pixels.data(),
		            surface.pixels.size() * sizeof(uint32_t));
		_draw = std::make_unique<win_draw_context>(_dc, clip);
	}

	~pixel_surface_draw_context() override
	{
		_draw.reset();
		if (_bits && !_surface.pixels.empty())
			std::memcpy(_surface.pixels.data(), _bits, _surface.pixels.size() * sizeof(uint32_t));
		if (_dc && _old_bitmap) SelectObject(_dc, _old_bitmap);
		if (_bitmap) DeleteObject(_bitmap);
		if (_dc) DeleteDC(_dc);
	}

	bool valid() const { return _draw != nullptr; }

	irect clip_rect() const override { return _draw->clip_rect(); }
	void fill_solid_rect(const irect& rc, const color_t color) override { _draw->fill_solid_rect(rc, color); }
	void fill_solid_rect(const int x, const int y, const int cx, const int cy,
	                     const color_t color) override { _draw->fill_solid_rect(x, y, cx, cy, color); }
	void draw_text(const int x, const int y, const irect& clip, const std::string_view text,
	               const font& f, const color_t text_color, const color_t bg_color) override
	{
		_draw->draw_text(x, y, clip, text, f, text_color, bg_color);
	}
	isize measure_text(const std::string_view text, const font& f) const override
	{
		return _draw->measure_text(text, f);
	}
	void draw_text_h(const int x, const int y, const std::string_view text,
	                 const font_handle handle, const color_t text_color) override
	{
		_draw->draw_text_h(x, y, text, handle, text_color);
	}
	isize measure_text_h(const std::string_view text, const font_handle handle) const override
	{
		return _draw->measure_text_h(text, handle);
	}
	void draw_lines(const std::span<const ipoint> points, const color_t color) override
	{
		_draw->draw_lines(points, color);
	}
	void draw_solid_line(const ipoint a, const ipoint b, const color_t color, const int width) override
	{
		_draw->draw_solid_line(a, b, color, width);
	}
	void draw_ellipse(const int x, const int y, const int w, const int h,
	                  const color_t color, const int line_width) override
	{
		_draw->draw_ellipse(x, y, w, h, color, line_width);
	}
	void fill_ellipse(const int x, const int y, const int w, const int h,
	                  const color_t color) override
	{
		_draw->fill_ellipse(x, y, w, h, color);
	}
	void set_clip_rect(const irect& rc) override { _draw->set_clip_rect(rc); }
	void clear_clip_rect() override { _draw->clear_clip_rect(); }
	void draw_bitmap(const int x, const int y, const bitmap& bmp) override
	{
		_draw->draw_bitmap(x, y, bmp);
	}
	void draw_bitmap(const irect& dest, const bitmap& bmp) override
	{
		_draw->draw_bitmap(dest, bmp);
	}
	void fill_blend_rect(const irect& rc, const color_t color) override
	{
		_draw->fill_blend_rect(rc, color);
	}
	void draw_bitmap(const irect& dest, const bitmap& bmp, const bool alpha) override
	{
		_draw->draw_bitmap(dest, bmp, alpha);
	}
	void sync_surface_to_pixels() override
	{
		if (_bits && !_surface.pixels.empty())
			std::memcpy(_surface.pixels.data(), _bits, _surface.pixels.size() * sizeof(uint32_t));
	}
	void sync_pixels_to_surface() override
	{
		if (_bits && !_surface.pixels.empty())
			std::memcpy(_bits, _surface.pixels.data(), _surface.pixels.size() * sizeof(uint32_t));
	}
};

std::unique_ptr<draw_context> create_pixel_surface_context(pixel_surface& surface, const irect clip)
{
	if (surface.width <= 0 || surface.height <= 0 ||
		surface.pixels.size() != static_cast<size_t>(surface.width) * surface.height)
		return {};
	const irect actual_clip = clip.width() > 0 && clip.height() > 0
		                           ? clip
		                           : irect(0, 0, surface.width, surface.height);
	auto result = std::make_unique<pixel_surface_draw_context>(surface, actual_clip);
	if (!result->valid()) return {};
	return result;
}
}

namespace
{
	class win_shell_tree final : public pf::shell_tree
	{
		pf::window_frame_ptr _owner;
		pf::window_frame_ptr _frame;
		pf::shell_tree_options _options;
		pf::file_path _current;
		std::unordered_map<HTREEITEM, std::unique_ptr<pf::file_path>> _items;
		UINT_PTR _subclass_id = 0;

		HWND native(const pf::window_frame_ptr& frame) const
		{
			auto* const impl = dynamic_cast<win_impl*>(frame.get());
			return impl ? impl->m_hWnd : nullptr;
		}

		static LRESULT CALLBACK owner_subclass(HWND hwnd, UINT message, WPARAM wparam,
		                                       LPARAM lparam, UINT_PTR id, DWORD_PTR data)
		{
			auto* const self = reinterpret_cast<win_shell_tree*>(data);
			if (self && message == WM_NOTIFY)
			{
				const auto* const header = reinterpret_cast<const NMHDR*>(lparam);
				if (header && header->hwndFrom == self->native(self->_frame) &&
				    header->code == TVN_SELCHANGEDW)
				{
					const auto* const change = reinterpret_cast<const NMTREEVIEWW*>(lparam);
					const auto found = self->_items.find(change->itemNew.hItem);
					if (found != self->_items.end() && self->_options.selection_changed)
						self->_options.selection_changed(*found->second);
				}
			}
			return DefSubclassProc(hwnd, message, wparam, lparam);
		}

		static LRESULT CALLBACK tree_subclass(HWND hwnd, UINT message, WPARAM wparam,
		                                      LPARAM lparam, UINT_PTR id, DWORD_PTR data)
		{
			auto* const self = reinterpret_cast<win_shell_tree*>(data);
			if (self && message == WM_DROPFILES)
			{
				const auto drop = reinterpret_cast<HDROP>(wparam);
				const auto paths = decode_dropped_files(drop);
				if (self->_options.files_dropped)
					self->_options.files_dropped(self->_current, paths, false);
				return 0;
			}
			return DefSubclassProc(hwnd, message, wparam, lparam);
		}

		void add_item(const pf::file_path& path, const std::wstring_view label,
		              HTREEITEM parent, const bool has_children)
		{
			auto stored = std::make_unique<pf::file_path>(path);
			TVINSERTSTRUCTW insert = {};
			insert.hParent = parent;
			insert.hInsertAfter = TVI_LAST;
			insert.item.mask = TVIF_TEXT | TVIF_PARAM | (has_children ? TVIF_CHILDREN : 0);
			insert.item.pszText = const_cast<LPWSTR>(label.data());
			insert.item.cChildren = has_children ? 1 : 0;
			insert.item.lParam = reinterpret_cast<LPARAM>(stored.get());
			const auto item = TreeView_InsertItem(native(_frame), &insert);
			if (item) _items.emplace(item, std::move(stored));
		}

	public:
		win_shell_tree(const pf::window_frame_ptr& owner, pf::shell_tree_options options)
			: _owner(owner), _options(std::move(options))
		{
			if (!_owner) return;
			_frame = _owner->create_child("SysTreeView32",
			                              pf::window_style::child | pf::window_style::visible,
			                              {});
			if (!_frame) return;
			static std::atomic_uintptr_t next_id = 1;
			_subclass_id = next_id.fetch_add(1);
			if (const auto owner_hwnd = native(_owner))
				SetWindowSubclass(owner_hwnd, owner_subclass, _subclass_id,
				                  reinterpret_cast<DWORD_PTR>(this));
			if (const auto tree_hwnd = native(_frame))
			{
				SetWindowSubclass(tree_hwnd, tree_subclass, _subclass_id,
				                  reinterpret_cast<DWORD_PTR>(this));
				DragAcceptFiles(tree_hwnd, TRUE);
			}
		}

		~win_shell_tree() override
		{
			if (const auto owner_hwnd = native(_owner))
				RemoveWindowSubclass(owner_hwnd, owner_subclass, _subclass_id);
			if (const auto tree_hwnd = native(_frame))
			{
				DragAcceptFiles(tree_hwnd, FALSE);
				RemoveWindowSubclass(tree_hwnd, tree_subclass, _subclass_id);
			}
		}

		pf::window_frame_ptr frame() override { return _frame; }
		void set_bounds(const pf::irect bounds) override
		{
			if (_frame) _frame->move_window(bounds);
		}
		void show(const bool visible) override
		{
			if (_frame) _frame->show(visible);
		}
		void set_focus() override
		{
			if (_frame) _frame->set_focus();
		}
		bool has_focus() const override
		{
			return _frame && _frame->has_focus();
		}
		void set_current_path(const pf::file_path path) override
		{
			_current = path;
			refresh();
		}
		void refresh() override
		{
			const auto tree = native(_frame);
			if (!tree) return;
			TreeView_DeleteAllItems(tree);
			_items.clear();
			if (_current.empty()) return;
			const auto root = _current.name();
			add_item(_current, pf::utf8_to_utf16(root), TVI_ROOT, true);
			const auto contents = pf::iterate_file_items(_current, false);
			for (const auto& folder : contents.folders)
				add_item(folder.path, pf::utf8_to_utf16(folder.path.name()), TVI_ROOT, true);
			for (const auto& file : contents.files)
				add_item(file.path, pf::utf8_to_utf16(file.path.name()), TVI_ROOT, false);
			if (_options.files_changed) _options.files_changed();
		}
	};
}

pf::shell_tree_ptr pf::create_shell_tree(const pf::window_frame_ptr& owner,
                                         pf::shell_tree_options options)
{
	auto result = std::make_shared<win_shell_tree>(owner, std::move(options));
	return result->frame() ? result : nullptr;
}

namespace
{
	class drag_format_enum final : public IEnumFORMATETC
	{
		ULONG _refs = 1;
		std::vector<FORMATETC> _formats;
		ULONG _index = 0;

	public:
		explicit drag_format_enum(std::vector<FORMATETC> formats) : _formats(std::move(formats)) {}

		HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** object) override
		{
			if (!object) return E_POINTER;
			*object = nullptr;
			if (iid == IID_IUnknown || iid == IID_IEnumFORMATETC)
			{
				*object = static_cast<IEnumFORMATETC*>(this);
				AddRef();
				return S_OK;
			}
			return E_NOINTERFACE;
		}
		ULONG STDMETHODCALLTYPE AddRef() override { return ++_refs; }
		ULONG STDMETHODCALLTYPE Release() override
		{
			const auto refs = --_refs;
			if (!refs) delete this;
			return refs;
		}
		HRESULT STDMETHODCALLTYPE Next(ULONG count, FORMATETC* result, ULONG* fetched) override
		{
			if (!result) return E_POINTER;
			ULONG copied = 0;
			while (copied < count && _index < _formats.size())
			{
				result[copied++] = _formats[_index++];
			}
			if (fetched) *fetched = copied;
			return copied == count ? S_OK : S_FALSE;
		}
		HRESULT STDMETHODCALLTYPE Skip(ULONG count) override
		{
			_index = std::min<ULONG>(_index + count, static_cast<ULONG>(_formats.size()));
			return _index == _formats.size() ? S_FALSE : S_OK;
		}
		HRESULT STDMETHODCALLTYPE Reset() override { _index = 0; return S_OK; }
		HRESULT STDMETHODCALLTYPE Clone(IEnumFORMATETC** result) override
		{
			if (!result) return E_POINTER;
			auto copy = std::make_unique<drag_format_enum>(_formats);
			copy->_index = _index;
			*result = copy.release();
			return S_OK;
		}
	};

	class file_drag_data_object final : public IDataObject
	{
		ULONG _refs = 1;
		std::vector<std::wstring> _paths;
		UINT _preferred_effect = 0;

		HGLOBAL make_hdrop() const
		{
			size_t chars = 1;
			for (const auto& path : _paths) chars += path.size() + 1;
			const auto bytes = sizeof(DROPFILES) + chars * sizeof(wchar_t);
			const auto memory = GlobalAlloc(GMEM_MOVEABLE | GMEM_ZEROINIT, bytes);
			if (!memory) return nullptr;
			auto* const drop = static_cast<DROPFILES*>(GlobalLock(memory));
			if (!drop)
			{
				GlobalFree(memory);
				return nullptr;
			}
			drop->pFiles = sizeof(DROPFILES);
			drop->fWide = TRUE;
			auto* target = reinterpret_cast<wchar_t*>(reinterpret_cast<uint8_t*>(drop) + sizeof(DROPFILES));
			for (const auto& path : _paths)
			{
				std::copy(path.begin(), path.end(), target);
				target += path.size() + 1;
			}
			GlobalUnlock(memory);
			return memory;
		}

		static FORMATETC drop_format()
		{
			return {CF_HDROP, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL};
		}
		FORMATETC effect_format() const
		{
			return {static_cast<CLIPFORMAT>(_preferred_effect), nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL};
		}

	public:
		explicit file_drag_data_object(const std::span<const pf::file_path> files)
			: _preferred_effect(RegisterClipboardFormatW(CFSTR_PREFERREDDROPEFFECT))
		{
			_paths.reserve(files.size());
			for (const auto& path : files)
				_paths.push_back(pf::utf8_to_utf16(path.view()));
		}

		HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** object) override
		{
			if (!object) return E_POINTER;
			*object = nullptr;
			if (iid == IID_IUnknown || iid == IID_IDataObject)
			{
				*object = static_cast<IDataObject*>(this);
				AddRef();
				return S_OK;
			}
			return E_NOINTERFACE;
		}
		ULONG STDMETHODCALLTYPE AddRef() override { return ++_refs; }
		ULONG STDMETHODCALLTYPE Release() override
		{
			const auto refs = --_refs;
			if (!refs) delete this;
			return refs;
		}
		HRESULT STDMETHODCALLTYPE GetData(FORMATETC* format, STGMEDIUM* medium) override
		{
			if (!format || !medium) return E_POINTER;
			if (format->tymed != TYMED_HGLOBAL || format->dwAspect != DVASPECT_CONTENT)
				return DV_E_FORMATETC;
			ZeroMemory(medium, sizeof(*medium));
			medium->tymed = TYMED_HGLOBAL;
			if (format->cfFormat == CF_HDROP)
				medium->hGlobal = make_hdrop();
			else if (format->cfFormat == _preferred_effect)
			{
				medium->hGlobal = GlobalAlloc(GMEM_MOVEABLE, sizeof(DWORD));
				if (medium->hGlobal)
				{
					auto* const effect = static_cast<DWORD*>(GlobalLock(medium->hGlobal));
					if (effect) *effect = DROPEFFECT_COPY | DROPEFFECT_MOVE;
					GlobalUnlock(medium->hGlobal);
				}
			}
			else
				return DV_E_FORMATETC;
			if (!medium->hGlobal) return E_OUTOFMEMORY;
			return S_OK;
		}
		HRESULT STDMETHODCALLTYPE GetDataHere(FORMATETC*, STGMEDIUM*) override { return DATA_E_FORMATETC; }
		HRESULT STDMETHODCALLTYPE QueryGetData(FORMATETC* format) override
		{
			if (!format || format->tymed != TYMED_HGLOBAL || format->dwAspect != DVASPECT_CONTENT)
				return DV_E_FORMATETC;
			return format->cfFormat == CF_HDROP || format->cfFormat == _preferred_effect
				       ? S_OK
				       : DV_E_FORMATETC;
		}
		HRESULT STDMETHODCALLTYPE GetCanonicalFormatEtc(FORMATETC*, FORMATETC* result) override
		{
			if (!result) return E_POINTER;
			result->ptd = nullptr;
			return E_NOTIMPL;
		}
		HRESULT STDMETHODCALLTYPE SetData(FORMATETC*, STGMEDIUM*, BOOL) override { return E_NOTIMPL; }
		HRESULT STDMETHODCALLTYPE EnumFormatEtc(DWORD direction, IEnumFORMATETC** result) override
		{
			if (!result) return E_POINTER;
			if (direction != DATADIR_GET) return E_NOTIMPL;
			*result = new drag_format_enum({drop_format(), effect_format()});
			return S_OK;
		}
		HRESULT STDMETHODCALLTYPE DAdvise(FORMATETC*, DWORD, IAdviseSink*, DWORD*) override { return OLE_E_ADVISENOTSUPPORTED; }
		HRESULT STDMETHODCALLTYPE DUnadvise(DWORD) override { return OLE_E_ADVISENOTSUPPORTED; }
		HRESULT STDMETHODCALLTYPE EnumDAdvise(IEnumSTATDATA**) override { return OLE_E_ADVISENOTSUPPORTED; }
	};

	class file_drag_source final : public IDropSource
	{
		ULONG _refs = 1;

	public:
		HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** object) override
		{
			if (!object) return E_POINTER;
			*object = nullptr;
			if (iid == IID_IUnknown || iid == IID_IDropSource)
			{
				*object = static_cast<IDropSource*>(this);
				AddRef();
				return S_OK;
			}
			return E_NOINTERFACE;
		}
		ULONG STDMETHODCALLTYPE AddRef() override { return ++_refs; }
		ULONG STDMETHODCALLTYPE Release() override
		{
			const auto refs = --_refs;
			if (!refs) delete this;
			return refs;
		}
		HRESULT STDMETHODCALLTYPE QueryContinueDrag(BOOL escape, DWORD key_state) override
		{
			if (escape) return DRAGDROP_S_CANCEL;
			if (!(key_state & (MK_LBUTTON | MK_RBUTTON | MK_MBUTTON))) return DRAGDROP_S_DROP;
			return S_OK;
		}
		HRESULT STDMETHODCALLTYPE GiveFeedback(DWORD) override { return DRAGDROP_S_USEDEFAULTCURSORS; }
	};
}

bool pf::begin_file_drag(const pf::window_frame_ptr&, const std::span<const pf::file_path> files)
{
	if (files.empty()) return false;
	const auto ole = OleInitialize(nullptr);
	if (FAILED(ole)) return false;
	auto* const data = new file_drag_data_object(files);
	auto* const source = new file_drag_source();
	DWORD effect = DROPEFFECT_NONE;
	const auto result = DoDragDrop(data, source, DROPEFFECT_COPY | DROPEFFECT_MOVE, &effect);
	data->Release();
	source->Release();
	OleUninitialize();
	return result == DRAGDROP_S_DROP && effect != DROPEFFECT_NONE;
}

namespace
{
	class win_text_input final : public pf::text_input
	{
		pf::window_frame_ptr _owner;
		pf::window_frame_ptr _frame;
		pf::text_input_options _options;
		pf::font_handle _font = 0;
		UINT_PTR _subclass_id = 0;

		HWND native(const pf::window_frame_ptr& frame) const
		{
			auto* const impl = dynamic_cast<win_impl*>(frame.get());
			return impl ? impl->m_hWnd : nullptr;
		}

		static LRESULT CALLBACK owner_subclass(HWND hwnd, UINT message, WPARAM wparam,
		                                       LPARAM lparam, UINT_PTR id, DWORD_PTR data)
		{
			auto* const self = reinterpret_cast<win_text_input*>(data);
			if (self && message == WM_COMMAND &&
			    reinterpret_cast<HWND>(lparam) == self->native(self->_frame) &&
			    HIWORD(wparam) == EN_CHANGE && self->_options.changed)
				self->_options.changed(self->text());
			return DefSubclassProc(hwnd, message, wparam, lparam);
		}

		static LRESULT CALLBACK edit_subclass(HWND hwnd, UINT message, WPARAM wparam,
		                                      LPARAM lparam, UINT_PTR id, DWORD_PTR data)
		{
			auto* const self = reinterpret_cast<win_text_input*>(data);
			if (self && message == WM_KEYDOWN)
			{
				if (wparam == VK_RETURN && self->_options.accepted)
				{
					self->_options.accepted(self->text());
					return 0;
				}
				if (wparam == VK_ESCAPE && self->_options.escaped)
				{
					self->_options.escaped();
					return 0;
				}
				if (wparam == VK_TAB && self->_options.tabbed)
				{
					self->_options.tabbed((GetKeyState(VK_SHIFT) & 0x8000) != 0);
					return 0;
				}
			}
			return DefSubclassProc(hwnd, message, wparam, lparam);
		}

	public:
		win_text_input(const pf::window_frame_ptr& owner, pf::text_input_options options)
			: _owner(owner), _options(std::move(options))
		{
			if (!owner) return;
			_frame = owner->create_child("EDIT", pf::window_style::child | pf::window_style::visible, {});
			if (!_frame) return;
			static std::atomic_uintptr_t next_id = 1;
			_subclass_id = next_id.fetch_add(1);
			const auto edit = native(_frame);
			if (!edit) return;
			SetWindowLongPtrW(edit, GWL_STYLE,
			                  GetWindowLongPtrW(edit, GWL_STYLE) | WS_TABSTOP | ES_AUTOHSCROLL);
			if (!_options.cue.empty())
				SendMessageW(edit, EM_SETCUEBANNER, TRUE,
				             reinterpret_cast<LPARAM>(pf::utf8_to_utf16(_options.cue).c_str()));
			SetWindowSubclass(edit, edit_subclass, _subclass_id, reinterpret_cast<DWORD_PTR>(this));
			if (const auto hwnd = native(_owner))
				SetWindowSubclass(hwnd, owner_subclass, _subclass_id, reinterpret_cast<DWORD_PTR>(this));
		}

		~win_text_input() override
		{
			if (const auto hwnd = native(_owner))
				RemoveWindowSubclass(hwnd, owner_subclass, _subclass_id);
			if (const auto edit = native(_frame))
			{
				RemoveWindowSubclass(edit, edit_subclass, _subclass_id);
				DestroyWindow(edit);
			}
			if (_font) pf::delete_font_handle(_font);
		}

		pf::window_frame_ptr frame() override { return _frame; }
		void set_bounds(const pf::irect bounds) override { if (_frame) _frame->move_window(bounds); }
		void show(const bool visible) override { if (_frame) _frame->show(visible); }
		void set_focus() override { if (_frame) _frame->set_focus(); }
		bool has_focus() const override { return _frame && _frame->has_focus(); }
		std::string text() const override
		{
			const auto edit = native(_frame);
			if (!edit) return {};
			const auto length = GetWindowTextLengthW(edit);
			std::wstring value(static_cast<size_t>(length) + 1, L'\0');
			if (length) GetWindowTextW(edit, value.data(), length + 1);
			value.resize(static_cast<size_t>(length));
			return pf::utf16_to_utf8(value);
		}
		void set_text(const std::string_view value) override
		{
			if (const auto edit = native(_frame))
				SetWindowTextW(edit, pf::utf8_to_utf16(value).c_str());
		}
		void set_font(const pf::font_def& definition) override
		{
			const auto next = pf::create_font_handle(definition);
			if (!next) return;
			if (const auto edit = native(_frame))
				SendMessageW(edit, WM_SETFONT, static_cast<WPARAM>(next), TRUE);
			if (_font) pf::delete_font_handle(_font);
			_font = next;
		}
	};
}

pf::text_input_ptr pf::create_text_input(const pf::window_frame_ptr& owner,
                                         pf::text_input_options options)
{
	auto result = std::make_shared<win_text_input>(owner, std::move(options));
	return result->frame() ? result : nullptr;
}

std::unique_ptr<pf::draw_context> pf::wrap_native_dc(const uintptr_t native_dc, const irect& clip)
{
	return std::make_unique<win_draw_context>(std::bit_cast<HDC>(native_dc), clip);
}

std::unique_ptr<pf::measure_context> pf::wrap_native_dc_measure(const uintptr_t native_dc)
{
	return std::make_unique<win_measure_context>(std::bit_cast<HDC>(native_dc));
}

std::string pf::resolve_url(const std::string_view base, const std::string_view rel)
{
	const auto wrel = utf8_to_utf16(rel);
	const auto wbase = utf8_to_utf16(base);
	std::wstring wresult;

	if (PathIsRelativeW(wrel.c_str()) && !PathIsURLW(wrel.c_str()))
	{
		wchar_t abs_url[2084]; // INTERNET_MAX_URL_LENGTH
		DWORD dl = std::size(abs_url);
		if (UrlCombineW(wbase.c_str(), wrel.c_str(), abs_url, &dl, 0) == S_OK)
			wresult.assign(abs_url, dl);
	}
	else if (PathIsURLW(wrel.c_str()))
	{
		wresult = wrel;
	}
	else
	{
		wchar_t abs_url[2084]; // INTERNET_MAX_URL_LENGTH
		DWORD dl = std::size(abs_url);
		if (UrlCreateFromPathW(wrel.c_str(), abs_url, &dl, 0) == S_OK)
			wresult.assign(abs_url, dl);
	}

	auto result = utf16_to_utf8(wresult);
	if (result.starts_with("file:///"))
		result.erase(5, 1);
	if (result.starts_with("file://"))
		result.erase(0, 7);
	return result;
}

uint32_t pf::charset_to_codepage(const std::string_view charset)
{
	struct entry
	{
		const char* name;
		uint32_t cp;
	};

	// Only the labels that actually show up on the web. Anything absent is
	// treated as UTF-8 by the caller.
	static constexpr entry table[] = {
		{"utf-8", CP_UTF8}, {"utf8", CP_UTF8}, {"us-ascii", CP_UTF8}, {"ascii", CP_UTF8},
		{"iso-8859-1", 28591}, {"latin1", 28591}, {"l1", 28591}, {"iso8859-1", 28591},
		{"windows-1250", 1250}, {"windows-1251", 1251}, {"windows-1252", 1252},
		{"windows-1253", 1253}, {"windows-1254", 1254}, {"windows-1255", 1255},
		{"windows-1256", 1256}, {"windows-1257", 1257}, {"windows-1258", 1258},
		{"cp1250", 1250}, {"cp1251", 1251}, {"cp1252", 1252},
		{"iso-8859-2", 28592}, {"iso-8859-3", 28593}, {"iso-8859-4", 28594},
		{"iso-8859-5", 28595}, {"iso-8859-6", 28596}, {"iso-8859-7", 28597},
		{"iso-8859-8", 28598}, {"iso-8859-9", 28599}, {"iso-8859-13", 28603},
		{"iso-8859-15", 28605},
		{"koi8-r", 20866}, {"koi8-u", 21866},
		{"shift_jis", 932}, {"shift-jis", 932}, {"sjis", 932}, {"ms_kanji", 932},
		{"euc-jp", 20932}, {"iso-2022-jp", 50220},
		{"gb2312", 936}, {"gbk", 936}, {"gb18030", 54936}, {"big5", 950},
		{"euc-kr", 949}, {"ks_c_5601-1987", 949},
		{"windows-874", 874}, {"tis-620", 874},
	};

	for (const auto& [name, cp] : table)
	{
		if (icmp(charset, name) == 0) return cp;
	}
	return 0;
}

std::string pf::transcode_to_utf8(const std::string_view bytes, const uint32_t codepage)
{
	if (codepage == 0 || codepage == CP_UTF8 || bytes.empty())
		return std::string(bytes);

	// MultiByteToWideChar rejects the UTF-16 code pages, so reinterpret those.
	if (codepage == 1200 || codepage == 1201)
	{
		const size_t units = bytes.size() / 2;
		std::wstring wide(units, L'\0');

		for (size_t i = 0; i < units; ++i)
		{
			const auto lo = static_cast<uint8_t>(bytes[i * 2]);
			const auto hi = static_cast<uint8_t>(bytes[i * 2 + 1]);
			wide[i] = static_cast<wchar_t>(codepage == 1200 ? lo | hi << 8 : hi | lo << 8);
		}

		return utf16_to_utf8(wide);
	}

	const auto len = static_cast<int>(bytes.size());
	const int wide_len = MultiByteToWideChar(codepage, 0, bytes.data(), len, nullptr, 0);
	if (wide_len <= 0) return std::string(bytes);

	std::wstring wide(wide_len, L'\0');
	MultiByteToWideChar(codepage, 0, bytes.data(), len, wide.data(), wide_len);
	return utf16_to_utf8(wide);
}

//  Cursor position (global) â”€

pf::ipoint pf::platform_cursor_pos()
{
	POINT pt;
	GetCursorPos(&pt);
	return {pt.x, pt.y};
}

pf::isize pf::platform_screen_size()
{
	return {GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN)};
}

int pf::platform_screen_dpi()
{
	const HDC hdc = GetDC(nullptr);
	const auto dpi = GetDeviceCaps(hdc, LOGPIXELSY);
	ReleaseDC(nullptr, hdc);
	return dpi > 0 ? dpi : 96;
}

std::string pf::platform_load_text_resource(const int id)
{
	const auto hinst = GetModuleHandleW(nullptr);
	const auto hrsrc = FindResourceW(hinst, MAKEINTRESOURCEW(id), RT_HTML);
	if (!hrsrc) return {};
	const auto hg = LoadResource(hinst, hrsrc);
	if (!hg) return {};
	const auto data = static_cast<const char*>(LockResource(hg));
	const auto size = SizeofResource(hinst, hrsrc);
	if (!data || size == 0) return {};
	return std::string(data, size);
}

//  Timer 


double pf::platform_get_time()
{
	LARGE_INTEGER now;
	QueryPerformanceCounter(&now);
	return static_cast<double>(now.QuadPart - g_perfStart.QuadPart)
		/ static_cast<double>(g_perfFreq.QuadPart);
}

uint64_t pf::platform_uptime_ms()
{
	return GetTickCount64();
}

void pf::platform_sleep(const int milliseconds)
{
	Sleep(static_cast<DWORD>(milliseconds));
}

//  Resource Loading â”€

void* pf::platform_load_resource(const std::string_view name, const std::string_view type)
{
	const auto wname = utf8_to_utf16(name);
	const auto wtype = utf8_to_utf16(type);
	LPCWSTR resType = wtype.c_str();
	if (type == "BITMAP"sv)
		resType = RT_BITMAP;

	const HRSRC hResInfo = FindResourceW(nullptr, wname.c_str(), resType);
	if (!hResInfo) return nullptr;

	const HGLOBAL hResData = LoadResource(nullptr, hResInfo);
	if (!hResData) return nullptr;

	return LockResource(hResData);
}

std::optional<pf::bitmap_data> pf::platform_load_bitmap_resource(const std::string_view resName)
{
	const auto pData = static_cast<const uint8_t*>(platform_load_resource(resName, "BITMAP"));
	if (!pData) return std::nullopt;

	// Determine resource size so we can bounds-check.
	const auto wname = utf8_to_utf16(resName);
	const HRSRC hres = FindResourceW(nullptr, wname.c_str(), RT_BITMAP);
	const DWORD res_size = hres ? SizeofResource(nullptr, hres) : 0;
	if (res_size < sizeof(BITMAPINFOHEADER)) return std::nullopt;

	const auto bih = reinterpret_cast<const BITMAPINFOHEADER*>(pData);
	if (bih->biSize < sizeof(BITMAPINFOHEADER) || bih->biSize > res_size) return std::nullopt;
	const int w = bih->biWidth;
	const int h = abs(bih->biHeight);
	if (w <= 0 || h <= 0 || w > 0x10000 || h > 0x10000) return std::nullopt;
	const bool topDown = bih->biHeight < 0;
	const int bpp = bih->biBitCount;

	int paletteSize = 0;
	if (bpp <= 8)
	{
		const DWORD entries = bih->biClrUsed ? bih->biClrUsed : (1u << bpp);
		if (entries > 256) return std::nullopt;
		paletteSize = static_cast<int>(entries * sizeof(RGBQUAD));
	}

	const int srcStride = (w * bpp + 31) / 32 * 4;
	const size_t pixel_bytes = static_cast<size_t>(srcStride) * h;
	if (bih->biSize + paletteSize + pixel_bytes > res_size) return std::nullopt;

	const uint8_t* pixelData = pData + bih->biSize + paletteSize;
	const RGBQUAD* palette = bpp <= 8 ? reinterpret_cast<const RGBQUAD*>(pData + bih->biSize) : nullptr;

	std::vector<uint32_t> pixels(static_cast<size_t>(w) * static_cast<size_t>(h), 0);

	for (int y = 0; y < h; y++)
	{
		const int srcY = topDown ? y : h - 1 - y;
		const uint8_t* srcRow = pixelData + static_cast<size_t>(srcY) * srcStride;

		for (int x = 0; x < w; x++)
		{
			uint32_t c = 0;
			if (bpp == 24)
			{
				c = xrgb(srcRow[x * 3 + 2], srcRow[x * 3 + 1], srcRow[x * 3 + 0]);
			}
			else if (bpp == 32)
			{
				c = *reinterpret_cast<const uint32_t*>(srcRow + x * 4) | 0xFF000000;
			}
			else if (bpp == 8 && palette)
			{
				const auto& p = palette[srcRow[x]];
				c = xrgb(p.rgbRed, p.rgbGreen, p.rgbBlue);
			}
			else if (bpp == 4 && palette)
			{
				const uint8_t idx = x & 1 ? srcRow[x / 2] & 0x0F : srcRow[x / 2] >> 4;
				c = xrgb(palette[idx].rgbRed, palette[idx].rgbGreen, palette[idx].rgbBlue);
			}
			pixels[static_cast<size_t>(y) * w + x] = c;
		}
	}

	return bitmap_data{w, h, pixels};
}

//  Utility 

void pf::platform_show_error(const std::string_view message, const std::string_view title)
{
	MessageBoxW(nullptr, utf8_to_utf16(message).c_str(), utf8_to_utf16(title).c_str(), MB_OK);
}

void pf::debug_trace(const std::string& msg)
{
#ifdef _DEBUG
	OutputDebugStringW(pf::utf8_to_utf16(msg).c_str());
#else
	(void)msg;
#endif
}

namespace
{
	void ensure_cli_stdout_bound()
	{
		const auto stdout_handle = GetStdHandle(STD_OUTPUT_HANDLE);
		if (stdout_handle != nullptr && stdout_handle != INVALID_HANDLE_VALUE)
		{
			SetLastError(NO_ERROR);
			const auto file_type = GetFileType(stdout_handle);
			if (file_type != FILE_TYPE_UNKNOWN || GetLastError() == NO_ERROR)
				return;
		}

		if (AttachConsole(ATTACH_PARENT_PROCESS))
		{
			FILE* dummy = nullptr;
			_wfreopen_s(&dummy, L"CONOUT$", L"w", stdout);
			_wfreopen_s(&dummy, L"CONOUT$", L"w", stderr);

			// AttachConsole leaves the std handles null, which would send every
			// write down write_stdout's fallback path.
			const auto out = CreateFileW(L"CONOUT$", GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
			                             nullptr, OPEN_EXISTING, 0, nullptr);
			if (out != INVALID_HANDLE_VALUE)
			{
				SetStdHandle(STD_OUTPUT_HANDLE, out);
				SetStdHandle(STD_ERROR_HANDLE, out);
			}
		}
	}
}

void pf::attach_console()
{
	ensure_cli_stdout_bound();
}

void pf::write_stdout(const std::string_view text)
{
	ensure_cli_stdout_bound();

	const auto stdout_handle = GetStdHandle(STD_OUTPUT_HANDLE);
	if (stdout_handle != nullptr && stdout_handle != INVALID_HANDLE_VALUE)
	{
		DWORD written = 0;
		if (WriteFile(stdout_handle, text.data(), static_cast<DWORD>(text.size()), &written, nullptr))
			return;
	}

	fwrite(text.data(), 1, text.size(), stdout);
	fflush(stdout);
}

size_t pf::read_stdin(char* const buffer, const size_t capacity)
{
	if (buffer == nullptr || capacity == 0) return 0;

	const auto stdin_handle = GetStdHandle(STD_INPUT_HANDLE);
	if (stdin_handle != nullptr && stdin_handle != INVALID_HANDLE_VALUE)
	{
		DWORD read = 0;
		if (ReadFile(stdin_handle, buffer, static_cast<DWORD>(capacity), &read, nullptr))
			return static_cast<size_t>(read);

		// A closed pipe is end of input, not a failure worth reporting.
		if (GetLastError() == ERROR_BROKEN_PIPE) return 0;
	}

	return fread(buffer, 1, capacity, stdin);
}

//  Sound â€” WAV resource helpers â”€

//  Menu & Accelerators 

static std::wstring menu_display_text(const pf::menu_command& item)
{
	auto text = pf::utf8_to_utf16(item.text);
	if (!item.accel.empty())
	{
		text += L'\t';
		text += pf::utf8_to_utf16(pf::format_key_binding(item.accel));
	}
	return text;
}

static HMENU build_popup_menu(const std::vector<pf::menu_command>& items)
{
	const HMENU hMenu = CreatePopupMenu();
	for (auto& item : items)
	{
		if (item.text.empty() && item.children.empty())
			AppendMenuW(hMenu, MF_SEPARATOR, 0, nullptr);
		else if (!item.children.empty())
			AppendMenuW(hMenu, MF_POPUP, reinterpret_cast<UINT_PTR>(build_popup_menu(item.children)),
			            menu_display_text(item).c_str());
		else
			AppendMenuW(hMenu, MF_STRING, item.id, menu_display_text(item).c_str());
	}
	return hMenu;
}

void pf::platform_set_menu(std::vector<menu_command> menuDef)
{
	g_menuDef = std::move(menuDef);
	if (g_hMenu)
	{
		SetMenu(g_hWnd, nullptr);
		DestroyMenu(g_hMenu);
	}
	g_hMenu = CreateMenu();
	for (auto& top : g_menuDef)
	{
		if (!top.children.empty())
			AppendMenuW(g_hMenu, MF_POPUP, reinterpret_cast<UINT_PTR>(build_popup_menu(top.children)),
			            utf8_to_utf16(top.text).c_str());
		else
			AppendMenuW(g_hMenu, MF_STRING, top.id, utf8_to_utf16(top.text).c_str());
	}
	if (g_hWnd)
		SetMenu(g_hWnd, g_hMenu);

	build_runtime_accelerators();
}


bool pf::platform_events()
{
	MSG msg = {};
	while (PeekMessage(&msg, nullptr, 0, 0, PM_REMOVE))
	{
		if (msg.message == WM_QUIT)
			return false;
		if (!TranslateAccelerator(g_hWnd, g_hAccel, &msg))
		{
			TranslateMessage(&msg);
			DispatchMessage(&msg);
		}
	}
	return true;
}

//  Message Loop â”€

static HANDLE exit_h = nullptr;

namespace
{
	struct task_queue_state
	{
		std::mutex mutex;
		std::condition_variable ready;
		std::condition_variable finished;
		std::deque<std::function<void()>> tasks;
		bool accepting = true;
		size_t workers = 0;
	};

	void invoke_task(std::function<void()>& task)
	{
		try { task(); }
		catch (const std::exception& e)
		{
			pf::debug_trace("Task exception: " + std::string(e.what()) + "\n");
		}
		catch (...)
		{
			pf::debug_trace("Task: unknown exception\n");
		}
	}

	void consume_tasks(const std::shared_ptr<task_queue_state>& state)
	{
		for (;;)
		{
			std::function<void()> task;
			{
				std::unique_lock lock(state->mutex);
				state->ready.wait(lock, [&] { return !state->accepting || !state->tasks.empty(); });
				if (state->tasks.empty())
				{
					--state->workers;
					state->finished.notify_all();
					return;
				}
				task = std::move(state->tasks.front());
				state->tasks.pop_front();
			}
			invoke_task(task);
		}
	}

	bool enqueue_task(const std::shared_ptr<task_queue_state>& state, std::function<void()> task)
	{
		if (!task) return false;
		{
			std::lock_guard lock(state->mutex);
			if (!state->accepting) return false;
			state->tasks.push_back(std::move(task));
		}
		state->ready.notify_one();
		return true;
	}

	void stop_queue(const std::shared_ptr<task_queue_state>& state)
	{
		{
			std::lock_guard lock(state->mutex);
			state->accepting = false;
		}
		state->ready.notify_all();
	}

	class win_serial_executor final : public pf::serial_executor
	{
		const std::shared_ptr<task_queue_state> _state = std::make_shared<task_queue_state>();
		std::mutex _join_mutex;
		std::thread _worker;
		std::thread::id _worker_id;

	public:
		win_serial_executor()
		{
			_state->workers = 1;
			_worker = std::thread([state = _state] { consume_tasks(state); });
			_worker_id = _worker.get_id();
		}

		~win_serial_executor() override
		{
			stop();
			if (_worker.joinable()) _worker.detach();
		}

		bool post(std::function<void()> task) override
		{
			const auto state = _state;
			return enqueue_task(state, std::move(task));
		}

		void stop() override
		{
			stop_queue(_state);
			if (std::this_thread::get_id() == _worker_id) return;
			std::lock_guard lock(_join_mutex);
			if (_worker.joinable()) _worker.join();
		}
	};

	class async_task_pool
	{
		const std::shared_ptr<task_queue_state> _state = std::make_shared<task_queue_state>();
		std::vector<std::thread> _workers;
		std::mutex _stop_mutex;
		bool _stopped = false;

	public:
		async_task_pool()
		{
			constexpr size_t worker_count = 4;
			_workers.reserve(worker_count);
			_state->workers = worker_count;
			try
			{
				for (size_t i = 0; i < worker_count; ++i)
					_workers.emplace_back([state = _state] { consume_tasks(state); });
			}
			catch (...)
			{
				{
					std::lock_guard lock(_state->mutex);
					_state->workers -= worker_count - _workers.size();
				}
				stop_queue(_state);
				for (auto& worker : _workers) worker.join();
				throw;
			}
		}

		bool post(std::function<void()> task)
		{
			return enqueue_task(_state, std::move(task));
		}

		void stop(const bool bounded)
		{
			std::lock_guard stop_lock(_stop_mutex);
			if (_stopped) return;
			_stopped = true;
			std::deque<std::function<void()>> cancelled;
			{
				std::lock_guard lock(_state->mutex);
				_state->accepting = false;
				if (bounded) cancelled.swap(_state->tasks);
			}
			_state->ready.notify_all();

			bool finished = true;
			if (bounded)
			{
				std::unique_lock lock(_state->mutex);
				finished = _state->finished.wait_for(lock, std::chrono::seconds(5),
					[&] { return _state->workers == 0; });
			}
			for (auto& worker : _workers)
			{
				if (finished && worker.get_id() != std::this_thread::get_id())
					worker.join();
				else
					worker.detach();
			}
			if (!finished)
				pf::debug_trace("Async shutdown: workers still running after 5 seconds\n");
		}
	};

	struct task_dispatcher
	{
		std::mutex pool_mutex;
		std::unique_ptr<async_task_pool> pool;
		bool stopping = false;
		std::mutex ui_mutex;
		std::vector<std::function<void()>> ui_tasks;
		HANDLE ui_event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
		bool ui_accepting = true;

		task_dispatcher()
		{
			if (!ui_event)
				throw std::runtime_error(std::format("CreateEvent(UI tasks) failed ({})", GetLastError()));
		}

		void shutdown(const bool bounded)
		{
			async_task_pool* active_pool = nullptr;
			{
				std::lock_guard lock(pool_mutex);
				stopping = true;
				active_pool = pool.get();
			}
			if (active_pool) active_pool->stop(bounded);
			std::vector<std::function<void()>> discarded;
			{
				std::lock_guard lock(ui_mutex);
				ui_accepting = false;
				discarded.swap(ui_tasks);
				if (ui_event) CloseHandle(ui_event);
				ui_event = nullptr;
			}
		}
	};

	task_dispatcher& dispatcher()
	{
		// Detached network jobs may still post during CRT teardown; keep the stopped dispatcher alive.
		static auto* value = new task_dispatcher;
		struct cleanup
		{
			~cleanup() { dispatcher().shutdown(false); }
		};
		static cleanup at_exit;
		return *value;
	}
}

pf::serial_executor_ptr pf::create_serial_executor()
{
	try { return std::make_unique<win_serial_executor>(); }
	catch (const std::exception& e)
	{
		debug_trace("create_serial_executor: " + std::string(e.what()) + "\n");
		return nullptr;
	}
}

void pf::run_async(std::function<void()> task)
{
	if (!task) return;
	auto& state = dispatcher();
	std::lock_guard lock(state.pool_mutex);
	if (state.stopping) return;
	if (!state.pool)
	{
		try { state.pool = std::make_unique<async_task_pool>(); }
		catch (const std::exception& e)
		{
			debug_trace("run_async: " + std::string(e.what()) + "\n");
			throw;
		}
	}
	state.pool->post(std::move(task));
}

void pf::run_ui(std::function<void()> task)
{
	if (!task) return;
	auto& state = dispatcher();
	std::lock_guard lock(state.ui_mutex);
	if (!state.ui_accepting) return;
	state.ui_tasks.push_back(std::move(task));
	SetEvent(state.ui_event);
}

static void run_ui_tasks()
{
	auto& state = dispatcher();
	std::vector<std::function<void()>> tasks;
	{
		std::lock_guard lock(state.ui_mutex);
		tasks.swap(state.ui_tasks);
	}
	for (auto& task : tasks) invoke_task(task);
}

void pf::pump_ui_tasks(const int timeout_ms)
{
	if (const auto event = dispatcher().ui_event)
		WaitForSingleObject(event, static_cast<DWORD>(std::max(0, timeout_ms)));
	run_ui_tasks();
}

static void init_handles()
{
	exit_h = CreateEvent(nullptr, TRUE, FALSE, nullptr);

	if (!exit_h || !dispatcher().ui_event)
	{
		pf::debug_trace("init_handles: CreateEvent failed\n");
		std::abort();
	}

	if (!QueryPerformanceFrequency(&g_perfFreq) || g_perfFreq.QuadPart == 0)
	{
		g_perfFreq.QuadPart = 1; // avoid division by zero in platform_get_time
	}
	QueryPerformanceCounter(&g_perfStart);
}

int pf::platform_run()
{
	MSG msg = {};
	int result = 0;

	for (;;)
	{
		const HANDLE h[] = {dispatcher().ui_event, exit_h};
		constexpr auto n = std::size(h);

		const auto wait = MsgWaitForMultipleObjects(n, h, FALSE, INFINITE, QS_ALLINPUT);

		if (wait == WAIT_OBJECT_0)
		{
			run_ui_tasks();
		}
		else if (wait == WAIT_OBJECT_0 + 1)
		{
			break;
		}
		else if (wait == WAIT_OBJECT_0 + 2)
		{
			// Windows message pending
		}
		else
		{
			result = 1;
			break;
		}

		while (PeekMessage(&msg, nullptr, 0, 0, PM_REMOVE))
		{
			if (msg.message == WM_QUIT)
			{
				result = static_cast<int>(msg.wParam);
				SetEvent(exit_h);
				goto cleanup;
			}

			if (!TranslateAccelerator(g_hWnd, g_hAccel, &msg))
			{
				TranslateMessage(&msg);
				::DispatchMessage(&msg);
			}
		}

		app_idle();
	}

cleanup:
	dispatcher().shutdown(true);
	CloseHandle(exit_h);
	exit_h = nullptr;

	return result;
}

// ── Path containment ───────────────────────────────────────────────────────────

namespace
{
	std::wstring full_path_of(const std::wstring& wide)
	{
		std::wstring full(MAX_PATH, L'\0');
		auto len = GetFullPathNameW(wide.c_str(), static_cast<DWORD>(full.size()), full.data(), nullptr);

		if (len >= full.size())
		{
			full.resize(len + 1);
			len = GetFullPathNameW(wide.c_str(), static_cast<DWORD>(full.size()), full.data(), nullptr);
		}

		if (len == 0 || len >= full.size())
			return wide;

		full.resize(len);
		return full;
	}

	// Resolves links, which GetFullPathName does not
	bool final_path_of(const std::wstring& wide, std::wstring& result)
	{
		const auto handle = CreateFileW(wide.c_str(), 0,
		                                FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
		                                nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);

		if (handle == INVALID_HANDLE_VALUE)
			return false;

		std::wstring buffer(MAX_PATH, L'\0');
		auto len = GetFinalPathNameByHandleW(handle, buffer.data(), static_cast<DWORD>(buffer.size()),
		                                     FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);

		if (len >= buffer.size())
		{
			buffer.resize(len + 1);
			len = GetFinalPathNameByHandleW(handle, buffer.data(), static_cast<DWORD>(buffer.size()),
			                                FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
		}

		CloseHandle(handle);

		if (len == 0 || len >= buffer.size())
			return false;

		buffer.resize(len);

		if (std::wstring_view(buffer).starts_with(L"\\\\?\\"))
			buffer.erase(0, 4);

		result = buffer;
		return true;
	}
}

pf::file_path pf::canonical_path(const file_path& path)
{
	if (path.empty())
		return {};

	// GetFullPathName resolves '..' and makes the path absolute, but does not follow links
	const auto full = utf16_to_utf8(full_path_of(utf8_to_utf16(path.view())));

	// A path that does not exist yet still has to compare against one that does, so the
	// nearest existing ancestor is resolved and the remaining names put back on.
	std::vector<std::string> tail;
	file_path current{full};

	for (auto depth = 0; depth < 64; ++depth)
	{
		if (std::wstring resolved; final_path_of(utf8_to_utf16(current.view()), resolved))
		{
			auto result = file_path(utf16_to_utf8(resolved));

			for (auto name = tail.rbegin(); name != tail.rend(); ++name)
				result = result.combine(*name);

			return result;
		}

		const auto parent = current.folder();
		const auto name = current.name();

		if (name.empty() || parent.empty() || parent == current)
			break;

		tail.push_back(name);
		current = parent;
	}

	return file_path{full};
}

bool pf::is_path_within(const file_path& root, const file_path& path)
{
	if (root.empty() || path.empty())
		return false;

	const auto canonical_root = canonical_path(root);
	const auto canonical_target = canonical_path(path);

	auto r = canonical_root.view();
	const auto p = canonical_target.view();

	while (!r.empty() && (r.back() == '\\' || r.back() == '/'))
		r.remove_suffix(1);

	if (r.empty() || p.size() < r.size())
		return false;

	if (icmp(p.substr(0, r.size()), r) != 0)
		return false;

	if (p.size() == r.size())
		return true;

	const auto separator = p[r.size()];
	return separator == '\\' || separator == '/';
}

// ── Child processes ────────────────────────────────────────────────────────────

std::string pf::quote_command_arg(const std::string_view arg)
{
	if (!arg.empty() && arg.find_first_of(" \t\n\v\"") == std::string_view::npos)
		return std::string(arg);

	std::string result;
	result.reserve(arg.size() + 2);
	result += '"';

	for (auto i = arg.begin();; ++i)
	{
		size_t backslashes = 0;

		while (i != arg.end() && *i == '\\')
		{
			++i;
			++backslashes;
		}

		if (i == arg.end())
		{
			// Backslashes before the closing quote must not escape it
			result.append(backslashes * 2, '\\');
			break;
		}

		if (*i == '"')
		{
			result.append(backslashes * 2 + 1, '\\');
		}
		else
		{
			result.append(backslashes, '\\');
		}

		result += *i;
	}

	result += '"';
	return result;
}

bool pf::has_shell_metacharacter(const std::string_view text)
{
	return text.find_first_of("&|<>^%!\"\r\n") != std::string_view::npos;
}

namespace
{
	struct process_handle
	{
		HANDLE value = nullptr;
		process_handle() = default;
		process_handle(const process_handle&) = delete;
		process_handle& operator=(const process_handle&) = delete;
		~process_handle() { reset(); }
		void reset(HANDLE next = nullptr)
		{
			if (value && value != INVALID_HANDLE_VALUE) CloseHandle(value);
			value = next;
		}
	};

	void process_error(const char* operation, DWORD error = GetLastError())
	{
		pf::debug_trace(std::format("process: {} failed ({})\n", operation, error));
	}

	struct process_startup
	{
		STARTUPINFOEXW info{};
		std::vector<unsigned char> storage;
		bool initialized = false;
		~process_startup()
		{
			if (initialized) DeleteProcThreadAttributeList(info.lpAttributeList);
		}
		bool set_handles(HANDLE (&handles)[3])
		{
			SIZE_T bytes = 0;
			InitializeProcThreadAttributeList(nullptr, 1, 0, &bytes);
			storage.resize(bytes);
			info.StartupInfo.cb = sizeof(info);
			info.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
			info.StartupInfo.hStdInput = handles[0];
			info.StartupInfo.hStdOutput = handles[1];
			info.StartupInfo.hStdError = handles[2];
			info.lpAttributeList = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(storage.data());
			initialized = InitializeProcThreadAttributeList(info.lpAttributeList, 1, 0, &bytes) != FALSE;
			return initialized && UpdateProcThreadAttribute(info.lpAttributeList, 0,
				PROC_THREAD_ATTRIBUTE_HANDLE_LIST, handles, sizeof(handles), nullptr, nullptr);
		}
	};

	struct process_pipe
	{
		process_handle handle;
		std::mutex mutex;
		bool eof = false;
	};

	bool process_make_pipe(process_pipe& parent, process_handle& child, bool input)
	{
		GUID id{};
		const auto result = CoCreateGuid(&id);
		if (FAILED(result))
		{
			SetLastError(static_cast<DWORD>(result));
			return false;
		}
		wchar_t guid[40]{};
		StringFromGUID2(id, guid, static_cast<int>(std::size(guid)));
		const auto name = std::wstring(L"\\\\.\\pipe\\pf-process-") + guid;
		parent.handle.reset(CreateNamedPipeW(name.c_str(),
			(input ? PIPE_ACCESS_OUTBOUND : PIPE_ACCESS_INBOUND) |
			FILE_FLAG_OVERLAPPED | FILE_FLAG_FIRST_PIPE_INSTANCE,
			PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS,
			1, 65536, 65536, 0, nullptr));
		if (parent.handle.value == INVALID_HANDLE_VALUE) return false;
		SECURITY_ATTRIBUTES security{sizeof(security), nullptr, TRUE};
		child.reset(CreateFileW(name.c_str(), input ? GENERIC_READ : GENERIC_WRITE,
			0, &security, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
		return child.value != INVALID_HANDLE_VALUE;
	}

	std::wstring process_env_value(const wchar_t* key)
	{
		std::wstring value(GetEnvironmentVariableW(key, nullptr, 0), L'\0');
		if (value.empty()) return {};
		const DWORD size = GetEnvironmentVariableW(key, value.data(), static_cast<DWORD>(value.size()));
		if (size >= value.size()) return {};
		value.resize(size);
		return value;
	}

	struct process_env_less
	{
		bool operator()(const std::wstring& a, const std::wstring& b) const
		{
			return CompareStringOrdinal(a.data(), static_cast<int>(a.size()),
				b.data(), static_cast<int>(b.size()), TRUE) == CSTR_LESS_THAN;
		}
	};

	bool process_environment(const std::vector<std::string>& overrides, std::vector<wchar_t>& block)
	{
		std::map<std::wstring, std::wstring, process_env_less> entries;
		auto* environment = GetEnvironmentStringsW();
		if (!environment) return false;
		for (auto* p = environment; *p; p += wcslen(p) + 1)
		{
			const std::wstring entry(p);
			const auto eq = entry.find(L'=', 1); // Preserve the hidden '=C:' drive entries.
			if (eq != std::wstring::npos) entries[entry.substr(0, eq)] = entry.substr(eq + 1);
		}
		FreeEnvironmentStringsW(environment);
		for (const auto& entry : overrides)
		{
			const auto eq = entry.find('=');
			if (!eq || eq == std::string::npos || entry.find('\0') != std::string::npos)
			{
				SetLastError(ERROR_INVALID_PARAMETER);
				return false;
			}
			entries[pf::utf8_to_utf16(entry.substr(0, eq))] = pf::utf8_to_utf16(entry.substr(eq + 1));
		}
		for (const auto& [key, value] : entries)
		{
			block.insert(block.end(), key.begin(), key.end());
			block.push_back(L'=');
			block.insert(block.end(), value.begin(), value.end());
			block.push_back(L'\0');
		}
		if (block.empty()) block.push_back(L'\0');
		block.push_back(L'\0');
		return true;
	}

	// cmd expands percent expressions even inside quotes; refuse ambiguous script input.
	bool process_batch_arg(const std::string& arg)
	{
		return arg.find_first_of("%\"\r\n") == std::string::npos;
	}

	std::string process_batch_quote(const std::string& arg)
	{
		// Shims forward %* to an executable: preserve CRT trailing-backslash escaping.
		auto quoted = pf::quote_command_arg(" " + arg);
		quoted.erase(1, 1);
		return quoted;
	}
}

struct pf::process
{
	process_handle child, job, cancelled, input_closed;
	process_pipe input, output, error;
	std::mutex lifecycle;

	~process()
	{
		// A job remains useful after its leader exits: grandchildren may still own pipes.
		if (job.value)
		{
			if (!TerminateJobObject(job.value, 1)) process_error("TerminateJobObject");
		}
		else if (child.value && WaitForSingleObject(child.value, 0) == WAIT_TIMEOUT)
		{
			if (!TerminateProcess(child.value, 1)) process_error("TerminateProcess");
		}
		if (child.value) WaitForSingleObject(child.value, INFINITE);
	}
};

pf::process_ptr pf::process_spawn(const process_options& options)
{
	if (options.exe.empty() || options.exe.find('\0') != std::string::npos ||
		options.cwd.find('\0') != std::string::npos ||
		std::any_of(options.args.begin(), options.args.end(),
			[](const auto& arg) { return arg.find('\0') != std::string::npos; }))
	{
		process_error("arguments", ERROR_INVALID_PARAMETER);
		return {};
	}
	auto exe = options.exe;
	if (exe.find_first_of("\\/") == std::string::npos)
		exe = std::string(find_executable(exe).view());
	if (exe.empty())
	{
		process_error("executable", ERROR_FILE_NOT_FOUND);
		return {};
	}
	const auto extension = to_lower(file_path(exe).extension());
	const bool batch = extension == ".bat" || extension == ".cmd";
	std::string command;
	std::wstring application;
	if (batch)
	{
		if (!process_batch_arg(exe) ||
			std::any_of(options.args.begin(), options.args.end(),
				[](const auto& arg) { return !process_batch_arg(arg); }))
		{
			process_error("batch arguments", ERROR_INVALID_PARAMETER);
			return {};
		}
		application = process_env_value(L"ComSpec");
		if (application.empty())
		{
			wchar_t system[MAX_PATH]{};
			const auto length = GetSystemDirectoryW(system, MAX_PATH);
			if (!length || length >= MAX_PATH)
			{
				process_error("GetSystemDirectoryW");
				return {};
			}
			application = std::wstring(system, length) + L"\\cmd.exe";
		}
		command = quote_command_arg(utf16_to_utf8(application)) +
			" /d /s /v:off /c \"" + process_batch_quote(exe);
		for (const auto& arg : options.args) command += " " + process_batch_quote(arg);
		command += '"';
	}
	else
	{
		application = utf8_to_utf16(exe);
		command = quote_command_arg(exe);
		for (const auto& arg : options.args) command += " " + quote_command_arg(arg);
	}
	auto wide_command = utf8_to_utf16(command);
	if (wide_command.size() >= (batch ? 8191u : 32767u))
	{
		process_error("command length", ERROR_BAD_LENGTH);
		return {};
	}
	std::vector<wchar_t> environment;
	if (!process_environment(options.env, environment))
	{
		process_error("environment");
		return {};
	}
	auto result = std::make_shared<process>();
	result->cancelled.reset(CreateEventW(nullptr, TRUE, FALSE, nullptr));
	result->input_closed.reset(CreateEventW(nullptr, TRUE, FALSE, nullptr));
	if (!result->cancelled.value || !result->input_closed.value)
	{
		process_error("CreateEventW");
		return {};
	}
	process_handle child_input, child_output, child_error;
	if (!process_make_pipe(result->input, child_input, true) ||
		!process_make_pipe(result->output, child_output, false) ||
		!process_make_pipe(result->error, child_error, false))
	{
		process_error("pipe");
		return {};
	}
	if (options.kill_descendants_on_close)
	{
		result->job.reset(CreateJobObjectW(nullptr, nullptr));
		JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
		limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
		if (!result->job.value || !SetInformationJobObject(result->job.value,
			JobObjectExtendedLimitInformation, &limits, sizeof(limits)))
		{
			process_error("job");
			return {};
		}
	}
	HANDLE handles[]{child_input.value, child_output.value, child_error.value};
	process_startup startup;
	if (!startup.set_handles(handles))
	{
		process_error("handle inheritance");
		return {};
	}
	PROCESS_INFORMATION created{};
	const auto cwd = utf8_to_utf16(options.cwd);
	if (!CreateProcessW(application.c_str(), wide_command.data(), nullptr, nullptr, TRUE,
		CREATE_NO_WINDOW | CREATE_SUSPENDED | CREATE_UNICODE_ENVIRONMENT | EXTENDED_STARTUPINFO_PRESENT,
		environment.data(), cwd.empty() ? nullptr : cwd.c_str(), &startup.info.StartupInfo, &created))
	{
		process_error("CreateProcessW");
		return {};
	}
	result->child.reset(created.hProcess);
	process_handle main_thread;
	main_thread.reset(created.hThread);
	if (result->job.value && !AssignProcessToJobObject(result->job.value, result->child.value))
	{
		process_error("AssignProcessToJobObject");
		// A suspended child not yet in our job must be killed explicitly on failure.
		if (!TerminateProcess(result->child.value, 1)) process_error("TerminateProcess");
		return {};
	}
	if (ResumeThread(main_thread.value) == static_cast<DWORD>(-1))
	{
		process_error("ResumeThread");
		return {};
	}
	return result;
}

namespace
{
	size_t process_transfer(pf::process& owner, process_pipe& pipe, char* data, size_t bytes, bool write)
	{
		if (pipe.eof || !pipe.handle.value ||
			WaitForSingleObject(owner.cancelled.value, 0) == WAIT_OBJECT_0 ||
			(write && WaitForSingleObject(owner.input_closed.value, 0) == WAIT_OBJECT_0))
			return 0;
		process_handle event;
		event.reset(CreateEventW(nullptr, TRUE, FALSE, nullptr));
		if (!event.value) { process_error("CreateEventW"); return 0; }
		OVERLAPPED operation{};
		operation.hEvent = event.value;
		DWORD transferred = 0;
		const DWORD count = static_cast<DWORD>(std::min<size_t>(bytes, 65536));
		const bool success = write ?
			WriteFile(pipe.handle.value, data, count, &transferred, &operation) != FALSE :
			ReadFile(pipe.handle.value, data, count, &transferred, &operation) != FALSE;
		DWORD error = success ? ERROR_SUCCESS : GetLastError();
		if (error == ERROR_IO_PENDING)
		{
			HANDLE events[]{owner.cancelled.value, write ? owner.input_closed.value : event.value, event.value};
			const DWORD wait = WaitForMultipleObjects(write ? 3 : 2, events, FALSE, INFINITE);
			if (wait != WAIT_OBJECT_0 + (write ? 2 : 1))
			{
				if (!CancelIoEx(pipe.handle.value, &operation) && GetLastError() != ERROR_NOT_FOUND)
					process_error("CancelIoEx");
			}
			// Even a cancelled operation owns its OVERLAPPED and buffer until completion.
			error = GetOverlappedResult(pipe.handle.value, &operation, &transferred, TRUE) ?
				ERROR_SUCCESS : GetLastError();
		}
		if (error != ERROR_SUCCESS)
		{
			if (error != ERROR_BROKEN_PIPE && error != ERROR_PIPE_NOT_CONNECTED &&
				error != ERROR_OPERATION_ABORTED) process_error(write ? "WriteFile" : "ReadFile", error);
			pipe.eof = true;
			return 0;
		}
		if (!transferred) pipe.eof = true;
		return transferred;
	}
}

size_t pf::process_read(const process_ptr& process, char* buffer, size_t bytes)
{
	const auto owner = process;
	if (!owner || !buffer || !bytes) return 0;
	std::lock_guard lock(owner->output.mutex);
	return process_transfer(*owner, owner->output, buffer, bytes, false);
}

size_t pf::process_read_err(const process_ptr& process, char* buffer, size_t bytes)
{
	const auto owner = process;
	if (!owner || !buffer || !bytes) return 0;
	std::lock_guard lock(owner->error.mutex);
	return process_transfer(*owner, owner->error, buffer, bytes, false);
}

bool pf::process_write(const process_ptr& process, std::string_view text)
{
	const auto owner = process;
	if (!owner) return false;
	std::lock_guard lock(owner->input.mutex);
	if (!owner->input.handle.value || owner->input.eof ||
		WaitForSingleObject(owner->input_closed.value, 0) == WAIT_OBJECT_0 ||
		WaitForSingleObject(owner->cancelled.value, 0) == WAIT_OBJECT_0) return false;
	while (!text.empty())
	{
		const auto written = process_transfer(*owner, owner->input,
			const_cast<char*>(text.data()), text.size(), true);
		if (!written) return false;
		text.remove_prefix(written);
	}
	return true;
}

void pf::process_close_input(const process_ptr& process)
{
	const auto owner = process;
	if (!owner) return;
	if (!SetEvent(owner->input_closed.value)) process_error("SetEvent");
	std::lock_guard lock(owner->input.mutex);
	owner->input.handle.reset();
}

bool pf::process_alive(const process_ptr& process)
{
	const auto owner = process;
	if (!owner) return false;
	const auto result = WaitForSingleObject(owner->child.value, 0);
	if (result == WAIT_FAILED) process_error("WaitForSingleObject");
	return result == WAIT_TIMEOUT;
}

void pf::process_terminate(const process_ptr& process)
{
	const auto owner = process;
	if (!owner) return;
	if (!SetEvent(owner->cancelled.value)) process_error("SetEvent");
	process_close_input(owner);
	std::lock_guard lock(owner->lifecycle);
	if (owner->job.value)
	{
		if (!TerminateJobObject(owner->job.value, 1)) process_error("TerminateJobObject");
	}
	else if (process_alive(owner) && !TerminateProcess(owner->child.value, 1))
		process_error("TerminateProcess");
	if (WaitForSingleObject(owner->child.value, INFINITE) == WAIT_FAILED)
		process_error("WaitForSingleObject");
}

pf::file_path pf::find_executable(const std::string_view name)
{
	if (name.empty())
		return {};

	const auto read_env = [](const wchar_t* key) -> std::wstring
	{
		std::wstring value(4096, L'\0');
		auto len = GetEnvironmentVariableW(key, value.data(), static_cast<DWORD>(value.size()));

		if (len >= value.size())
		{
			value.resize(len);
			len = GetEnvironmentVariableW(key, value.data(), static_cast<DWORD>(value.size()));
		}

		value.resize(len < value.size() ? len : 0);
		return value;
	};

	// PATH is passed explicitly so the current directory is never searched — otherwise
	// opening an untrusted folder could put an executable of this name ahead of the real one.
	const auto search_path = read_env(L"PATH");

	if (search_path.empty())
		return {};

	const auto try_path = [&](const std::wstring& candidate) -> file_path
	{
		wchar_t buffer[MAX_PATH * 2] = {};
		const auto len = SearchPathW(search_path.c_str(), candidate.c_str(), nullptr,
		                             static_cast<DWORD>(std::size(buffer)), buffer, nullptr);
		if (len == 0 || len >= std::size(buffer))
			return {};
		return file_path(utf16_to_utf8(std::wstring_view(buffer, len)));
	};

	const auto wide_name = utf8_to_utf16(name);

	// A bare name is only resolved through PATHEXT. Windows cannot execute an extensionless
	// file, and shim folders often contain one next to the real .exe.
	const auto last_separator = name.find_last_of("\\/");
	const auto tail = last_separator == std::string_view::npos ? name : name.substr(last_separator + 1);

	if (tail.find('.') != std::string_view::npos)
		return try_path(wide_name);

	auto path_ext = read_env(L"PATHEXT");

	if (path_ext.empty())
		path_ext = L".COM;.EXE;.BAT;.CMD";

	size_t pos = 0;

	while (pos < path_ext.size())
	{
		auto end = path_ext.find(L';', pos);
		if (end == std::wstring::npos)
			end = path_ext.size();

		const auto ext = path_ext.substr(pos, end - pos);
		pos = end + 1;

		if (ext.empty())
			continue;

		if (auto found = try_path(wide_name + ext); !found.empty())
			return found;
	}

	return {};
}

namespace
{
	// Shared with the reader threads so a callback queued for the UI thread can be
	// dropped when the process object has already gone away.
	struct child_shared_state
	{
		std::atomic<bool> cancelled{false};
		pf::child_process_callbacks callbacks;
	};

	using child_shared_state_ptr = std::shared_ptr<child_shared_state>;

	struct child_reader_args
	{
		HANDLE pipe = nullptr;
		HANDLE process = nullptr; // only the stdout reader reports the exit code
		bool is_stderr = false;
		child_shared_state_ptr state;
	};

	void post_child_line(const child_shared_state_ptr& state, const bool is_stderr, std::string line)
	{
		pf::run_ui([state, is_stderr, line = std::move(line)]
		{
			if (state->cancelled)
				return;

			const auto& callback = is_stderr ? state->callbacks.on_stderr_line : state->callbacks.on_stdout_line;

			if (callback)
				callback(line);
		});
	}

	DWORD WINAPI child_reader_proc(LPVOID param)
	{
		const std::unique_ptr<child_reader_args> args(static_cast<child_reader_args*>(param));

		pf::line_splitter splitter;
		char buffer[4096];

		const auto emit = [&](const std::string_view line)
		{
			post_child_line(args->state, args->is_stderr, std::string(line));
		};

		for (;;)
		{
			DWORD read = 0;

			if (!ReadFile(args->pipe, buffer, sizeof(buffer), &read, nullptr) || read == 0)
				break;

			splitter.feed(std::string_view(buffer, read), emit);
		}

		splitter.flush(emit);

		if (args->process)
		{
			WaitForSingleObject(args->process, INFINITE);

			DWORD exit_code = 0;
			GetExitCodeProcess(args->process, &exit_code);
			CloseHandle(args->process);

			auto state = args->state;
			pf::run_ui([state, exit_code]
			{
				if (state->cancelled)
					return;

				if (state->callbacks.on_exit)
					state->callbacks.on_exit(static_cast<int>(exit_code));
			});
		}

		return 0;
	}

	class win_child_process final : public pf::child_process
	{
	public:
		HANDLE _process = nullptr;
		HANDLE _stdin_write = nullptr;
		HANDLE _stdout_read = nullptr;
		HANDLE _stderr_read = nullptr;
		HANDLE _stdout_thread = nullptr;
		HANDLE _stderr_thread = nullptr;
		child_shared_state_ptr _state;

		~win_child_process() override
		{
			if (_state)
				_state->cancelled = true;

			terminate();
			close_input();

			// Killing the process breaks both pipes, so the readers return and can be joined
			join_thread(_stdout_thread);
			join_thread(_stderr_thread);

			close_handle(_stdout_read);
			close_handle(_stderr_read);
			close_handle(_process);
		}

		bool write_line(const std::string_view text) override
		{
			if (!_stdin_write)
				return false;

			std::string payload(text);
			payload += '\n';

			size_t written_total = 0;

			while (written_total < payload.size())
			{
				DWORD written = 0;

				if (!WriteFile(_stdin_write, payload.data() + written_total,
				               static_cast<DWORD>(payload.size() - written_total), &written, nullptr) ||
					written == 0)
					return false;

				written_total += written;
			}

			return true;
		}

		void close_input() override
		{
			close_handle(_stdin_write);
		}

		void terminate() override
		{
			if (_process && is_running())
				TerminateProcess(_process, 1);
		}

		[[nodiscard]] bool is_running() const override
		{
			return _process && WaitForSingleObject(_process, 0) == WAIT_TIMEOUT;
		}

	private:
		static void close_handle(HANDLE& h)
		{
			if (h)
			{
				CloseHandle(h);
				h = nullptr;
			}
		}

		static void join_thread(HANDLE& h)
		{
			if (h)
			{
				WaitForSingleObject(h, 5000);
				CloseHandle(h);
				h = nullptr;
			}
		}
	};

	bool create_child_pipe(HANDLE& read_end, HANDLE& write_end, const bool inherit_read)
	{
		SECURITY_ATTRIBUTES sa = {sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};

		if (!CreatePipe(&read_end, &write_end, &sa, 0))
			return false;

		// Only the child's end is inheritable, so it cannot outlive the child
		const auto ours = inherit_read ? write_end : read_end;
		return SetHandleInformation(ours, HANDLE_FLAG_INHERIT, 0) != 0;
	}
}

pf::child_process_ptr pf::spawn_child_process(const file_path& exe,
                                              const std::span<const std::string> args,
                                              const file_path& working_dir,
                                              child_process_callbacks callbacks)
{
	if (exe.empty())
		return nullptr;

	const auto extension = to_lower(exe.extension());
	const auto is_batch = extension == "bat" || extension == "cmd";

	std::string command_line;

	if (is_batch)
	{
		// A batch file has to run under cmd.exe, where quoting alone does not contain an
		// argument, so anything cmd would interpret is refused rather than escaped.
		for (const auto& arg : args)
		{
			if (has_shell_metacharacter(arg))
				return nullptr;
		}

		if (has_shell_metacharacter(exe.view()))
			return nullptr;

		command_line = "cmd.exe /c " + quote_command_arg(exe.view());
	}
	else
	{
		command_line = quote_command_arg(exe.view());
	}

	for (const auto& arg : args)
	{
		command_line += ' ';
		command_line += quote_command_arg(arg);
	}

	HANDLE stdin_read = nullptr;
	HANDLE stdin_write = nullptr;
	HANDLE stdout_read = nullptr;
	HANDLE stdout_write = nullptr;
	HANDLE stderr_read = nullptr;
	HANDLE stderr_write = nullptr;

	const auto close_all = [&]
	{
		for (auto* h : {&stdin_read, &stdin_write, &stdout_read, &stdout_write, &stderr_read, &stderr_write})
		{
			if (*h) CloseHandle(*h);
			*h = nullptr;
		}
	};

	if (!create_child_pipe(stdin_read, stdin_write, true) ||
		!create_child_pipe(stdout_read, stdout_write, false) ||
		!create_child_pipe(stderr_read, stderr_write, false))
	{
		close_all();
		return nullptr;
	}

	STARTUPINFOW si = {sizeof(STARTUPINFOW)};
	si.dwFlags = STARTF_USESTDHANDLES;
	si.hStdInput = stdin_read;
	si.hStdOutput = stdout_write;
	si.hStdError = stderr_write;

	PROCESS_INFORMATION pi = {};

	auto wide_command_line = utf8_to_utf16(command_line);
	const auto wide_working_dir = utf8_to_utf16(working_dir.view());

	const auto created = CreateProcessW(nullptr, wide_command_line.data(), nullptr, nullptr, TRUE,
	                                    CREATE_NO_WINDOW, nullptr,
	                                    wide_working_dir.empty() ? nullptr : wide_working_dir.c_str(),
	                                    &si, &pi);

	// The child owns its ends now; holding them open would hide the pipe closing
	CloseHandle(stdin_read);
	stdin_read = nullptr;
	CloseHandle(stdout_write);
	stdout_write = nullptr;
	CloseHandle(stderr_write);
	stderr_write = nullptr;

	if (!created)
	{
		close_all();
		return nullptr;
	}

	CloseHandle(pi.hThread);

	auto state = std::make_shared<child_shared_state>();
	state->callbacks = std::move(callbacks);

	auto process = std::make_unique<win_child_process>();
	process->_process = pi.hProcess;
	process->_stdin_write = stdin_write;
	process->_stdout_read = stdout_read;
	process->_stderr_read = stderr_read;
	process->_state = state;

	HANDLE exit_watch = nullptr;
	DuplicateHandle(GetCurrentProcess(), pi.hProcess, GetCurrentProcess(), &exit_watch,
	                0, FALSE, DUPLICATE_SAME_ACCESS);

	auto* stdout_args = new child_reader_args{stdout_read, exit_watch, false, state};
	process->_stdout_thread = CreateThread(nullptr, 0, child_reader_proc, stdout_args, 0, nullptr);

	if (!process->_stdout_thread)
	{
		delete stdout_args;
		if (exit_watch) CloseHandle(exit_watch);
	}

	auto* stderr_args = new child_reader_args{stderr_read, nullptr, true, state};
	process->_stderr_thread = CreateThread(nullptr, 0, child_reader_proc, stderr_args, 0, nullptr);

	if (!process->_stderr_thread)
		delete stderr_args;

	return process;
}


//  File I/O â”€


bool pf::platform_move_file_replace(const char* source, const char* dest)
{
	return os::move_file(source, dest, false).success();
}

std::string pf::platform_temp_file_path(const char* prefix)
{
	wchar_t dir[MAX_PATH + 1] = {0};	GetTempPathW(MAX_PATH, dir);
	wchar_t result[MAX_PATH + 1] = {0};
	GetTempFileNameW(dir, utf8_to_utf16(prefix).c_str(), 0, result);
	return utf16_to_utf8(result);
}

std::string pf::platform_last_error_message()
{
	std::string result;
	const auto error = GetLastError();
	if (error)
	{
		LPVOID lpMsgBuf = nullptr;
		const auto bufLen = FormatMessageW(
			FORMAT_MESSAGE_ALLOCATE_BUFFER |
			FORMAT_MESSAGE_FROM_SYSTEM |
			FORMAT_MESSAGE_IGNORE_INSERTS,
			nullptr, error,
			MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
			reinterpret_cast<LPWSTR>(&lpMsgBuf),
			0, nullptr);
		if (bufLen)
		{
			result = utf16_to_utf8(static_cast<const wchar_t*>(lpMsgBuf));
			LocalFree(lpMsgBuf);
		}
	}
	return result;
}

bool pf::platform_recycle_file(const file_path& path)
{
	return os::recycle_file(path.view()).success();
}

bool pf::platform_rename_file(const file_path& old_path, const file_path& new_path)
{
	return os::move_file(old_path.view(), new_path.view(), true).success();
}

bool pf::platform_delete_file(const file_path& path)
{
	return os::delete_file(path.view()).success();
}

bool pf::platform_create_directory(const file_path& path)
{
	return os::create_folder(path.view()).success();
}

bool pf::platform_copy_file(const file_path& source, const file_path& dest, const bool fail_if_exists)
{
	return os::copy_file(source.view(), dest.view(), fail_if_exists).success();
}

static std::vector<pf::file_path> decode_dropped_files(const HDROP hDrop)
{
	std::vector<pf::file_path> paths;
	const auto count = DragQueryFileW(hDrop, 0xFFFFFFFF, nullptr, 0);

	for (UINT i = 0; i < count; ++i)
	{
		const auto len = DragQueryFileW(hDrop, i, nullptr, 0);
		if (len == 0) continue;

		std::wstring buf(len + 1, L'\0');
		const auto copied = DragQueryFileW(hDrop, i, buf.data(), len + 1);
		if (copied == 0) continue;

		buf.resize(copied);
		paths.emplace_back(pf::utf16_to_utf8(buf));
	}

	DragFinish(hDrop);
	return paths;
}

bool pf::platform_clipboard_has_text()
{
	return os::clipboard_has_text();
}

std::string pf::platform_text_from_clipboard()
{
	return clipboard_get_text(nullptr);
}

bool pf::platform_text_to_clipboard(const std::string_view text)
{
	return clipboard_set_text(nullptr, text);
}

//  File system utilities 

//  Configuration (INI file) â”€

static std::string s_config_app_name = "app";
static std::optional<pf::file_path> s_config_path;
static std::optional<std::string> s_config_registry_root;
static os::ini_store_ptr s_config_store;

void pf::config_set_app_name(const std::string_view app_name)
{
	s_config_app_name = app_name;
	s_config_store.reset();
}

void pf::config_set_path(const file_path& path)
{
	if (path.empty())
		s_config_path.reset();
	else
		s_config_path = path;
	s_config_registry_root.reset();
	s_config_store.reset();
}

void pf::config_set_registry_root(const std::string_view root_key)
{
	if (root_key.empty())
		s_config_registry_root.reset();
	else
		s_config_registry_root = std::string(root_key);
	s_config_path.reset();
	s_config_store.reset();
}

static pf::file_path get_config_path()
{
	if (s_config_path) return *s_config_path;

	// Try next to the exe first
	wchar_t w_exe_path[MAX_PATH];
	GetModuleFileNameW(nullptr, w_exe_path, MAX_PATH);
	const auto exe_path = pf::utf16_to_utf8(w_exe_path);
	auto ini_path = pf::file_path(exe_path).folder().combine(s_config_app_name, "ini");

	// Probe writability without creating a sentinel file: try to open
	// the existing ini for write, or create a temp file in the same
	// folder and immediately delete it.
	if (os::open_file(ini_path.view(), os::file_open_mode::write))
	{
		return ini_path;
	}

	// File doesn't exist — try to create it. If creation succeeds the
	// directory is writable and we keep the (empty) file.
	if (os::open_file(ini_path.view(), os::file_open_mode::create_new))
	{
		return ini_path;
	}

	// Fall back to AppData\Local
	return tmp_folder().combine(s_config_app_name, "ini");
}

static os::ini_store_ptr config_store()
{
	if (!s_config_store)
	{
		s_config_store = s_config_registry_root
			                 ? os::open_registry_store(*s_config_registry_root)
			                 : os::open_ini_store(get_config_path().view());
	}
	return s_config_store;
}

std::string pf::config_read(const std::string_view section, const std::string_view key,
                            const std::string_view default_value)
{
	const auto store = config_store();
	if (!store) return std::string(default_value);
	const auto value = store->read(section, key);
	return value ? os::unescape_ini_value(*value) : std::string(default_value);
}

void pf::config_write(const std::string_view section, const std::string_view key, const std::string_view value)
{
	if (const auto store = config_store())
		store->write(section, key, os::escape_ini_value(value));
}

void pf::config_flush()
{
	if (const auto store = config_store()) store->flush();
}

bool pf::is_directory(const file_path& path)
{
	const auto attributes = os::query_file(path.view());
	return attributes.exists() && attributes.directory;
}

pf::file_path pf::current_directory()
{
	wchar_t buf[MAX_PATH];
	if (GetCurrentDirectoryW(MAX_PATH, buf))
		return file_path{utf16_to_utf8(buf)};
	return {};
}

static std::vector<os::file_dialog_filter> core_dialog_filters(const std::string_view filters)
{
	if (filters.empty()) return {{"All Files", "*.*"}, {"Text Files", "*.txt"}};

	std::vector<os::file_dialog_filter> result;
	size_t offset = 0;
	while (offset < filters.size())
	{
		const auto text_end = filters.find('\0', offset);
		if (text_end == std::string_view::npos || text_end == offset) break;
		const auto extension_begin = text_end + 1;
		if (extension_begin >= filters.size()) break;
		const auto extension_end = filters.find('\0', extension_begin);
		if (extension_end == std::string_view::npos || extension_end == extension_begin) break;
		result.push_back({std::string(filters.substr(offset, text_end - offset)),
		                  std::string(filters.substr(extension_begin, extension_end - extension_begin))});
		offset = extension_end + 1;
	}
	return result;
}

pf::file_path pf::open_file_path(const std::string_view title, const std::string_view filters)
{
	const auto result = os::open_file_dialog(reinterpret_cast<uintptr_t>(g_hWnd), title,
	                                         core_dialog_filters(filters), false);
	return result.accepted() ? file_path{result.paths.front()} : file_path{};
}

pf::file_path pf::save_file_path(const std::string_view title, const file_path& default_path,
                                 const std::string_view filters)
{
	const auto result = os::save_file_dialog(reinterpret_cast<uintptr_t>(g_hWnd), title, default_path.view(),
	                                         core_dialog_filters(filters));
	return result.accepted() ? file_path{result.paths.front()} : file_path{};
}

pf::file_path pf::pick_folder_path(const std::string_view title)
{
	const auto result = os::folder_dialog(reinterpret_cast<uintptr_t>(g_hWnd), title);
	return result.accepted() ? file_path{result.paths.front()} : file_path{};
}

int pf::show_choice(const pf::window_frame_ptr& owner, const pf::choice_definition& definition)
{
	if (definition.buttons.empty()) return 0;
	std::vector<std::wstring> labels;
	std::vector<TASKDIALOG_BUTTON> buttons;
	labels.reserve(definition.buttons.size());
	buttons.reserve(definition.buttons.size());
	for (const auto& [id, label] : definition.buttons)
	{
		labels.push_back(utf8_to_utf16(label));
		buttons.push_back({id, labels.back().c_str()});
	}

	TASKDIALOGCONFIG config = {};
	config.cbSize = sizeof(config);
	config.hwndParent = owner ? reinterpret_cast<HWND>(
		pf::platform_win::native_window_handle(owner)) : nullptr;
	const auto title = utf8_to_utf16(definition.title);
	const auto heading = utf8_to_utf16(definition.heading);
	const auto message = utf8_to_utf16(definition.message);
	config.pszWindowTitle = title.c_str();
	config.pszMainInstruction = heading.c_str();
	config.pszContent = message.c_str();
	config.pButtons = buttons.data();
	config.cButtons = static_cast<UINT>(buttons.size());
	config.nDefaultButton = definition.default_id;
	config.dwFlags = TDF_ALLOW_DIALOG_CANCELLATION;
	config.pszMainIcon = definition.warning ? TD_WARNING_ICON : TD_INFORMATION_ICON;

	int selected = 0;
	return SUCCEEDED(TaskDialogIndirect(&config, &selected, nullptr, nullptr)) ? selected : 0;
}

namespace
{
	class modal_dialog_host
	{
		pf::modal_definition _definition;
		HWND _owner = nullptr;
		HWND _window = nullptr;
		std::map<uint32_t, HWND> _controls;
		std::unordered_map<int, size_t> _command_fields;
		std::vector<HBITMAP> _preview_bitmaps;
		std::optional<std::map<uint32_t, pf::modal_value>> _result;

		static ATOM dialog_class()
		{
			static const ATOM atom = []
			{
				WNDCLASSW wc = {};
				wc.lpfnWndProc = &window_proc;
				wc.hInstance = GetModuleHandleW(nullptr);
				wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
				wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_BTNFACE + 1);
				wc.lpszClassName = L"LibdesktopModalDialog";
				return RegisterClassW(&wc);
			}();
			return atom;
		}

		static LRESULT CALLBACK window_proc(const HWND hwnd, const UINT message,
		                                   const WPARAM wparam, const LPARAM lparam)
		{
			auto* self = reinterpret_cast<modal_dialog_host*>(
				GetWindowLongPtrW(hwnd, GWLP_USERDATA));
			if (message == WM_NCCREATE)
			{
				const auto* const create = reinterpret_cast<const CREATESTRUCTW*>(lparam);
				self = static_cast<modal_dialog_host*>(create->lpCreateParams);
				SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
			}
			if (!self) return DefWindowProcW(hwnd, message, wparam, lparam);

			if (message == WM_COMMAND && HIWORD(wparam) == BN_CLICKED)
			{
				const auto id = LOWORD(wparam);
				if (id == 1)
				{
					self->accept();
					return 0;
				}
				if (id == 2)
				{
					DestroyWindow(hwnd);
					return 0;
				}
				const auto field = self->_command_fields.find(id);
				if (field != self->_command_fields.end())
				{
					auto& item = self->_definition.fields[field->second];
					if (item.action) item.action();
					return 0;
				}
			}
			if (message == WM_CLOSE)
			{
				DestroyWindow(hwnd);
				return 0;
			}
			if (message == WM_NCDESTROY)
			{
				SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
				self->_window = nullptr;
			}
			return DefWindowProcW(hwnd, message, wparam, lparam);
		}

		void add_control(const size_t index, const int y)
		{
			auto& field = _definition.fields[index];
			const auto label = pf::utf8_to_utf16(field.label);
			if (field.kind != pf::modal_field_kind::check_box &&
			    field.kind != pf::modal_field_kind::action)
				CreateWindowExW(0, L"STATIC", label.c_str(), WS_CHILD | WS_VISIBLE,
				                12, y + 4, 150, 22, _window, nullptr,
				                GetModuleHandleW(nullptr), nullptr);

			const auto id = static_cast<int>(1000 + index);
			HWND control = nullptr;
			switch (field.kind)
			{
			case pf::modal_field_kind::text:
			{
				const auto initial_text = pf::utf8_to_utf16(std::get<std::string>(field.value));
				control = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT",
				                          initial_text.c_str(),
				                          WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL,
				                          170, y, 300, 24, _window,
				                          reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
				                          GetModuleHandleW(nullptr), nullptr);
				break;
			}
			case pf::modal_field_kind::check_box:
				control = CreateWindowExW(0, L"BUTTON", label.c_str(),
				                          WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX,
				                          12, y, 458, 24, _window,
				                          reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
				                          GetModuleHandleW(nullptr), nullptr);
				if (std::get<bool>(field.value)) SendMessageW(control, BM_SETCHECK, BST_CHECKED, 0);
				break;
			case pf::modal_field_kind::choice:
				control = CreateWindowExW(0, L"COMBOBOX", nullptr,
				                          WS_CHILD | WS_VISIBLE | WS_TABSTOP | CBS_DROPDOWNLIST,
				                          170, y, 300, 180, _window,
				                          reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
				                          GetModuleHandleW(nullptr), nullptr);
				for (const auto& choice : field.choices)
					SendMessageW(control, CB_ADDSTRING, 0,
					             reinterpret_cast<LPARAM>(pf::utf8_to_utf16(choice.first).c_str()));
				break;
			case pf::modal_field_kind::search_list:
				control = CreateWindowExW(WS_EX_CLIENTEDGE, L"LISTBOX", nullptr,
				                          WS_CHILD | WS_VISIBLE | WS_TABSTOP | LBS_NOTIFY,
				                          170, y, 300, 80, _window,
				                          reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
				                          GetModuleHandleW(nullptr), nullptr);
				for (const auto& choice : field.choices)
					SendMessageW(control, LB_ADDSTRING, 0,
					             reinterpret_cast<LPARAM>(pf::utf8_to_utf16(choice.first).c_str()));
				break;
			case pf::modal_field_kind::action:
				control = CreateWindowExW(0, L"BUTTON", label.c_str(),
				                          WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
				                          12, y, 458, 26, _window,
				                          reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
				                          GetModuleHandleW(nullptr), nullptr);
				_command_fields[id] = index;
				break;
			}
			if (control)
			{
				EnableWindow(control, field.enabled ? TRUE : FALSE);
				_controls[field.id] = control;
			}
		}

		HBITMAP make_preview(const pf::modal_preview& preview)
		{
			if (preview.width <= 0 || preview.height <= 0 ||
			    preview.bgra_pixels.size() < static_cast<size_t>(preview.width) * preview.height)
				return nullptr;
			BITMAPINFO info = {};
			info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
			info.bmiHeader.biWidth = preview.width;
			info.bmiHeader.biHeight = -preview.height;
			info.bmiHeader.biPlanes = 1;
			info.bmiHeader.biBitCount = 32;
			info.bmiHeader.biCompression = BI_RGB;
			void* bits = nullptr;
			const auto dc = GetDC(_window);
			const auto bitmap = CreateDIBSection(dc, &info, DIB_RGB_COLORS, &bits, nullptr, 0);
			ReleaseDC(_window, dc);
			if (!bitmap || !bits)
			{
				if (bitmap) DeleteObject(bitmap);
				return nullptr;
			}
			std::memcpy(bits, preview.bgra_pixels.data(),
			            static_cast<size_t>(preview.width) * preview.height * sizeof(uint32_t));
			return bitmap;
		}

		void accept()
		{
			std::map<uint32_t, pf::modal_value> values;
			for (size_t i = 0; i != _definition.fields.size(); ++i)
			{
				auto& field = _definition.fields[i];
				const auto found = _controls.find(field.id);
				if (found == _controls.end()) continue;
				const auto control = found->second;
				if (field.kind == pf::modal_field_kind::text)
				{
					const auto length = GetWindowTextLengthW(control);
					std::wstring value(static_cast<size_t>(length) + 1, L'\0');
					GetWindowTextW(control, value.data(), length + 1);
					value.resize(static_cast<size_t>(length));
					values[field.id] = pf::utf16_to_utf8(value);
				}
				else if (field.kind == pf::modal_field_kind::check_box)
					values[field.id] = SendMessageW(control, BM_GETCHECK, 0, 0) == BST_CHECKED;
				else if (field.kind == pf::modal_field_kind::choice)
				{
					const auto selected = static_cast<int>(SendMessageW(control, CB_GETCURSEL, 0, 0));
					values[field.id] = selected >= 0 && selected < static_cast<int>(field.choices.size())
						                   ? field.choices[selected].second
						                   : int64_t{0};
				}
				else if (field.kind == pf::modal_field_kind::search_list)
				{
					const auto selected = static_cast<int>(SendMessageW(control, LB_GETCURSEL, 0, 0));
					values[field.id] = selected >= 0 && selected < static_cast<int>(field.choices.size())
						                   ? field.choices[selected].second
						                   : int64_t{0};
				}
			}
			if (_definition.validate)
			{
				if (const auto error = _definition.validate(values))
				{
					MessageBoxW(_window, pf::utf8_to_utf16(*error).c_str(),
					            pf::utf8_to_utf16(_definition.title).c_str(), MB_OK | MB_ICONWARNING);
					return;
				}
			}
			_result = std::move(values);
			DestroyWindow(_window);
		}

	public:
		~modal_dialog_host()
		{
			for (const auto bitmap : _preview_bitmaps)
				if (bitmap) DeleteObject(bitmap);
		}

		modal_dialog_host(HWND owner, pf::modal_definition definition)
			: _definition(std::move(definition)), _owner(owner)
		{
			dialog_class();
			const auto title = pf::utf8_to_utf16(_definition.title);
			const auto height = 120 + static_cast<int>(_definition.fields.size()) * 38 +
			                    static_cast<int>(_definition.previews.size()) * 104;
			_window = CreateWindowExW(WS_EX_DLGMODALFRAME, L"LibdesktopModalDialog", title.c_str(),
			                          WS_POPUP | WS_CAPTION | WS_SYSMENU | WS_VISIBLE,
			                          CW_USEDEFAULT, CW_USEDEFAULT, 500, height,
			                          owner, nullptr, GetModuleHandleW(nullptr), this);
			if (!_window) return;
			const auto message = pf::utf8_to_utf16(_definition.message);
			CreateWindowExW(0, L"STATIC", message.c_str(), WS_CHILD | WS_VISIBLE,
			                12, 10, 458, 32, _window, nullptr, GetModuleHandleW(nullptr), nullptr);
			int y = 48;
			for (const auto& preview : _definition.previews)
			{
				const auto bitmap = make_preview(preview);
				if (bitmap)
				{
					const auto width = std::min(458, preview.width);
					const auto height = std::min(96, preview.height);
					const auto control = CreateWindowExW(WS_EX_CLIENTEDGE, L"STATIC", nullptr,
					                                    WS_CHILD | WS_VISIBLE | SS_BITMAP | SS_CENTERIMAGE,
					                                    12, y, width, height, _window, nullptr,
					                                    GetModuleHandleW(nullptr), nullptr);
					SendMessageW(control, STM_SETIMAGE, IMAGE_BITMAP,
					             reinterpret_cast<LPARAM>(bitmap));
					_preview_bitmaps.push_back(bitmap);
				}
				y += 104;
			}
			for (size_t i = 0; i != _definition.fields.size(); ++i, y += 38)
				add_control(i, y);
			CreateWindowExW(0, L"BUTTON", pf::utf8_to_utf16(_definition.accept).c_str(),
			                WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON,
			                300, height - 58, 80, 28, _window, reinterpret_cast<HMENU>(1),
			                GetModuleHandleW(nullptr), nullptr);
			CreateWindowExW(0, L"BUTTON", pf::utf8_to_utf16(_definition.cancel).c_str(),
			                WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
			                390, height - 58, 80, 28, _window, reinterpret_cast<HMENU>(2),
			                GetModuleHandleW(nullptr), nullptr);
		}

		std::optional<std::map<uint32_t, pf::modal_value>> run()
		{
			if (!_window) return std::nullopt;
			const auto was_enabled = _owner && IsWindowEnabled(_owner);
			if (_owner) EnableWindow(_owner, FALSE);
			SetForegroundWindow(_window);
			MSG message = {};
			while (_window && GetMessageW(&message, nullptr, 0, 0) > 0)
			{
				if (!IsDialogMessageW(_window, &message))
				{
					TranslateMessage(&message);
					DispatchMessageW(&message);
				}
			}
			if (_owner)
			{
				EnableWindow(_owner, was_enabled ? TRUE : FALSE);
				SetActiveWindow(_owner);
			}
			return _result;
		}
	};
}

std::optional<std::map<uint32_t, pf::modal_value>> pf::show_modal_dialog(
	const pf::window_frame_ptr& owner, pf::modal_definition definition)
{
	const auto hwnd = owner ? reinterpret_cast<HWND>(
		pf::platform_win::native_window_handle(owner)) : nullptr;
	return modal_dialog_host(hwnd, std::move(definition)).run();
}

//  Platform locale 

std::string pf::platform_language()
{
	wchar_t buf[LOCALE_NAME_MAX_LENGTH];
	if (GetUserDefaultLocaleName(buf, LOCALE_NAME_MAX_LENGTH))
		return utf16_to_utf8(buf);
	return "en-US";
}

//  Platform Spell Checking (ISpellChecker) 

class win_spell_checker final : public pf::spell_checker
{
	ISpellChecker* _checker = nullptr;
	std::string _custom_dic_path;
	std::string _diagnostics;
	std::string _selected_language;

public:
	win_spell_checker()
	{
		ISpellCheckerFactory* factory = nullptr;
		const HRESULT hr = CoCreateInstance(__uuidof(SpellCheckerFactory), nullptr, CLSCTX_INPROC_SERVER,
		                                    IID_PPV_ARGS(&factory));
		if (FAILED(hr) || !factory)
		{
			_diagnostics = std::format("CoCreateInstance(SpellCheckerFactory) failed with HRESULT 0x{:08X}.",
			                           static_cast<uint32_t>(hr));
		}
		else
		{
			const auto lang = pf::utf8_to_utf16(pf::platform_language());
			BOOL supported = FALSE;
			const HRESULT lang_hr = factory->IsSupported(lang.c_str(), &supported);

			if (SUCCEEDED(lang_hr) && supported)
			{
				const HRESULT create_hr = factory->CreateSpellChecker(lang.c_str(), &_checker);
				if (SUCCEEDED(create_hr) && _checker)
				{
					_selected_language = pf::utf16_to_utf8(lang);
					_diagnostics = std::format("Spell checker created for {}.", _selected_language);
				}
				else
				{
					_diagnostics = std::format("CreateSpellChecker({}) failed with HRESULT 0x{:08X}.",
					                           pf::utf16_to_utf8(lang), static_cast<uint32_t>(create_hr));
				}
			}
			else
			{
				// Fall back to en-US
				const auto requested_language = pf::utf16_to_utf8(lang);
				const auto en_us = L"en-US";
				const HRESULT fallback_hr = factory->IsSupported(en_us, &supported);
				if (SUCCEEDED(fallback_hr) && supported)
				{
					const HRESULT create_hr = factory->CreateSpellChecker(en_us, &_checker);
					if (SUCCEEDED(create_hr) && _checker)
					{
						_selected_language = "en-US";
						_diagnostics = std::format(
							"Platform language {} is unsupported; spell checker created for en-US.",
							requested_language);
					}
					else
					{
						_diagnostics = std::format("CreateSpellChecker(en-US) failed with HRESULT 0x{:08X}.",
						                           static_cast<uint32_t>(create_hr));
					}
				}
				else
				{
					if (FAILED(lang_hr))
					{
						_diagnostics = std::format(
							"IsSupported({}) failed with HRESULT 0x{:08X}; en-US fallback check returned HRESULT 0x{:08X}.",
							requested_language,
							static_cast<uint32_t>(lang_hr),
							static_cast<uint32_t>(fallback_hr));
					}
					else
					{
						_diagnostics = std::format("No Windows spell-check dictionary is available for {} or en-US.",
						                           requested_language);
					}
				}
			}
			factory->Release();
		}

		if (_diagnostics.empty())
			_diagnostics = "Spell checker initialized.";

		_custom_dic_path = tmp_folder().combine(s_config_app_name, "dic").view();

		// Load custom dictionary words
		std::ifstream f(pf::utf8_to_utf16(_custom_dic_path));
		if (f.is_open())
		{
			std::string line;
			while (std::getline(f, line))
			{
				if (!line.empty() && _checker)
				{
					auto word = pf::utf8_to_utf16(pf::utf8_cast(line));
					_checker->Add(word.c_str());
				}
			}
		}
	}

	~win_spell_checker() override
	{
		if (_checker) _checker->Release();
	}

	bool available() const override
	{
		return _checker != nullptr;
	}

	std::string diagnostics() const override
	{
		return _diagnostics;
	}

	bool is_word_valid(const std::string_view word) override
	{
		if (!_checker) return true;

		const auto w = pf::utf8_to_utf16(word);
		IEnumSpellingError* errors = nullptr;
		const HRESULT hr = _checker->Check(w.c_str(), &errors);
		if (FAILED(hr) || !errors) return true;

		ISpellingError* error = nullptr;
		bool valid = true;
		if (errors->Next(&error) == S_OK && error)
		{
			valid = false;
			error->Release();
		}
		errors->Release();
		return valid;
	}

	std::vector<std::string> suggest(const std::string_view word) override
	{
		std::vector<std::string> results;
		if (!_checker) return results;

		const auto w = pf::utf8_to_utf16(word);
		IEnumString* suggestions = nullptr;
		const HRESULT hr = _checker->Suggest(w.c_str(), &suggestions);
		if (FAILED(hr) || !suggestions) return results;

		LPOLESTR suggestion = nullptr;
		while (suggestions->Next(1, &suggestion, nullptr) == S_OK)
		{
			results.emplace_back(pf::utf16_to_utf8(suggestion));
			CoTaskMemFree(suggestion);
		}
		suggestions->Release();
		return results;
	}

	void add_word(const std::string_view word) override
	{
		if (!_checker) return;

		const auto w = pf::utf8_to_utf16(word);
		_checker->Add(w.c_str());

		// Persist to custom dictionary file (word is already UTF-8)
		std::ofstream f(pf::utf8_to_utf16(_custom_dic_path), std::ios::out | std::ios::app);
		f.write(word.data(), word.size());
		f << std::endl;
	}
};

std::unique_ptr<pf::spell_checker> pf::create_spell_checker()
{
	return std::make_unique<win_spell_checker>();
}

//  Platform File I/O

struct win_file_handle final : pf::file_handle
{
	os::file_ptr _file;
	uint32_t _size = 0;

	win_file_handle(os::file_ptr file, const uint32_t size) : _file(std::move(file)), _size(size)
	{
	}

	bool read(uint8_t* buffer, const uint32_t bytesToRead, uint32_t* bytesRead) override
	{
		const auto count = _file ? _file->read(buffer, bytesToRead) : 0;
		if (bytesRead) *bytesRead = static_cast<uint32_t>(count);
		return bytesToRead == 0 || count > 0;
	}

	uint32_t size() const override { return _size; }
};

pf::file_handle_ptr pf::open_for_read(const file_path& path)
{
	return open_for_read_result(path).file;
}

pf::file_open_result pf::open_for_read_result(const file_path& path, const bool sequential_scan)
{
	const auto mode = sequential_scan ? os::file_open_mode::sequential_scan : os::file_open_mode::read;
	const auto file = os::open_file(path.view(), mode);
	if (!file)
		return {nullptr, file_failure(GetLastError())};
	const auto size = file->size();
	if (size > UINT32_MAX)
		return {nullptr, file_failure(ERROR_FILE_TOO_LARGE, "The file is larger than the 32-bit read API.")};
	return {std::make_shared<win_file_handle>(file, static_cast<uint32_t>(size)), {}};
}

class win_atomic_file_writer final : public pf::atomic_file_writer
{
	os::file_ptr _stage;
	pf::file_path _stage_path;
	pf::file_path _destination;
	bool _committed = false;
	pf::file_operation_result _failure;

	pf::file_operation_result remember_failure(pf::file_operation_result result)
	{
		if (_failure.success()) _failure = result;
		return _failure;
	}

	void discard_stage()
	{
		if (!_stage_path.empty())
		{
			_stage.reset();
			const auto removed = os::delete_file(_stage_path.view());
			if (!removed.success())
				pf::debug_trace(std::format("Atomic stage cleanup failed: {}", removed.diagnostic));
		}
	}

public:
	win_atomic_file_writer(os::file_ptr stage, pf::file_path stage_path, pf::file_path destination)
		: _stage(std::move(stage)), _stage_path(std::move(stage_path)), _destination(std::move(destination))
	{
	}

	~win_atomic_file_writer() override
	{
		if (!_committed) discard_stage();
	}

	pf::file_operation_result write(const std::span<const uint8_t> bytes) override
	{
		if (!_failure.success()) return _failure;
		if (g_test_atomic_file_failure.load(std::memory_order_relaxed) ==
		    pf::atomic_file_test_failure::write)
			return remember_failure(file_failure(ERROR_WRITE_FAULT, "The atomic writer write was forced to fail."));
		if (!_stage) return remember_failure(file_failure(ERROR_INVALID_HANDLE));
		const auto written = _stage->write(bytes.data(), bytes.size());
		if (written == bytes.size()) return {};
		return remember_failure(file_failure(GetLastError(),
		                                    "The atomic writer wrote fewer bytes than requested."));
	}

	pf::file_operation_result flush() override
	{
		if (!_failure.success()) return _failure;
		if (g_test_atomic_file_failure.load(std::memory_order_relaxed) ==
		    pf::atomic_file_test_failure::flush)
			return remember_failure(file_failure(ERROR_WRITE_FAULT, "The atomic writer flush was forced to fail."));
		if (!_stage || !_stage->flush()) return remember_failure(file_failure(GetLastError()));
		return {};
	}

	pf::file_operation_result commit() override
	{
		if (!_failure.success()) return _failure;
		if (_committed) return file_failure(ERROR_INVALID_FUNCTION, "The atomic writer was already committed.");
		if (g_test_atomic_file_failure.load(std::memory_order_relaxed) ==
		    pf::atomic_file_test_failure::commit)
			return remember_failure(file_failure(ERROR_WRITE_FAULT, "The atomic writer commit was forced to fail."));

		const auto flushed = flush();
		if (!flushed.success()) return flushed;

		_stage.reset();
		const auto moved = os::move_file(_stage_path.view(), _destination.view(), false);
		if (!moved.success())
		{
			const auto result = remember_failure(os_failure(moved));
			const auto removed = os::delete_file(_stage_path.view());
			if (!removed.success())
				pf::debug_trace(std::format("Atomic stage cleanup failed: {}", removed.diagnostic));
			return result;
		}
		_committed = true;
		return {};
	}

	pf::file_path staged_path() const override
	{
		return _stage_path;
	}
};

pf::atomic_file_writer_open_result pf::create_atomic_file_writer_result(const file_path& destination)
{
	if (destination.empty())
		return {nullptr, file_failure(ERROR_INVALID_NAME, "The atomic writer destination is empty.")};
	if (g_test_atomic_file_failure.load(std::memory_order_relaxed) ==
	    atomic_file_test_failure::open)
		return {nullptr, file_failure(ERROR_ACCESS_DENIED, "The atomic writer open was forced to fail.")};

	static std::atomic_uint32_t sequence = 0;
	bool force_collision = g_test_atomic_file_failure.load(std::memory_order_relaxed) ==
	                       atomic_file_test_failure::stage_collision;
	for (unsigned attempt = 0; attempt != 64; ++attempt)
	{
		if (force_collision)
		{
			force_collision = false;
			continue;
		}

		const auto name = std::format(".libdesktop-stage-{:08x}-{:08x}.tmp",
		                              static_cast<uint32_t>(GetCurrentProcessId()),
		                              sequence.fetch_add(1, std::memory_order_relaxed));
		const auto stage_path = destination.folder().combine(name);
		const auto stage = os::open_file(stage_path.view(), os::file_open_mode::create_new);
		if (stage)
		{
			try
			{
				return {std::make_shared<win_atomic_file_writer>(stage, stage_path, destination), {}};
			}
			catch (...)
			{
				const auto removed = os::delete_file(stage_path.view());
				if (!removed.success())
					pf::debug_trace(std::format("Atomic stage allocation cleanup failed: {}",
					                            removed.diagnostic));
				throw;
			}
		}

		const auto error = GetLastError();
		if (error != ERROR_FILE_EXISTS && error != ERROR_ALREADY_EXISTS)
			return {nullptr, file_failure(error)};
	}

	return {nullptr, file_failure(ERROR_ALREADY_EXISTS, "Unable to claim a unique atomic stage path.")};
}

pf::atomic_file_writer_ptr pf::create_atomic_file_writer(const file_path& destination)
{
	return create_atomic_file_writer_result(destination).writer;
}

void pf::test_set_atomic_file_failure(const atomic_file_test_failure failure)
{
	g_test_atomic_file_failure.store(failure, std::memory_order_relaxed);
}

//  Writable File Handle â”€

struct win_writable_file_handle final : pf::writable_file_handle
{
	os::file_ptr _file;

	explicit win_writable_file_handle(os::file_ptr file) : _file(std::move(file))
	{
	}

	uint32_t write(const uint8_t* buffer, const uint32_t bytes) override
	{
		return _file ? static_cast<uint32_t>(_file->write(buffer, bytes)) : 0;
	}
};

pf::writable_file_handle_ptr pf::open_file_for_write(const file_path& path)
{
	const auto file = os::open_file(path.view(), os::file_open_mode::create);
	if (!file) return nullptr;
	return std::make_shared<win_writable_file_handle>(file);
}

static BOOL SetProcessDpiAwarenessContextIndirect(const DPI_AWARENESS_CONTEXT dpiContext)
{
	static const auto dll = LoadLibraryW(L"user32.dll");

	if (dll != nullptr)
	{
		using PfnSetProcessDpiAwarenessContexts = int(WINAPI*)(DPI_AWARENESS_CONTEXT dpiContext);
		static auto pfn = (PfnSetProcessDpiAwarenessContexts)GetProcAddress(dll, "SetProcessDpiAwarenessContext");
		if (pfn != nullptr)
			return pfn(dpiContext);
	}

	return FALSE;
}

//  Entry Point 


INT WINAPI WinMain(const HINSTANCE hInstance, HINSTANCE, LPSTR, const int nCmdShow)
{
	// Windows-specific: Set output and input to UTF-8
	SetConsoleOutputCP(CP_UTF8);
	SetConsoleCP(CP_UTF8);

	resource_instance = hInstance;
	g_nCmdShow = nCmdShow;

	CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
	SetProcessDpiAwarenessContextIndirect(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
	init_handles();

	const auto app_statedow = std::make_shared<win_impl>();
	app_statedow->set_self_ref(app_statedow);

	int argc = 0;
	const auto argv = CommandLineToArgvW(GetCommandLineW(), &argc);

	// Convert argc/argv to u8string (skip argv[0] which is the exe path)
	std::vector<std::string> param_storage;
	for (int i = 1; i < argc; ++i)
		param_storage.push_back(pf::utf16_to_utf8(argv[i]));

	std::vector<std::string_view> params;
	for (const auto& p : param_storage)
		params.emplace_back(p);

	pf::debug_trace("WinMain: before app_init\n");

	// Bind the reactor and build menu definition before creating the HWND,
	// so WM_CREATE is delivered to the reactor's on_create handler.
	const auto init_result = app_init(app_statedow, params);
	if (!init_result.start_gui)
	{
		dispatcher().shutdown(false);
		g_instance_signals.clear();
		app_destroy();
		CloseHandle(exit_h);
		exit_h = nullptr;
		LocalFree(argv);
		CoUninitialize();
		return init_result.exit_code;
	}
	LocalFree(argv);

	pf::debug_trace("WinMain: before create\n");

	app_statedow->create_window(L"LibdesktopWnd", nullptr, {},
	                            WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN, 0, 0, init_result.window);

	pf::debug_trace("WinMain: after create\n");

	if (!app_statedow->m_hWnd)
	{
		pf::debug_trace(init_result.error_message.empty()
			                ? "WinMain: unable to create the application window\n"
			                : init_result.error_message + "\n");
		dispatcher().shutdown(false);
		CloseHandle(exit_h);
		exit_h = nullptr;
		g_instance_signals.clear();
		app_destroy();
		CoUninitialize();
		return init_result.exit_code != 0 ? init_result.exit_code : 1;
	}

	g_hWnd = app_statedow->m_hWnd;

	// Attach menu now that g_hWnd is valid
	if (g_hMenu)
		SetMenu(g_hWnd, g_hMenu);

	if (init_result.offscreen_gui)
	{
		// Park it far outside any monitor so it still paints and lays out, but
		// never appears and never takes focus.
		SetWindowPos(g_hWnd, HWND_BOTTOM, -32000, -32000, 0, 0, SWP_NOSIZE | SWP_NOACTIVATE);
		ShowWindow(g_hWnd, SW_SHOWNOACTIVATE);
	}
	else
	{
		ShowWindow(g_hWnd, g_nCmdShow);
	}
	UpdateWindow(g_hWnd);

	const int result = init_result.main_loop ? init_result.main_loop() : pf::platform_run();

	// A custom loop may return without destroying its frame. Close it while the application's
	// reactor and services are still alive; WM_DESTROY then breaks the frame ownership cycle.
	if (g_hWnd && IsWindow(g_hWnd))
	{
		DestroyWindow(g_hWnd);
		g_hWnd = nullptr;
	}
	g_instance_signals.clear();

	// The README promises app_destroy, and the applications release what they
	// hold in it — workers, network sessions, a COM spell checker. The task queue
	// stops first (platform_run has already stopped it; a main_loop has not), so
	// no queued task can start on what app_destroy frees; COM is still up. It
	// used never to run.
	dispatcher().shutdown(true);
	app_destroy();

	if (exit_h) CloseHandle(exit_h);
	exit_h = nullptr;
	CoUninitialize();
	return result;
}

pf::folder_contents pf::iterate_file_items(const file_path& folder, const bool show_hidden)
{
	const auto source = os::enumerate_files(folder.view(), show_hidden);
	folder_contents results;
	results.files.reserve(source.entries.size());
	results.folders.reserve(source.entries.size());
	for (const auto& entry : source.entries)
	{
		const auto path = folder.combine(entry.name);
		if (entry.attributes.directory)
		{
			folder_info item;
			item.path = path;
			item.attributes.is_readonly = entry.attributes.readonly;
			item.attributes.is_offline = entry.attributes.offline;
			item.attributes.is_hidden = entry.attributes.hidden;
			item.attributes.is_reparse = entry.attributes.reparse;
			item.attributes.modified = entry.attributes.modified;
			item.attributes.created = entry.attributes.created;
			item.attributes.size = entry.attributes.size;
			item.attributes.identity.volume = entry.attributes.identity.volume;
			item.attributes.identity.index = entry.attributes.identity.index;
			item.attributes.identity.valid = entry.attributes.identity.valid;
			results.folders.emplace_back(std::move(item));
		}
		else
		{
			file_info item;
			item.path = path;
			item.attributes.is_readonly = entry.attributes.readonly;
			item.attributes.is_offline = entry.attributes.offline;
			item.attributes.is_hidden = entry.attributes.hidden;
			item.attributes.is_reparse = entry.attributes.reparse;
			item.attributes.modified = entry.attributes.modified;
			item.attributes.created = entry.attributes.created;
			item.attributes.size = entry.attributes.size;
			item.attributes.identity.volume = entry.attributes.identity.volume;
			item.attributes.identity.index = entry.attributes.identity.index;
			item.attributes.identity.valid = entry.attributes.identity.valid;
			results.files.emplace_back(std::move(item));
		}
	}

	return results;
}

uint64_t pf::file_modified_time(const file_path& path)
{
	return os::query_file(path.view()).modified;
}


std::u32string pf::utf8_to_u32(const std::string_view str)
{
	std::u32string result;
	result.reserve(str.size());

	const auto u8 = utf8_cast(str);
	auto it = u8.begin();
	while (it < u8.end())
	{
		result.push_back(pop_utf8_char(it, u8.end()));
	}
	return result;
}

std::string pf::u32_to_utf8(const std::u32string_view str)
{
	std::string result;
	result.reserve(str.size());

	for (const auto cp : str)
	{
		if (cp < 0x80)
		{
			result.push_back(static_cast<char>(cp));
		}
		else if (cp < 0x800)
		{
			result.push_back(static_cast<char>(0xC0 | (cp >> 6)));
			result.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
		}
		else if (cp < 0x10000)
		{
			result.push_back(static_cast<char>(0xE0 | (cp >> 12)));
			result.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
			result.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
		}
		else
		{
			result.push_back(static_cast<char>(0xF0 | (cp >> 18)));
			result.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
			result.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
			result.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
		}
	}
	return result;
}

std::wstring pf::u32_to_wstr(const std::u32string_view str)
{
	std::wstring result;
	result.reserve(str.size());

	for (const auto cp : str)
	{
		if (cp <= 0xFFFF)
		{
			result.push_back(static_cast<wchar_t>(cp));
		}
		else
		{
			const auto adj = cp - 0x10000;
			result.push_back(static_cast<wchar_t>(0xD800 + (adj >> 10)));
			result.push_back(static_cast<wchar_t>(0xDC00 + (adj & 0x3FF)));
		}
	}
	return result;
}

std::u32string pf::wstr_to_u32(const std::wstring_view ws)
{
	std::u32string result;
	result.reserve(ws.size());

	for (size_t i = 0; i < ws.size(); ++i)
	{
		const auto ch = static_cast<uint32_t>(ws[i]);

		if (ch >= 0xD800 && ch <= 0xDBFF && i + 1 < ws.size())
		{
			const auto lo = static_cast<uint32_t>(ws[i + 1]);
			if (lo >= 0xDC00 && lo <= 0xDFFF)
			{
				result.push_back(0x10000 + ((ch - 0xD800) << 10) + (lo - 0xDC00));
				++i;
				continue;
			}
		}
		result.push_back(static_cast<char32_t>(ch));
	}
	return result;
}

static constexpr uint32_t FNV_PRIME_32 = 16777619u;
static constexpr uint32_t OFFSET_BASIS_32 = 2166136261u;

static constexpr uint64_t FNV_PRIME_64 = 1099511628211ULL;
static constexpr uint64_t OFFSET_BASIS_64 = 14695981039346656037ULL;

uint32_t pf::fnv1a_i(std::string_view sv1)
{
	auto p = sv1.begin();
	uint32_t result = OFFSET_BASIS_32;

	while (p < sv1.end())
	{
		result ^= to_lower(pop_utf8_char(p, sv1.end()));
		result *= FNV_PRIME_32;
	}

	return result;
}

uint64_t pf::fnv1a_i_64(std::string_view sv1)
{
	auto p = sv1.begin();
	uint64_t result = OFFSET_BASIS_64;

	while (p < sv1.end())
	{
		result ^= to_lower(pop_utf8_char(p, sv1.end()));
		result *= FNV_PRIME_64;
	}

	return result;
}


static_assert(std::is_move_constructible_v<pf::web_request>);
static_assert(std::is_move_constructible_v<pf::web_response>);

//  Web requests: libdesktop-os's client, in pf::'s types

bool pf::is_online()
{
	return os::is_online();
}

std::string pf::url_encode(const std::string_view input)
{
	return os::url_encode(input);
}

struct pf::web_host
{
	os::web_host_ptr native;
};

pf::web_host_ptr pf::connect_to_host(const std::string_view host, const bool secure, const int port,
                                     const std::string_view user_agent)
{
	auto native = os::connect_to_host(host, secure, port, user_agent.empty() ? "libdesktop/1.0" : user_agent);
	if (!native) return nullptr;
	auto result = std::make_shared<web_host>();
	result->native = std::move(native);
	return result;
}

pf::web_response pf::send_request(const web_host_ptr& host, const web_request& req)
{
	os::web_request native;
	native.path = req.path;
	native.body = req.body;
	native.query = req.query;
	native.headers = req.headers;
	native.form_data = req.form_data;
	native.file_form_data_name = req.file_form_data_name;
	native.file_name = req.file_name;
	native.upload_file_path = req.upload_file_path.view();
	native.download_file_path = req.download_file_path.view();
	native.verb = req.verb == web_request_verb::GET ? os::web_request_verb::GET : os::web_request_verb::POST;
	native.follow_redirects = req.follow_redirects;
	native.use_cookies = req.use_cookies;
	native.max_response_bytes = req.max_response_bytes;
	native.max_header_bytes = req.max_header_bytes;
	native.timeout_ms = req.timeout_ms;

	auto response = os::send_request(host ? host->native : nullptr, native);

	web_response result;
	result.headers = std::move(response.headers);
	result.body = std::move(response.body);
	result.content_type = std::move(response.content_type);
	result.status_code = response.status_code;

	switch (response.error)
	{
	case os::web_response_error::none: result.error = web_response_error::none; break;
	case os::web_response_error::invalid_request: result.error = web_response_error::invalid_request; break;
	case os::web_response_error::transport: result.error = web_response_error::transport; break;
	case os::web_response_error::response_limit: result.error = web_response_error::response_limit; break;
	}

	return result;
}

//  WIC Bitmap Loading & Async HTTP & Toolbar (Address Bar) 
//
// These implementations sit at the bottom of the file so they can be reviewed
// (and eventually moved out) as a single block.

// WIC bitmap loading was extracted to src/core/platform_win_image.cpp.


//  Async HTTP (WinHTTP) 

namespace
{
	class win_async_http_request final : public pf::async_http_request,
	                                     public std::enable_shared_from_this<win_async_http_request>
	{
		// Protects handles + cancelled flag. The callback thread also holds this.
		std::mutex _mtx;
		HINTERNET _connect = nullptr;
		HINTERNET _request = nullptr;
		bool _cancelled = false;
		bool _headers_done = false;

		pf::async_http_callbacks _cb;
		std::vector<uint8_t> _read_buf;

		// Keep a self-reference alive for the lifetime of the WinHTTP request,
		// so callbacks can safely deref the object even if the caller drops
		// their handle. Cleared on completion / error / cancel.
		std::shared_ptr<win_async_http_request> _self_keep_alive;

		void close_handles_locked()
		{
			if (_request)
			{
				WinHttpCloseHandle(_request);
				_request = nullptr;
			}
			if (_connect)
			{
				WinHttpCloseHandle(_connect);
				_connect = nullptr;
			}
		}

		void fail(std::string err)
		{
			pf::async_http_callbacks cb;
			{
				std::lock_guard lk(_mtx);
				if (_cancelled) return;
				_cancelled = true;
				close_handles_locked();
				cb = std::move(_cb);
				// _self_keep_alive is released only on HANDLE_CLOSING
			}
			if (cb.on_error) cb.on_error(std::move(err));
		}

		void finish()
		{
			pf::async_http_callbacks cb;
			{
				std::lock_guard lk(_mtx);
				if (_cancelled) return;
				_cancelled = true; // prevent re-entry from late callbacks
				close_handles_locked();
				cb = std::move(_cb);
				// _self_keep_alive is released only on HANDLE_CLOSING
			}
			if (cb.on_complete) cb.on_complete();
		}

	public:
		win_async_http_request() = default;

		~win_async_http_request() override
		{
			std::lock_guard lk(_mtx);
			close_handles_locked();
		}

		void cancel() override
		{
			{
				std::lock_guard lk(_mtx);
				if (_cancelled) return;
				_cancelled = true;
				close_handles_locked();
				// _self_keep_alive is released on HANDLE_CLOSING
			}
		}

		// Begin the request. Caller must hold a shared_ptr to *this so we can
		// store the keep-alive copy.
		bool start(HINTERNET session, const std::string& url, pf::async_http_callbacks cb)
		{
			_cb = std::move(cb);

			URL_COMPONENTS uc = {};
			uc.dwStructSize = sizeof(uc);
			uc.dwSchemeLength = static_cast<DWORD>(-1);
			uc.dwHostNameLength = static_cast<DWORD>(-1);
			uc.dwUrlPathLength = static_cast<DWORD>(-1);
			uc.dwExtraInfoLength = static_cast<DWORD>(-1);
			const auto wurl = pf::utf8_to_utf16(url);
			if (!WinHttpCrackUrl(wurl.c_str(), 0, 0, &uc))
			{
				fail("invalid url");
				return false;
			}

			std::wstring host(uc.lpszHostName, uc.dwHostNameLength);
			std::wstring path(uc.lpszUrlPath, uc.dwUrlPathLength + uc.dwExtraInfoLength);

			HINTERNET connect = WinHttpConnect(session, host.c_str(), uc.nPort, 0);
			if (!connect)
			{
				fail(std::format("connect failed (err {})", GetLastError()));
				return false;
			}

			// Detect HTTPS robustly. uc.nScheme is only populated by
			// WinHttpCrackUrl in some configurations; the documented behaviour
			// when lpszScheme/dwSchemeLength are used to receive a pointer +
			// length into the URL leaves nScheme set to 0
			// (INTERNET_SCHEME_UNKNOWN). Without WINHTTP_FLAG_SECURE the
			// request is sent as plain HTTP to port 443, which servers reject
			// (Cloudflare returns "400 The plain HTTP request was sent to
			// HTTPS port"; Google/Wikipedia surface as 12152).
			const bool is_https =
				uc.nScheme == INTERNET_SCHEME_HTTPS ||
				uc.nPort == INTERNET_DEFAULT_HTTPS_PORT ||
				(uc.lpszScheme != nullptr && uc.dwSchemeLength == 5 &&
					_wcsnicmp(uc.lpszScheme, L"https", 5) == 0);
			DWORD flags = 0;
			if (is_https) flags |= WINHTTP_FLAG_SECURE;
			HINTERNET request = WinHttpOpenRequest(connect, L"GET", path.c_str(),
			                                       nullptr, nullptr,
			                                       nullptr, flags);
			if (!request)
			{
				const auto err = GetLastError();
				WinHttpCloseHandle(connect);
				fail(std::format("open request failed (err {})", err));
				return false;
			}

			// Install callback BEFORE registration so we don't miss any events.
			WinHttpSetStatusCallback(request,
			                         &win_async_http_request::s_callback,
			                         WINHTTP_CALLBACK_FLAG_ALL_COMPLETIONS |
			                         WINHTTP_CALLBACK_FLAG_HANDLES,
			                         0);

			{
				std::lock_guard lk(_mtx);
				if (_cancelled)
				{
					WinHttpCloseHandle(request);
					WinHttpCloseHandle(connect);
					return false;
				}
				_connect = connect;
				_request = request;
				// Take the keep-alive *before* WinHttpSendRequest so any
				// callback fired between send and our return finds a valid
				// self-reference.
				_self_keep_alive = shared_from_this();
			}

			if (!WinHttpSendRequest(request,
			                        nullptr, 0,
			                        nullptr, 0,
			                        0, reinterpret_cast<DWORD_PTR>(this)))
			{
				fail(std::format("send request failed (err {})", GetLastError()));
				return false;
			}

			return true;
		}

	private:
		void on_send_complete()
		{
			HINTERNET req;
			{
				std::lock_guard lk(_mtx);
				if (_cancelled) return;
				req = _request;
			}
			if (!WinHttpReceiveResponse(req, nullptr))
				fail("receive response failed");
		}

		void on_headers_available()
		{
			HINTERNET req;
			pf::async_http_callbacks cb_copy;
			{
				std::lock_guard lk(_mtx);
				if (_cancelled) return;
				req = _request;
				_headers_done = true;
				cb_copy = _cb;
			}

			DWORD status = 0;
			DWORD sz = sizeof(status);
			WinHttpQueryHeaders(req,
			                    WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
			                    nullptr,
			                    &status, &sz, nullptr);

			std::string content_type;
			DWORD ct_sz = 0;
			WinHttpQueryHeaders(req, WINHTTP_QUERY_CONTENT_TYPE,
			                    nullptr, nullptr, &ct_sz,
			                    nullptr);
			if (ct_sz > 0)
			{
				std::wstring ct(ct_sz / sizeof(wchar_t), L'\0');
				if (WinHttpQueryHeaders(req, WINHTTP_QUERY_CONTENT_TYPE,
				                        nullptr,
				                        ct.data(), &ct_sz, nullptr))
				{
					if (!ct.empty() && ct.back() == L'\0') ct.pop_back();
					content_type = pf::utf16_to_utf8(ct);
				}
			}

			uint64_t content_length = 0;
			// Query Content-Length as a string and parse to uint64_t to
			// avoid truncating responses larger than 4 GiB.
			DWORD cl_sz = 0;
			WinHttpQueryHeaders(req, WINHTTP_QUERY_CONTENT_LENGTH,
			                    nullptr, nullptr, &cl_sz, nullptr);
			if (cl_sz > 0)
			{
				std::wstring cl_str(cl_sz / sizeof(wchar_t), L'\0');
				if (WinHttpQueryHeaders(req, WINHTTP_QUERY_CONTENT_LENGTH,
				                        nullptr,
				                        cl_str.data(), &cl_sz, nullptr))
				{
					while (!cl_str.empty() && cl_str.back() == L'\0') cl_str.pop_back();
					try { content_length = std::stoull(cl_str); }
					catch (...) { content_length = 0; }
				}
			}

			if (cb_copy.on_headers)
				cb_copy.on_headers(static_cast<int>(status), std::move(content_type), content_length);

			query_data_available();
		}

		void query_data_available()
		{
			HINTERNET req;
			{
				std::lock_guard lk(_mtx);
				if (_cancelled) return;
				req = _request;
			}
			if (!WinHttpQueryDataAvailable(req, nullptr))
				fail("query data available failed");
		}

		void on_data_available(const DWORD bytes)
		{
			if (bytes == 0)
			{
				finish();
				return;
			}
			HINTERNET req;
			{
				std::lock_guard lk(_mtx);
				if (_cancelled) return;
				req = _request;
				_read_buf.assign(bytes, 0);
			}
			if (!WinHttpReadData(req, _read_buf.data(), bytes, nullptr))
				fail("read data failed");
		}

		void on_read_complete(const DWORD bytes)
		{
			pf::async_http_callbacks cb_copy;
			{
				std::lock_guard lk(_mtx);
				if (_cancelled) return;
				cb_copy = _cb;
			}
			if (bytes == 0)
			{
				finish();
				return;
			}
			if (cb_copy.on_data)
				cb_copy.on_data(_read_buf.data(), bytes);

			query_data_available();
		}

		static void CALLBACK s_callback(HINTERNET, const DWORD_PTR ctx,
		                                const DWORD status,
		                                LPVOID info, const DWORD info_len)
		{
			if (ctx == 0) return;
			auto* self = reinterpret_cast<win_async_http_request*>(ctx);

			switch (status)
			{
			case WINHTTP_CALLBACK_STATUS_SENDREQUEST_COMPLETE:
				self->on_send_complete();
				break;
			case WINHTTP_CALLBACK_STATUS_HEADERS_AVAILABLE:
				self->on_headers_available();
				break;
			case WINHTTP_CALLBACK_STATUS_DATA_AVAILABLE:
				self->on_data_available(info ? *static_cast<DWORD*>(info) : 0);
				break;
			case WINHTTP_CALLBACK_STATUS_READ_COMPLETE:
				self->on_read_complete(info_len);
				break;
			case WINHTTP_CALLBACK_STATUS_REQUEST_ERROR:
				{
					std::string err = "winhttp request error";
					if (info)
					{
						const auto* const ar = static_cast<const WINHTTP_ASYNC_RESULT*>(info);
						err = std::format("winhttp error {} (code {})",
						                  static_cast<unsigned>(ar->dwResult),
						                  static_cast<unsigned>(ar->dwError));
					}
					self->fail(std::move(err));
				}
				break;
			case WINHTTP_CALLBACK_STATUS_HANDLE_CLOSING:
				{
					// Release the keep-alive reference now that WinHTTP has
					// confirmed the handle is fully closed and no further
					// callbacks will fire for it. This may delete `self`,
					// so do nothing else after.
					std::shared_ptr<win_async_http_request> keep;
					{
						std::lock_guard lk(self->_mtx);
						keep = std::move(self->_self_keep_alive);
					}
					(void)keep;
				}
				break;
			default:
				break;
			}
		}
	};

	class win_async_http_session final : public pf::async_http_session,
	                                     public std::enable_shared_from_this<win_async_http_session>
	{
		HINTERNET _session = nullptr;
		std::mutex _mtx;
		std::vector<std::weak_ptr<win_async_http_request>> _requests;

	public:
		explicit win_async_http_session(const std::wstring& user_agent)
		{
			_session = WinHttpOpen(user_agent.c_str(),
			                       WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
			                       nullptr,
			                       nullptr,
			                       WINHTTP_FLAG_ASYNC);
			if (_session)
			{
				// Apply 30s timeouts to prevent indefinite hangs on slow endpoints.
				WinHttpSetTimeouts(_session, 30000, 30000, 30000, 30000);
				// Enable modern TLS. WinHTTP defaults often exclude TLS 1.2/1.3,
				// causing handshake failures against most modern HTTPS sites.
				DWORD secure_protocols = WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_2;
#ifdef WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_3
				secure_protocols |= WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_3;
#endif
				WinHttpSetOption(_session, WINHTTP_OPTION_SECURE_PROTOCOLS,
				                 &secure_protocols, sizeof(secure_protocols));

				// Opt in to HTTP/2. Many sites speak only HTTP/2; without
				// this WinHTTP fails the response with
				// ERROR_HTTP_INVALID_SERVER_RESPONSE (12152).
				//
				// We deliberately do NOT enable HTTP/3: the
				// WINHTTP_PROTOCOL_FLAG_HTTP3 constant is present in recent
				// SDK headers but the underlying OS support is gated on
				// Windows 11 22H2+ and even there negotiation can fail
				// against many CDNs, surfacing as 12152.
#ifdef WINHTTP_OPTION_ENABLE_HTTP_PROTOCOL
				DWORD protocols = WINHTTP_PROTOCOL_FLAG_HTTP2;
				WinHttpSetOption(_session, WINHTTP_OPTION_ENABLE_HTTP_PROTOCOL,
				                 &protocols, sizeof(protocols));
#endif

				DWORD decompress = WINHTTP_DECOMPRESSION_FLAG_ALL;
				WinHttpSetOption(_session, WINHTTP_OPTION_DECOMPRESSION,
				                 &decompress, sizeof(decompress));
				DWORD redirect = WINHTTP_OPTION_REDIRECT_POLICY_ALWAYS;
				WinHttpSetOption(_session, WINHTTP_OPTION_REDIRECT_POLICY,
				                 &redirect, sizeof(redirect));
			}
		}

		~win_async_http_session() override
		{
			stop();
			if (_session) WinHttpCloseHandle(_session);
		}

		pf::async_http_request_ptr get(const std::string_view url, pf::async_http_callbacks cb) override
		{
			if (!_session)
			{
				if (cb.on_error) cb.on_error("no session");
				return nullptr;
			}
			auto req = std::make_shared<win_async_http_request>();
			{
				std::lock_guard lk(_mtx);
				_requests.emplace_back(req);
			}
			req->start(_session, std::string(url), std::move(cb));
			return req;
		}

		void stop() override
		{
			std::vector<std::weak_ptr<win_async_http_request>> snapshot;
			{
				std::lock_guard lk(_mtx);
				snapshot.swap(_requests);
			}
			for (auto& w : snapshot)
				if (auto p = w.lock()) p->cancel();
		}
	};
}

pf::async_http_session_ptr pf::create_async_http_session(const std::string_view user_agent)
{
	const auto agent = user_agent.empty() ? std::wstring(L"libdesktop/1.0") : utf8_to_utf16(user_agent);
	return std::make_shared<win_async_http_session>(agent);
}

//  Toolbar / Address Bar (Win32) â”€
//
// A platform-rendered toolbar widget. Implementation is a custom child
// window that owns:
//   - a row of icon-font buttons on the left
//   - an optional editable URL field (real Win32 EDIT subclass) in the middle
//   - a row of icon-font buttons on the right

namespace
{
	// ── Tunable layout constants (logical pixels at 96 DPI) ───────────────
	// All values are passed through dpi_scale() at the use site so the UI
	// scales correctly on high-DPI displays.

	// Address-bar EDIT control font (Segoe UI, character height in pt-ish
	// units; the negative sign is required by CreateFontW to mean
	// "character height" rather than "cell height").
	constexpr int k_address_edit_font_size = 16;
	// Toolbar button glyph font (Segoe Fluent / MDL2 icon font).
	constexpr int k_toolbar_glyph_font_size = 21;
	// Outer padding around all toolbar children.
	constexpr int k_toolbar_padding = 4;
	// Horizontal inset between the address-bar border rect and the EDIT.
	constexpr int k_address_edit_inset_x = 6;
	// Fallback EDIT line height when the font can't be measured.
	constexpr int k_address_edit_fallback_height = 20;
	// Suggestion popup row height.
	constexpr int k_suggest_popup_item_height = 28;
	// Suggestion popup horizontal text padding.
	constexpr int k_suggest_popup_text_inset_x = 8;

	// Window-property key used to attach the win_address_bar* to its parent
	// toolbar HWND (we cannot use GWLP_USERDATA there \u2014 win_impl owns it).
	constexpr auto k_address_bar_prop = L"PotatoAddressBarPtr";

	struct toolbar_btn_runtime
	{
		pf::toolbar_button cfg;
		pf::irect bounds;
		bool hovered = false;
	};

	class win_address_bar final : public pf::toolbar_frame,
	                              public std::enable_shared_from_this<win_address_bar>
	{
		std::shared_ptr<win_impl> _frame;
		pf::address_bar_config _cfg;

		std::vector<toolbar_btn_runtime> _left;
		std::vector<toolbar_btn_runtime> _right;
		pf::irect _edit_bounds;

		// Popup menus attached to specific buttons (by id) via set_menu.
		std::unordered_map<int, std::vector<pf::menu_command>> _button_menus;

		HWND _edit = nullptr;
		HFONT _edit_font = nullptr;
		WNDPROC _edit_orig_proc = nullptr;
		// Subclass on the parent toolbar HWND so we can answer
		// WM_CTLCOLOREDIT and paint the EDIT background to match the
		// pale-gray rect drawn behind it.
		WNDPROC _parent_orig_proc = nullptr;
		HBRUSH _edit_bg_brush = nullptr;

		// Suggestions dropdown popup. The popup is a borderless WS_POPUP
		// window with WS_EX_NOACTIVATE so showing / clicking it never moves
		// keyboard focus away from the EDIT control.
		HWND _popup_hwnd = nullptr;
		std::vector<std::string> _popup_items;
		int _popup_selected = -1; // keyboard selection (-1 = "use edit text")
		int _popup_hover = -1; // mouse hover index
		int _popup_item_h = 24;

		// Text the user actually typed (before any popup-driven preview).
		// Restored when the popup selection moves back to "no selection".
		std::string _typed_text;
		// The needle currently used for filtering — stored so the popup
		// can highlight the matched substring inside each result.
		std::string _popup_needle;
		// Edit text at the moment focus was acquired. Restored on Escape
		// or when the edit loses focus without the user committing a
		// navigation (e.g. they click back into the page).
		std::string _focus_original_text;
		// True between a successful Enter / item-click and the next focus
		// acquisition; suppresses "restore original" on the WM_KILLFOCUS
		// that might follow a navigation.
		bool _committed = false;

		// reactor that forwards Win32 messages (paint/mouse) to us.
		struct reactor : pf::frame_reactor
		{
			win_address_bar* owner = nullptr;

			uint32_t handle_message(pf::window_frame_ptr, pf::message_type m,
			                        const pf::message_params&) override
			{
				if (m == pf::message_type::erase_background) return 1;
				return 0;
			}

			uint32_t handle_mouse(pf::window_frame_ptr, pf::mouse_message_type m,
			                      const pf::mouse_params& p) override
			{
				if (!owner) return 0;
				switch (m)
				{
				case pf::mouse_message_type::mouse_move: owner->on_mouse_move(p.point);
					break;
				case pf::mouse_message_type::left_button_down: owner->on_left_down(p.point);
					break;
				case pf::mouse_message_type::mouse_leave: owner->on_mouse_leave();
					break;
				case pf::mouse_message_type::set_cursor: return 1;
				default: break;
				}
				return 0;
			}

			void handle_paint(pf::window_frame_ptr&, pf::draw_context& dc) override
			{
				if (owner) owner->paint(dc);
			}

			void handle_size(pf::window_frame_ptr&, pf::isize, pf::measure_context&) override
			{
				if (owner) owner->layout();
			}
		};

		std::shared_ptr<reactor> _reactor;

		int dpi_scale(const int v) const
		{
			return static_cast<int>(v * _frame->get_dpi_scale());
		}

	public:
		win_address_bar(std::shared_ptr<win_impl> frame, pf::address_bar_config cfg)
			: _frame(std::move(frame)), _cfg(std::move(cfg))
		{
			for (auto& b : _cfg.left_buttons) _left.push_back({b, {}});
			for (auto& b : _cfg.right_buttons) _right.push_back({b, {}});
		}

		void initialise()
		{
			_reactor = std::make_shared<reactor>();
			_reactor->owner = this;
			_frame->set_reactor(_reactor);

			if (_cfg.style == pf::toolbar_style::address_bar)
			{
				_edit = CreateWindowExW(
					0, L"EDIT", nullptr,
					WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL,
					0, 0, 100, 24,
					_frame->m_hWnd,
					nullptr,
					resource_instance,
					nullptr);

				if (_edit)
				{
					if (!_cfg.initial_text.empty())
						SetWindowTextW(_edit, pf::utf8_to_utf16(_cfg.initial_text).c_str());

					_edit_font = CreateFontW(
						-dpi_scale(k_address_edit_font_size), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
						DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
						CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS,
						L"Segoe UI");
					SendMessageW(_edit, WM_SETFONT, reinterpret_cast<WPARAM>(_edit_font), TRUE);

					SetWindowLongPtrW(_edit, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(this));
					_edit_orig_proc = reinterpret_cast<WNDPROC>(
						SetWindowLongPtrW(_edit, GWLP_WNDPROC,
						                  reinterpret_cast<LONG_PTR>(&win_address_bar::s_edit_proc)));

					// Subclass parent so WM_CTLCOLOREDIT can return our
					// brush, painting the EDIT's background to match the
					// edit_background rect drawn in paint().
					//
					// NOTE: do NOT use GWLP_USERDATA on the parent HWND —
					// win_impl already stores its own `this` there. Use a
					// window property instead.
					_edit_bg_brush = CreateSolidBrush(_cfg.edit_background.rgb());
					SetPropW(_frame->m_hWnd, k_address_bar_prop,
					         this);
					_parent_orig_proc = reinterpret_cast<WNDPROC>(
						SetWindowLongPtrW(_frame->m_hWnd, GWLP_WNDPROC,
						                  reinterpret_cast<LONG_PTR>(&win_address_bar::s_parent_proc)));
				}
			}

			layout();
		}

		~win_address_bar() override
		{
			if (_popup_hwnd)
			{
				DestroyWindow(_popup_hwnd);
				_popup_hwnd = nullptr;
			}
			if (_edit && _edit_orig_proc)
				SetWindowLongPtrW(_edit, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(_edit_orig_proc));
			if (_parent_orig_proc && _frame)
			{
				SetWindowLongPtrW(_frame->m_hWnd, GWLP_WNDPROC,
				                  reinterpret_cast<LONG_PTR>(_parent_orig_proc));
				RemovePropW(_frame->m_hWnd, k_address_bar_prop);
			}
			if (_edit_font) DeleteObject(_edit_font);
			if (_edit_bg_brush) DeleteObject(_edit_bg_brush);
		}

		//  pf::toolbar_frame 
		pf::window_frame_ptr frame() override { return _frame; }

		int preferred_height() const override
		{
			return dpi_scale(_cfg.height);
		}

		void set_address_text(const std::string_view text) override
		{
			if (_edit) SetWindowTextW(_edit, pf::utf8_to_utf16(text).c_str());
		}

		std::string address_text() const override
		{
			if (!_edit) return {};
			const auto len = GetWindowTextLengthW(_edit);
			if (len <= 0) return {};
			std::wstring buf(len + 1, L'\0');
			GetWindowTextW(_edit, buf.data(), len + 1);
			buf.resize(len);
			return pf::utf16_to_utf8(buf);
		}

		void focus_address() override
		{
			if (_edit) SetFocus(_edit);
		}

		void select_all_address() override
		{
			if (_edit) SendMessageW(_edit, EM_SETSEL, 0, -1);
		}

		void set_button_enabled(const int id, const bool enabled) override
		{
			for (auto* group : {&_left, &_right})
				for (auto& b : *group)
					if (b.cfg.id == id)
					{
						b.cfg.is_enabled = [enabled] { return enabled; };
					}
			_frame->invalidate();
		}

		void set_button_glyph(const int id, const uint32_t glyph) override
		{
			for (auto* group : {&_left, &_right})
				for (auto& b : *group)
					if (b.cfg.id == id)
						b.cfg.glyph = glyph;
			_frame->invalidate();
		}

		void set_menu(const int button_id, std::vector<pf::menu_command> items) override
		{
			if (items.empty())
				_button_menus.erase(button_id);
			else
				_button_menus[button_id] = std::move(items);
		}

	private:
		void layout()
		{
			const auto rc = _frame->get_client_rect();
			const int btn_w = dpi_scale(_cfg.button_width);
			const int padding = dpi_scale(k_toolbar_padding);

			int x = padding;
			for (auto& b : _left)
			{
				b.bounds = pf::irect(x, padding, x + btn_w, rc.bottom - padding);
				x += btn_w;
			}
			const int left_end = x + padding;

			int xr = rc.right - padding;
			for (auto it = _right.rbegin(); it != _right.rend(); ++it)
			{
				it->bounds = pf::irect(xr - btn_w, padding, xr, rc.bottom - padding);
				xr -= btn_w;
			}
			const int right_start = xr - padding;

			_edit_bounds = pf::irect(left_end, padding, right_start, rc.bottom - padding);

			if (_edit && _edit_bounds.width() > 0 && _edit_bounds.height() > 0)
			{
				// Measure the EDIT's font so we can size the control to match
				// a single line of text and centre it vertically inside the
				// visible border rect. EDIT controls top-align text within
				// their client area, so the only reliable way to vertically
				// centre is to make the control itself the height of the text.
				int text_h = dpi_scale(k_address_edit_fallback_height); // sensible fallback
				if (_edit_font)
				{
					HDC hdc = GetDC(_edit);
					auto old = static_cast<HFONT>(SelectObject(hdc, _edit_font));
					TEXTMETRICW tm{};
					GetTextMetricsW(hdc, &tm);
					SelectObject(hdc, old);
					ReleaseDC(_edit, hdc);
					text_h = tm.tmHeight + tm.tmExternalLeading;
				}

				const int inset_x = dpi_scale(k_address_edit_inset_x);
				const int edit_h = text_h;
				const int edit_y = _edit_bounds.top + (_edit_bounds.height() - edit_h) / 2;
				MoveWindow(_edit,
				           _edit_bounds.left + inset_x,
				           edit_y,
				           _edit_bounds.width() - inset_x * 2,
				           edit_h,
				           TRUE);

				// Remove the EDIT's default left/right margins so text starts
				// flush against our inset.
				SendMessageW(_edit, EM_SETMARGINS,
				             EC_LEFTMARGIN | EC_RIGHTMARGIN,
				             MAKELPARAM(0, 0));
			}
		}

		void paint_button(pf::draw_context& dc, const toolbar_btn_runtime& b)
		{
			const bool enabled = !b.cfg.is_enabled || b.cfg.is_enabled();
			const bool checked = b.cfg.is_checked && b.cfg.is_checked();

			const auto bg = (b.hovered && enabled) ? _cfg.button_hover : _cfg.background;
			dc.fill_solid_rect(b.bounds, bg);

			// Checked indicator: accent underline at the bottom of the button.
			if (checked)
			{
				const int bar_h = std::max(2, b.bounds.height() / 14);
				const pf::irect bar{b.bounds.left + 4, b.bounds.bottom - bar_h - 1,
				                    b.bounds.right - 4, b.bounds.bottom - 1};
				dc.fill_solid_rect(bar, _cfg.button_checked);
			}

			if (b.cfg.glyph == 0) return;

			const std::u32string cp_str(1, static_cast<char32_t>(b.cfg.glyph));
			const auto utf8 = pf::u32_to_utf8(cp_str);

			pf::font f;
			f.name = pf::font_name::segoe_icons;
			f.size = dpi_scale(k_toolbar_glyph_font_size);

			const auto sz = dc.measure_text(utf8, f);
			const int tx = b.bounds.left + (b.bounds.width() - sz.cx) / 2;
			const int ty = b.bounds.top + (b.bounds.height() - sz.cy) / 2;

			const auto color = !enabled ? _cfg.button_color.lighten(80)
			                 : checked  ? _cfg.button_checked
			                            : _cfg.button_color;
			dc.draw_text(tx, ty, b.bounds, utf8, f, color, bg);
		}

		void paint(pf::draw_context& dc)
		{
			const auto rc = _frame->get_client_rect();
			dc.fill_solid_rect(rc, _cfg.background);

			if (_cfg.style == pf::toolbar_style::address_bar)
			{
				dc.fill_solid_rect(_edit_bounds, _cfg.edit_background);
				const pf::ipoint pts[5] = {
					{_edit_bounds.left, _edit_bounds.top},
					{_edit_bounds.right - 1, _edit_bounds.top},
					{_edit_bounds.right - 1, _edit_bounds.bottom - 1},
					{_edit_bounds.left, _edit_bounds.bottom - 1},
					{_edit_bounds.left, _edit_bounds.top},
				};
				dc.draw_lines(std::span<const pf::ipoint>(pts, 5), _cfg.border_color);
			}

			for (auto& b : _left) paint_button(dc, b);
			for (auto& b : _right) paint_button(dc, b);
		}

		void on_mouse_move(const pf::ipoint pt)
		{
			bool changed = false;
			for (auto* group : {&_left, &_right})
				for (auto& b : *group)
				{
					const bool h = b.bounds.contains(pt);
					if (h != b.hovered)
					{
						b.hovered = h;
						changed = true;
					}
				}
			if (changed)
			{
				_frame->invalidate();
				_frame->track_mouse_leave();
			}
		}

		void on_mouse_leave()
		{
			bool changed = false;
			for (auto* group : {&_left, &_right})
				for (auto& b : *group)
					if (b.hovered)
					{
						b.hovered = false;
						changed = true;
					}
			if (changed) _frame->invalidate();
		}

		void on_left_down(const pf::ipoint pt)
		{
			for (auto* group : {&_left, &_right})
				for (auto& b : *group)
				{
					if (!b.bounds.contains(pt)) continue;
					if (b.cfg.is_enabled && !b.cfg.is_enabled()) return;

					// If a menu is attached to this button, show it
					// beneath the button instead of invoking the action.
					const auto it = _button_menus.find(b.cfg.id);
					if (it != _button_menus.end() && !it->second.empty())
					{
						POINT p{b.bounds.left, b.bounds.bottom};
						ClientToScreen(_frame->m_hWnd, &p);
						_frame->show_popup_menu(it->second, pf::ipoint(p.x, p.y));
						return;
					}

					if (b.cfg.action) b.cfg.action();
					return;
				}
		}

		// Subclass proc on the EDIT control: drives the suggestions popup
		// (Enter / Escape / arrows / text changes) and forwards everything
		// else to the standard EDIT proc.
		static LRESULT CALLBACK s_edit_proc(HWND h, UINT m, WPARAM w, LPARAM l)
		{
			auto* self = reinterpret_cast<win_address_bar*>(GetWindowLongPtrW(h, GWLP_USERDATA));
			if (!self || !self->_edit_orig_proc)
				return DefWindowProcW(h, m, w, l);

			if (m == WM_KEYDOWN)
			{
				switch (w)
				{
				case VK_RETURN:
					self->choose_selection();
					return 0;
				case VK_ESCAPE:
					// Escape always restores the text the field had when
					// focus was acquired, hides the popup, and yields
					// focus back to the content view.
					self->restore_focus_original();
					self->hide_suggestions();
					if (self->_frame)
					{
						HWND hp = GetParent(self->_frame->m_hWnd);
						if (hp) SetFocus(hp);
					}
					return 0;
				case VK_DOWN:
					if (self->is_popup_visible())
					{
						self->move_selection(1);
						return 0;
					}
					break;
				case VK_UP:
					if (self->is_popup_visible())
					{
						self->move_selection(-1);
						return 0;
					}
					break;
				default:
					break;
				}
			}
			if (m == WM_CHAR && (w == VK_RETURN || w == VK_ESCAPE))
				return 0; // suppress beep

			// Suppress the KEYUP for keys whose KEYDOWN we handled above.
			// Otherwise the trailing KEYUP runs through the EDIT proc and
			// our update_suggestions() call below either re-shows a popup
			// we just hid (Enter/Escape) or re-queries with the mirrored
			// preview text and hides the popup mid-navigation (Up/Down).
			if (m == WM_KEYUP && (w == VK_RETURN || w == VK_ESCAPE ||
			                      w == VK_UP || w == VK_DOWN))
				return 0;

			// First click into an unfocused EDIT: select the whole URL
			// instead of positioning the caret. Subsequent clicks (when
			// the field is already focused) behave normally.
			if (m == WM_LBUTTONDOWN && GetFocus() != h)
			{
				SetFocus(h); // triggers WM_SETFOCUS -> select-all path below
				return 0;
			}

			if (m == WM_KILLFOCUS)
			{
				self->hide_suggestions();
				if (!self->_committed)
					self->restore_focus_original();
			}

			const auto r = CallWindowProcW(self->_edit_orig_proc, h, m, w, l);

			// On focus, snapshot the text (so Escape / blur can restore
			// it), select everything, and show the default suggestions
			// (common bookmarks). Done after the original proc so the
			// EDIT has finished its own focus handling first.
			if (m == WM_SETFOCUS)
			{
				self->_focus_original_text = self->address_text();
				self->_typed_text = self->_focus_original_text;
				self->_committed = false;
				SendMessageW(h, EM_SETSEL, 0, -1);
				self->update_suggestions();
			}

			// Text may have changed â€” refresh suggestions. Skip if the
			// user has just committed a navigation (the popup must stay
			// hidden until focus returns to the EDIT).
			if (!self->_committed && (m == WM_CHAR || m == WM_KEYUP || m == WM_PASTE || m == WM_CUT))
				self->update_suggestions();

			return r;
		}

		// Subclass proc on the parent toolbar HWND: only used to answer
		// WM_CTLCOLOREDIT so the EDIT's own background paint matches the
		// pale-gray rect we draw behind it. Everything else falls through
		// to the toolbar's normal reactor-based WndProc.
		static LRESULT CALLBACK s_parent_proc(HWND h, UINT m, WPARAM w, LPARAM l)
		{
			auto* self = reinterpret_cast<win_address_bar*>(GetPropW(h, k_address_bar_prop));
			if (self && self->_parent_orig_proc)
			{
				if (m == WM_CTLCOLOREDIT && reinterpret_cast<HWND>(l) == self->_edit)
				{
					const auto hdc = reinterpret_cast<HDC>(w);
					SetTextColor(hdc, self->_cfg.text_color.rgb());
					SetBkColor(hdc, self->_cfg.edit_background.rgb());
					return reinterpret_cast<LRESULT>(self->_edit_bg_brush);
				}
				return CallWindowProcW(self->_parent_orig_proc, h, m, w, l);
			}
			return DefWindowProcW(h, m, w, l);
		}

		//  Suggestions popup 

		bool is_popup_visible() const
		{
			return _popup_hwnd && IsWindowVisible(_popup_hwnd);
		}

		void update_suggestions()
		{
			if (!_cfg.on_suggest)
			{
				hide_suggestions();
				return;
			}
			// Always ask the host for suggestions (including for empty text
			// — lets the host return common bookmarks on first focus).
			const auto text = address_text();
			// Snapshot the user's typed text so move_selection() can
			// restore it when keyboard navigation returns to "no
			// selection".
			_typed_text = text;
			_popup_needle = text;
			_popup_items = _cfg.on_suggest(text);
			if (_popup_items.empty())
			{
				hide_suggestions();
				return;
			}
			_popup_selected = -1;
			_popup_hover = -1;
			show_suggestions();
		}

		void show_suggestions()
		{
			ensure_suggest_class();
			if (!_popup_hwnd)
			{
				_popup_hwnd = CreateWindowExW(
					WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW | WS_EX_TOPMOST,
					L"PotatoSuggestPopup", nullptr,
					WS_POPUP,
					0, 0, 100, 100,
					_frame->m_hWnd, nullptr,
					resource_instance, this);
				if (!_popup_hwnd) return;
				SetWindowLongPtrW(_popup_hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(this));
			}

			_popup_item_h = dpi_scale(k_suggest_popup_item_height);
			POINT pt = {_edit_bounds.left, _edit_bounds.bottom};
			ClientToScreen(_frame->m_hWnd, &pt);
			const int w = _edit_bounds.width();
			const int h = static_cast<int>(_popup_items.size()) * _popup_item_h + 2;

			SetWindowPos(_popup_hwnd, HWND_TOPMOST, pt.x, pt.y, w, h,
			             SWP_NOACTIVATE | SWP_SHOWWINDOW);
			InvalidateRect(_popup_hwnd, nullptr, FALSE);
		}

		void hide_suggestions()
		{
			_popup_items.clear();
			_popup_selected = -1;
			_popup_hover = -1;
			if (_popup_hwnd && IsWindowVisible(_popup_hwnd))
				ShowWindow(_popup_hwnd, SW_HIDE);
		}

		void move_selection(const int delta)
		{
			if (_popup_items.empty()) return;
			const int n = static_cast<int>(_popup_items.size());
			int sel = _popup_selected + delta;
			if (sel < -1) sel = n - 1;
			if (sel >= n) sel = -1;
			_popup_selected = sel;

			// Mirror the selection into the edit field so the user sees what
			// pressing Enter would navigate to. When the selection wraps
			// back to "none", restore the text the user actually typed.
			if (_edit)
			{
				const std::string& preview = (_popup_selected >= 0)
					                             ? _popup_items[_popup_selected]
					                             : _typed_text;
				SetWindowTextW(_edit, pf::utf8_to_utf16(preview).c_str());
				SendMessageW(_edit, EM_SETSEL, 0, -1);
			}

			if (_popup_hwnd) InvalidateRect(_popup_hwnd, nullptr, FALSE);
		}

		void choose_selection()
		{
			std::string url;
			if (_popup_selected >= 0 && _popup_selected < static_cast<int>(_popup_items.size()))
				url = _popup_items[_popup_selected];
			else
				url = address_text();
			_committed = true;
			hide_suggestions();
			// Move focus off the EDIT before invoking the host callback, so
			// the page receives keyboard input once it loads (and any
			// trailing key events don't re-open the suggestions popup).
			if (_frame)
			{
				HWND hp = GetParent(_frame->m_hWnd);
				if (hp) SetFocus(hp);
			}
			if (_cfg.on_navigate) _cfg.on_navigate(url);
		}

		// Restore the address text that was present when the EDIT
		// last gained focus. Used by Escape and by the WM_KILLFOCUS
		// path when no navigation was committed.
		void restore_focus_original()
		{
			if (!_edit) return;
			SetWindowTextW(_edit, pf::utf8_to_utf16(_focus_original_text).c_str());
			SendMessageW(_edit, EM_SETSEL, 0, -1);
		}

		void paint_popup()
		{
			PAINTSTRUCT ps;
			const HDC hdc = BeginPaint(_popup_hwnd, &ps);

			RECT rc;
			GetClientRect(_popup_hwnd, &rc);

			const auto bg = CreateSolidBrush(_cfg.background.rgb());
			FillRect(hdc, &rc, bg);
			DeleteObject(bg);

			const auto border = CreateSolidBrush(_cfg.border_color.rgb());
			FrameRect(hdc, &rc, border);
			DeleteObject(border);

			const auto old_font = static_cast<HFONT>(SelectObject(
				hdc, _edit_font ? _edit_font : static_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT))));
			SetBkMode(hdc, TRANSPARENT);

			const int n = static_cast<int>(_popup_items.size());

			// Lower-cased needle for case-insensitive substring matching
			// (works on the raw UTF-8 bytes — adequate for the ASCII URLs
			// users typically type into an address bar).
			std::string needle_lc = _popup_needle;
			for (auto& c : needle_lc)
				c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));

			for (int i = 0; i < n; ++i)
			{
				RECT r{1, 1 + i * _popup_item_h, rc.right - 1, 1 + (i + 1) * _popup_item_h};
				const bool selected = (i == _popup_selected) || (i == _popup_hover && _popup_selected < 0);
				if (selected)
				{
					const auto hb = CreateSolidBrush(_cfg.button_hover.rgb());
					FillRect(hdc, &r, hb);
					DeleteObject(hb);
				}

				const std::string& item = _popup_items[i];
				RECT tr = r;
				tr.left += dpi_scale(k_suggest_popup_text_inset_x);
				tr.right -= dpi_scale(k_suggest_popup_text_inset_x);

				// Highlight the matched portion (if any) by painting a
				// yellow band behind it. Drawn before the text so the
				// transparent-background DrawText overlays cleanly.
				if (!needle_lc.empty())
				{
					std::string item_lc = item;
					for (auto& c : item_lc)
						c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));

					const auto pos = item_lc.find(needle_lc);
					if (pos != std::string::npos)
					{
						const auto pre = pf::utf8_to_utf16(item.substr(0, pos));
						const auto mid = pf::utf8_to_utf16(item.substr(pos, needle_lc.size()));

						SIZE sz_pre{}, sz_mid{};
						GetTextExtentPoint32W(hdc, pre.c_str(),
						                      static_cast<int>(pre.size()), &sz_pre);
						GetTextExtentPoint32W(hdc, mid.c_str(),
						                      static_cast<int>(mid.size()), &sz_mid);

						const long hl_left = tr.left + sz_pre.cx;
						const long hl_right = std::min<long>(tr.right, hl_left + sz_mid.cx);
						if (hl_right > hl_left)
						{
							RECT hr{hl_left, r.top + 1, hl_right, r.bottom - 1};
							const auto hb = CreateSolidBrush(RGB(255, 235, 130));
							FillRect(hdc, &hr, hb);
							DeleteObject(hb);
						}
					}
				}

				SetTextColor(hdc, _cfg.text_color.rgb());
				const auto wtext = pf::utf8_to_utf16(item);
				DrawTextW(hdc, wtext.c_str(), static_cast<int>(wtext.size()), &tr,
				          DT_SINGLELINE | DT_VCENTER | DT_LEFT | DT_END_ELLIPSIS | DT_NOPREFIX);
			}

			SelectObject(hdc, old_font);
			EndPaint(_popup_hwnd, &ps);
		}

		static void ensure_suggest_class()
		{
			static bool registered = false;
			if (registered) return;
			WNDCLASSEXW wcx = {};
			wcx.cbSize = sizeof(wcx);
			wcx.style = CS_SAVEBITS | CS_DROPSHADOW;
			wcx.lpfnWndProc = &win_address_bar::s_popup_proc;
			wcx.hInstance = resource_instance;
			wcx.hCursor = LoadCursor(nullptr, IDC_ARROW);
			wcx.hbrBackground = nullptr;
			wcx.lpszClassName = L"PotatoSuggestPopup";
			RegisterClassExW(&wcx);
			registered = true;
		}

		static LRESULT CALLBACK s_popup_proc(HWND h, UINT m, WPARAM w, LPARAM l)
		{
			auto* self = reinterpret_cast<win_address_bar*>(GetWindowLongPtrW(h, GWLP_USERDATA));

			switch (m)
			{
			case WM_MOUSEACTIVATE:
				return MA_NOACTIVATE; // never steal focus from the EDIT
			case WM_ERASEBKGND:
				return 1;
			case WM_PAINT:
				if (self)
				{
					self->paint_popup();
					return 0;
				}
				break;
			case WM_MOUSEMOVE:
				if (self)
				{
					const int y = GET_Y_LPARAM(l);
					const int idx = self->_popup_item_h > 0 ? (y - 1) / self->_popup_item_h : -1;
					const int n = static_cast<int>(self->_popup_items.size());
					const int new_hover = (idx >= 0 && idx < n) ? idx : -1;
					if (new_hover != self->_popup_hover)
					{
						self->_popup_hover = new_hover;
						InvalidateRect(h, nullptr, FALSE);
					}
				}
				return 0;
			case WM_LBUTTONDOWN:
				if (self)
				{
					const int y = GET_Y_LPARAM(l);
					const int idx = self->_popup_item_h > 0 ? (y - 1) / self->_popup_item_h : -1;
					const int n = static_cast<int>(self->_popup_items.size());
					if (idx >= 0 && idx < n)
					{
						self->_popup_selected = idx;
						if (self->_edit)
							SetWindowTextW(self->_edit,
							               pf::utf8_to_utf16(self->_popup_items[idx]).c_str());
						self->choose_selection();
					}
				}
				return 0;
			default:
				break;
			}
			return DefWindowProcW(h, m, w, l);
		}
	};
}

pf::toolbar_frame_ptr win_impl::create_address_bar(const pf::address_bar_config& cfg)
{
	auto child = std::static_pointer_cast<win_impl>(create_child("PotatoToolbar", pf::window_style::child |
	                                                             pf::window_style::visible |
	                                                             pf::window_style::clip_children, cfg.background));
	auto bar = std::make_shared<win_address_bar>(child, cfg);
	bar->initialise();
	return bar;
}

pf::toolbar_frame_ptr pf::create_address_bar_for_hwnd(const uintptr_t parent_native_handle,
                                                      const address_bar_config& cfg)
{
	const auto parent = std::bit_cast<HWND>(parent_native_handle);
	auto child = std::make_shared<win_impl>();
	child->create_window(L"PotatoToolbar", parent, cfg.background,
	                     WS_CHILD | WS_VISIBLE | WS_CLIPCHILDREN, 0);
	child->set_self_ref(child);
	auto bar = std::make_shared<win_address_bar>(child, cfg);
	bar->initialise();
	return bar;
}
