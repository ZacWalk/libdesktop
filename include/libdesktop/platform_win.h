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
using namespace Microsoft::WRL;


HWND app_wnd();
// The application's main window, which the window layer names when it creates it.
void set_app_wnd(HWND wnd);
// How the window layer brings its frames' enabled state back after a modal system dialog; it installs
// this when it starts, and platform::sync_app_window_enabled calls it.
void set_app_window_enabled_sync(void (*sync)());
// How the window layer waits on the UI thread while still handling the window's messages; it installs
// this when it starts. platform::wait_for asks it first, and waits plainly when it answers nothing,
// as it does on any other thread.
using ui_thread_wait = std::optional<uint32_t> (*)(const std::vector<std::reference_wrapper<platform::thread_event>>& events,
                                                   bool wait_all, uint32_t timeout_ms);
void set_ui_thread_wait(ui_thread_wait wait);

// The release the running system reports. Reads no files, so the UI thread may ask.
df::os_release windows_release();

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
		sizei bitmap_dimensions;
		sizei dib_dimensions;
	};

	data_object_probe probe_drag_data_object(const std::vector<df::file_path>& files,
	                                         const std::vector<df::folder_path>& folders);
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
	std::wstring probe_shell_destination_w(df::folder_path target);
	std::string probe_shell_destination_a(df::folder_path target);
	FILEOP_FLAGS probe_shell_delete_flags(bool allow_undo);

	// Shell and common-dialog APIs reject the \\?\ prefix that to_file_system_path adds for long
	// paths, so they take the plain form and accept the MAX_PATH limit those APIs already impose.
	// There is no cross-platform notion of a shell path, so this is Windows-private.
	std::wstring to_shell_path(df::file_path path);
	std::wstring to_shell_path(df::folder_path path);

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

// A path arriving from a Win32 API is UTF-16; df::file_path and df::folder_path are UTF-8 and know
// nothing of wide strings. These name that conversion once, on the side of the boundary that has a
// reason to know about it.
inline df::file_path to_file_path(const std::wstring_view w)
{
	return df::file_path(str::utf16_to_utf8(w));
}

inline df::folder_path to_folder_path(const std::wstring_view w)
{
	return df::folder_path(str::utf16_to_utf8(w));
}

class data_object_client : public platform::clipboard_data
{
	ComPtr<IDataObject> _pData;

public:
	data_object_client(IDataObject* pData);

	bool has_data(FORMATETC* pf) const;
	bool has_drop_files() const override;
	bool has_bitmap() const override;
	platform::file_op_result drop_files(df::folder_path target, platform::drop_effect effect) override;
	platform::clipboard_bitmap_ptr capture_bitmap() override;
	platform::file_op_result save_bitmap(df::folder_path save_path, std::string_view name, bool as_png) override;
	DWORD preferred_drop_effect() const;
	description files_description() const override;
	df::file_path first_path() const override;
	std::vector<df::file_path> drop_paths() const override;
};

df::blob load_resource(int id, LPCWSTR lpType);

platform::file_op_result save_bitmap_info(df::folder_path save_path, std::string_view name, bool as_png,
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

	explicit bstr_t(const std::string_view sv) : m_str(SysAllocString(str::utf8_to_utf16(sv).c_str()))
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
		df::assert_true(m_str == nullptr);
		return &m_str;
	}
};
