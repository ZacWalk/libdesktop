// check.h - the counting checks libdesktop-ui's and libdesktop-text's suites share. Every CHECK is
// counted and a failure prints its line and expression without stopping the run, so one report lists
// every failure; each suite's main() prints the totals and fails when any check did.

#pragma once

#include <cstdio>
#include <string_view>

inline int g_checks = 0;
inline int g_failures = 0;

inline void check(const bool ok, const char* expr, const int line)
{
	++g_checks;
	if (!ok)
	{
		++g_failures;
		std::printf("FAIL line %d: %s\n", line, expr);
	}
}

template <typename T, typename U>
void check_eq(const T& actual, const U& expected, const char* expr, const int line)
{
	++g_checks;
	if (!(actual == expected))
	{
		++g_failures;
		std::printf("FAIL line %d: %s\n", line, expr);
	}
}

inline void check_eq_str(const std::string_view actual, const std::string_view expected,
                         const char* expr, const int line)
{
	++g_checks;
	if (actual != expected)
	{
		++g_failures;
		std::printf("FAIL line %d: %s\n  expected: '%.*s'\n  actual:   '%.*s'\n",
		            line, expr,
		            static_cast<int>(expected.size()), expected.data(),
		            static_cast<int>(actual.size()), actual.data());
	}
}

#define CHECK(expr)     check((expr), #expr, __LINE__)
#define CHECK_EQ(a, b)  check_eq((a), (b), #a " == " #b, __LINE__)
#define CHECK_STR(a, b) check_eq_str((a), (b), #a " == " #b, __LINE__)
