// This file is part of libdesktop, a C++20 platform layer for desktop applications
// Copyright 2026  Zac Walker
//
// This program is free software; you can redistribute it and / or modify it
// under the terms of the LGPL License either version 2.1 or later.
// License details are available at https://www.gnu.org/licenses/lgpl-2.1.html
// This program is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY

// Purpose: Background workers, serialized executors, and explicit UI task pumping.

#pragma once

namespace platform
{
	// Lazily starts the shared four-worker pool. Empty work and submissions after shutdown are ignored.
	void run_async(std::function<void()> task);

	class serial_executor
	{
	public:
		virtual ~serial_executor() = default;
		// Rejects empty tasks and submissions after stop; accepted tasks run FIFO.
		virtual bool post(std::function<void()> task) = 0;
		// Stops accepting work, drains accepted tasks, and joins unless called by the worker itself.
		virtual void stop() = 0;
	};

	using serial_executor_ptr = std::unique_ptr<serial_executor>;

	// Starts a dedicated worker immediately; nullptr when the worker cannot be created.
	std::unique_ptr<serial_executor> create_serial_executor(std::string_view name = {});

	// Runs every queued run_ui callback on the calling thread, for headless code and tests.
	void pump_ui_tasks();
	// Waits up to timeout_ms for the first task, then runs every queued one: for a headless caller
	// waiting on work another thread posts back.
	void pump_ui_tasks(uint32_t timeout_ms);

	// Monotonic seconds shared by animation and background-work clients.
	double monotonic_seconds();
}