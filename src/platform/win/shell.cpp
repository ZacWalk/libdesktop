// This file is part of libdesktop, a C++20 platform layer for desktop applications
// Copyright 2026  Zac Walker
//
// This program is free software; you can redistribute it and / or modify it
// under the terms of the LGPL License either version 2.1 or later.
// License details are available at https://www.gnu.org/licenses/lgpl-2.1.html
// This program is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY

// Purpose: Windows shell integration, instance coordination, spelling, and file drag sources.

#include "libdesktop/platform.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <ole2.h>
#include <oleidl.h>
#include <shellapi.h>
#include <shlobj.h>
#include <sddl.h>
#include <spellcheck.h>

#include <algorithm>
#include <atomic>
#include <cstring>
#include <fstream>
#include <format>
#include <limits>
#include <mutex>
#include <unordered_map>

#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "oleaut32.lib")
#pragma comment(lib, "shell32.lib")

namespace
{
	std::string windows_error(const std::string_view operation, const DWORD error = GetLastError())
	{
		if (error == ERROR_SUCCESS) return std::format("{} failed", operation);

		LPWSTR raw = nullptr;
		const auto length = FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
		                                   FORMAT_MESSAGE_IGNORE_INSERTS, nullptr, error,
		                                   MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
		                                   reinterpret_cast<LPWSTR>(&raw), 0, nullptr);
		if (length == 0 || !raw)
			return std::format("{} failed (error {})", operation, error);

		auto text = core::utf16_to_utf8(std::wstring_view(raw, length));
		LocalFree(raw);
		while (!text.empty() && (text.back() == '\r' || text.back() == '\n' || text.back() == ' ')) text.pop_back();
		return std::format("{} failed: {}", operation, text);
	}

	bool valid_instance_name(const std::string_view name, const size_t max_length)
	{
		if (name.empty() || name.size() > max_length) return false;
		return std::all_of(name.begin(), name.end(), [](const char c)
		{
			return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
			       (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-';
		});
	}

	struct win_instance_lock final : platform::instance_lock
	{
		HANDLE handle = nullptr;

		~win_instance_lock() override
		{
			if (handle) CloseHandle(handle);
		}
	};

	platform::instance_lock_result lock_error(const std::string_view operation, const DWORD error)
	{
		return {nullptr, false, windows_error(operation, error)};
	}

	std::atomic_bool tray_publication_failure = false;
	std::mutex instance_signal_mutex;
	std::unordered_map<UINT, std::function<void()>> instance_signals;
	HWND instance_signal_window_handle = nullptr;

	UINT instance_signal_message(const std::string_view name)
	{
		if (!valid_instance_name(name, 128)) return 0;
		const auto wide_name = core::utf8_to_utf16(std::string("libdesktop.instance.") + std::string(name));
		return RegisterWindowMessageW(wide_name.c_str());
	}

	LRESULT CALLBACK instance_signal_window_proc(HWND hwnd, UINT message, WPARAM, LPARAM)
	{
		if (message == WM_NCDESTROY)
		{
			if (instance_signal_window_handle == hwnd) instance_signal_window_handle = nullptr;
		}

		std::function<void()> callback;
		{
			std::lock_guard lock(instance_signal_mutex);
			const auto found = instance_signals.find(message);
			if (found != instance_signals.end()) callback = found->second;
		}
		if (callback)
		{
			// Broadcast messages are received by the hidden window's thread; run_ui is the
			// boundary that guarantees the application's UI thread owns the callback.
			platform::run_ui(std::move(callback));
			return 0;
		}
		return DefWindowProcW(hwnd, message, 0, 0);
	}

	bool ensure_instance_signal_window()
	{
		if (instance_signal_window_handle && IsWindow(instance_signal_window_handle)) return true;

		const auto instance = GetModuleHandleW(nullptr);
		constexpr wchar_t class_name[] = L"LibdesktopInstanceSignalWindow";
		WNDCLASSW klass = {};
		klass.lpfnWndProc = &instance_signal_window_proc;
		klass.hInstance = instance;
		klass.lpszClassName = class_name;
		if (!RegisterClassW(&klass) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) return false;

		// HWND_BROADCAST reaches invisible top-level windows but not HWND_MESSAGE windows.
		instance_signal_window_handle = CreateWindowExW(WS_EX_TOOLWINDOW, class_name, L"", WS_POPUP,
		                                                0, 0, 0, 0, nullptr, nullptr, instance, nullptr);
		return instance_signal_window_handle != nullptr;
	}

	class win_tray_service final : public platform::tray_service
	{
		static constexpr UINT callback_message = WM_APP + 0x34;

		HWND _window = nullptr;
		HICON _icon = nullptr;
		UINT _id = 0;
		UINT _taskbar_created = 0;
		bool _visible = false;
		std::wstring _tooltip;
		platform::tray_callbacks _callbacks;
		std::string _diagnostics;

		static LRESULT CALLBACK window_proc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam)
		{
			auto* self = reinterpret_cast<win_tray_service*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
			if (message == WM_NCCREATE)
			{
				const auto* create = reinterpret_cast<const CREATESTRUCTW*>(lparam);
				self = create ? static_cast<win_tray_service*>(create->lpCreateParams) : nullptr;
				SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
			}
			if (!self) return DefWindowProcW(hwnd, message, wparam, lparam);

			if (message == self->_taskbar_created)
			{
				self->republish();
				return 0;
			}
			if (message == WM_ENDSESSION)
			{
				if (self->_callbacks.on_end_session) self->_callbacks.on_end_session(wparam != 0);
				return 0;
			}
			if (message != callback_message) return DefWindowProcW(hwnd, message, wparam, lparam);

			switch (static_cast<UINT>(lparam))
			{
			case WM_LBUTTONUP:
			case WM_LBUTTONDBLCLK:
			case NIN_SELECT:
				if (self->_callbacks.on_activate) self->_callbacks.on_activate();
				break;
			case WM_CONTEXTMENU:
			case WM_RBUTTONUP:
			case NIN_KEYSELECT:
				self->context_menu(static_cast<UINT>(lparam) == NIN_KEYSELECT);
				break;
			case NIN_BALLOONUSERCLICK:
				if (self->_callbacks.on_balloon_click) self->_callbacks.on_balloon_click();
				break;
			default:
				break;
			}
			return 0;
		}

		static ATOM register_window_class()
		{
			static const ATOM atom = []
			{
				WNDCLASSW klass = {};
				klass.lpfnWndProc = &window_proc;
				klass.hInstance = GetModuleHandleW(nullptr);
				klass.lpszClassName = L"LibdesktopTrayWindow";
				const auto atom = RegisterClassW(&klass);
				return atom != 0 || GetLastError() == ERROR_CLASS_ALREADY_EXISTS ? (atom ? atom : 1) : 0;
			}();
			return atom;
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
					GetCursorPos(&point);
				else
				{
					point.x = rect.left;
					point.y = rect.bottom;
				}
			}
			else
				GetCursorPos(&point);

			if (_callbacks.on_context_menu) _callbacks.on_context_menu({point.x, point.y}, keyboard_anchor);
		}

		HICON make_icon(const platform::tray_icon_image image)
		{
			if (image.extent.cx <= 0 || image.extent.cy <= 0 || image.extent.cx > 256 || image.extent.cy > 256 ||
				image.bgra_pixels.size() < static_cast<size_t>(image.extent.cx) * static_cast<size_t>(image.extent.cy))
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
			if (dc) ReleaseDC(nullptr, dc);
			if (!color || !bits)
			{
				if (color) DeleteObject(color);
				_diagnostics = "Unable to allocate the tray icon bitmap.";
				return nullptr;
			}
			std::memcpy(bits, image.bgra_pixels.data(),
			            static_cast<size_t>(image.extent.cx) * static_cast<size_t>(image.extent.cy) * sizeof(uint32_t));

			const auto mask_bytes = (image.extent.cx + 7) / 8 * image.extent.cy;
			std::vector<uint8_t> mask(static_cast<size_t>(mask_bytes), 0);
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

		std::string last_shell_error() const
		{
			return windows_error("Shell_NotifyIcon", GetLastError());
		}

		bool publish()
		{
			if (tray_publication_failure.load(std::memory_order_relaxed))
			{
				_diagnostics = "Tray publication was rejected by the test seam.";
				return false;
			}
			auto data = notification();
			if (!Shell_NotifyIconW(NIM_ADD, &data))
			{
				_diagnostics = last_shell_error();
				return false;
			}
			data.uVersion = NOTIFYICON_VERSION_4;
			if (!Shell_NotifyIconW(NIM_SETVERSION, &data))
			{
				_diagnostics = last_shell_error();
				Shell_NotifyIconW(NIM_DELETE, &data);
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
		win_tray_service(const platform::tray_icon_image image, const std::string_view tooltip,
		                 platform::tray_callbacks callbacks)
			: _id([] { static std::atomic_uint32_t next = 1; return next.fetch_add(1); }()),
			  _taskbar_created(RegisterWindowMessageW(L"TaskbarCreated")), _callbacks(std::move(callbacks))
		{
			if (!register_window_class())
			{
				_diagnostics = windows_error("RegisterClass(LibdesktopTrayWindow)");
				return;
			}
			_window = CreateWindowExW(0, L"LibdesktopTrayWindow", L"", 0, 0, 0, 0, 0,
			                          HWND_MESSAGE, nullptr, GetModuleHandleW(nullptr), this);
			if (!_window)
			{
				_diagnostics = windows_error("CreateWindow(LibdesktopTrayWindow)");
				return;
			}
			set_tooltip(tooltip);
			set_icon(image);
		}

		~win_tray_service() override
		{
			if (_window && _visible)
			{
				auto data = notification();
				Shell_NotifyIconW(NIM_DELETE, &data);
			}
			if (_icon) DestroyIcon(_icon);
			if (_window) DestroyWindow(_window);
		}

		bool available() const override { return _window != nullptr && _icon != nullptr; }
		std::string diagnostics() const override { return _diagnostics; }

		bool set_icon(const platform::tray_icon_image image) override
		{
			const auto icon = make_icon(image);
			if (!icon) return false;
			if (_icon) DestroyIcon(_icon);
			_icon = icon;
			if (!_visible) return true;
			auto data = notification();
			if (Shell_NotifyIconW(NIM_MODIFY, &data)) return true;
			_diagnostics = last_shell_error();
			return false;
		}

		bool set_tooltip(const std::string_view text) override
		{
			_tooltip = core::utf8_to_utf16(text);
			constexpr size_t max_tooltip_chars = 127;
			if (_tooltip.size() > max_tooltip_chars) _tooltip.resize(max_tooltip_chars);
			if (!_visible) return true;
			auto data = notification();
			if (Shell_NotifyIconW(NIM_MODIFY, &data)) return true;
			_diagnostics = last_shell_error();
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
				_diagnostics = last_shell_error();
				return false;
			}
			_visible = false;
			return true;
		}

		bool show_balloon(const std::string_view title, const std::string_view text,
		                 const platform::tray_balloon_kind kind) override
		{
			if (!_visible) return false;
			auto data = notification();
			data.uFlags = NIF_INFO;
			const auto wide_title = core::utf8_to_utf16(title);
			const auto wide_text = core::utf8_to_utf16(text);
			wcsncpy_s(data.szInfoTitle, wide_title.c_str(), _TRUNCATE);
			wcsncpy_s(data.szInfo, wide_text.c_str(), _TRUNCATE);
			data.dwInfoFlags = kind == platform::tray_balloon_kind::warning
				                   ? NIIF_WARNING
				                   : kind == platform::tray_balloon_kind::error ? NIIF_ERROR : NIIF_INFO;
			if (Shell_NotifyIconW(NIM_MODIFY, &data)) return true;
			_diagnostics = last_shell_error();
			return false;
		}
	};

	class file_drag_format_enum final : public IEnumFORMATETC
	{
		ULONG _refs = 1;
		std::vector<FORMATETC> _formats;
		ULONG _index = 0;

	public:
		explicit file_drag_format_enum(std::vector<FORMATETC> formats) : _formats(std::move(formats)) {}
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
			while (copied < count && _index < _formats.size()) result[copied++] = _formats[_index++];
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
			auto copy = std::make_unique<file_drag_format_enum>(_formats);
			copy->_index = _index;
			*result = copy.release();
			return S_OK;
		}
	};

	class file_drag_data_object final : public IDataObject
	{
		ULONG _refs = 1;
		std::vector<std::wstring> _paths;
		CLIPFORMAT _preferred_effect = 0;

		HGLOBAL make_hdrop() const
		{
			size_t chars = 1;
			for (const auto& path : _paths)
			{
				if (path.size() > (std::numeric_limits<size_t>::max() - chars - 1)) return nullptr;
				chars += path.size() + 1;
			}
			if (chars > (std::numeric_limits<size_t>::max() - sizeof(DROPFILES)) / sizeof(wchar_t)) return nullptr;
			const auto memory = GlobalAlloc(GMEM_MOVEABLE | GMEM_ZEROINIT,
			                                sizeof(DROPFILES) + chars * sizeof(wchar_t));
			if (!memory) return nullptr;
			auto* drop = static_cast<DROPFILES*>(GlobalLock(memory));
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

		FORMATETC drop_format() const { return {CF_HDROP, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL}; }
		FORMATETC effect_format() const
		{
			return {_preferred_effect, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL};
		}

	public:
		explicit file_drag_data_object(const std::span<const core::file_path> files)
			: _preferred_effect(static_cast<CLIPFORMAT>(RegisterClipboardFormatW(CFSTR_PREFERREDDROPEFFECT)))
		{
			_paths.reserve(files.size());
			for (const auto& path : files) _paths.push_back(platform::to_file_system_path(path));
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
			if (format->tymed != TYMED_HGLOBAL || format->dwAspect != DVASPECT_CONTENT) return DV_E_FORMATETC;
			ZeroMemory(medium, sizeof(*medium));
			medium->tymed = TYMED_HGLOBAL;
			if (format->cfFormat == CF_HDROP)
				medium->hGlobal = make_hdrop();
			else if (format->cfFormat == _preferred_effect && _preferred_effect != 0)
			{
				medium->hGlobal = GlobalAlloc(GMEM_MOVEABLE, sizeof(DWORD));
				if (medium->hGlobal)
				{
					auto* effect = static_cast<DWORD*>(GlobalLock(medium->hGlobal));
					if (!effect)
					{
						GlobalFree(medium->hGlobal);
						medium->hGlobal = nullptr;
					}
					else
					{
						*effect = DROPEFFECT_COPY | DROPEFFECT_MOVE;
						GlobalUnlock(medium->hGlobal);
					}
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
			if (!format || format->tymed != TYMED_HGLOBAL || format->dwAspect != DVASPECT_CONTENT) return DV_E_FORMATETC;
			return format->cfFormat == CF_HDROP || (format->cfFormat == _preferred_effect && _preferred_effect != 0)
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
			std::vector<FORMATETC> formats{drop_format()};
			if (_preferred_effect) formats.push_back(effect_format());
			*result = new file_drag_format_enum(std::move(formats));
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

	class win_spell_checker final : public platform::spell_checker
	{
		ISpellChecker* _checker = nullptr;
		std::string _diagnostics;
		core::file_path _custom_dictionary;

		void load_dictionary()
		{
			if (!_checker || _custom_dictionary.is_empty()) return;
			std::ifstream input(platform::to_stream_path(_custom_dictionary));
			std::string line;
			while (std::getline(input, line))
			{
				if (!line.empty())
				{
					const auto word = core::utf8_to_utf16(line);
					_checker->Add(word.c_str());
				}
			}
		}

		void initialize(const std::string_view requested_language)
		{
			ISpellCheckerFactory* factory = nullptr;
			const auto factory_hr = CoCreateInstance(__uuidof(SpellCheckerFactory), nullptr, CLSCTX_INPROC_SERVER,
			                                        IID_PPV_ARGS(&factory));
			if (FAILED(factory_hr) || !factory)
			{
				_diagnostics = std::format("CoCreateInstance(SpellCheckerFactory) failed with HRESULT 0x{:08X}.",
				                           static_cast<uint32_t>(factory_hr));
				return;
			}

			std::wstring language = core::utf8_to_utf16(requested_language);
			if (language.empty())
			{
				wchar_t buffer[LOCALE_NAME_MAX_LENGTH] = {};
				if (GetUserDefaultLocaleName(buffer, std::size(buffer))) language = buffer;
				else language = L"en-US";
			}

			BOOL supported = FALSE;
			const auto language_hr = factory->IsSupported(language.c_str(), &supported);
			if (SUCCEEDED(language_hr) && supported)
			{
				const auto create_hr = factory->CreateSpellChecker(language.c_str(), &_checker);
				if (FAILED(create_hr) || !_checker)
					_diagnostics = std::format("CreateSpellChecker({}) failed with HRESULT 0x{:08X}.",
					                           core::utf16_to_utf8(language), static_cast<uint32_t>(create_hr));
			}
			else if (core::icmp(core::utf16_to_utf8(language), "en-US") != 0)
			{
				BOOL fallback_supported = FALSE;
				const auto fallback_check_hr = factory->IsSupported(L"en-US", &fallback_supported);
				if (SUCCEEDED(fallback_check_hr) && fallback_supported)
				{
					const auto create_hr = factory->CreateSpellChecker(L"en-US", &_checker);
					if (FAILED(create_hr) || !_checker)
						_diagnostics = std::format("CreateSpellChecker(en-US) failed with HRESULT 0x{:08X}.",
						                           static_cast<uint32_t>(create_hr));
				}
				else if (FAILED(language_hr))
					_diagnostics = std::format(
						"IsSupported({}) failed with HRESULT 0x{:08X}; en-US fallback check returned HRESULT 0x{:08X}.",
						core::utf16_to_utf8(language), static_cast<uint32_t>(language_hr),
						static_cast<uint32_t>(fallback_check_hr));
				else
					_diagnostics = std::format("No Windows spell-check dictionary is available for {} or en-US.",
					                           core::utf16_to_utf8(language));
			}
			else if (FAILED(language_hr))
				_diagnostics = std::format("IsSupported({}) failed with HRESULT 0x{:08X}.",
				                           core::utf16_to_utf8(language), static_cast<uint32_t>(language_hr));
			else
				_diagnostics = std::format("No Windows spell-check dictionary is available for {}.",
				                           core::utf16_to_utf8(language));

			factory->Release();
			if (!_diagnostics.empty() && _checker == nullptr) return;
			const auto dictionary_folder = platform::known_path(platform::known_folder::app_data);
			if (!dictionary_folder.is_empty()) _custom_dictionary = dictionary_folder.combine_file("spell.dic");
			load_dictionary();
			if (_diagnostics.empty()) _diagnostics = std::format("Spell checker created for {}.", core::utf16_to_utf8(language));
		}

	public:
		explicit win_spell_checker(const std::string_view language) { initialize(language); }
		~win_spell_checker() override
		{
			if (_checker) _checker->Release();
		}
		bool available() const override { return _checker != nullptr; }
		std::string diagnostics() const override { return _diagnostics; }
		bool check_word(const std::string_view word) override
		{
			if (!_checker) return true;
			const auto wide = core::utf8_to_utf16(word);
			IEnumSpellingError* errors = nullptr;
			const auto hr = _checker->Check(wide.c_str(), &errors);
			if (FAILED(hr) || !errors) return true;
			ISpellingError* error = nullptr;
			const bool valid = errors->Next(&error) != S_OK;
			if (error) error->Release();
			errors->Release();
			return valid;
		}
		std::vector<std::string> suggest(const std::string_view word) override
		{
			std::vector<std::string> result;
			if (!_checker) return result;
			const auto wide = core::utf8_to_utf16(word);
			IEnumString* suggestions = nullptr;
			if (FAILED(_checker->Suggest(wide.c_str(), &suggestions)) || !suggestions) return result;
			LPOLESTR suggestion = nullptr;
			while (suggestions->Next(1, &suggestion, nullptr) == S_OK)
			{
				if (suggestion)
				{
					result.emplace_back(core::utf16_to_utf8(suggestion));
					CoTaskMemFree(suggestion);
					suggestion = nullptr;
				}
			}
			suggestions->Release();
			return result;
		}
		void add_word(const std::string_view word) override
		{
			if (!_checker) return;
			const auto wide = core::utf8_to_utf16(word);
			_checker->Add(wide.c_str());
			if (!_custom_dictionary.is_empty())
			{
				std::ofstream output(platform::to_stream_path(_custom_dictionary), std::ios::out | std::ios::app);
				if (output) output << word << '\n';
			}
		}
	};
}

platform::tray_service_ptr platform::create_tray_service(const platform::tray_icon_image image,
                                                          const std::string_view tooltip,
                                                          platform::tray_callbacks callbacks)
{
	auto result = std::make_shared<win_tray_service>(image, tooltip, std::move(callbacks));
	result->show();
	return result;
}

void platform::test_set_tray_publication_failure(const bool fail)
{
	tray_publication_failure.store(fail, std::memory_order_relaxed);
}

platform::instance_lock_result platform::try_lock_instance(const std::string_view name)
{
	if (!valid_instance_name(name, 128)) return lock_error("Instance lock name validation", ERROR_INVALID_NAME);

	HANDLE token_handle = nullptr;
	if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token_handle))
		return lock_error("OpenProcessToken", GetLastError());
	const core::scope_exit close_token([token_handle] { CloseHandle(token_handle); });

	DWORD bytes = 0;
	if (!GetTokenInformation(token_handle, TokenUser, nullptr, 0, &bytes) && GetLastError() != ERROR_INSUFFICIENT_BUFFER)
		return lock_error("GetTokenInformation(size)", GetLastError());
	if (bytes < sizeof(TOKEN_USER)) return lock_error("GetTokenInformation(size)", ERROR_INVALID_DATA);
	std::vector<uint8_t> user(bytes);
	if (!GetTokenInformation(token_handle, TokenUser, user.data(), bytes, &bytes))
		return lock_error("GetTokenInformation", GetLastError());

	PWSTR raw_sid = nullptr;
	if (!ConvertSidToStringSidW(reinterpret_cast<const TOKEN_USER*>(user.data())->User.Sid, &raw_sid))
		return lock_error("ConvertSidToStringSid", GetLastError());
	const core::scope_exit free_sid([raw_sid] { LocalFree(raw_sid); });
	// The prefix the applications' earlier releases locked, so a new release still finds an old one running.
	const auto qualified_name = std::wstring(L"Global\\pf.instance.") + raw_sid + L"." + core::utf8_to_utf16(name);

	auto lock = std::make_unique<win_instance_lock>();
	SetLastError(ERROR_SUCCESS);
	lock->handle = CreateMutexW(nullptr, FALSE, qualified_name.c_str());
	const auto error = GetLastError();
	if (!lock->handle) return lock_error("CreateMutex", error);
	if (error == ERROR_ALREADY_EXISTS) return {nullptr, true, {}};
	return {std::move(lock), false, {}};
}

platform::instance_lock_result platform::try_lock_named_instance(const std::string_view name)
{
	if (name.empty() || name.size() > 256 || name.find('\0') != std::string_view::npos)
		return lock_error("Named instance lock validation", ERROR_INVALID_NAME);

	auto lock = std::make_unique<win_instance_lock>();
	SetLastError(ERROR_SUCCESS);
	lock->handle = CreateMutexW(nullptr, FALSE, core::utf8_to_utf16(name).c_str());
	const auto error = GetLastError();
	if (!lock->handle) return lock_error("CreateMutex", error);
	if (error == ERROR_ALREADY_EXISTS) return {nullptr, true, {}};
	return {std::move(lock), false, {}};
}

bool platform::register_instance_signal(const std::string_view name, std::function<void()> callback)
{
	const auto message = instance_signal_message(name);
	if (!message || !callback || !ensure_instance_signal_window()) return false;
	std::lock_guard lock(instance_signal_mutex);
	instance_signals[message] = std::move(callback);
	return true;
}

bool platform::signal_existing_instance(const std::string_view name)
{
	const auto message = instance_signal_message(name);
	return message != 0 && PostMessageW(HWND_BROADCAST, message, 0, 0) != FALSE;
}

bool platform::begin_file_drag(platform::window*, const std::span<const core::file_path> files)
{
	if (files.empty()) return false;
	const auto ole = OleInitialize(nullptr);
	if (FAILED(ole)) return false;
	const core::scope_exit uninitialize([] { OleUninitialize(); });

	auto* data = new file_drag_data_object(files);
	auto* source = new file_drag_source();
	DWORD effect = DROPEFFECT_NONE;
	const auto result = DoDragDrop(data, source, DROPEFFECT_COPY | DROPEFFECT_MOVE, &effect);
	data->Release();
	source->Release();
	return result == DRAGDROP_S_DROP && effect != DROPEFFECT_NONE;
}

std::shared_ptr<platform::spell_checker> platform::create_spell_checker(const std::string_view language)
{
	auto result = std::make_shared<win_spell_checker>(language);
	if (!result->available()) return {};
	return result;
}
