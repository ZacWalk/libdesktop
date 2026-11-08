// This file is part of libdesktop, a C++20 platform layer for desktop applications
// Copyright 2026  Zac Walker
//
// This program is free software; you can redistribute it and / or modify it
// under the terms of the LGPL License either version 2.1 or later.
// License details are available at https://www.gnu.org/licenses/lgpl-2.1.html
// This program is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY

// Purpose: POSIX child processes with independent pipes and UI-thread line callbacks.

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "libdesktop/platform.h"

#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <spawn.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <array>
#include <cerrno>
#include <cstdlib>
#include <map>
#include <mutex>
#include <thread>

extern char** environ;

namespace
{
	struct pipe_state
	{
		int fd = -1;
		std::mutex mutex;
		bool eof = false;

		~pipe_state()
		{
			if (fd >= 0) ::close(fd);
		}
	};

	void close_fd(int& fd)
	{
		if (fd >= 0)
		{
			::close(fd);
			fd = -1;
		}
	}

	bool valid_executable(const std::string& path)
	{
		struct stat info{};
		return ::stat(path.c_str(), &info) == 0 && S_ISREG(info.st_mode) && ::access(path.c_str(), X_OK) == 0;
	}

	std::vector<std::string> environment_values(const std::vector<std::string>& overrides)
	{
		std::map<std::string, std::string> values;
		for (auto* item = environ; item && *item; ++item)
		{
			const std::string entry(*item);
			const auto equal = entry.find('=');
			if (equal != std::string::npos) values[entry.substr(0, equal)] = entry.substr(equal + 1);
		}
		for (const auto& entry : overrides)
		{
			const auto equal = entry.find('=');
			if (equal == std::string::npos || equal == 0 || entry.find('\0') != std::string::npos) return {};
			values[entry.substr(0, equal)] = entry.substr(equal + 1);
		}
		std::vector<std::string> result;
		result.reserve(values.size());
		for (const auto& [key, value] : values) result.push_back(key + '=' + value);
		return result;
	}
}

namespace platform
{
	struct process
	{
		pid_t pid = -1;
		pid_t process_group = -1;
		int input = -1;
		std::mutex input_mutex;
		pipe_state output;
		pipe_state error;
		bool kill_descendants = true;
		std::atomic_bool cancelled = false;
		int exit_status = 0;
		bool waited = false;
		std::mutex lifecycle;

		~process()
		{
			std::lock_guard lock(lifecycle);
			if (pid > 0 && !waited)
			{
				int status = 0;
				const auto state = ::waitpid(pid, &status, WNOHANG);
				if (state == 0)
				{
					if (kill_descendants && process_group > 0) ::kill(-process_group, SIGKILL);
					else ::kill(pid, SIGKILL);
					while (::waitpid(pid, &exit_status, 0) < 0 && errno == EINTR) {}
				}
				else if (state == pid) exit_status = status;
			}
			close_fd(input);
		}
	};

	core::file_path find_executable(const std::string_view name)
	{
		if (name.empty()) return {};
		const std::string value(name);
		if (value.find('/') != std::string::npos)
			return valid_executable(value) ? core::file_path(value) : core::file_path{};
		const auto* const raw_path = std::getenv("PATH");
		if (!raw_path || !*raw_path) return {};
		std::string_view path(raw_path);
		while (!path.empty())
		{
			const auto separator = path.find(':');
			const auto folder = path.substr(0, separator);
			// Empty PATH components mean the current directory; skip them deliberately.
			if (!folder.empty())
			{
				const auto candidate = std::string(folder) + '/' + value;
				if (valid_executable(candidate)) return core::file_path(candidate);
			}
			if (separator == std::string_view::npos) break;
			path.remove_prefix(separator + 1);
		}
		return {};
	}

	process_ptr process_spawn(const process_options& options)
	{
		if (options.exe.empty() || options.exe.find('\0') != std::string::npos || options.cwd.find('\0') != std::string::npos ||
			std::any_of(options.args.begin(), options.args.end(), [](const auto& value) { return value.find('\0') != std::string::npos; }))
			return {};
		auto executable = options.exe;
		if (executable.find('/') == std::string::npos) executable = find_executable(executable).str();
		if (executable.empty()) return {};
		if (options.cwd.empty() == false && options.cwd.find('\0') != std::string::npos) return {};

		const auto environment = environment_values(options.env);
		if (environment.empty() && environ && *environ && !options.env.empty()) return {};
		std::vector<char*> argv;
		argv.reserve(options.args.size() + 2);
		argv.push_back(const_cast<char*>(executable.c_str()));
		for (const auto& arg : options.args) argv.push_back(const_cast<char*>(arg.c_str()));
		argv.push_back(nullptr);
		std::vector<char*> envp;
		envp.reserve(environment.size() + 1);
		for (const auto& value : environment) envp.push_back(const_cast<char*>(value.c_str()));
		envp.push_back(nullptr);

		int input_pipe[2] = {-1, -1};
		int output_pipe[2] = {-1, -1};
		int error_pipe[2] = {-1, -1};
		if (::pipe2(input_pipe, O_CLOEXEC) != 0 || ::pipe2(output_pipe, O_CLOEXEC) != 0 || ::pipe2(error_pipe, O_CLOEXEC) != 0)
		{
			for (auto& fd : input_pipe) close_fd(fd);
			for (auto& fd : output_pipe) close_fd(fd);
			for (auto& fd : error_pipe) close_fd(fd);
			return {};
		}

		posix_spawn_file_actions_t actions{};
		if (posix_spawn_file_actions_init(&actions) != 0)
		{
			for (auto& fd : input_pipe) close_fd(fd);
			for (auto& fd : output_pipe) close_fd(fd);
			for (auto& fd : error_pipe) close_fd(fd);
			return {};
		}
		const auto finish_actions = [&] { posix_spawn_file_actions_destroy(&actions); };
		bool actions_ok = posix_spawn_file_actions_adddup2(&actions, input_pipe[0], STDIN_FILENO) == 0 &&
			posix_spawn_file_actions_adddup2(&actions, output_pipe[1], STDOUT_FILENO) == 0 &&
			posix_spawn_file_actions_adddup2(&actions, error_pipe[1], STDERR_FILENO) == 0;
		for (const auto fd : {input_pipe[0], input_pipe[1], output_pipe[0], output_pipe[1], error_pipe[0], error_pipe[1]})
			if (fd != STDIN_FILENO && fd != STDOUT_FILENO && fd != STDERR_FILENO)
				actions_ok = actions_ok && posix_spawn_file_actions_addclose(&actions, fd) == 0;
#ifdef __GLIBC__
		if (actions_ok && !options.cwd.empty()) actions_ok = posix_spawn_file_actions_addchdir_np(&actions, options.cwd.c_str()) == 0;
#else
		if (!options.cwd.empty()) actions_ok = false;
#endif
		if (!actions_ok)
		{
			finish_actions();
			for (auto& fd : input_pipe) close_fd(fd);
			for (auto& fd : output_pipe) close_fd(fd);
			for (auto& fd : error_pipe) close_fd(fd);
			return {};
		}

		posix_spawnattr_t attributes{};
		if (posix_spawnattr_init(&attributes) != 0)
		{
			finish_actions();
			for (auto& fd : input_pipe) close_fd(fd);
			for (auto& fd : output_pipe) close_fd(fd);
			for (auto& fd : error_pipe) close_fd(fd);
			return {};
		}
		short flags = 0;
#ifdef POSIX_SPAWN_SETPGROUP
		if (options.kill_descendants_on_close)
		{
			flags |= POSIX_SPAWN_SETPGROUP;
			posix_spawnattr_setpgroup(&attributes, 0);
		}
#endif
		posix_spawnattr_setflags(&attributes, flags);
		pid_t child = -1;
		const auto spawned = posix_spawn(&child, executable.c_str(), &actions, &attributes, argv.data(), envp.data());
		posix_spawnattr_destroy(&attributes);
		finish_actions();
		close_fd(input_pipe[0]);
		close_fd(output_pipe[1]);
		close_fd(error_pipe[1]);
		if (spawned != 0)
		{
			close_fd(input_pipe[1]);
			close_fd(output_pipe[0]);
			close_fd(error_pipe[0]);
			return {};
		}

		auto result = std::make_shared<process>();
		result->pid = child;
		result->process_group = options.kill_descendants_on_close ? child : -1;
		result->input = input_pipe[1];
		result->output.fd = output_pipe[0];
		result->error.fd = error_pipe[0];
		result->kill_descendants = options.kill_descendants_on_close;
		return result;
	}

	size_t process_read(const process_ptr& owner, char* const buffer, const size_t bytes)
	{
		if (!owner || !buffer || !bytes) return 0;
		std::lock_guard lock(owner->output.mutex);
		if (owner->output.eof || owner->output.fd < 0 || owner->cancelled) return 0;
		ssize_t count = 0;
		do count = ::read(owner->output.fd, buffer, bytes); while (count < 0 && errno == EINTR);
		if (count <= 0) owner->output.eof = true;
		return count > 0 ? static_cast<size_t>(count) : 0;
	}

	size_t process_read_err(const process_ptr& owner, char* const buffer, const size_t bytes)
	{
		if (!owner || !buffer || !bytes) return 0;
		std::lock_guard lock(owner->error.mutex);
		if (owner->error.eof || owner->error.fd < 0 || owner->cancelled) return 0;
		ssize_t count = 0;
		do count = ::read(owner->error.fd, buffer, bytes); while (count < 0 && errno == EINTR);
		if (count <= 0) owner->error.eof = true;
		return count > 0 ? static_cast<size_t>(count) : 0;
	}

	bool process_write(const process_ptr& owner, const std::string_view text)
	{
		if (!owner) return false;
		std::lock_guard lock(owner->input_mutex);
		if (owner->input < 0 || owner->cancelled) return false;
		size_t offset = 0;
		while (offset < text.size())
		{
			const auto count = ::write(owner->input, text.data() + offset, text.size() - offset);
			if (count < 0 && errno == EINTR) continue;
			if (count <= 0) return false;
			offset += static_cast<size_t>(count);
		}
		return true;
	}

	void process_close_input(const process_ptr& owner)
	{
		if (!owner) return;
		std::lock_guard lock(owner->input_mutex);
		close_fd(owner->input);
	}

	bool process_alive(const process_ptr& owner)
	{
		if (!owner) return false;
		std::lock_guard lock(owner->lifecycle);
		if (owner->pid <= 0 || owner->waited) return false;
		int status = 0;
		const auto result = ::waitpid(owner->pid, &status, WNOHANG);
		if (result == owner->pid)
		{
			owner->exit_status = status;
			owner->waited = true;
			return false;
		}
		return result == 0;
	}

	void process_terminate(const process_ptr& owner)
	{
		if (!owner) return;
		owner->cancelled = true;
		{
			std::lock_guard lock(owner->lifecycle);
			if (owner->pid > 0 && !owner->waited)
			{
				int status = 0;
				const auto state = ::waitpid(owner->pid, &status, WNOHANG);
				if (state == 0)
				{
					if (owner->kill_descendants && owner->process_group > 0) ::kill(-owner->process_group, SIGKILL);
					else ::kill(owner->pid, SIGKILL);
					while (::waitpid(owner->pid, &owner->exit_status, 0) < 0 && errno == EINTR) {}
				}
				else if (state == owner->pid) owner->exit_status = status;
				owner->waited = true;
			}
		}
		{
			std::lock_guard lock(owner->output.mutex);
			close_fd(owner->output.fd);
		}
		{
			std::lock_guard lock(owner->error.mutex);
			close_fd(owner->error.fd);
		}
	}

	int process_exit_code(const process_ptr& owner)
	{
		if (!owner) return 1;
		process_alive(owner);
		std::lock_guard lock(owner->lifecycle);
		return WIFEXITED(owner->exit_status) ? WEXITSTATUS(owner->exit_status) : 1;
	}

	class linux_child_process final : public child_process
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
				const auto callback = _callbacks.on_exit;
				const auto state = _cancelled;
				if (callback)
				{
					const auto exit_code = process_exit_code(_process);
					run_ui([state, callback, exit_code] { if (!state->load()) callback(exit_code); });
				}
			}
		}

	public:
		linux_child_process(process_ptr process, child_process_callbacks callbacks)
			: _process(std::move(process)), _callbacks(std::move(callbacks))
		{
			_stdout = std::thread([this] { read_lines(false); });
			_stderr = std::thread([this] { read_lines(true); });
		}
		~linux_child_process() override
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
		return process ? std::make_unique<linux_child_process>(std::move(process), std::move(callbacks)) : nullptr;
	}
}