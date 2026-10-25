// text_types.h — the coordinate and style vocabulary the text layer shares.
//
// These are the types a view, a text buffer and a syntax highlighter all have to
// agree on, so they sit below all three. Positions are UTF-8 **byte offsets**
// within a line, never code points and never display columns: step them with
// pf::utf8_next / pf::utf8_prev, never with ++ or --.

#pragma once

#include "platform.h"

namespace pf::ui
{
	// A position in a document: x is a byte offset into line y.
	class text_location
	{
	public:
		int x = 0;
		int y = 0;

		constexpr text_location(const int xx = 0, const int yy = 0) : x(xx), y(yy)
		{
		}

		bool operator==(const text_location& other) const = default;

		auto operator<=>(const text_location& other) const
		{
			if (const auto cmp = y <=> other.y; cmp != 0) return cmp;
			return x <=> other.x;
		}
	};

	// A range between two positions. The end may precede the start while the user
	// is dragging backwards, so normalize() before treating it as an interval.
	class text_selection
	{
	public:
		text_location _start;
		text_location _end;

		text_selection() = default;

		text_selection(const text_location& start, const text_location& end) : _start(start), _end(end)
		{
		}

		text_selection(const text_location& loc) : _start(loc), _end(loc)
		{
		}

		text_selection(const int x1, const int y1, const int x2, const int y2) : _start(x1, y1), _end(x2, y2)
		{
		}

		bool operator==(const text_selection& other) const = default;

		[[nodiscard]] bool empty() const
		{
			return _start == _end;
		}

		[[nodiscard]] bool is_valid() const
		{
			return _start.x >= 0 && _start.y >= 0 && _end.x >= 0 && _end.y >= 0;
		}

		[[nodiscard]] text_selection normalize() const
		{
			text_selection result;

			if (_start < _end)
			{
				result._start = _start;
				result._end = _end;
			}
			else
			{
				result._start = _end;
				result._end = _start;
			}

			return result;
		}
	};

	// What a run of text means, rather than what colour it is. The theme decides
	// the colour, so an application can repalette without touching a highlighter.
	enum class text_style
	{
		main_wnd_clr,
		tool_wnd_clr,

		white_space,
		normal_bkgnd,
		normal_text,

		sel_margin,
		sel_bkgnd,
		sel_text,

		error_bkgnd,
		error_text,

		code_keyword,
		code_comment,
		code_number,
		code_operator,
		code_string,
		code_preprocessor,

		md_heading1,
		md_heading2,
		md_heading3,
		md_bold,
		md_italic,
		md_code,
		md_link_text,
		md_link_url,
		md_marker,
		md_bullet,
		md_quote,
	};

	// One styled run, starting at a byte offset in the line.
	struct text_block
	{
		int _char_pos;
		text_style _color;
	};

	// Fills buf with the runs for one line and returns the cookie the next line
	// starts from, so a multi-line construct such as a block comment can continue.
	//
	// The span is the bound: a highlighter never writes more runs than it is given
	// room for, and an empty span asks for the cookie alone.
	using highlight_fn = std::function<uint32_t(uint32_t cookie, std::string_view line_view,
	                                            std::span<text_block> buf, int& count)>;
}
