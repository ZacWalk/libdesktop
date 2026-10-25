// text_line.h — one line of a text buffer.
//
// A line either owns its text or points into bytes shared with every other line of
// the same document, which is how a large file loads without copying each line.
//
// Those shared bytes are **always UTF-8**. Decoding a file that arrived in another
// encoding is loading work and belongs to the application; by the time a line points
// at bytes they are UTF-8, so rendering is a copy rather than a decode.

#pragma once

#include "platform.h"

#include <memory>
#include <vector>

namespace pf::ui
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
