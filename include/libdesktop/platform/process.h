// This file is part of libdesktop, a C++20 platform layer for desktop applications
// Copyright 2026  Zac Walker
//
// This program is free software; you can redistribute it and / or modify it
// under the terms of the LGPL License either version 2.1 or later.
// License details are available at https://www.gnu.org/licenses/lgpl-2.1.html
// This program is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY

// Purpose: Child-process, binary-stream, command-line, and console services.

#pragma once

namespace platform
{
	struct process_options
	{
		std::string exe;
		std::string cwd;
		std::vector<std::string> args;
		std::vector<std::string> env; // NAME=VALUE overrides of the inherited environment.
		bool kill_descendants_on_close = true;
	};

	struct process;
	using process_ptr = std::shared_ptr<process>;

	// Binary streams; stdout and stderr are independent and zero means EOF, error, or cancellation.
	process_ptr process_spawn(const process_options& options);
	size_t process_read(const process_ptr& process, char* buffer, size_t bytes);
	size_t process_read_err(const process_ptr& process, char* buffer, size_t bytes);
	bool process_write(const process_ptr& process, std::string_view text);
	void process_close_input(const process_ptr& process);
	bool process_alive(const process_ptr& process);
	void process_terminate(const process_ptr& process);

	struct child_process
	{
		virtual ~child_process() = default;
		// Appends a newline, so text must not contain one.
		virtual bool write_line(std::string_view text) = 0;
		virtual void close_input() = 0;
		virtual void terminate() = 0;
		[[nodiscard]] virtual bool is_running() const = 0;
	};

	using child_process_ptr = std::unique_ptr<child_process>;

	// Every callback is delivered on the UI thread, and none fires after the process is destroyed.
	struct child_process_callbacks
	{
		std::function<void(std::string_view)> on_stdout_line;
		std::function<void(std::string_view)> on_stderr_line;
		std::function<void(int)> on_exit;
	};

	child_process_ptr spawn_child_process(const core::file_path& exe,
	                                      std::span<const std::string> args,
	                                      const core::file_path& working_dir,
	                                      child_process_callbacks callbacks);

	// Searches PATH without falling back to the current directory.
	core::file_path find_executable(std::string_view name);

	// Quotes an argument according to the Windows command-line parser rules.
	std::string quote_command_arg(std::string_view arg);
	bool has_shell_metacharacter(std::string_view text);

	// Reassembles newline-delimited records from arbitrary pipe read chunks.
	struct line_splitter
	{
		static constexpr size_t default_max_line_bytes = 1024 * 1024;

		std::string buffer;
		bool discarding = false;
		size_t max_line_bytes = default_max_line_bytes;
		size_t discarded_count = 0;

		static std::string_view without_trailing_cr(std::string_view line)
		{
			return !line.empty() && line.back() == '\r' ? line.substr(0, line.size() - 1) : line;
		}

		void feed(std::string_view chunk, const std::function<void(std::string_view)>& emit)
		{
			size_t pos = 0;
			while (pos < chunk.size())
			{
				const auto newline = chunk.find('\n', pos);
				if (newline == std::string_view::npos)
				{
					const auto tail = chunk.substr(pos);
					if (discarding) return;
					if (buffer.size() + tail.size() > max_line_bytes)
					{
						buffer.clear();
						discarding = true;
						++discarded_count;
						return;
					}
					buffer.append(tail);
					return;
				}

				const auto piece = chunk.substr(pos, newline - pos);
				pos = newline + 1;
				if (discarding)
				{
					discarding = false;
					continue;
				}
				if (buffer.empty())
				{
					if (piece.size() > max_line_bytes)
					{
						++discarded_count;
						continue;
					}
					emit(without_trailing_cr(piece));
					continue;
				}
				if (buffer.size() + piece.size() > max_line_bytes)
				{
					buffer.clear();
					++discarded_count;
					continue;
				}
				buffer.append(piece);
				emit(without_trailing_cr(buffer));
				buffer.clear();
			}
		}

		void flush(const std::function<void(std::string_view)>& emit)
		{
			if (!discarding && !buffer.empty()) emit(without_trailing_cr(buffer));
			buffer.clear();
			discarding = false;
		}
	};

	void attach_console();
	void write_stdout(std::string_view text);
	size_t read_stdin(char* buffer, size_t capacity);
	std::string last_error_message();
	void show_error(std::string_view text);
}