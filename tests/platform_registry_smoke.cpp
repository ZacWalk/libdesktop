// This file is part of libdesktop, a C++20 platform layer for desktop applications
// Copyright 2026  Zac Walker
//
// This program is free software; you can redistribute it and / or modify it
// under the terms of the LGPL License either version 2.1 or later.
// License details are available at https://www.gnu.org/licenses/lgpl-2.1.html
// This program is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY

// Purpose: A tiny executable smoke-check for libdesktop_add_app(), embedded resources, and the UI task
// queue of an application platform::run started.

#include "libdesktop/platform.h"

#include <cstring>
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
			return delivered ? 0 : 1;
		}

		bool init(std::string_view) override { return false; }
		void crash(core::file_path) override {}
		void save_recovery_state() override {}
	};
}

int main(const int argc, char** argv)
{
	if (argc == 2 && std::strcmp(argv[1], "--check-embedded") == 0)
		return platform::embedded_resource_data("sample.css").empty() ? 1 : 0;

	if (argc == 2 && std::strcmp(argv[1], "--check-ui-pump") == 0)
	{
		core::set_app_name("libdesktop-platform-smoke");
		return platform::run(std::make_shared<ui_pump_check>(), argc, argv);
	}

	return 1;
}