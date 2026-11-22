// This file is part of libdesktop, a C++20 platform layer for desktop applications
// Copyright 2026  Zac Walker
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

// Purpose: The system's own prompts read and answered as an agent session answers them: a dialog of the system's
// class put up in front, made off the screen and cloaked so a test run shows nothing.

#include "libdesktop/platform.h"
#include "libdesktop/platform/win32.h"
#include "test.h"

#include <dwmapi.h>

#pragma comment(lib, "dwmapi.lib")

using namespace std::literals;

namespace
{
	int answered_with = 0;

	INT_PTR CALLBACK prompt_proc(const HWND hwnd, const UINT message, const WPARAM wparam, LPARAM)
	{
		if (message == WM_INITDIALOG) return TRUE;
		if (message != WM_COMMAND) return FALSE;
		answered_with = LOWORD(wparam);
		DestroyWindow(hwnd);
		return TRUE;
	}

	// A dialog template made in memory, as a resource compiler would: a question and Yes and No, as the shell's
	// confirmations ask.
	class prompt_template
	{
		std::vector<WORD> _words;

		void dword(const DWORD value)
		{
			_words.push_back(LOWORD(value));
			_words.push_back(HIWORD(value));
		}
		void text(const std::wstring_view s)
		{
			for (const auto c : s) _words.push_back(c);
			_words.push_back(0);
		}
		void align()
		{
			if (_words.size() % 2 != 0) _words.push_back(0);
		}
		void item(const WORD id, const WORD control_class, const DWORD style, const short x, const short y,
		          const short cx, const short cy, const std::wstring_view title)
		{
			align();
			dword(style | WS_CHILD | WS_VISIBLE);
			dword(0);
			for (const auto v : {x, y, cx, cy}) _words.push_back(static_cast<WORD>(v));
			_words.push_back(id);
			_words.push_back(0xFFFF);
			_words.push_back(control_class);
			text(title);
			_words.push_back(0);
		}

	public:
		prompt_template()
		{
			constexpr WORD button = 0x0080, label = 0x0082;
			dword(DS_MODALFRAME | DS_SETFONT | WS_POPUP | WS_CAPTION | WS_SYSMENU);
			dword(0);
			_words.push_back(3);
			for (const auto v : {short{0}, short{0}, short{200}, short{70}}) _words.push_back(static_cast<WORD>(v));
			_words.push_back(0);
			_words.push_back(0);
			text(L"Delete File");
			_words.push_back(9);
			text(L"Segoe UI");
			item(0xFFFF, label, SS_LEFT, 8, 8, 184, 20, L"Are you sure you want to permanently delete this file?");
			item(IDYES, button, BS_DEFPUSHBUTTON, 88, 48, 50, 14, L"&Yes");
			item(IDNO, button, BS_PUSHBUTTON, 142, 48, 50, 14, L"&No");
		}

		const DLGTEMPLATE* get() const { return reinterpret_cast<const DLGTEMPLATE*>(_words.data()); }
	};

	HWND show_prompt(const prompt_template& t)
	{
		answered_with = 0;
		const auto hwnd = CreateDialogIndirectParamW(GetModuleHandleW(nullptr), t.get(), nullptr, prompt_proc, 0);
		constexpr BOOL cloaked = TRUE;
		DwmSetWindowAttribute(hwnd, DWMWA_CLOAK, &cloaked, sizeof(cloaked));
		SetWindowPos(hwnd, nullptr, -32000, -32000, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_SHOWWINDOW);
		return hwnd;
	}

	// Messages handled until the prompt has been answered, or a second has passed.
	void pump_until_answered()
	{
		const auto until = GetTickCount64() + 1000;
		while (answered_with == 0 && GetTickCount64() < until)
		{
			MSG msg;
			while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE))
			{
				TranslateMessage(&msg);
				DispatchMessageW(&msg);
			}
			Sleep(5);
		}
	}
}

// docs/platform.md: a prompt the system puts up is found, read as a screen reader reads it - its title, its text and
// its buttons, without their access keys - and answered by a button's text, any case, or cancelled as Escape does.
static void should_read_and_answer_a_system_prompt()
{
	const prompt_template t;
	auto hwnd = show_prompt(t);
	const auto id = reinterpret_cast<uintptr_t>(hwnd);
	assert_equal(true, hwnd != nullptr && platform::system_prompt_shown() && platform::system_prompt_shown(id),
	             "a dialog of the system's class in front of the application is a prompt");

	const auto prompts = platform::system_prompts();
	const auto found = std::ranges::find(prompts, id, &platform::system_prompt::id);
	assert_equal(true, found != prompts.end(), "it is listed");
	if (found == prompts.end()) return;
	assert_equal("Delete File"s, found->title, "by its title");
	assert_equal(true,
	             std::ranges::find(found->texts, "Are you sure you want to permanently delete this file?"s) !=
	                 found->texts.end(),
	             "what it asks is read");
	assert_equal(true, found->buttons == std::vector<std::string>{"Yes", "No"}, "and its buttons, in order");

	assert_equal(false, platform::answer_system_prompt(id, "Maybe"), "a button it does not have is not pressed");
	assert_equal(true, platform::answer_system_prompt(id, "no"), "a button is pressed by its text");
	pump_until_answered();
	assert_equal(IDNO, answered_with, "and answers as that button does");
	assert_equal(false, platform::system_prompt_shown(id), "after which it has gone");
	assert_equal(false, platform::answer_system_prompt(id, "Yes"), "and nothing more can be pressed on it");

	hwnd = show_prompt(t);
	assert_equal(true, platform::answer_system_prompt(reinterpret_cast<uintptr_t>(hwnd), {}), "cancelled");
	pump_until_answered();
	assert_equal(IDCANCEL, answered_with, "it answers as Escape does");
	if (IsWindow(hwnd)) DestroyWindow(hwnd);
}

void register_platform_prompt_win_tests(test_registry& tests)
{
	tests.add("Should read and answer a system prompt"s, should_read_and_answer_a_system_prompt);
}
