// os_tests.cpp - the Windows os:: services, checked without a window: the web client against a scripted loopback
// server, clipboard text, the known folders, and the audio endpoint. The platform suite keeps its
// own tests of these services through its public API; these test the one implementation underneath.

#include "os.h"
#include "test.h"
#include "platform/test_support.h"
#include "../loopback_http.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <mmreg.h>

#include <chrono>
#include <array>
#include <cstdio>
#include <filesystem>
#include <format>
#include <fstream>

namespace
{
	std::filesystem::path temp_path(const std::string_view name)
	{
		return std::filesystem::temp_directory_path() / std::format("libdesktop-os-{}-{}", GetCurrentProcessId(), name);
	}

	std::string utf8(const std::filesystem::path& path)
	{
		const auto u8 = path.u8string();
		return {reinterpret_cast<const char*>(u8.data()), u8.size()};
	}

	std::string read_file(const std::filesystem::path& path)
	{
		std::ifstream in(path, std::ios::binary);
		return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
	}

	void write_file(const std::filesystem::path& path, const std::string_view text)
	{
		std::ofstream out(path, std::ios::binary | std::ios::trunc);
		out.write(text.data(), static_cast<std::streamsize>(text.size()));
	}

	void test_url_encode()
	{
		assert_equal_strict(("AZaz09-_.~"), (os::url_encode("AZaz09-_.~")), "os::url_encode(\"AZaz09-_.~\") == \"AZaz09-_.~\"");
		assert_equal_strict(("x%20y%2Fz"), (os::url_encode("x y/z")), "os::url_encode(\"x y/z\") == \"x%20y%2Fz\"");
		assert_equal_strict(("%C3%A9"), (os::url_encode("\xC3\xA9")), "os::url_encode(\"\xC3\xA9\") == \"%C3%A9\"");
	}

	void test_known_folders()
	{
		uint32_t status = 1;
		const auto local = os::known_folder_path(os::known_folder::local_app_data, false, &status);
		assert_equal(true, (!local.empty()), "!local.empty()");
		assert_equal(true, (status) == (0u), "status == 0u");
		assert_equal(true,
			(std::filesystem::is_directory(std::filesystem::path(std::u8string(local.begin(), local.end())))),
			"std::filesystem::is_directory(std::filesystem::path(std::u8string(local.begin(), local.end())))");

		for (const auto folder : {os::known_folder::profile, os::known_folder::documents, os::known_folder::pictures,
		                          os::known_folder::public_documents})
		{
			assert_equal(true, (!os::known_folder_path(folder).empty()), "!os::known_folder_path(folder).empty()");
		}
	}

	void test_files()
	{
		const auto root = temp_path("files");
		std::filesystem::remove_all(root);
		std::filesystem::create_directories(root);
		const auto source = root / "source.bin";
		const auto copied = root / "copied.bin";
		const auto renamed = root / "renamed.bin";
		write_file(source, "0123456789");

		const auto attributes = os::query_file(utf8(source));
		assert_equal(true, (attributes.exists()), "attributes.exists()");
		assert_equal(true, (!attributes.directory), "!attributes.directory");
		assert_equal(true, (attributes.identity.valid), "attributes.identity.valid");
		assert_equal(true, (attributes.size) == (10u), "attributes.size == 10u");

		const auto listing = os::enumerate_files(utf8(root), false);
		assert_equal(true, (listing.success), "listing.success");
		assert_equal(true, (listing.entries.size()) == (1u), "listing.entries.size() == 1u");
		if (!listing.entries.empty())
			assert_equal(
				true, (listing.entries.front().name == "source.bin"), "listing.entries.front().name == \"source.bin\"");

		const auto file = os::open_file(utf8(source), os::file_open_mode::read);
		assert_equal(true, (file != nullptr), "file != nullptr");
		if (file)
		{
			std::array<uint8_t, 4> bytes = {};
			assert_equal(true, (file->read_at(3, bytes.data(), bytes.size())) == (4u),
				"file->read_at(3, bytes.data(), bytes.size()) == 4u");
			assert_equal(true, (std::string(bytes.begin(), bytes.end()) == "3456"), "std::string(bytes.begin(), bytes.end()) == \"3456\"");
			assert_equal(true, (file->position()) == (0u), "file->position() == 0u");
		}

		assert_equal(true, (os::copy_file(utf8(source), utf8(copied), true).success()),
			"os::copy_file(utf8(source), utf8(copied), true).success()");
		const auto collision = os::copy_file(utf8(source), utf8(copied), true);
		assert_equal(true, (collision.code == os::file_operation_code::already_exists),
			"collision.code == os::file_operation_code::already_exists");
		assert_equal(true, (!collision.diagnostic.empty()), "!collision.diagnostic.empty()");
		assert_equal(true, (os::move_file(utf8(copied), utf8(renamed), true).success()),
			"os::move_file(utf8(copied), utf8(renamed), true).success()");
		const auto moved_collision = os::move_file(utf8(source), utf8(renamed), true);
		assert_equal(true, (moved_collision.code == os::file_operation_code::already_exists),
			"moved_collision.code == os::file_operation_code::already_exists");
		assert_equal(true, (!moved_collision.diagnostic.empty()), "!moved_collision.diagnostic.empty()");
		assert_equal(true, (os::delete_file(utf8(source)).success()), "os::delete_file(utf8(source)).success()");
		assert_equal(true, (os::delete_file(utf8(renamed)).success()), "os::delete_file(utf8(renamed)).success()");
		std::filesystem::remove_all(root);
	}

	// A folder is listed with one read of it rather than one open per entry, and what the listing says
	// of each entry is what asking for that entry alone answers.
	void test_enumerate_files()
	{
		const auto root = temp_path("listing");
		std::filesystem::remove_all(root);
		std::filesystem::create_directories(root / "child");
		write_file(root / "photo.jpg", "0123456789");
		write_file(root / "hidden.txt", "x");
		SetFileAttributesW((root / "hidden.txt").c_str(), FILE_ATTRIBUTE_HIDDEN);

		const auto listing = os::enumerate_files(utf8(root), false);
		assert_equal(true, (listing.success), "listing.success");
		assert_equal(true, (listing.entries.size()) == (2u), "listing.entries.size() == 2u");

		for (const auto& entry : listing.entries)
		{
			const auto alone = os::query_file(utf8(root) + "\\" + entry.name);
			assert_equal(true, (entry.attributes.exists()), "entry.attributes.exists()");
			assert_equal(true, (entry.attributes.identity.valid), "entry.attributes.identity.valid");
			assert_equal(true, (entry.attributes.identity == alone.identity), "entry.attributes.identity == alone.identity");
			assert_equal(true, (entry.attributes.directory) == (alone.directory), "entry.attributes.directory == alone.directory");
			assert_equal(true, (entry.attributes.size) == (alone.size), "entry.attributes.size == alone.size");
			assert_equal(true, (entry.attributes.modified) == (alone.modified), "entry.attributes.modified == alone.modified");
			assert_equal(true, (entry.can_recurse) == (entry.name == "child"), "entry.can_recurse == entry.name == \"child\"");
		}

		assert_equal(true, (os::enumerate_files(utf8(root), true).entries.size()) == (3u),
			"os::enumerate_files(utf8(root), true).entries.size() == 3u");

		const auto missing = os::enumerate_files(utf8(root / "missing"), false);
		assert_equal(true, (!missing.success), "!missing.success");
		assert_equal(true, (missing.error == ERROR_FILE_NOT_FOUND || missing.error == ERROR_PATH_NOT_FOUND),
			"missing.error == ERROR_FILE_NOT_FOUND || missing.error == ERROR_PATH_NOT_FOUND");

		SetFileAttributesW((root / "hidden.txt").c_str(), FILE_ATTRIBUTE_NORMAL);
		std::filesystem::remove_all(root);
	}

	// What a file system's ID for a file gives an identity. FAT's root, ReFS's 128-bit IDs and a server
	// reporting no ID cannot be made here, so the rule is checked as a table; a volume root and a folder
	// on this machine's disk are checked as they are.
	void test_identity_index()
	{
		assert_equal(true, (os::identity_index(5, 0, false) == std::optional<uint64_t>{5}),
			"os::identity_index(5, 0, false) == std::optional<uint64_t>{5}");
		assert_equal(true, (!os::identity_index(0, 0, false).has_value()), "!os::identity_index(0, 0, false).has_value()");
		assert_equal(true, (os::identity_index(0, 0, true) == std::optional<uint64_t>{0}),
			"os::identity_index(0, 0, true) == std::optional<uint64_t>{0}");
		assert_equal(true, (!os::identity_index(~uint64_t{0}, 0, true).has_value()),
			"!os::identity_index(~uint64_t{0}, 0, true).has_value()");
		assert_equal(true, (!os::identity_index(5, 1, false).has_value()), "!os::identity_index(5, 1, false).has_value()");
		assert_equal(true, (!os::identity_index(~uint64_t{0}, ~uint64_t{0}, false).has_value()),
			"!os::identity_index(~uint64_t{0}, ~uint64_t{0}, false).has_value()");

		const auto temp = std::filesystem::temp_directory_path();
		assert_equal(true, (os::query_file(utf8(temp.root_path())).identity.valid),
			"os::query_file(utf8(temp.root_path())).identity.valid");
		assert_equal(true, (os::query_file(utf8(temp)).identity.valid), "os::query_file(utf8(temp)).identity.valid");
	}

	// The user's clipboard text is put back afterwards.
	void test_clipboard_text()
	{
		const auto read = []
		{
			return retry_clipboard([] { return os::read_clipboard_text(0); },
				[](const os::clipboard_text& result) { return !result.opened; });
		};
		const auto write = [](const std::string_view text)
		{
			return retry_clipboard([&] { return os::write_clipboard_text(0, text); },
				[](const os::clipboard_write result) { return result == os::clipboard_write::open_failed; });
		};
		const auto before = read();
		assert_equal(true, (before.opened), "before.opened");
		if (!before.opened) return;

		const std::string text = "line one\r\nline two\t\xC3\xA9\xE2\x82\xAC";
		assert_equal(true, (write(text) == os::clipboard_write::ok), "write(text) == os::clipboard_write::ok");
		assert_equal(true, (os::clipboard_has_text()), "os::clipboard_has_text()");

		const auto after = read();
		assert_equal(true, (after.opened), "after.opened");
		assert_equal(true, (after.text.has_value()), "after.text.has_value()");
		assert_equal_strict((text), (after.text.value_or(std::string())), "after.text.value_or(std::string()) == text");

		assert_equal(true, (write("") == os::clipboard_write::ok), "write(\"\") == os::clipboard_write::ok");
		assert_equal_strict((""), (read().text.value_or("unset")), "read().text.value_or(\"unset\") == \"\"");

		if (before.text)
			assert_equal(true, (write(*before.text) == os::clipboard_write::ok),
				"write(*before.text) == os::clipboard_write::ok");
	}

	void test_web_client()
	{
		loopback::server server;
		assert_equal(true, (server.listening()), "server.listening()");
		if (!server.listening()) return;

		constexpr std::string_view boundary = "54B8723DE6044695A68C838E8BF0CB00";
		server.respond("/hello", {loopback::http_response(200, "OK", "hello world", "Content-Type: text/plain\r\n")});
		server.respond("/file", {loopback::http_response(200, "OK", "file body")});
		server.respond("/short", {"HTTP/1.1 200 OK\r\nContent-Length: 100\r\nConnection: close\r\n\r\n0123456789"});
		server.respond("/empty", {"HTTP/1.1 204 No Content\r\nConnection: close\r\n\r\n"});
		server.respond("/slow", {loopback::http_response(200, "OK", "late"), 3000});

		assert_equal(true, (os::connect_to_host("", false, server.port(), "agent") == nullptr),
			"os::connect_to_host(\"\", false, server.port(), \"agent\") == nullptr");
		assert_equal(true, (os::connect_to_host("127.0.0.1/x", false, server.port(), "agent") == nullptr),
			"os::connect_to_host(\"127.0.0.1/x\", false, server.port(), \"agent\") == nullptr");
		assert_equal(true, (os::connect_to_host("127.0.0.1", false, 70000, "agent") == nullptr),
			"os::connect_to_host(\"127.0.0.1\", false, 70000, \"agent\") == nullptr");
		assert_equal(true, (os::connect_to_host("127.0.0.1", false, server.port(), "bad\r\nagent") == nullptr),
			"os::connect_to_host(\"127.0.0.1\", false, server.port(), \"bad\r\nagent\") == nullptr");

		const auto host = os::connect_to_host("127.0.0.1", false, server.port(), "libdesktop-os-tests/1.0");
		assert_equal(true, (host != nullptr), "host != nullptr");
		if (!host) return;

		os::web_request get;
		get.path = "/hello";
		auto r = os::send_request(host, get);
		assert_equal(true, (r.status_code) == (200), "r.status_code == 200");
		assert_equal_strict(("hello world"), (r.body), "r.body == \"hello world\"");
		assert_equal_strict(("text/plain"), (r.content_type), "r.content_type == \"text/plain\"");
		assert_equal(true, (r.error == os::web_response_error::none), "r.error == os::web_response_error::none");
		assert_equal(true, (r.headers.starts_with("HTTP/1.1 200 OK\r\n")), "r.headers.starts_with(\"HTTP/1.1 200 OK\r\n\")");
		assert_equal(true, (!r.download_created), "!r.download_created");
		assert_equal_strict(("GET /hello HTTP/1.1"), (server.last().request_line), "server.last().request_line == \"GET /hello HTTP/1.1\"");
		assert_equal_strict(("libdesktop-os-tests/1.0"), (loopback::header_value(server.last(), "User-Agent")),
			"loopback::header_value(server.last(), \"User-Agent\") == \"libdesktop-os-tests/1.0\"");

		os::web_request query;
		query.path = "/query?x=1";
		query.query = {{"a", "1"}, {"b", "x y"}};
		query.headers = {{"X-Test", "1"}};
		r = os::send_request(host, query);
		assert_equal(true, (r.status_code) == (404), "r.status_code == 404");
		assert_equal_strict(("missing"), (r.body), "r.body == \"missing\"");
		assert_equal_strict(("GET /query?x=1&a=1&b=x%20y HTTP/1.1"), (server.last().request_line),
			"server.last().request_line == \"GET /query?x=1&a=1&b=x%20y HTTP/1.1\"");
		assert_equal_strict(("1"), (loopback::header_value(server.last(), "X-Test")),
			"loopback::header_value(server.last(), \"X-Test\") == \"1\"");

		os::web_request posted;
		posted.verb = os::web_request_verb::POST;
		posted.path = "/body";
		posted.body = "raw body";
		posted.headers = {{"Content-Type", "text/plain"}, {"Content-Length", "8"}};
		r = os::send_request(host, posted);
		assert_equal_strict(("POST /body HTTP/1.1"), (server.last().request_line), "server.last().request_line == \"POST /body HTTP/1.1\"");
		assert_equal_strict(("raw body"), (server.last().body), "server.last().body == \"raw body\"");

		const auto upload_path = temp_path("upload.ZIP");
		write_file(upload_path, std::string_view("PK\x03\x04zip", 7));
		os::web_request upload;
		upload.verb = os::web_request_verb::POST;
		upload.path = "/upload";
		upload.form_data = {{"k", "v"}};
		upload.file_form_data_name = "ff";
		upload.file_name = "logs.zip";
		upload.upload_file_path = utf8(upload_path);
		r = os::send_request(host, upload);
		assert_equal(true, (r.status_code) == (404), "r.status_code == 404");
		assert_equal_strict((std::format("multipart/form-data; boundary={}", boundary)),
			(loopback::header_value(server.last(), "Content-Type")),
			"loopback::header_value(server.last(), \"Content-Type\") == std::format(\"multipart/form-data; boundary={}\", boundary)");
		assert_equal_strict((std::format(
			"--{0}\r\nContent-Disposition: form-data; name=\"k\"\r\nContent-Type: text/plain; charset=\"utf-8\"\r\n"
			"\r\nv\r\n--{0}\r\nContent-Disposition: form-data; name=\"ff\"; filename=\"logs.zip\"\r\n"
			"Content-Type: application/x-zip-compressed\r\n\r\n{1}\r\n--{0}--", boundary,
			std::string_view("PK\x03\x04zip", 7))), (server.last().body), "multipart body matches");

		// An attachment that cannot be read fails the request before anything is sent.
		auto unreadable = upload;
		unreadable.upload_file_path = utf8(temp_path("missing.zip"));
		auto sent = server.requests().size();
		r = os::send_request(host, unreadable);
		assert_equal(true, (r.error == os::web_response_error::transport), "r.error == os::web_response_error::transport");
		assert_equal(true, (r.status_code) == (0), "r.status_code == 0");
		assert_equal(true, (server.requests().size()) == (sent), "server.requests().size() == sent");

		const auto download_path = temp_path("download.txt");
		os::web_request fetch;
		fetch.path = "/file";
		fetch.download_file_path = utf8(download_path);
		r = os::send_request(host, fetch);
		assert_equal(true, (r.status_code) == (200), "r.status_code == 200");
		assert_equal(true, (r.body.empty()), "r.body.empty()");
		assert_equal(true, (r.error == os::web_response_error::none), "r.error == os::web_response_error::none");
		assert_equal(true, (r.download_created), "r.download_created");
		assert_equal_strict(("file body"), (read_file(download_path)), "read_file(download_path) == \"file body\"");

		// A body shorter than its Content-Length is a failed transfer, not a short answer.
		os::web_request cut;
		cut.path = "/short";
		r = os::send_request(host, cut);
		assert_equal(true, (r.status_code) == (200), "r.status_code == 200");
		assert_equal(true, (r.error == os::web_response_error::transport), "r.error == os::web_response_error::transport");
		assert_equal(true, (r.body.empty()), "r.body.empty()");

		const auto cut_path = temp_path("short.txt");
		cut.download_file_path = utf8(cut_path);
		r = os::send_request(host, cut);
		assert_equal(true, (r.error == os::web_response_error::transport), "r.error == os::web_response_error::transport");
		assert_equal(true, (r.download_created), "r.download_created");
		assert_equal_strict(("0123456789"), (read_file(cut_path)), "read_file(cut_path) == \"0123456789\"");

		// A download that cannot be created reports that nothing at the path came from this request.
		os::web_request unwritable;
		unwritable.path = "/file";
		unwritable.download_file_path = utf8(temp_path("no-such-folder") / "download.txt");
		r = os::send_request(host, unwritable);
		assert_equal(true, (r.error == os::web_response_error::transport), "r.error == os::web_response_error::transport");
		assert_equal(true, (!r.download_created), "!r.download_created");

		os::web_request empty;
		empty.path = "/empty";
		r = os::send_request(host, empty);
		assert_equal(true, (r.status_code) == (204), "r.status_code == 204");
		assert_equal(true, (r.body.empty()), "r.body.empty()");
		assert_equal(true, (r.error == os::web_response_error::none), "r.error == os::web_response_error::none");

		os::web_request relative;
		relative.path = "hello";
		assert_equal(true, (os::send_request(host, relative).error == os::web_response_error::invalid_request),
			"os::send_request(host, relative).error == os::web_response_error::invalid_request");

		os::web_request doubled;
		doubled.path = "//hello";
		assert_equal(true, (os::send_request(host, doubled).error == os::web_response_error::invalid_request),
			"os::send_request(host, doubled).error == os::web_response_error::invalid_request");

		os::web_request injected;
		injected.path = "/hello";
		injected.headers = {{"X-Test", "a\r\nX-Evil: 1"}};
		assert_equal(true, (os::send_request(host, injected).error == os::web_response_error::invalid_request),
			"os::send_request(host, injected).error == os::web_response_error::invalid_request");

		os::web_request mismatched;
		mismatched.verb = os::web_request_verb::POST;
		mismatched.path = "/body";
		mismatched.body = "raw body";
		mismatched.headers = {{"Content-Length", "3"}};
		assert_equal(true, (os::send_request(host, mismatched).error == os::web_response_error::invalid_request),
			"os::send_request(host, mismatched).error == os::web_response_error::invalid_request");

		os::web_request no_cookies;
		no_cookies.path = "/hello";
		no_cookies.use_cookies = false;
		no_cookies.headers = {{"Cookie", "a=b"}};
		assert_equal(true, (os::send_request(host, no_cookies).error == os::web_response_error::invalid_request),
			"os::send_request(host, no_cookies).error == os::web_response_error::invalid_request");

		assert_equal(true, (os::send_request(nullptr, get).error == os::web_response_error::invalid_request),
			"os::send_request(nullptr, get).error == os::web_response_error::invalid_request");

		os::web_request limited;
		limited.path = "/hello";
		limited.max_response_bytes = 5;
		r = os::send_request(host, limited);
		assert_equal(true, (r.error == os::web_response_error::response_limit), "r.error == os::web_response_error::response_limit");
		assert_equal(true, (r.body.empty()), "r.body.empty()");
		assert_equal_strict(("libdesktop-os-tests/1.0"), (loopback::header_value(server.last(), "User-Agent")),
			"loopback::header_value(server.last(), \"User-Agent\") == \"libdesktop-os-tests/1.0\"");

		os::web_request roomy = limited;
		roomy.max_response_bytes = 64;
		r = os::send_request(host, roomy);
		assert_equal(true, (r.error == os::web_response_error::none), "r.error == os::web_response_error::none");
		assert_equal_strict(("hello world"), (r.body), "r.body == \"hello world\"");
		assert_equal_strict(("text/plain"), (r.content_type), "r.content_type == \"text/plain\"");

		os::web_request slow;
		slow.path = "/slow";
		slow.timeout_ms = 300;
		const auto started = std::chrono::steady_clock::now();
		r = os::send_request(host, slow);
		const auto waited = std::chrono::steady_clock::now() - started;
		assert_equal(true, (r.error == os::web_response_error::transport), "r.error == os::web_response_error::transport");
		assert_equal(true, (waited < std::chrono::milliseconds(2500)), "waited < std::chrono::milliseconds(2500)");

		// A host that keeps the system's own timeouts answers the same.
		const auto system_timeouts = os::connect_to_host("127.0.0.1", false, server.port(), "agent", false);
		assert_equal(true, (system_timeouts != nullptr), "system_timeouts != nullptr");
		if (system_timeouts)
		{
			r = os::send_request(system_timeouts, get);
			assert_equal(true, (r.status_code) == (200), "r.status_code == 200");
			assert_equal_strict(("hello world"), (r.body), "r.body == \"hello world\"");
			assert_equal_strict(("agent"), (loopback::header_value(server.last(), "User-Agent")),
				"loopback::header_value(server.last(), \"User-Agent\") == \"agent\"");
		}

		int closed_port = 0;
		{
			const loopback::server gone;
			closed_port = gone.port();
		}
		const auto nowhere = os::connect_to_host("127.0.0.1", false, closed_port, "agent");
		assert_equal(true, (nowhere != nullptr), "nowhere != nullptr");
		if (nowhere)
		{
			r = os::send_request(nowhere, get);
			assert_equal(true, (r.status_code) == (0), "r.status_code == 0");
			assert_equal(true, (r.error == os::web_response_error::transport), "r.error == os::web_response_error::transport");
		}

		std::error_code ec;
		for (const auto& p : {upload_path, download_path, cut_path}) std::filesystem::remove(p, ec);
	}

	// The mix formats an endpoint is described in: what the mixer and the media code write.
	void test_describe_wave_format()
	{
		const auto none = os::describe_wave_format(nullptr);
		assert_equal(true, (none.sample_format == os::audio_sample_format::none), "none.sample_format == os::audio_sample_format::none");
		assert_equal(true, (none.channels) == (0u), "none.channels == 0u");

		WAVEFORMATEX pcm{};
		pcm.wFormatTag = WAVE_FORMAT_PCM;
		pcm.nChannels = 2;
		pcm.nSamplesPerSec = 44100;
		pcm.wBitsPerSample = 16;
		pcm.nBlockAlign = 4;
		auto described = os::describe_wave_format(&pcm);
		assert_equal(true, (described.sample_format == os::audio_sample_format::int16),
			"described.sample_format == os::audio_sample_format::int16");
		assert_equal(true, (described.sample_rate) == (44100u), "described.sample_rate == 44100u");
		assert_equal(true, (described.channels) == (2u), "described.channels == 2u");
		assert_equal(true, (described.channel_mask) == (0u), "described.channel_mask == 0u");

		WAVEFORMATEXTENSIBLE ext{};
		ext.Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
		ext.Format.nChannels = 6;
		ext.Format.nSamplesPerSec = 48000;
		ext.Format.wBitsPerSample = 32;
		ext.Format.nBlockAlign = 24;
		ext.Format.cbSize = sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX);
		ext.dwChannelMask = 0x3F;
		ext.SubFormat = {0x00000003, 0x0000, 0x0010, {0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71}};
		described = os::describe_wave_format(&ext);
		assert_equal(true, (described.sample_format == os::audio_sample_format::float32),
			"described.sample_format == os::audio_sample_format::float32");
		assert_equal(true, (described.channels) == (6u), "described.channels == 6u");
		assert_equal(true, (described.channel_mask) == (0x3Fu), "described.channel_mask == 0x3Fu");

		ext.Format.wBitsPerSample = 24;
		assert_equal(true, (os::describe_wave_format(&ext).sample_format == os::audio_sample_format::none),
			"os::describe_wave_format(&ext).sample_format == os::audio_sample_format::none");
	}

	// The endpoint the system plays through, silent throughout. A machine with no endpoint - a build
	// machine - has nothing to open, and the rest is skipped.
	void test_audio_endpoint()
	{
		const os::audio_thread thread;

		const auto devices = os::list_audio_playback_devices();
		const auto output = os::create_audio_output({});
		assert_equal(true, (output == nullptr) == (devices.empty()), "output == nullptr == devices.empty()");

		if (!output)
		{
			std::printf("audio: no endpoint, skipping\n");
			return;
		}

		for (const auto& device : devices)
		{
			assert_equal(true, (!device.id.empty()), "!device.id.empty()");
			assert_equal(true, (!device.name.empty()), "!device.name.empty()");
		}

		output->volume(0.0);
		const auto format = output->format();
		assert_equal(true, (format.sample_rate > 0), "format.sample_rate > 0");
		assert_equal(true, (format.channels > 0), "format.channels > 0");
		assert_equal(true, (format.sample_format != os::audio_sample_format::none),
			"format.sample_format != os::audio_sample_format::none");
		assert_equal(true, (!output->id().empty()), "!output->id().empty()");
		assert_equal(true, (output->is_stopped()), "output->is_stopped()");
		assert_equal(true, (!output->is_device_lost()), "!output->is_device_lost()");

		// An empty ring takes a ring's worth of whole frames, and then has no room.
		const auto room = output->writable_frames();
		assert_equal(true, (room > 0), "room > 0");
		const auto frame_bytes = format.channels * (format.sample_format == os::audio_sample_format::int16 ? 2u : 4u);
		const std::vector<uint8_t> silence(static_cast<size_t>(room + 10) * frame_bytes + 1, 0);
		assert_equal(true, (output->write(silence)) == (static_cast<size_t>(room) * frame_bytes),
			"output->write(silence) == static_cast<size_t>(room) * frame_bytes");
		assert_equal(true, (output->writable_frames()) == (0u), "output->writable_frames() == 0u");

		// Started, it plays: its clock moves and the ring drains.
		output->start();
		assert_equal(true, (!output->is_stopped()), "!output->is_stopped()");
		output->wait_for_buffer(nullptr, 200);
		Sleep(100);
		assert_equal(true, (output->time() > 0.0), "output->time() > 0.0");
		assert_equal(true, (output->writable_frames() > 0), "output->writable_frames() > 0");
		output->write_silence();
		output->stop();
		assert_equal(true, (output->is_stopped()), "output->is_stopped()");

		// Reset empties the ring and restarts the clock.
		output->reset();
		assert_equal(true, (output->writable_frames()) == (room), "output->writable_frames() == room");
		assert_equal(true, (output->time()) == (0.0), "output->time() == 0.0");

		// A signalled wake event ends the wait long before its timeout.
		const auto wake = CreateEventW(nullptr, TRUE, TRUE, nullptr);
		const auto t0 = std::chrono::steady_clock::now();
		output->wait_for_buffer(wake, 5000);
		assert_equal(true, (std::chrono::steady_clock::now() - t0 < std::chrono::seconds(1)),
			"std::chrono::steady_clock::now() - t0 < std::chrono::seconds(1)");
		CloseHandle(wake);

		// An endpoint that is not there opens the default instead.
		assert_equal(true, (os::create_audio_output("{no-such-endpoint}") != nullptr),
			"os::create_audio_output(\"{no-such-endpoint}\") != nullptr");

		// A ring of its own length holds at least that much.
		const auto long_ring = os::create_audio_output({}, 0.5);
		assert_equal(true, (long_ring != nullptr), "long_ring != nullptr");
		if (long_ring)
		{
			assert_equal(true, (long_ring->writable_frames() >= long_ring->format().sample_rate * 4 / 10),
				"long_ring->writable_frames() >= long_ring->format().sample_rate * 4 / 10");
		}
	}
}

void register_os_tests(test_registry& tests)
{
	tests.add("test_url_encode", test_url_encode);
	tests.add("test_known_folders", test_known_folders);
	tests.add("test_files", test_files);
	tests.add("test_enumerate_files", test_enumerate_files);
	tests.add("test_identity_index", test_identity_index);
	tests.add("test_clipboard_text", test_clipboard_text);
	tests.add("test_web_client", test_web_client);
	tests.add("test_describe_wave_format", test_describe_wave_format);
	tests.add("test_audio_endpoint", test_audio_endpoint);
}
