// spell.cpp — the process-wide spell checker and its result cache.

#include "platform.h"
#include "ui/spell.h"

#include <memory>
#include <unordered_map>

namespace pf::ui::spell
{
	namespace
	{
		// Shared across every buffer: creating the platform checker is expensive and
		// the same words recur constantly while typing.
		std::unique_ptr<pf::spell_checker> s_checker;
		std::unordered_map<uint64_t, bool> s_cache;

		void ensure_checker()
		{
			if (!s_checker)
				s_checker = pf::create_spell_checker();
		}
	}

	bool check_word(const std::string_view word)
	{
		ensure_checker();
		if (!s_checker) return true;

		const auto key = pf::fnv1a_i_64(word);
		const auto it = s_cache.find(key);
		if (it != s_cache.end()) return it->second;

		const auto valid = s_checker->is_word_valid(word);
		s_cache[key] = valid;
		return valid;
	}

	std::vector<std::string> suggest(const std::string_view word)
	{
		ensure_checker();
		if (!s_checker) return {};

		const auto wresults = s_checker->suggest(word);
		std::vector<std::string> results;
		results.reserve(wresults.size());
		for (const auto& w : wresults)
			results.push_back(w);
		return results;
	}

	void add_word(const std::string_view word)
	{
		ensure_checker();
		if (s_checker)
		{
			s_checker->add_word(word);
			s_cache[pf::fnv1a_i_64(word)] = true;
		}
	}

	void reset()
	{
		s_checker.reset();
		s_cache.clear();
	}
}
