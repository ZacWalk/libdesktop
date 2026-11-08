// This file is part of libdesktop, a C++20 platform layer for desktop applications
// Copyright 2026  Zac Walker
// 
// This program is free software; you can redistribute it and / or modify it
// under the terms of the LGPL License either version 2.1 or later.
// License details are available at https://www.gnu.org/licenses/lgpl-2.1.html
// This program is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY

// Purpose: Windows platform implementation. Implements file system, shell integration,
// registry access, and Windows-specific functionality.

#pragma once

#define WINVER _WIN32_WINNT_WIN7
#define _WIN32_WINNT _WIN32_WINNT_WIN7
#define _WIN32_WINDOWS _WIN32_WINNT_WIN7
#define _WIN32_IE _WIN32_IE_IE110
#define NTDDI_VERSION   NTDDI_WIN7

#ifndef WIN32_LEAN_AND_MEAN
// WIN32_LEAN_AND_MEAN implies NOCRYPT and NOGDI.
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX 
#endif
#ifndef NOKERNEL
#define NOKERNEL
#endif
#ifndef NOSERVICE
#define NOSERVICE
#endif
#ifndef NOSOUND
#define NOSOUND
#endif
#ifndef NOMCX
#define NOMCX
#endif

#ifndef STRICT
#define STRICT
#endif
#define _WINSOCKAPI_    // stops windows.h including winsock.h

#include <dxgi.h>
#include <dxgi1_4.h> // IDXGIAdapter3, for the video-memory gauge in the session perf summary
#include <windows.h>
#include <Shellapi.h>
#include <wrl.h>
using Microsoft::WRL::ComPtr;

namespace platform { class window; }
// Fixed window timer used only by the Windows platform to age user inactivity during native modal loops.
inline constexpr UINT platform_activity_timer_id = 0x4c444154;
namespace platform
{
	// Window placement conversion and native caption-button seams used by the Windows backend and its tests.
	core::recti fit_window_to_work_area(core::recti saved, core::recti work_area);
	core::recti placement_to_screen(core::recti saved, core::recti monitor, core::recti work_area);
	core::recti screen_to_placement(core::recti screen, core::recti monitor, core::recti work_area);
	bool window_needs_refit(core::recti saved, core::recti work_area, bool reaches_a_display);
	bool can_present_maximize_caption_button(os_release release, uint32_t root_style);
	bool presents_maximize_caption_button(os_release release, uint32_t root_style);

	struct caption_button_probe
	{
		bool offered = false;
		int maximize_id = 0;
		int maximize_hit = HTNOWHERE;
		int other_hit = HTNOWHERE;
		int maximized_hit = HTNOWHERE;
		int frameless_hit = HTNOWHERE;
		int hovered_id = 0;
		int hover_after_leave = 0;
		int invocations = 0;
	};

	caption_button_probe probe_toolbar_caption_button();
}
namespace platform
{
	key_state wheel_key_state(uint32_t message, WPARAM parameter, key_state keyboard);
}


HWND app_wnd();
// The application's main window, which the window layer names when it creates it.
void set_app_wnd(HWND wnd);
// Shows every app-kind window after app::init has completed, preserving a saved maximize state.
void show_app_windows();
// Clears the mouse-move baseline after a fullscreen transition.
void reset_mouse_move_baseline();
// Restores the application window's reference-counted enabled state after a modal system dialog.
void sync_app_window_enabled();
// Finds a platform window for a native HWND while the UI thread is dispatching a modal loop.
platform::window* platform_window_from_native(HWND hwnd);
// How the window layer brings its frames' enabled state back after a modal system dialog; it installs
// this when it starts, and platform::sync_app_window_enabled calls it.
void set_app_window_enabled_sync(void (*sync)());
// How the window layer waits on the UI thread while still handling the window's messages; it installs
// this when it starts. platform::wait_for asks it first, and waits plainly when it answers nothing,
// as it does on any other thread.
using ui_thread_wait = std::optional<uint32_t> (*)(const std::vector<std::reference_wrapper<core::thread_event>>& events,
                                                   bool wait_all, uint32_t timeout_ms);
void set_ui_thread_wait(ui_thread_wait wait);

// The release the running system reports. Reads no files, so the UI thread may ask.
platform::os_release windows_release();

extern HINSTANCE get_resource_instance;
FILETIME ts_to_ft(uint64_t ts);
uint64_t ft_to_ts(const FILETIME& ft);

namespace platform
{
	uint64_t utc_to_local(uint64_t ts, const TIME_ZONE_INFORMATION* zone);
	uint64_t local_to_utc(uint64_t ts, const TIME_ZONE_INFORMATION* zone);

	// Test seams for the current zone's yearly offset tables: a rule set converted through its own
	// tables (0 where a year could not be tabulated), and the system conversions made on this thread.
	std::vector<uint64_t> tabulated_time_zone_conversions(std::span<const uint64_t> stamps,
	                                                     const TIME_ZONE_INFORMATION& zone, bool to_local);
	uint64_t time_zone_system_queries();
}

// Wrap an already-open Win32 file HANDLE in a platform::file. Takes ownership of the
// handle (closed when the returned file_ptr is released). Used by replace_file to hand
// back the still-open, cache-coherent handle it renamed through, so the file can be
// read back immediately without a stale by-name reopen over SMB. See platform_win_files.cpp.
namespace platform
{
	file_ptr make_file_from_handle(HANDLE h);

	using shell_file_operation_w_fn = decltype(&SHFileOperationW);
	using shell_file_operation_a_fn = decltype(&SHFileOperationA);

	struct shell_operation_probe
	{
		file_op_result result;
		std::wstring destination_w;
		std::string destination_a;
		FILEOP_FLAGS flags = {};
	};

	// What a shell drag source actually advertises. Windows-only by nature: the formats it names
	// are clipboard format ids, and the paths come back in the shell's own UTF-16.
	struct data_object_probe
	{
		std::vector<uint32_t> enum_formats; // cfFormat ids, in EnumFormatEtc (source-preference) order
		int hdrop_enum_index = -1; // position of CF_HDROP within enum_formats (-1 = absent)
		int shell_id_list_enum_index = -1; // position of CFSTR_SHELLIDLIST within enum_formats (-1 = absent)
		bool advertises_hdrop = false; // QueryGetData(CF_HDROP)
		bool advertises_shell_id_list = false; // QueryGetData(CFSTR_SHELLIDLIST)
		int hdrop_count = -1; // files parsed from CF_HDROP (-1 = no data returned)
		int shell_id_list_count = -1; // CIDA cidl from CFSTR_SHELLIDLIST (-1 = no data returned)
		std::vector<std::wstring> hdrop_paths;
		std::vector<std::wstring> shell_id_list_paths;
		std::vector<uint32_t> image_formats;
		std::vector<uint32_t> image_tymed;
		bool advertises_bitmap = false;
		bool advertises_dib = false;
		bool bitmap_retrieved = false;
		bool dib_retrieved = false;
		core::sizei bitmap_dimensions;
		core::sizei dib_dimensions;
	};

	data_object_probe probe_drag_data_object(const std::vector<core::file_path>& files,
	                                         const std::vector<core::folder_path>& folders);
	data_object_probe probe_drag_data_object(const clipboard_image_ptr& image);
	struct clipboard_contract_probe
	{
		bool clone_keeps_position = false;
		bool skip_to_end = false;
		bool skip_past_end = false;
		bool preferred_effect = false;
		bool rejects_wrong_medium = false;
	};
	clipboard_contract_probe probe_clipboard_contracts();
	std::wstring dialog_default_extension(std::string_view extension);

	shell_operation_probe probe_shell_file_operation_w(shell_file_operation_w_fn op);
	shell_operation_probe probe_shell_file_operation_a(shell_file_operation_a_fn op);
	shell_operation_probe probe_shell_delete_operation(shell_file_operation_w_fn op, bool allow_undo);
	std::wstring probe_shell_destination_w(core::folder_path target);
	std::string probe_shell_destination_a(core::folder_path target);
	FILEOP_FLAGS probe_shell_delete_flags(bool allow_undo);

	// Shell and common-dialog APIs reject the \\?\ prefix that to_file_system_path adds for long
	// paths, so they take the plain form and accept the MAX_PATH limit those APIs already impose.
	// There is no cross-platform notion of a shell path, so this is Windows-private.
	std::wstring to_shell_path(core::file_path path);
	std::wstring to_shell_path(core::folder_path path);

	struct number_format_probe_snapshot
	{
		std::wstring decimal_sep;
		std::wstring thousand_sep;
		uint32_t leading_zero = 1;
		uint32_t negative_order = 1;
	};

	void set_number_format_probe_snapshots(std::vector<number_format_probe_snapshot> snapshots,
	                                       bool invalidate_during_refresh);
	void clear_number_format_probe_snapshots();
}

std::string win32_to_string(const IID& iid);

// A path arriving from a Win32 API is UTF-16; core::file_path and core::folder_path are UTF-8 and know
// nothing of wide strings. These name that conversion once, on the side of the boundary that has a
// reason to know about it.
inline core::file_path to_file_path(const std::wstring_view w)
{
	return core::file_path(core::utf16_to_utf8(w));
}

inline core::folder_path to_folder_path(const std::wstring_view w)
{
	return core::folder_path(core::utf16_to_utf8(w));
}

// The text platform::set_clipboard offers. Tabs and line breaks pass through, so pasted text keeps its
// columns and lines; every other control character becomes '.', which the application that receives
// it cannot act on.
std::string clipboard_safe_text(std::string_view text);

class data_object_client : public platform::clipboard_data
{
	ComPtr<IDataObject> _pData;

public:
	data_object_client(IDataObject* pData);

	bool has_data(FORMATETC* format) const;
	bool has_drop_files() const override;
	bool has_bitmap() const override;
	platform::file_op_result drop_files(core::folder_path target, platform::drop_effect effect) override;
	platform::clipboard_bitmap_ptr capture_bitmap() override;
	platform::file_op_result save_bitmap(core::folder_path save_path, std::string_view name, bool as_png) override;
	DWORD preferred_drop_effect() const;
	description files_description() const override;
	core::file_path first_path() const override;
	std::vector<core::file_path> drop_paths() const override;
};

core::blob load_resource(int id, LPCWSTR lpType);

platform::file_op_result save_bitmap_info(core::folder_path save_path, std::string_view name, bool as_png,
                                          HBITMAP image_buffer_in);


struct variant_t
{
	VARIANT v = {};

	variant_t() noexcept
	{
		VariantInit(&v);
	}

	~variant_t() noexcept
	{
		VariantClear(&v);
	}

	variant_t(const variant_t& other) = delete;
	variant_t& operator=(const variant_t& other) = delete;
};

struct prop_variant_t
{
	PROPVARIANT v = {};

	prop_variant_t() noexcept
	{
		PropVariantInit(&v);
	}

	~prop_variant_t() noexcept
	{
		PropVariantClear(&v);
	}

	// PropVariantClear releases what v points to, so a copy would release it twice. The deleted pair
	// used to name variant_t, which left this type's own copy operations generated.
	prop_variant_t(const prop_variant_t& other) = delete;
	prop_variant_t& operator=(const prop_variant_t& other) = delete;
};


struct bstr_t
{
	BSTR m_str = nullptr;

	bstr_t() = default;

	explicit bstr_t(const std::wstring_view sv) : m_str(SysAllocStringLen(sv.data(), static_cast<uint32_t>(sv.size())))
	{
	}

	explicit bstr_t(const std::string_view sv) : m_str(SysAllocString(core::utf8_to_utf16(sv).c_str()))
	{
	}

	bstr_t(const bstr_t& other) = delete;
	bstr_t& operator=(const bstr_t& other) = delete;

	operator BSTR() const noexcept
	{
		return m_str;
	}

	~bstr_t() noexcept
	{
		if (m_str)
		{
			SysFreeString(m_str);
		}
	}

	BSTR* operator&() noexcept
	{
		core::assert_true(m_str == nullptr);
		return &m_str;
	}
};

// This file is part of libdesktop, a C++20 platform layer for desktop applications
// Copyright 2026  Zac Walker
//
// This program is free software; you can redistribute it and / or modify it
// under the terms of the LGPL License either version 2.1 or later.
// License details are available at https://www.gnu.org/licenses/lgpl-2.1.html
// This program is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY

// Purpose: What platform::run needs from process.cpp, which carries the process lifecycle around the
// window. Windows-only, and for the application's window layer alone.

#pragma once

// The running application, for the crash and recovery callbacks Windows makes with no context.
extern std::weak_ptr<platform::process_events> g_process_events;

// Binds stdout and stderr for the application's command-line modes, such as a /test run.
void setup_headless_console();

// A bare message box, for failures before there is a window to report them in.
void show_fatal_error(std::string_view message);

// Registers the command line Windows restarts with, and the recovery callback that saves state when
// a hung or crashing process is terminated.
void setup_restart(std::string_view restart_cmd_line);
void unregister_restart();

#ifndef WINSTORE
// Writes a minidump and reports the crash for as long as it is in scope.
struct unhandled_exception_filter
{
	PTOP_LEVEL_EXCEPTION_FILTER _original = nullptr;

	unhandled_exception_filter();
	~unhandled_exception_filter();
};
#endif
