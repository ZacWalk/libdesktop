// text_input.h — what untrusted text has to survive before it becomes a prompt.
//
// Anything a person can paste can arrive here: a fragment of a binary file, a
// half-decoded mail body, a megabyte of control characters. A prompt is then sent
// somewhere that will act on it, so this is a trust boundary rather than a
// convenience — and it is the one thing a prompt editor needs that a plain
// editing model does not have.
//
// pf::ui::text_buffer is an editing model. It happily holds whatever bytes it is
// given, which is right for a document loaded from disk and wrong for a field a
// stranger's text can reach. This is the filter in front of it.
//
// Everything here is pure, so all of it is directly testable.

#pragma once

#include "platform.h"

#include <cstdint>
#include <string>
#include <string_view>

namespace pf::ui::input
{
	// What one prompt may hold, and how much clipboard will even be looked at.
	// The scan bound matters as much as the output bound: text that is rejected
	// still costs time to reject, so a megabyte of control characters must not
	// become a megabyte of work.
	inline constexpr size_t max_text_bytes = 4096;
	inline constexpr size_t max_editor_bytes = 256 * 1024;
	inline constexpr size_t max_paste_bytes = max_editor_bytes;

	constexpr size_t utf8_size(const char32_t ch)
	{
		return ch < 0x80 ? 1 : ch < 0x800 ? 2 : ch < 0x10000 ? 3 : 4;
	}

	// A value Unicode can actually encode: in range, and not half of a surrogate
	// pair pretending to be a character in its own right.
	constexpr bool is_scalar(const char32_t ch)
	{
		return ch <= 0x10FFFF && !(ch >= 0xD800 && ch <= 0xDFFF);
	}

	// Printable means "a renderer can be handed this": not a control character,
	// not a C1 escape, and not one of the two separators that are really newlines.
	constexpr bool is_printable(const char32_t ch)
	{
		return is_scalar(ch) && ch >= 0x20 && !(ch >= 0x7F && ch <= 0x9F) &&
			ch != 0x2028 && ch != 0x2029;
	}

	constexpr bool is_newline(const char32_t ch)
	{
		return ch == U'\r' || ch == U'\n' || ch == 0x85 || ch == 0x2028 || ch == 0x2029;
	}

	// Filters text down to what may enter a prompt, in UTF-8.
	//
	// Invalid encoding is discarded rather than repaired: an overlong form, an
	// encoded surrogate, a truncated scalar and a stray continuation byte all
	// vanish, and none of them can swallow the valid text after them. Control
	// characters go. Runs of separators collapse to one.
	//
	// In single-line mode a newline becomes a space, because a prompt that is one
	// line must not be able to contain two. In multiline mode newlines survive and
	// CRLF becomes a single break, so a pasted Markdown paragraph stays one.
	//
	// The result never exceeds byte_budget and never ends mid-character.
	inline std::string normalize(const std::string_view text, size_t byte_budget = max_text_bytes,
	                             const bool multiline = false)
	{
		const auto end = std::min(text.size(), max_paste_bytes);
		byte_budget = std::min(byte_budget, max_editor_bytes);

		std::string out;
		out.reserve(std::min(byte_budget, end));

		size_t bytes = 0;
		bool previous_separator = false;

		for (size_t i = 0; i < end;)
		{
			const auto lead = static_cast<unsigned char>(text[i]);
			size_t count = 1;
			char32_t ch = lead;

			if (lead >= 0xC2 && lead <= 0xDF) { count = 2; ch = lead & 0x1F; }
			else if (lead >= 0xE0 && lead <= 0xEF) { count = 3; ch = lead & 0x0F; }
			else if (lead >= 0xF0 && lead <= 0xF4) { count = 4; ch = lead & 0x07; }
			else if (lead >= 0x80) { ++i; continue; } // a continuation with no lead, or an overlong two-byte form

			if (i + count > end) break; // a scalar cut off by the end of the input

			bool valid = true;

			for (size_t n = 1; n < count; ++n)
			{
				const auto next = static_cast<unsigned char>(text[i + n]);
				if ((next & 0xC0) != 0x80) { valid = false; break; }
				ch = (ch << 6) | (next & 0x3F);
			}

			// A malformed sequence costs only its lead byte, so the valid text
			// after it is still read rather than swallowed.
			if (!valid) { ++i; continue; }

			i += count;

			// Encoded in more bytes than it needs, or not a scalar at all
			if (!is_scalar(ch) || (count == 2 && ch < 0x80) ||
				(count == 3 && ch < 0x800) || (count == 4 && ch < 0x10000))
				continue;

			const auto newline = is_newline(ch);
			const auto separator = newline || ch == U'\t';

			if (separator)
			{
				if (multiline && newline)
				{
					if (ch == U'\r' && i < end && text[i] == '\n') ++i; // CRLF is one break
					ch = U'\n';
				}
				else
				{
					if (previous_separator) continue;
					ch = U' ';
				}
			}
			else if (!is_printable(ch))
			{
				continue;
			}

			previous_separator = separator;

			const auto length = utf8_size(ch);
			if (length > byte_budget - bytes) break; // stops between characters, never inside one

			bytes += length;
			pf::char32_to_utf8(std::back_inserter(out), ch);
		}

		return out;
	}

	// Reassembles a supplementary character from the two halves some backends
	// deliver it in.
	//
	// pf hands a view a char32_t, but a backend translating an older UTF-16
	// message can only give half a character at a time. Inserting either half on
	// its own would put an invalid scalar in the buffer, so the lead waits for its
	// trail — and anything else that happens first discards it rather than letting
	// it pair with a character it never belonged to.
	class char_assembler
	{
	public:
		// The complete character, or 0 while a lead is still waiting.
		char32_t accept(const char32_t ch)
		{
			if (ch >= 0xD800 && ch <= 0xDBFF)
			{
				_pending_lead = ch;
				return 0;
			}

			if (ch >= 0xDC00 && ch <= 0xDFFF)
			{
				if (_pending_lead == 0) return 0; // a trail with nothing before it is not a character
				const auto combined = 0x10000 + ((_pending_lead - 0xD800) << 10) + (ch - 0xDC00);
				_pending_lead = 0;
				return combined;
			}

			_pending_lead = 0;
			return ch;
		}

		// Anything that is not the other half — a click, a key, losing focus —
		// means the pair will never complete.
		void cancel() { _pending_lead = 0; }

		[[nodiscard]] bool waiting() const { return _pending_lead != 0; }

	private:
		char32_t _pending_lead = 0;
	};
}
