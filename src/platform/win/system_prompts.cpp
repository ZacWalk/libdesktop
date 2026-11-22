// This file is part of libdesktop, a C++20 platform layer for desktop applications
// Copyright 2026  Zac Walker
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

// Purpose: The system's own prompts over the application's window - a message box, or a shell confirmation or error -
// read as a screen reader reads them and answered by their buttons, for an agent session nobody else answers for.

#include "libdesktop/platform.h"
#include "libdesktop/platform/win32.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <oleacc.h>
#include <wrl/client.h>

#include <algorithm>

#pragma comment(lib, "oleacc.lib")

using Microsoft::WRL::ComPtr;

namespace
{
	// A dialog window of this process, shown. Every system prompt is of the dialog class - message boxes, task dialogs
	// and the shell's confirmations alike - and none of the application's own windows is.
	bool is_system_prompt(const HWND hwnd)
	{
		DWORD process = 0;
		GetWindowThreadProcessId(hwnd, &process);
		if (process != GetCurrentProcessId() || !IsWindowVisible(hwnd)) return false;
		wchar_t name[8] = {};
		return GetClassNameW(hwnd, name, 8) == 6 && wcscmp(name, L"#32770") == 0;
	}

	// Oldest first: the last is the one in front, as the application lists its own dialogs.
	std::vector<HWND> prompt_windows()
	{
		std::vector<HWND> result;
		EnumWindows([](const HWND hwnd, const LPARAM found) -> BOOL
		{
			if (is_system_prompt(hwnd)) reinterpret_cast<std::vector<HWND>*>(found)->push_back(hwnd);
			return TRUE;
		}, reinterpret_cast<LPARAM>(&result));
		std::ranges::reverse(result);
		return result;
	}

	// A control's text as it reads: its mnemonic marker taken out, and the space around it.
	std::string plain_text(const std::string_view text)
	{
		std::string result;
		for (size_t i = 0; i < text.size(); ++i)
		{
			if (text[i] != '&') result += text[i];
			else if (i + 1 < text.size() && text[i + 1] == '&') result += text[++i];
		}
		return std::string(core::trim(result));
	}

	std::string name_of(IAccessible* const element, const VARIANT& child)
	{
		BSTR name = nullptr;
		if (FAILED(element->get_accName(child, &name)) || !name) return {};
		auto result = plain_text(core::utf16_to_utf8(std::wstring_view(name, SysStringLen(name))));
		SysFreeString(name);
		return result;
	}

	long number_of(IAccessible* const element, const VARIANT& child, const bool role)
	{
		VARIANT value;
		VariantInit(&value);
		const auto hr = role ? element->get_accRole(child, &value) : element->get_accState(child, &value);
		const auto result = SUCCEEDED(hr) && value.vt == VT_I4 ? value.lVal : 0;
		VariantClear(&value);
		return result;
	}

	// What a prompt says and the buttons it offers, read in the order a screen reader reads them - and the button
	// pressed, where one was asked for by its text.
	struct prompt_reading
	{
		std::vector<std::string> texts;
		std::vector<std::string> buttons;
		std::string_view press;
		bool pressed = false;
		int visited = 0;
	};

	void add_once(std::vector<std::string>& list, std::string text)
	{
		if (!text.empty() && std::ranges::find(list, text) == list.end()) list.push_back(std::move(text));
	}

	void read_element(IAccessible* const element, const VARIANT& child, prompt_reading& reading)
	{
		const auto state = number_of(element, child, false);
		if ((state & STATE_SYSTEM_INVISIBLE) != 0) return;

		switch (number_of(element, child, true))
		{
		case ROLE_SYSTEM_PUSHBUTTON:
			if ((state & STATE_SYSTEM_UNAVAILABLE) == 0)
			{
				auto name = name_of(element, child);
				if (!reading.pressed && !reading.press.empty() && core::icmp(name, reading.press) == 0)
					reading.pressed = SUCCEEDED(element->accDoDefaultAction(child));
				add_once(reading.buttons, std::move(name));
			}
			break;
		case ROLE_SYSTEM_STATICTEXT:
		case ROLE_SYSTEM_TEXT: add_once(reading.texts, name_of(element, child)); break;
		default: break;
		}
	}

	void read_children(IAccessible* const element, prompt_reading& reading, const int depth)
	{
		// A prompt is a handful of controls; the bounds keep a window that is not one from costing much.
		constexpr int deepest = 12;
		constexpr int most = 400;
		long count = 0;
		if (depth > deepest || reading.visited > most || reading.pressed ||
		    FAILED(element->get_accChildCount(&count)) || count <= 0)
			return;

		std::vector<VARIANT> children(static_cast<size_t>(count));
		long obtained = 0;
		if (FAILED(AccessibleChildren(element, 0, count, children.data(), &obtained))) return;

		for (long i = 0; i < obtained; ++i)
		{
			const auto& child = children[static_cast<size_t>(i)];
			if (reading.pressed || ++reading.visited > most) continue;

			if (child.vt == VT_DISPATCH && child.pdispVal)
			{
				ComPtr<IAccessible> inner;
				if (FAILED(child.pdispVal->QueryInterface(IID_PPV_ARGS(&inner)))) continue;
				VARIANT self;
				VariantInit(&self);
				self.vt = VT_I4;
				self.lVal = CHILDID_SELF;
				// The title bar's buttons and the window's menu belong to the window, not to what it asks.
				const auto role = number_of(inner.Get(), self, true);
				if (role == ROLE_SYSTEM_TITLEBAR || role == ROLE_SYSTEM_MENUBAR || role == ROLE_SYSTEM_SCROLLBAR)
					continue;
				read_element(inner.Get(), self, reading);
				read_children(inner.Get(), reading, depth + 1);
			}
			else if (child.vt == VT_I4)
			{
				read_element(element, child, reading);
			}
		}

		for (long i = 0; i < obtained; ++i) VariantClear(&children[static_cast<size_t>(i)]);
	}

	prompt_reading read_prompt(const HWND hwnd, const std::string_view press)
	{
		prompt_reading reading;
		reading.press = press;
		ComPtr<IAccessible> client;
		if (SUCCEEDED(AccessibleObjectFromWindow(hwnd, static_cast<DWORD>(OBJID_CLIENT), IID_PPV_ARGS(&client))) &&
		    client)
			read_children(client.Get(), reading, 0);
		return reading;
	}
}

bool platform::system_prompt_shown(const uintptr_t id)
{
	if (id != 0)
	{
		const auto hwnd = reinterpret_cast<HWND>(id);
		return IsWindow(hwnd) && is_system_prompt(hwnd);
	}

	return !prompt_windows().empty();
}

std::vector<platform::system_prompt> platform::system_prompts()
{
	std::vector<system_prompt> result;

	for (const auto hwnd : prompt_windows())
	{
		system_prompt prompt;
		prompt.id = reinterpret_cast<uintptr_t>(hwnd);
		wchar_t title[256] = {};
		GetWindowTextW(hwnd, title, static_cast<int>(std::size(title)));
		prompt.title = plain_text(core::utf16_to_utf8(title));
		auto reading = read_prompt(hwnd, {});
		prompt.texts = std::move(reading.texts);
		prompt.buttons = std::move(reading.buttons);
		result.push_back(std::move(prompt));
	}

	return result;
}

bool platform::answer_system_prompt(const uintptr_t id, const std::string_view button)
{
	const auto hwnd = reinterpret_cast<HWND>(id);
	if (!IsWindow(hwnd) || !is_system_prompt(hwnd)) return false;

	// Cancelled as Escape cancels a dialog: the command its Cancel button sends. A prompt with no way to cancel it, a
	// message box asking only Yes or No, ignores it, and is answered by its buttons.
	if (button.empty()) return PostMessageW(hwnd, WM_COMMAND, MAKEWPARAM(IDCANCEL, BN_CLICKED), 0) != FALSE;

	return read_prompt(hwnd, button).pressed;
}
