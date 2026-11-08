// This file is part of libdesktop, a C++20 platform layer for desktop applications
// Copyright 2026  Zac Walker
//
// This program is free software; you can redistribute it and / or modify it
// under the terms of the LGPL License either version 2.1 or later.
// License details are available at https://www.gnu.org/licenses/lgpl-2.1.html
// This program is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY

// Purpose: Platform text, path, atomic-file and file-handle service tests.

#include "libdesktop/platform.h"
#include "test.h"
#include "platform/test_support.h"
#ifdef _WIN32
#include "loopback_http.h"
#endif

#include <array>
#include <iterator>

using namespace std::literals;

namespace
{
	void should_preserve_core_text_contract()
	{
		constexpr std::string_view mixed = "a\xC3\xA9\xE2\x82\xAC\xF0\x90\x8D\x88";
		std::vector<uint32_t> codepoints;
		for (auto it = mixed.begin(); it != mixed.end();)
			codepoints.push_back(core::pop_utf8_char(it, mixed.end()));
		assert_equal(4u, static_cast<uint32_t>(codepoints.size()), "UTF-8 code point count");
		assert_equal(static_cast<uint32_t>('a'), codepoints[0], "ASCII code point");
		assert_equal(0x10348u, codepoints[3], "astral code point");

		const auto wide = core::utf8_to_utf16(mixed);
		assert_equal(5u, static_cast<uint32_t>(wide.size()), "UTF-16 surrogate pair size");
		assert_equal(true, core::is_lead_surrogate(static_cast<uint32_t>(wide[3])), "lead surrogate");
		assert_equal(true, core::is_trail_surrogate(static_cast<uint32_t>(wide[4])), "trail surrogate");
		assert_equal_strict(mixed, core::utf16_to_utf8(wide), "UTF-16 round trip");
		assert_equal_strict("mixed case", core::to_lower("MiXeD Case"), "lower case");
		assert_equal_strict("MIXED CASE", core::to_upper("MiXeD Case"), "upper case");
		assert_equal(0, core::icmp("abc", "ABC"), "case-insensitive comparison");
		assert_equal(true, core::icmp("abc", "abd") < 0, "comparison order");

		const std::u16string lone_lead{static_cast<char16_t>(0xD800), u'x'};
		const std::u16string lone_trail{static_cast<char16_t>(0xDC00)};
		assert_equal_strict("\xEF\xBF\xBDx", core::utf16_to_utf8(lone_lead), "lone lead replacement");
		assert_equal_strict("\xEF\xBF\xBD", core::utf16_to_utf8(lone_trail), "lone trail replacement");
		const auto invalid = core::utf8_to_utf16("\xED\xA0\x80");
		assert_equal(1u, static_cast<uint32_t>(invalid.size()), "invalid UTF-8 scalar size");
		assert_equal(0xFFFDu, static_cast<uint32_t>(invalid[0]), "invalid UTF-8 scalar replacement");
		// U+1F600 written by a Java tool as two encoded surrogates (CESU-8) keeps its meaning.
		const auto cesu = core::utf8_to_utf16("\xED\xA0\xBD\xED\xB8\x80");
		assert_equal(2u, static_cast<uint32_t>(cesu.size()), "CESU-8 pair size");
		assert_equal(0xD83Du, static_cast<uint32_t>(cesu[0]), "CESU-8 lead surrogate");
		assert_equal(0xDE00u, static_cast<uint32_t>(cesu[1]), "CESU-8 trail surrogate");
	}

	void should_preserve_core_geometry_and_paths()
	{
		const core::recti r{10, 20, 110, 220};
		assert_equal(100, r.width(), "rectangle width");
		assert_equal(200, r.height(), "rectangle height");
		assert_equal(15, r.offset({5, -5}).left, "rectangle offset");
		assert_equal(110, r.inflate(5).width(), "rectangle inflation");
		assert_equal(true, r.contains({10, 20}), "rectangle contains edge");
		assert_equal(false, r.contains({9, 20}), "rectangle rejects outside point");
		assert_equal(true, r.intersects({100, 210, 200, 300}), "rectangle intersection");
		assert_equal(false, r.intersects({110, 220, 200, 300}), "rectangle touching edge");
		assert_equal(0x030201u, core::rgb(1, 2, 3), "RGB packing");
		assert_equal(core::rgba(255, 0, 128, 255), core::saturate_rgba(300, -1, 128, 255),
		             "saturating colour channels");

#ifdef _WIN32
		const core::file_path normalized("c:/dir/sub/");
		assert_equal_strict("c:\\dir\\sub", normalized.str(), "path separators");
		const core::file_path path("c:\\dir\\file.txt");
		assert_equal_strict("file.txt", path.name().sv(), "file name");
		assert_equal_strict(".txt", path.extension(), "file extension");
		assert_equal_strict("c:\\dir", path.folder().text().sv(), "file folder");
		assert_equal_strict("c:\\dir\\file", path.extension("").str(), "path without its extension");
		assert_equal_strict("", core::file_path("c:\\dir.v2\\file").extension(), "a folder's dot is no extension");
		assert_equal_strict("c:\\dir\\app.ini", path.folder().combine_file_ext("app", "ini").str(),
		                     "file extension combination");
		assert_equal(true, core::file_path("c:\\A\\B.TXT") == core::file_path("c:\\a\\b.txt"),
		             "case-insensitive path comparison");
#endif
		assert_equal(true, core::file_path{}.is_empty(), "empty file path");
	}

	void should_encode_platform_text()
	{
		assert_equal_strict("a%20b%26c%3Dd", platform::url_encode("a b&c=d"), "URL encoding");
		assert_equal_strict("https://example.com/a/c.css",
		                   platform::resolve_url("https://example.com/a/b.html", "c.css"), "relative URL");
		assert_equal_strict("https://example.com/root.css",
		                   platform::resolve_url("https://example.com/a/b.html", "/root.css"), "root URL");
		assert_equal_strict("https://other.com/x.css",
		                   platform::resolve_url("https://example.com/a/b.html", "https://other.com/x.css"),
		                   "absolute URL");

		assert_equal(65001u, platform::charset_to_codepage("UTF-8"), "UTF-8 charset");
		const std::string latin1{"caf\xE9", 4};
		assert_equal_strict("caf\xC3\xA9", platform::transcode_to_utf8(latin1, 28591), "Latin-1 conversion");
	}

	void should_canonicalize_and_contain_paths()
	{
		const auto root = platform::current_directory().combine("libdesktop-c3a-path-test");
		const auto child = root.combine("one").combine("two");
		const auto escaped = root.combine("..").combine("outside");
		const auto differently_cased = core::folder_path(root.text().sv());

		assert_equal(true, !platform::current_directory().is_empty(), "current directory");
		assert_equal(true, platform::is_directory(platform::current_directory()),
		             "current directory is a directory");
		assert_equal(true, platform::is_path_within(root, root), "root contains itself");
		assert_equal(true, platform::is_path_within(root, child), "root contains a child");
		assert_equal(true, platform::is_path_within(root, root.combine("one").combine("..").combine("two")),
		             "canonical child stays contained");
		assert_equal(true, platform::is_path_within(root, differently_cased), "same root spelling is contained");
		assert_equal(false, platform::is_path_within(root, escaped), "parent traversal escapes root");
		assert_equal(false, platform::is_path_within(root, root.parent()), "parent root is not contained");
		assert_equal(false, platform::is_path_within(root, core::folder_path{}), "empty path is not contained");
		assert_equal(true, platform::canonical_path(root) == platform::canonical_path(root), "canonical path is stable");
	}

	void should_expose_app_data_paths()
	{
		const auto app_data = platform::known_path(platform::known_folder::app_data);
		const auto roaming_app_data = platform::known_path(platform::known_folder::roaming_app_data);
		assert_equal(true, !app_data.is_empty(), "app-data path");
		assert_equal(true, !roaming_app_data.is_empty(), "roaming app-data path");
	}

	void should_write_and_read_atomically()
	{
		// temp_file makes its folder where it is missing; on Linux, temp_folder names one that a clean machine
		// may not have.
		const auto destination = platform::temp_file(".atomic");
		platform::delete_file(destination);

		auto opened = platform::create_atomic_file_writer_result(destination);
		assert_equal(true, opened.success(), "atomic writer opens");
		if (opened.writer)
		{
			const std::array<uint8_t, 5> bytes = {'h', 'e', 'l', 'l', 'o'};
			assert_equal(true, opened.writer->write(bytes).success(), "atomic writer writes");
			assert_equal(true, opened.writer->flush().success(), "atomic writer flushes");
			assert_equal(true, opened.writer->commit().success(), "atomic writer commits");
		}

		const auto read = platform::open_file(destination, platform::file_open_mode::read);
		assert_equal(true, read != nullptr, "atomic file reopens");
		if (read)
		{
			std::array<uint8_t, 5> bytes = {};
			const auto count = read->read(bytes.data(), bytes.size());
			assert_equal(uint64_t{5}, count, "file byte count");
			assert_equal_strict("hello", std::string(bytes.begin(), bytes.end()), "file contents");
		}

		platform::test_set_atomic_file_failure(platform::atomic_file_test_failure::flush);
		auto failed = platform::create_atomic_file_writer_result(destination);
		assert_equal(true, failed.success(), "flush failure is deferred to flush");
		if (failed.writer)
			assert_equal(false, failed.writer->flush().success(), "forced flush failure");
		platform::test_set_atomic_file_failure(platform::atomic_file_test_failure::none);

		const auto missing = platform::file_attributes(core::file_path(destination.str() + ".missing"));
		assert_equal(true, missing.confirmed_missing(), "missing file query");
		assert_equal(true, platform::file_attributes(destination).modified != 0, "modified timestamp");
		platform::delete_file(destination);
	}
}
#ifdef _WIN32
	void should_round_trip_platform_web_requests()
	{
		loopback::server server;
		assert_equal(true, server.listening(), "loopback server");
		if (!server.listening()) return;
		server.respond("/hello", {loopback::http_response(200, "OK", "hello world",
		                                                   "Content-Type: text/plain\r\n")});
		server.respond("/query", {loopback::http_response(200, "OK", "query ok")});
		server.respond("/form", {loopback::http_response(200, "OK", "form ok")});
		server.respond("/file", {loopback::http_response(200, "OK", "file body")});

		const auto host = platform::connect_to_host("127.0.0.1", false, server.port());
		assert_equal(true, host != nullptr, "web host");
		if (!host) return;

		platform::web_request get;
		get.path = "/hello";
		const auto response = platform::send_request(host, get);
		assert_equal(200, response.status_code, "GET status");
		assert_equal_strict("hello world", response.body, "GET body");
		assert_equal_strict("text/plain", response.content_type, "GET content type");
		assert_equal_strict("GET /hello HTTP/1.1", server.last().request_line, "GET request line");

		platform::web_request query;
		query.path = "/query";
		query.query = {{"a", "1"}, {"b", "x y"}};
		query.headers = {{"X-Test", "1"}};
		const auto queried = platform::send_request(host, query);
		assert_equal(200, queried.status_code, "query status");
		assert_equal_strict("GET /query?a=1&b=x%20y HTTP/1.1", server.last().request_line, "query encoding");
		assert_equal_strict("1", loopback::header_value(server.last(), "X-Test"), "request header");

		platform::web_request form;
		form.verb = platform::web_request_verb::POST;
		form.path = "/form";
		form.form_data = {{"name", "value"}};
		const auto posted = platform::send_request(host, form);
		assert_equal(200, posted.status_code, "form status");
		assert_equal(true, server.last().request_line.starts_with("POST /form "), "form request line");
		assert_equal(true, server.last().body.find("value") != std::string::npos, "form body");

		const auto download = _temps.folder().combine_file("platform-service-download.txt");
		platform::delete_file(download);
		platform::web_request fetch;
		fetch.path = "/file";
		fetch.download_file_path = download;
		const auto downloaded = platform::send_request(host, fetch);
		assert_equal(200, downloaded.status_code, "download status");
		assert_equal(true, platform::file_attributes(download).exists(), "download exists");
		if (const auto file = platform::open_file(download, platform::file_open_mode::read))
		{
			std::array<uint8_t, 9> bytes{};
			assert_equal(uint64_t{9}, file->read(bytes.data(), bytes.size()), "download byte count");
			assert_equal_strict("file body", std::string(reinterpret_cast<const char*>(bytes.data()), bytes.size()), "download body");
		}
		platform::delete_file(download);
	}
#endif


void register_platform_services_tests(test_registry& tests)
{
	tests.add("Should preserve core text contract"s, should_preserve_core_text_contract);
	tests.add("Should preserve core geometry and paths"s, should_preserve_core_geometry_and_paths);
	tests.add("Should encode platform text"s, should_encode_platform_text);
	tests.add("Should canonicalize contained paths"s, should_canonicalize_and_contain_paths);
	tests.add("Should expose application data paths"s, should_expose_app_data_paths);
	tests.add("Should write and read atomic files"s, should_write_and_read_atomically);
#ifdef _WIN32
	tests.add("Should round-trip platform web requests"s, should_round_trip_platform_web_requests);
#endif
}
