// This file is part of libdesktop, a C++20 platform layer for desktop applications
// Copyright 2026  Zac Walker
//
// This program is free software; you can redistribute it and / or modify it
// under the terms of the LGPL License either version 2.1 or later.
// License details are available at https://www.gnu.org/licenses/lgpl-2.1.html
// This program is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY

// Purpose: Shared background workers, serial executors, and command-line helpers.

#include "libdesktop/platform.h"

#include <condition_variable>
#include <cstdlib>
#include <deque>
#include <thread>

namespace platform::detail
{
	// The UI queue remains owned by each event backend, because only that backend can wake its loop.
	bool pump_backend_ui_tasks(uint32_t timeout_ms);
	void set_task_shutdown_bounded(bool bounded);
}

namespace
{
	struct task_queue_state
	{
		std::mutex mutex;
		std::condition_variable ready;
		std::condition_variable finished;
		std::deque<std::function<void()>> tasks;
		bool accepting = true;
		size_t workers = 0;
	};

	void invoke_task(std::function<void()>& task)
	{
		try
		{
			if (task) task();
		}
		catch (const std::exception& error)
		{
			core::log("platform task", error.what());
		}
		catch (...)
		{
			core::log("platform task", "unknown exception");
		}
	}

	void consume_tasks(const std::shared_ptr<task_queue_state>& state)
	{
		for (;;)
		{
			std::function<void()> task;
			{
				std::unique_lock lock(state->mutex);
				state->ready.wait(lock, [&] { return !state->accepting || !state->tasks.empty(); });
				if (state->tasks.empty())
				{
					if (state->workers > 0) --state->workers;
					state->finished.notify_all();
					return;
				}
				task = std::move(state->tasks.front());
				state->tasks.pop_front();
			}
			invoke_task(task);
		}
	}

	bool enqueue_task(const std::shared_ptr<task_queue_state>& state, std::function<void()> task)
	{
		if (!task) return false;
		{
			std::lock_guard lock(state->mutex);
			if (!state->accepting) return false;
			state->tasks.push_back(std::move(task));
		}
		state->ready.notify_one();
		return true;
	}

	void stop_queue(const std::shared_ptr<task_queue_state>& state, const bool cancel_pending)
	{
		{
			std::lock_guard lock(state->mutex);
			state->accepting = false;
			if (cancel_pending) state->tasks.clear();
		}
		state->ready.notify_all();
	}

	class serial_executor_impl final : public platform::serial_executor
	{
		const std::shared_ptr<task_queue_state> _state = std::make_shared<task_queue_state>();
		std::mutex _join_mutex;
		std::thread _worker;
		std::thread::id _worker_id;

	public:
		explicit serial_executor_impl(std::string name)
		{
			_state->workers = 1;
			_worker = std::thread([state = _state, name = std::move(name)]
			{
				if (!name.empty()) core::set_thread_description(name);
				consume_tasks(state);
			});
			_worker_id = _worker.get_id();
		}

		~serial_executor_impl() override
		{
			stop();
			if (_worker.joinable()) _worker.detach();
		}

		bool post(std::function<void()> task) override
		{
			return enqueue_task(_state, std::move(task));
		}

		void stop() override
		{
			stop_queue(_state, false);
			if (std::this_thread::get_id() == _worker_id) return;
			std::lock_guard lock(_join_mutex);
			if (_worker.joinable()) _worker.join();
		}
	};

	class async_task_pool
	{
		const std::shared_ptr<task_queue_state> _state = std::make_shared<task_queue_state>();
		std::vector<std::thread> _workers;
		std::mutex _stop_mutex;
		bool _stopped = false;

	public:
		async_task_pool()
		{
			constexpr size_t worker_count = 4;
			_workers.reserve(worker_count);
			try
			{
				for (size_t i = 0; i != worker_count; ++i)
				{
					{
						std::lock_guard lock(_state->mutex);
						++_state->workers;
					}
					_workers.emplace_back([state = _state] { consume_tasks(state); });
				}
			}
			catch (...)
			{
				stop_queue(_state, true);
				for (auto& worker : _workers)
					if (worker.joinable()) worker.join();
				throw;
			}
		}

		void post(std::function<void()> task)
		{
			enqueue_task(_state, std::move(task));
		}

		void stop(const bool bounded)
		{
			std::lock_guard stop_lock(_stop_mutex);
			if (_stopped) return;
			_stopped = true;
			stop_queue(_state, bounded);

			const auto self = std::this_thread::get_id();
			const bool on_worker = std::any_of(_workers.begin(), _workers.end(),
				[self](const auto& worker) { return worker.get_id() == self; });
			bool finished = on_worker ? false : true;
			if (finished)
			{
				std::unique_lock lock(_state->mutex);
				if (bounded)
					finished = _state->finished.wait_for(lock, std::chrono::seconds(5),
						[&] { return _state->workers == 0; });
				else
					_state->finished.wait(lock, [&] { return _state->workers == 0; });
			}

			for (auto& worker : _workers)
			{
				if (finished && worker.get_id() != self && worker.joinable()) worker.join();
				else if (worker.joinable()) worker.detach();
			}
			if (!finished) core::log("platform async shutdown", "workers still running after five seconds");
		}
	};

	struct dispatcher
	{
		std::mutex mutex;
		std::unique_ptr<async_task_pool> pool;
		bool stopping = false;
		std::once_flag shutdown_hook;
	};

	std::atomic_bool shutdown_bounded = true;

	dispatcher& get_dispatcher()
	{
		static auto* value = new dispatcher;
		return *value;
	}

	void shutdown_async_pool(const bool bounded)
	{
		auto& state = get_dispatcher();
		async_task_pool* pool = nullptr;
		{
			std::lock_guard lock(state.mutex);
			state.stopping = true;
			pool = state.pool.get();
		}
		if (pool) pool->stop(bounded);
	}

	void install_shutdown_hook()
	{
		auto& state = get_dispatcher();
		std::call_once(state.shutdown_hook, []
		{
			// The app loop owns the hook lifetime. The process-exit fallback is needed by
			// command-line callers that never enter platform::run.
			platform::add_shutdown_hook([] { shutdown_async_pool(shutdown_bounded.load()); });
			std::atexit([] { shutdown_async_pool(false); });
		});
	}
}

namespace platform::detail
{
	void set_task_shutdown_bounded(const bool bounded)
	{
		shutdown_bounded = bounded;
	}
}

namespace platform
{
	void run_async(std::function<void()> task)
	{
		if (!task) return;
		install_shutdown_hook();
		auto& state = get_dispatcher();
		std::lock_guard lock(state.mutex);
		if (state.stopping) return;
		if (!state.pool) state.pool = std::make_unique<async_task_pool>();
		state.pool->post(std::move(task));
	}

	std::unique_ptr<serial_executor> create_serial_executor(const std::string_view name)
	{
		try
		{
			return std::make_unique<serial_executor_impl>(std::string(name));
		}
		catch (const std::exception& error)
		{
			core::log("create_serial_executor", error.what());
			return nullptr;
		}
	}

	void pump_ui_tasks()
	{
		detail::pump_backend_ui_tasks(0);
	}

	void pump_ui_tasks(const uint32_t timeout_ms)
	{
		detail::pump_backend_ui_tasks(timeout_ms);
	}

	double monotonic_seconds()
	{
		return core::now();
	}

	std::string quote_command_arg(const std::string_view arg)
	{
		if (!arg.empty() && arg.find_first_of(" \t\n\v\"") == std::string_view::npos)
			return std::string(arg);

		std::string result;
		result.reserve(arg.size() + 2);
		result += '"';
		for (auto i = arg.begin();; ++i)
		{
			size_t backslashes = 0;
			while (i != arg.end() && *i == '\\')
			{
				++i;
				++backslashes;
			}
			if (i == arg.end())
			{
				result.append(backslashes * 2, '\\');
				break;
			}
			result.append(*i == '"' ? backslashes * 2 + 1 : backslashes, '\\');
			result += *i;
		}
		result += '"';
		return result;
	}

	bool has_shell_metacharacter(const std::string_view text)
	{
		return text.find_first_of("&|<>^%!\"\r\n") != std::string_view::npos;
	}
}