// This file is part of libdesktop, a C++20 platform layer for desktop applications
// Copyright 2026  Zac Walker
// 
// This program is free software; you can redistribute it and / or modify it
// under the terms of the LGPL License either version 2.1 or later.
// License details are available at https://www.gnu.org/licenses/lgpl-2.1.html
// This program is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY

// Purpose: libdesktop-os's Windows backend - clipboard text, known folders, and the synchronous web
// client, which uses WinINet and switches to WinHTTP for a request bounded by a timeout or a limit.

#include "libdesktop/os.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <shlobj.h>
#include <WinInet.h>
#include <winhttp.h>

#pragma comment(lib, "wininet.lib")
#pragma comment(lib, "winhttp.lib")

#include "os_win_text.h"

#include <algorithm>
#include <atomic>
#include <charconv>
#include <climits>
#include <cstring>

namespace
{
	using os::detail::narrow;
	using os::detail::widen;

	bool iequals(const std::string_view a, const std::string_view b)
	{
		return std::ranges::equal(a, b, [](char x, char y)
		{
			if (x >= 'A' && x <= 'Z') x = static_cast<char>(x - 'A' + 'a');
			if (y >= 'A' && y <= 'Z') y = static_cast<char>(y - 'A' + 'a');
			return x == y;
		});
	}

	// The extension is the part after the last '.' of the file name, not of a folder.
	bool has_extension(const std::string_view path, const std::string_view extension)
	{
		const auto dot = path.find_last_of("./\\");
		return dot != std::string_view::npos && path[dot] == '.' && iequals(path.substr(dot), extension);
	}

	class file_handle
	{
		HANDLE _h = INVALID_HANDLE_VALUE;

	public:
		explicit file_handle(const HANDLE h) : _h(h)
		{
		}

		~file_handle()
		{
			if (_h != INVALID_HANDLE_VALUE) CloseHandle(_h);
		}

		file_handle(const file_handle&) = delete;
		file_handle& operator=(const file_handle&) = delete;

		[[nodiscard]] bool is_valid() const { return _h != INVALID_HANDLE_VALUE; }
		[[nodiscard]] HANDLE get() const { return _h; }
	};
}

// ── Clipboard text ───────────────────────────────────────────────────────────────────────────

bool os::clipboard_has_text()
{
	return IsClipboardFormatAvailable(CF_UNICODETEXT) != 0;
}

os::clipboard_text os::read_clipboard_text(const uintptr_t owner)
{
	clipboard_text result;
	if (!OpenClipboard(reinterpret_cast<HWND>(owner))) return result;
	result.opened = true;

	if (const auto data = GetClipboardData(CF_UNICODETEXT))
	{
		if (const auto text = static_cast<const wchar_t*>(GlobalLock(data)))
		{
			result.text = narrow(text);
			GlobalUnlock(data);
		}
	}

	CloseClipboard();
	return result;
}

os::clipboard_write os::write_clipboard_text(const uintptr_t owner, const std::string_view text)
{
	const auto w = widen(text);

	if (w.size() > SIZE_MAX / sizeof(wchar_t) - 1) return clipboard_write::too_large;

	auto* const memory = GlobalAlloc(GMEM_MOVEABLE, (w.size() + 1) * sizeof(wchar_t));
	if (!memory) return clipboard_write::alloc_failed;

	auto* const copy = static_cast<wchar_t*>(GlobalLock(memory));
	if (!copy)
	{
		GlobalFree(memory);
		return clipboard_write::lock_failed;
	}

	if (!w.empty()) memcpy(copy, w.data(), w.size() * sizeof(wchar_t));
	copy[w.size()] = 0;
	GlobalUnlock(memory);

	if (!OpenClipboard(reinterpret_cast<HWND>(owner)))
	{
		GlobalFree(memory);
		return clipboard_write::open_failed;
	}

	if (!EmptyClipboard())
	{
		CloseClipboard();
		GlobalFree(memory);
		return clipboard_write::empty_failed;
	}

	// The clipboard owns the memory only once it has accepted it; refused, it is still ours.
	if (!SetClipboardData(CF_UNICODETEXT, memory))
	{
		CloseClipboard();
		GlobalFree(memory);
		return clipboard_write::set_failed;
	}

	CloseClipboard();
	return clipboard_write::ok;
}

// ── Known folders ────────────────────────────────────────────────────────────────────────────

std::string os::known_folder_path(const known_folder folder, const bool create, uint32_t* const error)
{
	const KNOWNFOLDERID* id = nullptr;

	switch (folder)
	{
	case known_folder::local_app_data: id = &FOLDERID_LocalAppData; break;
	case known_folder::profile: id = &FOLDERID_Profile; break;
	case known_folder::desktop: id = &FOLDERID_Desktop; break;
	case known_folder::documents: id = &FOLDERID_Documents; break;
	case known_folder::pictures: id = &FOLDERID_Pictures; break;
	case known_folder::videos: id = &FOLDERID_Videos; break;
	case known_folder::music: id = &FOLDERID_Music; break;
	case known_folder::public_documents: id = &FOLDERID_PublicDocuments; break;
	case known_folder::public_pictures: id = &FOLDERID_PublicPictures; break;
	case known_folder::public_videos: id = &FOLDERID_PublicVideos; break;
	case known_folder::public_music: id = &FOLDERID_PublicMusic; break;
	}

	if (!id)
	{
		if (error) *error = static_cast<uint32_t>(E_INVALIDARG);
		return {};
	}

	PWSTR raw = nullptr;
	const auto status = SHGetKnownFolderPath(*id, create ? KF_FLAG_CREATE : KF_FLAG_DEFAULT, nullptr, &raw);
	if (error) *error = static_cast<uint32_t>(status);

	std::string result;
	if (SUCCEEDED(status) && raw) result = narrow(raw);
	CoTaskMemFree(raw);
	return result;
}

// ── Web requests ─────────────────────────────────────────────────────────────────────────────

bool os::is_online()
{
	DWORD flags;
	return 0 != InternetGetConnectedState(&flags, 0);
}

std::string os::url_encode(const std::string_view input)
{
	static constexpr auto hex_chars = "0123456789ABCDEF";
	std::string result;
	result.reserve(input.size());

	for (const auto c : input)
	{
		if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
			(c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~')
		{
			result += c;
		}
		else
		{
			const auto byte = static_cast<uint8_t>(c);
			result += '%';
			result += hex_chars[byte >> 4];
			result += hex_chars[byte & 0x0F];
		}
	}

	return result;
}

static bool web_header_token(const std::string_view text)
{
	if (text.empty()) return false;
	for (const unsigned char c : text)
	{
		if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
			std::string_view("!#$%&'*+-.^_`|~").find(c) != std::string_view::npos)) return false;
	}
	return true;
}

static bool web_header_value(const std::string_view text)
{
	for (const unsigned char c : text)
	{
		if ((c < 32 && c != '\t') || c == 127) return false;
	}
	return text.size() < INT_MAX;
}

static bool web_multipart_name(const std::string_view text)
{
	return web_header_value(text) && text.find_first_of("\"\\") == std::string_view::npos;
}

static std::string_view web_trim(std::string_view text)
{
	while (!text.empty() && (text.front() == ' ' || text.front() == '\t')) text.remove_prefix(1);
	while (!text.empty() && (text.back() == ' ' || text.back() == '\t')) text.remove_suffix(1);
	return text;
}

static bool web_length(const std::string_view text, uint64_t& value)
{
	if (text.empty()) return false;
	const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value);
	return parsed.ec == std::errc{} && parsed.ptr == text.data() + text.size();
}

static bool set_web_timeouts(const HINTERNET handle, const uint32_t timeout)
{
	DWORD milliseconds = timeout == 0 ? 30000 : timeout;
	for (const auto option : {INTERNET_OPTION_CONNECT_TIMEOUT, INTERNET_OPTION_SEND_TIMEOUT,
		INTERNET_OPTION_RECEIVE_TIMEOUT})
	{
		if (!InternetSetOptionW(handle, option, &milliseconds, sizeof(milliseconds))) return false;
	}
	return true;
}

static std::string format_path(const os::web_request& req)
{
	auto result = req.path.empty() ? std::string("/") : req.path;

	if (!req.query.empty())
	{
		bool is_first = true;
		result += result.find('?') == std::string::npos ? "?" : "&";

		for (const auto& qp : req.query)
		{
			if (!is_first)
			{
				result += "&";
			}

			result += os::url_encode(qp.first);
			result += "=";
			result += os::url_encode(qp.second);
			is_first = false;
		}
	}

	return result;
}

// RAII wrapper for WinInet handles
class inet_handle
{
	HINTERNET _h;

public:
	explicit inet_handle(const HINTERNET handle = nullptr) : _h(handle)
	{
	}

	~inet_handle()
	{
		if (_h)
		{
			InternetCloseHandle(_h);
		}
	}

	HINTERNET detach()
	{
		const auto handle = _h;
		_h = nullptr;
		return handle;
	}

	inet_handle(const inet_handle&) = delete;
	inet_handle& operator=(const inet_handle&) = delete;

	operator HINTERNET() const { return _h; }
	[[nodiscard]] HINTERNET get() const { return _h; }
	[[nodiscard]] bool is_valid() const { return _h != nullptr; }

	void reset(const HINTERNET handle = nullptr)
	{
		if (_h)
		{
			InternetCloseHandle(_h);
		}
		_h = handle;
	}
};

struct os::web_host
{
	HINTERNET session_handle = nullptr;
	HINTERNET connection_handle = nullptr;
	bool secure = true;
	bool timeouts = true;
	std::string name;
	std::string user_agent;
	int port = 0;

	web_host() = default;
	web_host(const web_host&) = delete;
	web_host& operator=(const web_host&) = delete;

	~web_host()
	{
		if (connection_handle) InternetCloseHandle(connection_handle);
		if (session_handle) InternetCloseHandle(session_handle);
	}
};

struct sync_http_handle
{
	HINTERNET value = nullptr;
	sync_http_handle() = default;
	sync_http_handle(const sync_http_handle&) = delete;
	sync_http_handle& operator=(const sync_http_handle&) = delete;
	~sync_http_handle() { if (value) WinHttpCloseHandle(value); }
};

// The public call blocks, but async handles permit documented, race-free
// cancellation when WinHTTP's native timeout granularity exceeds the deadline.
class bounded_http_request
{
	HANDLE completed = nullptr;
	HANDLE closed = nullptr;
	bool has_callback = false;
	DWORD timeout = 30000;
	std::atomic<DWORD> error = ERROR_SUCCESS;
	std::atomic<DWORD> received = 0;

	static void CALLBACK callback(HINTERNET, const DWORD_PTR context, const DWORD status,
		void* info, const DWORD length)
	{
		const auto self = reinterpret_cast<bounded_http_request*>(context);
		if (!self) return;
		switch (status)
		{
		case WINHTTP_CALLBACK_STATUS_HANDLE_CLOSING:
			SetEvent(self->closed);
			break;
		case WINHTTP_CALLBACK_STATUS_REQUEST_ERROR:
			self->error = static_cast<WINHTTP_ASYNC_RESULT*>(info)->dwError;
			SetEvent(self->completed);
			break;
		case WINHTTP_CALLBACK_STATUS_READ_COMPLETE:
			self->received = length;
			SetEvent(self->completed);
			break;
		case WINHTTP_CALLBACK_STATUS_SENDREQUEST_COMPLETE:
		case WINHTTP_CALLBACK_STATUS_HEADERS_AVAILABLE:
			SetEvent(self->completed);
			break;
		default:
			break;
		}
	}

public:
	HINTERNET value = nullptr;
	bounded_http_request() = default;
	bounded_http_request(const bounded_http_request&) = delete;
	bounded_http_request& operator=(const bounded_http_request&) = delete;

	~bounded_http_request()
	{
		close();
		if (completed) CloseHandle(completed);
		if (closed) CloseHandle(closed);
	}

	void close()
	{
		if (!value) return;
		const auto handle = value;
		value = nullptr;
		WinHttpCloseHandle(handle);
		if (has_callback) WaitForSingleObject(closed, INFINITE);
	}

	bool initialize(const uint32_t timeout_ms)
	{
		completed = CreateEventW(nullptr, TRUE, FALSE, nullptr);
		closed = CreateEventW(nullptr, TRUE, FALSE, nullptr);
		if (!completed || !closed) return false;
		timeout = std::min<uint32_t>(timeout_ms ? timeout_ms : 30000, INFINITE - 1);
		DWORD_PTR context = reinterpret_cast<DWORD_PTR>(this);
		if (!WinHttpSetOption(value, WINHTTP_OPTION_CONTEXT_VALUE, &context, sizeof(context))) return false;
		if (WinHttpSetStatusCallback(value, callback,
			WINHTTP_CALLBACK_FLAG_ALL_COMPLETIONS | WINHTTP_CALLBACK_FLAG_HANDLES, 0) == WINHTTP_INVALID_STATUS_CALLBACK)
			return false;
		has_callback = true;
		return true;
	}

	template <typename F>
	bool invoke(F issue)
	{
		error = ERROR_SUCCESS;
		received = 0;
		ResetEvent(completed);
		if (!issue()) return false;
		if (WaitForSingleObject(completed, timeout) != WAIT_OBJECT_0)
		{
			// Complete cancellation before the caller releases its read/write buffers.
			close();
			SetLastError(ERROR_WINHTTP_TIMEOUT);
			return false;
		}
		if (error != ERROR_SUCCESS)
		{
			SetLastError(error);
			return false;
		}
		return true;
	}

	bool read(void* buffer, const DWORD size, DWORD& count)
	{
		if (!invoke([&] { return WinHttpReadData(value, buffer, size, nullptr); })) return false;
		count = received;
		return true;
	}
};

os::web_host_ptr os::connect_to_host(const std::string_view host, const bool secure_in, const int port_in,
                                     const std::string_view user_agent, const bool timeouts)
{
	if (host.empty() || host.size() >= INT_MAX || port_in < 0 || port_in > 65535 ||
		!web_header_value(user_agent)) return nullptr;
	for (const unsigned char c : host)
	{
		if (c <= 32 || c == 127 || std::string_view("/\\?#@").find(c) != std::string_view::npos) return nullptr;
	}

	inet_handle session_handle(InternetOpenW(widen(user_agent).c_str(), INTERNET_OPEN_TYPE_PRECONFIG, nullptr,
	                                         nullptr, 0));

	if (!session_handle.is_valid())
	{
		return nullptr;
	}

	if (timeouts && !set_web_timeouts(session_handle, 0)) return nullptr;

	const auto port = port_in == 0
		                  ? (secure_in ? INTERNET_DEFAULT_HTTPS_PORT : INTERNET_DEFAULT_HTTP_PORT)
		                  : port_in;
	inet_handle conn(InternetConnectW(session_handle, widen(host).c_str(), static_cast<INTERNET_PORT>(port), nullptr,
	                                  nullptr, INTERNET_SERVICE_HTTP, 0, 0));

	if (!conn.is_valid())
	{
		return nullptr;
	}

	// Built in place: web_host's destructor closes its handles, so a copied temporary would leave
	// the live instance holding handles that were already closed.
	auto result = std::make_shared<web_host>();
	result->session_handle = session_handle.detach();
	result->connection_handle = conn.detach();
	result->secure = secure_in;
	result->timeouts = timeouts;
	result->name = host;
	result->port = port_in;
	result->user_agent = user_agent;
	return result;
}

os::web_response os::send_request(const web_host_ptr& host, const web_request& req)
{
	web_response result;
	const auto fail = [&result](const web_response_error error)
	{
		result.error = error;
		result.body.clear();
		return result;
	};

	if (!host || (req.verb != web_request_verb::GET && req.verb != web_request_verb::POST) ||
		req.body.size() > MAXDWORD || req.path.size() >= INT_MAX ||
		(!req.path.empty() && (req.path.front() != '/' || req.path.starts_with("//"))))
		return fail(web_response_error::invalid_request);
	for (const unsigned char c : req.path)
	{
		if (c <= 32 || c == 127 || c == '\\' || c == '#') return fail(web_response_error::invalid_request);
	}

	std::string content;
	std::string header_str;

	for (const auto& h : req.headers)
	{
		if (!web_header_token(h.first) || !web_header_value(h.second) ||
			h.first.size() >= INT_MAX || header_str.size() + h.first.size() + h.second.size() + 4 >= INT_MAX ||
			iequals(h.first, "Transfer-Encoding") || (!req.use_cookies && iequals(h.first, "Cookie")))
			return fail(web_response_error::invalid_request);
		header_str += h.first;
		header_str += ": ";
		header_str += h.second;
		header_str += "\r\n";
	}

	if (!req.body.empty())
	{
		content = req.body;
	}
	else if (!req.form_data.empty())
	{
		const std::string boundary = "54B8723DE6044695A68C838E8BF0CB00";

		for (const auto& f : req.form_data)
		{
			if (!web_multipart_name(f.first) ||
				content.size() + f.first.size() + f.second.size() + 256 > MAXDWORD)
				return fail(web_response_error::invalid_request);
			content += "--";
			content += boundary;
			content += "\r\n";
			content += "Content-Disposition: form-data; name=\"";
			content += f.first;
			content += "\"\r\n";
			content += "Content-Type: text/plain; charset=\"utf-8\"\r\n";
			content += "\r\n";
			content += f.second;
			content += "\r\n";
		}

		if (!req.upload_file_path.empty() && !req.file_form_data_name.empty())
		{
			if (!web_multipart_name(req.file_form_data_name) || !web_multipart_name(req.file_name) ||
				content.size() + req.file_form_data_name.size() + req.file_name.size() + 256 > MAXDWORD)
				return fail(web_response_error::invalid_request);
			const auto content_type = has_extension(req.upload_file_path, ".zip")
				                          ? "application/x-zip-compressed"
				                          : "application/octet-stream";

			content += "--";
			content += boundary;
			content += "\r\n";
			content += "Content-Disposition: form-data; name=\"";
			content += req.file_form_data_name;
			content += "\"; filename=\"";
			content += req.file_name;
			content += "\"\r\n";
			content += "Content-Type: ";
			content += content_type;
			content += "\r\n\r\n";

			const file_handle upload(CreateFileW(widen(req.upload_file_path).c_str(), GENERIC_READ, FILE_SHARE_READ,
			                                     nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
			if (!upload.is_valid()) return fail(web_response_error::transport);
			DWORD high = 0;
			GetFileSize(upload.get(), &high);
			if (high != 0) return fail(web_response_error::transport); // over 4GB

			std::vector<uint8_t> buf(65536);
			for (;;)
			{
				DWORD bytes_read = 0;
				if (!ReadFile(upload.get(), buf.data(), static_cast<DWORD>(buf.size()), &bytes_read, nullptr))
					return fail(web_response_error::transport);
				if (bytes_read == 0) break;
				if (content.size() > MAXDWORD - 256 || bytes_read > MAXDWORD - content.size() - 256)
					return fail(web_response_error::invalid_request);
				content.append(reinterpret_cast<const char*>(buf.data()), bytes_read);
			}

			content += "\r\n";
		}

		content += "--";
		content += boundary;
		content += "--";
		header_str += "Content-Type: multipart/form-data; boundary=";
		header_str += boundary;
		header_str += "\r\n";
	}

	for (const auto& h : req.headers)
	{
		if (iequals(h.first, "Content-Length"))
		{
			uint64_t length = 0;
			if (!web_length(web_trim(h.second), length) || length != content.size())
				return fail(web_response_error::invalid_request);
		}
	}
	size_t path_bound = req.path.size();
	for (const auto& [key, value] : req.query)
	{
		if (path_bound > INT_MAX - 2 || key.size() > (INT_MAX - path_bound - 2) / 3 ||
			value.size() > (INT_MAX - path_bound - 2) / 3 - key.size())
			return fail(web_response_error::invalid_request);
		path_bound += 3 * (key.size() + value.size()) + 2;
	}
	const auto path = format_path(req);
	if (path.size() >= INT_MAX || header_str.size() >= INT_MAX)
		return fail(web_response_error::invalid_request);
	const auto wverb = req.verb == web_request_verb::GET ? L"GET" : L"POST";
	const auto wpath = widen(path.empty() ? "/" : path);
	auto flags = INTERNET_FLAG_KEEP_CONNECTION | INTERNET_FLAG_NO_CACHE_WRITE | INTERNET_FLAG_NO_AUTH |
		INTERNET_FLAG_RELOAD;
	if (host->secure) flags |= INTERNET_FLAG_SECURE;
	if (!req.follow_redirects) flags |= INTERNET_FLAG_NO_AUTO_REDIRECT;
	if (!req.use_cookies) flags |= INTERNET_FLAG_NO_COOKIES;

	// WinINet can report success on incomplete chunks and round short timeouts.
	// Bounded requests consume an isolated WinHTTP session synchronously instead.
	const bool bounded = req.max_response_bytes || req.max_header_bytes || req.timeout_ms;
	sync_http_handle bounded_session;
	sync_http_handle bounded_connection;
	bounded_http_request bounded_request;
	inet_handle request_handle;
	const auto headerW = widen(header_str);
	const auto transport_failure = [&]
	{
		const auto native_error = GetLastError();
		if (bounded && bounded_request.value && result.status_code == 0)
		{
			DWORD status = 0;
			DWORD size = sizeof(status);
			if (WinHttpQueryHeaders(bounded_request.value, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
				WINHTTP_HEADER_NAME_BY_INDEX, &status, &size, WINHTTP_NO_HEADER_INDEX))
				result.status_code = static_cast<int>(status);
		}
		return fail(bounded && native_error == ERROR_WINHTTP_HEADER_SIZE_OVERFLOW
			? web_response_error::response_limit : web_response_error::transport);
	};
	if (bounded)
	{
		bounded_session.value = WinHttpOpen(widen(host->user_agent).c_str(), WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
			WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, WINHTTP_FLAG_ASYNC);
		if (!bounded_session.value) return transport_failure();
		const auto timeout = static_cast<int>(std::min<uint32_t>(req.timeout_ms ? req.timeout_ms : 30000, INT_MAX));
		if (!WinHttpSetTimeouts(bounded_session.value, timeout, timeout, timeout, timeout))
			return transport_failure();
		const auto port = host->port ? host->port : (host->secure ? INTERNET_DEFAULT_HTTPS_PORT : INTERNET_DEFAULT_HTTP_PORT);
		bounded_connection.value = WinHttpConnect(bounded_session.value, widen(host->name).c_str(),
			static_cast<INTERNET_PORT>(port), 0);
		if (!bounded_connection.value) return transport_failure();
		bounded_request.value = WinHttpOpenRequest(bounded_connection.value, wverb, wpath.c_str(), nullptr,
			WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, host->secure ? WINHTTP_FLAG_SECURE : 0);
		if (!bounded_request.value) return transport_failure();
		if (!bounded_request.initialize(req.timeout_ms)) return transport_failure();
		DWORD receive_timeout = static_cast<DWORD>(timeout);
		if (!WinHttpSetTimeouts(bounded_request.value, timeout, timeout, timeout, timeout) ||
			!WinHttpSetOption(bounded_request.value, WINHTTP_OPTION_RECEIVE_RESPONSE_TIMEOUT,
				&receive_timeout, sizeof(receive_timeout))) return transport_failure();
		DWORD disabled = WINHTTP_DISABLE_AUTHENTICATION;
		if (!req.follow_redirects) disabled |= WINHTTP_DISABLE_REDIRECTS;
		if (!req.use_cookies) disabled |= WINHTTP_DISABLE_COOKIES;
		if (!WinHttpSetOption(bounded_request.value, WINHTTP_OPTION_DISABLE_FEATURE, &disabled, sizeof(disabled)))
			return transport_failure();
		if (req.max_header_bytes)
		{
			DWORD maximum = static_cast<DWORD>(std::min<size_t>(req.max_header_bytes, MAXDWORD));
			if (!WinHttpSetOption(bounded_request.value, WINHTTP_OPTION_MAX_RESPONSE_HEADER_SIZE,
				&maximum, sizeof(maximum))) return transport_failure();
		}
		if (!bounded_request.invoke([&] {
			return WinHttpSendRequest(bounded_request.value, headerW.c_str(), static_cast<DWORD>(headerW.size()),
				content.empty() ? nullptr : content.data(), static_cast<DWORD>(content.size()),
				static_cast<DWORD>(content.size()), reinterpret_cast<DWORD_PTR>(&bounded_request));
		}) || !bounded_request.invoke([&] { return WinHttpReceiveResponse(bounded_request.value, nullptr); }))
			return transport_failure();
	}
	else
	{
		request_handle.reset(HttpOpenRequestW(host->connection_handle, wverb, wpath.c_str(), nullptr, nullptr,
		                                      nullptr, flags, 0));
		if (!request_handle.is_valid()) return transport_failure();
		if (host->timeouts && !set_web_timeouts(request_handle, 0)) return transport_failure();

		if (content.empty())
		{
			if (!HttpSendRequestW(request_handle, headerW.c_str(), static_cast<DWORD>(headerW.size()), nullptr, 0))
				return fail(web_response_error::transport);
		}
		else
		{
			INTERNET_BUFFERSW buffers = {};
			buffers.dwStructSize = sizeof(buffers);
			buffers.lpcszHeader = headerW.c_str();
			buffers.dwHeadersTotal = buffers.dwHeadersLength = static_cast<DWORD>(headerW.size());
			buffers.dwBufferTotal = static_cast<DWORD>(content.size());

			if (!HttpSendRequestExW(request_handle, &buffers, nullptr, 0, 0))
				return fail(web_response_error::transport);

			constexpr size_t chunk_size = 8192;
			size_t total_written = 0;

			while (total_written < content.size())
			{
				const auto remaining = content.size() - total_written;
				const auto to_write = std::min(chunk_size, remaining);
				DWORD written = 0;
				if (!InternetWriteFile(request_handle, content.data() + total_written, static_cast<DWORD>(to_write),
					&written) || written == 0) return fail(web_response_error::transport);
				total_written += written;
			}
			if (!HttpEndRequestW(request_handle, nullptr, 0, 0)) return fail(web_response_error::transport);
		}
	}

	DWORD status = 0;
	DWORD size = sizeof(status);
	if (bounded ? !WinHttpQueryHeaders(bounded_request.value, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
		WINHTTP_HEADER_NAME_BY_INDEX, &status, &size, WINHTTP_NO_HEADER_INDEX)
		: !HttpQueryInfoW(request_handle, HTTP_QUERY_STATUS_CODE | HTTP_QUERY_FLAG_NUMBER,
		&status, &size, nullptr)) return transport_failure();
	result.status_code = static_cast<int>(status);

	size = 0;
	if (bounded)
	{
		if (WinHttpQueryHeaders(bounded_request.value, WINHTTP_QUERY_RAW_HEADERS_CRLF,
			WINHTTP_HEADER_NAME_BY_INDEX, nullptr, &size, WINHTTP_NO_HEADER_INDEX) ||
			GetLastError() != ERROR_INSUFFICIENT_BUFFER || size < sizeof(wchar_t))
			return transport_failure();
		const auto chars = size / sizeof(wchar_t);
		if (req.max_header_bytes && chars - 1 > req.max_header_bytes)
			return fail(web_response_error::response_limit);
		std::wstring headers(chars, L'\0');
		if (!WinHttpQueryHeaders(bounded_request.value, WINHTTP_QUERY_RAW_HEADERS_CRLF,
			WINHTTP_HEADER_NAME_BY_INDEX, headers.data(), &size, WINHTTP_NO_HEADER_INDEX))
			return transport_failure();
		headers.resize(size / sizeof(wchar_t));
		// HTTP header octets are widened one-to-one by WinHTTP, not UTF-8 text.
		result.headers.reserve(headers.size());
		for (const auto c : headers) result.headers.push_back(static_cast<char>(c));
	}
	else
	{
		if (HttpQueryInfoA(request_handle, HTTP_QUERY_RAW_HEADERS_CRLF, nullptr, &size, nullptr) ||
			GetLastError() != ERROR_INSUFFICIENT_BUFFER || size == 0)
			return fail(web_response_error::transport);
		result.headers.resize(size);
		if (!HttpQueryInfoA(request_handle, HTTP_QUERY_RAW_HEADERS_CRLF, result.headers.data(), &size, nullptr))
		{
			result.headers.clear();
			return fail(web_response_error::transport);
		}
		result.headers.resize(size);
	}
	if (req.max_header_bytes && result.headers.size() > req.max_header_bytes)
	{
		result.headers.clear();
		return fail(web_response_error::response_limit);
	}

	std::optional<uint64_t> content_length;
	bool transfer_encoded = false;
	std::string_view remaining_headers = result.headers;
	while (!remaining_headers.empty())
	{
		const auto end = remaining_headers.find("\r\n");
		const auto line = remaining_headers.substr(0, end);
		if (const auto colon = line.find(':'); colon != std::string_view::npos)
		{
			const auto name = line.substr(0, colon);
			const auto value = web_trim(line.substr(colon + 1));
			if (iequals(name, "Content-Length"))
			{
				uint64_t length = 0;
				if (!web_length(value, length) || (content_length && *content_length != length))
					return fail(web_response_error::transport);
				content_length = length;
			}
			else if (iequals(name, "Content-Type"))
				result.content_type = value;
			else if (iequals(name, "Transfer-Encoding"))
				transfer_encoded = true;
		}
		if (end == std::string_view::npos) break;
		remaining_headers.remove_prefix(end + 2);
	}
	if (transfer_encoded && content_length) return fail(web_response_error::transport);
	const bool no_body = status == 204 || status == 304 || (status >= 100 && status < 200);
	if (!no_body && req.max_response_bytes && content_length && *content_length > req.max_response_bytes)
		return fail(web_response_error::response_limit);

	std::unique_ptr<file_handle> download;
	if (!req.download_file_path.empty())
	{
		download = std::make_unique<file_handle>(CreateFileW(widen(req.download_file_path).c_str(), GENERIC_WRITE, 0,
		                                                     nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr));
		if (!download->is_valid()) return fail(web_response_error::transport);
		result.download_created = true;
	}
	uint64_t total = 0;
	for (;;)
	{
		uint8_t buffer[8192];
		DWORD read = 0;
		if (bounded ? !bounded_request.read(buffer, sizeof(buffer), read)
			: !InternetReadFile(request_handle, buffer, sizeof(buffer), &read))
			return transport_failure();
		if (read == 0) break;
		if (req.max_response_bytes && (total > req.max_response_bytes || read > req.max_response_bytes - total))
			return fail(web_response_error::response_limit);
		total += read;
		if (download)
		{
			DWORD written = 0;
			if (!WriteFile(download->get(), buffer, read, &written, nullptr) || written != read)
				return fail(web_response_error::transport);
		}
		else
			result.body.append(reinterpret_cast<const char*>(buffer), read);
	}
	if (!no_body && content_length && total != *content_length) return fail(web_response_error::transport);
	return result;
}
