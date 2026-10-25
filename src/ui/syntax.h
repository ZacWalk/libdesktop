// syntax.h — syntax highlighting for the text views.
//
// A highlighter turns one line into styled runs. It names a text_style rather than
// a colour, so the theme decides the presentation and a scanner never has to change
// when a palette does.
//
// Nothing here looks at a file path. Deciding which highlighter a document gets is
// the application's policy; language_from_extension is offered for the common case.

#pragma once

#include "platform.h"
#include "ui/text_types.h"

#include <span>

namespace pf::ui::syntax
{
	// The buffer size that always suffices for a line of this many bytes: a run can
	// start at every byte, and a zero-width run may share a position with the next.
	// Highlighters bound themselves by whatever span they are given, so a smaller
	// buffer is safe — it just truncates the styling of a pathological line.
	constexpr size_t blocks_for_line(const size_t line_bytes) { return line_bytes * 2 + 128; }

	enum class language
	{
		plain,
		markdown,
		cpp,
		rust,
		python,
		powershell,
	};

	// Fills buf with the runs for one line and returns the cookie the next line
	// starts from. Pass 0 as the cookie for the first line of a document.
	uint32_t highlight_text(uint32_t cookie, std::string_view line_view, std::span<text_block> buf, int& count);
	uint32_t highlight_markdown(uint32_t cookie, std::string_view line_view, std::span<text_block> buf, int& count);

	// A highlighter for the given language. `plain` yields one unstyled run.
	highlight_fn for_language(language lang);

    // Maps a file extension, with or without its leading dot, onto a language.
	// Unknown extensions are `plain`. This is a convenience, not a policy: an
	// application is free to choose differently.
	language language_from_extension(std::string_view ext);
}
