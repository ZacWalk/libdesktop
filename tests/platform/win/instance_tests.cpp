// This file is part of libdesktop, a C++20 platform layer for desktop applications
// Copyright 2026  Zac Walker
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

// Purpose: Cross-process instance handoffs, receiver lifetime, invalid requests and bounded waits.

#include "libdesktop/platform/win32.h"
#include "test.h"

#include <array>
#include <thread>

namespace
{
	std::vector<core::file_path> requested_files()
	{
		return {core::file_path(R"(C:\Test files\one.jpg)"), core::file_path("D:\\Test files\\caf\xc3\xa9.mp4")};
	}

	std::string receiver_name()
	{
		static unsigned sequence = 0;
		return std::format("instance-tests-{}-{}", GetCurrentProcessId(), ++sequence);
	}

	DWORD send_from_child(const std::string_view name, const bool pump)
	{
		std::array<wchar_t, 32768> executable{};
		GetModuleFileNameW(nullptr, executable.data(), static_cast<DWORD>(executable.size()));
		auto command = std::format(L"\"{}\" --send-instance-test {}", executable.data(), core::utf8_to_utf16(name));
		STARTUPINFOW startup{sizeof(STARTUPINFOW)};
		PROCESS_INFORMATION process{};
		assert_equal(true,
		             CreateProcessW(nullptr, command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr,
		                            nullptr, &startup, &process) != FALSE,
		             "requesting child starts");
		const core::scope_exit close([&]
		{
			CloseHandle(process.hThread);
			CloseHandle(process.hProcess);
		});
		const auto deadline = GetTickCount64() + 10000;
		while (WaitForSingleObject(process.hProcess, 1) == WAIT_TIMEOUT && GetTickCount64() < deadline)
		{
			if (pump)
			{
				MSG message{};
				while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE))
				{
					TranslateMessage(&message);
					DispatchMessageW(&message);
				}
			}
		}
		DWORD result = STILL_ACTIVE;
		GetExitCodeProcess(process.hProcess, &result);
		assert_equal(false, result == STILL_ACTIVE, "requesting child exits within its bounded timeout");
		return result;
	}

	void should_forward_files_between_processes()
	{
		const auto name = receiver_name();
		std::vector<core::file_path> received;
		const auto owner_thread = GetCurrentThreadId();
		DWORD handler_thread = 0;
		auto listener = platform::receive_instance_requests(name, [&](auto files)
		{
			received = std::move(files);
			handler_thread = GetCurrentThreadId();
			return true;
		});
		assert_equal(true, listener.receiver != nullptr && listener.error.empty(), "receiver owns its name");
		const auto duplicate = platform::receive_instance_requests(name, [](auto) { return true; });
		assert_equal(true, duplicate.already_running && !duplicate.receiver && duplicate.error.empty(),
		             "a concurrent receiver cannot take the name");
		auto other = platform::receive_instance_requests(name + "-other", [](auto) { return false; });
		assert_equal(true, other.receiver != nullptr, "another profile is independent");
		assert_equal(0u, send_from_child(name, true), "another process receives acceptance");
		assert_equal(true, received == requested_files(), "spaces, Unicode and file order survive marshalling");
		assert_equal(true, owner_thread == handler_thread, "the receiver's UI thread owns acceptance");
		listener.receiver.reset();
		const auto missing = platform::send_instance_request(name, {}, 0);
		assert_equal(true, missing.status == platform::instance_request_status::not_running,
		             "destruction removes the receiver");
		auto reacquired = platform::receive_instance_requests(name, [](auto) { return true; });
		assert_equal(true, reacquired.receiver != nullptr, "destruction releases the claim");
	}

	void should_decline_invalid_instance_requests()
	{
		const auto name = receiver_name();
		auto calls = 0;
		auto listener = platform::receive_instance_requests(name, [&](auto)
		{
			++calls;
			return false;
		});
		assert_equal(true, listener.receiver != nullptr, "receiver starts");
		const auto files = requested_files();
		const auto declined = platform::send_instance_request(name, files);
		assert_equal(true, declined.status == platform::instance_request_status::declined,
		             "the sender sees a policy refusal");
		assert_equal(1, calls, "the handler sees a valid request once");
		const std::array relative{core::file_path("relative.jpg")};
		const auto invalid = platform::send_instance_request(name, relative);
		assert_equal(true, invalid.status == platform::instance_request_status::failed && !invalid.error.empty(),
		             "a relative path never reaches the handler");
		const std::vector excessive(257, files.front());
		assert_equal(true,
		             platform::send_instance_request(name, excessive).status ==
		                 platform::instance_request_status::failed,
		             "the request has a bounded file count");
		assert_equal(1, calls, "invalid requests do not invoke application code");
		const auto bad_name = platform::receive_instance_requests("bad/name", [](auto) { return true; });
		assert_equal(true, !bad_name.receiver && !bad_name.error.empty(), "invalid names report failure");
	}

	void should_reject_malformed_instance_messages()
	{
		const auto name = receiver_name();
		auto calls = 0;
		auto listener = platform::receive_instance_requests(name, [&](auto)
		{
			++calls;
			return true;
		});
		assert_equal(true, listener.receiver != nullptr, "receiver starts");
		struct lookup
		{
			std::wstring suffix;
			HWND window = nullptr;
		} found{L"." + core::utf8_to_utf16(name)};
		EnumThreadWindows(GetCurrentThreadId(), [](HWND window, LPARAM data) -> BOOL
		{
			auto& match = *reinterpret_cast<lookup*>(data);
			std::array<wchar_t, 256> title{};
			GetWindowTextW(window, title.data(), static_cast<int>(title.size()));
			if (std::wstring_view(title.data()).ends_with(match.suffix))
			{
				match.window = window;
				return FALSE;
			}
			return TRUE;
		}, reinterpret_cast<LPARAM>(&found));
		assert_equal(true, found.window != nullptr, "the receiver has its own discoverable window");
		auto payload = std::wstring(L"C:\\file.jpg") + L'\0' + L'\0';
		COPYDATASTRUCT data{0x4c444901, static_cast<DWORD>(payload.size() * sizeof(wchar_t)), payload.data()};
		data.dwData = 0;
		assert_equal(true, SendMessageW(found.window, WM_COPYDATA, 0, reinterpret_cast<LPARAM>(&data)) == FALSE,
		             "unknown protocol versions are refused");
		data.dwData = 0x4c444901;
		data.cbData -= 1;
		assert_equal(true, SendMessageW(found.window, WM_COPYDATA, 0, reinterpret_cast<LPARAM>(&data)) == FALSE,
		             "partial UTF-16 code units are refused");
		data.cbData = static_cast<DWORD>((payload.size() - 2) * sizeof(wchar_t));
		assert_equal(true, SendMessageW(found.window, WM_COPYDATA, 0, reinterpret_cast<LPARAM>(&data)) == FALSE,
		             "unterminated lists are refused");
		payload = std::wstring(L"-movie-render:project.dfv") + L'\0' + L'\0';
		data.cbData = static_cast<DWORD>(payload.size() * sizeof(wchar_t));
		data.lpData = payload.data();
		assert_equal(true, SendMessageW(found.window, WM_COPYDATA, 0, reinterpret_cast<LPARAM>(&data)) == FALSE,
		             "switches cannot be delivered as file requests");
		assert_equal(0, calls, "malformed messages never invoke application code");
	}

	void should_bound_an_unresponsive_instance_request()
	{
		const auto name = receiver_name();
		auto calls = 0;
		auto listener = platform::receive_instance_requests(name, [&](auto)
		{
			++calls;
			return true;
		});
		assert_equal(true, listener.receiver != nullptr, "receiver starts");
		platform::instance_request_result result;
		// The receiving thread joins rather than pumping, so a send cannot be dispatched before its timeout.
		std::thread sender([&] { result = platform::send_instance_request(name, requested_files(), 50); });
		sender.join();
		assert_equal(true, result.status == platform::instance_request_status::timed_out && !result.error.empty(),
		             "an unresponsive receiver cannot hang the launcher");
		assert_equal(0, calls, "a timed-out undispatched request is not accepted");
	}
}

std::optional<int> run_instance_test_child(const int argc, char* argv[])
{
	if (argc != 3 || std::string_view(argv[1]) != "--send-instance-test") return {};
	const auto result = platform::send_instance_request(argv[2], requested_files());
	return result.status == platform::instance_request_status::accepted ? 0 : 1;
}

void register_instance_tests(test_registry& tests)
{
	tests.add("Should forward files between processes", should_forward_files_between_processes);
	tests.add("Should decline invalid instance requests", should_decline_invalid_instance_requests);
	tests.add("Should reject malformed instance messages", should_reject_malformed_instance_messages);
	tests.add("Should bound an unresponsive instance request", should_bound_an_unresponsive_instance_request);
}
