// This file is part of libdesktop, a C++20 platform layer for desktop applications
// Copyright 2026  Zac Walker
//
// This program is free software; you can redistribute it and / or modify it
// under the terms of the LGPL License either version 2.1 or later.
// License details are available at https://www.gnu.org/licenses/lgpl-2.1.html
// This program is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY

// Purpose: Shell locks, tray publication, and unavailable-platform shell contracts.

#include "libdesktop/platform.h"
#include "test.h"

#include <array>
#include <chrono>
#include <format>
#include <thread>

using namespace std::literals;

namespace
{
	void should_report_tray_publication_failure()
	{
		const std::array<uint32_t, 1> pixel = {0xff336699};
#ifdef _WIN32
		platform::test_set_tray_publication_failure(true);
		const auto tray = platform::create_tray_service({pixel, {1, 1}}, "libdesktop-test");
		assert_equal(true, tray != nullptr, "tray service exists");
		if (tray)
		{
			assert_equal(true, tray->available(), "tray window and icon are available");
			assert_equal(true, !tray->diagnostics().empty(), "tray publication reports its failure");
		}
		platform::test_set_tray_publication_failure(false);
#else
		const auto tray = platform::create_tray_service({pixel, {1, 1}}, "libdesktop-test");
		assert_equal(true, tray == nullptr, "Linux reports tray unavailable");
#endif
	}

	void should_hold_instance_lock_until_release()
	{
		const auto name = std::format("platform-shell-tests-{}",
		                             std::chrono::steady_clock::now().time_since_epoch().count());
		{
			auto first = platform::try_lock_instance(name);
			assert_equal(true, first.lock != nullptr, "first instance lock acquired");
			assert_equal(false, first.already_running, "first instance is not a duplicate");
			assert_equal(true, first.error.empty(), "first instance lock has no error");

			auto duplicate = platform::try_lock_instance(name);
			assert_equal(true, duplicate.lock == nullptr, "duplicate instance has no lock object");
			assert_equal(true, duplicate.already_running, "duplicate instance is reported");
			assert_equal(true, duplicate.error.empty(), "duplicate instance has no error");

			platform::instance_lock_result concurrent;
			std::thread contender([&] { concurrent = platform::try_lock_instance(name); });
			contender.join();
			assert_equal(true, concurrent.lock == nullptr, "concurrent instance has no lock object");
			assert_equal(true, concurrent.already_running, "concurrent instance is reported");

			auto different = platform::try_lock_instance(name + "-other");
			assert_equal(true, different.lock != nullptr, "different instance name is independent");
			assert_equal(false, different.already_running, "different instance is not running");
		}

		auto reacquired = platform::try_lock_instance(name);
		assert_equal(true, reacquired.lock != nullptr, "lock can be reacquired after release");
		assert_equal(false, reacquired.already_running, "released lock is no longer running");
		std::thread releaser([lock = std::move(reacquired.lock)]() mutable { lock.reset(); });
		releaser.join();

		for (const std::string invalid : {"", "a b", "a\\b", "a/b", "\xc3\xa9"})
		{
			const auto result = platform::try_lock_instance(invalid);
			assert_equal(true, result.lock == nullptr, "invalid instance has no lock");
			assert_equal(false, result.already_running, "invalid instance is not a duplicate");
			assert_equal(true, !result.error.empty(), "invalid instance reports an error");
		}
		for (const auto& invalid : {std::string(129, 'x'), std::string("a\0b", 3)})
		{
			const auto result = platform::try_lock_instance(invalid);
			assert_equal(true, result.lock == nullptr, "oversize instance has no lock");
			assert_equal(true, !result.error.empty(), "oversize instance reports an error");
		}
	}

	void should_distinguish_system_events()
	{
		assert_equal(true, platform::os_event_type::system_device_change != platform::os_event_type::session_ending,
		             "system event kinds remain distinct");
		assert_equal(true, platform::os_event_type::system_suspending != platform::os_event_type::system_resumed,
		             "suspend and resume remain distinct");
	}

	void should_report_unavailable_shell_services_honestly()
	{
		const auto checker = platform::create_spell_checker();
#ifdef _WIN32
		assert_equal(true, !checker || checker->available(), "Windows checker is available when returned");
#else
		assert_equal(true, checker == nullptr, "Linux checker is unavailable");
#endif
		assert_equal(false, platform::begin_file_drag(nullptr, {}), "empty file drag does not start");
		assert_equal(0, platform::show_choice(nullptr, {}), "empty choice is cancelled");
	}
}

void register_platform_shell_tests(test_registry& tests)
{
	tests.add("Should report tray publication failure"s, should_report_tray_publication_failure);
	tests.add("Should hold instance lock until release"s, should_hold_instance_lock_until_release);
	tests.add("Should distinguish system events"s, should_distinguish_system_events);
	tests.add("Should report unavailable shell services honestly"s, should_report_unavailable_shell_services_honestly);
}
