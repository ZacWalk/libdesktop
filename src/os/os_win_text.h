// os_win_text.h - UTF-8 and UTF-16 conversion for libdesktop-os's Windows backend. Internal to it.

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
