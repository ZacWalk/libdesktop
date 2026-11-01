// Unit tests for the parts of the platform layer that can be checked without a
// window or a message loop: text conversion, paths, geometry, embedded
// resources, and the handful of backend helpers with deterministic answers.

#include "platform.h"
#include "platform_audio.h"
#include "loopback_http.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <format>
#include <future>
#include <span>
#include <string>
#include <thread>
#include <vector>

namespace
{
	int g_checks = 0;
	int g_failures = 0;
	std::atomic<int> g_shutdown_completed = 0;
	std::promise<void> g_shutdown_release;
	const std::shared_future<void> g_shutdown_gate = g_shutdown_release.get_future().share();
	constexpr int shutdown_job_count = 16;

	void check_pool_teardown()
	{
		const bool drained = g_shutdown_completed == shutdown_job_count;
		std::printf("headless async teardown: %s (%d/%d jobs)\n",
			drained ? "PASS" : "FAIL", g_shutdown_completed.load(), shutdown_job_count);
		if (!drained)
		{
			std::fflush(stdout);
			std::_Exit(1);
		}
	}

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

	void test_instance_lock()
	{
		const auto name = std::format("platform-tests-{}",
			std::chrono::steady_clock::now().time_since_epoch().count());
		{
			auto first = pf::try_lock_instance(name);
			CHECK(first.lock != nullptr);
			CHECK(!first.already_running);
			CHECK(first.error.empty());

			const auto duplicate = pf::try_lock_instance(name);
			CHECK(duplicate.lock == nullptr);
			CHECK(duplicate.already_running);
			CHECK(duplicate.error.empty());

			pf::instance_lock_result concurrent;
			std::thread contender([&] { concurrent = pf::try_lock_instance(name); });
			contender.join();
			CHECK(concurrent.lock == nullptr);
			CHECK(concurrent.already_running);
			CHECK(concurrent.error.empty());

			auto different = pf::try_lock_instance(name + "-other");
			CHECK(different.lock != nullptr);
			CHECK(!different.already_running);
			CHECK(different.error.empty());
		}

		auto reacquired = pf::try_lock_instance(name);
		CHECK(reacquired.lock != nullptr);
		CHECK(!reacquired.already_running);
		CHECK(reacquired.error.empty());
		std::thread releaser([lock = std::move(reacquired.lock)]() mutable { lock.reset(); });
		releaser.join();
		const auto after_release = pf::try_lock_instance(name);
		CHECK(after_release.lock != nullptr);
		CHECK(!after_release.already_running);
		CHECK(after_release.error.empty());

		for (const std::string invalid : {"", "a b", "a\\b", "a/b", "\xc3\xa9"})
		{
			const auto result = pf::try_lock_instance(invalid);
			CHECK(result.lock == nullptr);
			CHECK(!result.already_running);
			CHECK(!result.error.empty());
		}
		for (const auto& invalid : {std::string(129, 'x'), std::string("a\0b", 3)})
		{
			const auto result = pf::try_lock_instance(invalid);
			CHECK(result.lock == nullptr);
			CHECK(!result.already_running);
			CHECK(!result.error.empty());
		}
	}

	void test_local_app_data_path()
	{
		const auto path = pf::local_app_data_path();
		CHECK(!path.empty());
		CHECK(pf::is_directory(path));
		CHECK(path == pf::canonical_path(path));
		CHECK(path.view().find('\0') == std::string_view::npos);
		CHECK(path == pf::local_app_data_path());
	}

	void test_serial_executor()
	{
		auto executor = pf::create_serial_executor();
		CHECK(executor != nullptr);
		if (!executor) return;
		CHECK(!executor->post({}));
		std::vector<int> order;
		for (int i = 0; i < 32; ++i)
			CHECK(executor->post([&, i] { order.push_back(i); }));
		CHECK(executor->post([] { throw std::runtime_error("expected serial task failure"); }));
		CHECK(executor->post([&] { order.push_back(32); }));
		executor->stop();
		CHECK_EQ(order.size(), 33u);
		for (size_t i = 0; i < order.size(); ++i)
			CHECK_EQ(order[i], static_cast<int>(i));
		CHECK(!executor->post([] {}));
		executor->stop();

		int drained = 0;
		{
			auto scoped = pf::create_serial_executor();
			CHECK(scoped != nullptr);
			if (scoped)
				for (int i = 0; i < 8; ++i)
					CHECK(scoped->post([&] { ++drained; }));
		}
		CHECK_EQ(drained, 8);

		auto self_stopping = pf::create_serial_executor();
		CHECK(self_stopping != nullptr);
		if (!self_stopping) return;
		std::promise<void> release;
		const auto gate = release.get_future().share();
		bool tail_ran = false;
		CHECK(self_stopping->post([&] { gate.wait(); self_stopping->stop(); }));
		CHECK(self_stopping->post([&] { tail_ran = true; }));
		release.set_value();
		// Joining concurrently with a worker's stop must not hold a lock that worker needs.
		self_stopping->stop();
		CHECK(tail_ran);

		auto owner = std::make_shared<pf::serial_executor_ptr>(pf::create_serial_executor());
		CHECK(*owner != nullptr);
		if (!*owner) return;
		std::promise<void> destroy;
		const auto destroy_gate = destroy.get_future().share();
		auto completed = std::make_shared<std::promise<void>>();
		auto completion = completed->get_future();
		CHECK((*owner)->post([owner, destroy_gate] { destroy_gate.wait(); owner->reset(); }));
		CHECK((*owner)->post([completed] { completed->set_value(); }));
		destroy.set_value();
		CHECK(completion.wait_for(std::chrono::seconds(5)) == std::future_status::ready);
	}

	void test_headless_async_pool()
	{
		auto release = std::make_shared<std::promise<void>>();
		const auto gate = release->get_future().share();
		std::vector<std::future<void>> started;
		std::vector<std::future<void>> finished;
		for (int i = 0; i < 4; ++i)
		{
			auto start = std::make_shared<std::promise<void>>();
			auto finish = std::make_shared<std::promise<void>>();
			started.push_back(start->get_future());
			finished.push_back(finish->get_future());
			pf::run_async([start, finish, gate]
			{
				start->set_value();
				gate.wait();
				finish->set_value();
			});
		}
		// No window, app_init, or platform_run has run in this console executable.
		for (auto& future : started)
			CHECK(future.wait_for(std::chrono::seconds(5)) == std::future_status::ready);
		auto fifth = std::make_shared<std::promise<void>>();
		auto fifth_done = fifth->get_future();
		pf::run_async([fifth] { fifth->set_value(); });
		CHECK(fifth_done.wait_for(std::chrono::milliseconds(50)) == std::future_status::timeout);
		release->set_value();
		for (auto& future : finished)
			CHECK(future.wait_for(std::chrono::seconds(5)) == std::future_status::ready);
		CHECK(fifth_done.wait_for(std::chrono::seconds(5)) == std::future_status::ready);

		pf::run_async([] { throw std::runtime_error("expected async task failure"); });
		auto ui_thread = std::make_shared<std::promise<std::thread::id>>();
		auto ui_done = ui_thread->get_future();
		pf::run_async([ui_thread]
		{
			pf::run_ui([ui_thread] { ui_thread->set_value(std::this_thread::get_id()); });
		});
		const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
		while (ui_done.wait_for(std::chrono::seconds(0)) != std::future_status::ready &&
			std::chrono::steady_clock::now() < deadline)
			pf::pump_ui_tasks(25);
		const bool delivered = ui_done.wait_for(std::chrono::seconds(0)) == std::future_status::ready;
		CHECK(delivered);
		if (delivered) CHECK(ui_done.get() == std::this_thread::get_id());
	}

	void test_pool_waiting_on_serial_executor()
	{
		auto writer = std::shared_ptr<pf::serial_executor>(pf::create_serial_executor());
		CHECK(writer != nullptr);
		if (!writer) return;
		auto release = std::make_shared<std::promise<void>>();
		const auto gate = release->get_future().share();
		std::vector<std::future<void>> started;
		std::vector<std::future<bool>> completed;
		for (int i = 0; i < 4; ++i)
		{
			auto start = std::make_shared<std::promise<void>>();
			auto done = std::make_shared<std::promise<bool>>();
			started.push_back(start->get_future());
			completed.push_back(done->get_future());
			pf::run_async([writer, start, done, gate]
			{
				start->set_value();
				gate.wait();
				auto written = std::make_shared<std::promise<void>>();
				auto result = written->get_future();
				const bool posted = writer->post([written] { written->set_value(); });
				done->set_value(posted &&
					result.wait_for(std::chrono::seconds(5)) == std::future_status::ready);
			});
		}
		for (auto& future : started)
			CHECK(future.wait_for(std::chrono::seconds(5)) == std::future_status::ready);
		release->set_value();
		for (auto& future : completed)
		{
			const bool ready = future.wait_for(std::chrono::seconds(10)) == std::future_status::ready;
			CHECK(ready);
			if (ready) CHECK(future.get());
		}
		writer->stop();
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

	// Any WAV the reader is asked about: a format, its data, and the chunks around them.
	struct wav_spec
	{
		uint16_t tag = 1; // PCM
		uint16_t channels = 1;
		uint32_t rate = 11025;
		uint16_t bits = 8;
		bool extensible = false;
		uint16_t sub_tag = 1; // the extensible sub-format's tag
		bool bad_sub_format = false;
		bool odd_chunk_first = false; // a padded three-byte chunk before the others
		bool data_first = false;
	};

	std::vector<uint8_t> build_wav(const wav_spec& spec, const std::vector<uint8_t>& data)
	{
		const auto put16 = [](std::vector<uint8_t>& out, const uint32_t v)
		{
			out.push_back(static_cast<uint8_t>(v));
			out.push_back(static_cast<uint8_t>(v >> 8));
		};
		const auto put32 = [&](std::vector<uint8_t>& out, const uint32_t v)
		{
			put16(out, v & 0xFFFF);
			put16(out, v >> 16);
		};
		const auto tag = [](std::vector<uint8_t>& out, const char* s)
		{
			for (int i = 0; i < 4; ++i) out.push_back(static_cast<uint8_t>(s[i]));
		};

		std::vector<uint8_t> body;
		const auto add_chunk = [&](const char* id, const std::vector<uint8_t>& bytes)
		{
			tag(body, id);
			put32(body, static_cast<uint32_t>(bytes.size()));
			body.insert(body.end(), bytes.begin(), bytes.end());
			if (bytes.size() & 1) body.push_back(0);
		};

		const auto align = static_cast<uint32_t>(spec.channels * spec.bits / 8);
		std::vector<uint8_t> fmt;
		put16(fmt, spec.extensible ? 0xFFFE : spec.tag);
		put16(fmt, spec.channels);
		put32(fmt, spec.rate);
		put32(fmt, spec.rate * align);
		put16(fmt, align);
		put16(fmt, spec.bits);

		if (spec.extensible)
		{
			put16(fmt, 22); // cbSize
			put16(fmt, spec.bits); // valid bits
			put32(fmt, spec.channels == 1 ? 0x4 : 0x3); // front centre, or front left and right
			put32(fmt, spec.sub_tag); // the GUID's first field is the tag it stands for
			put16(fmt, 0x0000);
			put16(fmt, 0x0010);
			const uint8_t rest[] = {0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, spec.bad_sub_format ? uint8_t{0x72} : uint8_t{0x71}};
			fmt.insert(fmt.end(), std::begin(rest), std::end(rest));
		}

		if (spec.odd_chunk_first) add_chunk("LIST", {1, 2, 3});

		if (spec.data_first)
		{
			add_chunk("data", data);
			add_chunk("fmt ", fmt);
		}
		else
		{
			add_chunk("fmt ", fmt);
			add_chunk("data", data);
		}

		std::vector<uint8_t> wav;
		tag(wav, "RIFF");
		put32(wav, static_cast<uint32_t>(4 + body.size()));
		tag(wav, "WAVE");
		wav.insert(wav.end(), body.begin(), body.end());
		return wav;
	}

	std::vector<uint8_t> float_bytes(const float value)
	{
		std::vector<uint8_t> bytes(sizeof(value));
		std::memcpy(bytes.data(), &value, sizeof(value));
		return bytes;
	}

	bool near_sample(const float actual, const float expected)
	{
		return std::fabs(actual - expected) < 1e-5f;
	}

	// One render of the mix as float frames, as an endpoint of that many channels at that rate takes it.
	std::vector<float> render_floats(pf::audio::mixer& mixer, const uint32_t frames, const uint32_t channels,
	                                 const uint32_t rate)
	{
		const pf::audio::output_format format{rate, channels, pf::audio::sample_format::float32};
		std::vector<uint8_t> bytes(static_cast<size_t>(frames) * format.frame_bytes());
		mixer.render(bytes, format);
		std::vector<float> result(static_cast<size_t>(frames) * channels);
		std::memcpy(result.data(), bytes.data(), bytes.size());
		return result;
	}

	bool all_silent(const std::vector<float>& frames)
	{
		return std::ranges::all_of(frames, [](const float x) { return x == 0.0f; });
	}

	// What a sound may be written in, and what is refused.
	void test_wav_reader()
	{
		using pf::audio::read_wav;

		// 8-bit unsigned mono: the games' samples.
		const auto ramp = read_wav(make_wav(11025, 300));
		CHECK(ramp.has_value());
		if (ramp)
		{
			CHECK_EQ(ramp->sample_rate, 11025u);
			CHECK_EQ(ramp->channels, 1u);
			CHECK_EQ(ramp->block_align, 1u);
			CHECK_EQ(ramp->frames(), 300u);
			CHECK(near_sample(ramp->samples[0], -1.0f));
			CHECK(near_sample(ramp->samples[128], 0.0f));
			CHECK(near_sample(ramp->samples[255], 127.0f / 128.0f));
		}

		auto sound = read_wav(build_wav({.channels = 2, .bits = 16}, {0x00, 0x40, 0x00, 0xC0}));
		CHECK(sound.has_value());
		if (sound)
		{
			CHECK_EQ(sound->block_align, 4u);
			CHECK_EQ(sound->frames(), 1u);
			CHECK(near_sample(sound->samples[0], 0.5f));
			CHECK(near_sample(sound->samples[1], -0.5f));
		}

		sound = read_wav(build_wav({.bits = 24}, {0x00, 0x00, 0x40}));
		CHECK(sound && near_sample(sound->samples[0], 0.5f));
		sound = read_wav(build_wav({.bits = 32}, {0x00, 0x00, 0x00, 0xC0}));
		CHECK(sound && near_sample(sound->samples[0], -0.5f));
		sound = read_wav(build_wav({.tag = 3, .bits = 32}, float_bytes(0.25f)));
		CHECK(sound && near_sample(sound->samples[0], 0.25f));
		sound = read_wav(build_wav({.bits = 32, .extensible = true, .sub_tag = 3}, float_bytes(0.25f)));
		CHECK(sound && near_sample(sound->samples[0], 0.25f));
		sound = read_wav(build_wav({.bits = 16, .extensible = true}, {0x00, 0x40}));
		CHECK(sound && near_sample(sound->samples[0], 0.5f));

		// The chunks in another order, and a padded odd-length chunk before them.
		CHECK(read_wav(build_wav({.data_first = true}, {1, 2, 3})).has_value());
		CHECK(read_wav(build_wav({.odd_chunk_first = true}, {1, 2, 3})).has_value());

		// Refused: a sub-format that is neither PCM nor float, a compressed format, more than two channels,
		// no whole frame, a file cut short, and a file that is not RIFF/WAVE.
		CHECK(!read_wav(build_wav({.bits = 32, .extensible = true, .sub_tag = 3, .bad_sub_format = true},
		                          float_bytes(0.25f))));
		CHECK(!read_wav(build_wav({.tag = 2, .bits = 4}, {1, 2, 3, 4})));
		CHECK(!read_wav(build_wav({.channels = 3, .bits = 16}, std::vector<uint8_t>(6))));
		CHECK(!read_wav(build_wav({.bits = 16}, {1})));
		CHECK(!read_wav(build_wav({}, {})));
		const auto whole = make_wav(11025, 8);
		CHECK(!read_wav(std::span(whole).first(20)));
		auto not_wave = whole;
		not_wave[8] = 'X';
		CHECK(!read_wav(not_wave));
	}

	// Sounds through a mixer with no device behind it, so every sample can be checked.
	void test_mixing_sounds()
	{
		const auto mixer = std::make_shared<pf::audio::mixer>();
		CHECK(pf::audio::create_sound_buffer(mixer, {}) == nullptr);

		// At the endpoint's own rate a sound plays its samples exactly, to both sides at full scale.
		const auto wav = make_wav(48000, 16);
		const auto source = *pf::audio::read_wav(wav);
		const auto sound = pf::audio::create_sound_buffer(mixer, wav);
		CHECK(sound != nullptr);
		if (!sound) return;

		CHECK(all_silent(render_floats(*mixer, 4, 2, 48000)));

		sound->play(false);
		auto out = render_floats(*mixer, 20, 2, 48000);
		for (size_t i = 0; i < 16; ++i)
		{
			CHECK(near_sample(out[2 * i], source.samples[i]));
			CHECK(near_sample(out[2 * i + 1], source.samples[i]));
		}
		CHECK(all_silent({out.begin() + 32, out.end()}));
		CHECK_EQ(sound->play_position().value_or(1), 0u); // ended, and back where it began

		// Panned hard left, then right of centre, which halves the left side; a mono endpoint hears no pan.
		sound->set_volume(0.5f);
		sound->set_pan(-1.0f);
		sound->play(false);
		out = render_floats(*mixer, 1, 2, 48000);
		CHECK(near_sample(out[0], source.samples[0] * 0.5f));
		CHECK(near_sample(out[1], 0.0f));
		sound->set_pan(0.5f);
		out = render_floats(*mixer, 1, 2, 48000);
		CHECK(near_sample(out[0], source.samples[1] * 0.25f));
		CHECK(near_sample(out[1], source.samples[1] * 0.5f));
		out = render_floats(*mixer, 1, 1, 48000);
		CHECK(near_sample(out[0], source.samples[2] * 0.5f));
		sound->stop();

		// At a quarter of the output rate each frame spans four, joined by straight lines.
		const auto slow_wav = make_wav(12000, 8);
		const auto slow_source = *pf::audio::read_wav(slow_wav);
		const auto slow = pf::audio::create_sound_buffer(mixer, slow_wav);
		slow->play(false);
		out = render_floats(*mixer, 8, 1, 48000);
		const auto s0 = slow_source.samples[0];
		const auto s1 = slow_source.samples[1];
		CHECK(near_sample(out[0], s0));
		CHECK(near_sample(out[1], s0 + (s1 - s0) * 0.25f));
		CHECK(near_sample(out[2], s0 + (s1 - s0) * 0.5f));
		CHECK(near_sample(out[4], s1));
		CHECK_EQ(slow->play_position().value_or(0), 2u);
		slow->stop();

		// The frequency is the rate the sound plays at, up to XAudio2's ratio of eight.
		const auto pitched = pf::audio::create_sound_buffer(mixer, make_wav(11025, 11025));
		pitched->set_volume(0.0f);
		pitched->set_frequency(22050);
		pitched->play(true);
		render_floats(*mixer, 4410, 2, 44100);
		CHECK_EQ(pitched->play_position().value_or(0), 2205u);
		pitched->set_frequency(11025 * 20);
		render_floats(*mixer, 100, 2, 44100);
		CHECK_EQ(pitched->play_position().value_or(0), 2405u);
		pitched->stop();

		// Playing a running sound carries on; a seek moves it; stopped, the seek is where it next begins.
		pitched->set_frequency(11025);
		pitched->play(true);
		render_floats(*mixer, 441, 2, 44100);
		CHECK_EQ(pitched->play_position().value_or(0), 110u);
		pitched->play(true);
		CHECK_EQ(pitched->play_position().value_or(0), 110u);
		pitched->set_play_position(5000);
		CHECK_EQ(pitched->play_position().value_or(0), 5000u);
		render_floats(*mixer, 4, 2, 44100);
		CHECK_EQ(pitched->play_position().value_or(0), 5001u);
		pitched->stop();
		CHECK_EQ(pitched->play_position().value_or(0), 5000u);

		// A loop wraps without a seam, started part-way through as well.
		const auto loop_wav = make_wav(48000, 100);
		const auto loop_source = *pf::audio::read_wav(loop_wav);
		const auto loop = pf::audio::create_sound_buffer(mixer, loop_wav);
		loop->set_play_position(60);
		loop->play(true);
		out = render_floats(*mixer, 50, 1, 48000);
		CHECK(near_sample(out[39], loop_source.samples[99]));
		CHECK(near_sample(out[40], loop_source.samples[0]));
		CHECK_EQ(loop->play_position().value_or(0), 10u);
		loop->stop();

		// A stereo sound keeps its sides whatever the pan, and a mono endpoint hears both.
		const auto stereo = pf::audio::create_sound_buffer(
			mixer, build_wav({.channels = 2, .rate = 48000, .bits = 16}, {0x00, 0x40, 0x00, 0xE0}));
		stereo->set_pan(1.0f);
		stereo->play(true);
		out = render_floats(*mixer, 1, 2, 48000);
		CHECK(near_sample(out[0], 0.5f));
		CHECK(near_sample(out[1], -0.25f));
		out = render_floats(*mixer, 1, 1, 48000);
		CHECK(near_sample(out[0], 0.125f));
		stereo->stop();
	}

	// A stream plays its blocks in order at its own rate, a frame behind; a block counts as queued until
	// its last frame has been taken, so the queue drains at exactly the stream's rate.
	void test_mixing_streams()
	{
		const auto mixer = std::make_shared<pf::audio::mixer>();
		CHECK(pf::audio::create_audio_stream(mixer, 0, 1, 4) == nullptr);
		CHECK(pf::audio::create_audio_stream(mixer, 48000, 0, 4) == nullptr);
		CHECK(pf::audio::create_audio_stream(mixer, 48000, 3, 4) == nullptr);
		CHECK(pf::audio::create_audio_stream(mixer, 48000, 1, 0) == nullptr);

		const auto stream = pf::audio::create_audio_stream(mixer, 48000, 1, 2);
		CHECK(stream != nullptr);
		if (!stream) return;

		const std::vector<int16_t> block = {8192, 16384, -8192};
		CHECK(!stream->write({}));
		CHECK(stream->write(block));
		CHECK(stream->write(block));
		CHECK(!stream->can_write());
		CHECK(!stream->write(block));
		CHECK_EQ(stream->queued_blocks(), 2);

		auto out = render_floats(*mixer, 4, 2, 48000);
		CHECK(near_sample(out[0], 0.0f));
		CHECK(near_sample(out[2], 0.25f));
		CHECK(near_sample(out[3], 0.25f));
		CHECK(near_sample(out[4], 0.5f));
		CHECK(near_sample(out[6], -0.25f));
		CHECK_EQ(stream->queued_blocks(), 1);
		CHECK(stream->can_write());

		// Stopped, nothing is queued or heard, and the next write starts from silence.
		stream->stop();
		CHECK_EQ(stream->queued_blocks(), 0);
		CHECK(all_silent(render_floats(*mixer, 4, 2, 48000)));
		CHECK(stream->write(block));
		CHECK_EQ(stream->queued_blocks(), 1);

		// Starved, it falls silent; written again, it plays on.
		render_floats(*mixer, 8, 1, 48000);
		CHECK_EQ(stream->queued_blocks(), 0);
		CHECK(all_silent(render_floats(*mixer, 4, 1, 48000)));
		CHECK(stream->write(block));
		out = render_floats(*mixer, 2, 1, 48000);
		CHECK(near_sample(out[1], 0.25f));
		stream->stop();

		// A block that ends part-way through a frame is refused whole, as XAudio2 refused it.
		const auto stereo = pf::audio::create_audio_stream(mixer, 48000, 2, 4);
		CHECK(!stereo->write(std::vector<int16_t>(3, 0)));
		CHECK_EQ(stereo->queued_blocks(), 0);

		// dui's pace: 44.1 kHz into a 48 kHz endpoint takes 44,100 frames a second.
		const auto paced = pf::audio::create_audio_stream(mixer, 44100, 1, 45);
		const std::vector<int16_t> thousand(1000, 0);
		for (int i = 0; i < 45; ++i) CHECK(paced->write(thousand));
		render_floats(*mixer, 48000, 2, 48000);
		CHECK_EQ(paced->queued_blocks(), 1);
	}

	// The mix in each endpoint format: clipped, and the stereo mix in the front pair of a surround endpoint.
	void test_mixer_output()
	{
		const auto mixer = std::make_shared<pf::audio::mixer>();
		const auto sound = pf::audio::create_sound_buffer(
			mixer, build_wav({.channels = 2, .rate = 48000, .bits = 16}, {0x00, 0x40, 0x00, 0xE0}));
		CHECK(sound != nullptr);
		if (!sound) return;
		sound->play(true);

		const pf::audio::output_format int16_format{48000, 2, pf::audio::sample_format::int16};
		std::vector<uint8_t> bytes(int16_format.frame_bytes());
		mixer->render(bytes, int16_format);
		int16_t s16[2];
		std::memcpy(s16, bytes.data(), sizeof(s16));
		CHECK(std::abs(s16[0] - 16384) <= 1);
		CHECK(std::abs(s16[1] + 8192) <= 1);

		sound->set_volume(4.0f);
		const pf::audio::output_format int32_format{48000, 2, pf::audio::sample_format::int32};
		bytes.assign(int32_format.frame_bytes(), 0);
		mixer->render(bytes, int32_format);
		int32_t s32[2];
		std::memcpy(s32, bytes.data(), sizeof(s32));
		CHECK_EQ(s32[0], 2147483647);
		CHECK_EQ(s32[1], -2147483647);

		sound->set_volume(1.0f);
		const pf::audio::output_format surround{48000, 6, pf::audio::sample_format::float32};
		bytes.assign(surround.frame_bytes(), 0xFF);
		mixer->render(bytes, surround);
		float f[6];
		std::memcpy(f, bytes.data(), sizeof(f));
		CHECK(near_sample(f[0], 0.5f));
		CHECK(near_sample(f[1], -0.25f));
		for (auto c = 2; c < 6; ++c) CHECK(f[c] == 0.0f);
		sound->stop();
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

	// Source frames a second a looping buffer plays: two readings of its cursor around a wait timed on the
	// steady clock, not assumed from the sleep. The sample must outlast the wait at the rate measured.
	double measured_rate(const pf::sound_buffer_ptr& sound, const uint32_t frames, const int wait_ms)
	{
		const auto t0 = std::chrono::steady_clock::now();
		const auto p0 = sound->play_position().value_or(0);
		pf::platform_sleep(wait_ms);
		const auto p1 = sound->play_position().value_or(0);
		const auto t1 = std::chrono::steady_clock::now();
		const auto advanced = (p1 + frames - p0) % frames;
		return advanced / std::chrono::duration<double>(t1 - t0).count();
	}

	bool near(const double measured, const double expected, const double tolerance)
	{
		return measured > expected * (1.0 - tolerance) && measured < expected * (1.0 + tolerance);
	}

	// What the games rely on, recorded from XAudio2 before the mixer replaced it: the rate a pitch plays
	// at, a re-triggered sound carrying on, a seek, a one-shot ending, a loop wrapping, and the engine's
	// hand-over of its cursor from one pitch's sample to the next.
	void test_sound_buffer_behaviour()
	{
		constexpr uint32_t long_frames = 33075; // three seconds at 11,025 Hz
		const auto wav = make_wav(11025, long_frames);
		const auto sound = pf::create_sound_buffer(wav);
		CHECK(sound != nullptr);
		if (!sound) return;
		sound->set_volume(0.0f);

		sound->play(true);
		pf::platform_sleep(50);
		CHECK(near(measured_rate(sound, long_frames, 400), 11025, 0.15));

		sound->set_frequency(22050);
		CHECK(near(measured_rate(sound, long_frames, 400), 22050, 0.15));

		sound->set_frequency(28604);
		CHECK(near(measured_rate(sound, long_frames, 400), 28604, 0.15));

		// Playing a sound that is running carries on rather than starting it over.
		sound->set_frequency(11025);
		const auto before = sound->play_position().value_or(0);
		sound->play(true);
		pf::platform_sleep(60);
		CHECK(sound->play_position().value_or(0) > before);

		// Seeking a running sound plays on from there; stopped, it is where the next play() begins.
		sound->set_play_position(8000);
		const auto sought = sound->play_position().value_or(0);
		CHECK(sought >= 8000u && sought < 9500u);
		sound->stop();
		CHECK_EQ(sound->play_position().value_or(0), 8000u);

		// A one-shot sound ends on its own, its cursor back where it began, and plays again from there.
		const auto short_wav = make_wav(11025, 1103);
		const auto blip = pf::create_sound_buffer(short_wav);
		CHECK(blip != nullptr);
		if (!blip) return;
		blip->set_volume(0.0f);
		blip->play(false);
		pf::platform_sleep(400);
		CHECK_EQ(blip->play_position().value_or(1), 0u);
		blip->play(false);
		pf::platform_sleep(50);
		const auto again = blip->play_position().value_or(0);
		CHECK(again > 0u && again < 1103u);
		blip->stop();

		// A looping sound wraps and keeps going.
		blip->play(true);
		pf::platform_sleep(350);
		const auto lap = blip->play_position().value_or(1103);
		CHECK(lap < 1103u);
		pf::platform_sleep(40);
		CHECK(blip->play_position().value_or(lap) != lap);
		blip->stop();

		// The engine's hand-over: the next pitch's sample starts at the last one's cursor and runs on.
		const auto next = pf::create_sound_buffer(wav);
		CHECK(next != nullptr);
		if (!next) return;
		next->set_volume(0.0f);
		sound->play(true);
		pf::platform_sleep(200);
		const auto cursor = sound->play_position().value_or(0);
		sound->stop();
		next->set_play_position(cursor);
		next->play(true);
		CHECK(next->play_position().value_or(0) >= cursor);
		pf::platform_sleep(60);
		CHECK(next->play_position().value_or(0) > cursor);
		next->stop();
	}

	// The queue is dui's clock: a producer that waits for room writes 60 blocks of 735 frames a second at
	// 44.1 kHz. Counted over two seconds after the queue has filled, between two full queues.
	void test_audio_stream_pace()
	{
		const auto stream = pf::create_audio_stream(44100, 1, 4);
		CHECK(stream != nullptr);
		if (!stream) return;
		stream->set_volume(0.0f);

		const std::vector<int16_t> block(735, 0);
		const auto pump = [&](const double seconds)
		{
			auto written = 0;
			const auto end = std::chrono::steady_clock::now() + std::chrono::duration<double>(seconds);
			while (std::chrono::steady_clock::now() < end)
			{
				if (stream->can_write())
				{
					if (stream->write(block)) ++written;
				}
				else
				{
					pf::platform_sleep(1);
				}
			}
			return written;
		};

		pump(0.5);
		const auto t0 = std::chrono::steady_clock::now();
		const auto written = pump(2.0);
		const auto seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
		CHECK(near(written / seconds, 60.0, 0.05));

		// Stopped, the queue is empty and the next write queues one block. XAudio2's stop() left up to the
		// whole queue to play when the stream started again, though stop() promises to drop it.
		stream->stop();
		CHECK_EQ(stream->queued_blocks(), 0);
		CHECK(stream->write(block));
		CHECK_EQ(stream->queued_blocks(), 1);
		stream->stop();

		// A block that ends part-way through a frame is refused.
		const auto stereo = pf::create_audio_stream(44100, 2, 4);
		CHECK(stereo != nullptr);
		if (!stereo) return;
		stereo->set_volume(0.0f);
		CHECK(!stereo->write(std::vector<int16_t>(3, 0)));
		CHECK_EQ(stereo->queued_blocks(), 0);
	}

	// Held past sound_shutdown, a sound plays on and a stream drains, as both did under XAudio2: what holds
	// them keeps the device, so a producer pacing itself on the queue is never left waiting. Nothing new
	// is made once it is shut down, and the device closes with the last of them.
	void test_held_past_shutdown()
	{
		const auto sound = pf::create_sound_buffer(make_wav(11025, 33075));
		const auto stream = pf::create_audio_stream(44100, 1, 4);
		CHECK(sound != nullptr);
		CHECK(stream != nullptr);

		if (!sound || !stream)
		{
			pf::sound_shutdown();
			return;
		}

		sound->set_volume(0.0f);
		stream->set_volume(0.0f);
		sound->play(true);
		CHECK(stream->write(std::vector<int16_t>(735, 0)));

		pf::sound_shutdown();
		CHECK(pf::create_sound_buffer(make_wav(11025, 100)) == nullptr);
		CHECK(pf::create_audio_stream(44100, 1, 4) == nullptr);

		const auto before = sound->play_position().value_or(0);
		pf::platform_sleep(150);
		CHECK(sound->play_position().value_or(before) != before);
		CHECK_EQ(stream->queued_blocks(), 0);
	}

	// The mixer needs no window, so playback is testable headlessly. Everything
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
		test_sound_buffer_behaviour();
		test_audio_stream_pace();
		test_held_past_shutdown();
	}

	void test_backend_helpers()
	{
		CHECK_STR(pf::url_encode("a b&c=d"), "a%20b%26c%3Dd");

		CHECK_STR(pf::format_key_binding({}), "");
		CHECK_STR(pf::format_key_binding({'S', pf::key_mod::ctrl}), "Ctrl+S");
		CHECK_STR(pf::format_key_binding({'F', pf::key_mod::ctrl | pf::key_mod::shift}), "Ctrl+Shift+F");
		for (auto key = pf::platform_key::F1; key <= pf::platform_key::F12; ++key)
		{
			const auto name = std::format("F{}", key - pf::platform_key::F1 + 1);
			CHECK_STR(pf::format_key_binding({key}), name);
			CHECK_STR(pf::format_key_binding({key, pf::key_mod::ctrl}), "Ctrl+" + name);
			CHECK_STR(pf::format_key_binding({key, pf::key_mod::shift}), "Shift+" + name);
			CHECK_STR(pf::format_key_binding({key, pf::key_mod::alt}), "Alt+" + name);
		}
		CHECK_STR(pf::format_key_binding({pf::platform_key::Left, pf::key_mod::alt}), "Alt+Left");
		CHECK_STR(pf::format_key_binding({pf::platform_key::Right, pf::key_mod::alt}), "Alt+Right");
		CHECK_STR(pf::format_key_binding({pf::platform_key::Insert, pf::key_mod::ctrl}), "Ctrl+Ins");
		CHECK_STR(pf::format_key_binding({pf::platform_key::Delete, pf::key_mod::shift}), "Shift+Del");
		CHECK_STR(pf::format_key_binding({pf::platform_key::F12,
		                                pf::key_mod::ctrl | pf::key_mod::alt | pf::key_mod::shift}),
		          "Ctrl+Alt+Shift+F12");
		CHECK_STR(pf::format_key_binding({0xFF}), "0xFF");

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

	std::string read_whole_file(const pf::file_path& path)
	{
		std::string result;
		if (const auto f = pf::open_for_read(path))
		{
			result.resize(f->size());
			uint32_t read = 0;
			if (!result.empty() && !f->read(reinterpret_cast<uint8_t*>(result.data()),
				static_cast<uint32_t>(result.size()), &read)) read = 0;
			result.resize(read);
		}
		return result;
	}

	void write_whole_file(const pf::file_path& path, const std::string_view text)
	{
		if (const auto f = pf::open_file_for_write(path))
			f->write(reinterpret_cast<const uint8_t*>(text.data()), static_cast<uint32_t>(text.size()));
	}

	// The synchronous web client against a scripted loopback server: what each request puts on the
	// wire, and what each response comes back as, including the ways a response can be cut short.
	void test_web_client()
	{
		loopback::server server;
		CHECK(server.listening());
		if (!server.listening()) return;

		constexpr std::string_view boundary = "54B8723DE6044695A68C838E8BF0CB00";
		server.respond("/hello", {loopback::http_response(200, "OK", "hello world", "Content-Type: text/plain\r\n")});
		server.respond("/file", {loopback::http_response(200, "OK", "file body")});
		server.respond("/short", {"HTTP/1.1 200 OK\r\nContent-Length: 100\r\nConnection: close\r\n\r\n0123456789"});
		server.respond("/empty", {"HTTP/1.1 204 No Content\r\nConnection: close\r\n\r\n"});
		server.respond("/slow", {loopback::http_response(200, "OK", "late"), 3000});

		const auto host = pf::connect_to_host("127.0.0.1", false, server.port(), "libdesktop-tests/1.0");
		CHECK(host != nullptr);
		if (!host) return;

		pf::web_request get;
		get.path = "/hello";
		auto r = pf::send_request(host, get);
		CHECK_EQ(r.status_code, 200);
		CHECK_STR(r.body, "hello world");
		CHECK_STR(r.content_type, "text/plain");
		CHECK(r.error == pf::web_response_error::none);
		CHECK_STR(server.last().request_line, "GET /hello HTTP/1.1");
		CHECK_STR(loopback::header_value(server.last(), "User-Agent"), "libdesktop-tests/1.0");

		pf::web_request query;
		query.path = "/query";
		query.query = {{"a", "1"}, {"b", "x y"}};
		query.headers = {{"X-Test", "1"}};
		r = pf::send_request(host, query);
		CHECK_EQ(r.status_code, 404);
		CHECK_STR(r.body, "missing");
		CHECK(r.error == pf::web_response_error::none);
		CHECK_STR(server.last().request_line, "GET /query?a=1&b=x%20y HTTP/1.1");
		CHECK_STR(loopback::header_value(server.last(), "X-Test"), "1");

		pf::web_request form;
		form.verb = pf::web_request_verb::POST;
		form.path = "/form";
		form.form_data = {{"name", "value"}};
		r = pf::send_request(host, form);
		CHECK_EQ(r.status_code, 404);
		CHECK_STR(server.last().request_line, "POST /form HTTP/1.1");
		CHECK_STR(loopback::header_value(server.last(), "Content-Type"),
		          std::format("multipart/form-data; boundary={}", boundary));
		CHECK_STR(server.last().body, std::format(
			"--{0}\r\nContent-Disposition: form-data; name=\"name\"\r\nContent-Type: text/plain; charset=\"utf-8\"\r\n"
			"\r\nvalue\r\n--{0}--", boundary));

		const auto temp = pf::platform_temp_file_path("ldt");
		const pf::file_path zip(temp + ".zip");
		write_whole_file(zip, std::string_view("PK\x03\x04zip", 7));
		pf::web_request upload;
		upload.verb = pf::web_request_verb::POST;
		upload.path = "/upload";
		upload.form_data = {{"k", "v"}};
		upload.file_form_data_name = "ff";
		upload.file_name = "logs.zip";
		upload.upload_file_path = zip;
		r = pf::send_request(host, upload);
		CHECK_EQ(r.status_code, 404);
		CHECK_STR(server.last().body, std::format(
			"--{0}\r\nContent-Disposition: form-data; name=\"k\"\r\nContent-Type: text/plain; charset=\"utf-8\"\r\n"
			"\r\nv\r\n--{0}\r\nContent-Disposition: form-data; name=\"ff\"; filename=\"logs.zip\"\r\n"
			"Content-Type: application/x-zip-compressed\r\n\r\n{1}\r\n--{0}--", boundary,
			std::string_view("PK\x03\x04zip", 7)));

		pf::web_request unreadable = upload;
		unreadable.upload_file_path = pf::file_path(temp + "-missing.zip");
		const auto requests_before = server.requests().size();
		r = pf::send_request(host, unreadable);
		CHECK_EQ(r.status_code, 0);
		CHECK(r.error == pf::web_response_error::transport);
		CHECK_EQ(server.requests().size(), requests_before);

		const pf::file_path download(temp + "-download.txt");
		pf::web_request fetch;
		fetch.path = "/file";
		fetch.download_file_path = download;
		r = pf::send_request(host, fetch);
		CHECK_EQ(r.status_code, 200);
		CHECK(r.body.empty());
		CHECK(r.error == pf::web_response_error::none);
		CHECK_STR(read_whole_file(download), "file body");

		// A body shorter than its Content-Length is a failed transfer. A download cut short keeps
		// what arrived; the error says it is incomplete.
		pf::web_request cut;
		cut.path = "/short";
		r = pf::send_request(host, cut);
		CHECK_EQ(r.status_code, 200);
		CHECK(r.error == pf::web_response_error::transport);
		CHECK(r.body.empty());

		const pf::file_path cut_download(temp + "-short.txt");
		cut.download_file_path = cut_download;
		r = pf::send_request(host, cut);
		CHECK_EQ(r.status_code, 200);
		CHECK(r.error == pf::web_response_error::transport);
		CHECK_STR(read_whole_file(cut_download), "0123456789");

		pf::web_request empty;
		empty.path = "/empty";
		r = pf::send_request(host, empty);
		CHECK_EQ(r.status_code, 204);
		CHECK(r.body.empty());
		CHECK(r.error == pf::web_response_error::none);

		pf::web_request relative;
		relative.path = "hello";
		CHECK(pf::send_request(host, relative).error == pf::web_response_error::invalid_request);

		pf::web_request injected;
		injected.path = "/hello";
		injected.headers = {{"X-Test", "a\r\nX-Evil: 1"}};
		CHECK(pf::send_request(host, injected).error == pf::web_response_error::invalid_request);

		pf::web_request limited;
		limited.path = "/hello";
		limited.max_response_bytes = 5;
		r = pf::send_request(host, limited);
		CHECK(r.error == pf::web_response_error::response_limit);
		CHECK(r.body.empty());
		CHECK_STR(loopback::header_value(server.last(), "User-Agent"), "libdesktop-tests/1.0");

		pf::web_request slow;
		slow.path = "/slow";
		slow.timeout_ms = 300;
		const auto started = std::chrono::steady_clock::now();
		r = pf::send_request(host, slow);
		const auto waited = std::chrono::steady_clock::now() - started;
		CHECK(r.error == pf::web_response_error::transport);
		CHECK(waited < std::chrono::milliseconds(2500));

		// An empty user agent is pf::'s default, which still names Potato.
		const auto anonymous = pf::connect_to_host("127.0.0.1", false, server.port());
		CHECK(anonymous != nullptr);
		if (anonymous)
		{
			pf::send_request(anonymous, get);
			CHECK_STR(loopback::header_value(server.last(), "User-Agent"), "PotatoApp/1.0");
		}

		int closed_port = 0;
		{
			const loopback::server gone;
			closed_port = gone.port();
		}
		const auto nowhere = pf::connect_to_host("127.0.0.1", false, closed_port, "x");
		CHECK(nowhere != nullptr);
		if (nowhere)
		{
			r = pf::send_request(nowhere, get);
			CHECK_EQ(r.status_code, 0);
			CHECK(r.error == pf::web_response_error::transport);
		}

		pf::platform_delete_file(zip);
		pf::platform_delete_file(download);
		pf::platform_delete_file(cut_download);
		pf::platform_delete_file(pf::file_path(temp));
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
	// Registered before dispatcher creation, so this checks after its teardown has drained.
	CHECK(std::atexit(check_pool_teardown) == 0);
	test_text();
	test_invalid_text();
	test_file_path();
	test_instance_lock();
	test_local_app_data_path();
	test_serial_executor();
	test_headless_async_pool();
	test_pool_waiting_on_serial_executor();
	test_geometry();
	test_embedded_resources();
	test_wav_reader();
	test_mixing_sounds();
	test_mixing_streams();
	test_mixer_output();
	test_audio();
	test_backend_helpers();
	test_line_splitter();
	test_child_process_arguments();
	test_path_containment();
	test_web_client();

	CHECK(std::atexit([] { g_shutdown_release.set_value(); }) == 0);
	std::printf("libdesktop tests: %s (%d checks, %d failures)\n",
	            g_failures == 0 ? "PASS" : "FAIL", g_checks, g_failures);
	for (int i = 0; i < shutdown_job_count; ++i)
		pf::run_async([] { g_shutdown_gate.wait(); ++g_shutdown_completed; });
	return g_failures == 0 ? 0 : 1;
}
