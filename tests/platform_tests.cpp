// Unit tests for the parts of the platform layer that can be checked without a
// window or a message loop: text conversion, paths, geometry, embedded
// resources, and the handful of backend helpers with deterministic answers.

#include "platform.h"

#include <cstdio>
#include <string>
#include <vector>

namespace
{
	int g_checks = 0;
	int g_failures = 0;

	void check(const bool ok, const char* expr, const int line)
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

	void check_eq_str(const std::string_view actual, const std::string_view expected,
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

#define CHECK(expr)          check((expr), #expr, __LINE__)
#define CHECK_EQ(a, b)       check_eq((a), (b), #a " == " #b, __LINE__)
#define CHECK_STR(a, b)      check_eq_str((a), (b), #a " == " #b, __LINE__)

	// "aé€𐍈" — one byte, two bytes, three bytes, and a surrogate pair.
	constexpr std::string_view mixed = "a\xC3\xA9\xE2\x82\xAC\xF0\x90\x8D\x88";

	void test_text()
	{
		CHECK_EQ(pf::utf8_truncate(mixed, 0), 0u);
		CHECK_EQ(pf::utf8_truncate(mixed, 1), 1u);
		CHECK_EQ(pf::utf8_truncate(mixed, 2), 3u);
		CHECK_EQ(pf::utf8_truncate(mixed, 3), 6u);
		CHECK_EQ(pf::utf8_truncate(mixed, 99), mixed.size());

		// Never splits a code point.
		CHECK_EQ(pf::utf8_next(mixed, 1), 3);
		CHECK_EQ(pf::utf8_next(mixed, 6), 10);
		CHECK_EQ(pf::utf8_prev(mixed, 10), 6);
		CHECK_EQ(pf::utf8_prev(mixed, 0), 0);

		// The astral code point must survive as a surrogate pair.
		const auto wide = pf::utf8_to_utf16(mixed);
		CHECK_EQ(wide.size(), 5u);
		CHECK(pf::is_lead_surrogate(wide[3]));
		CHECK(pf::is_trail_surrogate(wide[4]));
		CHECK_STR(pf::utf16_to_utf8(wide), mixed);

		CHECK_STR(pf::to_lower("MiXeD Case"), "mixed case");
		CHECK_STR(pf::to_upper("MiXeD Case"), "MIXED CASE");

		CHECK_EQ(pf::icmp("abc", "ABC"), 0);
		CHECK(pf::icmp("abc", "abd") < 0);
		CHECK(pf::icmp("abd", "abc") > 0);
		CHECK(pf::icmp("abc", "abcd") < 0);
		CHECK_EQ(pf::icmp("", ""), 0);

		CHECK_STR(pf::unquote("\"quoted\""), "quoted");
		CHECK_STR(pf::unquote("'quoted'"), "quoted");
		CHECK_STR(pf::unquote("bare"), "bare");

		CHECK_STR(pf::utf8_encode(U'a'), "a");
		CHECK_STR(pf::utf8_encode(U'\u20ac'), "\xE2\x82\xAC");
		CHECK_STR(pf::utf8_encode(U'\U00010348'), "\xF0\x90\x8D\x88");
	}

	// NTFS names and clipboard text can carry lone surrogates; converting them
	// must substitute U+FFFD rather than throw.
	void test_invalid_text()
	{
		constexpr std::string_view replacement = "\xEF\xBF\xBD";

		const std::wstring lone_lead{static_cast<wchar_t>(0xD800), L'x'};
		CHECK_STR(pf::utf16_to_utf8(lone_lead), std::string(replacement) + "x");

		const std::wstring lone_trail{static_cast<wchar_t>(0xDC00)};
		CHECK_STR(pf::utf16_to_utf8(lone_trail), replacement);

		const std::wstring trailing_lead{L'x', static_cast<wchar_t>(0xD800)};
		CHECK_STR(pf::utf16_to_utf8(trailing_lead), "x" + std::string(replacement));

		// A surrogate half encoded in UTF-8 has no UTF-16 form either.
		CHECK_EQ(pf::utf8_to_utf16("\xED\xA0\x80").size(), 1u);
		CHECK_EQ(static_cast<uint32_t>(pf::utf8_to_utf16("\xED\xA0\x80")[0]), pf::REPLACEMENT_CHAR);
	}

	void test_file_path()
	{
		// Separators are normalised and a trailing one is dropped.
		CHECK_STR(pf::file_path("c:/dir/sub/").view(), "c:\\dir\\sub");
		CHECK_STR(pf::file_path("c:\\").view(), "c:\\");

		const pf::file_path p("c:\\dir\\file.txt");
		CHECK_STR(p.name(), "file.txt");
		CHECK_STR(p.extension(), ".txt");
		CHECK_STR(p.without_extension(), "c:\\dir\\file");
		CHECK_STR(p.folder().view(), "c:\\dir");

		// A dot in a folder name is not an extension.
		CHECK_STR(pf::file_path("c:\\dir.v2\\file").extension(), "");

		CHECK_STR(pf::file_path("c:\\dir").combine("file.txt").view(), "c:\\dir\\file.txt");
		CHECK_STR(pf::file_path("c:\\dir").combine("app", "ini").view(), "c:\\dir\\app.ini");

		// Combining onto an empty path must not produce a leading separator.
		CHECK_STR(pf::file_path().combine("file.txt").view(), "file.txt");

		CHECK(pf::file_path("c:\\A\\B.TXT") == pf::file_path("c:\\a\\b.txt"));
		CHECK(pf::file_path().empty());
	}

	void test_geometry()
	{
		const pf::irect r(10, 20, 110, 220);
		CHECK_EQ(r.width(), 100);
		CHECK_EQ(r.height(), 200);
		CHECK_EQ(r.offset(5, -5).left, 15);
		CHECK_EQ(r.inflate(5).width(), 110);
		CHECK(r.contains(pf::ipoint(10, 20)));
		CHECK(!r.contains(pf::ipoint(9, 20)));
		CHECK(r.intersects(pf::irect(100, 210, 200, 300)));
		CHECK(!r.intersects(pf::irect(110, 220, 200, 300)));

		constexpr pf::color_t c(10, 250, 128);
		CHECK(c.lighten(10) == pf::color_t(20, 255, 138));  // saturates, no wrap
		CHECK(c.darken(20) == pf::color_t(0, 230, 108));
		CHECK_EQ(pf::color_t(1, 2, 3).rgb(), 0x030201u);
		CHECK_EQ(pf::clamp(5, 10, 1), 10);
	}

	void test_embedded_resources()
	{
		const auto css = pf::embedded_resource_text("sample.css");
		CHECK(!css.empty());
		CHECK(css.find("color: red") != std::string_view::npos);

		// The generated table appends a NUL that the size does not count.
		CHECK_EQ(css.data()[css.size()], '\0');
		CHECK_EQ(pf::embedded_resource_data("sample.css").size(), css.size());

		CHECK(pf::embedded_resource_text("no-such-file").empty());
		CHECK(pf::embedded_resource_data("no-such-file").empty());
	}

	// A one-second 8-bit mono ramp, in the RIFF layout create_sound_buffer parses.
	std::vector<uint8_t> make_wav(const uint32_t sample_rate, const uint32_t frames)
	{
		const auto put32 = [](std::vector<uint8_t>& out, const uint32_t v)
		{
			out.push_back(static_cast<uint8_t>(v));
			out.push_back(static_cast<uint8_t>(v >> 8));
			out.push_back(static_cast<uint8_t>(v >> 16));
			out.push_back(static_cast<uint8_t>(v >> 24));
		};
		const auto put16 = [](std::vector<uint8_t>& out, const uint16_t v)
		{
			out.push_back(static_cast<uint8_t>(v));
			out.push_back(static_cast<uint8_t>(v >> 8));
		};
		const auto tag = [](std::vector<uint8_t>& out, const char* s)
		{
			for (int i = 0; i < 4; ++i) out.push_back(static_cast<uint8_t>(s[i]));
		};

		std::vector<uint8_t> wav;
		tag(wav, "RIFF");
		put32(wav, 4 + 8 + 16 + 8 + frames);
		tag(wav, "WAVE");
		tag(wav, "fmt ");
		put32(wav, 16);
		put16(wav, 1); // PCM
		put16(wav, 1); // mono
		put32(wav, sample_rate);
		put32(wav, sample_rate); // bytes per second
		put16(wav, 1); // block align
		put16(wav, 8); // bits per sample
		tag(wav, "data");
		put32(wav, frames);
		for (uint32_t i = 0; i < frames; ++i)
			wav.push_back(static_cast<uint8_t>(i & 0xff));

		return wav;
	}

	// The queue is the app's clock, so what matters is that it fills, refuses
	// an over-write, and drains again on its own.
	void test_audio_stream()
	{
		constexpr int block_frames = 735; // one 60 Hz frame at 44.1 kHz
		constexpr int max_blocks = 3;

		const auto stream = pf::create_audio_stream(44100, 1, max_blocks);
		CHECK(stream != nullptr);
		if (!stream) return;

		stream->set_volume(0.0f);
		CHECK_EQ(stream->queued_blocks(), 0);
		CHECK(stream->can_write());
		CHECK(!stream->write({})); // nothing to queue is not a write

		const std::vector<int16_t> silence(block_frames, 0);
		for (int i = 0; i < max_blocks; ++i)
			CHECK(stream->write(silence));

		CHECK(!stream->can_write());
		CHECK(!stream->write(silence));

		// Three 16.7 ms blocks cannot outlast this, so room must reappear.
		pf::platform_sleep(200);
		CHECK(stream->can_write());

		stream->stop();
		CHECK_EQ(stream->queued_blocks(), 0);
	}

	// XAudio2 needs no window, so playback is testable headlessly. Everything
	// runs at zero volume: a test suite must not make a noise.
	void test_audio()
	{
		if (!pf::sound_init())
		{
			std::printf("audio: no device, skipping playback tests\n");
			return;
		}

		CHECK(pf::create_sound_buffer({}) == nullptr);

		// A RIFF header with the chunks cut off must be rejected, not trusted.
		const auto truncated = make_wav(11025, 8);
		CHECK(pf::create_sound_buffer(std::span<const uint8_t>(truncated).subspan(0, 20)) == nullptr);

		const auto wav = make_wav(11025, 11025);
		const auto sound = pf::create_sound_buffer(wav);
		CHECK(sound != nullptr);

		if (sound)
		{
			sound->set_volume(0.0f);
			sound->set_pan(0.0f);
			CHECK_EQ(sound->play_position().value_or(1), 0u);

			sound->play(true);
			pf::platform_sleep(120);
			const auto advanced = sound->play_position().value_or(0);
			CHECK(advanced > 0);

			// Seeking a stopped buffer decides where the next play() begins.
			sound->stop();
			sound->set_play_position(4000);
			CHECK_EQ(sound->play_position().value_or(0), 4000u);

			// Pitching up beyond the default 2.0 ratio cap must still work.
			sound->set_frequency(28604);
			sound->play(false);
			pf::platform_sleep(60);
			sound->stop();
		}

		test_audio_stream();

		pf::sound_shutdown();
	}

	void test_backend_helpers()
	{
		CHECK_STR(pf::url_encode("a b&c=d"), "a%20b%26c%3Dd");

		CHECK_STR(pf::format_key_binding({}), "");
		CHECK_STR(pf::format_key_binding({'S', pf::key_mod::ctrl}), "Ctrl+S");
		CHECK_STR(pf::format_key_binding({'F', pf::key_mod::ctrl | pf::key_mod::shift}), "Ctrl+Shift+F");

		CHECK_STR(pf::resolve_url("https://example.com/a/b.html", "c.css"),
		          "https://example.com/a/c.css");
		CHECK_STR(pf::resolve_url("https://example.com/a/b.html", "/root.css"),
		          "https://example.com/root.css");
		CHECK_STR(pf::resolve_url("https://example.com/a/b.html", "https://other.com/x.css"),
		          "https://other.com/x.css");
	}

	std::vector<std::string> split_stream(const std::vector<std::string_view>& chunks,
	                                      const size_t max_line_bytes = pf::line_splitter::default_max_line_bytes)
	{
		pf::line_splitter splitter;
		splitter.max_line_bytes = max_line_bytes;

		std::vector<std::string> lines;
		const auto emit = [&lines](const std::string_view line) { lines.emplace_back(line); };

		for (const auto& chunk : chunks)
			splitter.feed(chunk, emit);

		splitter.flush(emit);
		return lines;
	}

	// A pipe read boundary can fall anywhere, including inside a multi-byte character.
	void test_line_splitter()
	{
		const auto split = split_stream({"on", "e\ntw", "o\nthr", "ee\n"});
		CHECK_EQ(split.size(), 3u);
		CHECK_STR(split[0], "one");
		CHECK_STR(split[1], "two");
		CHECK_STR(split[2], "three");

		const auto by_byte = split_stream({"a", "b", "\n", "c", "\n"});
		CHECK_EQ(by_byte.size(), 2u);
		CHECK_STR(by_byte[0], "ab");

		const auto utf8 = split_stream({"caf\xC3", "\xA9\n"});
		CHECK_EQ(utf8.size(), 1u);
		CHECK_STR(utf8[0], "caf\xC3\xA9");

		const auto crlf = split_stream({"one\r\ntwo\r\n"});
		CHECK_EQ(crlf.size(), 2u);
		CHECK_STR(crlf[0], "one");

		const auto blanks = split_stream({"\n\na\n"});
		CHECK_EQ(blanks.size(), 3u);
		CHECK_STR(blanks[0], "");
		CHECK_STR(blanks[2], "a");

		// A stream that ends without a newline still yields its last record.
		const auto partial = split_stream({"tail"});
		CHECK_EQ(partial.size(), 1u);
		CHECK_STR(partial[0], "tail");

		CHECK_EQ(split_stream({""}).size(), 0u);

		// A stream that never sends a newline must not grow the buffer without bound.
		const std::string huge(64, 'x');
		const auto dropped = split_stream({huge, huge, "\nafter\n"}, 32);
		CHECK_EQ(dropped.size(), 1u);
		CHECK_STR(dropped[0], "after");

		// The record is dropped whole, never truncated into a partial one.
		const auto joined = split_stream({"12345678\nshort\n"}, 6);
		CHECK_EQ(joined.size(), 1u);
		CHECK_STR(joined[0], "short");
	}

	void test_child_process_arguments()
	{
		CHECK_STR(pf::quote_command_arg("simple"), "simple");
		CHECK_STR(pf::quote_command_arg(""), "\"\"");
		CHECK_STR(pf::quote_command_arg("a b"), "\"a b\"");
		CHECK_STR(pf::quote_command_arg("a\"b"), "\"a\\\"b\"");
		CHECK_STR(pf::quote_command_arg("C:\\path\\file"), "C:\\path\\file");
		CHECK_STR(pf::quote_command_arg("C:\\my path\\"), "\"C:\\my path\\\\\"");
		CHECK_STR(pf::quote_command_arg("a\\\"b"), "\"a\\\\\\\"b\"");

		CHECK(!pf::has_shell_metacharacter("--acp"));
		CHECK(!pf::has_shell_metacharacter("C:\\path\\file.txt"));
		CHECK(pf::has_shell_metacharacter("a & b"));
		CHECK(pf::has_shell_metacharacter("a | b"));
		CHECK(pf::has_shell_metacharacter("%PATH%"));
		CHECK(pf::has_shell_metacharacter("a > b"));
		CHECK(pf::has_shell_metacharacter("a\nb"));
	}

	// Neither '..' nor a differently-cased prefix may be used to escape the root.
	void test_path_containment()
	{
		const pf::file_path root("c:\\root\\folder");

		CHECK(pf::is_path_within(root, root));
		CHECK(pf::is_path_within(root, pf::file_path("c:\\root\\folder\\a\\b.txt")));
		CHECK(pf::is_path_within(root, pf::file_path("C:\\ROOT\\FOLDER\\a.txt")));
		CHECK(pf::is_path_within(root, pf::file_path("c:\\root\\folder\\a\\..\\b.txt")));

		CHECK(!pf::is_path_within(root, pf::file_path("c:\\root\\folder2\\a.txt")));
		CHECK(!pf::is_path_within(root, pf::file_path("c:\\root\\folder\\..\\other\\a.txt")));
		CHECK(!pf::is_path_within(root, pf::file_path("c:\\root")));
		CHECK(!pf::is_path_within(root, {}));
		CHECK(!pf::is_path_within({}, pf::file_path("c:\\root\\folder\\a.txt")));
	}
}

// The backend's WinMain references these; a console test never calls them.
app_init_result app_init(const pf::window_frame_ptr&, std::span<const std::string_view>)
{
	return {false, 0};
}

void app_idle()
{
}

void app_destroy()
{
}

int main()
{
	test_text();
	test_invalid_text();
	test_file_path();
	test_geometry();
	test_embedded_resources();
	test_audio();
	test_backend_helpers();
	test_line_splitter();
	test_child_process_arguments();
	test_path_containment();

	std::printf("platform tests: %s (%d checks, %d failures)\n",
	            g_failures == 0 ? "PASS" : "FAIL", g_checks, g_failures);
	return g_failures == 0 ? 0 : 1;
}
