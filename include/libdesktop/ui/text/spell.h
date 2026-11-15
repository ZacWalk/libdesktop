// This file is part of libdesktop, a C++20 platform layer for desktop applications
// Copyright 2026  Zac Walker
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

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
