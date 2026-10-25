// spell.h — spell checking for the text views.
//
// A thin cache over pf::spell_checker, shared by every buffer in the process: the
// platform checker is expensive to create and the same words recur constantly.
//
// Whether a given document *should* be spell checked is the application's policy —
// nothing here looks at a path or a document type.

#pragma once

#include "platform.h"

#include <string>
#include <string_view>
#include <vector>

namespace pf::ui::spell
{
	// True when this byte is part of a word. Non-ASCII bytes count, so a scan never
	// stops inside a multi-byte code point and splits a character in half.
	inline bool is_word_byte(const char ch)
	{
		const auto b = static_cast<unsigned char>(ch);
		return b >= 0x80 || isalnum(b) != 0;
	}

	// True when the word is spelled correctly, and true when no checker is available
	// — an absent checker must not underline the whole document.
	bool check_word(std::string_view word);

	std::vector<std::string> suggest(std::string_view word);
	void add_word(std::string_view word);

	// Drops the checker and the cache. Tests use it to start from a known state.
	void reset();
}
