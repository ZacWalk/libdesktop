// This file is part of libdesktop, a C++20 platform layer for desktop applications
// Copyright 2026  Zac Walker
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

// Purpose: A tiny executable smoke-check for libdesktop_add_app(), embedded resources, the UI task
// queue of an application platform::run started, the folders its event loop watches, and the agent session it
// serves on stdin and stdout, checked over real pipes by a copy of itself - which opens no other program until its
// application allows it.

#include "libdesktop/platform.h"
#include "libdesktop/core/mcp.h"

#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <format>
#include <mutex>
#include <thread>

namespace
{
	// A console mode whose worker queues UI work and runs it itself while the UI thread waits for the
	// worker, as a test suite run from a console mode does.
	class ui_pump_check final : public platform::app
	{
	public:
		bool parse_command_line(std::string_view) override { return true; }

		int run_console_mode() override
		{
			auto delivered = false;
			std::thread worker([&delivered]
			{
				platform::run_ui([&delivered] { delivered = true; });
				platform::pump_ui_tasks(1000);
			});
			worker.join();
			platform::write_stdout(delivered ? "UI task pump: 1 check, 0 failed\n"
			                                 : "UI task pump: 1 check, 1 failed\n");
			return delivered ? 0 : 1;
		}

		bool init(std::string_view) override { return false; }
		void crash(core::file_path) override {}
		void save_recovery_state() override {}
	};

	// A folder watched with its subfolders reports a change two folders down as the folder it happened in,
	// and one in the folder itself as that folder, so an application showing the whole tree can tell what
	// to look at again.
	class folder_watch_check final : public platform::app
	{
		core::folder_path _root;
		core::folder_path _nested;
		bool _nested_reported = false;
		bool _root_written = false;
		bool _root_reported = false;
		bool _finished = false;
		int _ticks = 0;

		static bool write(const core::file_path path)
		{
			constexpr std::string_view text = "changed";
			return platform::blob_save_to_file({reinterpret_cast<const uint8_t*>(text.data()), text.size()}, path);
		}

		void finish()
		{
			if (_finished) return;
			_finished = true;
			platform::monitor_folders({});
			platform::delete_items({}, {_root}, false);
			const auto failed = (_nested_reported ? 0 : 1) + (_root_reported ? 0 : 1);
			platform::write_stdout(std::format("Folder watch: 2 checks, {} failed\n", failed));
			platform::quit(failed == 0 ? 0 : 1);
		}

	public:
		bool init(std::string_view) override
		{
			const auto unique = std::chrono::steady_clock::now().time_since_epoch().count();
			_root = platform::temp_folder().combine(std::format("libdesktop-folder-watch-{}", unique));
			_nested = _root.combine("a").combine("b");
			if (platform::create_folder(_nested).failed())
			{
				platform::write_stdout("Folder watch: the folders to watch could not be made\n");
				platform::quit(1);
				return false;
			}

			platform::monitor_folders({{_root, true}});
			platform::set_frame_delay(50);
			if (!write(_nested.combine_file("nested.txt"))) finish();
			return true;
		}

		void folder_changed(const core::folder_path folder) override
		{
			if (folder == _nested && !_nested_reported)
			{
				_nested_reported = true;
				_root_written = write(_root.combine_file("root.txt"));
				if (!_root_written) finish();
			}
			else if (folder == _root && _root_written)
			{
				_root_reported = true;
				finish();
			}
		}

		// Ten seconds, far longer than a notification takes.
		void frame_tick() override
		{
			if (++_ticks >= 200) finish();
		}

		void crash(core::file_path) override {}
		void save_recovery_state() override {}
	};

	// An agent session with no window: its tools say which thread they ran on, and one prints where the
	// protocol must never see it.
	class agent_serve final : public platform::app
	{
		std::shared_ptr<core::mcp::server> _server;

	public:
		agent_serve()
		{
			_server = std::make_shared<core::mcp::server>(core::mcp::server_info{.name = "smoke", .version = "1"});

			core::mcp::tool echo;
			echo.name = "echo";
			echo.description = "Says the text back, and whether it ran on the UI thread.";
			echo.input_schema = R"({"type":"object","properties":{"text":{"type":"string"}}})";
			echo.handler = [](const core::mcp::call_ptr& c)
			{
				auto result = core::mcp::tool_result::text(c->text("text"));
				result.structured = std::format(R"({{"ui_thread":{}}})", platform::is_ui_thread());
				c->finish(std::move(result));
			};
			_server->add_tool(echo);

			core::mcp::tool print;
			print.name = "print";
			print.description = "Prints to stdout, and answers from a worker.";
			print.handler = [](const core::mcp::call_ptr& c)
			{
				std::printf("stray printf\n");
				std::fflush(stdout);
				platform::write_stdout("stray write_stdout\n");
				std::thread([c] { c->finish(core::mcp::tool_result::text("printed")); }).detach();
			};
			_server->add_tool(print);

			// Starts a copy of this program that leaves a marker, through platform::run, as an application opens
			// another program: only where the session's application has allowed it.
			core::mcp::tool launch;
			launch.name = "launch";
			launch.description = "Runs a copy of this program that writes the marker, where the session may.";
			launch.input_schema =
				R"({"type":"object","properties":{"allow":{"type":"boolean"},"marker":{"type":"string"}}})";
			launch.handler = [](const core::mcp::call_ptr& c)
			{
				platform::allow_agent_to_open(c->flag("allow", false));
				const auto exe = platform::running_app_path();
				const auto ran = platform::run(exe, std::format(R"("{}" --mark "{}")", exe.str(), c->text("marker")));
				auto result = core::mcp::tool_result::text(ran ? "ran" : "not run");
				result.structured = std::format(R"({{"ran":{}}})", ran);
				c->finish(std::move(result));
			};
			_server->add_tool(launch);
		}

		bool parse_command_line(std::string_view) override { return false; }
		std::shared_ptr<core::mcp::server> agent_server() override { return _server; }
		bool init(std::string_view) override { return platform::is_agent_session(); }
		void crash(core::file_path) override {}
		void save_recovery_state() override {}
	};

	// Starts a copy of this program serving an agent session, and talks to it as a client does: every line it
	// writes to stdout must be a message, answered in turn, and closing its input must end it.
	int check_agent()
	{
		platform::process_options options;
		options.exe = platform::running_app_path().str();
		options.args = {"--serve-agent"};
		const auto child = platform::process_spawn(options);

		if (!child)
		{
			platform::write_stdout("Agent session: the server could not be started\n");
			return 1;
		}

		std::mutex mutex;
		std::condition_variable arrived;
		std::vector<std::string> lines;
		auto ended = false;

		std::thread reader([&]
		{
			platform::line_splitter splitter;
			std::array<char, 4096> buffer{};

			for (;;)
			{
				const auto read = platform::process_read(child, buffer.data(), buffer.size());
				if (read == 0) break;
				splitter.feed(std::string_view(buffer.data(), read), [&](const std::string_view line)
				{
					std::lock_guard lock(mutex);
					lines.emplace_back(line);
					arrived.notify_all();
				});
			}

			std::lock_guard lock(mutex);
			ended = true;
			arrived.notify_all();
		});

		auto checks = 0;
		auto failures = 0;
		const auto check = [&](const bool ok, const std::string_view what)
		{
			++checks;
			if (!ok)
			{
				++failures;
				platform::write_stdout(std::format("  FAIL  {}\n", what));
			}
		};

		// Sends a request and waits for the line that answers it.
		const auto ask = [&](const std::string_view request, const std::string_view id) -> rapidjson::Document
		{
			platform::process_write(child, std::string(request) + "\n");
			std::unique_lock lock(mutex);
			const auto answered = [&]
			{
				return ended || std::ranges::any_of(lines, [id](const std::string& l)
				{ return l.find(std::format(R"("id":{})", id)) != std::string::npos; });
			};
			arrived.wait_for(lock, std::chrono::seconds(20), answered);

			rapidjson::Document d;
			for (const auto& l : lines)
				if (l.find(std::format(R"("id":{})", id)) != std::string::npos) d.Parse(l.c_str());
			return d;
		};

		constexpr std::string_view meta =
			R"("_meta":{"io.modelcontextprotocol/protocolVersion":"2026-07-28","io.modelcontextprotocol/clientCapabilities":{}})";

		const auto discover =
			ask(std::format(R"({{"jsonrpc":"2.0","id":1,"method":"server/discover","params":{{{}}}}})", meta), "1");
		check(discover.IsObject() && discover.HasMember("result"), "server/discover answers");

		const auto echo = ask(
			std::format(
				R"({{"jsonrpc":"2.0","id":2,"method":"tools/call","params":{{"name":"echo","arguments":{{"text":"hi"}},{}}}}})",
				meta),
			"2");
		check(echo.IsObject() && echo.HasMember("result") && echo["result"]["structuredContent"]["ui_thread"].GetBool(),
		      "a tool's handler runs on the UI thread");

		const auto print = ask(R"({"jsonrpc":"2.0","id":3,"method":"tools/call","params":{"name":"print"}})", "3");
		check(print.IsObject() && print.HasMember("result"), "a tool answers from a worker");

		// A session opens no other program until its application allows it: the copy that would leave the marker is
		// not run, then is.
		const auto unique = std::chrono::steady_clock::now().time_since_epoch().count();
		const auto marker = platform::temp_folder().combine_file(std::format("libdesktop-agent-launch-{}.txt", unique));
		std::string escaped;
		for (const auto ch : marker.str())
		{
			if (ch == '\\' || ch == '"') escaped += '\\';
			escaped += ch;
		}
		const auto launch = [&](const bool allow, const std::string_view id)
		{
			return ask(
				std::format(
					R"({{"jsonrpc":"2.0","id":{},"method":"tools/call","params":{{"name":"launch","arguments":{{"allow":{},"marker":"{}"}}}}}})",
					id, allow, escaped),
				id);
		};
		const auto ran = [](const rapidjson::Document& d)
		{
			return d.IsObject() && d.HasMember("result") && d["result"]["structuredContent"]["ran"].GetBool();
		};

		const auto refused = launch(false, "4");
		std::this_thread::sleep_for(std::chrono::milliseconds(500));
		check(refused.IsObject() && refused.HasMember("result") && !ran(refused) && !platform::exists(marker),
		      "a session opens no other program until its application allows it");
#ifdef _WIN32
		// Linux's platform::run is a stand-in that runs nothing.
		const auto allowed = launch(true, "5");
		for (auto waited = 0; waited < 200 && !platform::exists(marker); ++waited)
			std::this_thread::sleep_for(std::chrono::milliseconds(50));
		check(ran(allowed) && platform::exists(marker), "and opens one once it does");
		platform::delete_items({marker}, {}, false);
#endif

		{
			std::lock_guard lock(mutex);

			for (const auto& l : lines)
			{
				rapidjson::Document d;
				check(!d.Parse(l.c_str()).HasParseError() && d.IsObject(),
				      std::format("stdout carries only messages: {}", l));
			}
		}

		platform::process_close_input(child);
		{
			std::unique_lock lock(mutex);
			arrived.wait_for(lock, std::chrono::seconds(20), [&] { return ended; });
			check(ended, "closing the input ends the session");
		}

		for (auto waited = 0; waited < 200 && platform::process_alive(child); ++waited)
			std::this_thread::sleep_for(std::chrono::milliseconds(50));
		check(!platform::process_alive(child), "and the process exits");

		if (platform::process_alive(child)) platform::process_terminate(child);
		reader.join();

		platform::write_stdout(std::format("Agent session: {} checks, {} failed\n", checks, failures));
		return failures == 0 ? 0 : 1;
	}
}

int main(const int argc, char** argv)
{
	if (argc == 2 && std::strcmp(argv[1], "--check-agent") == 0)
	{
		core::set_app_name("libdesktop-platform-smoke");
		return check_agent();
	}

	if (argc == 2 && std::strcmp(argv[1], "--serve-agent") == 0)
	{
		core::set_app_name("libdesktop-platform-smoke");
		return platform::run(std::make_shared<agent_serve>(), argc, argv);
	}

	if (argc == 2 && std::strcmp(argv[1], "--check-embedded") == 0)
	{
		const auto available = !platform::embedded_resource_data("sample.css").empty();
		platform::write_stdout(available ? "Embedded resources: 1 check, 0 failed\n"
		                                 : "Embedded resources: 1 check, 1 failed\n");
		return available ? 0 : 1;
	}

	if (argc == 2 && std::strcmp(argv[1], "--check-ui-pump") == 0)
	{
		core::set_app_name("libdesktop-platform-smoke");
		return platform::run(std::make_shared<ui_pump_check>(), argc, argv);
	}

	if (argc == 2 && std::strcmp(argv[1], "--check-folder-watch") == 0)
	{
		core::set_app_name("libdesktop-platform-smoke");
		return platform::run(std::make_shared<folder_watch_check>(), argc, argv);
	}

	// The copy the agent session's launch tool starts: it leaves its marker, and nothing else.
	if (argc == 3 && std::strcmp(argv[1], "--mark") == 0)
	{
		constexpr std::string_view text = "launched";
		return platform::blob_save_to_file({reinterpret_cast<const uint8_t*>(text.data()), text.size()},
		                                   core::file_path(argv[2]))
		           ? 0
		           : 1;
	}

	return 1;
}
