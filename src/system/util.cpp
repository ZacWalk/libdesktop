// This file is part of libdesktop, a C++20 platform layer for desktop applications
// Copyright 2026  Zac Walker
// 
// This program is free software; you can redistribute it and / or modify it
// under the terms of the LGPL License either version 2.1 or later.
// License details are available at https://www.gnu.org/licenses/lgpl-2.1.html
// This program is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY

// Purpose: Core utility functions and data types. Implements file paths, dates, blobs,
// logging, and other foundational types used throughout the application.

#include "libdesktop/system.h"
#include "libdesktop/util.h"
#include "libdesktop/util_simd.h"

namespace
{
	constexpr std::array<uint32_t, 64> sha256_round_constants = {
		0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4,
		0xab1c5ed5, 0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe,
		0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f,
		0x4a7484aa, 0x5cb0a9dc, 0x76f988da, 0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7,
		0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc,
		0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b,
		0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070, 0x19a4c116,
		0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
		0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7,
		0xc67178f2
	};

	constexpr uint32_t rotr(const uint32_t value, const uint32_t amount)
	{
		return (value >> amount) | (value << (32 - amount));
	}

	class sha256_state
	{
		std::array<uint32_t, 8> _state = {
			0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
			0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19
		};
		std::array<uint8_t, 64> _block = {};
		size_t _block_size = 0;
		uint64_t _bytes = 0;

		void transform(const uint8_t* block)
		{
			std::array<uint32_t, 64> words = {};
			for (size_t i = 0; i != 16; ++i)
			{
				const auto offset = i * 4;
				words[i] = static_cast<uint32_t>(block[offset]) << 24 |
				           static_cast<uint32_t>(block[offset + 1]) << 16 |
				           static_cast<uint32_t>(block[offset + 2]) << 8 |
				           block[offset + 3];
			}
			for (size_t i = 16; i != words.size(); ++i)
			{
				const auto s0 = rotr(words[i - 15], 7) ^ rotr(words[i - 15], 18) ^ (words[i - 15] >> 3);
				const auto s1 = rotr(words[i - 2], 17) ^ rotr(words[i - 2], 19) ^ (words[i - 2] >> 10);
				words[i] = words[i - 16] + s0 + words[i - 7] + s1;
			}

			auto a = _state[0];
			auto b = _state[1];
			auto c = _state[2];
			auto d = _state[3];
			auto e = _state[4];
			auto f = _state[5];
			auto g = _state[6];
			auto h = _state[7];
			for (size_t i = 0; i != words.size(); ++i)
			{
				const auto sum1 = rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25);
				const auto choice = (e & f) ^ (~e & g);
				const auto t1 = h + sum1 + choice + sha256_round_constants[i] + words[i];
				const auto sum0 = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22);
				const auto majority = (a & b) ^ (a & c) ^ (b & c);
				const auto t2 = sum0 + majority;
				h = g;
				g = f;
				f = e;
				e = d + t1;
				d = c;
				c = b;
				b = a;
				a = t1 + t2;
			}
			_state[0] += a;
			_state[1] += b;
			_state[2] += c;
			_state[3] += d;
			_state[4] += e;
			_state[5] += f;
			_state[6] += g;
			_state[7] += h;
		}

	public:
		void update(const std::span<const uint8_t> bytes)
		{
			_bytes += bytes.size();
			auto* data = bytes.data();
			auto remaining = bytes.size();
			while (remaining)
			{
				const auto copied = std::min(remaining, _block.size() - _block_size);
				std::memcpy(_block.data() + _block_size, data, copied);
				_block_size += copied;
				data += copied;
				remaining -= copied;
				if (_block_size == _block.size())
				{
					transform(_block.data());
					_block_size = 0;
				}
			}
		}

		platform::sha256_digest finish() const
		{
			auto copy = *this;
			copy._block[copy._block_size++] = 0x80;
			if (copy._block_size > 56)
			{
				std::fill(copy._block.begin() + copy._block_size, copy._block.end(), 0);
				copy.transform(copy._block.data());
				copy._block_size = 0;
			}
			std::fill(copy._block.begin() + copy._block_size, copy._block.begin() + 56, 0);
			const auto bits = copy._bytes * 8;
			for (size_t i = 0; i != 8; ++i)
				copy._block[56 + i] = static_cast<uint8_t>(bits >> (56 - i * 8));
			copy.transform(copy._block.data());

			platform::sha256_digest result = {};
			for (size_t i = 0; i != copy._state.size(); ++i)
			{
				result[i * 4] = static_cast<uint8_t>(copy._state[i] >> 24);
				result[i * 4 + 1] = static_cast<uint8_t>(copy._state[i] >> 16);
				result[i * 4 + 2] = static_cast<uint8_t>(copy._state[i] >> 8);
				result[i * 4 + 3] = static_cast<uint8_t>(copy._state[i]);
			}
			return result;
		}
	};
}


df_assert_pod(df::file_size);
df_assert_pod(df::day_range_t);
df_assert_pod(df::day_t);
df_assert_pod(df::date_t);
df_assert_pod(sizei);
df_assert_pod(pointi);
df_assert_pod(recti);
df_assert_pod(sized);
df_assert_pod(pointd);
df_assert_pod(rectd);
df_assert_pod(affined);
df_assert_pod(quadd);
df_assert_pod(df::xy8);
df_assert_pod(df::xy16);
df_assert_pod(df::xy32);
df_assert_pod(df::int_counter);
df_assert_pod(df::file_path);
df_assert_pod(df::folder_path);
df_assert_pod(std::string_view);

df_assert_pod(df::cspan);
df_assert_pod(df::span);
df_assert_move_only(df::blob);


std::atomic_bool df::is_closing = false;
std::atomic_int df::file_handles_detached = 0;
std::atomic_int df::jobs_running = 0;
std::atomic_int df::loading_media = 0;
std::atomic_int df::command_active = 0;
std::atomic_int df::dragging_items = 0;
std::atomic_int df::handling_crash = 0;
std::atomic<const char*> df::rendering_func = "";

std::string df::gpu_desc = "unknown"s;
std::string df::gpu_id = "unknown"s;
std::string df::d3d_info = "unknown"s;

int df::max_texture_dimension = 16384;
int64_t df::max_texture_bytes = 128ll * 1024ll * 1024ll;
int64_t df::max_decode_bytes = 1024ll * 1024ll * 1024ll;

df::date_t df::start_time;
std::atomic_int df::cancel_token::empty;
std::atomic<df::file_path> df::last_loaded_path;

std::string_view df::app_file_stem()
{
	// ASCII lowering on purpose: the log's name needs this during static initialisation, before any
	// locale is set, and an application name is an identifier rather than text.
	static const std::string stem = []
	{
		std::string result(s_app_name);
		for (auto& c : result)
		{
			if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
		}
		return result;
	}();
	return stem;
}

// Portable and per-user installs keep the log beside the executable. Store packages and
// per-machine installs have a read-only install folder, so those fall back to app data.
static df::folder_path resolve_log_folder()
{
	const auto module_folder = known_path(platform::known_folder::running_app_folder);
	return platform::is_writable(module_folder) ? module_folder : known_path(platform::known_folder::app_data);
}

// Deliberately resolved during static initialisation rather than on first use: the platform calls
// this makes can themselves log, so resolving it from inside df::log would re-enter both the log
// lock (an SRWLOCK, so it would deadlock) and this initialisation.
static const df::folder_path s_log_folder = resolve_log_folder();
df::file_path df::log_path = s_log_folder.combine_file(std::format("{}.log", df::app_file_stem()));
df::file_path df::previous_log_path = s_log_folder.combine_file(std::format("{}.previous.log", df::app_file_stem()));

static platform::mutex log_mutex;
_Guarded_by_(log_mutex) static std::ofstream log_file;
_Guarded_by_(log_mutex) static bool log_truncated = false;

// A repeating per-item failure logs once per file, so a long session over a large collection would
// otherwise grow the log without bound. One rotation is kept, so the worst case on disk is twice this.
constexpr std::streamoff max_log_bytes = 4ll * 1024 * 1024;

namespace
{
	size_t ifind_from(const std::string_view text, const std::string_view sub, const size_t from)
	{
		if (from >= text.size()) return std::string::npos;
		const auto found = str::ifind(text.substr(from), sub);
		return found == std::string_view::npos ? std::string::npos : found + from;
	}

	// The log is attached to support reports and crash uploads, so it must not carry the account
	// name. Everything else about a path is kept - the folder shape is what makes a report
	// reproducible. Deliberately pattern-based rather than a platform::user_name() comparison:
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

void df::log(const std::string_view context, const std::string_view message)
{
	platform::exclusive_lock ll(log_mutex);
	static auto start_time = platform::tick_count();
	static bool did_try_open = false;

#ifdef _DEBUG

	platform::trace(std::format("{}:{}\n", context, message));

#endif

	// Not latched until there is a path to open: resolve_log_folder logs on some failure paths, and
	// consuming the one open attempt on the empty path it sees would disable the log for the session.
	if (!did_try_open && !log_path.is_empty())
	{
		did_try_open = true;

		if (log_path.exists())
		{
			platform::move_file(log_path, previous_log_path, false);
		}

		log_file.open(platform::to_stream_path(log_path), std::ios::out | std::ios::trunc);
	}

	if (log_file.is_open() && (!log_truncated || is_session_summary(context)))
	{
		const auto time = platform::tick_count() - start_time;
		const auto thread_id = platform::current_thread_id();

		log_file << std::right << std::setfill('0') << std::setw(8) << thread_id
			<< " "
			<< std::setw(8) << time
			<< " "
			<< std::setfill(' ')
			// setw is a minimum, so a context longer than the column would otherwise abut the message.
			<< std::left << std::setw(32) << context << ' '
			<< scrub_user_identity(message) << '\n';

		if (!log_truncated && log_file.tellp() >= max_log_bytes)
		{
			log_truncated = true;
			log_file << "*** log truncated at " << max_log_bytes
				<< " bytes - only end-of-session summaries follow ***\n";
		}
	}
}

void df::trace(const std::string_view message)
{
#ifdef _DEBUG
	platform::trace(std::format("{}\n", message));
#endif
}

// One malformed batch repeats the same codec message for every item scanned. The distinct-message
// ceiling bounds the other direction, where a library emits a message that varies every time.
void df::log_once(const std::string_view context, const std::string_view message)
{
	constexpr size_t max_distinct = 128;

	static platform::mutex mutex;
	_Guarded_by_(mutex) static hash_set<std::string> seen;

	{
		platform::exclusive_lock lock(mutex);
		if (seen.size() >= max_distinct) return;
		if (!seen.emplace(std::format("{}|{}", context, message)).second) return;
	}

	log(context, message);
}

df::file_path df::close_log()
{
	platform::exclusive_lock ll(log_mutex);

	if (log_file.is_open())
	{
		log_file.close();
	}

	return log_path;
}

std::string df::file_size::str() const
{
	return format_size(*this);
}

df::version::version(const std::string_view version)
{
	const auto parts = str::split(version, true, [](const char c) { return c == '.'; });

	if (!parts.empty())
	{
		major = str::to_int(parts[0]);
	}

	if (parts.size() > 1)
	{
		minor = str::to_int(parts[1]);
	}
}

std::string df::url_extract(const std::string_view text)
{
	static const std::regex url_regex(R"((https?:\/\/[^\s\"\']+))");
	std::match_results<std::string_view::const_iterator> match;

	if (std::regex_search(text.begin(), text.end(), match, url_regex))
	{
		return match[1].str();
	}

	return {};
}

std::vector<std::string> df::url_extract_all(const std::string_view text)
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

std::string df::date_t::to_xmp_date() const
{
	const auto st = date();
	return std::format("{:04}-{:02}-{:02}T{:02}:{:02}:{:02}", st.year, st.month, st.day, st.hour, st.minute,
	                   st.second);
}

df::date_t df::date_t::system_to_local() const
{
	return date_t(platform::utc_to_local(_i));
}

df::date_t df::date_t::local_to_system() const
{
	return date_t(platform::local_to_utc(_i));
}

df::date_t df::date_t::from(const std::string_view r)
{
	date_t ft;
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

static bool parse_iso_8601_like(const std::string_view r, df::day_t& result)
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
			mm = str::month(std::bit_cast<const char*>(static_cast<char*>(month)));
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
			mm = str::month(std::bit_cast<const char*>(static_cast<char*>(month)));
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

bool df::date_t::parse(const std::string_view text)
{
	day_t d = {0};

	if (parse_iso_8601_like(text, d))
	{
		date(d);
		return true;
	}

	return false;
}

bool df::date_t::parse_exif_date(const std::string_view r)
{
	// "2006:01:14 15:51:31"
	day_t d = {0};
	return parse_iso_8601_like(r, d) && date(d);
}

bool df::date_t::parse_xml_date(const std::string_view r)
{
	// "2006:01:14 15:51:31"
	// "2011-10-03T02:59:13.000000Z"
	// "2019-09-23T18:45:46.309-07:00"
	// "2007-11-09T08:00:00Z"
	// "2006-01-14T15:51:31"
	// "2006-01-14"
	day_t d = {0};
	return parse_iso_8601_like(r, d) && date(d);
}

df::file_size df::file_size::null;


df::file_path df::probe_data_file(const std::string_view file_name)
{
	const auto module_folder = known_path(platform::known_folder::running_app_folder);
	const auto app_data_folder = known_path(platform::known_folder::app_data);
	const auto module_path = module_folder.combine_file(file_name);

	if (module_path.exists())
	{
		return module_path;
	}

	return app_data_folder.combine_file(file_name);
}

df::blob df::blob_from_file(const file_path path)
{
	file f;

	if (f.open_read(path, true))
	{
		const auto file_len = f.file_size();

		// Refused, not clamped. Returning the first max_blob_size bytes of a larger file answers
		// with something that looks like the file and is not, which every caller then treats as
		// whole - the edit path re-encodes it over the original.
		if (file_len > max_blob_size)
		{
			const auto message = std::format("Cannot read file into memory ({} bytes)", file_len);
			df::log(__FUNCTION__, message);
			throw app_exception(message);
		}

		return f.read_blob(static_cast<size_t>(file_len));
	}

	return {};
}

df::blob df::blob_head_from_file(const file_path path, const size_t max_load)
{
	file f;

	if (f.open_read(path, true))
	{
		const auto file_len = f.file_size();
		const auto load_len = max_load != 0 && file_len > max_load ? max_load : file_len;

		if (load_len > max_blob_size)
		{
			const auto message = std::format("Cannot read file into memory ({} bytes)", file_len);
			df::log(__FUNCTION__, message);
			throw app_exception(message);
		}

		return f.read_blob(static_cast<size_t>(load_len));
	}

	return {};
}

bool df::blob_save_to_file(const cspan data, const file_path path)
{
	// Staged beside the destination and moved into place, because writing
	// straight to the destination truncates whatever is there before the first byte is written, so
	// a short write or a full disk would leave a partial file where a whole one used to be.
	const auto staged = platform::temp_file(path.extension(), path.folder());
	auto file = open_file(staged, platform::file_open_mode::create);

	if (!file)
	{
		return false;
	}

	const auto len = data.size;

	if (len > 0 && static_cast<size_t>(file->write(data.data, len)) != len)
	{
		file.reset();
		platform::delete_file(staged);
		return false;
	}

	file.reset();

	if (platform::replace_file(path, staged).failed())
	{
		platform::delete_file(staged);
		return false;
	}

	return true;
}

std::optional<platform::sha256_digest> platform::file_sha256(const df::file_path path,
                                                             const df::cancel_token& token)
{
	return file_sha256(open_file_lock(path, file_lock_mode::read), token);
}

std::optional<platform::sha256_digest> platform::file_sha256(const file_ptr& file,
                                                             const df::cancel_token& token)
{
	if (!file) return {};

	const auto expected = file->size();
	std::array<uint8_t, 64 * 1024> buffer = {};
	sha256_state hash;
	uint64_t total = 0;
	while (total < expected)
	{
		if (token.is_cancelled()) return {};
		const auto wanted = std::min<uint64_t>(expected - total, buffer.size());
		const auto read = file->read_at(total, buffer.data(), wanted);
		if (read == 0 || read > wanted) return {};
		hash.update({buffer.data(), static_cast<size_t>(read)});
		total += read;
	}

	if (total != expected || file->size() != expected || token.is_cancelled()) return {};
	return hash.finish();
}

uint32_t df::crc32c(const void* data, const size_t len)
{
	return ~crc32c(crc32c_init, data, len);
}

uint32_t df::crc32c(const uint32_t crc, const void* data, const size_t len)
{
	if (platform::crc32_supported)
	{
		return calc_crc32c_x86(crc, data, len);
	}

	if (platform::arm_crc32_supported)
	{
		return calc_crc32c_arm(crc, data, len);
	}

	return calc_crc32c_c(crc, data, len);
}

uint32_t df::crc32c(const std::string_view sv)
{
	return crc32c(sv.data(), sv.size());
}

static constexpr int64_t KB = 1024;
static constexpr int64_t MB = KB * 1024;
static constexpr int64_t GB = MB * 1024;
static constexpr int64_t TB = GB * 1024;

df::size_rounded df::round_size(const uint64_t s)
{
	size_rounded result;

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

	const auto nn = df::round(s * 10.0 / result.div);
	result.n = nn / 10;
	result.dec = nn % 10;
	result.rounded = df::round(static_cast<double>(s) / result.div);

	return result;
}

std::string df::format_size(const df::file_size& s)
{
	const auto rounded = round_size(s.to_int64());
	return rounded.dec == 0
		       ? std::format("{} {}", rounded.n, rounded.unit)
		       : std::format("{}{}{} {}", rounded.n, platform::number_dec_sep(), rounded.dec, rounded.unit);
}
