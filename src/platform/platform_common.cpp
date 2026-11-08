// This file is part of libdesktop, a C++20 platform layer for desktop applications
// Copyright 2026  Zac Walker
//
// This program is free software; you can redistribute it and / or modify it
// under the terms of the LGPL License either version 2.1 or later.
// License details are available at https://www.gnu.org/licenses/lgpl-2.1.html
// This program is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY

// Purpose: Portable platform services whose implementation uses the OS-neutral core/platform contracts.

#include "libdesktop/platform.h"
#include "libdesktop/core/json.h"
#include "libdesktop/platform/file.h"
#include "libdesktop/core/selector.h"
#include "rapidjson/istreamwrapper.h"

namespace
{
	core::mutex log_file_mutex;
	std::ofstream log_file;
	bool log_started = false;

	void write_log_line(const std::string_view line)
	{
		core::exclusive_lock lock(log_file_mutex);
		if (log_file.is_open()) log_file.write(line.data(), static_cast<std::streamsize>(line.size()));
	}
}

std::atomic_bool platform::is_closing = false;
core::file_path platform::log_path;
core::file_path platform::previous_log_path;

void platform::start_log()
{
	{
		core::exclusive_lock lock(log_file_mutex);
		if (log_started) return;
		log_started = true;

		const auto module_folder = platform::known_path(platform::known_folder::running_app_folder);
		const auto folder = platform::is_writable(module_folder) ? module_folder : platform::known_path(platform::known_folder::app_data);
		log_path = folder.combine_file(std::format("{}.log", core::app_file_stem()));
		previous_log_path = folder.combine_file(std::format("{}.previous.log", core::app_file_stem()));

		if (!log_path.is_empty())
		{
			if (platform::exists(log_path)) platform::move_file(log_path, previous_log_path, false);
			log_file.open(platform::to_stream_path(log_path), std::ios::out | std::ios::trunc);
		}
	}

	core::set_log_sink(&write_log_line);
}

// The platform owns the file and detaches the sink before closing it, so late library diagnostics
// cannot write through a closed stream.
core::file_path platform::close_log()
{
	core::set_log_sink(nullptr);
	core::exclusive_lock lock(log_file_mutex);
	if (log_file.is_open()) log_file.close();
	return log_path;
}

core::date_t platform::utc_to_local(const core::date_t timestamp)
{
	return core::date_t(utc_to_local(timestamp._i));
}

core::date_t platform::local_to_utc(const core::date_t timestamp)
{
	return core::date_t(local_to_utc(timestamp._i));
}

std::string platform::format_size(const core::file_size& size)
{
	const auto rounded = core::round_size(size.to_int64());
	return rounded.dec == 0
		       ? std::format("{} {}", rounded.n, rounded.unit)
		       : std::format("{}{}{} {}", rounded.n, platform::number_dec_sep(), rounded.dec, rounded.unit);
}

core::json::json_doc platform::json_from_file(const core::file_path path)
{
	std::ifstream ifs(platform::to_stream_path(path));
	rapidjson::BasicIStreamWrapper<std::ifstream> isw(ifs);
	core::json::json_doc d;
	d.ParseStream(isw);
	return d;
}

namespace
{
	constexpr std::array<uint32_t, 64> sha256_round_constants = {
		0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4,
		0xab1c5ed5, 0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe,
		0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f,
		0x4a7484aa, 0x5cb0a9dc, 0x76f988da, 0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7,
		0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc,
		0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b,
		0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070, 0x19a4c116,
		0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
		0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7,
		0xc67178f2
	};

	constexpr uint32_t rotr(const uint32_t value, const uint32_t amount)
	{
		return (value >> amount) | (value << (32 - amount));
	}

	class sha256_state
	{
		std::array<uint32_t, 8> _state = {
			0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
			0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19
		};
		std::array<uint8_t, 64> _block = {};
		size_t _block_size = 0;
		uint64_t _bytes = 0;

		void transform(const uint8_t* block)
		{
			std::array<uint32_t, 64> words = {};
			for (size_t i = 0; i != 16; ++i)
			{
				const auto offset = i * 4;
				words[i] = static_cast<uint32_t>(block[offset]) << 24 |
				           static_cast<uint32_t>(block[offset + 1]) << 16 |
				           static_cast<uint32_t>(block[offset + 2]) << 8 |
				           block[offset + 3];
			}
			for (size_t i = 16; i != words.size(); ++i)
			{
				const auto s0 = rotr(words[i - 15], 7) ^ rotr(words[i - 15], 18) ^ (words[i - 15] >> 3);
				const auto s1 = rotr(words[i - 2], 17) ^ rotr(words[i - 2], 19) ^ (words[i - 2] >> 10);
				words[i] = words[i - 16] + s0 + words[i - 7] + s1;
			}

			auto a = _state[0];
			auto b = _state[1];
			auto c = _state[2];
			auto d = _state[3];
			auto e = _state[4];
			auto f = _state[5];
			auto g = _state[6];
			auto h = _state[7];
			for (size_t i = 0; i != words.size(); ++i)
			{
				const auto sum1 = rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25);
				const auto choice = (e & f) ^ (~e & g);
				const auto t1 = h + sum1 + choice + sha256_round_constants[i] + words[i];
				const auto sum0 = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22);
				const auto majority = (a & b) ^ (a & c) ^ (b & c);
				const auto t2 = sum0 + majority;
				h = g;
				g = f;
				f = e;
				e = d + t1;
				d = c;
				c = b;
				b = a;
				a = t1 + t2;
			}
			_state[0] += a;
			_state[1] += b;
			_state[2] += c;
			_state[3] += d;
			_state[4] += e;
			_state[5] += f;
			_state[6] += g;
			_state[7] += h;
		}

	public:
		void update(const std::span<const uint8_t> bytes)
		{
			_bytes += bytes.size();
			auto* data = bytes.data();
			auto remaining = bytes.size();
			while (remaining)
			{
				const auto copied = std::min(remaining, _block.size() - _block_size);
				std::memcpy(_block.data() + _block_size, data, copied);
				_block_size += copied;
				data += copied;
				remaining -= copied;
				if (_block_size == _block.size())
				{
					transform(_block.data());
					_block_size = 0;
				}
			}
		}

		core::sha256_digest finish() const
		{
			auto copy = *this;
			copy._block[copy._block_size++] = 0x80;
			if (copy._block_size > 56)
			{
				std::fill(copy._block.begin() + copy._block_size, copy._block.end(), 0);
				copy.transform(copy._block.data());
				copy._block_size = 0;
			}
			std::fill(copy._block.begin() + copy._block_size, copy._block.begin() + 56, 0);
			const auto bits = copy._bytes * 8;
			for (size_t i = 0; i != 8; ++i)
				copy._block[56 + i] = static_cast<uint8_t>(bits >> (56 - i * 8));
			copy.transform(copy._block.data());

			core::sha256_digest result = {};
			for (size_t i = 0; i != copy._state.size(); ++i)
			{
				result[i * 4] = static_cast<uint8_t>(copy._state[i] >> 24);
				result[i * 4 + 1] = static_cast<uint8_t>(copy._state[i] >> 16);
				result[i * 4 + 2] = static_cast<uint8_t>(copy._state[i] >> 8);
				result[i * 4 + 3] = static_cast<uint8_t>(copy._state[i]);
			}
			return result;
		}
	};
}
core::file_path platform::probe_data_file(const std::string_view file_name)
{
	const auto module_folder = platform::known_path(platform::known_folder::running_app_folder);
	const auto app_data_folder = platform::known_path(platform::known_folder::app_data);
	const auto module_path = module_folder.combine_file(file_name);

	if (platform::exists(module_path))
	{
		return module_path;
	}

	return app_data_folder.combine_file(file_name);
}

core::blob platform::blob_from_file(const core::file_path path)
{
	platform::buffered_file f;

	if (f.open_read(path, true))
	{
		const auto file_len = f.file_size();

		// Refused, not clamped. Returning the first max_blob_size bytes of a larger file answers
		// with something that looks like the file and is not, which every caller then treats as
		// whole - the edit path re-encodes it over the original.
		if (file_len > core::max_blob_size)
		{
			const auto message = std::format("Cannot read file into memory ({} bytes)", file_len);
			core::log(__FUNCTION__, message);
			throw core::app_exception(message);
		}

		return f.read_blob(static_cast<size_t>(file_len));
	}

	return {};
}

core::blob platform::blob_head_from_file(const core::file_path path, const size_t max_load)
{
	platform::buffered_file f;

	if (f.open_read(path, true))
	{
		const auto file_len = f.file_size();
		const auto load_len = max_load != 0 && file_len > max_load ? max_load : file_len;

		if (load_len > core::max_blob_size)
		{
			const auto message = std::format("Cannot read file into memory ({} bytes)", file_len);
			core::log(__FUNCTION__, message);
			throw core::app_exception(message);
		}

		return f.read_blob(static_cast<size_t>(load_len));
	}

	return {};
}

bool platform::blob_save_to_file(const core::cspan data, const core::file_path path)
{
	// Staged beside the destination and moved into place, because writing
	// straight to the destination truncates whatever is there before the first byte is written, so
	// a short write or a full disk would leave a partial file where a whole one used to be.
	const auto staged = platform::temp_file(path.extension(), path.folder());
	auto file = platform::open_file(staged, platform::file_open_mode::create);

	if (!file)
	{
		return false;
	}

	const auto len = data.size;

	if (len > 0 && static_cast<size_t>(file->write(data.data, len)) != len)
	{
		file.reset();
		platform::delete_file(staged);
		return false;
	}

	file.reset();

	if (platform::replace_file(path, staged).failed())
	{
		platform::delete_file(staged);
		return false;
	}

	return true;
}
std::optional<core::sha256_digest> platform::file_sha256(const core::file_path path,
                                                             const core::cancel_token& token)
{
	return platform::file_sha256(platform::open_file_lock(path, platform::file_lock_mode::read), token);
}

std::optional<core::sha256_digest> platform::file_sha256(const platform::file_ptr& file,
                                                             const core::cancel_token& token)
{
	if (!file) return {};

	const auto expected = file->size();
	std::array<uint8_t, 64 * 1024> buffer = {};
	sha256_state hash;
	uint64_t total = 0;
	while (total < expected)
	{
		if (token.is_cancelled()) return {};
		const auto wanted = std::min<uint64_t>(expected - total, buffer.size());
		const auto read = file->read_at(total, buffer.data(), wanted);
		if (read == 0 || read > wanted) return {};
		hash.update({buffer.data(), static_cast<size_t>(read)});
		total += read;
	}

	if (total != expected || file->size() != expected || token.is_cancelled()) return {};
	return hash.finish();
}


bool core::item_selector::can_iterate() const
{
	return platform::exists(_root) || core::is_server(_root.text());
}
