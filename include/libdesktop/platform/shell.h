// This file is part of libdesktop, a C++20 platform layer for desktop applications
// Copyright 2026  Zac Walker
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

// Purpose: Shell integration, single-instance coordination, spelling, and file drag operations.

#pragma once

namespace platform
{
	class window;

	enum class tray_balloon_kind
	{
		info,
		warning,
		error
	};

	struct tray_icon_image
	{
		std::span<const uint32_t> bgra_pixels;
		core::sizei extent;
	};

	struct tray_callbacks
	{
		std::function<void()> on_activate;
		std::function<void(core::pointi screen_position, bool keyboard_anchor)> on_context_menu;
		std::function<void()> on_balloon_click;
		std::function<void(bool session_ending)> on_end_session;
	};

	class tray_service
	{
	public:
		virtual ~tray_service() = default;
		virtual bool available() const = 0;
		virtual std::string diagnostics() const = 0;
		virtual bool set_icon(tray_icon_image image) = 0;
		virtual bool set_tooltip(std::string_view text) = 0;
		virtual bool show() = 0;
		virtual bool hide() = 0;
		virtual bool show_balloon(std::string_view title, std::string_view text,
		                          tray_balloon_kind kind = tray_balloon_kind::info) = 0;
	};

	using tray_service_ptr = std::shared_ptr<tray_service>;

	// A tray service is optional; callbacks own the application's restore, menu, and exit policy.
	tray_service_ptr create_tray_service(tray_icon_image image, std::string_view tooltip,
	                                     tray_callbacks callbacks = {});
	// Test seam for the publication step. It does not make an otherwise unavailable service available.
	void test_set_tray_publication_failure(bool fail);

	class instance_lock
	{
	public:
		virtual ~instance_lock() = default;
	};

	using instance_lock_ptr = std::unique_ptr<instance_lock>;

	struct instance_lock_result
	{
		instance_lock_ptr lock;
		bool already_running = false;
		std::string error;
	};

	// The lock is held by object lifetime, not by the thread that acquired it.
	instance_lock_result try_lock_instance(std::string_view name);
	// Coordinates with a legacy/native mutex name without exposing a native handle.
	instance_lock_result try_lock_named_instance(std::string_view name);
	// The callback is queued through platform::run_ui before it is invoked.
	bool register_instance_signal(std::string_view name, std::function<void()> callback);
	// Broadcasts the named second-launch signal; true means the OS accepted the post.
	bool signal_existing_instance(std::string_view name);

	enum class instance_request_status
	{
		accepted,
		declined,
		not_running,
		timed_out,
		unavailable,
		failed
	};

	struct instance_request_result
	{
		instance_request_status status = instance_request_status::failed;
		std::string error;
	};

	class instance_receiver
	{
	public:
		virtual ~instance_receiver() = default;
	};

	using instance_receiver_ptr = std::unique_ptr<instance_receiver>;

	struct instance_receiver_result
	{
		instance_receiver_ptr receiver;
		bool already_running = false;
		std::string error;
	};

	bool can_receive_instance_requests();
	// Owns a per-user, per-session name until destruction, on the creating UI thread.
	// The handler must only accept and queue work: it may be called inside a modal loop.
	// Files are absolute paths; an empty list requests activation alone.
	instance_receiver_result receive_instance_requests(std::string_view name,
	                                                   std::function<bool(std::vector<core::file_path>)> handler);
	// Waits at most timeout_ms for a starting receiver and its acknowledgement.
	instance_request_result send_instance_request(std::string_view name, std::span<const core::file_path> files,
	                                              uint32_t timeout_ms = 3000);

	class spell_checker
	{
	public:
		virtual ~spell_checker() = default;
		virtual bool available() const = 0;
		virtual std::string diagnostics() const = 0;
		virtual bool check_word(std::string_view word) = 0;
		virtual std::vector<std::string> suggest(std::string_view word) = 0;
		virtual void add_word(std::string_view word) = 0;
	};

	// Returns null when the operating system has no usable dictionary or checker.
	std::shared_ptr<spell_checker> create_spell_checker(std::string_view language = {});

	// Starts the native file drag loop. It is synchronous, like the shell's DoDragDrop operation.
	bool begin_file_drag(window* owner, std::span<const core::file_path> files);
}
