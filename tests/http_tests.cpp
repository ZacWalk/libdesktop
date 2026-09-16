#include "platform.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <winsock2.h>
#include <ws2tcpip.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <thread>

namespace
{
	int checks = 0;
	int failures = 0;
#define CHECK(expr) do { ++checks; if (!(expr)) { ++failures; std::printf("FAIL %d: %s\n", __LINE__, #expr); } } while (false)

	using namespace std::chrono_literals;

	struct received_request
	{
		std::string head;
		std::string body;
	};

	bool send_all(const SOCKET socket, const std::string_view text)
	{
		size_t offset = 0;
		while (offset < text.size())
		{
			const auto written = send(socket, text.data() + offset, static_cast<int>(text.size() - offset), 0);
			if (written <= 0) return false;
			offset += written;
		}
		return true;
	}

	// Raw TCP keeps malformed lengths and chunk framing under the test's control.
	class loopback_server
	{
		SOCKET listener = INVALID_SOCKET;
		std::atomic<bool> stopping = false;
		std::thread worker;
		std::vector<std::thread> clients;
		std::mutex mutex;
		std::vector<received_request> requests;
		std::atomic<int> server_failures = 0;

		void serve(const SOCKET socket)
		{
			DWORD timeout = 2000;
			setsockopt(socket, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeout), sizeof(timeout));
			setsockopt(socket, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char*>(&timeout), sizeof(timeout));
			std::string bytes;
			char buffer[4096];
			size_t head_end = std::string::npos;
			while ((head_end = bytes.find("\r\n\r\n")) == std::string::npos && bytes.size() < 65536)
			{
				const auto read = recv(socket, buffer, sizeof(buffer), 0);
				if (read <= 0) break;
				bytes.append(buffer, read);
			}
			if (head_end == std::string::npos)
			{
				++server_failures;
				closesocket(socket);
				return;
			}
			received_request request{bytes.substr(0, head_end + 4), bytes.substr(head_end + 4)};
			const auto lower = pf::to_lower(request.head);
			size_t length = 0;
			if (const auto at = lower.find("\r\ncontent-length:"); at != std::string::npos)
				length = std::strtoull(lower.c_str() + at + 17, nullptr, 10);
			if (length > 65536)
			{
				++server_failures;
				closesocket(socket);
				return;
			}
			if (lower.find("expect: 100-continue") != std::string::npos)
				send_all(socket, "HTTP/1.1 100 Continue\r\n\r\n");
			while (request.body.size() < length)
			{
				const auto read = recv(socket, buffer, sizeof(buffer), 0);
				if (read <= 0) break;
				request.body.append(buffer, read);
			}
			{
				std::lock_guard lock(mutex);
				requests.push_back(request);
			}
			const auto first_space = request.head.find(' ');
			const auto path = request.head.substr(first_space + 1, request.head.find(' ', first_space + 1) - first_space - 1);
			std::string response;
			if (path.starts_with("/redirect"))
				response = "HTTP/1.1 302 Found\r\nLocation: /target\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
			else if (path.starts_with("/declared"))
				response = "HTTP/1.1 200 OK\r\nContent-Length: 18446744073709551614\r\nConnection: close\r\n\r\n";
			else if (path.starts_with("/truncated"))
				response = "HTTP/1.1 200 OK\r\nContent-Length: 9\r\nConnection: close\r\n\r\nabc";
			else if (path.starts_with("/chunk-truncated"))
				response = "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\nConnection: close\r\n\r\n5\r\nabc";
			else if (path.starts_with("/chunk-missing-end"))
				response = "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\nConnection: close\r\n\r\n3\r\nabc\r\n";
			else if (path.starts_with("/large-chunk"))
				response = "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\nConnection: close\r\n\r\n2328\r\n" +
					std::string(9000, 'x') + "\r\n0\r\n\r\n";
			else if (path.starts_with("/large-trailer"))
				response = "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\nConnection: close\r\n\r\n3\r\nabc\r\n0\r\nX-Trailer: " +
					std::string(16384, 'x') + "\r\n\r\n";
			else if (path.starts_with("/chunked"))
				response = "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\nConnection: close\r\n\r\n3\r\nabc\r\n3\r\ndef\r\n0\r\n\r\n";
			else if (path.starts_with("/eof"))
				response = "HTTP/1.1 200 OK\r\nConnection: close\r\n\r\nabcdef";
			else if (path.starts_with("/not-modified"))
				response = "HTTP/1.1 304 Not Modified\r\nContent-Length: 999\r\nConnection: close\r\n\r\n";
			else if (path.starts_with("/empty"))
				response = "HTTP/1.1 204 No Content\r\nConnection: close\r\n\r\n";
			else if (path.starts_with("/close"))
			{
				closesocket(socket);
				return;
			}
			else if (path.starts_with("/headers"))
				response = "HTTP/1.1 200 OK\r\nX-Large: " + std::string(16384, 'x') +
					"\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
			else if (path.starts_with("/timeout"))
			{
				std::this_thread::sleep_for(1500ms);
				response = "HTTP/1.1 200 OK\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
			}
			else if (path.starts_with("/read-timeout"))
			{
				send_all(socket, "HTTP/1.1 200 OK\r\nContent-Length: 6\r\nConnection: close\r\n\r\nabc");
				std::this_thread::sleep_for(1500ms);
				response = "def";
			}
			else
			{
				std::string extra;
				if (path.find("/cookie-redirect") != std::string::npos)
				{
					response = "HTTP/1.1 302 Found\r\nSet-Cookie: redirected=yes; Path=/\r\nLocation: /cookie-target\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
					send_all(socket, response);
					closesocket(socket);
					return;
				}
				if (path.find("/cookie-set") != std::string::npos)
				{
					const auto cookie_path = path.substr(0, path.find("/cookie-set"));
					extra = "Set-Cookie: platform_http_test=seed; Path=" + cookie_path + "\r\n";
				}
				if (path.find("/cookie-clear") != std::string::npos)
				{
					const auto cookie_path = path.substr(0, path.find("/cookie-clear"));
					extra = "Set-Cookie: platform_http_test=; Max-Age=0; Expires=Thu, 01 Jan 1970 00:00:00 GMT; Path=" +
						cookie_path + "\r\n";
				}
				const auto body = path.starts_with("/echo") ? request.body : std::string("a\0b\xff", 4);
				response = "HTTP/1.1 200 OK\r\nContent-Type: application/octet-stream\r\nX-Test: exact\r\n" +
					extra + "Content-Length: " + std::to_string(body.size()) + "\r\nConnection: close\r\n\r\n" + body;
			}
			send_all(socket, response);
			shutdown(socket, SD_BOTH);
			closesocket(socket);
		}

	public:
		int port = 0;

		loopback_server()
		{
			listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
			if (listener == INVALID_SOCKET) return;
			sockaddr_in address{};
			address.sin_family = AF_INET;
			address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
			address.sin_port = 0;
			if (bind(listener, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) != 0 ||
				listen(listener, 16) != 0) return;
			int size = sizeof(address);
			if (getsockname(listener, reinterpret_cast<sockaddr*>(&address), &size) != 0) return;
			port = ntohs(address.sin_port);
			worker = std::thread([this]
			{
				while (!stopping)
				{
					fd_set ready;
					FD_ZERO(&ready);
					FD_SET(listener, &ready);
					timeval timeout{0, 100000};
					if (select(0, &ready, nullptr, nullptr, &timeout) <= 0) continue;
					const auto client = accept(listener, nullptr, nullptr);
					if (client == INVALID_SOCKET) break;
					clients.emplace_back([this, client] { serve(client); });
				}
			});
		}

		~loopback_server()
		{
			stopping = true;
			if (worker.joinable()) worker.join();
			for (auto& client : clients) client.join();
			if (listener != INVALID_SOCKET) closesocket(listener);
			CHECK(server_failures == 0);
		}

		received_request last()
		{
			std::lock_guard lock(mutex);
			return requests.empty() ? received_request{} : requests.back();
		}

		size_t count()
		{
			std::lock_guard lock(mutex);
			return requests.size();
		}
	};

	void test_http(loopback_server& server, const bool baseline)
	{
		const auto host = pf::connect_to_host("127.0.0.1", false, server.port, "platform-loopback-tests/1.0");
		CHECK(host != nullptr);
		pf::web_request request;
		CHECK(request.follow_redirects && request.use_cookies && request.max_response_bytes == 0 &&
			request.max_header_bytes == 0 && request.timeout_ms == 0);
		request.path = "/get";
		request.headers = {{"X-Request", "exact"}, {"Authorization", "test-only-secret"}};
		auto response = pf::send_request(host, request);
		CHECK(response.error == pf::web_response_error::none);
		CHECK(response.status_code == 200);
		CHECK(response.body == std::string("a\0b\xff", 4));
		CHECK(response.content_type == "application/octet-stream");
		CHECK(response.headers.find("X-Test: exact\r\n") != std::string::npos);
		CHECK(server.last().head.find("User-Agent: platform-loopback-tests/1.0\r\n") != std::string::npos);
		CHECK(server.last().head.find("X-Request: exact\r\n") != std::string::npos);

		request.path = "/echo";
		request.verb = pf::web_request_verb::POST;
		request.body = std::string("post\0raw\xff", 9);
		response = pf::send_request(host, request);
		CHECK(response.error == pf::web_response_error::none && response.body == request.body);
		CHECK(server.last().body == request.body);
		CHECK(server.last().head.starts_with("POST /echo HTTP/"));
		request = {};
		request.path = "/get";
		response = pf::send_request(host, request);
		const auto exact_header_size = response.headers.size();
		CHECK(exact_header_size > 0);
		request.max_response_bytes = 4;
		request.max_header_bytes = exact_header_size;
		response = pf::send_request(host, request);
		CHECK(response.error == pf::web_response_error::none && response.body.size() == 4);
		if (exact_header_size > 1)
		{
			request.max_header_bytes = exact_header_size - 1;
			response = pf::send_request(host, request);
			CHECK(response.error == pf::web_response_error::response_limit && response.body.empty());
		}
		request.max_header_bytes = 1024;
		request.max_response_bytes = 3;
		response = pf::send_request(host, request);
		CHECK(response.error == pf::web_response_error::response_limit && response.body.empty());
		CHECK(response.status_code == 200);
		request.path = "/declared";
		response = pf::send_request(host, request);
		CHECK(response.error == pf::web_response_error::response_limit && response.body.empty());
		request.path = "/chunked";
		request.max_response_bytes = 6;
		response = pf::send_request(host, request);
		CHECK(response.error == pf::web_response_error::none && response.body == "abcdef");
		request.max_response_bytes = 5;
		response = pf::send_request(host, request);
		CHECK(response.error == pf::web_response_error::response_limit && response.body.empty());
		CHECK(response.status_code == 200);
		request.path = "/headers";
		response = pf::send_request(host, request);
		CHECK(response.error == pf::web_response_error::response_limit && response.headers.empty());
		// A native header-limit rejection can happen before WinHTTP exposes the status.
		CHECK(response.status_code == 0 || response.status_code == 200);
		request = {};
		request.max_response_bytes = 1024;
		for (const auto path : {"/truncated", "/chunk-truncated", "/chunk-missing-end", "/close"})
		{
			request.path = path;
			response = pf::send_request(host, request);
			CHECK(response.error == pf::web_response_error::transport && response.body.empty());
			CHECK(response.status_code == (std::string_view(path) == "/close" ? 0 : 200));
		}
		request = {};
		request.path = "/redirect";
		request.follow_redirects = false;
		const auto before = server.count();
		response = pf::send_request(host, request);
		CHECK(response.error == pf::web_response_error::none && response.status_code == 302);
		CHECK(response.headers.find("Location: /target") != std::string::npos);
		CHECK(server.count() == before + 1);
		request.follow_redirects = true;
		response = pf::send_request(host, request);
		CHECK(response.error == pf::web_response_error::none && response.status_code == 200);
		CHECK(server.last().head.starts_with("GET /target "));

		const auto unique = "/platform-test-" + std::to_string(server.port);
		request.path = unique + "/cookie-set";
		response = pf::send_request(host, request);
		CHECK(response.error == pf::web_response_error::none);
		request.path = unique + "/cookie-check";
		pf::send_request(host, request);
		CHECK(pf::to_lower(server.last().head).find("\r\ncookie:") != std::string::npos);
		request.use_cookies = false;
		pf::send_request(host, request);
		CHECK(pf::to_lower(server.last().head).find("\r\ncookie:") == std::string::npos);
		request.use_cookies = true;
		request.path = unique + "/cookie-clear";
		pf::send_request(host, request);
		request.use_cookies = false;
		request.path = unique + "-no-store/cookie-set";
		pf::send_request(host, request);
		request.use_cookies = true;
		request.path = unique + "-no-store/cookie-check";
		pf::send_request(host, request);
		CHECK(pf::to_lower(server.last().head).find("\r\ncookie:") == std::string::npos);
		request.path = unique + "-no-store/cookie-clear";
		pf::send_request(host, request);

		request = {};
		request.timeout_ms = 100;
		for (const auto path : {"/timeout", "/read-timeout"})
		{
			request.path = path;
			const auto start = std::chrono::steady_clock::now();
			response = pf::send_request(host, request);
			CHECK(response.error == pf::web_response_error::transport && response.body.empty());
			CHECK(std::chrono::steady_clock::now() - start < 1400ms);
		}
		request.timeout_ms = 0;
		request.path = "/timeout";
		response = pf::send_request(host, request);
		CHECK(response.error == pf::web_response_error::none && response.status_code == 200);

		// The pre-fix red run exercises only known loopback targets.
		if (baseline) return;
		const auto invalid_before = server.count();
		for (const auto path : {"http://elsewhere.invalid/", "/get\r\nX-Injected: yes", "//elsewhere.invalid/"})
		{
			request.path = path;
			response = pf::send_request(host, request);
			CHECK(response.error == pf::web_response_error::invalid_request && response.body.empty());
		}
		request.path = "/get";
		for (const auto headers : {pf::web_params{{"X-Bad\r\nInjected", "value"}},
			pf::web_params{{"X-Bad", "value\r\nInjected: yes"}}, pf::web_params{{"X Bad", "value"}},
			pf::web_params{{"X-Bad", std::string("a\0b", 3)}}, pf::web_params{{"Content-Length", "5"}},
			pf::web_params{{"Transfer-Encoding", "chunked"}}})
		{
			request.headers = headers;
			response = pf::send_request(host, request);
			CHECK(response.error == pf::web_response_error::invalid_request && response.body.empty());
		}
		CHECK(server.count() == invalid_before);
		response = pf::send_request({}, {});
		CHECK(response.error == pf::web_response_error::invalid_request && response.body.empty());
	}

	void test_bounded_http(loopback_server& server)
	{
		const auto host = pf::connect_to_host("127.0.0.1", false, server.port, "bounded-agent/1.0");
		pf::web_request request;
		request.follow_redirects = false;
		request.use_cookies = false;
		request.max_header_bytes = 1024;
		request.max_response_bytes = 65536;
		request.timeout_ms = 1000;
		request.path = "/echo";
		request.verb = pf::web_request_verb::POST;
		request.body = std::string("binary\0payload\xff", 15);
		request.headers = {{"Authorization", "test-only-secret"}, {"X-Exact", "value"}, {"Content-Type", "application/json"}};
		auto response = pf::send_request(host, request);
		CHECK(response.error == pf::web_response_error::none && response.status_code == 200);
		CHECK(response.body == request.body && server.last().body == request.body);
		CHECK(response.content_type == "application/octet-stream");
		CHECK(response.headers.find("X-Test: exact") != std::string::npos);
		CHECK(server.last().head.find("User-Agent: bounded-agent/1.0\r\n") != std::string::npos);
		CHECK(server.last().head.find("Authorization: test-only-secret\r\n") != std::string::npos);
		CHECK(server.last().head.find("X-Exact: value\r\n") != std::string::npos);
		CHECK(server.last().head.find("Content-Type: application/json\r\n") != std::string::npos);
		request.verb = pf::web_request_verb::GET;
		response = pf::send_request(host, request);
		CHECK(response.error == pf::web_response_error::none && response.body == request.body);
		request.body.clear();
		request.headers.clear();
		request.path = "/get?existing=1";
		request.query = {{"encoded key", "a&b"}};
		response = pf::send_request(host, request);
		CHECK(response.error == pf::web_response_error::none);
		CHECK(server.last().head.starts_with("GET /get?existing=1&encoded%20key=a%26b "));
		request.path.clear();
		response = pf::send_request(host, request);
		CHECK(response.error == pf::web_response_error::none);
		CHECK(server.last().head.starts_with("GET /?encoded%20key=a%26b "));
		request.query.clear();
		request.path = "/redirect";
		const auto before = server.count();
		response = pf::send_request(host, request);
		CHECK(response.error == pf::web_response_error::none && response.status_code == 302);
		CHECK(server.count() == before + 1);
		CHECK(response.headers.find("Location: /target") != std::string::npos);
		request.follow_redirects = true;
		response = pf::send_request(host, request);
		CHECK(response.error == pf::web_response_error::none && response.status_code == 200);
		CHECK(server.last().head.starts_with("GET /target "));
		request.path = "/cookie-redirect";
		response = pf::send_request(host, request);
		CHECK(response.error == pf::web_response_error::none && response.status_code == 200);
		CHECK(pf::to_lower(server.last().head).find("\r\ncookie:") == std::string::npos);
		request.use_cookies = true;
		response = pf::send_request(host, request);
		CHECK(response.error == pf::web_response_error::none && response.status_code == 200);
		CHECK(pf::to_lower(server.last().head).find("\r\ncookie: redirected=yes") != std::string::npos);
		request.path = "/cookie-target";
		response = pf::send_request(host, request);
		CHECK(response.error == pf::web_response_error::none);
		CHECK(pf::to_lower(server.last().head).find("\r\ncookie:") == std::string::npos);
		request.use_cookies = false;
		for (const auto path : {"/eof", "/large-chunk"})
		{
			request.path = path;
			request.max_response_bytes = std::string_view(path) == "/eof" ? 6 : 9000;
			response = pf::send_request(host, request);
			CHECK(response.error == pf::web_response_error::none && response.body.size() == request.max_response_bytes);
			--request.max_response_bytes;
			response = pf::send_request(host, request);
			CHECK(response.error == pf::web_response_error::response_limit && response.body.empty());
		}
		request.max_response_bytes = 1;
		for (const auto path : {"/empty", "/not-modified"})
		{
			request.path = path;
			response = pf::send_request(host, request);
			CHECK(response.error == pf::web_response_error::none && response.body.empty());
		}
		request.max_response_bytes = 1024;
		request.path = "/large-trailer";
		response = pf::send_request(host, request);
		// WinHTTP versions may discard or reject oversized trailers; never return them as headers.
		CHECK(response.error == pf::web_response_error::none ? response.body == "abc" : response.body.empty());
		CHECK(response.headers.size() <= request.max_header_bytes && response.headers.find("X-Trailer") == std::string::npos);
		request.path = "/get";
		request.headers = {{"Cookie", "should-not-be-sent=yes"}};
		const auto invalid_before = server.count();
		response = pf::send_request(host, request);
		CHECK(response.error == pf::web_response_error::invalid_request);
		CHECK(server.count() == invalid_before);
		CHECK(!pf::connect_to_host("127.0.0.1\r\ninjected", false, server.port));
		CHECK(!pf::connect_to_host("http://127.0.0.1", false, server.port));
		CHECK(!pf::connect_to_host("127.0.0.1", false, 65536));
		CHECK(!pf::connect_to_host("127.0.0.1", false, server.port, "agent\r\nInjected: yes"));
	}

	void test_http_files(loopback_server& server)
	{
		const auto directory = std::filesystem::path(pf::file_path::module_folder().view()) /
			("http-fixture-" + std::to_string(server.port));
		const bool created = std::filesystem::create_directory(directory);
		CHECK(created);
		if (!created) return;
		const auto upload = directory / "upload.bin";
		const auto download = directory / "download.bin";
		const std::string payload("file\0bytes\xff", 11);
		{
			std::ofstream out(upload, std::ios::binary);
			out.write(payload.data(), payload.size());
		}
		const auto host = pf::connect_to_host("127.0.0.1", false, server.port);
		for (const bool bounded : {false, true})
		{
			pf::web_request request;
			if (bounded)
			{
				request.max_response_bytes = 65536;
				request.max_header_bytes = 1024;
				request.timeout_ms = 1000;
				request.use_cookies = false;
				request.follow_redirects = false;
			}
			request.path = "/echo";
			request.verb = pf::web_request_verb::POST;
			request.form_data = {{"field", "text-value"}};
			request.file_form_data_name = "file";
			request.file_name = "upload.bin";
			request.upload_file_path = pf::file_path(upload.string());
			auto response = pf::send_request(host, request);
			CHECK(response.error == pf::web_response_error::none);
			CHECK(response.body.find("name=\"field\"\r\n") != std::string::npos);
			CHECK(response.body.find("name=\"file\"; filename=\"upload.bin\"") != std::string::npos);
			CHECK(response.body.find(payload) != std::string::npos);
			CHECK(server.last().head.find("Content-Type: multipart/form-data; boundary=") != std::string::npos);
			CHECK(response.body == server.last().body);
			request.path = "/get";
			request.verb = pf::web_request_verb::GET;
			request.form_data.clear();
			request.upload_file_path = {};
			request.download_file_path = pf::file_path(download.string());
			response = pf::send_request(host, request);
			CHECK(response.error == pf::web_response_error::none && response.body.empty());
			std::ifstream in(download, std::ios::binary);
			const std::string actual((std::istreambuf_iterator<char>(in)), {});
			CHECK(actual == std::string("a\0b\xff", 4));
			in.close();
			if (bounded)
			{
				request.path = "/declared";
				response = pf::send_request(host, request);
				CHECK(response.error == pf::web_response_error::response_limit);
				CHECK(std::filesystem::file_size(download) == 4);
				request.path = "/large-chunk";
				request.max_response_bytes = 8500;
				response = pf::send_request(host, request);
				CHECK(response.error == pf::web_response_error::response_limit);
				CHECK(std::filesystem::file_size(download) <= 8500);
			}
		}
		std::filesystem::remove(upload);
		std::filesystem::remove(download);
		std::filesystem::remove(directory);
	}
}

app_init_result app_init(const pf::window_frame_ptr&, std::span<const std::string_view>) { return {false, 0}; }
void app_idle() {}
void app_destroy() {}

int main(const int argc, char** argv)
{
	WSADATA data{};
	if (WSAStartup(MAKEWORD(2, 2), &data) != 0) return 1;
	{
		loopback_server server;
		CHECK(server.port != 0);
		if (server.port != 0)
		{
			test_http(server, argc > 1 && std::string_view(argv[1]) == "--baseline-red");
			test_bounded_http(server);
			test_http_files(server);
		}
	}
	WSACleanup();
	std::printf("HTTP tests: %s (%d checks, %d failures)\n", failures ? "FAIL" : "PASS", checks, failures);
	return failures ? 1 : 0;
}
