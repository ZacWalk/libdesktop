// This file is part of libdesktop, a C++20 platform layer for desktop applications
// Copyright 2026  Zac Walker
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

// Purpose: The agent session platform::run serves (app::agent_server): the protocol on stdin and stdout, one message
// a line, read on a thread of its own and answered from any thread, with the server's handlers run on the UI thread.
// The end of input ends the session. The OS's half of the transport is in win/process.cpp and linux/platform.cpp.

#include "libdesktop/platform.h"
#include "agent.h"

namespace
{
	std::atomic_bool agent_active = false;
	std::atomic_bool agent_opens = false;

	// A read that cannot be woken is waited for this long at most before the session lets it go.
	constexpr int stop_attempts = 100;
	constexpr auto stop_interval = std::chrono::milliseconds(10);
}

bool platform::is_agent_session()
{
	return agent_active.load();
}

void platform::allow_agent_to_open(const bool allowed)
{
	agent_opens = allowed;
}

namespace platform::detail
{
	bool agent_skips(const std::string_view what)
	{
		if (!platform::is_agent_session()) return false;
		core::log("agent session", std::format("Not shown, as nobody is there to answer it: {}", what));
		return true;
	}

	bool agent_skips_opening(const std::string_view what)
	{
		if (!platform::is_agent_session() || agent_opens.load()) return false;
		core::log("agent session", std::format("Not opened, as the session may not open other programs: {}", what));
		return true;
	}

	agent_session::agent_session(std::shared_ptr<core::mcp::server> server) : _server(std::move(server))
	{
		_server->connect([](const std::string_view message)
		{
			std::string line;
			line.reserve(message.size() + 1);
			line.append(message);
			line.push_back('\n');
			if (!write_agent_output(line)) core::log("agent session", "The client has stopped reading");
		}, [](std::function<void()> task) { platform::run_ui(std::move(task)); });
	}

	agent_session::~agent_session()
	{
		stop();
	}

	void agent_session::start_reading()
	{
		if (_reader.joinable() || _stopping) return;
		_reader = std::thread([this] { read_loop(); });
	}

	void agent_session::stop()
	{
		if (_stopping.exchange(true)) return;
		_server->close();

		if (!_reader.joinable()) return;

		for (auto attempt = 0; attempt < stop_attempts && !_reader_done; ++attempt)
		{
			interrupt_agent_input(_reader);
			std::this_thread::sleep_for(stop_interval);
		}

		// A console's read cannot always be woken. The process is ending, and the reader delivers nothing more.
		if (_reader_done) _reader.join();
		else _reader.detach();
	}

	void agent_session::read_loop()
	{
		core::set_thread_description("agent input");

		platform::line_splitter lines;
		lines.max_line_bytes = core::mcp::server::max_message_bytes;
		std::vector<char> buffer(64 * 1024);
		const auto server = _server;
		const auto deliver = [this, &server](const std::string_view line)
		{
			if (!_stopping) server->receive(line);
		};

		while (!_stopping)
		{
			const auto read = read_agent_input(buffer.data(), buffer.size());
			if (!read) continue;
			if (*read == 0) break;
			lines.feed(std::string_view(buffer.data(), *read), deliver);
		}

		if (!_stopping)
		{
			lines.flush(deliver);

			// The client closed the input, which is how it ends the session.
			server->close();
			platform::run_ui([] { platform::quit(0); });
		}

		_reader_done = true;
	}

	std::unique_ptr<agent_session> start_agent_session(std::shared_ptr<core::mcp::server> server)
	{
		if (!server || !claim_agent_stdio()) return nullptr;
		agent_active = true;
		core::log("agent session",
		          std::format("Serving {} {} on stdin and stdout", server->info().name, server->info().version));
		return std::make_unique<agent_session>(std::move(server));
	}

	void end_agent_session()
	{
		agent_active = false;
	}
}
