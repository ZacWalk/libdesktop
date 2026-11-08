// This file is part of libdesktop, a C++20 platform layer for desktop applications
// Copyright 2026  Zac Walker
//
// This program is free software; you can redistribute it and / or modify it
// under the terms of the LGPL License either version 2.1 or later.
// License details are available at https://www.gnu.org/licenses/lgpl-2.1.html
// This program is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY

// Purpose: Linux text, asynchronous web and filesystem services.

#include "libdesktop/platform.h"

#include <cerrno>
#include <cctype>
#include <cstring>
#include <filesystem>
#include <format>
#include <iconv.h>
#include <string>
#include <string_view>
#include <vector>

#include <unistd.h>

namespace
{
	std::atomic<platform::atomic_file_test_failure> atomic_failure{platform::atomic_file_test_failure::none};

	platform::file_op_result failure(const int error, std::string message = {})
	{
		if (message.empty()) message = std::strerror(error);
		platform::file_op_result result{platform::file_op_result_code::FAILED, std::move(message)};
		if (error == EEXIST) result.code = platform::file_op_result_code::ALREADY_EXISTS;
		result.error = {error < 0 ? 0 : error, std::generic_category()};
		return result;
	}

	platform::file_op_result operation_failure(const platform::file_op_result& result)
	{
		return result;
	}

	std::string canonical_text(const std::string_view input)
	{
		if (input.empty()) return {};
		std::error_code error;
		auto result = std::filesystem::weakly_canonical(std::filesystem::path(std::string(input)), error);
		if (error)
		{
			error.clear();
			result = std::filesystem::absolute(std::filesystem::path(std::string(input)), error);
			if (error) return std::filesystem::path(std::string(input)).lexically_normal().generic_string();
			result = result.lexically_normal();
		}
		return result.generic_string();
	}

	bool path_within_text(const std::string_view root_text, const std::string_view path_text)
	{
		if (root_text.empty() || path_text.empty()) return false;
		const auto root_canonical = canonical_text(root_text);
		const auto path_canonical = canonical_text(path_text);
		std::string_view root = root_canonical;
		const std::string_view path = path_canonical;
		while (root.size() > 1 && root.back() == '/') root.remove_suffix(1);
		if (root.empty() || path.size() < root.size()) return false;
		if (root == "/") return !path.empty() && path.front() == '/';
		if (path.substr(0, root.size()) != root) return false;
		if (path.size() == root.size()) return true;
		return path[root.size()] == '/';
	}

	class linux_atomic_writer final : public platform::atomic_file_writer
	{
		platform::file_ptr _stage;
		core::file_path _stage_path;
		core::file_path _destination;
		bool _committed = false;
		platform::file_op_result _failure;

		platform::file_op_result remember(platform::file_op_result result)
		{
			if (_failure.success()) _failure = result;
			return _failure;
		}

		void discard_stage()
		{
			if (_stage_path.is_empty()) return;
			_stage.reset();
			const auto removed = platform::delete_file(_stage_path);
			if (removed.failed()) core::log(__FUNCTION__, removed.error_message);
		}

	public:
		linux_atomic_writer(platform::file_ptr stage, core::file_path stage_path, core::file_path destination)
			: _stage(std::move(stage)), _stage_path(std::move(stage_path)), _destination(std::move(destination)),
			  _failure{platform::file_op_result_code::OK}
		{
		}

		~linux_atomic_writer() override
		{
			if (!_committed) discard_stage();
		}

		platform::file_op_result write(const std::span<const uint8_t> bytes) override
		{
			if (!_failure.success()) return _failure;
			if (atomic_failure.load(std::memory_order_relaxed) == platform::atomic_file_test_failure::write)
				return remember({platform::file_op_result_code::FAILED, "The atomic writer write was forced to fail."});
			if (!_stage) return remember({platform::file_op_result_code::FAILED, "The atomic writer has no stage."});
			const auto written = _stage->write(bytes.data(), bytes.size());
			if (written == bytes.size()) return {platform::file_op_result_code::OK};
			return remember(failure(errno, "The atomic writer wrote fewer bytes than requested."));
		}

		platform::file_op_result flush() override
		{
			if (!_failure.success()) return _failure;
			if (atomic_failure.load(std::memory_order_relaxed) == platform::atomic_file_test_failure::flush)
				return remember({platform::file_op_result_code::FAILED, "The atomic writer flush was forced to fail."});
			if (!_stage || !_stage->flush()) return remember(failure(errno));
			return {platform::file_op_result_code::OK};
		}

		platform::file_op_result commit() override
		{
			if (!_failure.success()) return _failure;
			if (_committed)
				return remember({platform::file_op_result_code::FAILED, "The atomic writer was already committed."});
			if (atomic_failure.load(std::memory_order_relaxed) == platform::atomic_file_test_failure::commit)
				return remember({platform::file_op_result_code::FAILED, "The atomic writer commit was forced to fail."});

			const auto flushed = flush();
			if (!flushed.success()) return flushed;
			_stage.reset();
			const auto moved = platform::move_file(_stage_path, _destination, false);
			if (moved.failed())
			{
				const auto result = remember(operation_failure(moved));
				const auto removed = platform::delete_file(_stage_path);
				if (removed.failed()) core::log(__FUNCTION__, removed.error_message);
				return result;
			}
			_committed = true;
			return {platform::file_op_result_code::OK};
		}

		core::file_path staged_path() const override { return _stage_path; }
	};


	std::string normalize_url_path(const std::string_view path)
	{
		const bool absolute = !path.empty() && path.front() == '/';
		const bool trailing = !path.empty() && path.back() == '/';
		std::vector<std::string_view> parts;
		size_t begin = 0;
		while (begin <= path.size())
		{
			const auto end = path.find('/', begin);
			const auto part = path.substr(begin, end == std::string_view::npos ? path.size() - begin : end - begin);
			if (part.empty() || part == ".")
			{
			}
			else if (part == "..")
			{
				if (!parts.empty() && parts.back() != "..") parts.pop_back();
				else if (!absolute) parts.push_back(part);
			}
			else
			{
				parts.push_back(part);
			}
			if (end == std::string_view::npos) break;
			begin = end + 1;
		}

		std::string result;
		if (absolute) result = "/";
		for (size_t i = 0; i != parts.size(); ++i)
		{
			if (!result.empty() && result.back() != '/') result += '/';
			result += parts[i];
		}
		if (trailing && (result.empty() || result.back() != '/')) result += '/';
		if (result.empty() && absolute) result = "/";
		return result;
	}

	bool url_has_scheme(const std::string_view text)
	{
		const auto colon = text.find(':');
		if (colon == std::string_view::npos || colon == 0) return false;
		for (size_t i = 0; i != colon; ++i)
		{
			const auto c = static_cast<unsigned char>(text[i]);
			if (!(std::isalnum(c) || c == '+' || c == '-' || c == '.')) return false;
		}
		return true;
	}

	const char* iconv_name(const uint32_t codepage)
	{
		switch (codepage)
		{
		case 1250: return "WINDOWS-1250";
		case 1251: return "WINDOWS-1251";
		case 1252: return "WINDOWS-1252";
		case 1253: return "WINDOWS-1253";
		case 1254: return "WINDOWS-1254";
		case 1255: return "WINDOWS-1255";
		case 1256: return "WINDOWS-1256";
		case 1257: return "WINDOWS-1257";
		case 1258: return "WINDOWS-1258";
		case 28591: return "ISO-8859-1";
		case 28592: return "ISO-8859-2";
		case 28593: return "ISO-8859-3";
		case 28594: return "ISO-8859-4";
		case 28595: return "ISO-8859-5";
		case 28596: return "ISO-8859-6";
		case 28597: return "ISO-8859-7";
		case 28598: return "ISO-8859-8";
		case 28599: return "ISO-8859-9";
		case 28603: return "ISO-8859-13";
		case 28605: return "ISO-8859-15";
		case 20866: return "KOI8-R";
		case 21866: return "KOI8-U";
		case 932: return "SHIFT_JIS";
		case 20932: return "EUC-JP";
		case 50220: return "ISO-2022-JP";
		case 936: return "GBK";
		case 54936: return "GB18030";
		case 950: return "BIG5";
		case 949: return "EUC-KR";
		case 874: return "TIS-620";
		default: return nullptr;
		}
	}

	std::string transcode_utf16(const std::string_view bytes, const bool little_endian)
	{
		std::string result;
		result.reserve(bytes.size());
		auto read = [bytes, little_endian](const size_t index)
		{
			const auto first = static_cast<uint8_t>(bytes[index]);
			const auto second = static_cast<uint8_t>(bytes[index + 1]);
			return static_cast<uint16_t>(little_endian ? first | second << 8 : second | first << 8);
		};
		for (size_t i = 0; i + 1 < bytes.size(); i += 2)
		{
			uint32_t code_point = read(i);
			if (code_point >= 0xd800 && code_point <= 0xdbff && i + 3 < bytes.size())
			{
				const auto trail = read(i + 2);
				if (trail >= 0xdc00 && trail <= 0xdfff)
				{
					code_point = 0x10000 + ((code_point - 0xd800) << 10) + trail - 0xdc00;
					i += 2;
				}
			}
			if ((code_point >= 0xd800 && code_point <= 0xdfff) || code_point > 0x10ffff) code_point = 0xfffd;
			core::char32_to_utf8(std::back_inserter(result), code_point);
		}
		return result;
	}
}

bool platform::is_directory(const core::file_path& path)
{
	if (path.is_empty()) return false;
	std::error_code error;
	return std::filesystem::is_directory(std::filesystem::path(path.str()), error) && !error;
}

bool platform::is_directory(const core::folder_path& path)
{
	if (path.is_empty()) return false;
	std::error_code error;
	return std::filesystem::is_directory(std::filesystem::path(std::string(path.text().sv())), error) && !error;
}

core::folder_path platform::current_directory()
{
	std::error_code error;
	const auto path = std::filesystem::current_path(error);
	return error ? core::folder_path{} : core::folder_path(path.generic_string());
}


core::file_path platform::canonical_path(const core::file_path& path)
{
	return path.is_empty() ? core::file_path{} : core::file_path(canonical_text(path.str()));
}

core::folder_path platform::canonical_path(const core::folder_path& path)
{
	return path.is_empty() ? core::folder_path{} : core::folder_path(canonical_text(path.text().sv()));
}

bool platform::is_path_within(const core::file_path& root, const core::file_path& path)
{
	return path_within_text(root.str(), path.str());
}

bool platform::is_path_within(const core::folder_path& root, const core::file_path& path)
{
	return path_within_text(root.text().sv(), path.str());
}

bool platform::is_path_within(const core::folder_path& root, const core::folder_path& path)
{
	return path_within_text(root.text().sv(), path.text().sv());
}

platform::atomic_file_writer_open_result platform::create_atomic_file_writer_result(const core::file_path& destination)
{
	if (destination.is_empty())
		return {nullptr, {file_op_result_code::FAILED, "The atomic writer destination is empty."}};
	if (atomic_failure.load(std::memory_order_relaxed) == atomic_file_test_failure::open)
		return {nullptr, {file_op_result_code::FAILED, "The atomic writer open was forced to fail."}};

	bool force_collision = atomic_failure.load(std::memory_order_relaxed) == atomic_file_test_failure::stage_collision;
	for (unsigned attempt = 0; attempt != 64; ++attempt)
	{
		if (force_collision)
		{
			force_collision = false;
			continue;
		}
		uint32_t random = 0;
		core::generate_random_bytes(reinterpret_cast<uint8_t*>(&random), sizeof(random));
		const auto name = std::format(".libdesktop-stage-{:08x}-{:08x}.tmp",
		                              static_cast<uint32_t>(::getpid()), random);
		const auto stage_path = destination.folder().combine_file(name);
		const auto stage = platform::open_file(stage_path, file_open_mode::create_new);
		if (stage)
			return {std::make_shared<linux_atomic_writer>(stage, stage_path, destination), {platform::file_op_result_code::OK}};
		if (errno != EEXIST) return {nullptr, failure(errno)};
	}
	return {nullptr, {file_op_result_code::ALREADY_EXISTS, "Unable to claim a unique atomic stage path."}};
}

platform::atomic_file_writer_ptr platform::create_atomic_file_writer(const core::file_path& destination)
{
	return create_atomic_file_writer_result(destination).writer;
}

void platform::test_set_atomic_file_failure(const atomic_file_test_failure failure_kind)
{
	atomic_failure.store(failure_kind, std::memory_order_relaxed);
}


platform::async_http_session_ptr platform::create_async_http_session(const std::string_view)
{
	return nullptr;
}

std::string platform::resolve_url(const std::string_view base, const std::string_view relative)
{
	if (relative.empty()) return std::string(base);
	if (url_has_scheme(relative)) return std::string(relative);

	const auto scheme_end = base.find("://");
	if (scheme_end == std::string_view::npos)
		return relative.front() == '/' ? normalize_url_path(relative) : std::string(relative);

	const auto authority_end = base.find('/', scheme_end + 3);
	const auto prefix = std::string(base.substr(0, authority_end));
	const auto base_path = authority_end == std::string_view::npos ? std::string_view("/")
	                                                               : base.substr(authority_end);
	if (relative.starts_with("//")) return std::string(base.substr(0, scheme_end)) + ":" + std::string(relative);

	const auto query_or_fragment = base_path.find_first_of("?#");
	const auto path_without_query = base_path.substr(0, query_or_fragment);
	if (relative.front() == '#' || relative.front() == '?') return prefix + std::string(path_without_query) + std::string(relative);

	std::string joined;
	if (relative.front() == '/')
		joined = std::string(relative);
	else
	{
		const auto slash = path_without_query.find_last_of('/');
		joined = std::string(path_without_query.substr(0, slash == std::string_view::npos ? 0 : slash + 1));
		joined += relative;
	}
	return prefix + normalize_url_path(joined);
}

uint32_t platform::charset_to_codepage(const std::string_view charset)
{
	struct entry { std::string_view name; uint32_t codepage; };
	static constexpr entry table[] = {
		{"utf-8", 65001}, {"utf8", 65001}, {"us-ascii", 65001}, {"ascii", 65001},
		{"utf-16", 1200}, {"utf-16le", 1200}, {"utf-16be", 1201},
		{"iso-8859-1", 28591}, {"latin1", 28591}, {"l1", 28591}, {"iso8859-1", 28591},
		{"windows-1250", 1250}, {"windows-1251", 1251}, {"windows-1252", 1252},
		{"windows-1253", 1253}, {"windows-1254", 1254}, {"windows-1255", 1255},
		{"windows-1256", 1256}, {"windows-1257", 1257}, {"windows-1258", 1258},
		{"cp1250", 1250}, {"cp1251", 1251}, {"cp1252", 1252},
		{"iso-8859-2", 28592}, {"iso-8859-3", 28593}, {"iso-8859-4", 28594},
		{"iso-8859-5", 28595}, {"iso-8859-6", 28596}, {"iso-8859-7", 28597},
		{"iso-8859-8", 28598}, {"iso-8859-9", 28599}, {"iso-8859-13", 28603},
		{"iso-8859-15", 28605}, {"koi8-r", 20866}, {"koi8-u", 21866},
		{"shift_jis", 932}, {"shift-jis", 932}, {"sjis", 932}, {"ms_kanji", 932},
		{"euc-jp", 20932}, {"iso-2022-jp", 50220}, {"gb2312", 936}, {"gbk", 936},
		{"gb18030", 54936}, {"big5", 950}, {"euc-kr", 949}, {"ks_c_5601-1987", 949},
		{"windows-874", 874}, {"tis-620", 874},
	};
	for (const auto& candidate : table)
		if (core::icmp(charset, candidate.name) == 0) return candidate.codepage;
	return 0;
}

std::string platform::transcode_to_utf8(const std::string_view bytes, const uint32_t codepage)
{
	if (codepage == 0 || codepage == 65001 || bytes.empty()) return std::string(bytes);
	if (codepage == 1200 || codepage == 1201) return transcode_utf16(bytes, codepage == 1200);
	if (codepage == 28591)
	{
		std::string result;
		result.reserve(bytes.size() * 2);
		for (const auto byte : bytes) core::char32_to_utf8(std::back_inserter(result), static_cast<uint8_t>(byte));
		return result;
	}

	const auto encoding = iconv_name(codepage);
	if (!encoding) return std::string(bytes);
	const auto converter = iconv_open("UTF-8", encoding);
	if (converter == reinterpret_cast<iconv_t>(-1)) return std::string(bytes);

	std::vector<char> output(std::max<size_t>(64, bytes.size() * 4 + 16));
	char* input = const_cast<char*>(bytes.data());
	size_t input_left = bytes.size();
	std::string result;
	while (input_left != 0)
	{
		char* out = output.data();
		size_t output_left = output.size();
		const auto converted = iconv(converter, &input, &input_left, &out, &output_left);
		result.append(output.data(), output.size() - output_left);
		if (converted != static_cast<size_t>(-1)) continue;
		if (errno == E2BIG) continue;
		iconv_close(converter);
		return std::string(bytes);
	}
	iconv_close(converter);
	return result;
}

std::string platform::url_encode(const std::string_view input)
{
	static constexpr char hex[] = "0123456789ABCDEF";
	std::string result;
	result.reserve(input.size());
	for (const auto c : input)
	{
		const auto byte = static_cast<uint8_t>(c);
		if ((byte >= 'A' && byte <= 'Z') || (byte >= 'a' && byte <= 'z') ||
			(byte >= '0' && byte <= '9') || byte == '-' || byte == '_' || byte == '.' || byte == '~')
			result += static_cast<char>(byte);
		else
		{
			result += '%';
			result += hex[byte >> 4];
			result += hex[byte & 0x0f];
		}
	}
	return result;
}
