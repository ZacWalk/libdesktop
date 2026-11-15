// This file is part of libdesktop, a C++20 platform layer for desktop applications
// Copyright 2026  Zac Walker
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

// Purpose: The process lifecycle platform::run runs around the window: the console the command-line tools
// write to, the fatal-error message, the crash dump and the unhandled-exception filter that writes it,
// restart and recovery registration with Windows, and the graphics crash guard read at startup.

#include "libdesktop/platform.h"
#include "libdesktop/platform/win32.h"
#include "../agent.h"

#include <io.h>
#include <fcntl.h>

#ifndef WINSTORE
#include <DbgHelp.h>
#endif

std::weak_ptr<platform::process_events> g_process_events;

// Wire up stdout/stderr for the headless console commands (/test, /gen-docs,
// /validate-po). This app is built as a GUI subsystem executable, so it has no
// console by default. When its output is redirected to a pipe or file (e.g.
// `app.exe /test | Out-Host` or `> log.txt`, as CI does), we must bind
// the CRT stdout/stderr to the INHERITED handle so the parent process captures
// it. Reopening "CONOUT$" instead (the old behaviour) sends the output to a
// console screen buffer that a piped/redirected/headless caller never sees --
// which made CI show an empty log. Only attach/allocate a real console when the
// output is NOT redirected (i.e. a genuine interactive run).
void setup_headless_console()
{
	const auto bind_std_stream = [](const DWORD std_handle, FILE* const stream)
	{
		const HANDLE h = GetStdHandle(std_handle);
		const DWORD type = GetFileType(h);

		if (type == FILE_TYPE_PIPE || type == FILE_TYPE_DISK)
		{
			// Redirected: point the CRT stream at the inherited pipe/file.
			const int fd = _open_osfhandle(reinterpret_cast<intptr_t>(h), _O_TEXT);

			// Either failure leaves the stream on its original handle, so the caller still
			// needs the console fallback.
			if (fd == -1)
			{
				return false;
			}

			fflush(stream);
			const int duped = _dup2(fd, _fileno(stream));
			_close(fd);

			if (duped != 0)
			{
				return false;
			}

			setvbuf(stream, nullptr, _IONBF, 0);
			return true;
		}

		return false;
	};

	const bool out_redirected = bind_std_stream(STD_OUTPUT_HANDLE, stdout);
	const bool err_redirected = bind_std_stream(STD_ERROR_HANDLE, stderr);

	if (!out_redirected || !err_redirected)
	{
		// At least one stream is not redirected, so we need a console for it.
		if (!AttachConsole(ATTACH_PARENT_PROCESS))
		{
			AllocConsole();
		}

		FILE* fp = nullptr;
		if (!out_redirected) freopen_s(&fp, "CONOUT$", "w", stdout);
		if (!err_redirected) freopen_s(&fp, "CONOUT$", "w", stderr);
	}

	std::setlocale(LC_ALL, "en_US.UTF-8");
}

namespace
{
	HANDLE agent_input = INVALID_HANDLE_VALUE;
	HANDLE agent_output = INVALID_HANDLE_VALUE;
	HANDLE agent_error = INVALID_HANDLE_VALUE;

	HANDLE duplicate_handle(const HANDLE h)
	{
		if (!h || h == INVALID_HANDLE_VALUE) return INVALID_HANDLE_VALUE;
		const auto process = GetCurrentProcess();
		HANDLE result = INVALID_HANDLE_VALUE;
		return DuplicateHandle(process, h, process, &result, 0, FALSE, DUPLICATE_SAME_ACCESS) ? result
		                                                                                      : INVALID_HANDLE_VALUE;
	}

	bool write_all(const HANDLE h, const std::string_view bytes)
	{
		if (!h || h == INVALID_HANDLE_VALUE) return false;
		size_t offset = 0;

		while (offset < bytes.size())
		{
			const auto wanted = static_cast<DWORD>(std::min<size_t>(bytes.size() - offset, UINT32_MAX));
			DWORD written = 0;
			if (!WriteFile(h, bytes.data() + offset, wanted, &written, nullptr) || written == 0) return false;
			offset += written;
		}

		return true;
	}
}

bool platform::detail::claim_agent_stdio()
{
	agent_input = duplicate_handle(GetStdHandle(STD_INPUT_HANDLE));
	agent_output = duplicate_handle(GetStdHandle(STD_OUTPUT_HANDLE));
	if (agent_input == INVALID_HANDLE_VALUE || agent_output == INVALID_HANDLE_VALUE) return false;

	// Only the protocol reaches the client's end of stdout. Whatever else writes there - the C runtime's stream,
	// and write_stdout's handle - goes to stderr from now on, or nowhere where there is none.
	auto error = GetStdHandle(STD_ERROR_HANDLE);

	if (!error || error == INVALID_HANDLE_VALUE)
	{
		error = CreateFileW(L"NUL", GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0,
		                    nullptr);
		SetStdHandle(STD_ERROR_HANDLE, error);
	}

	agent_error = error;
	SetStdHandle(STD_OUTPUT_HANDLE, error);

	std::fflush(stdout);
	FILE* reopened = nullptr;
	if (_fileno(stdout) < 0) freopen_s(&reopened, "NUL", "w", stdout);

	if (const auto out_fd = _fileno(stdout); out_fd >= 0)
	{
		const auto error_fd = _open_osfhandle(reinterpret_cast<intptr_t>(duplicate_handle(error)), _O_TEXT);

		// Replacing the descriptor closes the stdout handle the runtime held; the protocol keeps its own.
		if (error_fd >= 0)
		{
			_dup2(error_fd, out_fd);
			_close(error_fd);
		}
	}

	setvbuf(stdout, nullptr, _IONBF, 0);
	return true;
}

std::optional<size_t> platform::detail::read_agent_input(char* const buffer, const size_t capacity)
{
	DWORD read = 0;
	if (ReadFile(agent_input, buffer, static_cast<DWORD>(std::min<size_t>(capacity, UINT32_MAX)), &read, nullptr))
		return static_cast<size_t>(read);
	// Woken by interrupt_agent_input rather than by input or its end.
	if (GetLastError() == ERROR_OPERATION_ABORTED) return std::nullopt;
	return 0;
}

bool platform::detail::write_agent_output(const std::string_view bytes)
{
	return write_all(agent_output, bytes);
}

void platform::detail::interrupt_agent_input(std::thread& reader)
{
	CancelSynchronousIo(reader.native_handle());
}

void platform::detail::write_agent_diagnostic(const std::string_view text)
{
	write_all(agent_error, std::string(text) + "\n");
}

#ifndef WINSTORE

static bool create_dump(EXCEPTION_POINTERS* exception_pointers, const core::file_path dump_file_path)
{
	using MINIDUMP_WRITE_DUMP = BOOL(WINAPI*)(IN HANDLE hProcess, IN uint32_t ProcessId, IN HANDLE hFile,
	                                          IN MINIDUMP_TYPE DumpType,
	                                          IN PMINIDUMP_EXCEPTION_INFORMATION ExceptionParam,
	                                          OPTIONAL IN PMINIDUMP_USER_STREAM_INFORMATION UserStreamParam,
	                                          OPTIONAL IN PMINIDUMP_CALLBACK_INFORMATION CallbackParam OPTIONAL);

	auto* const dbg_help = LoadLibrary(L"DBGHELP.DLL");
	auto dump_successful = false;

	if (dbg_help)
	{
		const auto write_dump = (MINIDUMP_WRITE_DUMP)GetProcAddress(dbg_help, "MiniDumpWriteDump");

		if (write_dump)
		{
			const auto w = platform::to_file_system_path(dump_file_path);
			auto* const dump_file = CreateFile(w.c_str(), GENERIC_READ | GENERIC_WRITE,
			                                   FILE_SHARE_WRITE | FILE_SHARE_READ, nullptr, CREATE_ALWAYS, 0, nullptr);

			if (dump_file != INVALID_HANDLE_VALUE)
			{
				MINIDUMP_EXCEPTION_INFORMATION exception_information;
				exception_information.ThreadId = GetCurrentThreadId();
				exception_information.ExceptionPointers = exception_pointers;
				exception_information.ClientPointers = TRUE;

				auto dump_flags = MiniDumpWithDataSegs |        // Include DS from all loaded modules
				                  MiniDumpWithHandleData |      // Include high level OS handle info
				                  MiniDumpScanMemory |          // Scan for pointer references in module list
				                  MiniDumpWithUnloadedModules | // Recently unloaded modules
				                  MiniDumpWithThreadInfo |      // Include thread state information
				                  MiniDumpIgnoreInaccessibleMemory |
				                  // Ignore memory read failures when attempting to read inaccessible regions
				                  MiniDumpNormal; // Normal stack trace info

				dump_successful = write_dump(GetCurrentProcess(), GetCurrentProcessId(), dump_file,
				                             static_cast<MINIDUMP_TYPE>(dump_flags), &exception_information, nullptr,
				                             nullptr);

				CloseHandle(dump_file);
			}
		}
	}

	if (dbg_help)
	{
		FreeLibrary(dbg_help);
	}

	return dump_successful;
}

// How long a thread that faults while another writes its report waits before ending the process itself:
// long enough for a dump and its upload, bounded in case the report needs a lock that thread was holding.
constexpr DWORD second_fault_wait_ms = 60'000;

static LONG WINAPI exception_callback(EXCEPTION_POINTERS* pExceptionPointers)
{
	// One thread reports. MiniDumpWriteDump must not run on two threads at once, and a second faulting
	// thread that returned at once would end the process under the first one's dump and report; it waits
	// instead, and the first ends the process when it is done. A fault on the reporting thread itself, in
	// the handler, ends it at once.
	static std::atomic<DWORD> reporting_thread = 0;
	const auto this_thread = GetCurrentThreadId();
	DWORD unclaimed = 0;

	if (!reporting_thread.compare_exchange_strong(unclaimed, this_thread))
	{
		if (unclaimed != this_thread) Sleep(second_fault_wait_ms);
		return EXCEPTION_CONTINUE_SEARCH;
	}

	const auto app = g_process_events.lock();

	if (app)
	{
		const auto dump_file_path = platform::temp_file();
		const auto dump_created = create_dump(pExceptionPointers, dump_file_path);

		// A failed write still leaves the file create_dump opened.
		if (!dump_created) platform::delete_file(dump_file_path);

		// Reported even when no dump could be written: dbghelp or the temp file can fail, and that
		// is exactly the machine whose next launch would crash again. The handler also records the
		// crashed file in the skip list, which must not depend on the minidump.
		app->crash(dump_created ? dump_file_path : core::file_path{});
	}

	return EXCEPTION_CONTINUE_SEARCH;
}

unhandled_exception_filter::unhandled_exception_filter() : _original(SetUnhandledExceptionFilter(exception_callback))
{
}

unhandled_exception_filter::~unhandled_exception_filter()
{
	SetUnhandledExceptionFilter(_original);
}

#endif // !WINSTORE

// The application recovery/restart APIs live in kernel32 but are not implemented on every
// host. Wine, for example, terminates the process when its unimplemented
// UnregisterApplicationRecoveryCallback stub is called. Resolve them dynamically so a missing
// export degrades to a harmless no-op instead of aborting the app.
namespace
{
	template <typename T> T resolve_kernel32(const char* name)
	{
		const auto h = GetModuleHandleW(L"kernel32.dll");
		return h ? reinterpret_cast<T>(GetProcAddress(h, name)) : nullptr;
	}

	using register_application_restart_t = HRESULT(WINAPI*)(PCWSTR, DWORD);
	using unregister_application_restart_t = HRESULT(WINAPI*)();
	using register_application_recovery_callback_t = HRESULT(WINAPI*)(APPLICATION_RECOVERY_CALLBACK, PVOID, DWORD,
	                                                                  DWORD);
	using unregister_application_recovery_callback_t = HRESULT(WINAPI*)();
	using application_recovery_in_progress_t = HRESULT(WINAPI*)(PBOOL);
	using application_recovery_finished_t = void(WINAPI*)(BOOL);
}

#ifndef WINSTORE
// The desktop build asks to be started again after a crash or a hang, but not after an update or a
// restart of Windows.
constexpr DWORD restart_flags = RESTART_NO_PATCH | RESTART_NO_REBOOT;
#else
// The Store build restarts only after an update. RegisterApplicationRestart re-launches the executable
// directly after a crash or a hang, which for a packaged build would start the process without its
// package identity. An update is different: the Store closes an open app to replace it, and this
// registration is how Microsoft documents a packaged app asking to be started again afterwards.
constexpr DWORD restart_flags = RESTART_NO_CRASH | RESTART_NO_HANG | RESTART_NO_REBOOT;
#endif

static void register_restart(const std::string_view restart_cmd_line)
{
	const auto pRegisterApplicationRestart = resolve_kernel32<register_application_restart_t>(
		"RegisterApplicationRestart");
	if (!pRegisterApplicationRestart) return;

	const auto restart_cmd_line_w = core::utf8_to_utf16(restart_cmd_line);

	// RegisterApplicationRestart rejects anything longer, and wcscpy_s would terminate the process
	// rather than truncate. Losing restart registration is preferable to either.
	if (restart_cmd_line_w.size() >= RESTART_MAX_CMD_LINE)
	{
		core::log(__FUNCTION__, "restart command line too long to register");
		return;
	}

	static WCHAR wsCommandLine[RESTART_MAX_CMD_LINE];
	wcscpy_s(wsCommandLine, restart_cmd_line_w.c_str());
	[[maybe_unused]] const auto hr = pRegisterApplicationRestart(wsCommandLine, restart_flags);
	core::assert_true(SUCCEEDED(hr));
}

void unregister_restart()
{
	const auto pUnregisterApplicationRecoveryCallback = resolve_kernel32<unregister_application_recovery_callback_t>(
		"UnregisterApplicationRecoveryCallback");
	const auto pUnregisterApplicationRestart = resolve_kernel32<unregister_application_restart_t>(
		"UnregisterApplicationRestart");
	if (pUnregisterApplicationRecoveryCallback) pUnregisterApplicationRecoveryCallback();
	if (pUnregisterApplicationRestart) pUnregisterApplicationRestart();
}

static DWORD WINAPI recover_callback(PVOID /*pContext*/)
{
	core::log(__FUNCTION__, "*** recover callback ***");

	BOOL bCanceled = FALSE;
	const auto pApplicationRecoveryInProgress = resolve_kernel32<application_recovery_in_progress_t>(
		"ApplicationRecoveryInProgress");
	if (pApplicationRecoveryInProgress) pApplicationRecoveryInProgress(&bCanceled);

	if (bCanceled)
	{
		core::log(__FUNCTION__, "Recovery was canceled by the user.");
	}

	const auto app = g_process_events.lock();

	if (app)
	{
		app->save_recovery_state();
	}

	platform::close_log();

	const auto pApplicationRecoveryFinished = resolve_kernel32<application_recovery_finished_t>(
		"ApplicationRecoveryFinished");
	if (pApplicationRecoveryFinished) pApplicationRecoveryFinished(bCanceled ? FALSE : TRUE);
	return 0;
}

void setup_restart(const std::string_view restart_cmd_line)
{
	register_restart(restart_cmd_line);

	// The recovery callback runs in both builds: it is what persists the crashed-file skip list
	// and the session recovery state when Windows terminates a hung or crashing process.
	const auto pRegisterApplicationRecoveryCallback = resolve_kernel32<register_application_recovery_callback_t>(
		"RegisterApplicationRecoveryCallback");
	if (!pRegisterApplicationRecoveryCallback) return;
	[[maybe_unused]] const auto hr = pRegisterApplicationRecoveryCallback(recover_callback, nullptr,
	                                                                      RECOVERY_DEFAULT_PING_INTERVAL, 0);
	core::assert_true(SUCCEEDED(hr));
}

void show_fatal_error(const std::string_view message)
{
	core::log(__FUNCTION__, message);

	// Nobody is at an agent session's screen to dismiss a message box, which would hold the process open.
	if (platform::is_agent_session())
	{
		platform::detail::write_agent_diagnostic(
			std::format("{}: {} {}", platform::text().title_error, platform::text().error_cannot_continue, message));
		return;
	}

	std::wstring s;
	s += core::utf8_to_utf16(platform::text().title_error);
	s += L"\n\n";
	s += core::utf8_to_utf16(platform::text().error_cannot_continue);
	s += L"\n\n";
	s += core::utf8_to_utf16(message);

	::MessageBox(nullptr, s.c_str(), core::utf8_to_utf16(core::app_name()).c_str(), MB_OK | MB_ICONHAND);
}

// The app's own failure dialog needs a window and a running message loop. Startup failures happen
// before both exist, so they come back here for a bare message box instead of being queued to a
// loop that is never entered - a silent exit is the one outcome worth ruling out.
void platform::show_startup_failure(const std::string_view message)
{
	show_fatal_error(message);
}
