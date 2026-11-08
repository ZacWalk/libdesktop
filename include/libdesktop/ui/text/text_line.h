// This file is part of libdesktop, a C++20 platform layer for desktop applications
// Copyright 2026  Zac Walker
//
// This program is free software; you can redistribute it and / or modify it
// under the terms of the LGPL License either version 2.1 or later.
// License details are available at https://www.gnu.org/licenses/lgpl-2.1.html
// This program is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY

// Purpose: UTF-8 text lines with shared immutable backing storage.

#pragma once

#include <cstdint>

#include <memory>
#include <string>
#include <utility>
#include <string_view>
#include <vector>

namespace ui
{
	// Shared UTF-8 backing bytes for the lines of one loaded document.
	using text_bytes_ptr = std::shared_ptr<const std::vector<uint8_t>>;

	constexpr int invalid_length = -1;

	class text_line
	{
	public:
		text_line() = default;

		explicit text_line(std::string line) : _text(std::move(line))
		{
		}

		text_line(const std::string_view text) : _text(text)
		{
		}

		text_line(text_bytes_ptr bytes, const uint32_t offset, const uint32_t length)
			: _bytes(std::move(bytes)), _offset(offset), _length(length)
		{
		}

		[[nodiscard]] bool empty() const
		{
			return _bytes ? _length == 0 : _text.empty();
		}

		// Byte length of the UTF-8 text; O(1).
		[[nodiscard]] size_t size() const
		{
			return _bytes ? _length : _text.size();
		}

		void render(std::string& text_out) const
		{
			if (!_bytes)
			{
				text_out.assign(_text);
				return;
			}

			text_out.assign(reinterpret_cast<const char*>(_bytes->data() + _offset), _length);
		}

		// Replacing the text detaches the line from the shared bytes: an edited line
		// owns itself, so the loaded buffer stays immutable for every other line.
		void update(const std::string_view text)
		{
			_text = text;
			_bytes.reset();
			_offset = 0;
			_length = 0;
			_expanded_length = invalid_length;
		}

		void invalidate_expanded_length() const { _expanded_length = invalid_length; }
		[[nodiscard]] int expanded_length_cache() const { return _expanded_length; }
		void set_expanded_length(const int len) const { _expanded_length = len; }

	private:
		std::string _text;
		text_bytes_ptr _bytes;
		uint32_t _offset = 0;
		uint32_t _length = 0;

		// Width in display columns once tabs are expanded. Cached because layout and
		// paint both ask for it on every line, every frame.
		mutable int _expanded_length = invalid_length;
	};
}
