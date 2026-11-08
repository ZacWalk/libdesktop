// This file is part of libdesktop, a C++20 platform layer for desktop applications
// Copyright 2026  Zac Walker
//
// This program is free software; you can redistribute it and / or modify it
// under the terms of the LGPL License either version 2.1 or later.
// License details are available at https://www.gnu.org/licenses/lgpl-2.1.html
// This program is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY

// Purpose: Asynchronous HTTP session and request contracts.

#pragma once

namespace platform
{
	// Callbacks are made on a worker thread. Applications that update a window marshal their work
	// with platform::run_ui; cancellation prevents callbacks that have not started yet.
	struct async_http_callbacks
	{
		std::function<void(int status_code, std::string content_type, uint64_t content_length)> on_headers;
		std::function<void(const uint8_t* data, size_t size)> on_data;
		std::function<void()> on_complete;
		std::function<void(std::string error)> on_error;
	};

	struct async_http_request
	{
		virtual ~async_http_request() = default;
		virtual void cancel() = 0;
	};

	using async_http_request_ptr = std::shared_ptr<async_http_request>;

	struct async_http_session
	{
		virtual ~async_http_session() = default;
		virtual async_http_request_ptr get(std::string_view url, async_http_callbacks callbacks) = 0;
		virtual void stop() = 0;
	};

	using async_http_session_ptr = std::shared_ptr<async_http_session>;

	// Returns null on a backend without an asynchronous HTTP implementation.
	async_http_session_ptr create_async_http_session(std::string_view user_agent = {});
}
