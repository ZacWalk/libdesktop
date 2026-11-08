// This file is part of libdesktop, a C++20 platform layer for desktop applications
// Copyright 2026  Zac Walker
//
// This program is free software; you can redistribute it and / or modify it
// under the terms of the LGPL License either version 2.1 or later.
// License details are available at https://www.gnu.org/licenses/lgpl-2.1.html
// This program is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY

// Purpose: Tests for platform task queues, process argument helpers, and key contracts.

#include "libdesktop/platform.h"
#include "test.h"

#include <future>
#include <stdexcept>

using namespace std::literals;

namespace
{
	void should_run_serial_work_fifo()
	{
		auto executor = platform::create_serial_executor("test-serial");
		assert_equal(true, executor != nullptr, "executor created");
		if (!executor) return;
		assert_equal(false, executor->post({}), "empty work is refused");
		std::vector<int> order;
		for (int i = 0; i != 16; ++i)
			assert_equal(true, executor->post([&, i] { order.push_back(i); }), "work accepted");
		assert_equal(true, executor->post([] { throw std::runtime_error("expected serial task failure"); }), "exception work accepted");
		executor->stop();
		assert_equal(16, static_cast<int>(order.size()), "all work drained");
		for (int i = 0; i != 16; ++i) assert_equal(i, order[static_cast<size_t>(i)], "FIFO order");
		assert_equal(false, executor->post([] {}), "stopped executor refuses work");
	}


	void should_stop_serial_work_from_its_worker()
	{
		auto executor = platform::create_serial_executor();
		assert_equal(true, executor != nullptr, "executor created");
		if (!executor) return;
		std::promise<void> release;
		const auto gate = release.get_future().share();
		bool tail_ran = false;
		assert_equal(true, executor->post([&executor, gate] { gate.wait(); executor->stop(); }), "self-stop accepted");
		assert_equal(true, executor->post([&tail_ran] { tail_ran = true; }), "tail accepted");
		release.set_value();
		executor->stop();
		assert_equal(true, tail_ran, "self-stop drains the tail");

		auto owner = std::make_shared<platform::serial_executor_ptr>(platform::create_serial_executor());
		assert_equal(true, *owner != nullptr, "owned executor created");
		if (!*owner) return;
		std::promise<void> destroy_release;
		const auto destroy_gate = destroy_release.get_future().share();
		auto destroyed_tail = std::make_shared<std::promise<void>>();
		const auto destroyed_done = destroyed_tail->get_future();
		assert_equal(true, (*owner)->post([owner, destroy_gate] { destroy_gate.wait(); owner->reset(); }), "worker destruction accepted");
		assert_equal(true, (*owner)->post([destroyed_tail] { destroyed_tail->set_value(); }), "post-destruction tail accepted");
		destroy_release.set_value();
		assert_equal(true, destroyed_done.wait_for(5s) == std::future_status::ready, "worker destruction drains tail");
	}

	void should_serialize_pool_jobs_without_blocking_the_pool()
	{
		std::shared_ptr<platform::serial_executor> writer;
		{
			auto unique = platform::create_serial_executor("test-writer");
			writer = std::move(unique);
		}
		assert_equal(true, writer != nullptr, "writer created");
		if (!writer) return;
		std::promise<void> release;
		const auto gate = release.get_future().share();
		std::vector<std::future<void>> started;
		std::vector<std::future<bool>> completed;
		for (int i = 0; i != 4; ++i)
		{
			auto begin = std::make_shared<std::promise<void>>();
			auto done = std::make_shared<std::promise<bool>>();
			started.push_back(begin->get_future());
			completed.push_back(done->get_future());
			platform::run_async([writer, begin, done, gate]
			{
				begin->set_value();
				gate.wait();
				auto write_done = std::make_shared<std::promise<void>>();
				auto finished = write_done->get_future();
				const bool posted = writer->post([write_done] { write_done->set_value(); });
				done->set_value(posted && finished.wait_for(5s) == std::future_status::ready);
			});
		}
		for (auto& future : started)
			assert_equal(true, future.wait_for(5s) == std::future_status::ready, "pool workers start");
		release.set_value();
		for (auto& future : completed)
		{
			assert_equal(true, future.wait_for(10s) == std::future_status::ready, "pool job completes");
			if (future.wait_for(0ms) == std::future_status::ready) assert_equal(true, future.get(), "serial write completes");
		}
		writer->stop();
	}


	void should_bound_async_pool_work()
	{
		auto release = std::make_shared<std::promise<void>>();
		const auto gate = release->get_future().share();
		std::vector<std::future<void>> started;
		std::vector<std::future<void>> finished;
		for (int i = 0; i != 4; ++i)
		{
			auto begin = std::make_shared<std::promise<void>>();
			auto done = std::make_shared<std::promise<void>>();
			started.push_back(begin->get_future());
			finished.push_back(done->get_future());
			platform::run_async([begin, done, gate]
			{
				begin->set_value();
				gate.wait();
				done->set_value();
			});
		}
		for (auto& future : started)
			assert_equal(true, future.wait_for(5s) == std::future_status::ready, "all four workers start");
		auto fifth = std::make_shared<std::promise<void>>();
		auto fifth_done = fifth->get_future();
		platform::run_async([fifth] { fifth->set_value(); });
		assert_equal(true, fifth_done.wait_for(50ms) == std::future_status::timeout, "fifth task waits for a worker");
		release->set_value();
		for (auto& future : finished)
			assert_equal(true, future.wait_for(5s) == std::future_status::ready, "gated task finishes");
		assert_equal(true, fifth_done.wait_for(5s) == std::future_status::ready, "queued task finishes");
	}

	void should_run_async_work_and_pump_ui_callbacks()
	{
		constexpr int count = 4;
		std::vector<std::future<void>> completed;
		for (int i = 0; i != count; ++i)
		{
			auto promise = std::make_shared<std::promise<void>>();
			completed.push_back(promise->get_future());
			platform::run_async([promise] { promise->set_value(); });
		}
		for (auto& future : completed)
			assert_equal(true, future.wait_for(5s) == std::future_status::ready, "async task completes");

		auto ui_promise = std::make_shared<std::promise<std::thread::id>>();
		auto ui_done = ui_promise->get_future();
		platform::run_async([ui_promise]
		{
			platform::run_ui([ui_promise] { ui_promise->set_value(std::this_thread::get_id()); });
		});
		const auto deadline = std::chrono::steady_clock::now() + 5s;
		while (ui_done.wait_for(0ms) != std::future_status::ready && std::chrono::steady_clock::now() < deadline)
			platform::pump_ui_tasks(25);
		assert_equal(true, ui_done.wait_for(0ms) == std::future_status::ready, "UI callback is delivered");
		if (ui_done.wait_for(0ms) == std::future_status::ready)
			assert_equal(true, ui_done.get() == std::this_thread::get_id(), "UI callback runs on pump thread");
	}

	void should_reassemble_process_lines()
	{
		platform::line_splitter splitter;
		std::vector<std::string> lines;
		const auto emit = [&lines](const std::string_view line) { lines.emplace_back(line); };
		splitter.feed("on", emit);
		splitter.feed("e\ntw", emit);
		splitter.feed("o\nthr", emit);
		splitter.feed("ee\n", emit);
		splitter.feed("tail", emit);
		splitter.flush(emit);
		assert_equal(4, static_cast<int>(lines.size()), "line count");
		assert_equal_strict("one", lines[0], "split records join");
		assert_equal_strict("two", lines[1], "second record joins across chunks");
		assert_equal_strict("three", lines[2], "third record joins across chunks");
		assert_equal_strict("tail", lines[3], "partial record flushes");

		lines.clear();
		platform::line_splitter crlf;
		crlf.feed("one\r\ntwo\r\n", emit);
		crlf.flush(emit);
		assert_equal(2, static_cast<int>(lines.size()), "CRLF line count");
		assert_equal_strict("one", lines[0], "CRLF first record");
		assert_equal_strict("two", lines[1], "CRLF second record");

		platform::line_splitter bounded;
		bounded.max_line_bytes = 4;
		lines.clear();
		bounded.feed("12345\nok\n", emit);
		assert_equal(1, static_cast<int>(lines.size()), "oversize line is dropped");
		assert_equal_strict("ok", lines[0], "line splitter resynchronizes");
	}

	void should_quote_process_arguments()
	{
		assert_equal_strict("simple", platform::quote_command_arg("simple"), "simple argument");
		assert_equal_strict("\"\"", platform::quote_command_arg(""), "empty argument");
		assert_equal_strict("\"a b\"", platform::quote_command_arg("a b"), "space argument");
		assert_equal_strict("\"a\\\"b\"", platform::quote_command_arg("a\"b"), "quote argument");
		assert_equal_strict("C:\\path\\file", platform::quote_command_arg("C:\\path\\file"), "plain path argument");
		assert_equal_strict("\"C:\\my path\\\\\"", platform::quote_command_arg("C:\\my path\\"), "trailing slash argument");
		assert_equal_strict("\"a\\\\\\\"b\"", platform::quote_command_arg("a\\\"b"), "slash quote argument");
		assert_equal(false, platform::has_shell_metacharacter("--safe"), "safe shell text");
		assert_equal(false, platform::has_shell_metacharacter("C:\\path\\file.txt"), "safe path text");
		assert_equal(true, platform::has_shell_metacharacter("a & b"), "shell operator");
		assert_equal(true, platform::has_shell_metacharacter("a | b"), "shell pipe");
		assert_equal(true, platform::has_shell_metacharacter("%PATH%"), "environment expansion");
		assert_equal(true, platform::has_shell_metacharacter("a > b"), "shell redirection");
		assert_equal(true, platform::has_shell_metacharacter("a\nb"), "shell newline");
	}

	void should_complete_key_contract()
	{
		assert_equal(true, platform::keys::F12 != 0, "F12 is defined");
		assert_equal(true, platform::keys::NUMPAD0 != platform::keys::NUMPAD1, "numpad keys are distinct");
		assert_equal(static_cast<uint32_t>(U'0'),
			static_cast<uint32_t>(platform::keys::normalize_numpad(platform::keys::NUMPAD0)), "numpad normalizes");
		assert_equal(static_cast<uint32_t>(U'9'),
			static_cast<uint32_t>(platform::keys::normalize_numpad(platform::keys::NUMPAD9)), "last numpad normalizes");
		assert_equal(static_cast<uint32_t>(U'A'), static_cast<uint32_t>(platform::keys::normalize_numpad(U'A')),
			"ordinary keys pass through");
		const auto before = platform::monotonic_seconds();
		const auto after = platform::monotonic_seconds();
		assert_equal(true, after >= before, "clock is monotonic");
	}
}

void register_platform_tasks_tests(test_registry& tests)
{
	tests.add("Should run serial work FIFO"s, should_run_serial_work_fifo);
	tests.add("Should stop serial work from its worker"s, should_stop_serial_work_from_its_worker);
	tests.add("Should serialize pool jobs without blocking the pool"s, should_serialize_pool_jobs_without_blocking_the_pool);
	tests.add("Should bound async pool work"s, should_bound_async_pool_work);
	tests.add("Should run async work and pump UI callbacks"s, should_run_async_work_and_pump_ui_callbacks);
	tests.add("Should reassemble process lines"s, should_reassemble_process_lines);
	tests.add("Should quote process arguments"s, should_quote_process_arguments);
	tests.add("Should complete the key contract"s, should_complete_key_contract);
}