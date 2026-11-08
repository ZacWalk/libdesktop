// This file is part of libdesktop, a C++20 platform layer for desktop applications
// Copyright 2026  Zac Walker
//
// This program is free software; you can redistribute it and / or modify it
// under the terms of the LGPL License either version 2.1 or later.
// License details are available at https://www.gnu.org/licenses/lgpl-2.1.html
// This program is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY

// Purpose: WinHTTP asynchronous GET sessions with cancellation and streaming callbacks.

#include "libdesktop/platform.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <winhttp.h>

#include <format>
#include <mutex>
#include <string>
#include <vector>

#pragma comment(lib, "winhttp.lib")

namespace
{
	class win_async_request final : public platform::async_http_request,
	                                public std::enable_shared_from_this<win_async_request>
	{
		std::mutex _mutex;
		HINTERNET _connect = nullptr;
		HINTERNET _request = nullptr;
		bool _cancelled = false;
		platform::async_http_callbacks _callbacks;
		std::vector<uint8_t> _read_buffer;
		std::shared_ptr<win_async_request> _self_keep_alive;

		std::pair<HINTERNET, HINTERNET> take_handles_locked()
		{
			const auto handles = std::pair{_request, _connect};
			_request = nullptr;
			_connect = nullptr;
			return handles;
		}

		static void close_handles(const std::pair<HINTERNET, HINTERNET>& handles)
		{
			if (handles.first) WinHttpCloseHandle(handles.first);
			if (handles.second) WinHttpCloseHandle(handles.second);
		}

		void fail(std::string error)
		{
			platform::async_http_callbacks callbacks;
			std::pair<HINTERNET, HINTERNET> handles;
			{
				std::lock_guard lock(_mutex);
				if (_cancelled) return;
				_cancelled = true;
				callbacks = std::move(_callbacks);
				handles = take_handles_locked();
			}
			close_handles(handles);
			if (callbacks.on_error) callbacks.on_error(std::move(error));
		}

		void complete()
		{
			platform::async_http_callbacks callbacks;
			std::pair<HINTERNET, HINTERNET> handles;
			{
				std::lock_guard lock(_mutex);
				if (_cancelled) return;
				_cancelled = true;
				callbacks = std::move(_callbacks);
				handles = take_handles_locked();
			}
			close_handles(handles);
			if (callbacks.on_complete) callbacks.on_complete();
		}

		void headers_available()
		{
			HINTERNET request;
			platform::async_http_callbacks callbacks;
			{
				std::lock_guard lock(_mutex);
				if (_cancelled) return;
				request = _request;
				callbacks = _callbacks;
			}

			DWORD status = 0;
			DWORD status_size = sizeof(status);
			WinHttpQueryHeaders(request,
			                    WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
			                    nullptr, &status, &status_size, nullptr);

			std::string content_type;
			DWORD content_type_size = 0;
			WinHttpQueryHeaders(request, WINHTTP_QUERY_CONTENT_TYPE, nullptr, nullptr,
			                    &content_type_size, nullptr);
			if (content_type_size != 0)
			{
				std::wstring value((content_type_size + sizeof(wchar_t) - 1) / sizeof(wchar_t), L'\0');
				if (WinHttpQueryHeaders(request, WINHTTP_QUERY_CONTENT_TYPE, nullptr, value.data(),
				                        &content_type_size, nullptr))
				{
					if (!value.empty() && value.back() == L'\0') value.pop_back();
					content_type = core::utf16_to_utf8(value);
				}
			}

			uint64_t content_length = 0;
			DWORD content_length_size = 0;
			WinHttpQueryHeaders(request, WINHTTP_QUERY_CONTENT_LENGTH, nullptr, nullptr,
			                    &content_length_size, nullptr);
			if (content_length_size != 0)
			{
				std::wstring value((content_length_size + sizeof(wchar_t) - 1) / sizeof(wchar_t), L'\0');
				if (WinHttpQueryHeaders(request, WINHTTP_QUERY_CONTENT_LENGTH, nullptr, value.data(),
				                        &content_length_size, nullptr))
				{
					while (!value.empty() && value.back() == L'\0') value.pop_back();
					try { content_length = std::stoull(value); }
					catch (...) { content_length = 0; }
				}
			}

			if (callbacks.on_headers) callbacks.on_headers(static_cast<int>(status), std::move(content_type), content_length);
			query_data_available();
		}

		void query_data_available()
		{
			HINTERNET request;
			{
				std::lock_guard lock(_mutex);
				if (_cancelled) return;
				request = _request;
			}
			if (!request || !WinHttpQueryDataAvailable(request, nullptr)) fail("query data available failed");
		}

		void data_available(const DWORD bytes)
		{
			if (bytes == 0)
			{
				complete();
				return;
			}

			HINTERNET request;
			{
				std::lock_guard lock(_mutex);
				if (_cancelled) return;
				request = _request;
				_read_buffer.assign(bytes, 0);
			}
			if (!request || !WinHttpReadData(request, _read_buffer.data(), bytes, nullptr))
				fail("read data failed");
		}

		void read_complete(const DWORD bytes)
		{
			platform::async_http_callbacks callbacks;
			{
				std::lock_guard lock(_mutex);
				if (_cancelled) return;
				callbacks = _callbacks;
			}
			if (bytes == 0)
			{
				complete();
				return;
			}
			if (callbacks.on_data) callbacks.on_data(_read_buffer.data(), bytes);
			query_data_available();
		}

		static void CALLBACK callback(HINTERNET, const DWORD_PTR context, const DWORD status,
		                             LPVOID info, const DWORD info_length)
		{
			if (context == 0) return;
			auto* const self = reinterpret_cast<win_async_request*>(context);
			switch (status)
			{
			case WINHTTP_CALLBACK_STATUS_SENDREQUEST_COMPLETE:
				self->send_complete();
				break;
			case WINHTTP_CALLBACK_STATUS_HEADERS_AVAILABLE:
				self->headers_available();
				break;
			case WINHTTP_CALLBACK_STATUS_DATA_AVAILABLE:
				self->data_available(info ? *static_cast<DWORD*>(info) : 0);
				break;
			case WINHTTP_CALLBACK_STATUS_READ_COMPLETE:
				self->read_complete(info_length);
				break;
			case WINHTTP_CALLBACK_STATUS_REQUEST_ERROR:
				{
					std::string error = "winhttp request error";
					if (info)
					{
						const auto* const result = static_cast<const WINHTTP_ASYNC_RESULT*>(info);
						error = std::format("winhttp error {} (code {})",
						                   static_cast<unsigned>(result->dwResult),
						                   static_cast<unsigned>(result->dwError));
					}
					self->fail(std::move(error));
				}
				break;
			case WINHTTP_CALLBACK_STATUS_HANDLE_CLOSING:
				{
					std::shared_ptr<win_async_request> keep;
					{
						std::lock_guard lock(self->_mutex);
						keep = std::move(self->_self_keep_alive);
					}
					(void)keep;
				}
				break;
			default:
				break;
			}
		}

		void send_complete()
		{
			HINTERNET request;
			{
				std::lock_guard lock(_mutex);
				if (_cancelled) return;
				request = _request;
			}
			if (!request || !WinHttpReceiveResponse(request, nullptr)) fail("receive response failed");
		}

	public:
		~win_async_request() override
		{
			std::pair<HINTERNET, HINTERNET> handles;
			{
				std::lock_guard lock(_mutex);
				handles = take_handles_locked();
			}
			close_handles(handles);
		}

		void cancel() override
		{
			std::pair<HINTERNET, HINTERNET> handles;
			{
				std::lock_guard lock(_mutex);
				if (_cancelled) return;
				_cancelled = true;
				handles = take_handles_locked();
			}
			close_handles(handles);
		}

		bool start(const HINTERNET session, const std::string_view url, platform::async_http_callbacks callbacks)
		{
			_callbacks = std::move(callbacks);
			URL_COMPONENTS components = {};
			components.dwStructSize = sizeof(components);
			components.dwSchemeLength = static_cast<DWORD>(-1);
			components.dwHostNameLength = static_cast<DWORD>(-1);
			components.dwUrlPathLength = static_cast<DWORD>(-1);
			components.dwExtraInfoLength = static_cast<DWORD>(-1);
			const auto wide_url = core::utf8_to_utf16(url);
			if (!WinHttpCrackUrl(wide_url.c_str(), 0, 0, &components) || !components.lpszHostName)
			{
				fail("invalid url");
				return false;
			}

			const std::wstring host(components.lpszHostName, components.dwHostNameLength);
			const auto path_length = static_cast<size_t>(components.dwUrlPathLength) + components.dwExtraInfoLength;
			std::wstring object = components.lpszUrlPath && path_length != 0
				                       ? std::wstring(components.lpszUrlPath, path_length)
				                       : L"/";
			const bool secure = components.nScheme == INTERNET_SCHEME_HTTPS ||
			                    components.nPort == INTERNET_DEFAULT_HTTPS_PORT ||
			                    (components.lpszScheme && components.dwSchemeLength == 5 &&
			                     _wcsnicmp(components.lpszScheme, L"https", 5) == 0);

			const auto connect = WinHttpConnect(session, host.c_str(), components.nPort, 0);
			if (!connect)
			{
				fail(std::format("connect failed (err {})", GetLastError()));
				return false;
			}
			const auto request = WinHttpOpenRequest(connect, L"GET", object.c_str(), nullptr, nullptr, nullptr,
			                                      secure ? WINHTTP_FLAG_SECURE : 0);
			if (!request)
			{
				const auto error = GetLastError();
				WinHttpCloseHandle(connect);
				fail(std::format("open request failed (err {})", error));
				return false;
			}

			WinHttpSetStatusCallback(request, &win_async_request::callback,
			                         WINHTTP_CALLBACK_FLAG_ALL_COMPLETIONS | WINHTTP_CALLBACK_FLAG_HANDLES, 0);
			bool cancelled;
			{
				std::lock_guard lock(_mutex);
				cancelled = _cancelled;
				if (!cancelled)
				{
					_connect = connect;
					_request = request;
					_self_keep_alive = shared_from_this();
				}
			}
			if (cancelled)
			{
				WinHttpCloseHandle(request);
				WinHttpCloseHandle(connect);
				return false;
			}

			if (!WinHttpSendRequest(request, nullptr, 0, nullptr, 0, 0, reinterpret_cast<DWORD_PTR>(this)))
			{
				fail(std::format("send request failed (err {})", GetLastError()));
				return false;
			}
			return true;
		}
	};

	class win_async_session final : public platform::async_http_session
	{
		HINTERNET _session = nullptr;
		std::mutex _mutex;
		std::vector<std::weak_ptr<win_async_request>> _requests;

	public:
		explicit win_async_session(const std::wstring& user_agent)
		{
			_session = WinHttpOpen(user_agent.c_str(), WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, nullptr, nullptr,
			                       WINHTTP_FLAG_ASYNC);
			if (!_session) return;
			WinHttpSetTimeouts(_session, 30000, 30000, 30000, 30000);
			DWORD protocols = WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_2;
#ifdef WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_3
			protocols |= WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_3;
#endif
			WinHttpSetOption(_session, WINHTTP_OPTION_SECURE_PROTOCOLS, &protocols, sizeof(protocols));
#if defined(WINHTTP_OPTION_ENABLE_HTTP_PROTOCOL) && defined(WINHTTP_PROTOCOL_FLAG_HTTP2)
			DWORD http_protocols = WINHTTP_PROTOCOL_FLAG_HTTP2;
			WinHttpSetOption(_session, WINHTTP_OPTION_ENABLE_HTTP_PROTOCOL, &http_protocols, sizeof(http_protocols));
#endif
			DWORD decompression = WINHTTP_DECOMPRESSION_FLAG_ALL;
			WinHttpSetOption(_session, WINHTTP_OPTION_DECOMPRESSION, &decompression, sizeof(decompression));
			DWORD redirects = WINHTTP_OPTION_REDIRECT_POLICY_ALWAYS;
			WinHttpSetOption(_session, WINHTTP_OPTION_REDIRECT_POLICY, &redirects, sizeof(redirects));
		}

		~win_async_session() override
		{
			stop();
			if (_session) WinHttpCloseHandle(_session);
		}

		platform::async_http_request_ptr get(const std::string_view url,
		                                     platform::async_http_callbacks callbacks) override
		{
			if (!_session)
			{
				if (callbacks.on_error) callbacks.on_error("no session");
				return nullptr;
			}
			auto request = std::make_shared<win_async_request>();
			{
				std::lock_guard lock(_mutex);
				_requests.emplace_back(request);
			}
			request->start(_session, url, std::move(callbacks));
			return request;
		}

		void stop() override
		{
			std::vector<std::weak_ptr<win_async_request>> requests;
			{
				std::lock_guard lock(_mutex);
				requests.swap(_requests);
			}
			for (const auto& weak : requests)
				if (const auto request = weak.lock()) request->cancel();
		}
	};
}

platform::async_http_session_ptr platform::create_async_http_session(const std::string_view user_agent)
{
	const auto agent = user_agent.empty() ? std::wstring(L"libdesktop/1.0") : core::utf8_to_utf16(user_agent);
	return std::make_shared<win_async_session>(agent);
}
