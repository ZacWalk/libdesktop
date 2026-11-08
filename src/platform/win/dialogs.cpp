// This file is part of libdesktop, a C++20 platform layer for desktop applications
// Copyright 2026  Zac Walker
//
// This program is free software; you can redistribute it and / or modify it
// under the terms of the LGPL License either version 2.1 or later.
// License details are available at https://www.gnu.org/licenses/lgpl-2.1.html
// This program is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY

// Purpose: Windows native message boxes, choice and modal dialogs, and file pickers.

#include "libdesktop/platform.h"
#include "libdesktop/platform/win32.h"
#include "os.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <commctrl.h>
#include <shobjidl.h>

#include <algorithm>
#include <limits>
#include <unordered_map>

#pragma comment(lib, "comctl32.lib")

namespace
{
	// Without an owner a dialog belongs to the application's window, as the owner-less pickers do, so it
	// stays modal to the application rather than floating free of it.
	HWND native_owner(platform::window* owner)
	{
		if (!owner) return app_wnd();
		const auto handle = owner->native_handle();
		if (const auto value = std::any_cast<HWND>(&handle)) return *value;
		return app_wnd();
	}

	UINT message_box_flags(const platform::message_box_style style)
	{
		UINT result = 0;
		switch (style.buttons)
		{
		case platform::message_box_buttons::ok: result |= MB_OK; break;
		case platform::message_box_buttons::ok_cancel: result |= MB_OKCANCEL; break;
		case platform::message_box_buttons::yes_no: result |= MB_YESNO; break;
		case platform::message_box_buttons::yes_no_cancel: result |= MB_YESNOCANCEL; break;
		case platform::message_box_buttons::retry_cancel: result |= MB_RETRYCANCEL; break;
		}
		switch (style.icon)
		{
		case platform::message_box_icon::none: break;
		case platform::message_box_icon::info: result |= MB_ICONINFORMATION; break;
		case platform::message_box_icon::warning: result |= MB_ICONWARNING; break;
		case platform::message_box_icon::error: result |= MB_ICONERROR; break;
		case platform::message_box_icon::question: result |= MB_ICONQUESTION; break;
		}
		return result;
	}

	platform::message_box_result map_message_box_result(const int result)
	{
		switch (result)
		{
		case IDOK: return platform::message_box_result::ok;
		case IDCANCEL: return platform::message_box_result::cancel;
		case IDYES: return platform::message_box_result::yes;
		case IDNO: return platform::message_box_result::no;
		case IDRETRY: return platform::message_box_result::retry;
		default: return platform::message_box_result::none;
		}
	}

	std::vector<os::file_dialog_filter> convert_filters(const std::vector<platform::file_dialog_filter>& filters)
	{
		std::vector<os::file_dialog_filter> result;
		result.reserve(filters.size());
		for (const auto& filter : filters) result.push_back({filter.text, filter.extensions});
		return result;
	}

	std::vector<os::file_dialog_filter> image_save_filters()
	{
		return {
			{std::string(platform::text().jpeg_best), "*.jpg;*.jpe;*.jpeg"},
			{std::string(platform::text().png_best), "*.png"},
			{std::string(platform::text().webp_best), "*.webp"},
			{std::string(platform::text().jxl_best), "*.jxl"}
		};
	}

	class modal_dialog_host
	{
		platform::modal_definition _definition;
		HWND _owner = nullptr;
		HWND _window = nullptr;
		std::map<uint32_t, HWND> _controls;
		std::unordered_map<int, size_t> _command_fields;
		std::vector<HBITMAP> _preview_bitmaps;
		std::optional<std::map<uint32_t, platform::modal_value>> _result;

		static ATOM dialog_class()
		{
			static const ATOM atom = []
			{
				WNDCLASSW klass = {};
				klass.lpfnWndProc = &window_proc;
				klass.hInstance = GetModuleHandleW(nullptr);
				klass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
				klass.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_BTNFACE + 1);
				klass.lpszClassName = L"LibdesktopModalDialog";
				return RegisterClassW(&klass);
			}();
			return atom;
		}

		static LRESULT CALLBACK window_proc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam)
		{
			auto* self = reinterpret_cast<modal_dialog_host*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
			if (message == WM_NCCREATE)
			{
				const auto* create = reinterpret_cast<const CREATESTRUCTW*>(lparam);
				self = create ? static_cast<modal_dialog_host*>(create->lpCreateParams) : nullptr;
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
				const auto found = self->_command_fields.find(id);
				if (found != self->_command_fields.end())
				{
					auto& field = self->_definition.fields[found->second];
					if (field.action) field.action();
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
			const auto label = core::utf8_to_utf16(field.label);
			if (field.kind != platform::modal_field_kind::check_box && field.kind != platform::modal_field_kind::action)
				CreateWindowExW(0, L"STATIC", label.c_str(), WS_CHILD | WS_VISIBLE,
				                12, y + 4, 150, 22, _window, nullptr, GetModuleHandleW(nullptr), nullptr);

			const auto id = static_cast<int>(1000 + index);
			HWND control = nullptr;
			switch (field.kind)
			{
			case platform::modal_field_kind::text:
				{
					const auto* initial = std::get_if<std::string>(&field.value);
					const auto initial_text = core::utf8_to_utf16(initial ? *initial : std::string{});
					control = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", initial_text.c_str(),
					                         WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL,
					                         170, y, 300, 24, _window,
					                         reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
					                         GetModuleHandleW(nullptr), nullptr);
					break;
				}
			case platform::modal_field_kind::check_box:
				control = CreateWindowExW(0, L"BUTTON", label.c_str(),
				                          WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX,
				                          12, y, 458, 24, _window,
				                          reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
				                          GetModuleHandleW(nullptr), nullptr);
				if (control && std::get_if<bool>(&field.value) && std::get<bool>(field.value))
					SendMessageW(control, BM_SETCHECK, BST_CHECKED, 0);
				break;
			case platform::modal_field_kind::choice:
				control = CreateWindowExW(0, L"COMBOBOX", nullptr,
				                          WS_CHILD | WS_VISIBLE | WS_TABSTOP | CBS_DROPDOWNLIST,
				                          170, y, 300, 180, _window,
				                          reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
				                          GetModuleHandleW(nullptr), nullptr);
				for (const auto& choice : field.choices)
				{
					const auto text = core::utf8_to_utf16(choice.first);
					SendMessageW(control, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(text.c_str()));
				}
				break;
			case platform::modal_field_kind::search_list:
				control = CreateWindowExW(WS_EX_CLIENTEDGE, L"LISTBOX", nullptr,
				                          WS_CHILD | WS_VISIBLE | WS_TABSTOP | LBS_NOTIFY,
				                          170, y, 300, 80, _window,
				                          reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
				                          GetModuleHandleW(nullptr), nullptr);
				for (const auto& choice : field.choices)
				{
					const auto text = core::utf8_to_utf16(choice.first);
					SendMessageW(control, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(text.c_str()));
				}
				break;
			case platform::modal_field_kind::action:
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

		HBITMAP make_preview(const platform::modal_preview& preview)
		{
			if (preview.width <= 0 || preview.height <= 0 ||
				static_cast<size_t>(preview.width) > std::numeric_limits<size_t>::max() / static_cast<size_t>(preview.height) ||
				preview.bgra_pixels.size() < static_cast<size_t>(preview.width) * static_cast<size_t>(preview.height))
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
			if (dc) ReleaseDC(_window, dc);
			if (!bitmap || !bits)
			{
				if (bitmap) DeleteObject(bitmap);
				return nullptr;
			}
			std::memcpy(bits, preview.bgra_pixels.data(),
			            static_cast<size_t>(preview.width) * static_cast<size_t>(preview.height) * sizeof(uint32_t));
			return bitmap;
		}

		void accept()
		{
			std::map<uint32_t, platform::modal_value> values;
			for (size_t i = 0; i != _definition.fields.size(); ++i)
			{
				auto& field = _definition.fields[i];
				const auto found = _controls.find(field.id);
				if (found == _controls.end()) continue;
				const auto control = found->second;
				if (field.kind == platform::modal_field_kind::text)
				{
					const auto length = GetWindowTextLengthW(control);
					std::wstring value(static_cast<size_t>(length) + 1, L'\0');
					GetWindowTextW(control, value.data(), length + 1);
					value.resize(static_cast<size_t>(length));
					values[field.id] = core::utf16_to_utf8(value);
				}
				else if (field.kind == platform::modal_field_kind::check_box)
					values[field.id] = SendMessageW(control, BM_GETCHECK, 0, 0) == BST_CHECKED;
				else if (field.kind == platform::modal_field_kind::choice)
				{
					const auto selected = static_cast<int>(SendMessageW(control, CB_GETCURSEL, 0, 0));
					values[field.id] = selected >= 0 && selected < static_cast<int>(field.choices.size())
						                   ? field.choices[selected].second : int64_t{0};
				}
				else if (field.kind == platform::modal_field_kind::search_list)
				{
					const auto selected = static_cast<int>(SendMessageW(control, LB_GETCURSEL, 0, 0));
					values[field.id] = selected >= 0 && selected < static_cast<int>(field.choices.size())
						                   ? field.choices[selected].second : int64_t{0};
				}
			}
			if (_definition.validate)
			{
				if (const auto error = _definition.validate(values))
				{
					const auto message = core::utf8_to_utf16(*error);
					const auto title = core::utf8_to_utf16(_definition.title);
					MessageBoxW(_window, message.c_str(), title.c_str(), MB_OK | MB_ICONWARNING);
					return;
				}
			}
			_result = std::move(values);
			DestroyWindow(_window);
		}

	public:
		modal_dialog_host(HWND owner, platform::modal_definition definition)
			: _definition(std::move(definition)), _owner(owner)
		{
			dialog_class();
			const auto title = core::utf8_to_utf16(_definition.title);
			const auto fields_height = std::min<size_t>(_definition.fields.size(), 40) * 38;
			const auto previews_height = std::min<size_t>(_definition.previews.size(), 20) * 104;
			const auto height = static_cast<int>(std::min<size_t>(2000, 120 + fields_height + previews_height));
			_window = CreateWindowExW(WS_EX_DLGMODALFRAME, L"LibdesktopModalDialog", title.c_str(),
			                          WS_POPUP | WS_CAPTION | WS_SYSMENU | WS_VISIBLE,
			                          CW_USEDEFAULT, CW_USEDEFAULT, 500, height,
			                          owner, nullptr, GetModuleHandleW(nullptr), this);
			if (!_window) return;
			const auto message = core::utf8_to_utf16(_definition.message);
			CreateWindowExW(0, L"STATIC", message.c_str(), WS_CHILD | WS_VISIBLE,
			                12, 10, 458, 32, _window, nullptr, GetModuleHandleW(nullptr), nullptr);
			int y = 48;
			for (const auto& preview : _definition.previews)
			{
				const auto bitmap = make_preview(preview);
				if (bitmap)
				{
					const auto width = std::min(458, preview.width);
					const auto preview_height = std::min(96, preview.height);
					const auto control = CreateWindowExW(WS_EX_CLIENTEDGE, L"STATIC", nullptr,
					                                    WS_CHILD | WS_VISIBLE | SS_BITMAP | SS_CENTERIMAGE,
					                                    12, y, width, preview_height, _window, nullptr,
					                                    GetModuleHandleW(nullptr), nullptr);
					SendMessageW(control, STM_SETIMAGE, IMAGE_BITMAP, reinterpret_cast<LPARAM>(bitmap));
					_preview_bitmaps.push_back(bitmap);
				}
				y += 104;
			}
			for (size_t i = 0; i != _definition.fields.size() && i < 40; ++i, y += 38) add_control(i, y);
			const auto accept = core::utf8_to_utf16(_definition.accept);
			const auto cancel = core::utf8_to_utf16(_definition.cancel);
			CreateWindowExW(0, L"BUTTON", accept.c_str(), WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON,
			                300, height - 58, 80, 28, _window, reinterpret_cast<HMENU>(1),
			                GetModuleHandleW(nullptr), nullptr);
			CreateWindowExW(0, L"BUTTON", cancel.c_str(), WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
			                390, height - 58, 80, 28, _window, reinterpret_cast<HMENU>(2),
			                GetModuleHandleW(nullptr), nullptr);
		}

		~modal_dialog_host()
		{
			if (_window) DestroyWindow(_window);
			for (const auto bitmap : _preview_bitmaps) if (bitmap) DeleteObject(bitmap);
		}

		std::optional<std::map<uint32_t, platform::modal_value>> run()
		{
			if (!_window) return std::nullopt;
			const auto was_enabled = _owner && IsWindowEnabled(_owner);
			if (_owner) EnableWindow(_owner, FALSE);
			SetForegroundWindow(_window);
			platform::run_modal_loop([this] { return _window == nullptr; });
			if (_window) DestroyWindow(_window);
			if (_owner)
			{
				EnableWindow(_owner, was_enabled ? TRUE : FALSE);
				SetActiveWindow(_owner);
			}
			return _result;
		}
	};
}

platform::message_box_result platform::message_box(platform::window* owner, const std::string_view text,
                                                    const std::string_view title, const message_box_style style)
{
	const auto wide_text = core::utf8_to_utf16(text);
	const auto wide_title = core::utf8_to_utf16(title);
	return map_message_box_result(::MessageBoxW(native_owner(owner), wide_text.c_str(), wide_title.c_str(),
	                                       message_box_flags(style)));
}

int platform::show_choice(platform::window* owner, const choice_definition& definition)
{
	if (definition.buttons.empty()) return 0;
	std::vector<std::wstring> labels;
	std::vector<TASKDIALOG_BUTTON> buttons;
	labels.reserve(definition.buttons.size());
	buttons.reserve(definition.buttons.size());
	for (const auto& [id, label] : definition.buttons)
	{
		labels.push_back(core::utf8_to_utf16(label));
		buttons.push_back({id, labels.back().c_str()});
	}
	const auto title = core::utf8_to_utf16(definition.title);
	const auto heading = core::utf8_to_utf16(definition.heading);
	const auto message = core::utf8_to_utf16(definition.message);
	TASKDIALOGCONFIG config = {};
	config.cbSize = sizeof(config);
	config.hwndParent = native_owner(owner);
	config.pszWindowTitle = title.c_str();
	config.pszMainInstruction = heading.c_str();
	config.pszContent = message.c_str();
	config.pButtons = buttons.data();
	config.cButtons = static_cast<UINT>(buttons.size());
	config.nDefaultButton = definition.default_id;
	config.dwFlags = TDF_ALLOW_DIALOG_CANCELLATION;
	config.pszMainIcon = definition.warning ? TD_WARNING_ICON : TD_INFORMATION_ICON;
	// TaskDialogIndirect exists only in Common Controls 6, which an executable gets from its manifest.
	// Resolving it here rather than importing it keeps an executable without that manifest loadable;
	// it then answers 0, as a cancelled choice does.
	using task_dialog_indirect_fn = HRESULT(WINAPI*)(const TASKDIALOGCONFIG*, int*, int*, BOOL*);
	static const auto task_dialog_indirect = []() -> task_dialog_indirect_fn
	{
		const auto module = LoadLibraryW(L"comctl32.dll");
		return module ? reinterpret_cast<task_dialog_indirect_fn>(GetProcAddress(module, "TaskDialogIndirect"))
		              : nullptr;
	}();
	if (!task_dialog_indirect) return 0;
	int selected = 0;
	return SUCCEEDED(task_dialog_indirect(&config, &selected, nullptr, nullptr)) ? selected : 0;
}

std::optional<std::map<uint32_t, platform::modal_value>> platform::show_modal_dialog(
	platform::window* owner, platform::modal_definition definition)
{
	return modal_dialog_host(native_owner(owner), std::move(definition)).run();
}

bool platform::browse_for_folder(platform::window* owner, core::folder_path& path, const std::string_view title)
{
	const auto result = os::folder_dialog(reinterpret_cast<uintptr_t>(native_owner(owner)),
	                                      title.empty() ? static_cast<std::string_view>(platform::text().select_folder)
	                                                    : title,
	                                      path.text());
	if (!result.accepted()) return false;
	path = core::folder_path(result.paths.front());
	return true;
}

bool platform::prompt_for_save_path(platform::window* owner, core::file_path& path)
{
	const auto result = os::save_file_dialog(reinterpret_cast<uintptr_t>(native_owner(owner)), {}, path.pack(),
	                                         image_save_filters());
	if (!result.accepted()) return false;
	path = core::file_path(result.paths.front());
	return true;
}

bool platform::prompt_for_open_paths(platform::window* owner, std::vector<core::file_path>& paths,
                                     const std::vector<file_dialog_filter>& filters, const bool allow_multiple,
                                     const std::string_view title)
{
	const auto result = os::open_file_dialog(reinterpret_cast<uintptr_t>(native_owner(owner)), title,
	                                         convert_filters(filters), allow_multiple);
	if (!result.accepted()) return false;
	paths.clear();
	for (const auto& selected : result.paths) paths.emplace_back(selected);
	return !paths.empty();
}

bool platform::prompt_for_save_path(platform::window* owner, core::file_path& path,
                                    const std::vector<file_dialog_filter>& filters, const std::string_view title)
{
	const auto result = os::save_file_dialog(reinterpret_cast<uintptr_t>(native_owner(owner)), title, path.pack(),
	                                         convert_filters(filters));
	if (!result.accepted()) return false;
	path = core::file_path(result.paths.front());
	return true;
}

