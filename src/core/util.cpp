// This file is part of libdesktop, a C++20 platform layer for desktop applications
// Copyright 2026  Zac Walker
// 
// This program is free software; you can redistribute it and / or modify it
// under the terms of the LGPL License either version 2.1 or later.
// License details are available at https://www.gnu.org/licenses/lgpl-2.1.html
// This program is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY

// Purpose: Core utility functions and data types. Implements file paths, dates, blobs,
// logging, and other foundational types used throughout the application.

#include "libdesktop/core.h"
#include "libdesktop/core/util.h"
#include "libdesktop/core/simd.h"

using namespace std::literals;

namespace
{
	struct app_registration
	{
		std::string name;
		std::string file_stem;
	};

	app_registration& app_registration_state()
	{
		// Deliberately immortal: callers may retain the returned views through static teardown.
		static auto* const state = new app_registration;
		return *state;
	}
}

void core::set_app_name(const std::string_view name)
{
	auto& state = app_registration_state();
	state.name.assign(name);
	state.file_stem = state.name;
	for (auto& c : state.file_stem)
	{
		if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
	}
}

std::string_view core::app_name()
{
	return app_registration_state().name;
}

df_assert_pod(core::file_size);
df_assert_pod(core::day_range_t);
df_assert_pod(core::day_t);
df_assert_pod(core::date_t);
df_assert_pod(core::sizei);
df_assert_pod(core::pointi);
df_assert_pod(core::recti);
df_assert_pod(core::sized);
df_assert_pod(core::pointd);
df_assert_pod(core::rectd);
df_assert_pod(core::affined);
df_assert_pod(core::quadd);
df_assert_pod(core::xy8);
df_assert_pod(core::xy16);
df_assert_pod(core::xy32);
df_assert_pod(core::int_counter);
df_assert_pod(core::file_path);
df_assert_pod(core::folder_path);
df_assert_pod(std::string_view);

df_assert_pod(core::cspan);
df_assert_pod(core::span);
df_assert_move_only(core::blob);


std::atomic_int core::cancel_token::empty;

double core::now()
{
	return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

int64_t core::now_ms()
{
	return std::chrono::duration_cast<std::chrono::milliseconds>(
		std::chrono::steady_clock::now().time_since_epoch()).count();
}

int64_t core::now_us()
{
	return std::chrono::duration_cast<std::chrono::microseconds>(
		std::chrono::steady_clock::now().time_since_epoch()).count();
}

std::string_view core::app_file_stem()
{
	return app_registration_state().file_stem;
}

static core::mutex log_mutex;
_Guarded_by_(log_mutex) static core::log_sink s_log_sink = nullptr;
_Guarded_by_(log_mutex) static size_t log_bytes = 0;
_Guarded_by_(log_mutex) static bool log_truncated = false;

// A repeating per-item failure logs once per file, so a long session over a large collection would
// otherwise grow the log without bound. One rotation is kept, so the worst case on disk is twice this.
constexpr std::streamoff max_log_bytes = 4ll * 1024 * 1024;

namespace
{
	size_t ifind_from(const std::string_view text, const std::string_view sub, const size_t from)
	{
		if (from >= text.size()) return std::string::npos;
		const auto found = core::ifind(text.substr(from), sub);
		return found == std::string_view::npos ? std::string::npos : found + from;
	}

	// The log is attached to support reports and crash uploads, so it must not carry the account
	// name. Everything else about a path is kept - the folder shape is what makes a report
	// reproducible. Deliberately pattern-based rather than a OS user-name comparison:
	// that call logs on failure, and this runs holding the log lock. It also catches the 8.3 short
	// form (ZACWAL~1) and names embedded in OS and third-party error strings.
	std::string scrub_user_identity(const std::string_view message)
	{
		std::string result(message);

		for (const auto users : {"\\users\\"sv, "/users/"sv})
		{
			for (size_t at = 0; (at = ifind_from(result, users, at)) != std::string::npos;)
			{
				const auto begin = at + users.size();
				auto end = begin;
				while (end < result.size() && result[end] != '\\' && result[end] != '/') ++end;

				if (end == begin)
				{
					at = begin;
					continue;
				}

				constexpr auto token = "%user%"sv;
				result.replace(begin, end - begin, token);
				at = begin + token.size();
			}
		}

		return result;
	}

	// The end-of-session summaries are the highest-value lines in the file and are written last, so
	// they stay exempt from the cap. Each is emitted once per session, so the exemption is bounded.
	bool is_session_summary(const std::string_view context)
	{
		return context.starts_with("perf") || context == "main";
	}
}

void core::set_log_sink(const core::log_sink sink)
{
	core::exclusive_lock lock(log_mutex);
	s_log_sink = sink;
}

void core::log(const std::string_view context, const std::string_view message)
{
	core::exclusive_lock ll(log_mutex);
	static const auto start_time = core::tick_count();

#ifdef _DEBUG
	core::debug_output(std::format("{}:{}\n", context, message));
#endif

	if (s_log_sink != nullptr && (!log_truncated || is_session_summary(context)))
	{
		const auto time = core::tick_count() - start_time;
		const auto thread_id = core::current_thread_id();

		std::ostringstream line;
		line << std::right << std::setfill('0') << std::setw(8) << thread_id
			<< " "
			<< std::setw(8) << time
			<< " "
			<< std::setfill(' ')
			// setw is a minimum, so a context longer than the column would otherwise abut the message.
			<< std::left << std::setw(32) << context << ' '
			<< scrub_user_identity(message) << '\n';
		const auto formatted = line.str();
		s_log_sink(formatted);
		log_bytes += formatted.size();

		if (!log_truncated && log_bytes >= max_log_bytes)
		{
			log_truncated = true;
			const auto marker = std::format("*** log truncated at {} bytes - only end-of-session summaries follow ***\n",
			                               max_log_bytes);
			s_log_sink(marker);
			log_bytes += marker.size();
		}
	}
}

void core::trace(const std::string_view message)
{
#ifdef _DEBUG
	core::debug_output(std::format("{}\n", message));
#endif
}

// One malformed batch repeats the same codec message for every item scanned. The distinct-message
// ceiling bounds the other direction, where a library emits a message that varies every time.
void core::log_once(const std::string_view context, const std::string_view message)
{
	constexpr size_t max_distinct = 128;

	static core::mutex mutex;
	_Guarded_by_(mutex) static core::hash_set<std::string> seen;

	{
		core::exclusive_lock lock(mutex);
		if (seen.size() >= max_distinct) return;
		if (!seen.emplace(std::format("{}|{}", context, message)).second) return;
	}

	log(context, message);
}

core::version::version(const std::string_view version)
{
	const auto parts = core::split(version, true, [](const char c) { return c == '.'; });

	if (!parts.empty())
	{
		major = core::to_int(parts[0]);
	}

	if (parts.size() > 1)
	{
		minor = core::to_int(parts[1]);
	}
}

std::string core::url_extract(const std::string_view text)
{
	static const std::regex url_regex(R"((https?:\/\/[^\s\"\']+))");
	std::match_results<std::string_view::const_iterator> match;

	if (std::regex_search(text.begin(), text.end(), match, url_regex))
	{
		return match[1].str();
	}

	return {};
}

std::vector<std::string> core::url_extract_all(const std::string_view text)
{
	static const std::regex url_regex(R"((https?:\/\/[^\s\"\']+))");
	std::vector<std::string> result;

	for (std::regex_iterator<std::string_view::const_iterator> i(text.begin(), text.end(), url_regex), end;
	     i != end; ++i)
	{
		auto found = (*i)[1].str();
		if (std::ranges::find(result, found) == result.end()) result.emplace_back(std::move(found));
	}

	return result;
}

std::string core::date_t::to_xmp_date() const
{
	const auto st = date();
	return std::format("{:04}-{:02}-{:02}T{:02}:{:02}:{:02}", st.year, st.month, st.day, st.hour, st.minute,
	                   st.second);
}

core::date_t core::date_t::from(const std::string_view r)
{
	core::date_t ft;
	ft.parse(r);
	return ft;
}

namespace
{
	// A date's text read as the C library's sscanf reads it - by glibc's rules, which MSVC's break where
	// a 0xFF byte meets a number, reading it as the end of the text and swallowing it - for the
	// conversions dates use. White space is the C locale's, and the text ends at its length or a NUL.
	class date_text
	{
		std::string_view _s;
		size_t _at = 0;

		int peek() const
		{
			return _at < _s.size() ? static_cast<unsigned char>(_s[_at]) : -1;
		}

		static bool space(const int c) { return c == ' ' || (c >= '\t' && c <= '\r'); }
		static bool digit(const int c) { return c >= '0' && c <= '9'; }
		static bool hex_digit(const int c) { return digit(c) || ((c | 0x20) >= 'a' && (c | 0x20) <= 'f'); }
		static int lower(const int c) { return c >= 'A' && c <= 'Z' ? c | 0x20 : c; }

		// Where the first significant digit of a number from_chars found out of range lies, as an
		// exponent of its base: positive for one too large, negative for one too small.
		static int64_t leading_exponent(const std::string_view s, const bool hex)
		{
			const auto in_number = [hex](const char c) { return hex ? hex_digit(static_cast<unsigned char>(c)) : digit(c); };
			size_t i = 0;
			int64_t integer_digits = 0;
			int64_t fraction_zeros = 0;
			auto significant = false;

			for (; i < s.size() && in_number(s[i]); ++i)
			{
				if (significant || s[i] != '0')
				{
					significant = true;
					++integer_digits;
				}
			}

			if (i < s.size() && s[i] == '.')
			{
				for (++i; i < s.size() && in_number(s[i]); ++i)
				{
					if (significant) continue;
					if (s[i] == '0') ++fraction_zeros;
					else significant = true;
				}
			}

			int64_t exponent = 0;

			if (i < s.size() && lower(static_cast<unsigned char>(s[i])) == (hex ? 'p' : 'e'))
			{
				++i;
				const auto negative = i < s.size() && s[i] == '-';
				if (i < s.size() && (s[i] == '-' || s[i] == '+')) ++i;
				for (; i < s.size() && digit(s[i]); ++i) exponent = std::min<int64_t>(exponent * 10 + (s[i] - '0'), 1000000000000);
				if (negative) exponent = -exponent;
			}

			const auto digits = integer_digits > 0 ? integer_digits - 1 : -fraction_zeros - 1;
			return (hex ? 4 * digits : digits) + exponent;
		}

		// What strtod makes of the number item begins with: false when it begins with none.
		static bool convert(std::string_view item, double& out)
		{
			const auto negative = !item.empty() && item[0] == '-';
			if (!item.empty() && (item[0] == '-' || item[0] == '+')) item.remove_prefix(1);

			const auto hex = item.size() >= 2 && item[0] == '0' && lower(static_cast<unsigned char>(item[1])) == 'x';
			const auto number = item.substr(hex ? 2 : 0);
			double value = 0;
			const auto [end, error] = std::from_chars(number.data(), number.data() + number.size(), value,
			                                          hex ? std::chars_format::hex : std::chars_format::general);

			if (error == std::errc::invalid_argument)
			{
				// strtod reads "0x" with no digit after it as the 0 before it.
				if (!hex) return false;
				value = 0;
			}
			else if (error == std::errc::result_out_of_range)
			{
				value = leading_exponent(number.substr(0, static_cast<size_t>(end - number.data())), hex) > 0
					        ? std::numeric_limits<double>::infinity()
					        : 0.0;
			}

			out = negative ? -value : value;
			return true;
		}

		bool take_word(const std::string_view word, std::string& item)
		{
			for (const auto c : word)
			{
				if (lower(peek()) != c) return false;
				item += _s[_at++];
			}

			return true;
		}

	public:
		explicit date_text(const std::string_view s) : _s(s.substr(0, s.find('\0')))
		{
		}

		// White space in a format: a run of it, or none.
		date_text& spaces()
		{
			while (space(peek())) ++_at;
			return *this;
		}

		// An ordinary character in a format, which white space before it does not match.
		bool is(const char c)
		{
			if (peek() != static_cast<unsigned char>(c)) return false;
			++_at;
			return true;
		}

		// %Ns, N one less than the buffer: after white space, 1 to N characters that are not.
		template <size_t N>
		bool word(char (&out)[N])
		{
			spaces();
			size_t n = 0;
			while (n + 1 < N && peek() >= 0 && !space(peek())) out[n++] = _s[_at++];
			out[n] = 0;
			return n != 0;
		}

		// %d, or %Nd for a width: after white space, a sign and digits within the width. A value a 64-bit
		// long cannot hold is held at its limit, then cut to an int, as glibc's strtol and assignment do.
		bool integer(int& out, const size_t width = 0)
		{
			spaces();
			auto left = width == 0 ? std::numeric_limits<size_t>::max() : width;
			auto negative = false;

			if (left != 0 && (peek() == '+' || peek() == '-'))
			{
				negative = peek() == '-';
				++_at;
				--left;
			}

			uint64_t magnitude = 0;
			auto digits = false;
			auto past = false;

			for (; left != 0 && digit(peek()); ++_at, --left)
			{
				const auto d = static_cast<uint64_t>(peek() - '0');
				if (magnitude > (std::numeric_limits<uint64_t>::max() - d) / 10) past = true;
				else magnitude = magnitude * 10 + d;
				digits = true;
			}

			if (!digits) return false;

			constexpr auto long_max = static_cast<uint64_t>(std::numeric_limits<int64_t>::max());
			const auto value = negative
				                   ? past || magnitude > long_max + 1 ? std::numeric_limits<int64_t>::min() : static_cast<int64_t>(0 - magnitude)
				                   : past || magnitude > long_max ? std::numeric_limits<int64_t>::max() : static_cast<int64_t>(magnitude);
			out = static_cast<int>(static_cast<uint32_t>(static_cast<uint64_t>(value)));
			return true;
		}

		// %lg: after white space, a sign, then "nan", "inf" or "infinity", or the longest run that a
		// number could go on from - digits, a point, an exponent, hexadecimal after 0x - read by strtod.
		// As glibc does, a run strtod reads none of fails, and so does a sign or a 0x with nothing after.
		bool real(double& out)
		{
			spaces();
			std::string item;
			auto c = peek();
			if (c < 0) return false;

			const auto signed_item = c == '-' || c == '+';

			if (signed_item)
			{
				item += static_cast<char>(c);
				++_at;
				c = peek();
				if (c < 0) return false;
			}

			if (lower(c) == 'n')
			{
				return take_word("nan", item) && convert(item, out);
			}

			if (lower(c) == 'i')
			{
				if (!take_word("inf", item)) return false;
				if (lower(peek()) == 'i' && !take_word("inity", item)) return false;
				return convert(item, out);
			}

			auto hex = false;
			auto got_digit = false;
			auto got_dot = false;
			auto got_e = false;
			auto exp_char = 'e';

			if (c == '0')
			{
				item += '0';
				++_at;
				c = peek();

				if (lower(c) == 'x')
				{
					item += static_cast<char>(c);
					++_at;
					c = peek();
					hex = true;
					exp_char = 'p';
				}
				else
				{
					got_digit = true;
				}
			}

			for (; c >= 0; ++_at, c = peek())
			{
				if (digit(c) || (hex && !got_e && hex_digit(c)))
				{
					item += static_cast<char>(c);
					got_digit = true;
				}
				else if (got_e && item.back() == exp_char && (c == '-' || c == '+'))
				{
					item += static_cast<char>(c);
				}
				else if (got_digit && !got_e && lower(c) == exp_char)
				{
					item += exp_char;
					got_e = got_dot = true;
				}
				else if (!got_dot && c == '.')
				{
					item += '.';
					got_dot = true;
				}
				else
				{
					break;
				}
			}

			const size_t sign = signed_item ? 1 : 0;
			if (item.size() == sign || (hex && item.size() == 2 + sign)) return false;
			return convert(item, out);
		}
	};
}

static bool parse_iso_8601_like(const std::string_view r, core::day_t& result)
{
	int yyyy = 0, mm = 0, dd = 0, hh = 0, ss = 0, min = 0;
	double sec = 0.0;
	bool success = false;

	if (!r.empty())
	{
		// Samsung 2005:08:17 11:42:43
		// Canon    THU OCT 26 16:46:04 2006
		//          TUE MAY 08 10:00:1: 2007
		// Fujifilm Mon Mar 3 09:44:56 2008

		char day[4] = {0};
		char month[4] = {0};

		// "%3s %3s %d %d:%d:%d %d", or with a colon before the year: "%3s %3s %d %d:%d:%d: %d".
		const auto camera = [&](const bool colon)
		{
			date_text t(r);
			return t.word(day) && t.spaces().word(month) && t.spaces().integer(dd) && t.spaces().integer(hh) &&
				t.is(':') && t.integer(min) && t.is(':') && t.integer(ss) && (!colon || t.is(':')) &&
				t.spaces().integer(yyyy);
		};

		// "%4d<a>%2d<a>%2d<b>%2d<c>%2d<c>%lg", each separator a character, a space or none.
		const auto numeric = [&](const char a, const char b, const char c)
		{
			const auto separator = [](date_text& t, const char s) { return s == 0 || (s == ' ' ? (t.spaces(), true) : t.is(s)); };
			date_text t(r);
			return t.integer(yyyy, 4) && separator(t, a) && t.integer(mm, 2) && separator(t, a) && t.integer(dd, 2) &&
				separator(t, b) && t.integer(hh, 2) && separator(t, c) && t.integer(min, 2) && separator(t, c) &&
				t.real(sec);
		};

		// "%4d-%2d-%2d", or with the month's name: "%4d-%3s-%2d".
		const auto date_only = [&](const bool named)
		{
			date_text t(r);
			return t.integer(yyyy, 4) && t.is('-') && (named ? t.word(month) : t.integer(mm, 2)) && t.is('-') &&
				t.integer(dd, 2);
		};

		if (camera(false) || camera(true))
		{
			mm = core::month(std::bit_cast<const char*>(static_cast<char*>(month)));
			sec = ss;
			success = mm != 0;
		}
		else if (numeric('-', ' ', ':') || // "2006-01-14 15:51:31"
			numeric(':', ' ', ':') || // "2006:01:14 15:51:31"
			numeric('-', 'T', ':') || // "2006-01-14T15:51:31"
			numeric(0, 'T', 0) || // "20060114T155131"
			numeric(0, 0, 0)) // "20060114155131"
		{
			success = true;
		}
		else if (date_only(false)) // "2006-01-14"
		{
			hh = 0, ss = 0, min = 0;
			success = true;
		}
		else if (date_only(true)) // "2006-jan-14"
		{
			mm = core::month(std::bit_cast<const char*>(static_cast<char*>(month)));
			hh = 0, ss = 0, min = 0;
			success = true;
		}
	}

	if (success)
	{
		// %lg accepts "inf", "nan" and overflowing exponents, and converting any of those to int is
		// undefined. A seconds field that is not a real clock value fails the parse instead of
		// reaching the conversion; the date is then unparseable, which is what it is.
		if (!std::isfinite(sec) || sec < 0.0 || sec >= 61.0) return false;

		result.year = yyyy;
		result.month = mm;
		result.day = dd;
		result.hour = hh;
		result.minute = min;
		result.second = static_cast<int>(floor(sec + 0.1));
	}

	return success;
}

bool core::date_t::parse(const std::string_view text)
{
	core::day_t d = {0};

	if (parse_iso_8601_like(text, d))
	{
		date(d);
		return true;
	}

	return false;
}

bool core::date_t::parse_exif_date(const std::string_view r)
{
	// "2006:01:14 15:51:31"
	core::day_t d = {0};
	return parse_iso_8601_like(r, d) && date(d);
}

bool core::date_t::parse_xml_date(const std::string_view r)
{
	// "2006:01:14 15:51:31"
	// "2011-10-03T02:59:13.000000Z"
	// "2019-09-23T18:45:46.309-07:00"
	// "2007-11-09T08:00:00Z"
	// "2006-01-14T15:51:31"
	// "2006-01-14"
	core::day_t d = {0};
	return parse_iso_8601_like(r, d) && date(d);
}

core::file_size core::file_size::null;


uint32_t core::crc32c(const void* data, const size_t len)
{
	return ~crc32c(core::crc32c_init, data, len);
}

uint32_t core::crc32c(const uint32_t crc, const void* data, const size_t len)
{
	if (core::crc32_supported)
	{
		return calc_crc32c_x86(crc, data, len);
	}

	if (core::arm_crc32_supported)
	{
		return calc_crc32c_arm(crc, data, len);
	}

	return calc_crc32c_c(crc, data, len);
}

uint32_t core::crc32c(const std::string_view sv)
{
	return crc32c(sv.data(), sv.size());
}

static constexpr int64_t KB = 1024;
static constexpr int64_t MB = KB * 1024;
static constexpr int64_t GB = MB * 1024;
static constexpr int64_t TB = GB * 1024;

core::size_rounded core::round_size(const uint64_t s)
{
	core::size_rounded result;

	result.unit = "KB";
	result.short_unit = "K";
	result.div = KB;

	if (s > TB / 5)
	{
		result.div = TB;
		result.unit = "TB";
		result.short_unit = "T";
	}
	else if (s > GB / 5)
	{
		result.div = GB;
		result.unit = "GB";
		result.short_unit = "G";
	}
	else if (s > MB / 5)
	{
		result.div = MB;
		result.unit = "MB";
		result.short_unit = "M";
	}

	if (s < KB)
	{
		result.div = KB;
	}

	const auto nn = core::round(s * 10.0 / result.div);
	result.n = nn / 10;
	result.dec = nn % 10;
	result.rounded = core::round(static_cast<double>(s) / result.div);

	return result;
}
