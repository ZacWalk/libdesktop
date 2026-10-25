// system_test.h — the assertions and registry libdesktop-system's tests are written with.
//
// They are Diffractor's own (its src/test.h), restricted to the module's types, so a test moves
// between that suite and this one unchanged. Inline rather than static: a test file that uses only
// some of them would otherwise warn about the rest.

#pragma once

#include <source_location>

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

	if (str::icmp(actual, expected) != 0)
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

	if (str::icmp(actual, expected) == 0)
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
	assert_equal(str::utf16_to_utf8(expected), str::utf16_to_utf8(actual), name, message, loc);
}

inline void assert_equal(const int expected, const int actual, const std::string_view name = {},
                         const std::string_view message = {},
                         const std::source_location& loc = std::source_location::current())
{
	assert_equal(str::to_string(expected), str::to_string(actual), name, message, loc);
}

inline void assert_equal(const uint32_t expected, const uint32_t actual, const std::string_view name = {},
                         const std::string_view message = {},
                         const std::source_location& loc = std::source_location::current())
{
	assert_equal(str::to_string(expected), str::to_string(actual), name, message, loc);
}

inline void assert_equal(const uint64_t expected, const uint64_t actual, const std::string_view name = {},
                         const std::string_view message = {},
                         const std::source_location& loc = std::source_location::current())
{
	assert_equal(str::to_string(expected), str::to_string(actual), name, message, loc);
}

inline void assert_equal(const bool expected, const bool actual, const std::string_view name = {},
                         const std::string_view message = {},
                         const std::source_location& loc = std::source_location::current())
{
	assert_equal(str::to_string(expected), str::to_string(actual), name, message, loc);
}

// Note: assert_true/assert_false are reserved: util.h already defines assert_true as a runtime
// invariant macro. Use assert_equal(true, ...) in tests.

inline void assert_equal(const double expected, const double actual, const std::string_view name = {},
                         const std::string_view message = {},
                         const std::source_location& loc = std::source_location::current())
{
	assert_equal(str::to_string(expected, 5), str::to_string(actual, 5), name, message, loc);
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

inline void assert_equal(const df::date_t expected, const df::date_t actual, const std::string_view name = {},
                         const std::string_view message = {},
                         const std::source_location& loc = std::source_location::current())
{
	++test_assert_count;

	if (expected.to_int64() != actual.to_int64())
	{
		throw test_assert_exception(
			std::format("{} - {}: expected '{}' ({}), got '{}' ({}) at {}", message, name,
			            platform::format_date_time(expected), expected.to_int64(),
			            platform::format_date_time(actual), actual.to_int64(), test_assert_where(loc)));
	}
}

inline void assert_equal(const df::xy8 expected, const df::xy8 actual, const std::string_view name = {},
                         const std::string_view message = {},
                         const std::source_location& loc = std::source_location::current())
{
	assert_equal(expected.str(), actual.str(), name, message, loc);
}

// A private temporary folder per run, removed at the end; Diffractor's fixture without its
// /test-temp override.
class temp_files
{
	mutable df::folder_path _folder;
	df::file_paths _paths;
	mutable int _folder_seq = 0;
	mutable bool _owns_folder = false;

	df::folder_path resolve_folder() const
	{
		if (_folder.is_empty())
		{
			const auto base = platform::temp_folder();
			if (!base.exists())
			{
				platform::create_folder(base);
			}

			for (auto attempt = 0; attempt < 100; ++attempt)
			{
				const auto candidate = base.combine(std::format("libdesktop-{}-{}", platform::tick_count(), attempt));
				if (candidate.exists()) continue;

				const auto result = platform::create_folder(candidate);
				if (result.success())
				{
					_folder = candidate;
					_owns_folder = true;
					break;
				}
			}

			if (_folder.is_empty())
			{
				throw std::runtime_error("Failed to create an exclusive test temp folder");
			}
		}

		return _folder;
	}

public:
	temp_files() = default;
	temp_files(const temp_files&) = delete;
	temp_files& operator=(const temp_files&) = delete;

	std::vector<std::string> delete_temps()
	{
		std::vector<std::string> errors;
		df::file_paths remaining_paths;

		for (const auto& path : _paths)
		{
			if (!path.exists()) continue;

			const auto result = platform::delete_file(path);
			if (result.failed() && path.exists())
			{
				errors.emplace_back(result.format_error("delete temp file", path.str()));
				remaining_paths.emplace_back(path);
			}
		}

		_paths = std::move(remaining_paths);

		if (_owns_folder && !_folder.is_empty() && _folder.exists())
		{
			std::error_code ec;
			std::filesystem::remove_all(platform::to_stream_path(_folder), ec);
			if (ec)
			{
				errors.emplace_back(std::format("delete temp folder\n{}\n{}", _folder.text(), ec.message()));
			}

			if (!_folder.exists())
			{
				_folder = {};
				_owns_folder = false;
				_folder_seq = 0;
				_paths.clear();
			}
		}

		return errors;
	}

	df::folder_path folder() const
	{
		return resolve_folder();
	}

	df::file_path next_path(const std::string_view ext = ".jpg")
	{
		auto result = platform::temp_file(ext, resolve_folder());
		_paths.emplace_back(result);
		return result;
	}

	df::folder_path next_folder(const std::string_view name)
	{
		const auto result = resolve_folder().combine(std::format("{}-{}", name, ++_folder_seq));

		if (!result.exists())
		{
			platform::create_folder(result);
		}

		return result;
	}
};

inline temp_files _temps;

inline void write_test_file(const df::file_path path, const std::string_view text)
{
	std::ofstream fs(platform::to_stream_path(path), std::ios::binary | std::ios::trunc);
	fs << text;
}

inline std::string read_test_file(const df::file_path path)
{
	const auto data = df::blob_from_file(path);
	return {std::bit_cast<const char*>(data.data()), data.size()};
}
