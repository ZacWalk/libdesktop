// This file is part of libdesktop, a C++20 platform layer for desktop applications
// Copyright 2026  Zac Walker
//
// This program is free software; you can redistribute it and / or modify it
// under the terms of the LGPL License either version 2.1 or later.
// License details are available at https://www.gnu.org/licenses/lgpl-2.1.html
// This program is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY

// Purpose: Cached process-wide spell-checking seam for text views.

#include "libdesktop/platform/shell.h"
#include "libdesktop/ui/text/spell.h"

#include <cstdint>
#include <memory>
#include <unordered_map>

namespace ui::spell
{
	namespace
	{
		std::shared_ptr<platform::spell_checker> s_checker;
		std::unordered_map<uint64_t, bool> s_cache;

		uint64_t word_key(const std::string_view word)
		{
			uint64_t result = 1469598103934665603ull;
			for (const auto ch : word)
			{
				const auto b = static_cast<unsigned char>(ch);
				const auto folded = b >= 'A' && b <= 'Z' ? static_cast<unsigned char>(b + ('a' - 'A')) : b;
				result ^= folded;
				result *= 1099511628211ull;
			}
			return result;
		}

		void ensure_checker()
		{
			if (!s_checker) s_checker = platform::create_spell_checker();
		}

		bool checker_accepts(const std::string_view word)
		{
			return s_checker->check_word(word);
		}
	}

	bool check_word(const std::string_view word)
	{
		ensure_checker();
		if (!s_checker) return true;

		const auto key = word_key(word);
		if (const auto it = s_cache.find(key); it != s_cache.end()) return it->second;

		const auto valid = checker_accepts(word);
		s_cache[key] = valid;
		return valid;
	}

	std::vector<std::string> suggest(const std::string_view word)
	{
		ensure_checker();
		if (!s_checker) return {};
		const auto suggestions = s_checker->suggest(word);
		std::vector<std::string> result;
		result.reserve(suggestions.size());
		for (const auto& suggestion : suggestions) result.emplace_back(suggestion);
		return result;
	}

	void add_word(const std::string_view word)
	{
		ensure_checker();
		if (!s_checker) return;
		s_checker->add_word(word);
		s_cache[word_key(word)] = true;
	}

	void reset()
	{
		s_checker.reset();
		s_cache.clear();
	}
}
