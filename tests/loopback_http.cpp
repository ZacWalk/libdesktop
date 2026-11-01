// loopback_http.cpp - the Winsock side of loopback_http.h: one thread accepts, and each connection is
// served on its own thread, so a response held back to test a timeout does not hold up the next test.

#include "loopback_http.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <winsock2.h>
#include <ws2tcpip.h>

#pragma comment(lib, "ws2_32.lib")

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <format>
#include <map>
#include <mutex>
#include <thread>

namespace
{
	bool iequal(const std::string_view a, const std::string_view b)
	{
		if (a.size() != b.size()) return false;
		for (size_t i = 0; i < a.size(); ++i)
		{
			auto x = a[i];
			auto y = b[i];
			if (x >= 'A' && x <= 'Z') x = static_cast<char>(x - 'A' + 'a');
			if (y >= 'A' && y <= 'Z') y = static_cast<char>(y - 'A' + 'a');
			if (x != y) return false;
		}
		return true;
	}

	std::string_view trim(std::string_view text)
	{
		while (!text.empty() && (text.front() == ' ' || text.front() == '\t')) text.remove_prefix(1);
		while (!text.empty() && (text.back() == ' ' || text.back() == '\t')) text.remove_suffix(1);
		return text;
	}

	std::string header_in(const std::string_view headers, const std::string_view name)
	{
		size_t start = 0;
		while (start < headers.size())
		{
			auto end = headers.find("\r\n", start);
			if (end == std::string_view::npos) end = headers.size();
			const auto line = headers.substr(start, end - start);
			if (const auto colon = line.find(':'); colon != std::string_view::npos &&
				iequal(line.substr(0, colon), name))
				return std::string(trim(line.substr(colon + 1)));
			start = end + 2;
		}
		return {};
	}
}

std::string loopback::http_response(const int status, const std::string_view reason, const std::string_view body,
                                    const std::string_view extra_headers)
{
	return std::format("HTTP/1.1 {} {}\r\nContent-Length: {}\r\nConnection: close\r\n{}\r\n{}", status, reason,
	                   body.size(), extra_headers, body);
}

std::string loopback::header_value(const recorded_request& request, const std::string_view name)
{
	return header_in(request.headers, name);
}

struct loopback::server::impl
{
	SOCKET listener = INVALID_SOCKET;
	int port = 0;
	bool started = false;
	std::atomic<bool> stopping = false;
	std::thread acceptor;

	mutable std::mutex mutex;
	std::condition_variable stopped;
	std::map<std::string, response> responses;
	std::vector<recorded_request> recorded;
	std::vector<std::thread> connections;

	void serve(const SOCKET client)
	{
		DWORD timeout = 10000;
		setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeout), sizeof(timeout));

		std::string data;
		char buffer[8192];
		size_t header_end = std::string::npos;

		while (header_end == std::string::npos)
		{
			const auto n = recv(client, buffer, sizeof(buffer), 0);
			if (n <= 0) break;
			data.append(buffer, static_cast<size_t>(n));
			header_end = data.find("\r\n\r\n");
		}

		if (header_end == std::string::npos)
		{
			closesocket(client);
			return;
		}

		recorded_request request;
		const auto line_end = data.find("\r\n");
		request.request_line = data.substr(0, line_end);
		request.headers = line_end < header_end ? data.substr(line_end + 2, header_end - line_end - 2) : std::string();
		request.body = data.substr(header_end + 4);

		const auto length_text = header_in(request.headers, "Content-Length");
		const size_t length = length_text.empty() ? 0 : std::stoull(length_text);

		while (request.body.size() < length)
		{
			const auto n = recv(client, buffer, sizeof(buffer), 0);
			if (n <= 0) break;
			request.body.append(buffer, static_cast<size_t>(n));
		}

		auto path = request.request_line;
		if (const auto space = path.find(' '); space != std::string::npos) path = path.substr(space + 1);
		if (const auto space = path.find(' '); space != std::string::npos) path.resize(space);
		if (const auto query = path.find('?'); query != std::string::npos) path.resize(query);

		response reply;
		{
			std::lock_guard lock(mutex);
			recorded.push_back(request);
			if (const auto found = responses.find(path); found != responses.end()) reply = found->second;
			else reply.raw = http_response(404, "Not Found", "missing");
		}

		if (reply.delay_ms)
		{
			std::unique_lock lock(mutex);
			stopped.wait_for(lock, std::chrono::milliseconds(reply.delay_ms), [this] { return stopping.load(); });
		}

		size_t sent = 0;
		while (sent < reply.raw.size())
		{
			const auto n = send(client, reply.raw.data() + sent, static_cast<int>(reply.raw.size() - sent), 0);
			if (n <= 0) break;
			sent += static_cast<size_t>(n);
		}

		shutdown(client, SD_SEND);
		while (recv(client, buffer, sizeof(buffer), 0) > 0)
		{
		}
		closesocket(client);
	}
};

loopback::server::server() : _impl(std::make_unique<impl>())
{
	WSADATA wsa;
	if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) return;
	_impl->started = true;

	_impl->listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
	if (_impl->listener == INVALID_SOCKET) return;

	sockaddr_in address{};
	address.sin_family = AF_INET;
	address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	address.sin_port = 0;

	int size = sizeof(address);
	if (bind(_impl->listener, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) != 0 ||
		listen(_impl->listener, SOMAXCONN) != 0 ||
		getsockname(_impl->listener, reinterpret_cast<sockaddr*>(&address), &size) != 0)
	{
		closesocket(_impl->listener);
		_impl->listener = INVALID_SOCKET;
		return;
	}

	_impl->port = ntohs(address.sin_port);
	_impl->acceptor = std::thread([impl = _impl.get()]
	{
		for (;;)
		{
			const auto client = accept(impl->listener, nullptr, nullptr);
			if (client == INVALID_SOCKET) break;
			std::lock_guard lock(impl->mutex);
			impl->connections.emplace_back([impl, client] { impl->serve(client); });
		}
	});
}

loopback::server::~server()
{
	{
		std::lock_guard lock(_impl->mutex);
		_impl->stopping = true;
	}
	_impl->stopped.notify_all();

	if (_impl->listener != INVALID_SOCKET) closesocket(_impl->listener);
	if (_impl->acceptor.joinable()) _impl->acceptor.join();

	std::vector<std::thread> connections;
	{
		std::lock_guard lock(_impl->mutex);
		connections.swap(_impl->connections);
	}
	for (auto& c : connections) c.join();

	if (_impl->started) WSACleanup();
}

bool loopback::server::listening() const
{
	return _impl->port != 0;
}

int loopback::server::port() const
{
	return _impl->port;
}

void loopback::server::respond(std::string path, response r)
{
	std::lock_guard lock(_impl->mutex);
	_impl->responses[std::move(path)] = std::move(r);
}

std::vector<loopback::recorded_request> loopback::server::requests() const
{
	std::lock_guard lock(_impl->mutex);
	return _impl->recorded;
}

loopback::recorded_request loopback::server::last() const
{
	std::lock_guard lock(_impl->mutex);
	return _impl->recorded.empty() ? recorded_request{} : _impl->recorded.back();
}
