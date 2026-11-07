// os_tests.cpp - libdesktop-os, checked without a window: the web client against a scripted loopback
// server, clipboard text, the known folders, and the audio endpoint. The pf:: and platform:: suites keep
// their own tests of these services through their own APIs; these test the one implementation underneath.

#include "libdesktop/os.h"
#include "check.h"
#include "loopback_http.h"

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
		CHECK_STR(os::url_encode("AZaz09-_.~"), "AZaz09-_.~");
		CHECK_STR(os::url_encode("x y/z"), "x%20y%2Fz");
		CHECK_STR(os::url_encode("\xC3\xA9"), "%C3%A9");
	}

	void test_known_folders()
	{
		uint32_t status = 1;
		const auto local = os::known_folder_path(os::known_folder::local_app_data, false, &status);
		CHECK(!local.empty());
		CHECK_EQ(status, 0u);
		CHECK(std::filesystem::is_directory(std::filesystem::path(std::u8string(local.begin(), local.end()))));

		for (const auto folder : {os::known_folder::profile, os::known_folder::documents, os::known_folder::pictures,
		                          os::known_folder::public_documents})
		{
			CHECK(!os::known_folder_path(folder).empty());
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
		CHECK(attributes.exists());
		CHECK(!attributes.directory);
		CHECK(attributes.identity.valid);
		CHECK_EQ(attributes.size, 10u);

		const auto listing = os::enumerate_files(utf8(root), false);
		CHECK(listing.success);
		CHECK_EQ(listing.entries.size(), 1u);
		if (!listing.entries.empty()) CHECK(listing.entries.front().name == "source.bin");

		const auto file = os::open_file(utf8(source), os::file_open_mode::read);
		CHECK(file != nullptr);
		if (file)
		{
			std::array<uint8_t, 4> bytes = {};
			CHECK_EQ(file->read_at(3, bytes.data(), bytes.size()), 4u);
			CHECK(std::string(bytes.begin(), bytes.end()) == "3456");
			CHECK_EQ(file->position(), 0u);
		}

		CHECK(os::copy_file(utf8(source), utf8(copied), true).success());
		const auto collision = os::copy_file(utf8(source), utf8(copied), true);
		CHECK(collision.code == os::file_operation_code::already_exists);
		CHECK(!collision.diagnostic.empty());
		CHECK(os::move_file(utf8(copied), utf8(renamed), true).success());
		const auto moved_collision = os::move_file(utf8(source), utf8(renamed), true);
		CHECK(moved_collision.code == os::file_operation_code::already_exists);
		CHECK(!moved_collision.diagnostic.empty());
		CHECK(os::delete_file(utf8(source)).success());
		CHECK(os::delete_file(utf8(renamed)).success());
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
		CHECK(listing.success);
		CHECK_EQ(listing.entries.size(), 2u);

		for (const auto& entry : listing.entries)
		{
			const auto alone = os::query_file(utf8(root) + "\\" + entry.name);
			CHECK(entry.attributes.exists());
			CHECK(entry.attributes.identity.valid);
			CHECK(entry.attributes.identity == alone.identity);
			CHECK_EQ(entry.attributes.directory, alone.directory);
			CHECK_EQ(entry.attributes.size, alone.size);
			CHECK_EQ(entry.attributes.modified, alone.modified);
			CHECK_EQ(entry.can_recurse, entry.name == "child");
		}

		CHECK_EQ(os::enumerate_files(utf8(root), true).entries.size(), 3u);

		const auto missing = os::enumerate_files(utf8(root / "missing"), false);
		CHECK(!missing.success);
		CHECK(missing.error == ERROR_FILE_NOT_FOUND || missing.error == ERROR_PATH_NOT_FOUND);

		SetFileAttributesW((root / "hidden.txt").c_str(), FILE_ATTRIBUTE_NORMAL);
		std::filesystem::remove_all(root);
	}

	// What a file system's ID for a file gives an identity. FAT's root, ReFS's 128-bit IDs and a server
	// reporting no ID cannot be made here, so the rule is checked as a table; a volume root and a folder
	// on this machine's disk are checked as they are.
	void test_identity_index()
	{
		CHECK(os::identity_index(5, 0, false) == std::optional<uint64_t>{5});
		CHECK(!os::identity_index(0, 0, false).has_value());
		CHECK(os::identity_index(0, 0, true) == std::optional<uint64_t>{0});
		CHECK(!os::identity_index(~uint64_t{0}, 0, true).has_value());
		CHECK(!os::identity_index(5, 1, false).has_value());
		CHECK(!os::identity_index(~uint64_t{0}, ~uint64_t{0}, false).has_value());

		const auto temp = std::filesystem::temp_directory_path();
		CHECK(os::query_file(utf8(temp.root_path())).identity.valid);
		CHECK(os::query_file(utf8(temp)).identity.valid);
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
		CHECK(before.opened);
		if (!before.opened) return;

		const std::string text = "line one\r\nline two\t\xC3\xA9\xE2\x82\xAC";
		CHECK(write(text) == os::clipboard_write::ok);
		CHECK(os::clipboard_has_text());

		const auto after = read();
		CHECK(after.opened);
		CHECK(after.text.has_value());
		CHECK_STR(after.text.value_or(std::string()), text);

		CHECK(write("") == os::clipboard_write::ok);
		CHECK_STR(read().text.value_or("unset"), "");

		if (before.text) CHECK(write(*before.text) == os::clipboard_write::ok);
	}

	void test_web_client()
	{
		loopback::server server;
		CHECK(server.listening());
		if (!server.listening()) return;

		constexpr std::string_view boundary = "54B8723DE6044695A68C838E8BF0CB00";
		server.respond("/hello", {loopback::http_response(200, "OK", "hello world", "Content-Type: text/plain\r\n")});
		server.respond("/file", {loopback::http_response(200, "OK", "file body")});
		server.respond("/short", {"HTTP/1.1 200 OK\r\nContent-Length: 100\r\nConnection: close\r\n\r\n0123456789"});
		server.respond("/empty", {"HTTP/1.1 204 No Content\r\nConnection: close\r\n\r\n"});
		server.respond("/slow", {loopback::http_response(200, "OK", "late"), 3000});

		CHECK(os::connect_to_host("", false, server.port(), "agent") == nullptr);
		CHECK(os::connect_to_host("127.0.0.1/x", false, server.port(), "agent") == nullptr);
		CHECK(os::connect_to_host("127.0.0.1", false, 70000, "agent") == nullptr);
		CHECK(os::connect_to_host("127.0.0.1", false, server.port(), "bad\r\nagent") == nullptr);

		const auto host = os::connect_to_host("127.0.0.1", false, server.port(), "libdesktop-os-tests/1.0");
		CHECK(host != nullptr);
		if (!host) return;

		os::web_request get;
		get.path = "/hello";
		auto r = os::send_request(host, get);
		CHECK_EQ(r.status_code, 200);
		CHECK_STR(r.body, "hello world");
		CHECK_STR(r.content_type, "text/plain");
		CHECK(r.error == os::web_response_error::none);
		CHECK(r.headers.starts_with("HTTP/1.1 200 OK\r\n"));
		CHECK(!r.download_created);
		CHECK_STR(server.last().request_line, "GET /hello HTTP/1.1");
		CHECK_STR(loopback::header_value(server.last(), "User-Agent"), "libdesktop-os-tests/1.0");

		os::web_request query;
		query.path = "/query?x=1";
		query.query = {{"a", "1"}, {"b", "x y"}};
		query.headers = {{"X-Test", "1"}};
		r = os::send_request(host, query);
		CHECK_EQ(r.status_code, 404);
		CHECK_STR(r.body, "missing");
		CHECK_STR(server.last().request_line, "GET /query?x=1&a=1&b=x%20y HTTP/1.1");
		CHECK_STR(loopback::header_value(server.last(), "X-Test"), "1");

		os::web_request posted;
		posted.verb = os::web_request_verb::POST;
		posted.path = "/body";
		posted.body = "raw body";
		posted.headers = {{"Content-Type", "text/plain"}, {"Content-Length", "8"}};
		r = os::send_request(host, posted);
		CHECK_STR(server.last().request_line, "POST /body HTTP/1.1");
		CHECK_STR(server.last().body, "raw body");

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
		CHECK_EQ(r.status_code, 404);
		CHECK_STR(loopback::header_value(server.last(), "Content-Type"),
		          std::format("multipart/form-data; boundary={}", boundary));
		CHECK_STR(server.last().body, std::format(
			"--{0}\r\nContent-Disposition: form-data; name=\"k\"\r\nContent-Type: text/plain; charset=\"utf-8\"\r\n"
			"\r\nv\r\n--{0}\r\nContent-Disposition: form-data; name=\"ff\"; filename=\"logs.zip\"\r\n"
			"Content-Type: application/x-zip-compressed\r\n\r\n{1}\r\n--{0}--", boundary,
			std::string_view("PK\x03\x04zip", 7)));

		// An attachment that cannot be read fails the request before anything is sent.
		auto unreadable = upload;
		unreadable.upload_file_path = utf8(temp_path("missing.zip"));
		auto sent = server.requests().size();
		r = os::send_request(host, unreadable);
		CHECK(r.error == os::web_response_error::transport);
		CHECK_EQ(r.status_code, 0);
		CHECK_EQ(server.requests().size(), sent);

		const auto download_path = temp_path("download.txt");
		os::web_request fetch;
		fetch.path = "/file";
		fetch.download_file_path = utf8(download_path);
		r = os::send_request(host, fetch);
		CHECK_EQ(r.status_code, 200);
		CHECK(r.body.empty());
		CHECK(r.error == os::web_response_error::none);
		CHECK(r.download_created);
		CHECK_STR(read_file(download_path), "file body");

		// A body shorter than its Content-Length is a failed transfer, not a short answer.
		os::web_request cut;
		cut.path = "/short";
		r = os::send_request(host, cut);
		CHECK_EQ(r.status_code, 200);
		CHECK(r.error == os::web_response_error::transport);
		CHECK(r.body.empty());

		const auto cut_path = temp_path("short.txt");
		cut.download_file_path = utf8(cut_path);
		r = os::send_request(host, cut);
		CHECK(r.error == os::web_response_error::transport);
		CHECK(r.download_created);
		CHECK_STR(read_file(cut_path), "0123456789");

		// A download that cannot be created reports that nothing at the path came from this request.
		os::web_request unwritable;
		unwritable.path = "/file";
		unwritable.download_file_path = utf8(temp_path("no-such-folder") / "download.txt");
		r = os::send_request(host, unwritable);
		CHECK(r.error == os::web_response_error::transport);
		CHECK(!r.download_created);

		os::web_request empty;
		empty.path = "/empty";
		r = os::send_request(host, empty);
		CHECK_EQ(r.status_code, 204);
		CHECK(r.body.empty());
		CHECK(r.error == os::web_response_error::none);

		os::web_request relative;
		relative.path = "hello";
		CHECK(os::send_request(host, relative).error == os::web_response_error::invalid_request);

		os::web_request doubled;
		doubled.path = "//hello";
		CHECK(os::send_request(host, doubled).error == os::web_response_error::invalid_request);

		os::web_request injected;
		injected.path = "/hello";
		injected.headers = {{"X-Test", "a\r\nX-Evil: 1"}};
		CHECK(os::send_request(host, injected).error == os::web_response_error::invalid_request);

		os::web_request mismatched;
		mismatched.verb = os::web_request_verb::POST;
		mismatched.path = "/body";
		mismatched.body = "raw body";
		mismatched.headers = {{"Content-Length", "3"}};
		CHECK(os::send_request(host, mismatched).error == os::web_response_error::invalid_request);

		os::web_request no_cookies;
		no_cookies.path = "/hello";
		no_cookies.use_cookies = false;
		no_cookies.headers = {{"Cookie", "a=b"}};
		CHECK(os::send_request(host, no_cookies).error == os::web_response_error::invalid_request);

		CHECK(os::send_request(nullptr, get).error == os::web_response_error::invalid_request);

		os::web_request limited;
		limited.path = "/hello";
		limited.max_response_bytes = 5;
		r = os::send_request(host, limited);
		CHECK(r.error == os::web_response_error::response_limit);
		CHECK(r.body.empty());
		CHECK_STR(loopback::header_value(server.last(), "User-Agent"), "libdesktop-os-tests/1.0");

		os::web_request roomy = limited;
		roomy.max_response_bytes = 64;
		r = os::send_request(host, roomy);
		CHECK(r.error == os::web_response_error::none);
		CHECK_STR(r.body, "hello world");
		CHECK_STR(r.content_type, "text/plain");

		os::web_request slow;
		slow.path = "/slow";
		slow.timeout_ms = 300;
		const auto started = std::chrono::steady_clock::now();
		r = os::send_request(host, slow);
		const auto waited = std::chrono::steady_clock::now() - started;
		CHECK(r.error == os::web_response_error::transport);
		CHECK(waited < std::chrono::milliseconds(2500));

		// A host that keeps the system's own timeouts answers the same.
		const auto system_timeouts = os::connect_to_host("127.0.0.1", false, server.port(), "agent", false);
		CHECK(system_timeouts != nullptr);
		if (system_timeouts)
		{
			r = os::send_request(system_timeouts, get);
			CHECK_EQ(r.status_code, 200);
			CHECK_STR(r.body, "hello world");
			CHECK_STR(loopback::header_value(server.last(), "User-Agent"), "agent");
		}

		int closed_port = 0;
		{
			const loopback::server gone;
			closed_port = gone.port();
		}
		const auto nowhere = os::connect_to_host("127.0.0.1", false, closed_port, "agent");
		CHECK(nowhere != nullptr);
		if (nowhere)
		{
			r = os::send_request(nowhere, get);
			CHECK_EQ(r.status_code, 0);
			CHECK(r.error == os::web_response_error::transport);
		}

		std::error_code ec;
		for (const auto& p : {upload_path, download_path, cut_path}) std::filesystem::remove(p, ec);
	}

	// The mix formats an endpoint is described in: what the mixer and the media code write.
	void test_describe_wave_format()
	{
		const auto none = os::describe_wave_format(nullptr);
		CHECK(none.sample_format == os::audio_sample_format::none);
		CHECK_EQ(none.channels, 0u);

		WAVEFORMATEX pcm{};
		pcm.wFormatTag = WAVE_FORMAT_PCM;
		pcm.nChannels = 2;
		pcm.nSamplesPerSec = 44100;
		pcm.wBitsPerSample = 16;
		pcm.nBlockAlign = 4;
		auto described = os::describe_wave_format(&pcm);
		CHECK(described.sample_format == os::audio_sample_format::int16);
		CHECK_EQ(described.sample_rate, 44100u);
		CHECK_EQ(described.channels, 2u);
		CHECK_EQ(described.channel_mask, 0u);

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
		CHECK(described.sample_format == os::audio_sample_format::float32);
		CHECK_EQ(described.channels, 6u);
		CHECK_EQ(described.channel_mask, 0x3Fu);

		ext.Format.wBitsPerSample = 24;
		CHECK(os::describe_wave_format(&ext).sample_format == os::audio_sample_format::none);
	}

	// The endpoint the system plays through, silent throughout. A machine with no endpoint - a build
	// machine - has nothing to open, and the rest is skipped.
	void test_audio_endpoint()
	{
		const os::audio_thread thread;

		const auto devices = os::list_audio_playback_devices();
		const auto output = os::create_audio_output({});
		CHECK_EQ(output == nullptr, devices.empty());

		if (!output)
		{
			std::printf("audio: no endpoint, skipping\n");
			return;
		}

		for (const auto& device : devices)
		{
			CHECK(!device.id.empty());
			CHECK(!device.name.empty());
		}

		output->volume(0.0);
		const auto format = output->format();
		CHECK(format.sample_rate > 0);
		CHECK(format.channels > 0);
		CHECK(format.sample_format != os::audio_sample_format::none);
		CHECK(!output->id().empty());
		CHECK(output->is_stopped());
		CHECK(!output->is_device_lost());

		// An empty ring takes a ring's worth of whole frames, and then has no room.
		const auto room = output->writable_frames();
		CHECK(room > 0);
		const auto frame_bytes = format.channels * (format.sample_format == os::audio_sample_format::int16 ? 2u : 4u);
		const std::vector<uint8_t> silence(static_cast<size_t>(room + 10) * frame_bytes + 1, 0);
		CHECK_EQ(output->write(silence), static_cast<size_t>(room) * frame_bytes);
		CHECK_EQ(output->writable_frames(), 0u);

		// Started, it plays: its clock moves and the ring drains.
		output->start();
		CHECK(!output->is_stopped());
		output->wait_for_buffer(nullptr, 200);
		Sleep(100);
		CHECK(output->time() > 0.0);
		CHECK(output->writable_frames() > 0);
		output->write_silence();
		output->stop();
		CHECK(output->is_stopped());

		// Reset empties the ring and restarts the clock.
		output->reset();
		CHECK_EQ(output->writable_frames(), room);
		CHECK_EQ(output->time(), 0.0);

		// A signalled wake event ends the wait long before its timeout.
		const auto wake = CreateEventW(nullptr, TRUE, TRUE, nullptr);
		const auto t0 = std::chrono::steady_clock::now();
		output->wait_for_buffer(wake, 5000);
		CHECK(std::chrono::steady_clock::now() - t0 < std::chrono::seconds(1));
		CloseHandle(wake);

		// An endpoint that is not there opens the default instead.
		CHECK(os::create_audio_output("{no-such-endpoint}") != nullptr);

		// A ring of its own length holds at least that much.
		const auto long_ring = os::create_audio_output({}, 0.5);
		CHECK(long_ring != nullptr);
		if (long_ring)
		{
			CHECK(long_ring->writable_frames() >= long_ring->format().sample_rate * 4 / 10);
		}
	}
}

int main()
{
	test_url_encode();
	test_known_folders();
	test_files();
	test_enumerate_files();
	test_identity_index();
	test_clipboard_text();
	test_web_client();
	test_describe_wave_format();
	test_audio_endpoint();

	std::printf("libdesktop-os tests: %s (%d checks, %d failures)\n", g_failures == 0 ? "PASS" : "FAIL", g_checks,
	            g_failures);
	return g_failures == 0 ? 0 : 1;
}
