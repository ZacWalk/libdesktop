#include "platform.h"
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <future>
#include <string>
#include <thread>
#include <vector>

uintptr_t process_test_inheritable_handle();
void process_test_close_handle(uintptr_t handle);

namespace
{
	int failures = 0, checks = 0;
	void check(bool ok, int line)
	{
		++checks;
		if (!ok) { ++failures; std::printf("process FAIL line %d\n", line); }
	}
#define REQUIRE(ok) check((ok), __LINE__)
	using namespace std::chrono_literals;

	pf::process_options fixture(std::vector<std::string> args)
	{
		pf::process_options options;
		options.exe = std::string(pf::file_path::module_folder().combine("platform_process_fixture.exe").view());
		options.args = std::move(args);
		return options;
	}

	std::string drain(pf::process_ptr child, bool error = false)
	{
		std::string result;
		char buffer[8192];
		for (;;)
		{
			auto count = error ? pf::process_read_err(child, buffer, sizeof(buffer)) :
				pf::process_read(child, buffer, sizeof(buffer));
			if (!count) break;
			result.append(buffer, count);
		}
		return result;
	}

	bool exited(const pf::process_ptr& child)
	{
		const auto deadline = std::chrono::steady_clock::now() + 5s;
		while (pf::process_alive(child) && std::chrono::steady_clock::now() < deadline)
			std::this_thread::sleep_for(10ms);
		return !pf::process_alive(child);
	}

	std::string records(const std::vector<std::string>& args, size_t start = 1)
	{
		std::string result;
		for (size_t i = start; i < args.size(); ++i)
			result += std::to_string(args[i].size()) + ":" + args[i] + "\n";
		return result;
	}

	void arguments()
	{
		const auto options = fixture({"args", "", "space here", "a\"b", "trailing space\\",
			"&|<>^%!()", "\xE2\x82\xAC", "a\nb", "a\\\"b"});
		auto child = pf::process_spawn(options);
		REQUIRE(child != nullptr);
		if (!child) return;
		pf::process_close_input(child);
		REQUIRE(drain(child) == records(options.args));
		REQUIRE(drain(child, true).empty());
		REQUIRE(exited(child));
	}

	void environment()
	{
		auto options = fixture({"env", "PF_PROCESS_TEST", "PF_PROCESS_EMPTY"});
		options.env = {"PF_PROCESS_TEST=first", "pf_process_test=override=\xE2\x82\xAC", "PF_PROCESS_EMPTY="};
		options.cwd = std::string(pf::file_path::module_folder().view());
		auto child = pf::process_spawn(options);
		REQUIRE(child != nullptr);
		pf::process_close_input(child);
		REQUIRE(drain(child) == records({"", "override=\xE2\x82\xAC", "", options.cwd}));
		REQUIRE(exited(child));
		options.env = {"INVALID"};
		REQUIRE(!pf::process_spawn(options));
		options.env = {"=INVALID"};
		REQUIRE(!pf::process_spawn(options));
	}

	void streams()
	{
		auto child = pf::process_spawn(fixture({"streams"}));
		REQUIRE(child != nullptr);
		auto errors = std::async(std::launch::async, [child] { return drain(child, true); });
		REQUIRE(drain(child) == std::string(512 * 1024, 'o'));
		REQUIRE(errors.get() == std::string(512 * 1024, 'e'));
		REQUIRE(exited(child));

		child = pf::process_spawn(fixture({"echo"}));
		REQUIRE(child != nullptr);
		auto output = std::async(std::launch::async, [child] { return drain(child); });
		errors = std::async(std::launch::async, [child] { return drain(child, true); });
		std::string payload(256 * 1024, 'x');
		payload[5] = '\0';
		REQUIRE(pf::process_write(child, payload));
		pf::process_close_input(child);
		pf::process_close_input(child);
		REQUIRE(!pf::process_write(child, "late"));
		REQUIRE(output.get() == payload);
		REQUIRE(errors.get() == "closed\n");
		REQUIRE(exited(child));

		child = pf::process_spawn(fixture({"echo"}));
		REQUIRE(child != nullptr);
		output = std::async(std::launch::async, [child] { return drain(child); });
		const std::string a(128 * 1024, 'a'), b(128 * 1024, 'b');
		auto first = std::async(std::launch::async, [child, &a] { return pf::process_write(child, a); });
		auto second = std::async(std::launch::async, [child, &b] { return pf::process_write(child, b); });
		REQUIRE(first.get());
		REQUIRE(second.get());
		pf::process_close_input(child);
		const auto combined = output.get();
		REQUIRE(combined == a + b || combined == b + a);
		REQUIRE(drain(child, true) == "closed\n");
		REQUIRE(exited(child));
	}

	void inheritance()
	{
		const auto handle = process_test_inheritable_handle();
		REQUIRE(handle != 0);
		auto child = pf::process_spawn(fixture({"handle", std::to_string(handle)}));
		REQUIRE(child != nullptr);
		REQUIRE(drain(child) == "absent\n");
		REQUIRE(exited(child));
		process_test_close_handle(handle);

		auto options = fixture({"hold"});
		options.kill_descendants_on_close = false;
		child = pf::process_spawn(options);
		REQUIRE(child != nullptr);
		REQUIRE(pf::process_alive(child));
		pf::process_terminate(child);
		REQUIRE(!pf::process_alive(child));
	}

	void inherited_stdio()
	{
		auto options = fixture({});
		options.exe = std::string(pf::file_path::module_folder().combine("platform_stdio_fixture.exe").view());
		auto child = pf::process_spawn(options);
		REQUIRE(child != nullptr);
		auto output = std::async(std::launch::async, [child] { return drain(child); });
		std::string bytes(192 * 1024, 'x');
		bytes[0] = '\0';
		bytes[10] = '\n';
		bytes[11] = '\r';
		bytes[12] = '\x1a';
		REQUIRE(pf::process_write(child, bytes));
		pf::process_close_input(child);
		REQUIRE(output.get() == bytes);
		REQUIRE(drain(child, true).empty());
		REQUIRE(exited(child));
		options.args = {"self"};
		child = pf::process_spawn(options);
		REQUIRE(child != nullptr);
		pf::process_close_input(child);
		REQUIRE(drain(child) == options.exe);
		REQUIRE(exited(child));
		REQUIRE(pf::executable_path().name() == "platform_tests.exe");
		options.args = {"invalid-read"};
		child = pf::process_spawn(options);
		REQUIRE(child != nullptr);
		pf::process_close_input(child);
		REQUIRE(drain(child) == "error");
		REQUIRE(exited(child));
	}

	void cancelled_io()
	{
		for (const bool close_only : {false, true})
		{
			auto child = pf::process_spawn(fixture({"hold"}));
			REQUIRE(child != nullptr);
			char ready[64]{};
			REQUIRE(pf::process_read(child, ready, sizeof(ready)) == 6);
			auto reader = std::async(std::launch::async, [child] { return drain(child); });
			auto error = std::async(std::launch::async, [child] { return drain(child, true); });
			auto writer = std::async(std::launch::async, [child]
			{
				return pf::process_write(child, std::string(8 * 1024 * 1024, 'x'));
			});
			std::this_thread::sleep_for(50ms);
			if (close_only)
			{
				pf::process_close_input(child);
				REQUIRE(writer.wait_for(5s) == std::future_status::ready);
				REQUIRE(pf::process_alive(child));
			}
			auto second = std::async(std::launch::async, [child] { pf::process_terminate(child); });
			pf::process_terminate(child);
			REQUIRE(second.wait_for(5s) == std::future_status::ready);
			REQUIRE(writer.wait_for(5s) == std::future_status::ready);
			REQUIRE(reader.wait_for(5s) == std::future_status::ready);
			REQUIRE(error.wait_for(5s) == std::future_status::ready);
			REQUIRE(!writer.get());
			REQUIRE(reader.get().empty());
			REQUIRE(error.get().empty());
			REQUIRE(!pf::process_alive(child));
		}
	}

	void descendants()
	{
		for (const auto mode : {"tree", "tree-exit"})
		{
			auto child = pf::process_spawn(fixture({mode}));
			REQUIRE(child != nullptr);
			std::string line;
			char c{};
			while (pf::process_read(child, &c, 1) && c != '\n') line += c;
			REQUIRE(!line.empty());
			if (std::string_view(mode) == "tree-exit") REQUIRE(exited(child));
			auto watcher = pf::process_spawn(fixture({"watch", line}));
			REQUIRE(watcher != nullptr);
			child.reset();
			REQUIRE(drain(watcher) == "dead\n");
		}
	}

	void batch()
	{
		const auto base = std::filesystem::path(pf::utf8_to_utf16(pf::file_path::module_folder().view()));
		const auto native = fixture({}).exe;
		for (const auto extension : {L".cmd", L".bat"})
		{
			const auto script = base / (std::wstring(L"pf process & fixture") + extension);
			{
				std::ofstream file(script);
				file << "@echo off\r\n\"" << native << "\" %*\r\n";
			}
			auto options = fixture({"args", "", "space here", "&|<>^!()", "\xE2\x82\xAC",
				"trailing space\\", "trailing\\", "\\", "two\\\\"});
			options.exe = pf::utf16_to_utf8(script.wstring());
			auto child = pf::process_spawn(options);
			REQUIRE(child != nullptr);
			pf::process_close_input(child);
			REQUIRE(drain(child) == records(options.args));
			REQUIRE(exited(child));
			for (const auto argument : {"%PATH%", "a\"b", "a\nb", "a\rb"})
			{
				options.args = {"args", argument};
				REQUIRE(!pf::process_spawn(options));
			}
			std::filesystem::remove(script);
		}
	}
}

bool test_processes()
{
	char buffer[4]{};
	REQUIRE(!pf::process_spawn({}));
	REQUIRE(!pf::process_spawn(fixture({std::string("x\0y", 3)})));
	REQUIRE(!pf::process_spawn({"platform-fixture-does-not-exist.exe"}));
	REQUIRE(!pf::process_alive({}));
	REQUIRE(pf::process_read({}, buffer, sizeof(buffer)) == 0);
	REQUIRE(pf::process_read_err({}, buffer, sizeof(buffer)) == 0);
	REQUIRE(!pf::process_write({}, "x"));
	pf::process_close_input({});
	pf::process_terminate({});
	arguments();
	environment();
	streams();
	inheritance();
	inherited_stdio();
	cancelled_io();
	descendants();
	batch();
	std::printf("process tests: %d checks, %d failures\n", checks, failures);
	return failures == 0;
}
