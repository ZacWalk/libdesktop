// loopback_http.h - a scripted HTTP/1.1 server on 127.0.0.1, so the web clients can be tested
// offline. Each path answers with the bytes a test gives it, and every request is recorded as it
// arrived. The implementation is in loopback_http.cpp, which keeps Winsock out of the test files.

#pragma once

#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace loopback
{
	struct response
	{
		// Sent verbatim: status line, headers and body. Declaring a longer body than is sent, then
		// closing, is how a test cuts a response short.
		std::string raw;
		int delay_ms = 0;
	};

	struct recorded_request
	{
		std::string request_line;
		std::string headers;
		std::string body;
	};

	// A response with Content-Length and Connection: close; extra_headers are complete lines.
	std::string http_response(int status, std::string_view reason, std::string_view body,
	                          std::string_view extra_headers = {});

	// The value of the first header with this name, compared without regard to ASCII case.
	std::string header_value(const recorded_request& request, std::string_view name);

	class server
	{
	public:
		server();
		~server();
		server(const server&) = delete;
		server& operator=(const server&) = delete;

		[[nodiscard]] bool listening() const;
		[[nodiscard]] int port() const;

		// The path is matched without its query; an unscripted path answers 404.
		void respond(std::string path, response r);
		[[nodiscard]] std::vector<recorded_request> requests() const;
		[[nodiscard]] recorded_request last() const;

	private:
		struct impl;
		std::unique_ptr<impl> _impl;
	};
}
