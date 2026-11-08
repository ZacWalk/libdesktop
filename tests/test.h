// test.h — the assertions and registry shared by every libdesktop test suite.
//
// They are Diffractor's own (its src/test.h), restricted to the module's types, so a test moves
// between that suite and this one unchanged. Inline rather than static: a test file that uses only
// some of them would otherwise warn about the rest.

#pragma once

#include "libdesktop/core.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <format>
#include <functional>
#include <limits>
#include <source_location>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

class test_assert_exception final : public std::exception
{
public:
	std::string message;

	explicit test_assert_exception(std::string m) : message(std::move(m))
	{
	}
};

struct test_registry
{
	virtual ~test_registry() = default;
	virtual void add(std::string name, std::function<void(void)> f) = 0;
};

// Counted so the runner can fail a test that asserted nothing: a test whose only assertions sit
// behind an early return or a disabled #if would otherwise report PASS on an unsuitable machine.
inline int test_assert_count = 0;

inline std::string test_assert_where(const std::source_location& loc)
{
	const std::string_view path(loc.file_name());
	const auto slash = path.find_last_of("\\/");
	return std::format("{}:{}", slash == std::string_view::npos ? path : path.substr(slash + 1), loc.line());
}

inline void assert_equal(const std::string_view expected, const std::string_view actual,
                         const std::string_view name = {}, const std::string_view message = {},
                         const std::source_location& loc = std::source_location::current())
{
	++test_assert_count;

	if (core::icmp(actual, expected) != 0)
	{
		throw test_assert_exception(
			std::format("{} - {}: expected '{}', got '{}' at {}", message, name, expected, actual,
			            test_assert_where(loc)));
	}
}

inline void assert_not_equal(const std::string_view expected, const std::string_view actual,
                             const std::string_view name = {}, const std::string_view message = {},
                             const std::source_location& loc = std::source_location::current())
{
	++test_assert_count;

	if (core::icmp(actual, expected) == 0)
	{
		throw test_assert_exception(std::format("{} - {}: expected '{}' not to equal '{}' at {}", message, name,
		                                        expected, actual, test_assert_where(loc)));
	}
}

inline void assert_equal_strict(const std::string_view expected, const std::string_view actual,
                                const std::string_view name = {}, const std::string_view message = {},
                                const std::source_location& loc = std::source_location::current())
{
	++test_assert_count;

	if (actual != expected)
	{
		throw test_assert_exception(
			std::format("{} - {}: expected '{}', got '{}' at {}", message, name, expected, actual,
			            test_assert_where(loc)));
	}
}

inline void assert_equal(const std::wstring_view expected, const std::wstring_view actual,
                         const std::string_view name = {}, const std::string_view message = {},
                         const std::source_location& loc = std::source_location::current())
{
	assert_equal(core::utf16_to_utf8(expected), core::utf16_to_utf8(actual), name, message, loc);
}

inline void assert_equal(const int expected, const int actual, const std::string_view name = {},
                         const std::string_view message = {},
                         const std::source_location& loc = std::source_location::current())
{
	assert_equal(core::to_string(expected), core::to_string(actual), name, message, loc);
}

inline void assert_equal(const uint32_t expected, const uint32_t actual, const std::string_view name = {},
                         const std::string_view message = {},
                         const std::source_location& loc = std::source_location::current())
{
	assert_equal(core::to_string(expected), core::to_string(actual), name, message, loc);
}

inline void assert_equal(const uint64_t expected, const uint64_t actual, const std::string_view name = {},
                         const std::string_view message = {},
                         const std::source_location& loc = std::source_location::current())
{
	assert_equal(core::to_string(expected), core::to_string(actual), name, message, loc);
}

inline void assert_equal(const bool expected, const bool actual, const std::string_view name = {},
                         const std::string_view message = {},
                         const std::source_location& loc = std::source_location::current())
{
	assert_equal(core::to_string(expected), core::to_string(actual), name, message, loc);
}

// Note: assert_true/assert_false are reserved: util.h already defines assert_true as a runtime
// invariant macro. Use assert_equal(true, ...) in tests.

inline void assert_equal(const double expected, const double actual, const std::string_view name = {},
                         const std::string_view message = {},
                         const std::source_location& loc = std::source_location::current())
{
	assert_equal(core::to_string(expected, 5), core::to_string(actual, 5), name, message, loc);
}

inline void assert_near(const double expected, const double actual, const double tolerance,
                        const std::string_view name = {}, const std::string_view message = {},
                        const std::source_location& loc = std::source_location::current())
{
	++test_assert_count;

	if (!std::isfinite(expected) || !std::isfinite(actual) || !std::isfinite(tolerance) || tolerance < 0.0 ||
		std::abs(expected - actual) > tolerance)
	{
		throw test_assert_exception(
			std::format("{} - {}: expected '{}' within '{}', got '{}' at {}", message, name, expected, tolerance,
			            actual, test_assert_where(loc)));
	}
}

inline void assert_equal(const core::date_t expected, const core::date_t actual, const std::string_view name = {},
                         const std::string_view message = {},
                         const std::source_location& loc = std::source_location::current())
{
	++test_assert_count;

	if (expected.to_int64() != actual.to_int64())
	{
		throw test_assert_exception(
			std::format("{} - {}: expected {} ({}), got {} ({}) at {}", message, name,
			            expected.to_int64(), expected.to_int64(),
			            actual.to_int64(), actual.to_int64(), test_assert_where(loc)));
	}
}

inline void assert_equal(const core::xy8 expected, const core::xy8 actual, const std::string_view name = {},
                         const std::string_view message = {},
                         const std::source_location& loc = std::source_location::current())
{
	assert_equal(expected.str(), actual.str(), name, message, loc);
}

template <typename Registry, typename Cleanup>
int run_registered_tests(Registry& all, const int argc, char* argv[], Cleanup cleanup)
{
	const std::string_view filter = argc > 1 ? argv[1] : "*";
	auto run = 0;
	auto failed = 0;

	for (const auto& [name, test] : all.tests)
	{
		if (!core::wildcard_icmp(name, filter)) continue;

		++run;
		const auto checks_before = test_assert_count;
		std::string failure;

		try
		{
			test();
			if (test_assert_count == checks_before) failure = "asserted nothing";
		}
		catch (const test_assert_exception& e)
		{
			failure = e.message;
		}
		catch (const std::exception& e)
		{
			failure = std::format("threw {}", e.what());
		}

		if (failure.empty())
		{
			std::printf("PASS  %s\n", name.c_str());
		}
		else
		{
			++failed;
			std::printf("FAIL  %s: %s\n", name.c_str(), failure.c_str());
		}
	}

	for (const auto& error : cleanup())
	{
		++failed;
		std::printf("FAIL  temporary files: %s\n", error.c_str());
	}

	std::printf("\n%d tests, %d failed, %d checks\n", run, failed, test_assert_count);
	return failed == 0 && run > 0 ? 0 : 1;
}

