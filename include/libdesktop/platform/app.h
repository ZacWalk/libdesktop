// This file is part of libdesktop, a C++20 platform layer for desktop applications
// Copyright 2026  Zac Walker
// 
// This program is free software; you can redistribute it and / or modify it
// under the terms of the LGPL License either version 2.1 or later.
// License details are available at https://www.gnu.org/licenses/lgpl-2.1.html
// This program is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY

// Purpose: UI-facing application lifecycle and desktop event services.

#pragma once

namespace platform
{
	enum class sys_command_type
	{
		MINIMIZE,
		MAXIMIZE,
		RESTORE
	};
	enum class os_event_type
	{
		options_changed,
		dpi_changed,
		screen_locked,
		system_device_change,

		// The machine is about to suspend. A suspend is not guaranteed to be followed by a
		// resume, so this is the last chance to persist state, and audio must not survive it.
		system_suspending,

		// Woken from suspend. Volumes, displays and the index may all have moved on.
		system_resumed,

		// The Windows session is ending (shutdown, restart, log off). Persist state without
		// prompting: UI shown here is ignored or force-closed by the shutdown sequence.
		session_ending,

		// The Direct3D device was lost. Release every GPU-backed resource; rendering
		// continues on the CPU software backend for the rest of the session.
		graphics_device_lost
	};

	// Colours supplied by the running desktop theme, kept neutral so UI does not name an OS API.
	enum class system_color
	{
		window,
		window_text,
		desktop
	};

	core::color32 system_color_value(system_color c);

	core::recti desktop_bounds(bool work_area);
	bool is_ui_thread();
}
namespace platform
{
	class window;

	// The application's lifecycle, which run() drives on the UI thread.
	class app : public process_events
	{
	public:
		// Before anything else; false ends the process.
		virtual bool pre_init() { return true; }
		// The whole command line, before any window; true when it asks for a headless mode.
		virtual bool parse_command_line(std::string_view command_line) { return false; }
		// That headless mode, in the console run() has attached; the process's exit code.
		virtual int run_console_mode() { return 0; }
		virtual bool load_settings(const setting_file_ptr& store) { return true; }
		// Makes the application's windows; false ends the process, with the code given to quit if
		// init called it.
		virtual bool init(std::string_view command_line) = 0;
		// A realtime application (a game, an emulator) replaces run's blocking loop with its own after
		// init: run calls main_loop, which pumps with pump_events, and returns its result after teardown.
		virtual bool owns_loop() const { return false; }
		virtual int main_loop() { return 0; }
		virtual void idle() {}
		// Each tick of the frame delay set with set_frame_delay.
		virtual void frame_tick() {}
		virtual void system_event(os_event_type e) {}
		virtual void folder_changed(core::folder_path folder) {}
		virtual bool can_exit() { return true; }
		virtual void exit() {}
		virtual void final_exit() {}
		virtual void fail(std::string_view message, std::string_view more_text) {}
		virtual std::string restart_command_line() { return {}; }
		void crash(core::file_path dump_file_path) override {}
		void save_recovery_state() override {}
	};

	// The application's main calls this and returns what it returns: process setup (COM, DPI
	// awareness, the UI thread, the log, crash handling), the console for a headless mode, the
	// event loop with idle, frame ticks and folder notifications, and teardown.
	int run(std::shared_ptr<app> a, int argc, char** argv);
	// Ends run's loop once the current event has been handled; run then returns exit_code.
	void quit(int exit_code = 0);
	// Handles every pending event, UI tasks included, without blocking; false once the application
	// should stop. For an app whose owns_loop answers true.
	bool pump_events();

	void queue_idle();
	// Milliseconds between frame ticks; 0 stops them.
	void set_frame_delay(int milliseconds);
	void monitor_folders(const std::vector<core::folder_path>& folders);
	void enable_screen_saver(bool enable);
	// How far long-running work has got, on the application's taskbar button; a total of zero is
	// work of unknown size.
	void show_progress(int64_t done, int64_t total);
	void end_progress();
	// Queues task for execution by the UI thread's main or modal loop.
	void run_ui(std::function<void()> task);
	// Delivers e synchronously on the UI thread.
	void send_system_event(os_event_type e);
	// Runs after app::final_exit and before platform COM/OLE teardown.
	void add_shutdown_hook(std::function<void()> hook);

	// What a modal loop's owner sees of input first, wherever it is going: an open menu takes the
	// keyboard, and closes on a press outside it or when the application loses activation.
	class modal_filter
	{
	public:
		virtual ~modal_filter() = default;
		// True when the filter consumed the event.
		virtual bool key_down(int key, key_state keys) { return false; }
		virtual bool pointer_down(window& target, core::pointi screen) { return false; }
		virtual void activation_lost() {}
	};

	// Handles events until done answers true, or the timeout passes; false on the timeout.
	bool run_modal_loop(const std::function<bool()>& done, modal_filter* filter = nullptr,
	                    uint32_t timeout_ms = UINT32_MAX);
}
