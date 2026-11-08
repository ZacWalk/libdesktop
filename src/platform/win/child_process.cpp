// This file is part of libdesktop, a C++20 platform layer for desktop applications
// Copyright 2026  Zac Walker
//
// This program is free software; you can redistribute it and / or modify it
// under the terms of the LGPL License either version 2.1 or later.
// License details are available at https://www.gnu.org/licenses/lgpl-2.1.html
// This program is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY

// Purpose: Windows child processes and their independently drained standard streams.

#include "libdesktop/platform.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <algorithm>
#include <array>
#include <map>
#include <mutex>
#include <thread>

namespace
{
	struct pipe_state
	{
		HANDLE handle = nullptr;
		std::mutex mutex;
		bool eof = false;

		~pipe_state()
		{
			if (handle) CloseHandle(handle);
		}
	};

	void close_handle(HANDLE& handle)
	{
		if (handle)
		{
			CloseHandle(handle);
			handle = nullptr;
		}
	}

	std::wstring environment_value(const wchar_t* name)
	{
		const auto required = GetEnvironmentVariableW(name, nullptr, 0);
		if (!required) return {};
		std::wstring result(required + 1, L'\0');
		const auto copied = GetEnvironmentVariableW(name, result.data(), required + 1);
		if (!copied || copied > required) return {};
		result.resize(copied);
		return result;
	}

	bool make_environment(const std::vector<std::string>& overrides, std::vector<wchar_t>& block)
	{
		std::map<std::wstring, std::wstring> values;
		if (auto* raw = GetEnvironmentStringsW())
		{
			for (auto* item = raw; *item; item += wcslen(item) + 1)
			{
				const std::wstring entry(item);
				const auto equal = entry.find('=', 1);
				if (equal != std::wstring::npos) values[entry.substr(0, equal)] = entry.substr(equal + 1);
			}
			FreeEnvironmentStringsW(raw);
		}
		else return false;

		for (const auto& override_value : overrides)
		{
			const auto equal = override_value.find('=');
			if (equal == std::string::npos || equal == 0 || override_value.find('\0') != std::string::npos) return false;
			values[core::utf8_to_utf16(override_value.substr(0, equal))] =
				core::utf8_to_utf16(override_value.substr(equal + 1));
		}
		for (const auto& [key, value] : values)
		{
			block.insert(block.end(), key.begin(), key.end());
			block.push_back(L'=');
			block.insert(block.end(), value.begin(), value.end());
			block.push_back(L'\0');
		}
		block.push_back(L'\0');
		return true;
	}

	bool create_pipe(HANDLE& parent, HANDLE& child, const bool parent_writes)
	{
		SECURITY_ATTRIBUTES attributes{sizeof(attributes), nullptr, TRUE};
		HANDLE read = nullptr;
		HANDLE write = nullptr;
		if (!CreatePipe(&read, &write, &attributes, 0)) return false;
		if (parent_writes)
		{
			parent = write;
			child = read;
		}
		else
		{
			parent = read;
			child = write;
		}
		if (!SetHandleInformation(parent, HANDLE_FLAG_INHERIT, 0))
		{
			close_handle(parent);
			close_handle(child);
			return false;
		}
		return true;
	}

	std::string process_batch_quote(const std::string_view value)
	{
		auto result = platform::quote_command_arg(std::string(" ") + std::string(value));
		if (result.size() > 1) result.erase(1, 1);
		return result;
	}
}

namespace platform
{
	struct process
	{
		HANDLE child = nullptr;
		HANDLE job = nullptr;
		HANDLE input = nullptr;
		std::mutex input_mutex;
		pipe_state output;
		pipe_state error;
		bool kill_descendants = true;
		std::atomic_bool cancelled = false;

		~process()
		{
			if (child && WaitForSingleObject(child, 0) == WAIT_TIMEOUT)
			{
				cancelled = true;
				if (job) TerminateJobObject(job, 1);
				else TerminateProcess(child, 1);
				WaitForSingleObject(child, INFINITE);
			}
			close_handle(input);
			close_handle(output.handle);
			close_handle(error.handle);
			close_handle(child);
			close_handle(job);
		}
	};

	core::file_path find_executable(const std::string_view name)
	{
		if (name.empty()) return {};
		const auto read_environment = [](const wchar_t* variable)
		{
			return environment_value(variable);
		};
		const auto search_path = read_environment(L"PATH");
		if (search_path.empty()) return {};
		const auto wide_name = core::utf8_to_utf16(name);
		const auto try_path = [&](const std::wstring& candidate) -> core::file_path
		{
			std::wstring result(MAX_PATH * 2, L'\0');
			const auto length = SearchPathW(search_path.c_str(), candidate.c_str(), nullptr,
			                               static_cast<DWORD>(result.size()), result.data(), nullptr);
			if (!length || length >= result.size()) return {};
			result.resize(length);
			return core::file_path(core::utf16_to_utf8(result));
		};

		const auto slash = name.find_last_of("\\/");
		const auto tail = slash == std::string_view::npos ? name : name.substr(slash + 1);
		if (tail.find('.') != std::string_view::npos) return try_path(wide_name);
		auto extensions = read_environment(L"PATHEXT");
		if (extensions.empty()) extensions = L".COM;.EXE;.BAT;.CMD";
		for (size_t pos = 0; pos < extensions.size();)
		{
			const auto end = extensions.find(L';', pos);
			const auto extension = extensions.substr(pos, end == std::wstring::npos ? end : end - pos);
			if (!extension.empty())
				if (auto result = try_path(wide_name + extension); !result.is_empty()) return result;
			if (end == std::wstring::npos) break;
			pos = end + 1;
		}
		return {};
	}

	process_ptr process_spawn(const process_options& options)
	{
		if (options.exe.empty() || options.exe.find('\0') != std::string::npos || options.cwd.find('\0') != std::string::npos ||
			std::any_of(options.args.begin(), options.args.end(), [](const auto& arg) { return arg.find('\0') != std::string::npos; }))
			return {};

		auto executable = options.exe;
		if (executable.find_first_of("\\/") == std::string::npos)
			executable = find_executable(executable).str();
		if (executable.empty()) return {};

		const auto extension = core::to_lower(core::file_path(executable).extension());
		const bool batch = extension == ".bat" || extension == ".cmd";
		std::string command;
		std::wstring application;
		if (batch)
		{
			if (has_shell_metacharacter(executable) ||
				std::any_of(options.args.begin(), options.args.end(), [](const auto& arg) { return has_shell_metacharacter(arg); }))
				return {};
			application = environment_value(L"ComSpec");
			if (application.empty())
			{
				wchar_t system[MAX_PATH]{};
				const auto length = GetSystemDirectoryW(system, MAX_PATH);
				if (!length || length >= MAX_PATH) return {};
				application = std::wstring(system, length) + L"\\cmd.exe";
			}
			command = quote_command_arg(core::utf16_to_utf8(application)) + " /d /s /v:off /c \"" + process_batch_quote(executable);
			for (const auto& arg : options.args) command += " " + process_batch_quote(arg);
			command += '"';
		}
		else
		{
			application = core::utf8_to_utf16(executable);
			command = quote_command_arg(executable);
			for (const auto& arg : options.args) command += " " + quote_command_arg(arg);
		}

		std::vector<wchar_t> environment;
		if (!make_environment(options.env, environment)) return {};
		HANDLE child_input = nullptr;
		HANDLE child_output = nullptr;
		HANDLE child_error = nullptr;
		HANDLE parent_input = nullptr;
		HANDLE parent_output = nullptr;
		HANDLE parent_error = nullptr;
		if (!create_pipe(parent_input, child_input, true) || !create_pipe(parent_output, child_output, false) ||
			!create_pipe(parent_error, child_error, false))
		{
			for (auto* handle :
				{&child_input, &child_output, &child_error, &parent_input, &parent_output, &parent_error})
				close_handle(*handle);
			return {};
		}

		STARTUPINFOW startup{sizeof(startup)};
		startup.dwFlags = STARTF_USESTDHANDLES;
		startup.hStdInput = child_input;
		startup.hStdOutput = child_output;
		startup.hStdError = child_error;
		PROCESS_INFORMATION information{};
		auto wide_command = core::utf8_to_utf16(command);
		const auto wide_cwd = core::utf8_to_utf16(options.cwd);
		const auto created = CreateProcessW(application.c_str(), wide_command.data(), nullptr, nullptr, TRUE,
		                                    CREATE_NO_WINDOW | CREATE_UNICODE_ENVIRONMENT, environment.data(),
		                                    wide_cwd.empty() ? nullptr : wide_cwd.c_str(), &startup, &information);
		close_handle(child_input);
		close_handle(child_output);
		close_handle(child_error);
		if (!created)
		{
			close_handle(parent_input);
			close_handle(parent_output);
			close_handle(parent_error);
			return {};
		}
		close_handle(information.hThread);

		auto result = std::make_shared<process>();
		result->child = information.hProcess;
		result->input = parent_input;
		result->output.handle = parent_output;
		result->error.handle = parent_error;
		result->kill_descendants = options.kill_descendants_on_close;
		if (result->kill_descendants)
		{
			result->job = CreateJobObjectW(nullptr, nullptr);
			JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
			limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
			if (!result->job || !SetInformationJobObject(result->job, JobObjectExtendedLimitInformation, &limits, sizeof(limits)) ||
				!AssignProcessToJobObject(result->job, result->child))
			{
				TerminateProcess(result->child, 1);
				WaitForSingleObject(result->child, INFINITE);
				return {};
			}
		}
		return result;
	}

	size_t process_read(const process_ptr& owner, char* const buffer, const size_t bytes)
	{
		if (!owner || !buffer || !bytes) return 0;
		std::lock_guard lock(owner->output.mutex);
		if (owner->output.eof || !owner->output.handle || owner->cancelled) return 0;
		DWORD read = 0;
		if (!ReadFile(owner->output.handle, buffer, static_cast<DWORD>(std::min<size_t>(bytes, UINT32_MAX)), &read, nullptr) || !read)
		{
			owner->output.eof = true;
			return 0;
		}
		return read;
	}

	size_t process_read_err(const process_ptr& owner, char* const buffer, const size_t bytes)
	{
		if (!owner || !buffer || !bytes) return 0;
		std::lock_guard lock(owner->error.mutex);
		if (owner->error.eof || !owner->error.handle || owner->cancelled) return 0;
		DWORD read = 0;
		if (!ReadFile(owner->error.handle, buffer, static_cast<DWORD>(std::min<size_t>(bytes, UINT32_MAX)), &read, nullptr) || !read)
		{
			owner->error.eof = true;
			return 0;
		}
		return read;
	}

	bool process_write(const process_ptr& owner, const std::string_view text)
	{
		if (!owner) return false;
		std::lock_guard lock(owner->input_mutex);
		if (!owner->input || owner->cancelled) return false;
		size_t offset = 0;
		while (offset < text.size())
		{
			DWORD written = 0;
			if (!WriteFile(owner->input, text.data() + offset,
					static_cast<DWORD>(std::min<size_t>(text.size() - offset, UINT32_MAX)), &written, nullptr) ||
				!written)
				return false;
			offset += written;
		}
		return true;
	}

	void process_close_input(const process_ptr& owner)
	{
		if (!owner) return;
		std::lock_guard lock(owner->input_mutex);
		close_handle(owner->input);
	}

	bool process_alive(const process_ptr& owner)
	{
		if (!owner || !owner->child) return false;
		return WaitForSingleObject(owner->child, 0) == WAIT_TIMEOUT;
	}

	void process_terminate(const process_ptr& owner)
	{
		if (!owner || !owner->child) return;
		owner->cancelled = true;
		if (owner->job) TerminateJobObject(owner->job, 1);
		else if (process_alive(owner)) TerminateProcess(owner->child, 1);
		WaitForSingleObject(owner->child, INFINITE);
	}

	class win_child_process final : public child_process
	{
		process_ptr _process;
		child_process_callbacks _callbacks;
		std::shared_ptr<std::atomic_bool> _cancelled = std::make_shared<std::atomic_bool>(false);
		std::thread _stdout;
		std::thread _stderr;

		void read_lines(const bool error_stream)
		{
			line_splitter splitter;
			std::array<char, 4096> buffer{};
			const auto emit = [this, error_stream](const std::string_view line)
			{
				const auto state = _cancelled;
				const auto callback = error_stream ? _callbacks.on_stderr_line : _callbacks.on_stdout_line;
				if (!callback) return;
				run_ui([state, callback, line = std::string(line)]
				{
					if (!state->load()) callback(line);
				});
			};
			for (;;)
			{
				const auto read = error_stream ? process_read_err(_process, buffer.data(), buffer.size())
				                               : process_read(_process, buffer.data(), buffer.size());
				if (!read) break;
				splitter.feed(std::string_view(buffer.data(), read), emit);
			}
			splitter.flush(emit);
			if (!error_stream)
			{
				while (process_alive(_process) && !_cancelled->load()) std::this_thread::sleep_for(std::chrono::milliseconds(1));
				const auto state = _cancelled;
				const auto callback = _callbacks.on_exit;
				if (callback)
				{
					const auto exit_code = _process->child ? [] (HANDLE child)
					{
						DWORD code = 1;
						GetExitCodeProcess(child, &code);
						return static_cast<int>(code);
					}(_process->child) : 1;
					run_ui([state, callback, exit_code] { if (!state->load()) callback(exit_code); });
				}
			}
		}

	public:
		win_child_process(process_ptr process, child_process_callbacks callbacks)
			: _process(std::move(process)), _callbacks(std::move(callbacks))
		{
			_stdout = std::thread([this] { read_lines(false); });
			_stderr = std::thread([this] { read_lines(true); });
		}

		~win_child_process() override
		{
			_cancelled->store(true);
			process_terminate(_process);
			process_close_input(_process);
			if (_stdout.joinable()) _stdout.join();
			if (_stderr.joinable()) _stderr.join();
		}

		bool write_line(const std::string_view text) override
		{
			if (text.find_first_of("\r\n") != std::string_view::npos) return false;
			std::string line(text);
			line.push_back('\n');
			return process_write(_process, line);
		}
		void close_input() override { process_close_input(_process); }
		void terminate() override { process_terminate(_process); }
		bool is_running() const override { return process_alive(_process); }
	};

	child_process_ptr spawn_child_process(const core::file_path& exe, const std::span<const std::string> args,
	                                      const core::file_path& working_dir, child_process_callbacks callbacks)
	{
		if (exe.is_empty()) return nullptr;
		process_options options;
		options.exe = exe.str();
		options.cwd = working_dir.str();
		options.args.assign(args.begin(), args.end());
		auto process = process_spawn(options);
		return process ? std::make_unique<win_child_process>(std::move(process), std::move(callbacks)) : nullptr;
	}
}