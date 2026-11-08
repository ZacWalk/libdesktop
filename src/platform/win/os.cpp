// This file is part of libdesktop, a C++20 platform layer for desktop applications
// Copyright 2026  Zac Walker
// 
// This program is free software; you can redistribute it and / or modify it
// under the terms of the LGPL License either version 2.1 or later.
// License details are available at https://www.gnu.org/licenses/lgpl-2.1.html
// This program is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY

// Purpose: The Windows os:: services - clipboard text, known folders, and the synchronous web
// client, which uses WinINet and switches to WinHTTP for a request bounded by a timeout or a limit.

#include "os.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <shlobj.h>
#include <commdlg.h>
#include <shellapi.h>
#include <shobjidl.h>
#include <WinInet.h>
#include <winhttp.h>

#pragma comment(lib, "wininet.lib")
#pragma comment(lib, "winhttp.lib")

#include "os_text.h"

#include <algorithm>
#include <atomic>
#include <charconv>
#include <climits>
#include <cstddef>
#include <cstring>
#include <format>
#include <memory>
#include <mutex>
#include <optional>
#include <vector>

namespace
{
	using os::detail::narrow;
	using os::detail::widen;

	bool iequals(const std::string_view a, const std::string_view b)
	{
		return std::ranges::equal(a, b, [](char x, char y)
		{
			if (x >= 'A' && x <= 'Z') x = static_cast<char>(x - 'A' + 'a');
			if (y >= 'A' && y <= 'Z') y = static_cast<char>(y - 'A' + 'a');
			return x == y;
		});
	}

	// The extension is the part after the last '.' of the file name, not of a folder.
	bool has_extension(const std::string_view path, const std::string_view extension)
	{
		const auto dot = path.find_last_of("./\\");
		return dot != std::string_view::npos && path[dot] == '.' && iequals(path.substr(dot), extension);
	}

	class file_handle
	{
		HANDLE _h = INVALID_HANDLE_VALUE;

	public:
		explicit file_handle(const HANDLE h) : _h(h)
		{
		}

		~file_handle()
		{
			if (_h != INVALID_HANDLE_VALUE) CloseHandle(_h);
		}

		file_handle(const file_handle&) = delete;
		file_handle& operator=(const file_handle&) = delete;

		[[nodiscard]] bool is_valid() const { return _h != INVALID_HANDLE_VALUE; }
		[[nodiscard]] HANDLE get() const { return _h; }
		HANDLE release()
		{
			const auto result = _h;
			_h = INVALID_HANDLE_VALUE;
			return result;
		}
	};

	std::wstring native_path(const std::string_view path)
	{
		auto result = widen(path);
		if (result.empty() || result.starts_with(LR"(\\?\)") || result.starts_with(LR"(\\.\)"))
			return result;

		const bool drive_rooted = result.size() >= 3 && result[1] == L':' &&
			(result[2] == L'\\' || result[2] == L'/');
		const bool unc_rooted = result.size() >= 2 && result[0] == L'\\' && result[1] == L'\\';
		// CreateDirectoryW needs the extended form a little before MAX_PATH because it reserves
		// room for an 8.3 child; using the same threshold for all file APIs keeps one path policy.
		if (result.size() < MAX_PATH - 12 || (!drive_rooted && !unc_rooted))
			return result;

		if (unc_rooted)
			return LR"(\\?\UNC\)" + result.substr(2);
		return LR"(\\?\)" + result;
	}

	uint64_t file_time(const FILETIME& time)
	{
		return (static_cast<uint64_t>(time.dwHighDateTime) << 32) | time.dwLowDateTime;
	}

	std::string error_text(const DWORD error)
	{
		if (error == ERROR_SUCCESS) return {};

		LPWSTR raw = nullptr;
		const auto length = FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
		                                   FORMAT_MESSAGE_IGNORE_INSERTS, nullptr, error,
		                                   MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
		                                   reinterpret_cast<LPWSTR>(&raw), 0, nullptr);
		if (length == 0 || raw == nullptr)
			return std::format("Windows error {}", error);

		auto result = narrow(std::wstring_view(raw, length));
		LocalFree(raw);
		while (!result.empty() && (result.back() == '\r' || result.back() == '\n' || result.back() == ' '))
			result.pop_back();
		return result;
	}

	os::file_operation operation_result(const BOOL success)
	{
		if (success) return {os::file_operation_code::ok, ERROR_SUCCESS, {}};

		const auto error = GetLastError();
		const auto code = error == ERROR_FILE_EXISTS || error == ERROR_ALREADY_EXISTS
			                  ? os::file_operation_code::already_exists
			                  : os::file_operation_code::failed;
		auto diagnostic = error_text(error);
		if (diagnostic.empty()) diagnostic = std::format("Windows error {}", error);
		return {code, error, std::move(diagnostic)};
	}

	// FAT and exFAT number a file by the directory holding it, so a volume's root - held by none - is 0.
	bool numbers_root_zero(const HANDLE handle)
	{
		wchar_t file_system[MAX_PATH + 1] = {};
		if (!GetVolumeInformationByHandleW(handle, nullptr, 0, nullptr, nullptr, nullptr, file_system,
		                                   MAX_PATH + 1))
		{
			return false;
		}

		for (const auto* const name : {L"FAT", L"FAT12", L"FAT16", L"FAT32", L"exFAT"})
		{
			if (_wcsicmp(file_system, name) == 0) return true;
		}

		return false;
	}

	uint64_t low_half(const FILE_ID_128& id)
	{
		uint64_t result = 0;
		std::memcpy(&result, id.Identifier, sizeof(result));
		return result;
	}

	uint64_t high_half(const FILE_ID_128& id)
	{
		uint64_t result = 0;
		std::memcpy(&result, id.Identifier + sizeof(result), sizeof(result));
		return result;
	}

	// The file system's own ID for an open file, when it names one file. The 128-bit ID is asked for where
	// the file system keeps one, because on ReFS the 64-bit ID the older query returns can name two files.
	void fill_identity(const HANDLE handle, os::file_attributes& result)
	{
		BY_HANDLE_FILE_INFORMATION info = {};
		if (!GetFileInformationByHandle(handle, &info)) return;

		auto low = (static_cast<uint64_t>(info.nFileIndexHigh) << 32) | info.nFileIndexLow;
		uint64_t high = 0;

		FILE_ID_INFO id = {};
		if (GetFileInformationByHandleEx(handle, FileIdInfo, &id, sizeof(id)))
		{
			low = low_half(id.FileId);
			high = high_half(id.FileId);
		}

		const auto index = os::identity_index(low, high, low == 0 && high == 0 && numbers_root_zero(handle));
		if (!index) return;

		result.identity.volume = info.dwVolumeSerialNumber;
		result.identity.index = *index;
		result.identity.valid = true;
	}

	void fill_attributes(os::file_attributes& result, const DWORD attributes,
	                     const FILETIME& created, const FILETIME& modified,
	                     const DWORD size_high, const DWORD size_low)
	{
		result.presence = os::file_presence::found;
		result.directory = (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
		result.readonly = (attributes & FILE_ATTRIBUTE_READONLY) != 0;
		result.hidden = (attributes & FILE_ATTRIBUTE_HIDDEN) != 0;
		result.reparse = (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
		result.offline = (attributes & (FILE_ATTRIBUTE_OFFLINE | FILE_ATTRIBUTE_RECALL_ON_OPEN |
		                               FILE_ATTRIBUTE_RECALL_ON_DATA_ACCESS | FILE_ATTRIBUTE_VIRTUAL)) != 0;
		result.created = file_time(created);
		result.modified = file_time(modified);
		result.size = (static_cast<uint64_t>(size_high) << 32) | size_low;
	}

	bool can_recurse(const DWORD attributes)
	{
		return (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0 && (attributes & FILE_ATTRIBUTE_REPARSE_POINT) == 0;
	}

	FILETIME to_file_time(const LARGE_INTEGER time)
	{
		return {time.LowPart, static_cast<DWORD>(time.HighPart)};
	}

	// What FIND_FIRST_EX_LARGE_FETCH asks of the file system per read, and the largest read every SMB
	// server accepts.
	constexpr DWORD directory_read_bytes = 64 * 1024;

	// A listed ID as an identity's index. A listing never holds a volume's root, so zero there is unknown.
	std::optional<uint64_t> file_index(const LARGE_INTEGER id)
	{
		return os::identity_index(static_cast<uint64_t>(id.QuadPart), 0, false);
	}

	std::optional<uint64_t> file_index(const FILE_ID_128& id)
	{
		return os::identity_index(low_half(id), high_half(id), false);
	}

	// Reads one information class through to its end. Answers false, having listed nothing, when the
	// first read fails, so the caller can try another.
	template <typename Info>
	bool read_listing(const HANDLE folder, const FILE_INFO_BY_HANDLE_CLASS restart_class,
	                  const FILE_INFO_BY_HANDLE_CLASS next_class, const uint32_t volume, const bool show_hidden,
	                  os::file_enumeration& result)
	{
		// Whole words, so every record's 64-bit fields are aligned as the structures require, and left
		// unfilled, since every read writes what it returns.
		const auto buffer = std::make_unique_for_overwrite<uint64_t[]>(directory_read_bytes / sizeof(uint64_t));
		std::vector<os::file_entry> entries;
		auto info_class = restart_class;

		for (;;)
		{
			if (!GetFileInformationByHandleEx(folder, info_class, buffer.get(), directory_read_bytes))
			{
				const auto error = GetLastError();
				if (error == ERROR_NO_MORE_FILES) break;
				if (info_class == restart_class) return false;

				// The listing ended on a failure rather than at its end - a share that dropped part way -
				// so it must not pass for a complete one.
				result.entries = std::move(entries);
				result.error = error;
				return true;
			}

			info_class = next_class;
			const auto* record = reinterpret_cast<const std::byte*>(buffer.get());

			for (;;)
			{
				const auto& found = *reinterpret_cast<const Info*>(record);
				const std::wstring_view name(found.FileName, found.FileNameLength / sizeof(wchar_t));
				const auto attributes = found.FileAttributes;

				if (name != L"." && name != L".." && (show_hidden || (attributes & FILE_ATTRIBUTE_HIDDEN) == 0))
				{
					os::file_entry entry;
					entry.name = narrow(name);
					fill_attributes(entry.attributes, attributes, to_file_time(found.CreationTime),
					                to_file_time(found.LastWriteTime), static_cast<DWORD>(found.EndOfFile.HighPart),
					                found.EndOfFile.LowPart);

					if (const auto index = file_index(found.FileId))
					{
						entry.attributes.identity.volume = volume;
						entry.attributes.identity.index = *index;
						entry.attributes.identity.valid = true;
					}

					entry.can_recurse = can_recurse(attributes);
					entries.emplace_back(std::move(entry));
				}

				if (found.NextEntryOffset == 0) break;
				record += found.NextEntryOffset;
			}
		}

		result.entries = std::move(entries);
		result.success = true;
		return true;
	}

	// Lists a folder through one open of it, each entry with the identity the file system keeps for it:
	// the same volume serial number and file index query_file answers for a file that is not a link.
	// The extended listing leaves out the 8.3 names NTFS otherwise looks up for every entry; file systems
	// and redirectors without it list the other. Answers false, having listed nothing, when the folder
	// cannot be read either way, for the listing by name to report why or to list it without identities.
	bool list_with_identities(const std::wstring& root, const bool show_hidden, os::file_enumeration& result)
	{
		const file_handle folder(CreateFileW(root.c_str(), FILE_LIST_DIRECTORY,
		                                     FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
		                                     OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr));
		if (!folder.is_valid()) return false;

		DWORD volume = 0;
		if (!GetVolumeInformationByHandleW(folder.get(), nullptr, 0, &volume, nullptr, nullptr, nullptr, 0))
			return false;

		return read_listing<FILE_ID_EXTD_DIR_INFO>(folder.get(), FileIdExtdDirectoryRestartInfo,
		                                           FileIdExtdDirectoryInfo, volume, show_hidden, result) ||
			read_listing<FILE_ID_BOTH_DIR_INFO>(folder.get(), FileIdBothDirectoryRestartInfo, FileIdBothDirectoryInfo,
			                                    volume, show_hidden, result);
	}

	std::wstring dialog_filter(const std::vector<os::file_dialog_filter>& filters)
	{
		std::string text;
		for (const auto& filter : filters)
		{
			text += filter.text;
			text += " (";
			text += filter.extensions;
			text += ")|";
			text += filter.extensions;
			text += "|";
		}
		text += "|";

		auto result = widen(text);
		for (auto& c : result)
		{
			if (c == L'|') c = L'\0';
		}
		return result;
	}

	std::wstring dialog_default_extension(const std::string_view path)
	{
		const auto slash = path.find_last_of("/\\");
		const auto dot = path.find_last_of('.');
		if (dot == std::string_view::npos || (slash != std::string_view::npos && dot < slash))
			return {};
		const auto extension = path.substr(dot + 1);
		return widen(extension);
	}

	class win_file final : public os::file
	{
		HANDLE _handle = INVALID_HANDLE_VALUE;
		std::string _path;
		mutable std::mutex _mutex;

		uint64_t read_locked(uint8_t* data, const uint64_t size) const
		{
			uint64_t total = 0;
			while (total < size)
			{
				const auto request = static_cast<DWORD>(std::min<uint64_t>(size - total, MAXDWORD));
				DWORD count = 0;
				if (!ReadFile(_handle, data + total, request, &count, nullptr) || count == 0)
					break;
				total += count;
			}
			return total;
		}

		uint64_t write_locked(const uint8_t* data, const uint64_t size) const
		{
			uint64_t total = 0;
			while (total < size)
			{
				const auto request = static_cast<DWORD>(std::min<uint64_t>(size - total, MAXDWORD));
				DWORD count = 0;
				if (!WriteFile(_handle, data + total, request, &count, nullptr) || count == 0)
					break;
				total += count;
			}
			return total;
		}

		uint64_t position_locked() const
		{
			LARGE_INTEGER zero = {};
			LARGE_INTEGER position = {};
			return SetFilePointerEx(_handle, zero, &position, FILE_CURRENT)
				       ? static_cast<uint64_t>(position.QuadPart)
				       : UINT64_MAX;
		}

	public:
		win_file(const HANDLE handle, std::string path) : _handle(handle), _path(std::move(path))
		{
		}

		~win_file() override
		{
			if (_handle != INVALID_HANDLE_VALUE) CloseHandle(_handle);
		}

		win_file(const win_file&) = delete;
		win_file& operator=(const win_file&) = delete;

		uint64_t size() const override
		{
			LARGE_INTEGER value = {};
			return GetFileSizeEx(_handle, &value) ? static_cast<uint64_t>(value.QuadPart) : 0;
		}

		uint64_t read(uint8_t* data, const uint64_t size) const override
		{
			std::scoped_lock lock(_mutex);
			return read_locked(data, size);
		}

		uint64_t write(const uint8_t* data, const uint64_t size) override
		{
			std::scoped_lock lock(_mutex);
			return write_locked(data, size);
		}

		uint64_t read_at(const uint64_t offset, uint8_t* data, const uint64_t size) const override
		{
			std::scoped_lock lock(_mutex);
			const auto old = position_locked();
			if (old == UINT64_MAX) return 0;

			LARGE_INTEGER where = {};
			where.QuadPart = static_cast<LONGLONG>(offset);
			if (!SetFilePointerEx(_handle, where, nullptr, FILE_BEGIN)) return 0;
			const auto result = read_locked(data, size);
			where.QuadPart = static_cast<LONGLONG>(old);
			SetFilePointerEx(_handle, where, nullptr, FILE_BEGIN);
			return result;
		}

		uint64_t write_at(const uint64_t offset, const uint8_t* data, const uint64_t size) override
		{
			std::scoped_lock lock(_mutex);
			const auto old = position_locked();
			if (old == UINT64_MAX) return 0;

			LARGE_INTEGER where = {};
			where.QuadPart = static_cast<LONGLONG>(offset);
			if (!SetFilePointerEx(_handle, where, nullptr, FILE_BEGIN)) return 0;
			const auto result = write_locked(data, size);
			where.QuadPart = static_cast<LONGLONG>(old);
			SetFilePointerEx(_handle, where, nullptr, FILE_BEGIN);
			return result;
		}

		bool flush() const override
		{
			std::scoped_lock lock(_mutex);
			return FlushFileBuffers(_handle) != 0;
		}

		uint64_t seek(const uint64_t offset, const os::file_whence whence) const override
		{
			std::scoped_lock lock(_mutex);
			LARGE_INTEGER where = {};
			where.QuadPart = static_cast<LONGLONG>(offset);
			DWORD method = FILE_BEGIN;
			if (whence == os::file_whence::current) method = FILE_CURRENT;
			if (whence == os::file_whence::end) method = FILE_END;

			LARGE_INTEGER result = {};
			return SetFilePointerEx(_handle, where, &result, method)
				       ? static_cast<uint64_t>(result.QuadPart)
				       : UINT64_MAX;
		}

		uint64_t position() const override
		{
			std::scoped_lock lock(_mutex);
			return position_locked();
		}

		bool truncate(const uint64_t size) const override
		{
			std::scoped_lock lock(_mutex);
			LARGE_INTEGER where = {};
			where.QuadPart = static_cast<LONGLONG>(size);
			if (!SetFilePointerEx(_handle, where, nullptr, FILE_BEGIN)) return false;
			return SetEndOfFile(_handle) != 0;
		}

		std::string path() const override
		{
			return _path;
		}

		HANDLE native_handle() const { return _handle; }
	};

	class ini_store_impl final : public os::ini_store
	{
		std::wstring _path;
		bool _root_created = false;
		bool _failed = false;
		mutable std::mutex _mutex;

	public:
		ini_store_impl(std::wstring path, const bool write_unicode_bom) : _path(std::move(path))
		{
			const auto handle = CreateFileW(_path.c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
			                                nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
			if (handle != INVALID_HANDLE_VALUE)
			{
				_root_created = true;
				if (write_unicode_bom)
				{
					constexpr uint8_t bom[] = {0xff, 0xfe};
					DWORD written = 0;
					if (!WriteFile(handle, bom, sizeof(bom), &written, nullptr) || written != sizeof(bom))
						_failed = true;
				}
				CloseHandle(handle);
			}
			else if (GetLastError() != ERROR_FILE_EXISTS && GetLastError() != ERROR_ALREADY_EXISTS)
			{
				_failed = true;
			}
		}

		bool root_created() const override { return _root_created; }

		std::optional<std::string> read(const std::string_view section,
		                               const std::string_view key) const override
		{
			std::scoped_lock lock(_mutex);
			if (_failed) return {};

			const auto section_w = widen(section);
			const auto key_w = widen(key);
			static constexpr wchar_t absent[] = {0x10, 0};
			std::vector<wchar_t> buffer(1024);
			for (;;)
			{
				const auto length = GetPrivateProfileStringW(section_w.c_str(), key_w.c_str(), absent,
				                                             buffer.data(), static_cast<DWORD>(buffer.size()),
				                                             _path.c_str());
				if (length < buffer.size() - 1)
				{
					if (std::wstring_view(buffer.data(), length) == std::wstring_view(absent, 1))
						return {};
					return narrow(std::wstring_view(buffer.data(), length));
				}
				if (buffer.size() >= 1024 * 1024) return {};
				buffer.resize(buffer.size() * 2);
			}
		}

		bool write(const std::string_view section, const std::string_view key,
		           const std::string_view value) override
		{
			std::scoped_lock lock(_mutex);
			if (_failed) return false;
			return WritePrivateProfileStringW(widen(section).c_str(), widen(key).c_str(), widen(value).c_str(),
			                                  _path.c_str()) != 0;
		}

		bool flush() override
		{
			// WritePrivateProfileStringW is write-through for the profile API. An empty write flushes
			// the system's profile cache without changing a key.
			std::scoped_lock lock(_mutex);
			return WritePrivateProfileStringW(nullptr, nullptr, nullptr, _path.c_str()) != 0;
		}
	};

	class registry_store_impl final : public os::settings_store
	{
		std::wstring _root;
		bool _root_created = false;
		bool _failed = false;

		std::wstring key_path(const std::string_view section) const
		{
			auto result = _root;
			if (!section.empty())
			{
				if (!result.empty()) result += L'\\';
				result += widen(section);
			}
			return result;
		}

	public:
		explicit registry_store_impl(const std::string_view root) : _root(widen(root))
		{
			if (_root.empty())
			{
				_failed = true;
				return;
			}

			HKEY key = nullptr;
			DWORD disposition = 0;
			const auto status = RegCreateKeyExW(HKEY_CURRENT_USER, _root.c_str(), 0, nullptr, 0,
			                                    KEY_READ | KEY_WRITE, nullptr, &key, &disposition);
			if (status != ERROR_SUCCESS)
			{
				_failed = true;
				return;
			}
			_root_created = disposition == REG_CREATED_NEW_KEY;
			RegCloseKey(key);
		}

		bool root_created() const override { return _root_created; }

		std::optional<std::string> read(const std::string_view section,
		                               const std::string_view name) const override
		{
			if (_failed) return {};
			HKEY key = nullptr;
			const auto key_name = key_path(section);
			if (RegOpenKeyExW(HKEY_CURRENT_USER, key_name.c_str(), 0, KEY_QUERY_VALUE, &key) != ERROR_SUCCESS)
				return {};

			DWORD type = 0;
			DWORD bytes = 0;
			auto status = RegQueryValueExW(key, widen(name).c_str(), nullptr, &type, nullptr, &bytes);
			if (status != ERROR_SUCCESS || (type != REG_SZ && type != REG_EXPAND_SZ))
			{
				RegCloseKey(key);
				return {};
			}

			std::vector<wchar_t> value(bytes / sizeof(wchar_t) + 1, L'\0');
			status = RegQueryValueExW(key, widen(name).c_str(), nullptr, &type,
			                          reinterpret_cast<LPBYTE>(value.data()), &bytes);
			RegCloseKey(key);
			if (status != ERROR_SUCCESS) return {};
			const auto length = wcsnlen(value.data(), value.size());
			return narrow(std::wstring_view(value.data(), length));
		}

		bool write(const std::string_view section, const std::string_view name,
		           const std::string_view value) override
		{
			if (_failed) return false;
			HKEY key = nullptr;
			DWORD disposition = 0;
			const auto key_name = key_path(section);
			if (RegCreateKeyExW(HKEY_CURRENT_USER, key_name.c_str(), 0, nullptr, 0,
			                    KEY_SET_VALUE, nullptr, &key, &disposition) != ERROR_SUCCESS)
				return false;
			const auto wide = widen(value);
			const auto status = RegSetValueExW(key, widen(name).c_str(), 0, REG_SZ,
			                                   reinterpret_cast<const BYTE*>(wide.c_str()),
			                                   static_cast<DWORD>((wide.size() + 1) * sizeof(wchar_t)));
			RegCloseKey(key);
			return status == ERROR_SUCCESS;
		}

		bool flush() override { return !_failed; }
	};
}

// ── Clipboard text ───────────────────────────────────────────────────────────────────────────

bool os::clipboard_has_text()
{
	return IsClipboardFormatAvailable(CF_UNICODETEXT) != 0;
}

os::clipboard_text os::read_clipboard_text(const uintptr_t owner)
{
	clipboard_text result;
	if (!OpenClipboard(reinterpret_cast<HWND>(owner))) return result;
	result.opened = true;

	if (const auto data = GetClipboardData(CF_UNICODETEXT))
	{
		if (const auto text = static_cast<const wchar_t*>(GlobalLock(data)))
		{
			result.text = narrow(text);
			GlobalUnlock(data);
		}
	}

	CloseClipboard();
	return result;
}

os::clipboard_write os::write_clipboard_text(const uintptr_t owner, const std::string_view text)
{
	const auto w = widen(text);

	if (w.size() > SIZE_MAX / sizeof(wchar_t) - 1) return clipboard_write::too_large;

	auto* const memory = GlobalAlloc(GMEM_MOVEABLE, (w.size() + 1) * sizeof(wchar_t));
	if (!memory) return clipboard_write::alloc_failed;

	auto* const copy = static_cast<wchar_t*>(GlobalLock(memory));
	if (!copy)
	{
		GlobalFree(memory);
		return clipboard_write::lock_failed;
	}

	if (!w.empty()) memcpy(copy, w.data(), w.size() * sizeof(wchar_t));
	copy[w.size()] = 0;
	GlobalUnlock(memory);

	if (!OpenClipboard(reinterpret_cast<HWND>(owner)))
	{
		GlobalFree(memory);
		return clipboard_write::open_failed;
	}

	if (!EmptyClipboard())
	{
		CloseClipboard();
		GlobalFree(memory);
		return clipboard_write::empty_failed;
	}

	// The clipboard owns the memory only once it has accepted it; refused, it is still ours.
	if (!SetClipboardData(CF_UNICODETEXT, memory))
	{
		CloseClipboard();
		GlobalFree(memory);
		return clipboard_write::set_failed;
	}

	CloseClipboard();
	return clipboard_write::ok;
}

// ── Known folders ────────────────────────────────────────────────────────────────────────────

std::string os::known_folder_path(const known_folder folder, const bool create, uint32_t* const error)
{
	const KNOWNFOLDERID* id = nullptr;

	switch (folder)
	{
	case known_folder::local_app_data: id = &FOLDERID_LocalAppData; break;
	case known_folder::roaming_app_data: id = &FOLDERID_RoamingAppData; break;
	case known_folder::profile: id = &FOLDERID_Profile; break;
	case known_folder::desktop: id = &FOLDERID_Desktop; break;
	case known_folder::documents: id = &FOLDERID_Documents; break;
	case known_folder::pictures: id = &FOLDERID_Pictures; break;
	case known_folder::videos: id = &FOLDERID_Videos; break;
	case known_folder::music: id = &FOLDERID_Music; break;
	case known_folder::public_documents: id = &FOLDERID_PublicDocuments; break;
	case known_folder::public_pictures: id = &FOLDERID_PublicPictures; break;
	case known_folder::public_videos: id = &FOLDERID_PublicVideos; break;
	case known_folder::public_music: id = &FOLDERID_PublicMusic; break;
	}

	if (!id)
	{
		if (error) *error = static_cast<uint32_t>(E_INVALIDARG);
		return {};
	}

	PWSTR raw = nullptr;
	const auto status = SHGetKnownFolderPath(*id, create ? KF_FLAG_CREATE : KF_FLAG_DEFAULT, nullptr, &raw);
	if (error) *error = static_cast<uint32_t>(status);

	std::string result;
	if (SUCCEEDED(status) && raw) result = narrow(raw);
	CoTaskMemFree(raw);
	return result;
}

// ── Files and file dialogs ─────────────────────────────────────────────────────────────────

std::optional<uint64_t> os::identity_index(const uint64_t low, const uint64_t high, const bool zero_is_root)
{
	if (high != 0 || low == ~uint64_t{0}) return std::nullopt;
	if (low == 0 && !zero_is_root) return std::nullopt;
	return low;
}

os::file_attributes os::query_file(const std::string_view path)
{
	file_attributes result;
	const auto w = native_path(path);
	WIN32_FILE_ATTRIBUTE_DATA data = {};
	if (!GetFileAttributesExW(w.c_str(), GetFileExInfoStandard, &data))
	{
		const auto error = GetLastError();
		result.error = error;
		result.presence = error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND
			                  ? file_presence::not_found
			                  : file_presence::unknown;
		return result;
	}

	fill_attributes(result, data.dwFileAttributes, data.ftCreationTime, data.ftLastWriteTime,
	                data.nFileSizeHigh, data.nFileSizeLow);

	const auto handle = CreateFileW(w.c_str(), FILE_READ_ATTRIBUTES,
	                                FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
	                                OPEN_EXISTING,
	                                data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY
		                                ? FILE_FLAG_BACKUP_SEMANTICS
		                                : FILE_ATTRIBUTE_NORMAL,
	                                nullptr);
	if (handle != INVALID_HANDLE_VALUE)
	{
		fill_identity(handle, result);
		CloseHandle(handle);
	}
	return result;
}

os::file_enumeration os::enumerate_files(const std::string_view folder, const bool show_hidden)
{
	file_enumeration result;
	auto root = native_path(folder);
	if (root.empty()) root = L".";
	if (!root.ends_with(L'\\') && !root.ends_with(L'/')) root += L'\\';

	if (list_with_identities(root, show_hidden, result)) return result;

	// A file system or redirector that cannot list identities is listed by name, and its entries carry
	// none: opening every entry to ask is what made a walk of a large collection take minutes.
	const auto search = root + L'*';

	WIN32_FIND_DATAW data = {};
	const auto handle = FindFirstFileExW(search.c_str(), FindExInfoBasic, &data, FindExSearchNameMatch,
	                                     nullptr, FIND_FIRST_EX_LARGE_FETCH);
	if (handle == INVALID_HANDLE_VALUE)
	{
		result.error = GetLastError();
		return result;
	}

	bool complete = true;
	do
	{
		if (std::wstring_view(data.cFileName) == L"." || std::wstring_view(data.cFileName) == L"..")
			continue;
		if (!show_hidden && (data.dwFileAttributes & FILE_ATTRIBUTE_HIDDEN) != 0)
			continue;

		file_entry entry;
		entry.name = narrow(data.cFileName);
		fill_attributes(entry.attributes, data.dwFileAttributes, data.ftCreationTime, data.ftLastWriteTime,
		                data.nFileSizeHigh, data.nFileSizeLow);
		entry.can_recurse = can_recurse(data.dwFileAttributes);
		result.entries.emplace_back(std::move(entry));
	}
	while (FindNextFileW(handle, &data) != 0);

	const auto error = GetLastError();
	if (error != ERROR_NO_MORE_FILES)
	{
		complete = false;
		result.error = error;
	}
	FindClose(handle);
	result.success = complete;
	return result;
}

os::file_ptr os::open_file(const std::string_view path, const file_open_mode mode,
                           const file_share_mode share)
{
	DWORD desired_access = GENERIC_READ;
	DWORD share_mode = FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE;
	DWORD disposition = OPEN_EXISTING;
	DWORD flags = FILE_ATTRIBUTE_NORMAL;

	switch (mode)
	{
	case file_open_mode::read:
		break;
	case file_open_mode::write:
		desired_access = GENERIC_WRITE;
		share_mode = FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE;
		break;
	case file_open_mode::create:
		desired_access = GENERIC_READ | GENERIC_WRITE;
		share_mode = FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE;
		disposition = CREATE_ALWAYS;
		break;
	case file_open_mode::create_new:
		desired_access = GENERIC_READ | GENERIC_WRITE;
		share_mode = FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE;
		disposition = CREATE_NEW;
		break;
	case file_open_mode::read_write:
		desired_access = GENERIC_READ | GENERIC_WRITE;
		share_mode = FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE;
		break;
	case file_open_mode::sequential_scan:
		flags |= FILE_FLAG_SEQUENTIAL_SCAN;
		break;
	}
	switch (share)
	{
	case file_share_mode::all: break;
	case file_share_mode::read: share_mode = FILE_SHARE_READ; break;
	case file_share_mode::write: share_mode = FILE_SHARE_WRITE; break;
	case file_share_mode::none: share_mode = 0; break;
	}

	const auto w = native_path(path);
	const auto handle = CreateFileW(w.c_str(), desired_access, share_mode, nullptr, disposition, flags, nullptr);
	if (handle == INVALID_HANDLE_VALUE) return {};
	file_handle owner(handle);
	auto result = std::make_shared<win_file>(owner.get(), std::string(path));
	owner.release();
	return result;
}

uintptr_t os::native_file_handle(const file_ptr& file)
{
	const auto* const native = dynamic_cast<const win_file*>(file.get());
	return native ? reinterpret_cast<uintptr_t>(native->native_handle()) : 0;
}

os::file_operation os::copy_file(const std::string_view source, const std::string_view destination,
                                 const bool fail_if_exists)
{
	return operation_result(::CopyFileW(native_path(source).c_str(), native_path(destination).c_str(),
	                                    fail_if_exists ? TRUE : FALSE));
}

os::file_operation os::move_file(const std::string_view source, const std::string_view destination,
                                 const bool fail_if_exists, const bool allow_copy)
{
	const auto flags = (allow_copy ? MOVEFILE_COPY_ALLOWED : 0u) | MOVEFILE_WRITE_THROUGH |
	                   (fail_if_exists ? 0u : MOVEFILE_REPLACE_EXISTING);
	return operation_result(::MoveFileExW(native_path(source).c_str(), native_path(destination).c_str(), flags));
}

os::file_operation os::create_folder(const std::string_view path)
{
	if (query_file(path).directory) return {file_operation_code::ok, ERROR_SUCCESS, {}};
	return operation_result(::CreateDirectoryW(native_path(path).c_str(), nullptr));
}

os::file_operation os::delete_file(const std::string_view path)
{
	return operation_result(::DeleteFileW(native_path(path).c_str()));
}

os::file_operation os::recycle_file(const std::string_view path)
{
	// SHFileOperationW is a shell API and intentionally receives the unprefixed form. It cannot
	// handle an extended path, but it can still recycle ordinary local and UNC paths. A caller
	// whose path is too long must use permanent deletion instead of claiming an undo was made.
	auto w = widen(path);
	if (w.empty() || w.size() >= MAX_PATH)
	{
		SetLastError(ERROR_FILENAME_EXCED_RANGE);
		return operation_result(FALSE);
	}
	w.push_back(L'\0');
	SHFILEOPSTRUCTW operation = {};
	operation.wFunc = FO_DELETE;
	operation.pFrom = w.c_str();
	operation.fFlags = FOF_ALLOWUNDO | FOF_NOCONFIRMATION | FOF_NOERRORUI | FOF_SILENT;
	const auto result = SHFileOperationW(&operation);
	if (result == 0 && operation.fAnyOperationsAborted == FALSE)
		return {file_operation_code::ok, ERROR_SUCCESS, {}};
	if (result == 0)
		return {file_operation_code::cancelled, ERROR_CANCELLED, "The file operation was cancelled."};
	return {file_operation_code::failed, static_cast<uint32_t>(result), error_text(static_cast<DWORD>(result))};
}

bool os::can_recycle(const std::string_view path)
{
	const auto w = widen(path);
	if (w.size() >= MAX_PATH) return false;
	if (w.starts_with(LR"(\\)")) return false;

	const auto root = w.size() >= 3 && w[1] == L':' ? w.substr(0, 3) : w;
	const auto type = GetDriveTypeW(root.c_str());
	return type != DRIVE_NO_ROOT_DIR && type != DRIVE_UNKNOWN && type != DRIVE_REMOTE &&
	       type != DRIVE_CDROM;
}

os::file_dialog_result os::open_file_dialog(const uintptr_t owner, const std::string_view title,
                                            const std::vector<file_dialog_filter>& filters,
                                            const bool allow_multiple)
{
	file_dialog_result result;
	const auto filter = dialog_filter(filters);
	const auto title_w = widen(title);
	std::vector<wchar_t> buffer(64 * 1024, L'\0');

	OPENFILENAMEW dialog = {};
	dialog.lStructSize = sizeof(dialog);
	dialog.hwndOwner = reinterpret_cast<HWND>(owner);
	dialog.lpstrTitle = title_w.empty() ? nullptr : title_w.c_str();
	dialog.lpstrFilter = filter.c_str();
	dialog.lpstrFile = buffer.data();
	dialog.nMaxFile = static_cast<DWORD>(buffer.size());
	dialog.nFilterIndex = 1;
	dialog.Flags = OFN_EXPLORER | OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR |
	              (allow_multiple ? OFN_ALLOWMULTISELECT : 0);

	if (GetOpenFileNameW(&dialog) == 0)
	{
		const auto error = CommDlgExtendedError();
		result.error = error;
		return result;
	}

	const std::wstring first(buffer.data());
	auto* next = buffer.data() + first.size() + 1;
	if (!allow_multiple || *next == L'\0')
	{
		result.paths.emplace_back(narrow(first));
		return result;
	}

	const auto folder = first;
	while (*next != L'\0')
	{
		const std::wstring name(next);
		result.paths.emplace_back(narrow(folder + L'\\' + name));
		next += name.size() + 1;
	}
	return result;
}

os::file_dialog_result os::save_file_dialog(const uintptr_t owner, const std::string_view title,
                                            const std::string_view default_path,
                                            const std::vector<file_dialog_filter>& filters)
{
	file_dialog_result result;
	const auto filter = dialog_filter(filters);
	std::vector<wchar_t> buffer(64 * 1024, L'\0');
	const auto initial = widen(default_path);
	if (initial.size() < buffer.size())
		std::copy(initial.begin(), initial.end(), buffer.begin());

	const auto title_w = widen(title);
	const auto extension = dialog_default_extension(default_path);
	OPENFILENAMEW dialog = {};
	dialog.lStructSize = sizeof(dialog);
	dialog.hwndOwner = reinterpret_cast<HWND>(owner);
	dialog.lpstrTitle = title_w.empty() ? nullptr : title_w.c_str();
	dialog.lpstrFilter = filter.c_str();
	dialog.lpstrFile = buffer.data();
	dialog.nMaxFile = static_cast<DWORD>(buffer.size());
	dialog.lpstrDefExt = extension.empty() ? nullptr : extension.c_str();
	dialog.nFilterIndex = 1;
	dialog.Flags = OFN_EXPLORER | OFN_OVERWRITEPROMPT | OFN_NOCHANGEDIR;

	if (GetSaveFileNameW(&dialog) == 0)
	{
		result.error = CommDlgExtendedError();
		return result;
	}
	result.paths.emplace_back(narrow(buffer.data()));
	return result;
}

os::file_dialog_result os::folder_dialog(const uintptr_t owner, const std::string_view title,
                                        const std::string_view default_path)
{
	file_dialog_result result;
	IFileOpenDialog* dialog = nullptr;
	if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER,
	                            IID_PPV_ARGS(&dialog))) || dialog == nullptr)
	{
		result.error = static_cast<uint32_t>(GetLastError());
		return result;
	}

	DWORD options = 0;
	const auto title_w = widen(title);
	if (SUCCEEDED(dialog->GetOptions(&options)) &&
		SUCCEEDED(dialog->SetOptions(options | FOS_PICKFOLDERS | FOS_PATHMUSTEXIST | FOS_FORCEFILESYSTEM)) &&
		(title_w.empty() || SUCCEEDED(dialog->SetTitle(title_w.c_str()))))
	{
		if (!default_path.empty())
		{
			IShellItem* folder = nullptr;
			const auto default_w = widen(default_path);
			if (SUCCEEDED(SHCreateItemFromParsingName(default_w.c_str(), nullptr, IID_PPV_ARGS(&folder))) &&
			    folder != nullptr)
			{
				dialog->SetFolder(folder);
				folder->Release();
			}
		}

		const auto shown = dialog->Show(reinterpret_cast<HWND>(owner));
		if (SUCCEEDED(shown))
		{
			IShellItem* item = nullptr;
			if (SUCCEEDED(dialog->GetResult(&item)) && item)
			{
				PWSTR path = nullptr;
				if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path)) && path)
				{
					result.paths.emplace_back(narrow(path));
					CoTaskMemFree(path);
				}
				item->Release();
			}
		}
		else if (shown != HRESULT_FROM_WIN32(ERROR_CANCELLED))
		{
			result.error = static_cast<uint32_t>(shown);
		}
	}
	dialog->Release();
	return result;
}

os::ini_store_ptr os::open_ini_store(const std::string_view path, const bool write_unicode_bom)
{
	return std::make_shared<ini_store_impl>(widen(path), write_unicode_bom);
}

os::settings_store_ptr os::open_registry_store(const std::string_view root_key)
{
	return std::make_shared<registry_store_impl>(root_key);
}

std::string os::escape_ini_value(const std::string_view value)
{
	constexpr char escape = '\x10';
	std::string result;
	result.reserve(value.size());
	for (const auto c : value)
	{
		if (c == '\r' || c == '\n' || c == escape)
		{
			result += escape;
			result += c == '\r' ? 'r' : c == '\n' ? 'n' : escape;
		}
		else
		{
			result += c;
		}
	}
	return result;
}

std::string os::unescape_ini_value(const std::string_view value)
{
	constexpr char escape = '\x10';
	std::string result;
	result.reserve(value.size());
	for (size_t i = 0; i < value.size(); ++i)
	{
		const auto next = i + 1 < value.size() ? value[i + 1] : '\0';
		if (value[i] == escape && (next == 'r' || next == 'n' || next == escape))
		{
			result += next == 'r' ? '\r' : next == 'n' ? '\n' : escape;
			++i;
		}
		else
		{
			result += value[i];
		}
	}
	return result;
}

// ── Web requests ─────────────────────────────────────────────────────────────────────────────

bool os::is_online()
{
	DWORD flags;
	return 0 != InternetGetConnectedState(&flags, 0);
}

std::string os::url_encode(const std::string_view input)
{
	static constexpr auto hex_chars = "0123456789ABCDEF";
	std::string result;
	result.reserve(input.size());

	for (const auto c : input)
	{
		if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
			(c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~')
		{
			result += c;
		}
		else
		{
			const auto byte = static_cast<uint8_t>(c);
			result += '%';
			result += hex_chars[byte >> 4];
			result += hex_chars[byte & 0x0F];
		}
	}

	return result;
}

static bool web_header_token(const std::string_view text)
{
	if (text.empty()) return false;
	for (const unsigned char c : text)
	{
		if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
			std::string_view("!#$%&'*+-.^_`|~").find(c) != std::string_view::npos)) return false;
	}
	return true;
}

static bool web_header_value(const std::string_view text)
{
	for (const unsigned char c : text)
	{
		if ((c < 32 && c != '\t') || c == 127) return false;
	}
	return text.size() < INT_MAX;
}

static bool web_multipart_name(const std::string_view text)
{
	return web_header_value(text) && text.find_first_of("\"\\") == std::string_view::npos;
}

static std::string_view web_trim(std::string_view text)
{
	while (!text.empty() && (text.front() == ' ' || text.front() == '\t')) text.remove_prefix(1);
	while (!text.empty() && (text.back() == ' ' || text.back() == '\t')) text.remove_suffix(1);
	return text;
}

static bool web_length(const std::string_view text, uint64_t& value)
{
	if (text.empty()) return false;
	const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value);
	return parsed.ec == std::errc{} && parsed.ptr == text.data() + text.size();
}

static bool set_web_timeouts(const HINTERNET handle, const uint32_t timeout)
{
	DWORD milliseconds = timeout == 0 ? 30000 : timeout;
	for (const auto option : {INTERNET_OPTION_CONNECT_TIMEOUT, INTERNET_OPTION_SEND_TIMEOUT,
		INTERNET_OPTION_RECEIVE_TIMEOUT})
	{
		if (!InternetSetOptionW(handle, option, &milliseconds, sizeof(milliseconds))) return false;
	}
	return true;
}

static std::string format_path(const os::web_request& req)
{
	auto result = req.path.empty() ? std::string("/") : req.path;

	if (!req.query.empty())
	{
		bool is_first = true;
		result += result.find('?') == std::string::npos ? "?" : "&";

		for (const auto& qp : req.query)
		{
			if (!is_first)
			{
				result += "&";
			}

			result += os::url_encode(qp.first);
			result += "=";
			result += os::url_encode(qp.second);
			is_first = false;
		}
	}

	return result;
}

// RAII wrapper for WinInet handles
class inet_handle
{
	HINTERNET _h;

public:
	explicit inet_handle(const HINTERNET handle = nullptr) : _h(handle)
	{
	}

	~inet_handle()
	{
		if (_h)
		{
			InternetCloseHandle(_h);
		}
	}

	HINTERNET detach()
	{
		const auto handle = _h;
		_h = nullptr;
		return handle;
	}

	inet_handle(const inet_handle&) = delete;
	inet_handle& operator=(const inet_handle&) = delete;

	operator HINTERNET() const { return _h; }
	[[nodiscard]] HINTERNET get() const { return _h; }
	[[nodiscard]] bool is_valid() const { return _h != nullptr; }

	void reset(const HINTERNET handle = nullptr)
	{
		if (_h)
		{
			InternetCloseHandle(_h);
		}
		_h = handle;
	}
};

struct os::web_host
{
	HINTERNET session_handle = nullptr;
	HINTERNET connection_handle = nullptr;
	bool secure = true;
	bool timeouts = true;
	std::string name;
	std::string user_agent;
	int port = 0;

	web_host() = default;
	web_host(const web_host&) = delete;
	web_host& operator=(const web_host&) = delete;

	~web_host()
	{
		if (connection_handle) InternetCloseHandle(connection_handle);
		if (session_handle) InternetCloseHandle(session_handle);
	}
};

struct sync_http_handle
{
	HINTERNET value = nullptr;
	sync_http_handle() = default;
	sync_http_handle(const sync_http_handle&) = delete;
	sync_http_handle& operator=(const sync_http_handle&) = delete;
	~sync_http_handle() { if (value) WinHttpCloseHandle(value); }
};

// The public call blocks, but async handles permit documented, race-free
// cancellation when WinHTTP's native timeout granularity exceeds the deadline.
class bounded_http_request
{
	HANDLE completed = nullptr;
	HANDLE closed = nullptr;
	bool has_callback = false;
	DWORD timeout = 30000;
	std::atomic<DWORD> error = ERROR_SUCCESS;
	std::atomic<DWORD> received = 0;

	static void CALLBACK callback(HINTERNET, const DWORD_PTR context, const DWORD status,
		void* info, const DWORD length)
	{
		const auto self = reinterpret_cast<bounded_http_request*>(context);
		if (!self) return;
		switch (status)
		{
		case WINHTTP_CALLBACK_STATUS_HANDLE_CLOSING:
			SetEvent(self->closed);
			break;
		case WINHTTP_CALLBACK_STATUS_REQUEST_ERROR:
			self->error = static_cast<WINHTTP_ASYNC_RESULT*>(info)->dwError;
			SetEvent(self->completed);
			break;
		case WINHTTP_CALLBACK_STATUS_READ_COMPLETE:
			self->received = length;
			SetEvent(self->completed);
			break;
		case WINHTTP_CALLBACK_STATUS_SENDREQUEST_COMPLETE:
		case WINHTTP_CALLBACK_STATUS_HEADERS_AVAILABLE:
			SetEvent(self->completed);
			break;
		default:
			break;
		}
	}

public:
	HINTERNET value = nullptr;
	bounded_http_request() = default;
	bounded_http_request(const bounded_http_request&) = delete;
	bounded_http_request& operator=(const bounded_http_request&) = delete;

	~bounded_http_request()
	{
		close();
		if (completed) CloseHandle(completed);
		if (closed) CloseHandle(closed);
	}

	void close()
	{
		if (!value) return;
		const auto handle = value;
		value = nullptr;
		WinHttpCloseHandle(handle);
		if (has_callback) WaitForSingleObject(closed, INFINITE);
	}

	bool initialize(const uint32_t timeout_ms)
	{
		completed = CreateEventW(nullptr, TRUE, FALSE, nullptr);
		closed = CreateEventW(nullptr, TRUE, FALSE, nullptr);
		if (!completed || !closed) return false;
		timeout = std::min<uint32_t>(timeout_ms ? timeout_ms : 30000, INFINITE - 1);
		DWORD_PTR context = reinterpret_cast<DWORD_PTR>(this);
		if (!WinHttpSetOption(value, WINHTTP_OPTION_CONTEXT_VALUE, &context, sizeof(context))) return false;
		if (WinHttpSetStatusCallback(value, callback,
			WINHTTP_CALLBACK_FLAG_ALL_COMPLETIONS | WINHTTP_CALLBACK_FLAG_HANDLES, 0) == WINHTTP_INVALID_STATUS_CALLBACK)
			return false;
		has_callback = true;
		return true;
	}

	template <typename F>
	bool invoke(F issue)
	{
		error = ERROR_SUCCESS;
		received = 0;
		ResetEvent(completed);
		if (!issue()) return false;
		if (WaitForSingleObject(completed, timeout) != WAIT_OBJECT_0)
		{
			// Complete cancellation before the caller releases its read/write buffers.
			close();
			SetLastError(ERROR_WINHTTP_TIMEOUT);
			return false;
		}
		if (error != ERROR_SUCCESS)
		{
			SetLastError(error);
			return false;
		}
		return true;
	}

	bool read(void* buffer, const DWORD size, DWORD& count)
	{
		if (!invoke([&] { return WinHttpReadData(value, buffer, size, nullptr); })) return false;
		count = received;
		return true;
	}
};

os::web_host_ptr os::connect_to_host(const std::string_view host, const bool secure_in, const int port_in,
                                     const std::string_view user_agent, const bool timeouts)
{
	if (host.empty() || host.size() >= INT_MAX || port_in < 0 || port_in > 65535 ||
		!web_header_value(user_agent)) return nullptr;
	for (const unsigned char c : host)
	{
		if (c <= 32 || c == 127 || std::string_view("/\\?#@").find(c) != std::string_view::npos) return nullptr;
	}

	inet_handle session_handle(InternetOpenW(widen(user_agent).c_str(), INTERNET_OPEN_TYPE_PRECONFIG, nullptr,
	                                         nullptr, 0));

	if (!session_handle.is_valid())
	{
		return nullptr;
	}

	if (timeouts && !set_web_timeouts(session_handle, 0)) return nullptr;

	const auto port = port_in == 0
		                  ? (secure_in ? INTERNET_DEFAULT_HTTPS_PORT : INTERNET_DEFAULT_HTTP_PORT)
		                  : port_in;
	inet_handle conn(InternetConnectW(session_handle, widen(host).c_str(), static_cast<INTERNET_PORT>(port), nullptr,
	                                  nullptr, INTERNET_SERVICE_HTTP, 0, 0));

	if (!conn.is_valid())
	{
		return nullptr;
	}

	// Built in place: web_host's destructor closes its handles, so a copied temporary would leave
	// the live instance holding handles that were already closed.
	auto result = std::make_shared<web_host>();
	result->session_handle = session_handle.detach();
	result->connection_handle = conn.detach();
	result->secure = secure_in;
	result->timeouts = timeouts;
	result->name = host;
	result->port = port_in;
	result->user_agent = user_agent;
	return result;
}

os::web_response os::send_request(const web_host_ptr& host, const web_request& req)
{
	web_response result;
	const auto fail = [&result](const web_response_error error)
	{
		result.error = error;
		result.body.clear();
		return result;
	};

	if (!host || (req.verb != web_request_verb::GET && req.verb != web_request_verb::POST) ||
		req.body.size() > MAXDWORD || req.path.size() >= INT_MAX ||
		(!req.path.empty() && (req.path.front() != '/' || req.path.starts_with("//"))))
		return fail(web_response_error::invalid_request);
	for (const unsigned char c : req.path)
	{
		if (c <= 32 || c == 127 || c == '\\' || c == '#') return fail(web_response_error::invalid_request);
	}

	std::string content;
	std::string header_str;

	for (const auto& h : req.headers)
	{
		if (!web_header_token(h.first) || !web_header_value(h.second) ||
			h.first.size() >= INT_MAX || header_str.size() + h.first.size() + h.second.size() + 4 >= INT_MAX ||
			iequals(h.first, "Transfer-Encoding") || (!req.use_cookies && iequals(h.first, "Cookie")))
			return fail(web_response_error::invalid_request);
		header_str += h.first;
		header_str += ": ";
		header_str += h.second;
		header_str += "\r\n";
	}

	if (!req.body.empty())
	{
		content = req.body;
	}
	else if (!req.form_data.empty())
	{
		const std::string boundary = "54B8723DE6044695A68C838E8BF0CB00";

		for (const auto& f : req.form_data)
		{
			if (!web_multipart_name(f.first) ||
				content.size() + f.first.size() + f.second.size() + 256 > MAXDWORD)
				return fail(web_response_error::invalid_request);
			content += "--";
			content += boundary;
			content += "\r\n";
			content += "Content-Disposition: form-data; name=\"";
			content += f.first;
			content += "\"\r\n";
			content += "Content-Type: text/plain; charset=\"utf-8\"\r\n";
			content += "\r\n";
			content += f.second;
			content += "\r\n";
		}

		if (!req.upload_file_path.empty() && !req.file_form_data_name.empty())
		{
			if (!web_multipart_name(req.file_form_data_name) || !web_multipart_name(req.file_name) ||
				content.size() + req.file_form_data_name.size() + req.file_name.size() + 256 > MAXDWORD)
				return fail(web_response_error::invalid_request);
			const auto content_type = has_extension(req.upload_file_path, ".zip")
				                          ? "application/x-zip-compressed"
				                          : "application/octet-stream";

			content += "--";
			content += boundary;
			content += "\r\n";
			content += "Content-Disposition: form-data; name=\"";
			content += req.file_form_data_name;
			content += "\"; filename=\"";
			content += req.file_name;
			content += "\"\r\n";
			content += "Content-Type: ";
			content += content_type;
			content += "\r\n\r\n";

			const file_handle upload(CreateFileW(widen(req.upload_file_path).c_str(), GENERIC_READ, FILE_SHARE_READ,
			                                     nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
			if (!upload.is_valid()) return fail(web_response_error::transport);
			DWORD high = 0;
			GetFileSize(upload.get(), &high);
			if (high != 0) return fail(web_response_error::transport); // over 4GB

			std::vector<uint8_t> buf(65536);
			for (;;)
			{
				DWORD bytes_read = 0;
				if (!ReadFile(upload.get(), buf.data(), static_cast<DWORD>(buf.size()), &bytes_read, nullptr))
					return fail(web_response_error::transport);
				if (bytes_read == 0) break;
				if (content.size() > MAXDWORD - 256 || bytes_read > MAXDWORD - content.size() - 256)
					return fail(web_response_error::invalid_request);
				content.append(reinterpret_cast<const char*>(buf.data()), bytes_read);
			}

			content += "\r\n";
		}

		content += "--";
		content += boundary;
		content += "--";
		header_str += "Content-Type: multipart/form-data; boundary=";
		header_str += boundary;
		header_str += "\r\n";
	}

	for (const auto& h : req.headers)
	{
		if (iequals(h.first, "Content-Length"))
		{
			uint64_t length = 0;
			if (!web_length(web_trim(h.second), length) || length != content.size())
				return fail(web_response_error::invalid_request);
		}
	}
	size_t path_bound = req.path.size();
	for (const auto& [key, value] : req.query)
	{
		if (path_bound > INT_MAX - 2 || key.size() > (INT_MAX - path_bound - 2) / 3 ||
			value.size() > (INT_MAX - path_bound - 2) / 3 - key.size())
			return fail(web_response_error::invalid_request);
		path_bound += 3 * (key.size() + value.size()) + 2;
	}
	const auto path = format_path(req);
	if (path.size() >= INT_MAX || header_str.size() >= INT_MAX)
		return fail(web_response_error::invalid_request);
	const auto wverb = req.verb == web_request_verb::GET ? L"GET" : L"POST";
	const auto wpath = widen(path.empty() ? "/" : path);
	auto flags = INTERNET_FLAG_KEEP_CONNECTION | INTERNET_FLAG_NO_CACHE_WRITE | INTERNET_FLAG_NO_AUTH |
		INTERNET_FLAG_RELOAD;
	if (host->secure) flags |= INTERNET_FLAG_SECURE;
	if (!req.follow_redirects) flags |= INTERNET_FLAG_NO_AUTO_REDIRECT;
	if (!req.use_cookies) flags |= INTERNET_FLAG_NO_COOKIES;

	// WinINet can report success on incomplete chunks and round short timeouts.
	// Bounded requests consume an isolated WinHTTP session synchronously instead.
	const bool bounded = req.max_response_bytes || req.max_header_bytes || req.timeout_ms;
	sync_http_handle bounded_session;
	sync_http_handle bounded_connection;
	bounded_http_request bounded_request;
	inet_handle request_handle;
	const auto headerW = widen(header_str);
	const auto transport_failure = [&]
	{
		const auto native_error = GetLastError();
		if (bounded && bounded_request.value && result.status_code == 0)
		{
			DWORD status = 0;
			DWORD size = sizeof(status);
			if (WinHttpQueryHeaders(bounded_request.value, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
				WINHTTP_HEADER_NAME_BY_INDEX, &status, &size, WINHTTP_NO_HEADER_INDEX))
				result.status_code = static_cast<int>(status);
		}
		return fail(bounded && native_error == ERROR_WINHTTP_HEADER_SIZE_OVERFLOW
			? web_response_error::response_limit : web_response_error::transport);
	};
	if (bounded)
	{
		bounded_session.value = WinHttpOpen(widen(host->user_agent).c_str(), WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
			WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, WINHTTP_FLAG_ASYNC);
		if (!bounded_session.value) return transport_failure();
		const auto timeout = static_cast<int>(std::min<uint32_t>(req.timeout_ms ? req.timeout_ms : 30000, INT_MAX));
		if (!WinHttpSetTimeouts(bounded_session.value, timeout, timeout, timeout, timeout))
			return transport_failure();
		const auto port = host->port ? host->port : (host->secure ? INTERNET_DEFAULT_HTTPS_PORT : INTERNET_DEFAULT_HTTP_PORT);
		bounded_connection.value = WinHttpConnect(bounded_session.value, widen(host->name).c_str(),
			static_cast<INTERNET_PORT>(port), 0);
		if (!bounded_connection.value) return transport_failure();
		bounded_request.value = WinHttpOpenRequest(bounded_connection.value, wverb, wpath.c_str(), nullptr,
			WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, host->secure ? WINHTTP_FLAG_SECURE : 0);
		if (!bounded_request.value) return transport_failure();
		if (!bounded_request.initialize(req.timeout_ms)) return transport_failure();
		DWORD receive_timeout = static_cast<DWORD>(timeout);
		if (!WinHttpSetTimeouts(bounded_request.value, timeout, timeout, timeout, timeout) ||
			!WinHttpSetOption(bounded_request.value, WINHTTP_OPTION_RECEIVE_RESPONSE_TIMEOUT,
				&receive_timeout, sizeof(receive_timeout))) return transport_failure();
		DWORD disabled = WINHTTP_DISABLE_AUTHENTICATION;
		if (!req.follow_redirects) disabled |= WINHTTP_DISABLE_REDIRECTS;
		if (!req.use_cookies) disabled |= WINHTTP_DISABLE_COOKIES;
		if (!WinHttpSetOption(bounded_request.value, WINHTTP_OPTION_DISABLE_FEATURE, &disabled, sizeof(disabled)))
			return transport_failure();
		if (req.max_header_bytes)
		{
			DWORD maximum = static_cast<DWORD>(std::min<size_t>(req.max_header_bytes, MAXDWORD));
			if (!WinHttpSetOption(bounded_request.value, WINHTTP_OPTION_MAX_RESPONSE_HEADER_SIZE,
				&maximum, sizeof(maximum))) return transport_failure();
		}
		if (!bounded_request.invoke([&] {
			return WinHttpSendRequest(bounded_request.value, headerW.c_str(), static_cast<DWORD>(headerW.size()),
				content.empty() ? nullptr : content.data(), static_cast<DWORD>(content.size()),
				static_cast<DWORD>(content.size()), reinterpret_cast<DWORD_PTR>(&bounded_request));
		}) || !bounded_request.invoke([&] { return WinHttpReceiveResponse(bounded_request.value, nullptr); }))
			return transport_failure();
	}
	else
	{
		request_handle.reset(HttpOpenRequestW(host->connection_handle, wverb, wpath.c_str(), nullptr, nullptr,
		                                      nullptr, flags, 0));
		if (!request_handle.is_valid()) return transport_failure();
		if (host->timeouts && !set_web_timeouts(request_handle, 0)) return transport_failure();

		if (content.empty())
		{
			if (!HttpSendRequestW(request_handle, headerW.c_str(), static_cast<DWORD>(headerW.size()), nullptr, 0))
				return fail(web_response_error::transport);
		}
		else
		{
			INTERNET_BUFFERSW buffers = {};
			buffers.dwStructSize = sizeof(buffers);
			buffers.lpcszHeader = headerW.c_str();
			buffers.dwHeadersTotal = buffers.dwHeadersLength = static_cast<DWORD>(headerW.size());
			buffers.dwBufferTotal = static_cast<DWORD>(content.size());

			if (!HttpSendRequestExW(request_handle, &buffers, nullptr, 0, 0))
				return fail(web_response_error::transport);

			constexpr size_t chunk_size = 8192;
			size_t total_written = 0;

			while (total_written < content.size())
			{
				const auto remaining = content.size() - total_written;
				const auto to_write = std::min(chunk_size, remaining);
				DWORD written = 0;
				if (!InternetWriteFile(request_handle, content.data() + total_written, static_cast<DWORD>(to_write),
					&written) || written == 0) return fail(web_response_error::transport);
				total_written += written;
			}
			if (!HttpEndRequestW(request_handle, nullptr, 0, 0)) return fail(web_response_error::transport);
		}
	}

	DWORD status = 0;
	DWORD size = sizeof(status);
	if (bounded ? !WinHttpQueryHeaders(bounded_request.value, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
		WINHTTP_HEADER_NAME_BY_INDEX, &status, &size, WINHTTP_NO_HEADER_INDEX)
		: !HttpQueryInfoW(request_handle, HTTP_QUERY_STATUS_CODE | HTTP_QUERY_FLAG_NUMBER,
		&status, &size, nullptr)) return transport_failure();
	result.status_code = static_cast<int>(status);

	size = 0;
	if (bounded)
	{
		if (WinHttpQueryHeaders(bounded_request.value, WINHTTP_QUERY_RAW_HEADERS_CRLF,
			WINHTTP_HEADER_NAME_BY_INDEX, nullptr, &size, WINHTTP_NO_HEADER_INDEX) ||
			GetLastError() != ERROR_INSUFFICIENT_BUFFER || size < sizeof(wchar_t))
			return transport_failure();
		const auto chars = size / sizeof(wchar_t);
		if (req.max_header_bytes && chars - 1 > req.max_header_bytes)
			return fail(web_response_error::response_limit);
		std::wstring headers(chars, L'\0');
		if (!WinHttpQueryHeaders(bounded_request.value, WINHTTP_QUERY_RAW_HEADERS_CRLF,
			WINHTTP_HEADER_NAME_BY_INDEX, headers.data(), &size, WINHTTP_NO_HEADER_INDEX))
			return transport_failure();
		headers.resize(size / sizeof(wchar_t));
		// HTTP header octets are widened one-to-one by WinHTTP, not UTF-8 text.
		result.headers.reserve(headers.size());
		for (const auto c : headers) result.headers.push_back(static_cast<char>(c));
	}
	else
	{
		if (HttpQueryInfoA(request_handle, HTTP_QUERY_RAW_HEADERS_CRLF, nullptr, &size, nullptr) ||
			GetLastError() != ERROR_INSUFFICIENT_BUFFER || size == 0)
			return fail(web_response_error::transport);
		result.headers.resize(size);
		if (!HttpQueryInfoA(request_handle, HTTP_QUERY_RAW_HEADERS_CRLF, result.headers.data(), &size, nullptr))
		{
			result.headers.clear();
			return fail(web_response_error::transport);
		}
		result.headers.resize(size);
	}
	if (req.max_header_bytes && result.headers.size() > req.max_header_bytes)
	{
		result.headers.clear();
		return fail(web_response_error::response_limit);
	}

	std::optional<uint64_t> content_length;
	bool transfer_encoded = false;
	std::string_view remaining_headers = result.headers;
	while (!remaining_headers.empty())
	{
		const auto end = remaining_headers.find("\r\n");
		const auto line = remaining_headers.substr(0, end);
		if (const auto colon = line.find(':'); colon != std::string_view::npos)
		{
			const auto name = line.substr(0, colon);
			const auto value = web_trim(line.substr(colon + 1));
			if (iequals(name, "Content-Length"))
			{
				uint64_t length = 0;
				if (!web_length(value, length) || (content_length && *content_length != length))
					return fail(web_response_error::transport);
				content_length = length;
			}
			else if (iequals(name, "Content-Type"))
				result.content_type = value;
			else if (iequals(name, "Transfer-Encoding"))
				transfer_encoded = true;
		}
		if (end == std::string_view::npos) break;
		remaining_headers.remove_prefix(end + 2);
	}
	if (transfer_encoded && content_length) return fail(web_response_error::transport);
	const bool no_body = status == 204 || status == 304 || (status >= 100 && status < 200);
	if (!no_body && req.max_response_bytes && content_length && *content_length > req.max_response_bytes)
		return fail(web_response_error::response_limit);

	std::unique_ptr<file_handle> download;
	if (!req.download_file_path.empty())
	{
		download = std::make_unique<file_handle>(CreateFileW(widen(req.download_file_path).c_str(), GENERIC_WRITE, 0,
		                                                     nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr));
		if (!download->is_valid()) return fail(web_response_error::transport);
		result.download_created = true;
	}
	uint64_t total = 0;
	for (;;)
	{
		uint8_t buffer[8192];
		DWORD read = 0;
		if (bounded ? !bounded_request.read(buffer, sizeof(buffer), read)
			: !InternetReadFile(request_handle, buffer, sizeof(buffer), &read))
			return transport_failure();
		if (read == 0) break;
		if (req.max_response_bytes && (total > req.max_response_bytes || read > req.max_response_bytes - total))
			return fail(web_response_error::response_limit);
		total += read;
		if (download)
		{
			DWORD written = 0;
			if (!WriteFile(download->get(), buffer, read, &written, nullptr) || written != read)
				return fail(web_response_error::transport);
		}
		else
			result.body.append(reinterpret_cast<const char*>(buffer), read);
	}
	if (!no_body && content_length && total != *content_length) return fail(web_response_error::transport);
	return result;
}
