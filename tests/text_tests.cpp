// Unit tests for libdesktop-text: the position and style vocabulary the text layer shares, and the
// syntax highlighters. Standard C++ alone, like the library, so the suite runs wherever libdesktop
// builds.

#include "ui/syntax.h"
#include "check.h"

#include <cstdint>
#include <cstdio>
#include <string>

namespace
{
	void test_text_location()
	{
		using pf::ui::text_location;
		using pf::ui::text_selection;

		// Ordering is by line first, then by byte offset within the line.
		CHECK(text_location(0, 1) < text_location(5, 2));
		CHECK(text_location(2, 3) < text_location(7, 3));
		CHECK(text_location(4, 4) == text_location(4, 4));
		CHECK(!(text_location(9, 1) < text_location(2, 1)));

		// A backwards drag is still a valid selection; normalize orders it.
		const text_selection backwards({8, 5}, {2, 1});
		CHECK(!backwards.empty());
		const auto forwards = backwards.normalize();
		CHECK(forwards._start == text_location(2, 1));
		CHECK(forwards._end == text_location(8, 5));

		// Normalizing an ordered selection leaves it alone.
		const text_selection ordered({1, 1}, {3, 3});
		CHECK(ordered.normalize() == ordered);

		// A caret is an empty selection at one point.
		const text_selection caret(text_location{4, 2});
		CHECK(caret.empty());
		CHECK(caret.is_valid());

		// Negative coordinates mark "no selection" rather than a position.
		CHECK(!text_selection(-1, -1, -1, -1).is_valid());
	}

	void test_syntax_language_from_extension()
	{
		namespace sx = pf::ui::syntax;

		// With or without the leading dot, and case-insensitively.
		CHECK(sx::language_from_extension("cpp") == sx::language::cpp);
		CHECK(sx::language_from_extension(".cpp") == sx::language::cpp);
		CHECK(sx::language_from_extension(".H") == sx::language::cpp);
		CHECK(sx::language_from_extension("rs") == sx::language::rust);
		CHECK(sx::language_from_extension("py") == sx::language::python);
		CHECK(sx::language_from_extension("ps1") == sx::language::powershell);
		CHECK(sx::language_from_extension("psd1") == sx::language::powershell);

		// Anything unrecognised is plain, including nothing at all.
		CHECK(sx::language_from_extension("txt") == sx::language::plain);
		CHECK(sx::language_from_extension("") == sx::language::plain);
		CHECK(sx::language_from_extension(".") == sx::language::plain);
	}

	// Returns the style covering byte `at`, given the runs a highlighter produced.
	pf::ui::text_style style_at(const pf::ui::text_block* buf, const int count, const int at)
	{
		auto result = pf::ui::text_style::normal_text;
		for (int i = 0; i < count; ++i)
			if (buf[i]._char_pos <= at) result = buf[i]._color;
		return result;
	}

	void test_syntax_cpp()
	{
		namespace sx = pf::ui::syntax;
		const auto cpp = sx::for_language(sx::language::cpp);

		pf::ui::text_block buf[64];
		int count = 0;

		// A keyword is distinguished from the identifier beside it.
		cpp(0, "int value = 1;", buf, count);
		CHECK(count > 0);
		CHECK(style_at(buf, count, 0) == pf::ui::text_style::code_keyword);
		CHECK(style_at(buf, count, 4) == pf::ui::text_style::normal_text);

		// A line comment styles the rest of the line and does not carry over.
		count = 0;
		const auto after_line_comment = cpp(0, "x; // trailing", buf, count);
		CHECK(style_at(buf, count, 4) == pf::ui::text_style::code_comment);
		CHECK_EQ(after_line_comment, 0u);

		// A block comment carries its state to the next line through the cookie.
		count = 0;
		const auto open = cpp(0, "/* start", buf, count);
		CHECK(open != 0u);
		count = 0;
		const auto still_open = cpp(open, "still inside", buf, count);
		CHECK(still_open != 0u);
		CHECK(style_at(buf, count, 0) == pf::ui::text_style::code_comment);
		count = 0;
		const auto closed = cpp(still_open, "done */ x;", buf, count);
		CHECK_EQ(closed, 0u);

		// A preprocessor directive is its own style.
		count = 0;
		cpp(0, "#include <vector>", buf, count);
		CHECK(style_at(buf, count, 0) == pf::ui::text_style::code_preprocessor);

		// A string literal, and a number.
		count = 0;
		cpp(0, "s = \"text\";", buf, count);
		CHECK(style_at(buf, count, 5) == pf::ui::text_style::code_string);

		// An empty line is legal and produces no runs.
		count = 0;
		cpp(0, "", buf, count);
		CHECK_EQ(count, 0);
	}

	void test_syntax_languages_differ()
	{
		namespace sx = pf::ui::syntax;
		pf::ui::text_block buf[64];
		int count = 0;

		// '#' starts a comment in Python but a directive in C++.
		const auto py = sx::for_language(sx::language::python);
		py(0, "# a comment", buf, count);
		CHECK(style_at(buf, count, 0) == pf::ui::text_style::code_comment);

		count = 0;
		const auto cpp = sx::for_language(sx::language::cpp);
		cpp(0, "#define X 1", buf, count);
		CHECK(style_at(buf, count, 0) == pf::ui::text_style::code_preprocessor);

		// `fn` is a Rust keyword and nothing in Python.
		count = 0;
		const auto rs = sx::for_language(sx::language::rust);
		rs(0, "fn main() {}", buf, count);
		CHECK(style_at(buf, count, 0) == pf::ui::text_style::code_keyword);

		count = 0;
		py(0, "fn main()", buf, count);
		CHECK(style_at(buf, count, 0) == pf::ui::text_style::normal_text);

		// PowerShell keywords are case-insensitive, unlike every other language here.
		count = 0;
		const auto ps = sx::for_language(sx::language::powershell);
		ps(0, "FOREACH ($x in $y)", buf, count);
		CHECK(style_at(buf, count, 0) == pf::ui::text_style::code_keyword);
	}

	void test_syntax_markdown()
	{
		namespace sx = pf::ui::syntax;
		pf::ui::text_block buf[64];
		int count = 0;

		// Heading level drives the style.
		sx::highlight_markdown(0, "# Title", buf, count);
		CHECK(style_at(buf, count, 2) == pf::ui::text_style::md_heading1);
		count = 0;
		sx::highlight_markdown(0, "## Title", buf, count);
		CHECK(style_at(buf, count, 3) == pf::ui::text_style::md_heading2);
		count = 0;
		sx::highlight_markdown(0, "### Title", buf, count);
		CHECK(style_at(buf, count, 4) == pf::ui::text_style::md_heading3);

		// A bullet marker is styled apart from the text after it.
		count = 0;
		sx::highlight_markdown(0, "- an item", buf, count);
		CHECK(style_at(buf, count, 0) == pf::ui::text_style::md_bullet);

		// Plain prose gets no heading styling.
		count = 0;
		sx::highlight_markdown(0, "ordinary text", buf, count);
		CHECK(style_at(buf, count, 0) == pf::ui::text_style::normal_text);

		count = 0;
		sx::highlight_markdown(0, "", buf, count);
		CHECK_EQ(count, 0);
	}

	void test_syntax_plain()
	{
		namespace sx = pf::ui::syntax;
		const auto plain = sx::for_language(sx::language::plain);

		pf::ui::text_block buf[64];
		int count = 0;

		// Plain text never claims a keyword, whatever it looks like.
		plain(0, "int class return #include", buf, count);
		CHECK(style_at(buf, count, 0) == pf::ui::text_style::normal_text);
		CHECK(style_at(buf, count, 10) == pf::ui::text_style::normal_text);
		CHECK_EQ(plain(0, "anything", buf, count), 0u);
	}

	void test_syntax_bounds()
	{
		namespace sx = pf::ui::syntax;
		const auto cpp = sx::for_language(sx::language::cpp);

		// A line that would produce more runs than the buffer holds must stop at the
		// cap rather than write past it. Guard bytes either side catch an overrun.
		constexpr int cap = 64;
		struct guarded
		{
			uint64_t front = 0xFEEDFACEFEEDFACEull;
			pf::ui::text_block blocks[cap]{};
			uint64_t back = 0xFEEDFACEFEEDFACEull;
		} g;

		std::string pathological;
		for (int i = 0; i < 500; ++i) pathological += "int x; ";

		int count = 0;
		cpp(0, pathological, g.blocks, count);

		CHECK_EQ(g.front, 0xFEEDFACEFEEDFACEull);
		CHECK_EQ(g.back, 0xFEEDFACEFEEDFACEull);
		CHECK(count >= 0);

		// Runs come back in non-decreasing position order; a view walks them once.
		bool ordered = true;
		for (int i = 1; i < count; ++i)
			if (g.blocks[i]._char_pos < g.blocks[i - 1]._char_pos) ordered = false;
		CHECK(ordered);
	}

	// Extensions and PowerShell's keywords are compared without regard to ASCII case. The comparison
	// was libdesktop-core's until the highlighters became portable, and it is their own now.
	void test_syntax_ascii_case()
	{
		namespace sx = pf::ui::syntax;
		CHECK(sx::language_from_extension("PY") == sx::language::python);
		CHECK(sx::language_from_extension(".Ps1") == sx::language::powershell);
		CHECK(sx::language_from_extension("RS") == sx::language::rust);
		CHECK(sx::language_from_extension("CPP2") == sx::language::plain);

		const auto ps = sx::for_language(sx::language::powershell);
		pf::ui::text_block buf[64];
		int count = 0;
		ps(0, "Function f { elseIF }", buf, count);
		CHECK(style_at(buf, count, 0) == pf::ui::text_style::code_keyword);
		CHECK(style_at(buf, count, 13) == pf::ui::text_style::code_keyword);
		count = 0;
		ps(0, "functions", buf, count);
		CHECK(style_at(buf, count, 0) == pf::ui::text_style::normal_text);
	}
}

int main()
{
	test_text_location();
	test_syntax_language_from_extension();
	test_syntax_cpp();
	test_syntax_languages_differ();
	test_syntax_markdown();
	test_syntax_plain();
	test_syntax_bounds();
	test_syntax_ascii_case();

	std::printf("libdesktop-text tests: %s (%d checks, %d failures)\n",
	            g_failures == 0 ? "PASS" : "FAIL", g_checks, g_failures);
	return g_failures == 0 ? 0 : 1;
}
