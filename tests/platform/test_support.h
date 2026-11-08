#pragma once

// A private temporary folder per run, removed at the end; Diffractor's fixture without its
// /test-temp override.
#include "test.h"
#include "libdesktop/platform.h"

#include <bit>
#include <chrono>
#include <thread>

template <typename Operation, typename IsBusy>
auto retry_clipboard(Operation operation, IsBusy is_busy)
{
	const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(200);
	auto result = operation();
	while (is_busy(result) && std::chrono::steady_clock::now() < deadline)
	{
		std::this_thread::sleep_for(std::chrono::milliseconds(5));
		result = operation();
	}
	return result;
}
#include <filesystem>
#include <fstream>
#include <system_error>

class temp_files
{
	mutable core::folder_path _folder;
	core::file_paths _paths;
	mutable int _folder_seq = 0;
	mutable bool _owns_folder = false;

	core::folder_path resolve_folder() const
	{
		if (_folder.is_empty())
		{
			const auto base = platform::temp_folder();
			if (!platform::exists(base))
			{
				platform::create_folder(base);
			}

			for (auto attempt = 0; attempt < 100; ++attempt)
			{
				const auto candidate = base.combine(std::format("libdesktop-{}-{}", core::tick_count(), attempt));
				if (platform::exists(candidate)) continue;

				const auto result = platform::create_folder(candidate);
				if (result.success())
				{
					_folder = candidate;
					_owns_folder = true;
					break;
				}
			}

			if (_folder.is_empty())
			{
				throw std::runtime_error("Failed to create an exclusive test temp folder");
			}
		}

		return _folder;
	}

public:
	temp_files() = default;
	temp_files(const temp_files&) = delete;
	temp_files& operator=(const temp_files&) = delete;

	std::vector<std::string> delete_temps()
	{
		std::vector<std::string> errors;
		core::file_paths remaining_paths;

		for (const auto& path : _paths)
		{
			if (!platform::exists(path)) continue;

			const auto result = platform::delete_file(path);
			if (result.failed() && platform::exists(path))
			{
				errors.emplace_back(result.format_error("delete temp file", path.str()));
				remaining_paths.emplace_back(path);
			}
		}

		_paths = std::move(remaining_paths);

		if (_owns_folder && !_folder.is_empty() && platform::exists(_folder))
		{
			std::error_code ec;
			std::filesystem::remove_all(platform::to_stream_path(_folder), ec);
			if (ec)
			{
				errors.emplace_back(std::format("delete temp folder\n{}\n{}", _folder.text(), ec.message()));
			}

			if (!platform::exists(_folder))
			{
				_folder = {};
				_owns_folder = false;
				_folder_seq = 0;
				_paths.clear();
			}
		}

		return errors;
	}

	core::folder_path folder() const
	{
		return resolve_folder();
	}

	core::file_path next_path(const std::string_view ext = ".jpg")
	{
		auto result = platform::temp_file(ext, resolve_folder());
		_paths.emplace_back(result);
		return result;
	}

	core::folder_path next_folder(const std::string_view name)
	{
		const auto result = resolve_folder().combine(std::format("{}-{}", name, ++_folder_seq));

		if (!platform::exists(result))
		{
			platform::create_folder(result);
		}

		return result;
	}
};

inline temp_files _temps;

inline void write_test_file(const core::file_path path, const std::string_view text)
{
	std::ofstream fs(platform::to_stream_path(path), std::ios::binary | std::ios::trunc);
	fs << text;
}

inline std::string read_test_file(const core::file_path path)
{
	const auto data = platform::blob_from_file(path);
	return {std::bit_cast<const char*>(data.data()), data.size()};
}
