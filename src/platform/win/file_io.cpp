// This file is part of libdesktop, a C++20 platform layer for desktop applications
// Copyright 2026  Zac Walker
//
// This program is free software; you can redistribute it and / or modify it
// under the terms of the LGPL License either version 2.1 or later.
// License details are available at https://www.gnu.org/licenses/lgpl-2.1.html
// This program is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY

// Purpose: Windows atomic file replacement and filesystem path operations.

#include "libdesktop/platform.h"
#include "libdesktop/platform/win32.h"
#include "os.h"

#include <windows.h>

#include <atomic>
#include <format>
#include <string>
#include <string_view>
#include <vector>

namespace
{
	std::atomic<platform::atomic_file_test_failure> atomic_failure{platform::atomic_file_test_failure::none};

	platform::file_op_result failure(const DWORD error, std::string message = {})
	{
		if (message.empty()) message = platform::last_os_error();
		platform::file_op_result result{platform::file_op_result_code::FAILED};
		result.code = error == ERROR_FILE_EXISTS || error == ERROR_ALREADY_EXISTS
			               ? platform::file_op_result_code::ALREADY_EXISTS
			               : platform::file_op_result_code::FAILED;
		result.error_message = std::move(message);
		result.error = {static_cast<int>(error), std::system_category()};
		return result;
	}

	platform::file_op_result operation_failure(const platform::file_op_result& result)
	{
		return result;
	}

	std::wstring full_path_of(const std::wstring& path)
	{
		std::wstring result(256, L'\0');
		for (;;)
		{
			const auto length = GetFullPathNameW(path.c_str(), static_cast<DWORD>(result.size()), result.data(), nullptr);
			if (length == 0) return path;
			if (length < result.size())
			{
				result.resize(length);
				return result;
			}
			result.resize(static_cast<size_t>(length) + 1);
		}
	}

	bool final_path_of(const std::wstring& path, std::wstring& result)
	{
		const auto handle = CreateFileW(path.c_str(), 0,
		                                FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
		                                nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
		if (handle == INVALID_HANDLE_VALUE) return false;

		std::wstring buffer(256, L'\0');
		for (;;)
		{
			const auto length = GetFinalPathNameByHandleW(handle, buffer.data(), static_cast<DWORD>(buffer.size()),
		                                                  FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
			if (length == 0)
			{
				CloseHandle(handle);
				return false;
			}
			if (length < buffer.size())
			{
				buffer.resize(length);
				break;
			}
			buffer.resize(static_cast<size_t>(length) + 1);
		}
		CloseHandle(handle);

		if (std::wstring_view(buffer).starts_with(L"\\\\?\\")) buffer.erase(0, 4);
		result = std::move(buffer);
		return true;
	}

	std::string append_tail(std::string result, const std::vector<std::string>& tail)
	{
		for (auto i = tail.rbegin(); i != tail.rend(); ++i)
		{
			if (!result.empty() && result.back() != '\\' && result.back() != '/') result += '\\';
			result += *i;
		}
		return result;
	}

	std::string canonical_text(const std::string_view input)
	{
		if (input.empty()) return {};
		const auto full_wide = full_path_of(core::utf8_to_utf16(input));
		const auto full = core::utf16_to_utf8(full_wide);
		std::vector<std::string> tail;
		std::string current_text = full;

		for (int depth = 0; depth != 64; ++depth)
		{
			std::wstring resolved;
			if (final_path_of(core::utf8_to_utf16(current_text), resolved))
				return append_tail(core::utf16_to_utf8(resolved), tail);

			const core::file_path current(current_text);
			const auto name = std::string(current.name().sv());
			const auto parent = current.folder();
			const auto parent_text = std::string(parent.text().sv());
			if (name.empty() || parent.is_empty() || parent_text == current_text) break;
			tail.push_back(name);
			current_text = parent_text;
		}
		return full;
	}

	bool path_within_text(const std::string_view root_text, const std::string_view path_text)
	{
		if (root_text.empty() || path_text.empty()) return false;
		const auto root_canonical = canonical_text(root_text);
		const auto path_canonical = canonical_text(path_text);
		std::string_view root = root_canonical;
		const std::string_view path = path_canonical;
		while (root.size() > 1 && (root.back() == '\\' || root.back() == '/')) root.remove_suffix(1);
		if (root.empty() || path.size() < root.size()) return false;
		if (core::icmp(path.substr(0, root.size()), root) != 0) return false;
		if (path.size() == root.size()) return true;
		return path[root.size()] == '\\' || path[root.size()] == '/';
	}

	class win_atomic_writer final : public platform::atomic_file_writer
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
		win_atomic_writer(platform::file_ptr stage, core::file_path stage_path, core::file_path destination)
			: _stage(std::move(stage)), _stage_path(std::move(stage_path)), _destination(std::move(destination)),
			  _failure{platform::file_op_result_code::OK}
		{
		}

		~win_atomic_writer() override
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
			return remember(failure(GetLastError(), "The atomic writer wrote fewer bytes than requested."));
		}

		platform::file_op_result flush() override
		{
			if (!_failure.success()) return _failure;
			if (atomic_failure.load(std::memory_order_relaxed) == platform::atomic_file_test_failure::flush)
				return remember({platform::file_op_result_code::FAILED, "The atomic writer flush was forced to fail."});
			if (!_stage || !_stage->flush()) return remember(failure(GetLastError()));
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
}

bool platform::is_directory(const core::file_path& path)
{
	if (path.is_empty()) return false;
	const auto attributes = GetFileAttributesW(to_file_system_path(path).c_str());
	return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
}

bool platform::is_directory(const core::folder_path& path)
{
	if (path.is_empty()) return false;
	const auto attributes = GetFileAttributesW(to_file_system_path(path).c_str());
	return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
}

core::folder_path platform::current_directory()
{
	std::wstring result(256, L'\0');
	for (;;)
	{
		const auto length = GetCurrentDirectoryW(static_cast<DWORD>(result.size()), result.data());
		if (length == 0) return {};
		if (length < result.size())
		{
			result.resize(length);
			return core::folder_path(core::utf16_to_utf8(result));
		}
		result.resize(static_cast<size_t>(length) + 1);
	}
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

	static std::atomic_uint32_t sequence = 0;
	bool force_collision = atomic_failure.load(std::memory_order_relaxed) == atomic_file_test_failure::stage_collision;
	for (unsigned attempt = 0; attempt != 64; ++attempt)
	{
		if (force_collision)
		{
			force_collision = false;
			continue;
		}

		const auto name = std::format(".libdesktop-stage-{:08x}-{:08x}.tmp",
		                              static_cast<uint32_t>(GetCurrentProcessId()),
		                              sequence.fetch_add(1, std::memory_order_relaxed));
		const auto stage_path = destination.folder().combine_file(name);
		const auto stage = platform::open_file(stage_path, file_open_mode::create_new);
		if (stage)
			return {std::make_shared<win_atomic_writer>(stage, stage_path, destination), {platform::file_op_result_code::OK}};

		const auto error = GetLastError();
		if (error != ERROR_FILE_EXISTS && error != ERROR_ALREADY_EXISTS)
			return {nullptr, failure(error)};
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
