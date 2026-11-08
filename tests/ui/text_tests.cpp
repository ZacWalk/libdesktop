// Unit tests for libdesktop-ui's text vocabulary: the positions and styles the text layer shares, and the
// syntax highlighters. Standard C++ alone, like the library, so the suite runs wherever libdesktop
// builds.

#include "libdesktop/ui/syntax.h"
#include "test.h"

#include <cstdint>
#include <cstdio>
#include <string>

namespace
{
	void test_text_location()
	{

		// Ordering is by line first, then by byte offset within the line.
		assert_equal(true, (ui::text_location(0, 1) < ui::text_location(5, 2)), "ui::text_location(0, 1) < ui::text_location(5, 2)");
		assert_equal(true, (ui::text_location(2, 3) < ui::text_location(7, 3)), "ui::text_location(2, 3) < ui::text_location(7, 3)");
		assert_equal(true, (ui::text_location(4, 4) == ui::text_location(4, 4)), "ui::text_location(4, 4) == ui::text_location(4, 4)");
		assert_equal(true, (!(ui::text_location(9, 1) < ui::text_location(2, 1))), "!(ui::text_location(9, 1) < ui::text_location(2, 1))");

		// A backwards drag is still a valid selection; normalize orders it.
		const ui::text_selection backwards({8, 5}, {2, 1});
		assert_equal(true, (!backwards.empty()), "!backwards.empty()");
		const auto forwards = backwards.normalize();
		assert_equal(true, (forwards._start == ui::text_location(2, 1)), "forwards._start == ui::text_location(2, 1)");
		assert_equal(true, (forwards._end == ui::text_location(8, 5)), "forwards._end == ui::text_location(8, 5)");

		// Normalizing an ordered selection leaves it alone.
		const ui::text_selection ordered({1, 1}, {3, 3});
		assert_equal(true, (ordered.normalize() == ordered), "ordered.normalize() == ordered");

		// A caret is an empty selection at one point.
		const ui::text_selection caret(ui::text_location{4, 2});
		assert_equal(true, (caret.empty()), "caret.empty()");
		assert_equal(true, (caret.is_valid()), "caret.is_valid()");

		// Negative coordinates mark "no selection" rather than a position.
		assert_equal(true, (!ui::text_selection(-1, -1, -1, -1).is_valid()), "!ui::text_selection(-1, -1, -1, -1).is_valid()");
	}

	void test_syntax_language_from_extension()
	{
		namespace sx = ui::syntax;

		// With or without the leading dot, and case-insensitively.
		assert_equal(true, (sx::language_from_extension("cpp") == sx::language::cpp),
			"sx::language_from_extension(\"cpp\") == sx::language::cpp");
		assert_equal(true, (sx::language_from_extension(".cpp") == sx::language::cpp),
			"sx::language_from_extension(\".cpp\") == sx::language::cpp");
		assert_equal(true, (sx::language_from_extension(".H") == sx::language::cpp),
			"sx::language_from_extension(\".H\") == sx::language::cpp");
		assert_equal(true, (sx::language_from_extension("rs") == sx::language::rust),
			"sx::language_from_extension(\"rs\") == sx::language::rust");
		assert_equal(true, (sx::language_from_extension("py") == sx::language::python),
			"sx::language_from_extension(\"py\") == sx::language::python");
		assert_equal(true, (sx::language_from_extension("ps1") == sx::language::powershell),
			"sx::language_from_extension(\"ps1\") == sx::language::powershell");
		assert_equal(true, (sx::language_from_extension("psd1") == sx::language::powershell),
			"sx::language_from_extension(\"psd1\") == sx::language::powershell");

		// Anything unrecognised is plain, including nothing at all.
		assert_equal(true, (sx::language_from_extension("txt") == sx::language::plain),
			"sx::language_from_extension(\"txt\") == sx::language::plain");
		assert_equal(true, (sx::language_from_extension("") == sx::language::plain),
			"sx::language_from_extension(\"\") == sx::language::plain");
		assert_equal(true, (sx::language_from_extension(".") == sx::language::plain),
			"sx::language_from_extension(\".\") == sx::language::plain");
	}

	// Returns the style covering byte `at`, given the runs a highlighter produced.
	ui::text_style style_at(const ui::text_block* buf, const int count, const int at)
	{
		auto result = ui::text_style::normal_text;
		for (int i = 0; i < count; ++i)
			if (buf[i]._char_pos <= at) result = buf[i]._color;
		return result;
	}

	void test_syntax_cpp()
	{
		namespace sx = ui::syntax;
		const auto cpp = sx::for_language(sx::language::cpp);

		ui::text_block buf[64];
		int count = 0;

		// A keyword is distinguished from the identifier beside it.
		cpp(0, "int value = 1;", buf, count);
		assert_equal(true, (count > 0), "count > 0");
		assert_equal(true, (style_at(buf, count, 0) == ui::text_style::code_keyword),
			"style_at(buf, count, 0) == ui::text_style::code_keyword");
		assert_equal(true, (style_at(buf, count, 4) == ui::text_style::normal_text),
			"style_at(buf, count, 4) == ui::text_style::normal_text");

		// A line comment styles the rest of the line and does not carry over.
		count = 0;
		const auto after_line_comment = cpp(0, "x; // trailing", buf, count);
		assert_equal(true, (style_at(buf, count, 4) == ui::text_style::code_comment),
			"style_at(buf, count, 4) == ui::text_style::code_comment");
		assert_equal(true, (after_line_comment) == (0u), "after_line_comment == 0u");

		// A block comment carries its state to the next line through the cookie.
		count = 0;
		const auto open = cpp(0, "/* start", buf, count);
		assert_equal(true, (open != 0u), "open != 0u");
		count = 0;
		const auto still_open = cpp(open, "still inside", buf, count);
		assert_equal(true, (still_open != 0u), "still_open != 0u");
		assert_equal(true, (style_at(buf, count, 0) == ui::text_style::code_comment),
			"style_at(buf, count, 0) == ui::text_style::code_comment");
		count = 0;
		const auto closed = cpp(still_open, "done */ x;", buf, count);
		assert_equal(true, (closed) == (0u), "closed == 0u");

		// A preprocessor directive is its own style.
		count = 0;
		cpp(0, "#include <vector>", buf, count);
		assert_equal(true, (style_at(buf, count, 0) == ui::text_style::code_preprocessor),
			"style_at(buf, count, 0) == ui::text_style::code_preprocessor");

		// A string literal, and a number.
		count = 0;
		cpp(0, "s = \"text\";", buf, count);
		assert_equal(true, (style_at(buf, count, 5) == ui::text_style::code_string),
			"style_at(buf, count, 5) == ui::text_style::code_string");

		// An empty line is legal and produces no runs.
		count = 0;
		cpp(0, "", buf, count);
		assert_equal(true, (count) == (0), "count == 0");
	}

	void test_syntax_languages_differ()
	{
		namespace sx = ui::syntax;
		ui::text_block buf[64];
		int count = 0;

		// '#' starts a comment in Python but a directive in C++.
		const auto py = sx::for_language(sx::language::python);
		py(0, "# a comment", buf, count);
		assert_equal(true, (style_at(buf, count, 0) == ui::text_style::code_comment),
			"style_at(buf, count, 0) == ui::text_style::code_comment");

		count = 0;
		const auto cpp = sx::for_language(sx::language::cpp);
		cpp(0, "#define X 1", buf, count);
		assert_equal(true, (style_at(buf, count, 0) == ui::text_style::code_preprocessor),
			"style_at(buf, count, 0) == ui::text_style::code_preprocessor");

		// `fn` is a Rust keyword and nothing in Python.
		count = 0;
		const auto rs = sx::for_language(sx::language::rust);
		rs(0, "fn main() {}", buf, count);
		assert_equal(true, (style_at(buf, count, 0) == ui::text_style::code_keyword),
			"style_at(buf, count, 0) == ui::text_style::code_keyword");

		count = 0;
		py(0, "fn main()", buf, count);
		assert_equal(true, (style_at(buf, count, 0) == ui::text_style::normal_text),
			"style_at(buf, count, 0) == ui::text_style::normal_text");

		// PowerShell keywords are case-insensitive, unlike every other language here.
		count = 0;
		const auto ps = sx::for_language(sx::language::powershell);
		ps(0, "FOREACH ($x in $y)", buf, count);
		assert_equal(true, (style_at(buf, count, 0) == ui::text_style::code_keyword),
			"style_at(buf, count, 0) == ui::text_style::code_keyword");
	}

	void test_syntax_markdown()
	{
		namespace sx = ui::syntax;
		ui::text_block buf[64];
		int count = 0;

		// Heading level drives the style.
		sx::highlight_markdown(0, "# Title", buf, count);
		assert_equal(true, (style_at(buf, count, 2) == ui::text_style::md_heading1),
			"style_at(buf, count, 2) == ui::text_style::md_heading1");
		count = 0;
		sx::highlight_markdown(0, "## Title", buf, count);
		assert_equal(true, (style_at(buf, count, 3) == ui::text_style::md_heading2),
			"style_at(buf, count, 3) == ui::text_style::md_heading2");
		count = 0;
		sx::highlight_markdown(0, "### Title", buf, count);
		assert_equal(true, (style_at(buf, count, 4) == ui::text_style::md_heading3),
			"style_at(buf, count, 4) == ui::text_style::md_heading3");

		// A bullet marker is styled apart from the text after it.
		count = 0;
		sx::highlight_markdown(0, "- an item", buf, count);
		assert_equal(true, (style_at(buf, count, 0) == ui::text_style::md_bullet), "style_at(buf, count, 0) == ui::text_style::md_bullet");

		// Plain prose gets no heading styling.
		count = 0;
		sx::highlight_markdown(0, "ordinary text", buf, count);
		assert_equal(true, (style_at(buf, count, 0) == ui::text_style::normal_text),
			"style_at(buf, count, 0) == ui::text_style::normal_text");

		count = 0;
		sx::highlight_markdown(0, "", buf, count);
		assert_equal(true, (count) == (0), "count == 0");
	}

	void test_syntax_plain()
	{
		namespace sx = ui::syntax;
		const auto plain = sx::for_language(sx::language::plain);

		ui::text_block buf[64];
		int count = 0;

		// Plain text never claims a keyword, whatever it looks like.
		plain(0, "int class return #include", buf, count);
		assert_equal(true, (style_at(buf, count, 0) == ui::text_style::normal_text),
			"style_at(buf, count, 0) == ui::text_style::normal_text");
		assert_equal(true, (style_at(buf, count, 10) == ui::text_style::normal_text),
			"style_at(buf, count, 10) == ui::text_style::normal_text");
		assert_equal(true, (plain(0, "anything", buf, count)) == (0u), "plain(0, \"anything\", buf, count) == 0u");
	}

	void test_syntax_bounds()
	{
		namespace sx = ui::syntax;
		const auto cpp = sx::for_language(sx::language::cpp);

		// A line that would produce more runs than the buffer holds must stop at the
		// cap rather than write past it. Guard bytes either side catch an overrun.
		constexpr int cap = 64;
		struct guarded
		{
			uint64_t front = 0xFEEDFACEFEEDFACEull;
			ui::text_block blocks[cap]{};
			uint64_t back = 0xFEEDFACEFEEDFACEull;
		} g;

		std::string pathological;
		for (int i = 0; i < 500; ++i) pathological += "int x; ";

		int count = 0;
		cpp(0, pathological, g.blocks, count);

		assert_equal(true, (g.front) == (0xFEEDFACEFEEDFACEull), "g.front == 0xFEEDFACEFEEDFACEull");
		assert_equal(true, (g.back) == (0xFEEDFACEFEEDFACEull), "g.back == 0xFEEDFACEFEEDFACEull");
		assert_equal(true, (count >= 0), "count >= 0");

		// Runs come back in non-decreasing position order; a view walks them once.
		bool ordered = true;
		for (int i = 1; i < count; ++i)
			if (g.blocks[i]._char_pos < g.blocks[i - 1]._char_pos) ordered = false;
		assert_equal(true, (ordered), "ordered");
	}

	// Extensions and PowerShell's keywords are compared without regard to ASCII case, by the
	// highlighters' own comparison.
	void test_syntax_ascii_case()
	{
		namespace sx = ui::syntax;
		assert_equal(true, (sx::language_from_extension("PY") == sx::language::python),
			"sx::language_from_extension(\"PY\") == sx::language::python");
		assert_equal(true, (sx::language_from_extension(".Ps1") == sx::language::powershell),
			"sx::language_from_extension(\".Ps1\") == sx::language::powershell");
		assert_equal(true, (sx::language_from_extension("RS") == sx::language::rust),
			"sx::language_from_extension(\"RS\") == sx::language::rust");
		assert_equal(true, (sx::language_from_extension("CPP2") == sx::language::plain),
			"sx::language_from_extension(\"CPP2\") == sx::language::plain");

		const auto ps = sx::for_language(sx::language::powershell);
		ui::text_block buf[64];
		int count = 0;
		ps(0, "Function f { elseIF }", buf, count);
		assert_equal(true, (style_at(buf, count, 0) == ui::text_style::code_keyword),
			"style_at(buf, count, 0) == ui::text_style::code_keyword");
		assert_equal(true, (style_at(buf, count, 13) == ui::text_style::code_keyword),
			"style_at(buf, count, 13) == ui::text_style::code_keyword");
		count = 0;
		ps(0, "functions", buf, count);
		assert_equal(true, (style_at(buf, count, 0) == ui::text_style::normal_text),
			"style_at(buf, count, 0) == ui::text_style::normal_text");
	}
}


void register_text_tests(test_registry& tests)
{
	tests.add("test_text_location", test_text_location);
	tests.add("test_syntax_language_from_extension", test_syntax_language_from_extension);
	tests.add("test_syntax_cpp", test_syntax_cpp);
	tests.add("test_syntax_languages_differ", test_syntax_languages_differ);
	tests.add("test_syntax_markdown", test_syntax_markdown);
	tests.add("test_syntax_plain", test_syntax_plain);
	tests.add("test_syntax_bounds", test_syntax_bounds);
	tests.add("test_syntax_ascii_case", test_syntax_ascii_case);
}
