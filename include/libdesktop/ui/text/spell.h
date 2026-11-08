// This file is part of libdesktop, a C++20 platform layer for desktop applications
// Copyright 2026  Zac Walker
//
// This program is free software; you can redistribute it and / or modify it
// under the terms of the LGPL License either version 2.1 or later.
// License details are available at https://www.gnu.org/licenses/lgpl-2.1.html
// This program is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY

// Purpose: Cached process-wide spell-checking seam for text views.

#pragma once

#include <cctype>
#include <string>
#include <string_view>
#include <vector>

namespace ui::spell
{
	// Non-ASCII bytes count as word bytes, so a scan never stops inside a UTF-8 code point.
	inline bool is_word_byte(const char ch)
	{
		const auto b = static_cast<unsigned char>(ch);
		return b >= 0x80 || std::isalnum(b) != 0;
	}

	bool check_word(std::string_view word);
	std::vector<std::string> suggest(std::string_view word);
	void add_word(std::string_view word);
	void reset();
}
