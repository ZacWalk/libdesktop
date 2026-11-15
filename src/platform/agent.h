// This file is part of libdesktop, a C++20 platform layer for desktop applications
// Copyright 2026  Zac Walker
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

// Purpose: Internal to platform: the agent session platform::run serves (app::agent_server), and the half of its
// transport each OS supplies.

#pragma once

#include "libdesktop/core/mcp.h"

namespace platform::detail
{
	// Each OS's half, in win/process.cpp and linux/platform.cpp. Claiming keeps the process's stdin and stdout for
	// the protocol, and points stdout - printf's and write_stdout's alike - at stderr from then on.
	bool claim_agent_stdio();
	// Blocks for input: what was read, nothing at the end of input, or nullopt when the read was woken without any.
	std::optional<size_t> read_agent_input(char* buffer, size_t capacity);
	bool write_agent_output(std::string_view bytes);
	// Wakes a read the thread is blocked in, where the OS can; a read that cannot be woken times out by itself.
	void interrupt_agent_input(std::thread& reader);
	// What the session says on stderr, which a client may show or ignore.
	void write_agent_diagnostic(std::string_view text);
	// An agent session shows no system dialog or window - nobody is there to answer one - so each is answered as the
	// user's cancelling it would be. True in such a session, once it has logged what was not shown.
	bool agent_skips(std::string_view what);
	// Nor does it open another program, a folder or a web page - one would open on the user's desktop, beyond the
	// session - unless the application has allowed it (platform::allow_agent_to_open). True where it is skipped, once it
	// has logged what was not opened.
	bool agent_skips_opening(std::string_view what);

	class agent_session
	{
	public:
		explicit agent_session(std::shared_ptr<core::mcp::server> server);
		~agent_session();
		agent_session(const agent_session&) = delete;
		agent_session& operator=(const agent_session&) = delete;

		// Once the application is ready to answer: init has returned.
		void start_reading();
		// Before the application tears down: what is running is cancelled and nothing more is read.
		void stop();

	private:
		void read_loop();

		std::shared_ptr<core::mcp::server> _server;
		std::thread _reader;
		std::atomic_bool _stopping = false;
		std::atomic_bool _reader_done = false;
	};

	// Null when there is no stdin or stdout to serve on.
	std::unique_ptr<agent_session> start_agent_session(std::shared_ptr<core::mcp::server> server);
	// When run has finished with the session.
	void end_agent_session();
}
