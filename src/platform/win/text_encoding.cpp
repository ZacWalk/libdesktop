// This file is part of libdesktop, a C++20 platform layer for desktop applications
// Copyright 2026  Zac Walker
//
// This program is free software; you can redistribute it and / or modify it
// under the terms of the LGPL License either version 2.1 or later.
// License details are available at https://www.gnu.org/licenses/lgpl-2.1.html
// This program is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY

// Purpose: Windows URL resolution and code-page conversion for platform text services.

#include "libdesktop/platform.h"
#include "os.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <shlwapi.h>

#include <climits>
#include <string>
#include <string_view>

#pragma comment(lib, "shlwapi.lib")

std::string platform::resolve_url(const std::string_view base, const std::string_view relative)
{
	const auto wrelative = core::utf8_to_utf16(relative);
	const auto wbase = core::utf8_to_utf16(base);
	std::wstring resolved;

	if (PathIsRelativeW(wrelative.c_str()) && !PathIsURLW(wrelative.c_str()))
	{
		wchar_t buffer[2084] = {};
		DWORD length = static_cast<DWORD>(std::size(buffer));
		if (UrlCombineW(wbase.c_str(), wrelative.c_str(), buffer, &length, 0) == S_OK)
			resolved.assign(buffer, length);
	}
	else if (PathIsURLW(wrelative.c_str()))
	{
		resolved = wrelative;
	}
	else
	{
		wchar_t buffer[2084] = {};
		DWORD length = static_cast<DWORD>(std::size(buffer));
		if (UrlCreateFromPathW(wrelative.c_str(), buffer, &length, 0) == S_OK)
			resolved.assign(buffer, length);
	}

	auto result = core::utf16_to_utf8(resolved);
	// The legacy API returns filesystem paths for file URLs, not a file:// spelling.
	if (result.starts_with("file:///"))
		result.erase(5, 1);
	if (result.starts_with("file://"))
		result.erase(0, 7);
	return result;
}

uint32_t platform::charset_to_codepage(const std::string_view charset)
{
	struct entry
	{
		std::string_view name;
		uint32_t codepage;
	};

	// The labels web pages use. Unknown labels deliberately return zero so callers can
	// apply their UTF-8 default rather than guessing a locale-specific code page.
	static constexpr entry table[] = {
		{"utf-8", CP_UTF8}, {"utf8", CP_UTF8}, {"us-ascii", CP_UTF8}, {"ascii", CP_UTF8},
		{"utf-16", 1200}, {"utf-16le", 1200}, {"utf-16be", 1201},
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

	for (const auto& candidate : table)
		if (core::icmp(charset, candidate.name) == 0) return candidate.codepage;
	return 0;
}

std::string platform::transcode_to_utf8(const std::string_view bytes, const uint32_t codepage)
{
	if (codepage == 0 || codepage == CP_UTF8 || bytes.empty() || bytes.size() > INT_MAX)
		return std::string(bytes);

	// MultiByteToWideChar does not accept UTF-16 code pages; decode their byte order directly.
	if (codepage == 1200 || codepage == 1201)
	{
		const size_t units = bytes.size() / 2;
		std::wstring wide(units, L'\0');
		for (size_t i = 0; i != units; ++i)
		{
			const auto first = static_cast<uint8_t>(bytes[i * 2]);
			const auto second = static_cast<uint8_t>(bytes[i * 2 + 1]);
			wide[i] = static_cast<wchar_t>(codepage == 1200 ? first | second << 8 : second | first << 8);
		}
		return core::utf16_to_utf8(wide);
	}

	const auto byte_count = static_cast<int>(bytes.size());
	const auto wide_count = MultiByteToWideChar(static_cast<UINT>(codepage), 0, bytes.data(), byte_count,
	                                             nullptr, 0);
	if (wide_count <= 0) return std::string(bytes);

	std::wstring wide(static_cast<size_t>(wide_count), L'\0');
	if (MultiByteToWideChar(static_cast<UINT>(codepage), 0, bytes.data(), byte_count, wide.data(), wide_count) <= 0)
		return std::string(bytes);
	return core::utf16_to_utf8(wide);
}

std::string platform::url_encode(const std::string_view input)
{
	return os::url_encode(input);
}
