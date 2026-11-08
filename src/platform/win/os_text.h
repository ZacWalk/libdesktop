// This file is part of libdesktop, a C++20 platform layer for desktop applications
// Copyright 2026  Zac Walker
//
// This program is free software; you can redistribute it and / or modify it
// under the terms of the LGPL License either version 2.1 or later.
// License details are available at https://www.gnu.org/licenses/lgpl-2.1.html
// This program is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY

// os_text.h - UTF-8 and UTF-16 conversion for the Windows os:: services. Internal to them.

#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <climits>
#include <string>
#include <string_view>

namespace os::detail
{
	inline std::wstring widen(const std::string_view text)
	{
		if (text.empty() || text.size() > INT_MAX) return {};
		const auto size = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
		std::wstring result(static_cast<size_t>(size), L'\0');
		MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), result.data(), size);
		return result;
	}

	inline std::string narrow(const std::wstring_view text)
	{
		if (text.empty() || text.size() > INT_MAX) return {};
		const auto size = WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0,
		                                      nullptr, nullptr);
		std::string result(static_cast<size_t>(size), '\0');
		WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), result.data(), size, nullptr,
		                    nullptr);
		return result;
	}
}
