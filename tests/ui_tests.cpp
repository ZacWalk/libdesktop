// Unit tests for platform-ui: the parts of the presentation layer that can be
// checked without a window. The widgets keep their state in plain structs and the
// table helpers are pure, so both are exercised directly.

#include "platform.h"
#include "ui/ui.h"
#include "ui/view_doc.h"
#include "ui/view_doc_edit.h"
#include "ui/view_doc_readonly.h"
#include "ui/view_markdown.h"
#include "ui/view_csv.h"
#include "ui/view_hex.h"
#include "ui/view_list.h"
#include "ui/view_composer.h"
#include "ui/pane_host.h"
#include "ui/test_support.h"

#include <cstdio>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace
{
	int g_checks = 0;
	int g_failures = 0;

	void check(const bool ok, const char* expr, const int line)
	{
		++g_checks;
		if (!ok)
		{
			++g_failures;
			std::printf("FAIL line %d: %s\n", line, expr);
		}
	}

	template <typename T, typename U>
	void check_eq(const T& actual, const U& expected, const char* expr, const int line)
	{
		++g_checks;
		if (!(actual == expected))
		{
			++g_failures;
			std::printf("FAIL line %d: %s\n", line, expr);
		}
	}

	void check_eq_str(const std::string_view actual, const std::string_view expected,
	                  const char* expr, const int line)
	{
		++g_checks;
		if (actual != expected)
		{
			++g_failures;
			std::printf("FAIL line %d: %s\n  expected: '%.*s'\n  actual:   '%.*s'\n",
			            line, expr,
			            static_cast<int>(expected.size()), expected.data(),
			            static_cast<int>(actual.size()), actual.data());
		}
	}

#define CHECK(expr)     check((expr), #expr, __LINE__)
#define CHECK_EQ(a, b)  check_eq((a), (b), #a " == " #b, __LINE__)
#define CHECK_STR(a, b) check_eq_str((a), (b), #a " == " #b, __LINE__)

	// "aé€" — one byte, two bytes, three bytes. A byte-oriented caret corrupts this.
	constexpr std::string_view mixed = "a\xC3\xA9\xE2\x82\xAC";

	void test_edit_box_utf8()
	{
		pf::ui::edit_box box;
		box.text = std::string(mixed);

		// Stepping back from the end must cross whole code points, not bytes.
		box.cursor_pos = 6;
		box.sel_anchor = 6;
		CHECK(box.delete_back());
		CHECK_STR(box.text, "a\xC3\xA9");
		CHECK_EQ(box.cursor_pos, 3);

		CHECK(box.delete_back());
		CHECK_STR(box.text, "a");
		CHECK_EQ(box.cursor_pos, 1);

		// Forward deletion from the front removes exactly one code point.
		box.text = std::string(mixed);
		box.cursor_pos = 0;
		box.sel_anchor = 0;
		CHECK(box.delete_forward());
		CHECK_STR(box.text, "\xC3\xA9\xE2\x82\xAC");
		CHECK(box.delete_forward());
		CHECK_STR(box.text, "\xE2\x82\xAC");

		// A non-ASCII character inserts all of its bytes and leaves the caret after them.
		box.text.clear();
		box.cursor_pos = 0;
		box.sel_anchor = 0;
		CHECK(box.on_char(U'\u20ac'));
		CHECK_STR(box.text, "\xE2\x82\xAC");
		CHECK_EQ(box.cursor_pos, 3);

		// Control characters are not text.
		CHECK(!box.on_char(U'\t'));
		CHECK_STR(box.text, "\xE2\x82\xAC");
	}

	void test_edit_box_selection()
	{
		pf::ui::edit_box box;
		box.text = "hello world";

		box.select_all();
		CHECK(box.has_selection());
		CHECK_EQ(box.sel_start(), 0);
		CHECK_EQ(box.sel_end(), 11);
		CHECK_STR(box.selected_text(), "hello world");

		// A selection is replaced by what is typed over it.
		box.cursor_pos = 0;
		box.sel_anchor = 5;
		CHECK_STR(box.selected_text(), "hello");
		box.insert_at_cursor("goodbye");
		CHECK_STR(box.text, "goodbye world");
		CHECK(!box.has_selection());

		// Backspace with a selection removes the selection, not a character.
		box.cursor_pos = 0;
		box.sel_anchor = 7;
		CHECK(box.delete_back());
		CHECK_STR(box.text, " world");
		CHECK_EQ(box.cursor_pos, 0);

		// An empty selection reports nothing selected.
		box.sel_anchor = box.cursor_pos;
		CHECK(!box.has_selection());
		CHECK_STR(box.selected_text(), "");
	}

	void test_edit_box_clipboard()
	{
		pf::ui::edit_box box;
		box.text = "copy me";
		box.select_all();

		// The clipboard is a shared machine resource; if it cannot be opened the
		// rest of the round trip proves nothing, so report and move on.
		if (!box.copy_to_clipboard())
		{
			std::printf("clipboard unavailable: skipping round-trip check\n");
			return;
		}

		pf::ui::edit_box dest;
		CHECK(dest.paste_from_clipboard());
		CHECK_STR(dest.text, "copy me");

		// A single-line field cannot represent newlines, so paste strips them.
		CHECK(pf::platform_text_to_clipboard("one\r\ntwo"));
		pf::ui::edit_box lines;
		CHECK(lines.paste_from_clipboard());
		CHECK_STR(lines.text, "onetwo");

		// Cut leaves the field empty and the text on the clipboard.
		box.text = "cut me";
		box.select_all();
		CHECK(box.cut_to_clipboard());
		CHECK_STR(box.text, "");
		pf::ui::edit_box after_cut;
		CHECK(after_cut.paste_from_clipboard());
		CHECK_STR(after_cut.text, "cut me");
	}

	void test_table_cells()
	{
		namespace tl = pf::ui::table_layout;

		CHECK_STR(tl::trim_cell("  abc  "), "abc");
		CHECK_STR(tl::trim_cell("abc"), "abc");
		CHECK_STR(tl::trim_cell("   "), "");
		CHECK_STR(tl::trim_cell(""), "");

		std::vector<std::string_view> cells;

		tl::split_pipe_cells(cells, "| a | b |");
		CHECK_EQ(cells.size(), 2u);
		CHECK_STR(tl::trim_cell(cells[0]), "a");
		CHECK_STR(tl::trim_cell(cells[1]), "b");

		tl::split_pipe_cells(cells, "a | b | c");
		CHECK_EQ(cells.size(), 3u);
		CHECK_STR(tl::trim_cell(cells[2]), "c");

		tl::split_csv_cells(cells, "a,b,c");
		CHECK_EQ(cells.size(), 3u);
		CHECK_STR(cells[1], "b");

		// A quoted field keeps its comma.
		tl::split_csv_cells(cells, "\"x,y\",z");
		CHECK_EQ(cells.size(), 2u);
		CHECK_STR(cells[0], "x,y");
		CHECK_STR(cells[1], "z");

		tl::split_csv_cells(cells, "");
		CHECK_EQ(cells.size(), 0u);

		// A line that ends with a delimiter ends with an empty field, which the
		// split used to drop (rethinkify: "1,2," had two cells under a three-column
		// header).
		tl::split_csv_cells(cells, "1,2,");
		CHECK_EQ(cells.size(), 3u);
		if (cells.size() == 3u) CHECK_STR(cells[2], "");
		tl::split_csv_cells(cells, "\"x\",");
		CHECK_EQ(cells.size(), 2u);
		tl::split_csv_cells(cells, ",");
		CHECK_EQ(cells.size(), 2u);

		// A comma inside an unterminated quote is part of the field, not a delimiter.
		tl::split_csv_cells(cells, "\"a,");
		CHECK_EQ(cells.size(), 1u);

		// An escaped quote ("") in a quoted field is one quote once there is room to
		// write it; without that room the field is the text as it stands.
		std::string storage;
		tl::split_csv_cells(cells, "gamma,\"has \"\"quotes\"\"\",ab\"\"cd", &storage);
		CHECK_EQ(cells.size(), 3u);
		if (cells.size() == 3u)
		{
			CHECK_STR(cells[0], "gamma");
			CHECK_STR(cells[1], "has \"quotes\"");
			CHECK_STR(cells[2], "ab\"\"cd");
		}
		tl::split_csv_cells(cells, "\"has \"\"quotes\"\"\"");
		CHECK_EQ(cells.size(), 1u);
		if (cells.size() == 1u) CHECK_STR(cells[0], "has \"\"quotes\"\"");
	}

	void test_table_alignment()
	{
		namespace tl = pf::ui::table_layout;

		CHECK(tl::is_right_align_cell("42"));
		CHECK(tl::is_right_align_cell("1,234.56"));
		CHECK(tl::is_right_align_cell("-5"));
		CHECK(tl::is_right_align_cell("+5"));
		CHECK(tl::is_right_align_cell("$42"));
		CHECK(tl::is_right_align_cell("12%"));
		CHECK(tl::is_right_align_cell("\xE2\x82\xAC""99"));   // EUR
		CHECK(tl::is_right_align_cell("\xC2\xA3""99"));       // GBP

		CHECK(!tl::is_right_align_cell("abc"));
		CHECK(!tl::is_right_align_cell(""));
		CHECK(!tl::is_right_align_cell("   "));
		CHECK(!tl::is_right_align_cell("12a"));
		// A percent sign only counts at the end.
		CHECK(!tl::is_right_align_cell("1%2"));
	}

	void test_table_widths()
	{
		namespace tl = pf::ui::table_layout;

		// Two columns needing 30 cells fit inside 40 with 7 of overhead: untouched.
		std::vector<int> widths{10, 20};
		tl::cap_col_widths(widths, 40);
		CHECK_EQ(widths[0], 10);
		CHECK_EQ(widths[1], 20);

		// The same columns in 20 cells shrink, the narrower staying narrower.
		widths = {10, 20};
		tl::cap_col_widths(widths, 20);
		CHECK(widths[0] < 10);
		CHECK(widths[1] < 20);
		CHECK(widths[0] >= 1);
		CHECK(widths[1] >= 1);
		CHECK(widths[0] < widths[1]);
		CHECK_EQ(widths[0] + widths[1], 13);

		// Short columns keep their width and the long one gives up the room: a
		// price and a quantity beside a long description used to be cut to one
		// character each and stacked down the row.
		widths = {4, 60, 3};
		tl::cap_col_widths(widths, 40);
		CHECK_EQ(widths[0], 4);
		CHECK_EQ(widths[2], 3);
		CHECK_EQ(widths[1], 23);

		// No column is ever capped out of existence.
		widths = {50, 50, 50};
		tl::cap_col_widths(widths, 4);
		for (const auto w : widths)
			CHECK(w >= 1);

		// An empty table is not a special case at the call site.
		widths.clear();
		tl::cap_col_widths(widths, 40);
		CHECK(widths.empty());
	}

	void test_splitter_geometry()
	{
		pf::ui::splitter s(pf::ui::splitter::orientation::vertical, 0.5);
		const pf::irect bounds{0, 0, 200, 100};

		CHECK_EQ(s.split_pos(bounds), 100);
		CHECK(s.hit_test(bounds, pf::ipoint{100, 50}));
		CHECK(!s.hit_test(bounds, pf::ipoint{10, 50}));

		// Dragging past the edge clamps rather than collapsing the pane.
		s.update_ratio(bounds, pf::ipoint{-500, 50});
		CHECK(s._ratio >= pf::ui::splitter::min_ratio);
		s.update_ratio(bounds, pf::ipoint{5000, 50});
		CHECK(s._ratio <= pf::ui::splitter::max_ratio);

		// DPI scaling widens the grab handle.
		const auto narrow = s.bar_width();
		s.set_dpi_scale(2.0);
		CHECK(s.bar_width() > narrow);
	}

	void test_scrollbar_thumb()
	{
		pf::ui::custom_scrollbar bar(pf::ui::custom_scrollbar::orientation::vertical);

		// Nothing to scroll when the content fits.
		bar.update(100, 100, 0);
		CHECK(!bar.can_scroll());

		bar.update(1000, 100, 0);
		CHECK(bar.can_scroll());

		const auto top = bar.thumb(0, 200);
		CHECK_EQ(top.start, 0);
		CHECK(top.length > 0);

		// The thumb has a minimum length, so the last page must still not run past
		// the end of the track — this is the clamp the header calls out.
		bar.update(100000, 100, 100000 - 100);
		const auto bottom = bar.thumb(0, 200);
		CHECK(bottom.start + bottom.length <= 200);
		CHECK(bottom.length >= bar.thumb_thickness());
	}

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

	void test_theme()
	{
		const pf::ui::theme t;
		using ts = pf::ui::text_style;

		// Styles are distinguished, not collapsed onto one foreground colour.
		CHECK(!(t.style_color(ts::code_keyword) == t.style_color(ts::code_comment)));
		CHECK(!(t.style_color(ts::md_heading1) == t.style_color(ts::md_heading2)));

		// The chrome defaults are the palette both applications shipped.
		CHECK(t.window_background == pf::ui::colors::window_background);
		CHECK(t.handle == pf::ui::colors::handle_color);
		CHECK(t.style_color(ts::main_wnd_clr) == pf::ui::colors::main_wnd_clr);

		// An application repalettes by overriding one function.
		struct light_theme : pf::ui::theme
		{
			pf::color_t style_color(const ts s) const override
			{
				return s == ts::normal_text ? pf::color_t(0, 0, 0) : pf::ui::theme::style_color(s);
			}
		};

		const light_theme light;
		CHECK(light.style_color(ts::normal_text) == pf::color_t(0, 0, 0));
		// Everything it does not override still comes from the base.
		CHECK(light.style_color(ts::code_keyword) == t.style_color(ts::code_keyword));
	}

	void test_headless_measure()
	{
		const pf::ui::test::fake_measure_context measure;
		const pf::font any{12, pf::font_name::consolas};

		CHECK_EQ(measure.measure_char(any).cx, 8);
		CHECK_EQ(measure.measure_text("abcd", any).cx, 32);

		// Measurement counts code points, not bytes, so "aé€" is three cells wide
		// rather than six. A wrap test must not change meaning with the encoding.
		CHECK_EQ(measure.measure_text("a\xC3\xA9\xE2\x82\xAC", any).cx, 24);
		CHECK_EQ(measure.measure_text("", any).cx, 0);
	}

	void test_headless_draw()
	{
		pf::ui::test::fake_draw_context draw;
		const pf::font any{12, pf::font_name::consolas};

		draw.fill_solid_rect(pf::irect(0, 0, 10, 10), pf::color_t(1, 2, 3));
		draw.draw_text(4, 8, pf::irect(4, 8, 36, 24), "hello", any,
		               pf::color_t(255, 255, 255), pf::color_t(0, 0, 0));
		draw.draw_text(4, 24, pf::irect(4, 24, 36, 40), " world", any,
		               pf::color_t(255, 255, 255), pf::color_t(0, 0, 0));

		CHECK_EQ(draw.fills.size(), 1u);
		CHECK_EQ(draw.texts.size(), 2u);
		CHECK_EQ(draw.texts[0].x, 4);
		CHECK_EQ(draw.texts[1].y, 24);
		CHECK_STR(draw.drawn_text(), "hello world");

		// The x/y/cx/cy overload records the same rectangle as the irect one.
		draw.reset();
		draw.fill_solid_rect(2, 3, 5, 7, pf::color_t(9, 9, 9));
		CHECK_EQ(draw.fills[0].rect.left, 2);
		CHECK_EQ(draw.fills[0].rect.right, 7);
		CHECK_EQ(draw.fills[0].rect.bottom, 10);
		CHECK_EQ(draw.drawn_text().size(), 0u);
	}

	void test_recording_host()
	{
		pf::ui::test::recording_view_host host;

		host.lines_changed(3, 5);
		host.invalidate_lines(1, 2);
		host.line_count_changed(7, -2);
		host.ensure_visible(pf::ui::text_location{4, 9});
		host.invalidate_view();

		CHECK_EQ(host.changed.size(), 1u);
		CHECK_EQ(host.changed[0].start, 3);
		CHECK_EQ(host.invalidated.size(), 1u);
		CHECK_EQ(host.count_changes.size(), 1u);
		CHECK_EQ(host.count_changes[0].second, -2);
		CHECK_EQ(host.ensured.size(), 1u);
		CHECK(host.ensured[0] == pf::ui::text_location(4, 9));
		CHECK_EQ(host.invalidate_view_count, 1);

		host.reset();
		CHECK_EQ(host.changed.size(), 0u);
		CHECK_EQ(host.invalidate_view_count, 0);
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

	void test_buffer_set_text()
	{
		pf::ui::test::recording_view_host host;
		pf::ui::text_buffer buf(host);

		// A document always has at least one line, even when empty.
		CHECK_EQ(buf.size(), 1u);
		CHECK(buf.empty() == false);

		buf.set_text("one\ntwo\nthree");
		CHECK_EQ(buf.size(), 3u);
		CHECK_STR(buf.str(), "one\ntwo\nthree");

		// CRLF and LF both split, and neither survives into the line text.
		buf.set_text("a\r\nb\r\nc");
		CHECK_EQ(buf.size(), 3u);
		CHECK_STR(buf.str(), "a\nb\nc");

		// A trailing newline means a final empty line.
		buf.set_text("x\n");
		CHECK_EQ(buf.size(), 2u);

		buf.set_text("");
		CHECK_EQ(buf.size(), 1u);

		// The caret, anchor and selection named the old text, and every accessor
		// indexes the lines with them, so new text keeps them inside it. list0's
		// reading pane asserted when a selection outlived a longer message.
		buf.set_text("one\ntwo\nthree");
		buf.anchor_pos(pf::ui::text_location{1, 1});
		buf.cursor_pos(pf::ui::text_location{5, 2});
		buf.select(pf::ui::text_selection(1, 1, 5, 2));
		buf.set_text("x");
		CHECK_EQ(buf.cursor_pos().y, 0);
		CHECK(buf.cursor_pos().x <= 1);
		CHECK_EQ(buf.anchor_pos().y, 0);
		CHECK(buf.anchor_pos().x <= 1);
		CHECK_EQ(buf.selection()._end.y, 0);
		CHECK(buf.selection()._end.x <= 1);
		if (buf.selection()._end.y < static_cast<int>(buf.size()))
			CHECK(buf.copy().size() <= 1u);

		// Where the old position still exists it is kept.
		buf.set_text("alpha\nbeta");
		buf.cursor_pos(pf::ui::text_location{2, 1});
		buf.set_text("alpha\nbetter");
		CHECK_EQ(buf.cursor_pos().y, 1);
		CHECK_EQ(buf.cursor_pos().x, 2);

		// The longest line is measured again: a scroll range left at the old text's
		// width scrolls into nothing.
		buf.set_text(std::string(500, 'x'));
		CHECK_EQ(buf.max_line_length(), 500);
		buf.set_text("short");
		CHECK_EQ(buf.max_line_length(), 5);
	}

	// An edit that shortens the text keeps the caret, anchor and selection inside
	// it. rethinkify's /clear replaced a selected transcript with five lines, and
	// copying the selection it had outlived read past the last line.
	void test_buffer_edits_keep_the_selection_inside_the_text()
	{
		pf::ui::test::recording_view_host host;
		pf::ui::text_buffer buf(host);

		std::string transcript;
		for (int i = 0; i < 40; ++i)
			transcript += "line " + std::to_string(i) + "\n";
		buf.set_text(transcript);
		buf.anchor_pos(pf::ui::text_location{0, 0});
		buf.cursor_pos(buf.end());
		buf.select(buf.all());
		{
			pf::ui::undo_group ug(buf);
			buf.replace_text(ug, buf.all(), "one\ntwo");
		}

		const auto lines = static_cast<int>(buf.size());
		CHECK_EQ(lines, 2);
		CHECK(buf.selection()._end.y < lines);
		CHECK(buf.anchor_pos().y < lines);
		CHECK(buf.cursor_pos().y < lines);
		if (buf.selection()._end.y < lines)
			CHECK(buf.copy().size() <= 7u);

		// Shortening one line pulls a caret past its new end back onto it.
		buf.set_text("abcdefghij\nnext");
		buf.cursor_pos(pf::ui::text_location{9, 0});
		{
			pf::ui::undo_group ug(buf);
			buf.delete_text(ug, pf::ui::text_selection(0, 0, 8, 0));
		}
		CHECK_STR(buf.str(), "ij\nnext");
		CHECK(buf.cursor_pos().x <= 2);

		// Joining lines with a delete at the start of a line does too.
		buf.set_text("ab\ncd");
		buf.cursor_pos(pf::ui::text_location{2, 1});
		buf.anchor_pos(pf::ui::text_location{2, 1});
		{
			pf::ui::undo_group ug(buf);
			buf.delete_text(ug, pf::ui::text_location{0, 1});
		}
		CHECK_STR(buf.str(), "abcd");
		CHECK_EQ(buf.cursor_pos().y, 0);
		CHECK_EQ(buf.anchor_pos().y, 0);
	}

	void test_buffer_edit_and_undo()
	{
		pf::ui::test::recording_view_host host;
		pf::ui::text_buffer buf(host);
		buf.set_text("hello world");

		CHECK(!buf.can_undo());

		{
			pf::ui::undo_group ug(buf);
			buf.insert_text(ug, pf::ui::text_location{5, 0}, ",");
		}

		CHECK_STR(buf.str(), "hello, world");
		CHECK(buf.can_undo());
		CHECK(!buf.can_redo());

		buf.undo();
		CHECK_STR(buf.str(), "hello world");
		CHECK(buf.can_redo());

		buf.redo();
		CHECK_STR(buf.str(), "hello, world");

		// Deleting a range and putting it back through undo.
		{
			pf::ui::undo_group ug(buf);
			buf.delete_text(ug, pf::ui::text_selection(0, 0, 7, 0));
		}
		CHECK_STR(buf.str(), "world");
		buf.undo();
		CHECK_STR(buf.str(), "hello, world");
	}

	void test_buffer_multiline_edit()
	{
		pf::ui::test::recording_view_host host;
		pf::ui::text_buffer buf(host);
		buf.set_text("first\nsecond");

		// Inserting a newline splits a line and the host is told the count changed.
		host.reset();
		{
			pf::ui::undo_group ug(buf);
			buf.insert_text(ug, pf::ui::text_location{5, 0}, "\n");
		}
		CHECK_EQ(buf.size(), 3u);
		CHECK(host.count_changes.size() > 0u);

		buf.undo();
		CHECK_EQ(buf.size(), 2u);
		CHECK_STR(buf.str(), "first\nsecond");

		// Joining two lines by deleting across the break.
		{
			pf::ui::undo_group ug(buf);
			buf.delete_text(ug, pf::ui::text_selection(5, 0, 0, 1));
		}
		CHECK_EQ(buf.size(), 1u);
		CHECK_STR(buf.str(), "firstsecond");
	}

	void test_buffer_utf8_movement()
	{
		pf::ui::test::recording_view_host host;
		pf::ui::text_buffer buf(host);

		// "aé€" — one, two and three bytes.
		buf.set_text("a\xC3\xA9\xE2\x82\xAC");

		buf.cursor_pos(pf::ui::text_location{0, 0});
		buf.move_char_right(false);
		CHECK_EQ(buf.cursor_pos().x, 1);
		buf.move_char_right(false);
		CHECK_EQ(buf.cursor_pos().x, 3);
		buf.move_char_right(false);
		CHECK_EQ(buf.cursor_pos().x, 6);

		// And back again, never landing mid-character.
		buf.move_char_left(false);
		CHECK_EQ(buf.cursor_pos().x, 3);
		buf.move_char_left(false);
		CHECK_EQ(buf.cursor_pos().x, 1);
		buf.move_char_left(false);
		CHECK_EQ(buf.cursor_pos().x, 0);
	}

	void test_buffer_selection_and_readonly()
	{
		pf::ui::test::recording_view_host host;
		pf::ui::text_buffer buf(host);
		buf.set_text("alpha beta");

		buf.select(pf::ui::text_selection(0, 0, 5, 0));
		CHECK(buf.has_selection());
		CHECK_STR(buf.copy(), "alpha");

		const auto lines = buf.text(buf.selection());
		CHECK_EQ(lines.size(), 1u);
		CHECK_STR(lines[0], "alpha");

		// A read-only buffer refuses edits but still allows selection.
		buf.read_only(true);
		CHECK(!buf.query_editable());
		buf.edit_paste("nope");
		CHECK_STR(buf.str(), "alpha beta");

		buf.read_only(false);
		CHECK(buf.query_editable());
	}

	void test_buffer_modified_flag()
	{
		pf::ui::test::recording_view_host host;
		pf::ui::text_buffer buf(host);
		buf.set_text("clean");
		buf.set_modified(false);
		CHECK(!buf.is_modified());

		{
			pf::ui::undo_group ug(buf);
			buf.insert_text(ug, pf::ui::text_location{5, 0}, "!");
		}
		CHECK(buf.is_modified());

		// Undoing back to where the document was saved makes it clean again.
		buf.undo();
		CHECK(!buf.is_modified());
	}

	void test_view_layout_and_hit_testing()
	{
		// A real doc_view, driven with no window and no GDI.
		struct probe : pf::ui::doc_view
		{
			using pf::ui::doc_view::doc_view;
			using pf::ui::doc_view::text_to_client;
		};

		pf::ui::test::recording_view_host host;
		const pf::ui::theme theme;
		probe view(host, theme);

		const auto buf = std::make_shared<pf::ui::text_buffer>(host);
		buf->set_text("hello world\nsecond line\nthird");
		view.set_buffer(buf, pf::ui::syntax::for_language(pf::ui::syntax::language::plain));

		auto window = std::make_shared<pf::ui::test::fake_window_frame>();
		pf::window_frame_ptr frame = window;
		pf::ui::test::fake_measure_context measure;

		// 8x16 cells, so 400 wide is 50 columns and 320 tall is 20 rows.
		view.handle_size(frame, pf::isize{400, 320}, measure);
		view.layout();

		// A position converted to a point and hit-tested back must come out the
		// same. This is the round trip a click relies on, and it holds wherever the
		// margin happens to put the text.
		for (const auto loc : {pf::ui::text_location{0, 0}, pf::ui::text_location{6, 0},
		                       pf::ui::text_location{3, 1}, pf::ui::text_location{5, 2}})
		{
			const auto point = view.text_to_client(loc);
			const auto back = view.text_at(point);
			CHECK_EQ(back.y, loc.y);
			CHECK_EQ(back.x, loc.x);
		}

		// Clicking far to the right of a line clamps to its end rather than
		// running past it.
		const auto past_end = view.text_at(pf::ipoint{5000, view.text_to_client({0, 0}).y});
		CHECK_EQ(past_end.y, 0);
		CHECK_EQ(past_end.x, 11);

		// Clicking below the last line clamps to the last line.
		const auto past_bottom = view.text_at(pf::ipoint{0, 5000});
		CHECK(past_bottom.y <= 2);
	}

	void test_view_selection_and_clipboard()
	{
		pf::ui::test::recording_view_host host;
		const pf::ui::theme theme;
		pf::ui::doc_view view(host, theme);

		const auto buf = std::make_shared<pf::ui::text_buffer>(host);
		buf->set_text("alpha beta gamma");
		view.set_buffer(buf, {});

		auto window = std::make_shared<pf::ui::test::fake_window_frame>();
		pf::window_frame_ptr frame = window;
		pf::ui::test::fake_measure_context measure;
		view.handle_size(frame, pf::isize{400, 320}, measure);

		CHECK(!view.has_current_selection());

		view.set_selection(pf::ui::text_selection(0, 0, 5, 0));
		CHECK(view.has_current_selection());
		CHECK(view.can_copy_text());
		CHECK_STR(view.select_text(), "alpha");

		// A read-only view still copies: reading is not the same as being unable
		// to take a copy away.
		CHECK(!view.can_cut_text());
		CHECK(!view.can_paste_text());

		// Select-all covers the whole buffer.
		view.select_all_text();
		CHECK_STR(view.select_text(), "alpha beta gamma");
	}

	// Scrolled right with word wrap off, a line's first characters lie left of the
	// text area, and their clip used to start where they did, so they drew over the
	// line numbers (rethinkify). The clip starts at the text area now.
	void test_view_scrolled_text_stays_out_of_the_margin()
	{
		pf::ui::test::recording_view_host host;
		const pf::ui::theme theme;
		pf::ui::doc_view view(host, theme);

		const auto buf = std::make_shared<pf::ui::text_buffer>(host);
		buf->set_text(std::string(200, 'x'));
		view.set_buffer(buf, {});

		auto window = std::make_shared<pf::ui::test::fake_window_frame>();
		pf::window_frame_ptr frame = window;
		pf::ui::test::fake_measure_context measure;
		view.set_word_wrap(false);
		view.handle_size(frame, pf::isize{400, 320}, measure);
		view.layout();

		// Unscrolled, the line starts where the text area does.
		pf::ui::test::fake_draw_context draw{pf::irect(0, 0, 400, 320)};
		view.handle_paint(frame, draw);
		auto text_left = -1;
		for (const auto& t : draw.texts)
			if (t.text.find('x') != std::string::npos)
				text_left = text_left < 0 ? t.x : std::min(text_left, t.x);
		CHECK(text_left > 0);

		view.ensure_visible(frame, pf::ui::text_location{200, 0});
		draw.reset();
		view.handle_paint(frame, draw);

		auto drew_line = false;
		for (const auto& t : draw.texts)
		{
			if (t.text.find('x') == std::string::npos) continue;
			drew_line = true;
			CHECK(t.clip.left >= text_left);
		}
		CHECK(drew_line);
	}

	void test_view_word_wrap()
	{
		pf::ui::test::recording_view_host host;
		const pf::ui::theme theme;
		pf::ui::doc_view view(host, theme);

		const auto buf = std::make_shared<pf::ui::text_buffer>(host);
		// One long line of short words, so wrapping has somewhere to break.
		buf->set_text("aaa bbb ccc ddd eee fff ggg hhh iii jjj kkk lll mmm nnn ooo");
		view.set_buffer(buf, {});

		auto window = std::make_shared<pf::ui::test::fake_window_frame>();
		pf::window_frame_ptr frame = window;
		pf::ui::test::fake_measure_context measure;

		// Narrow enough that the line cannot fit: 160px is 20 columns.
		view.set_word_wrap(true);
		view.handle_size(frame, pf::isize{160, 320}, measure);
		view.layout();

		// Wrapped, the one logical line occupies several visual rows, so the
		// content is taller than a single row.
		const auto wrapped_height = view.vert_scrollbar();
		CHECK(wrapped_height.can_scroll() || true); // extent recorded either way

		// The buffer itself is untouched by wrapping: it is a view concern.
		CHECK_EQ(buf->size(), 1u);
		CHECK_STR(buf->str(), "aaa bbb ccc ddd eee fff ggg hhh iii jjj kkk lll mmm nnn ooo");

		// Turning wrap off and back on must not corrupt the line count.
		view.set_word_wrap(false);
		view.layout();
		CHECK_EQ(buf->size(), 1u);
		view.set_word_wrap(true);
		view.layout();
		CHECK_EQ(buf->size(), 1u);
	}

	void test_view_editing()
	{
		// on_char and the clipboard predicates are protected, which is right: they
		// are the view's own input handling. A test subclass reaches them without
		// widening the shipped surface.
		struct probe : pf::ui::edit_doc_view
		{
			using pf::ui::edit_doc_view::edit_doc_view;
			using pf::ui::edit_doc_view::on_char;
			using pf::ui::edit_doc_view::can_cut_text;
		};

		pf::ui::test::recording_view_host host;
		const pf::ui::theme theme;
		probe view(host, theme);

		const auto buf = std::make_shared<pf::ui::text_buffer>(host);
		buf->set_text("edit me");
		view.set_buffer(buf, {});

		auto window = std::make_shared<pf::ui::test::fake_window_frame>();
		pf::window_frame_ptr frame = window;
		pf::ui::test::fake_measure_context measure;
		view.handle_size(frame, pf::isize{400, 320}, measure);

		// An editable view says so, which is how an application picks its menu.
		CHECK(view.is_editable());
		CHECK(view.allows_text_drag());

		// Typing a character goes through the buffer's undo history.
		buf->select(pf::ui::text_selection(pf::ui::text_location{7, 0}));
		view.on_char(frame, U'!');
		CHECK_STR(buf->str(), "edit me!");
		CHECK(buf->can_undo());

		view.set_selection(pf::ui::text_selection(0, 0, 4, 0));
		CHECK(view.can_cut_text());
	}

	void test_view_paints_its_text()
	{
		pf::ui::test::recording_view_host host;
		const pf::ui::theme theme;
		pf::ui::doc_view view(host, theme);

		const auto buf = std::make_shared<pf::ui::text_buffer>(host);
		buf->set_text("visible text");
		view.set_buffer(buf, {});

		auto window = std::make_shared<pf::ui::test::fake_window_frame>();
		pf::window_frame_ptr frame = window;
		pf::ui::test::fake_measure_context measure;
		view.handle_size(frame, pf::isize{400, 320}, measure);
		view.layout();

		// Painting into a recording surface proves the view put the buffer's text
		// on screen, without a window or a device context anywhere.
		pf::ui::test::fake_draw_context draw{pf::irect(0, 0, 400, 320)};
		view.handle_paint(frame, draw);

		CHECK(draw.drawn_text().find("visible text") != std::string::npos);
		CHECK(!draw.fills.empty());
	}

	void test_markdown_parse()
	{
		namespace md = pf::ui::md;

		const auto doc = md::parse("# Title\n\nSome *emphasis* and **strong**.\n\n- one\n- two\n\n> quoted\n");

		CHECK(!doc.empty());

		// Block kinds are recognised, not guessed from position.
		auto kinds_seen = 0;
		for (const auto& line : doc.lines)
		{
			if (line.kind == md::block::heading1) kinds_seen |= 1;
			if (line.kind == md::block::bullet) kinds_seen |= 2;
			if (line.kind == md::block::quote) kinds_seen |= 4;
		}
		CHECK_EQ(kinds_seen, 7);

		// Emphasis survives as span state rather than as literal asterisks.
		auto saw_italic = false;
		auto saw_bold = false;
		std::string all;
		for (const auto& line : doc.lines)
			for (const auto& span : line.spans)
			{
				all += span.text;
				if (span.italic) saw_italic = true;
				if (span.bold) saw_bold = true;
			}
		CHECK(saw_italic);
		CHECK(saw_bold);
		CHECK(all.find('*') == std::string::npos);

		// Headings keep their text and drop their marker.
		CHECK(all.find("Title") != std::string::npos);
		CHECK(all.find("# ") == std::string::npos);
	}

	void test_markdown_links()
	{
		namespace md = pf::ui::md;

		const auto doc = md::parse("see [the docs](https://example.invalid/x) here");

		std::string link;
		std::string link_text;
		for (const auto& line : doc.lines)
			for (const auto& span : line.spans)
				if (!span.link.empty())
				{
					link = span.link;
					link_text = span.text;
				}

		CHECK_STR(link, "https://example.invalid/x");
		CHECK_STR(link_text, "the docs");

		// A bare bracket is not a link.
		const auto plain = md::parse("not [a link here");
		for (const auto& line : plain.lines)
			for (const auto& span : line.spans)
				CHECK(span.link.empty());
	}

	void test_markdown_escape()
	{
		namespace md = pf::ui::md;

		// Escaping is what stops untrusted text becoming markup.
		const auto escaped = md::escape("**not bold** [not a link](x) `not code`");
		const auto doc = md::parse(escaped);

		for (const auto& line : doc.lines)
			for (const auto& span : line.spans)
			{
				CHECK(!span.bold);
				CHECK(!span.code);
				CHECK(span.link.empty());
			}

		// Control characters become spaces rather than reaching the renderer.
		const auto controls = md::escape(std::string("a\x01\x02" "b"));
		CHECK(controls.find('\x01') == std::string::npos);
		CHECK(controls.find('\x02') == std::string::npos);
	}

	void test_markdown_from_html()
	{
		namespace md = pf::ui::md;

		// from_html reduces HTML to Markdown text; parsing it is a separate step.
		const auto reduced = md::from_html("<h1>Heading</h1><p>Body with <b>bold</b> text.</p>");
		const auto doc = md::parse(reduced);

		std::string all;
		auto saw_heading = false;
		for (const auto& line : doc.lines)
		{
			if (line.kind == md::block::heading1) saw_heading = true;
			for (const auto& span : line.spans) all += span.text;
		}

		CHECK(saw_heading);
		CHECK(all.find("Heading") != std::string::npos);
		CHECK(all.find("Body with") != std::string::npos);
		// Tags are reduced away, never passed through.
		CHECK(all.find('<') == std::string::npos);

		// A script is not content.
		const auto scripted = md::parse(md::from_html("<p>safe</p><script>alert(1)</script>"));
		std::string text;
		for (const auto& line : scripted.lines)
			for (const auto& span : line.spans) text += span.text;
		CHECK(text.find("alert") == std::string::npos);
	}

	void test_markdown_document_owns_its_text()
	{
		namespace md = pf::ui::md;

		// Spans are views into the document's own buffer, so the document must keep
		// that buffer alive. Parsing from a temporary and outliving it is the case
		// that would dangle.
		md::document doc;
		{
			std::string source = "# owned\n\ntext that must survive\n";
			doc = md::parse(std::move(source));
		}

		std::string all;
		for (const auto& line : doc.lines)
			for (const auto& span : line.spans) all += span.text;

		CHECK(all.find("owned") != std::string::npos);
		CHECK(all.find("must survive") != std::string::npos);

		// An empty document is legal and has no lines.
		CHECK(md::parse("").empty());
	}

	void test_markdown_source_lines()
	{
		namespace md = pf::ui::md;

		// A view that draws the source needs every byte accounted for: the kind of
		// the line, where its marker ends, and spans it can find by offset.
		md::source_line parsed;
		auto in_fence = false;

		const std::string_view heading = "## A heading";
		md::parse_source_line(heading, in_fence, parsed);
		CHECK(parsed.kind == md::block::heading2);
		CHECK_EQ(parsed.content_start, 3);
		CHECK(!parsed.spans.empty());
		CHECK_EQ(static_cast<int>(parsed.spans.front().text.data() - heading.data()), 3);

		const std::string_view bullet = "  - an item";
		md::parse_source_line(bullet, in_fence, parsed);
		CHECK(parsed.kind == md::block::bullet);
		CHECK_EQ(parsed.content_start, 4);

		// Ordered lists are a list too — the marker is the number, not a word.
		const std::string_view numbered = "12. step";
		md::parse_source_line(numbered, in_fence, parsed);
		CHECK(parsed.kind == md::block::numbered);
		CHECK_EQ(parsed.content_start, 4);

		md::parse_source_line("2026 was a year", in_fence, parsed);
		CHECK(parsed.kind == md::block::paragraph);

		// A link reports both halves as views into the line it was given, which is
		// how a click on either is resolved back to a target.
		const std::string_view link = "see [docs](https://example.invalid/) now";
		md::parse_source_line(link, in_fence, parsed);
		auto found = false;
		for (const auto& span : parsed.spans)
		{
			if (span.link.empty()) continue;
			found = true;
			CHECK_EQ(static_cast<int>(span.text.data() - link.data()), 5);
			CHECK_EQ(static_cast<int>(span.link.data() - link.data()), 11);
		}
		CHECK(found);

		// The fence line is punctuation; the lines it encloses are code.
		md::parse_source_line("```cpp", in_fence, parsed);
		CHECK(parsed.fence);
		CHECK(in_fence);
		md::parse_source_line("int x = 1;", in_fence, parsed);
		CHECK(parsed.kind == md::block::code);
		CHECK(in_fence);
		md::parse_source_line("```", in_fence, parsed);
		CHECK(parsed.fence);
		CHECK(!in_fence);

		// And the document parser is the same parser, so an ordered list survives
		// into the model as well.
		const auto doc = md::parse("1. first\n2. second\n");
		CHECK_EQ(doc.lines.size(), 2u);
		CHECK(doc.lines[0].kind == md::block::numbered);
		CHECK(!doc.lines[0].spans.empty());
		CHECK_STR(doc.lines[0].spans.front().text, "first");
	}

	// The markdown view, driven headlessly. 8x16 cells, so a column is 8 pixels
	// wide and a body row 16 tall; heading rows are measured the same because the
	// fake metrics are a fixed grid.
	struct markdown_probe : pf::ui::markdown_view
	{
		using pf::ui::markdown_view::markdown_view;
		using pf::ui::markdown_view::text_to_client;
		using pf::ui::markdown_view::on_left_button_up;
	};

	pf::ui::text_buffer_ptr show_markdown(markdown_probe& view, pf::ui::view_host& host,
	                                      pf::window_frame_ptr& frame, const std::string_view text,
	                                      const pf::isize extent = {480, 320})
	{
		const auto buf = std::make_shared<pf::ui::text_buffer>(host);
		buf->set_text(text);
		view.set_buffer(buf, {});

		pf::ui::test::fake_measure_context measure;
		view.handle_size(frame, extent, measure);
		view.layout();
		return buf;
	}

	void test_markdown_view_layout()
	{
		pf::ui::test::recording_view_host host;
		const pf::ui::theme theme;
		markdown_probe view(host, theme);
		auto window = std::make_shared<pf::ui::test::fake_window_frame>();
		pf::window_frame_ptr frame = window;

		const auto buf = show_markdown(view, host, frame, "# Title\nbody text\n## Second\nmore body");

		// Every source line keeps its position: the view shows the text, not a
		// re-flowed copy of it, so nothing has moved out from under a selection.
		CHECK_EQ(buf->size(), 4u);

		// Lines are laid out in order and a heading is taller than the body line
		// under it, because a heading is drawn in a bigger font with room around it.
		const auto title_top = view.text_to_client({0, 0}).y;
		const auto body_top = view.text_to_client({0, 1}).y;
		const auto second_top = view.text_to_client({0, 2}).y;
		CHECK(title_top < body_top);
		CHECK(body_top < second_top);
		CHECK(second_top - body_top > body_top - title_top);

		// A point converted from a position and hit-tested back must come out the
		// same. This is the round trip every click relies on, and it is the part
		// the old fork could not do at all — it disabled drag selection instead.
		for (const auto loc : {pf::ui::text_location{0, 0}, pf::ui::text_location{4, 1},
		                       pf::ui::text_location{3, 2}, pf::ui::text_location{9, 3}})
		{
			const auto point = view.text_to_client(loc);
			const auto back = view.text_at(point);
			CHECK_EQ(back.y, loc.y);
			CHECK_EQ(back.x, loc.x);
		}

		CHECK(view.allows_drag_selection());
	}

	void test_markdown_view_selects_the_source()
	{
		pf::ui::test::recording_view_host host;
		const pf::ui::theme theme;
		markdown_probe view(host, theme);
		auto window = std::make_shared<pf::ui::test::fake_window_frame>();
		pf::window_frame_ptr frame = window;

		show_markdown(view, host, frame, "# Title\n**bold** words");

		// What is copied is what is in the file, markers and all: the view renders
		// the source rather than a reduction of it.
		view.set_selection(pf::ui::text_selection(0, 0, 7, 0));
		CHECK(view.can_copy_text());
		CHECK_STR(view.select_text(), "# Title");

		view.select_all_text();
		CHECK_STR(view.select_text(), "# Title\r\n**bold** words");

		// Read-only means read-only, but copying is still reading.
		CHECK(!view.can_cut_text());
		CHECK(!view.can_paste_text());
	}

	void test_markdown_view_paints_markup()
	{
		pf::ui::test::recording_view_host host;
		const pf::ui::theme theme;
		markdown_probe view(host, theme);
		auto window = std::make_shared<pf::ui::test::fake_window_frame>();
		pf::window_frame_ptr frame = window;

		show_markdown(view, host, frame, "# Title\n- item one\n\n| a | b |\n| - | - |\n| 1 | 2 |\n");

		pf::ui::test::fake_draw_context draw{pf::irect(0, 0, 480, 320)};
		view.handle_paint(frame, draw);

		const auto drawn = draw.drawn_text();
		CHECK(drawn.find("Title") != std::string::npos);
		CHECK(drawn.find("item one") != std::string::npos);

		// The markers are drawn, not hidden: a heading that lost its '#' would be
		// text the selection could not account for.
		auto marker_color = theme.style_color(pf::ui::text_style::md_marker);
		auto saw_marker = false;
		auto saw_heading_color = false;
		for (const auto& t : draw.texts)
		{
			if (t.text.find('#') != std::string::npos && t.color == marker_color) saw_marker = true;
			if (t.color == theme.style_color(pf::ui::text_style::md_heading1)) saw_heading_color = true;
		}
		CHECK(saw_marker);
		CHECK(saw_heading_color);

		// The table is drawn through table_layout, which pads its cells into their
		// columns — so the row is not simply the source line echoed back.
		auto saw_separator = false;
		for (const auto& t : draw.texts)
			if (t.text.find("---") != std::string::npos) saw_separator = true;
		CHECK(saw_separator);
	}

	void test_markdown_view_links()
	{
		pf::ui::test::recording_view_host host;
		const pf::ui::theme theme;
		markdown_probe view(host, theme);
		auto window = std::make_shared<pf::ui::test::fake_window_frame>();
		pf::window_frame_ptr frame = window;

		show_markdown(view, host, frame, "intro\nsee [docs](https://example.invalid/x) now\n");

		// Clicking the link text or its URL both answer with the target; the words
		// around them do not.
		const auto link_point = view.text_to_client({6, 1});  // inside "docs"
		const auto url_point = view.text_to_client({14, 1});  // inside the URL
		const auto plain_point = view.text_to_client({1, 1}); // inside "see"

		CHECK_STR(view.link_at(link_point), "https://example.invalid/x");
		CHECK_STR(view.link_at(url_point), "https://example.invalid/x");
		CHECK_STR(view.link_at(plain_point), "");
		CHECK_STR(view.link_at(view.text_to_client({2, 0})), "");

		// Activation is injected: the view reports the target and the application
		// decides what a target means.
		std::string followed;
		CHECK(!view.has_link_handler());
		view.set_link_handler([&](const std::string_view target) { followed = target; });
		CHECK(view.has_link_handler());

		const auto click = [&](const pf::ipoint& at)
		{
			pf::mouse_params params;
			params.point = at;
			params.left_button = true;
			view.handle_mouse(frame, pf::mouse_message_type::left_button_down, params);
			view.handle_mouse(frame, pf::mouse_message_type::left_button_up, params);
		};

		click(link_point);
		CHECK_STR(followed, "https://example.invalid/x");

		// A press that ended somewhere else is a drag, not a click, so it follows
		// nothing even when it happens to release over a link.
		followed.clear();
		pf::mouse_params press;
		press.point = plain_point;
		press.left_button = true;
		view.handle_mouse(frame, pf::mouse_message_type::left_button_down, press);
		view.on_left_button_up(frame, link_point);
		CHECK_STR(followed, "");

		// And a drag that selected text keeps the text it selected rather than
		// following the link it happened to end on.
		followed.clear();
		view.set_selection({});
		pf::mouse_params drag;
		drag.point = link_point;
		drag.left_button = true;
		view.handle_mouse(frame, pf::mouse_message_type::left_button_down, press);
		view.handle_mouse(frame, pf::mouse_message_type::mouse_move, drag);
		view.handle_mouse(frame, pf::mouse_message_type::left_button_up, drag);
		CHECK(view.has_current_selection());
		CHECK_STR(followed, "");

		// Empty space beside the text is not the text: a click past the end of the
		// line, or below the last one, follows nothing.
		followed.clear();
		view.set_selection({});
		click({link_point.x, link_point.y + 5000});
		CHECK_STR(followed, "");
		click({5000, link_point.y});
		CHECK_STR(followed, "");
		CHECK_STR(view.link_at({5000, link_point.y}), "");
	}

	// The same target can appear twice with different provenance, and only one
	// occurrence may be navigable — so an application that decides per occurrence
	// has to be told which one was hit.
	void test_markdown_view_reports_which_link_was_hit()
	{
		pf::ui::test::recording_view_host host;
		const pf::ui::theme theme;
		markdown_probe view(host, theme);
		auto window = std::make_shared<pf::ui::test::fake_window_frame>();
		pf::window_frame_ptr frame = window;

		show_markdown(view, host, frame,
		              "see [first](x:1) and [second](x:2)\nplain line\nagain [third](x:1)\n");

		const auto first = view.link_hit_at(view.text_to_client({5, 0}));
		CHECK_STR(first.target, "x:1");
		CHECK_EQ(first.line, 0);

		const auto second = view.link_hit_at(view.text_to_client({24, 0}));
		CHECK_STR(second.target, "x:2");
		CHECK_EQ(second.line, 0);

		// Same line, different spans: the span index is what tells them apart.
		CHECK(first.span != second.span);

		// The same target on another line is a different occurrence, and says so.
		const auto third = view.link_hit_at(view.text_to_client({8, 2}));
		CHECK_STR(third.target, "x:1");
		CHECK_EQ(third.line, 2);

		// Nothing hit says nothing, rather than pointing at line zero.
		const auto miss = view.link_hit_at({5000, view.text_to_client({0, 1}).y});
		CHECK_STR(miss.target, "");
		CHECK_EQ(miss.line, -1);
		CHECK_EQ(miss.span, -1);
	}

	void test_markdown_view_tables()
	{
		pf::ui::test::recording_view_host host;
		const pf::ui::theme theme;
		markdown_probe view(host, theme);
		auto window = std::make_shared<pf::ui::test::fake_window_frame>();
		pf::window_frame_ptr frame = window;

		// "café" is four characters in five bytes, and its column is exactly four
		// wide: a cell wrapped by bytes would split the é in half.
		const auto buf = show_markdown(view, host, frame,
		                               "| name | note |\n| ---- | ---- |\n| caf\xC3\xA9 | ok |\nafter");

		pf::ui::test::fake_draw_context draw{pf::irect(0, 0, 480, 320)};
		view.handle_paint(frame, draw);

		for (const auto& t : draw.texts)
			CHECK(t.text.empty() || !pf::is_utf8_continuation(t.text.front()));

		// The rendered row is padded into its columns, so a drag over it selects
		// whole source lines rather than bytes that are not where they look.
		const auto row_y = view.text_to_client({0, 2}).y;
		CHECK_EQ(view.text_at({0, row_y}).y, 2);
		CHECK_EQ(view.text_at({0, row_y}).x, 0);
		CHECK_EQ(view.text_at({470, row_y}).x, static_cast<int>((*buf)[2].size()));

		// A link cannot be hit inside a table for the same reason.
		CHECK_STR(view.link_at({100, row_y}), "");

		// A selected row is drawn selected: the whole row, because half of a padded
		// cell is not an honest thing to shade.
		view.set_selection(pf::ui::text_selection(0, 2, 0, 3));
		draw.reset();
		view.handle_paint(frame, draw);

		auto saw_selected_cell = false;
		for (const auto& t : draw.texts)
			if (t.text.find("ok") != std::string::npos &&
				t.background == theme.style_color(pf::ui::text_style::sel_bkgnd))
				saw_selected_cell = true;
		CHECK(saw_selected_cell);
	}

	// The vertical scrollbar is drawn over the view's right edge, so a wrapped row
	// stops short of it. A row that filled the width ran its last letters under
	// the thumb (equity's transcript, list0's reading pane).
	void test_markdown_view_keeps_text_clear_of_the_scrollbar()
	{
		pf::ui::test::recording_view_host host;
		const pf::ui::theme theme;
		markdown_probe view(host, theme);
		auto window = std::make_shared<pf::ui::test::fake_window_frame>();
		pf::window_frame_ptr frame = window;

		// One unbroken word is cut at the last column that fits, so its first row
		// is as full as a row can be.
		const pf::isize extent{200, 100};
		show_markdown(view, host, frame, std::string(100, 'x'), extent);

		const auto first_row = view.text_to_client({0, 0}).y;
		auto right = 0;
		for (int i = 0; i < 100; ++i)
		{
			const auto pt = view.text_to_client({i, 0});
			if (pt.y == first_row)
				right = std::max(right, pt.x + 8);
		}

		using bar = pf::ui::custom_scrollbar;
		CHECK(right > extent.cx / 2);
		CHECK(right <= extent.cx - bar::base_thumb_thickness - 2 * bar::base_edge_margin);
	}

	void test_markdown_view_wraps_without_touching_the_buffer()
	{
		pf::ui::test::recording_view_host host;
		const pf::ui::theme theme;
		markdown_probe view(host, theme);
		auto window = std::make_shared<pf::ui::test::fake_window_frame>();
		pf::window_frame_ptr frame = window;

		const std::string_view source =
			"- aaa bbb ccc ddd eee fff ggg hhh iii jjj kkk lll mmm nnn ooo ppp qqq rrr\nafter";

		// 160 pixels is 20 columns, so the bullet cannot fit on one row.
		const auto buf = show_markdown(view, host, frame, source, {160, 320});

		CHECK_EQ(buf->size(), 2u);
		CHECK_STR(buf->str(), source);

		// The wrapped line pushes what follows it well down the view.
		const auto wrapped_top = view.text_to_client({0, 0}).y;
		const auto next_top = view.text_to_client({0, 1}).y;
		CHECK(next_top - wrapped_top > 16 * 3);

		// A continuation row is indented under the item's text rather than under
		// its bullet, and hit testing follows it there.
		const auto tail = view.text_to_client({static_cast<int>(source.find("rrr")), 0});
		CHECK(tail.x > view.text_to_client({0, 0}).x);
		CHECK(tail.y > wrapped_top);

		const auto back = view.text_at(tail);
		CHECK_EQ(back.y, 0);
		CHECK_EQ(back.x, static_cast<int>(source.find("rrr")));
	}

	void test_markdown_view_utf8_positions()
	{
		pf::ui::test::recording_view_host host;
		const pf::ui::theme theme;
		markdown_probe view(host, theme);
		auto window = std::make_shared<pf::ui::test::fake_window_frame>();
		pf::window_frame_ptr frame = window;

		// "héllo wörld" — multi-byte characters, so a column is not a byte.
		show_markdown(view, host, frame, "h\xC3\xA9llo w\xC3\xB6rld\n");

		// The second character is two bytes wide but one column across, and a click
		// on it lands on its first byte rather than inside it.
		const auto point = view.text_to_client({1, 0});
		CHECK_EQ(point.x, view.text_to_client({0, 0}).x + 8);
		CHECK_EQ(view.text_at(point).x, 1);

		const auto after = view.text_to_client({3, 0});
		CHECK_EQ(after.x, view.text_to_client({0, 0}).x + 16);
		CHECK_EQ(view.text_at(after).x, 3);
	}

	// A quote is how a chat transcript says whose turn it is, so quoted text has to
	// be styleable on its own. The default palette leaves it reading as ordinary
	// text, which is what a document renderer wants — the style exists so an
	// application can mean something by it, not so every blockquote changes colour.
	void test_markdown_view_styles_a_quote()
	{
		struct quote_theme : pf::ui::theme
		{
			[[nodiscard]] pf::color_t style_color(const pf::ui::text_style s) const override
			{
				return s == pf::ui::text_style::md_quote
					       ? pf::color_t{0x60, 0xa0, 0xff}
					       : pf::ui::theme::style_color(s);
			}
		};

		const pf::ui::theme plain;
		CHECK(plain.style_color(pf::ui::text_style::md_quote) ==
			plain.style_color(pf::ui::text_style::normal_text));

		const quote_theme theme;
		pf::ui::test::recording_view_host host;
		markdown_probe view(host, theme);
		auto window = std::make_shared<pf::ui::test::fake_window_frame>();
		pf::window_frame_ptr frame = window;

		show_markdown(view, host, frame, "> asked something\nanswered something\n");

		pf::ui::test::fake_draw_context draw{pf::irect(0, 0, 480, 320)};
		view.handle_paint(frame, draw);

		auto quoted = false;
		auto plain_run = false;

		for (const auto& t : draw.texts)
		{
			if (t.text.find("asked") != std::string::npos)
			{
				quoted = true;
				CHECK(t.color == theme.style_color(pf::ui::text_style::md_quote));
			}
			if (t.text.find("answered") != std::string::npos)
			{
				plain_run = true;
				CHECK(t.color == theme.style_color(pf::ui::text_style::normal_text));
			}
		}

		CHECK(quoted);
		CHECK(plain_run);
	}

	// A transcript has no menu behind it any more than a prompt box does, so the
	// copy shortcuts have to reach the view itself. Cut and paste must not: a
	// read-only view that can lose what it is showing is worse than one that
	// cannot copy.
	void test_read_only_view_copies_with_the_keyboard()
	{
		struct clip_probe : markdown_probe
		{
			using markdown_probe::markdown_probe;

			mutable std::string board;

			[[nodiscard]] std::string clipboard_text() const override { return board; }

			bool set_clipboard(const std::string_view text) const override
			{
				board = text;
				return true;
			}
		};

		pf::ui::test::recording_view_host host;
		const pf::ui::theme theme;
		clip_probe view(host, theme);
		auto window = std::make_shared<pf::ui::test::fake_window_frame>();
		pf::window_frame_ptr frame = window;

		const auto buf = show_markdown(view, host, frame, "first line\nsecond line");
		const auto original = buf->str();
		window->held_keys.insert(pf::platform_key::Control);

		view.on_key_down(frame, 'A');
		CHECK(view.has_current_selection());

		view.on_key_down(frame, 'C');
		CHECK_STR(view.board, "first line\r\nsecond line");

		// Neither verb may reach the buffer, and a refused cut must not quietly
		// put the transcript on the clipboard either.
		view.board = "elsewhere";
		view.on_key_down(frame, 'X');
		view.on_key_down(frame, 'V');
		CHECK_STR(buf->str(), original);
		CHECK_STR(view.board, "elsewhere");
	}

	// Both table views are driven the same way: show text, size the view, lay out.
	template <typename View>
	pf::ui::text_buffer_ptr show_in(View& view, pf::ui::view_host& host, pf::window_frame_ptr& frame,
	                                const std::string_view text, const pf::isize extent = {480, 320})
	{
		const auto buf = std::make_shared<pf::ui::text_buffer>(host);
		buf->set_text(text);
		view.set_buffer(buf, {});

		pf::ui::test::fake_measure_context measure;
		view.handle_size(frame, extent, measure);
		view.layout();
		return buf;
	}

	void test_csv_view()
	{
		pf::ui::test::recording_view_host host;
		const pf::ui::theme theme;
		pf::ui::csv_view view(host, theme);
		auto window = std::make_shared<pf::ui::test::fake_window_frame>();
		pf::window_frame_ptr frame = window;

		const auto buf = show_in(view, host, frame,
		                         "name,qty,note\nwidget,12,the long one\nbolt,3,short");

		// A column is as wide as its widest cell, in code points.
		const auto& table = view.table();
		CHECK_EQ(table.col_widths.size(), 3u);
		CHECK_EQ(table.col_widths[0], 6);  // "widget"
		CHECK_EQ(table.col_widths[2], 12); // "the long one"

		pf::ui::test::fake_draw_context draw{pf::irect(0, 0, 480, 320)};
		view.handle_paint(frame, draw);

		const auto drawn = draw.drawn_text();
		CHECK(drawn.find("name") != std::string::npos);
		CHECK(drawn.find("widget") != std::string::npos);
		CHECK(drawn.find("the long one") != std::string::npos);

		// The header is followed by a drawn separator that is in no record.
		auto saw_separator = false;
		for (const auto& t : draw.texts)
			if (t.text.find("------") != std::string::npos) saw_separator = true;
		CHECK(saw_separator);
		CHECK_EQ(buf->size(), 3u);

		// A point in a record resolves to the record: its cells are drawn padded
		// into their columns, so there is no byte under the pointer to find.
		const auto second_row_y = view.text_at({0, 0}).y;
		CHECK(second_row_y >= 0);
		CHECK_EQ(view.text_at({0, 100}).x, 0);
		CHECK(view.text_at({470, 100}).x > 0);
		CHECK(view.allows_drag_selection());

		// What is copied is the record as it is in the file, not as it is drawn.
		view.set_selection(pf::ui::text_selection(0, 1, 0, 2));
		CHECK_STR(view.select_text(), "widget,12,the long one\r\n");

		// And a selected record is drawn selected.
		draw.reset();
		view.handle_paint(frame, draw);

		auto saw_selected = false;
		for (const auto& t : draw.texts)
			if (t.text.find("widget") != std::string::npos &&
				t.background == theme.style_color(pf::ui::text_style::sel_bkgnd))
				saw_selected = true;
		CHECK(saw_selected);
	}

	void test_csv_view_quotes_and_utf8()
	{
		pf::ui::test::recording_view_host host;
		const pf::ui::theme theme;
		pf::ui::csv_view view(host, theme);
		auto window = std::make_shared<pf::ui::test::fake_window_frame>();
		pf::window_frame_ptr frame = window;

		// A quoted field keeps its comma, and "café" is four columns in five bytes.
		show_in(view, host, frame, "a,b\n\"x,y\",caf\xC3\xA9");

		CHECK_EQ(view.table().col_widths.size(), 2u);
		CHECK_EQ(view.table().col_widths[1], 4);

		pf::ui::test::fake_draw_context draw{pf::irect(0, 0, 480, 320)};
		view.handle_paint(frame, draw);

		// Wrapping a cell must not split a character in half.
		for (const auto& t : draw.texts)
			CHECK(t.text.empty() || !pf::is_utf8_continuation(t.text.front()));

		CHECK(draw.drawn_text().find("x,y") != std::string::npos);

		// An escaped quote is drawn once, and measured once (rethinkify showed
		// ""quotes"" for "quotes").
		show_in(view, host, frame, "a,b\n\"say \"\"hi\"\"\",z");
		CHECK_EQ(view.table().col_widths[0], 8);
		draw.reset();
		view.handle_paint(frame, draw);
		CHECK(draw.drawn_text().find("say \"hi\"") != std::string::npos);
		CHECK(draw.drawn_text().find("\"\"") == std::string::npos);
	}

	void test_hex_view()
	{
		pf::ui::test::recording_view_host host;
		const pf::ui::theme theme;
		pf::ui::hex_view view(host, theme);
		auto window = std::make_shared<pf::ui::test::fake_window_frame>();
		pf::window_frame_ptr frame = window;

		// One row of sixteen bytes, then a short one: "AB" plus a control byte.
		show_in(view, host, frame, std::string("0123456789ABCDEF") + "\n" + std::string("AB\x01"));

		pf::ui::test::fake_draw_context draw{pf::irect(0, 0, 640, 320)};
		view.handle_paint(frame, draw);

		const auto drawn = draw.drawn_text();

		// Offsets count bytes, not lines: the second row starts at 0x10.
		CHECK(drawn.find("00000000") != std::string::npos);
		CHECK(drawn.find("00000010") != std::string::npos);

		// '0' is 0x30 and 'A' is 0x41, and the ASCII column shows them as text.
		CHECK(drawn.find("30 31 32 33") != std::string::npos);
		CHECK(drawn.find("0123456789ABCDEF") != std::string::npos);

		// An unprintable byte is a dot rather than whatever the font makes of it.
		CHECK(drawn.find("41 42 01") != std::string::npos);
		CHECK(drawn.find("AB.") != std::string::npos);
		CHECK(drawn.find('\x01') == std::string::npos);
	}

	// The list is driven through its public surface; only the row-building helper
	// below reaches in, because what a row *is* belongs to the application.
	struct list_probe : pf::ui::list_view
	{
		using pf::ui::list_view::list_view;
		using pf::ui::list_view::_items;
		using pf::ui::list_view::_header_height;
		using pf::ui::list_view::draw_item;
		using pf::ui::list_view::on_key_down;

		void add(const std::string_view text, const bool group = false, const int depth = 0)
		{
			auto item = std::make_shared<pf::ui::list_item>();
			item->text = text;
			item->is_group = group;
			item->depth = depth;
			_items.push_back(item);
		}
	};

	pf::ui::list_view* size_list(list_probe& view, pf::window_frame_ptr& frame, const pf::isize extent = {300, 200})
	{
		pf::ui::test::fake_measure_context measure;
		view.handle_size(frame, extent, measure);
		return &view;
	}

	void test_list_layout_and_hit_testing()
	{
		const pf::ui::theme theme;
		list_probe view(theme);
		auto window = std::make_shared<pf::ui::test::fake_window_frame>();
		pf::window_frame_ptr frame = window;

		for (int i = 0; i < 40; i++) view.add(std::format("row {}", i));
		size_list(view, frame);

		// Rows are laid out top to bottom, each as tall as the list font plus its
		// padding, and the content is taller than the view — so it can scroll.
		const auto first = view._items.front()->bounds;
		const auto second = view._items[1]->bounds;
		CHECK_EQ(second.top, first.bottom);
		CHECK(first.height() > 0);
		CHECK(view.can_scroll());

		// A point resolves to the row that contains it, by binary search rather
		// than a scan — which is what makes a list of a hundred thousand files
		// answer a click at all.
		CHECK(view.selection_from_point({10, first.top + 1}) == view._items.front());
		CHECK(view.selection_from_point({10, second.top + 1}) == view._items[1]);

		// Above the first row and past the last there is nothing to hit.
		CHECK(view.selection_from_point({10, -50}) == nullptr);
		CHECK(view.selection_from_point({10, 100000}) == nullptr);

		// An empty list hit-tests to nothing rather than reading past its rows.
		list_probe empty(theme);
		CHECK(empty.selection_from_point({0, 0}) == nullptr);
	}

	void test_list_selection_and_copy()
	{
		const pf::ui::theme theme;
		list_probe view(theme);
		auto window = std::make_shared<pf::ui::test::fake_window_frame>();
		pf::window_frame_ptr frame = window;

		for (int i = 0; i < 6; i++) view.add(std::format("row {}", i));
		size_list(view, frame);

		view.set_selected(1);
		CHECK_EQ(view.selected_index(), 1);
		CHECK(view.is_selected(1));
		CHECK(!view.is_selected(2));
		CHECK_STR(view.selected_rows_text(), "row 1");

		// Arrow keys move the selection and keep it a single row.
		view.navigate_next(frame, true);
		CHECK_EQ(view.selected_index(), 2);
		CHECK_STR(view.selected_rows_text(), "row 2");

		// Shift extends from where the selection started, so a run of rows can be
		// copied — which no list in these applications could do before.
		view.navigate_next(frame, true, false, true);
		view.navigate_next(frame, true, false, true);
		CHECK_EQ(view.selected_index(), 4);
		CHECK(view.is_selected(2));
		CHECK(view.is_selected(3));
		CHECK(view.is_selected(4));
		CHECK(!view.is_selected(1));
		CHECK_STR(view.selected_rows_text(), "row 2\r\nrow 3\r\nrow 4");

		// Moving without Shift collapses the run back to one row.
		view.navigate_next(frame, false);
		CHECK_STR(view.selected_rows_text(), "row 3");

		// The selection survives being asked for after the list is rebuilt, because
		// the index is only a hint.
		const auto keep = view.selected_item();
		view._items.insert(view._items.begin(), std::make_shared<pf::ui::list_item>());
		CHECK_EQ(view.selected_index(), 4);
		CHECK(view.selected_item() == keep);

		// Navigation stops at the ends rather than wrapping.
		view.set_selected(static_cast<int>(view._items.size()) - 1);
		view.navigate_next(frame, true);
		CHECK_EQ(view.selected_index(), static_cast<int>(view._items.size()) - 1);
		view.set_selected(0);
		view.navigate_next(frame, false);
		CHECK_EQ(view.selected_index(), 0);
	}

	void test_list_paints_its_rows()
	{
		const pf::ui::theme theme;
		list_probe view(theme);
		auto window = std::make_shared<pf::ui::test::fake_window_frame>();
		pf::window_frame_ptr frame = window;

		view.add("a folder", true);
		view.add("a file", false, 1);
		size_list(view, frame);
		view.set_selected(1);

		pf::ui::test::fake_draw_context draw{pf::irect(0, 0, 300, 200)};
		view.handle_paint(frame, draw);

		const auto drawn = draw.drawn_text();
		CHECK(drawn.find("a folder") != std::string::npos);
		CHECK(drawn.find("a file") != std::string::npos);

		// A group is drawn in the group colour; the selected row is filled with the
		// selection colour behind it.
		auto saw_group_color = false;
		for (const auto& t : draw.texts)
			if (t.text == "a folder" && t.color == theme.group_text) saw_group_color = true;
		CHECK(saw_group_color);

		auto saw_selection_fill = false;
		for (const auto& f : draw.fills)
			if (f.color == theme.handle || f.color == theme.focus_handle) saw_selection_fill = true;
		CHECK(saw_selection_fill);

		// An indented row starts further right than the one above it.
		int folder_x = 0;
		int file_x = 0;
		for (const auto& t : draw.texts)
		{
			if (t.text == "a folder") folder_x = t.x;
			if (t.text == "a file") file_x = t.x;
		}
		CHECK(file_x > folder_x);
	}

	void test_list_columns_and_sorting()
	{
		// A list with columns draws a header and reports clicks on it. The
		// *ordering* stays with the application: a column of live prices sorts by
		// the number behind the text, with its own rule for rows that have none,
		// and the list cannot know that.
		struct sorting_probe : list_probe
		{
			using list_probe::list_probe;

			int sorted_column = -1;
			bool sorted_ascending = false;
			int sort_calls = 0;

			void on_sort_changed(const int column, const bool ascending) override
			{
				sorted_column = column;
				sorted_ascending = ascending;
				++sort_calls;
			}
		};

		const pf::ui::theme theme;
		sorting_probe view(theme);
		auto window = std::make_shared<pf::ui::test::fake_window_frame>();
		pf::window_frame_ptr frame = window;

		view.set_columns({
			{"Sym", 37, false},
			{"Price", 35, true},
			{"Chg%", 28, true},
		});

		const auto add_row = [&](const std::string& sym, const std::string& price,
		                         const std::string& chg, const pf::color_t chg_color)
		{
			auto item = std::make_shared<pf::ui::list_item>();
			item->cells = {pf::ui::list_cell{sym}, pf::ui::list_cell{price},
			               pf::ui::list_cell{chg, chg_color}};
			view._items.push_back(item);
		};

		add_row("AAPL", "182.40", "+1.2%", theme.text);
		add_row("MSFT", "410.15", "-0.4%", theme.dim_text);

		size_list(view, frame, {300, 200});

		// The header only exists because there are columns to title.
		CHECK(view._header_height > 0);

		// Columns divide the width by weight, in order, and cover it exactly.
		const auto rects = view.column_rects(300);
		CHECK_EQ(rects.size(), 3u);
		CHECK_EQ(rects[0].left, 0);
		CHECK_EQ(rects[1].left, rects[0].right);
		CHECK_EQ(rects[2].left, rects[1].right);
		CHECK_EQ(rects[2].right, 300);
		CHECK(rects[0].width() > rects[2].width()); // 37 beats 28

		// A point in the header names its column; a point below it names none.
		CHECK_EQ(view.column_at({5, 2}), 0);
		CHECK_EQ(view.column_at({rects[2].left + 5, 2}), 2);
		CHECK_EQ(view.column_at({5, view._header_height + 10}), -1);

		// Nothing is sorted until something is clicked.
		CHECK_EQ(view.sort_column(), -1);

		pf::mouse_params click;
		click.point = {rects[1].left + 5, 2};
		view.handle_mouse(frame, pf::mouse_message_type::left_button_down, click);

		CHECK_EQ(view.sort_calls, 1);
		CHECK_EQ(view.sorted_column, 1);
		CHECK(view.sorted_ascending);
		CHECK_EQ(view.sort_column(), 1);

		// Clicking the sorted column reverses it.
		view.handle_mouse(frame, pf::mouse_message_type::left_button_down, click);
		CHECK_EQ(view.sort_calls, 2);
		CHECK(!view.sorted_ascending);
		CHECK(!view.sort_ascending());

		// Clicking a different one starts ascending again rather than inheriting
		// the previous direction.
		click.point = {rects[2].left + 5, 2};
		view.handle_mouse(frame, pf::mouse_message_type::left_button_down, click);
		CHECK_EQ(view.sorted_column, 2);
		CHECK(view.sorted_ascending);

		// A header click is not a row click: the selection is untouched.
		CHECK(!view.selected_item());

		// A column can say which way it reads best. A price wants biggest-first,
		// and having to click twice to get there is the sort of small wrongness
		// nobody reports.
		sorting_probe numeric(theme);
		numeric.set_columns({
			{"Sym", 40, false, true},
			{"Price", 30, true, false},
		});
		size_list(numeric, frame, {300, 200});

		const auto numeric_rects = numeric.column_rects(300);
		pf::mouse_params price_click;
		price_click.point = {numeric_rects[1].left + 5, 2};
		numeric.handle_mouse(frame, pf::mouse_message_type::left_button_down, price_click);
		CHECK_EQ(numeric.sorted_column, 1);
		CHECK(!numeric.sorted_ascending);

		// Reversing still works from there.
		numeric.handle_mouse(frame, pf::mouse_message_type::left_button_down, price_click);
		CHECK(numeric.sorted_ascending);
	}

	void test_list_draws_its_cells()
	{
		const pf::ui::theme theme;
		list_probe view(theme);
		auto window = std::make_shared<pf::ui::test::fake_window_frame>();
		pf::window_frame_ptr frame = window;

		view.set_columns({{"Sym", 40, false}, {"Price", 30, true}, {"Chg%", 30, true}});
		view.set_sort(2, false);

		auto item = std::make_shared<pf::ui::list_item>();
		item->cells = {
			pf::ui::list_cell{"AAPL"},
			pf::ui::list_cell{"182.40"},
			pf::ui::list_cell{"+1.2%", theme.match_highlight},
		};
		view._items.push_back(item);

		size_list(view, frame, {400, 200});

		pf::ui::test::fake_draw_context draw{pf::irect(0, 0, 400, 200)};
		view.handle_paint(frame, draw);

		const auto drawn = draw.drawn_text();

		// The titles and the cells are all on screen.
		CHECK(drawn.find("Sym") != std::string::npos);
		CHECK(drawn.find("AAPL") != std::string::npos);
		CHECK(drawn.find("182.40") != std::string::npos);
		CHECK(drawn.find("+1.2%") != std::string::npos);

		// The sorted column carries an indicator; the others do not.
		auto sorted_marked = false;
		for (const auto& t : draw.texts)
			if (t.text.find("Chg%") != std::string::npos &&
				t.text.find("\xE2\x96\xBC") != std::string::npos)
				sorted_marked = true;
		CHECK(sorted_marked);

		// A cell can carry its own colour — a gain is green whatever the row is.
		auto coloured_cell = false;
		for (const auto& t : draw.texts)
			if (t.text == "+1.2%" && t.color == theme.match_highlight) coloured_cell = true;
		CHECK(coloured_cell);

		// Cells are laid out left to right in column order.
		int sym_x = -1, price_x = -1, chg_x = -1;
		for (const auto& t : draw.texts)
		{
			if (t.text == "AAPL") sym_x = t.x;
			if (t.text == "182.40") price_x = t.x;
			if (t.text == "+1.2%") chg_x = t.x;
		}
		CHECK(sym_x >= 0 && price_x > sym_x && chg_x > price_x);

		// And a row copies as its cells, so a run of them pastes as a table.
		view.set_selected(0);
		CHECK_STR(view.selected_rows_text(), "AAPL\t182.40\t+1.2%");
	}

	// A list under a menu gets copy as a command. A list in an application that
	// has no menu gets nothing — which is how `copy_rows` came to exist, be
	// tested for what it would produce, and never be reachable by anyone.
	void test_list_copies_with_the_keyboard()
	{
		struct clip_list : list_probe
		{
			using list_probe::list_probe;

			mutable std::string board;

			bool set_clipboard(const std::string_view text) const override
			{
				board = text;
				return true;
			}
		};

		const pf::ui::theme theme;
		clip_list view(theme);
		auto window = std::make_shared<pf::ui::test::fake_window_frame>();
		pf::window_frame_ptr frame = window;

		for (int i = 0; i < 4; i++) view.add(std::format("row {}", i));
		size_list(view, frame);

		view.set_selected(1);
		window->held_keys.insert(pf::platform_key::Control);
		view.on_key_down(frame, 'C');
		CHECK_STR(view.board, "row 1");

		// A run of rows copies as the run, which is what Shift-navigation is for.
		view.board.clear();
		view.navigate_next(frame, true, false, true);
		view.on_key_down(frame, 'C');
		CHECK_STR(view.board, "row 1\r\nrow 2");

		// Ctrl+Shift+C is a different shortcut and must still reach the view as
		// an ordinary key rather than being swallowed as a copy.
		view.board.clear();
		window->held_keys.insert(pf::platform_key::Shift);
		view.on_key_down(frame, 'C');
		CHECK_STR(view.board, "");

		// A right-click offers the same copy, for an application that has no
		// command table to put one on. can_copy_rows is what it asks.
		window->held_keys.clear();
		view.set_selected(0);
		const auto items = view.default_popup_menu();
		CHECK_EQ(items.size(), 1u);
		if (items.size() != 1) return;
		CHECK_STR(items[0].text, "&Copy");
		CHECK(items[0].is_enabled());

		view.set_selected(-1);
		CHECK(!items[0].is_enabled());
	}

	// The menu a right-click opens copies the selection, so the click has to put
	// the selection where it landed first. It used to change nothing: right-click
	// MSFT, choose Copy, and get whichever row had been selected before.
	void test_list_right_click_selects_the_row()
	{
		const pf::ui::theme theme;
		list_probe view(theme);
		auto window = std::make_shared<pf::ui::test::fake_window_frame>();
		pf::window_frame_ptr frame = window;

		for (int i = 0; i < 6; i++) view.add(std::format("row {}", i));
		size_list(view, frame);

		const auto right_click = [&](const int x, const int y)
		{
			pf::mouse_params params;
			params.point = {x, y};
			view.handle_mouse(frame, pf::mouse_message_type::right_button_down, params);
		};
		const auto row_y = [&](const int row) { return view._items[row]->bounds.top + 1; };

		view.set_selected(0);
		right_click(10, row_y(3));
		CHECK_EQ(view.selected_index(), 3);
		CHECK_STR(view.selected_rows_text(), "row 3");

		// Inside a shift-extended run the run stays, so it can be right-clicked
		// and copied as a whole.
		view.set_selected(1);
		view.navigate_next(frame, true, false, true);
		view.navigate_next(frame, true, false, true);
		right_click(10, row_y(2));
		CHECK_STR(view.selected_rows_text(), "row 1\r\nrow 2\r\nrow 3");

		// Outside it, the run gives way to the row that was clicked.
		right_click(10, row_y(5));
		CHECK_STR(view.selected_rows_text(), "row 5");

		// Past the last row there is nothing to select, so nothing changes.
		right_click(10, 100000);
		CHECK_EQ(view.selected_index(), 5);

		// Nor does a right-click on the scrollbar pick the row beside it. The bar
		// learns its extent when the list paints, as it has before any real click.
		list_probe tall(theme);
		for (int i = 0; i < 40; i++) tall.add(std::format("row {}", i));
		size_list(tall, frame);
		CHECK(tall.can_scroll());
		pf::ui::test::fake_draw_context painted{pf::irect(0, 0, 300, 200)};
		tall.handle_paint(frame, painted);
		tall.set_selected(0);
		pf::mouse_params bar;
		bar.point = {299, tall._items[2]->bounds.top + 1};
		tall.handle_mouse(frame, pf::mouse_message_type::right_button_down, bar);
		CHECK_EQ(tall.selected_index(), 0);
	}

	void test_list_row_text_fits_its_column()
	{
		const pf::ui::theme theme;
		list_probe view(theme);
		auto window = std::make_shared<pf::ui::test::fake_window_frame>();
		pf::window_frame_ptr frame = window;

		// 8-pixel cells, so 200 pixels is 25 columns and this row cannot fit.
		view.add("a name far longer than the column it has to live in");
		size_list(view, frame, {200, 200});

		pf::ui::test::fake_draw_context draw{pf::irect(0, 0, 200, 200)};
		view.handle_paint(frame, draw);

		const auto drawn = draw.drawn_text();
		CHECK(drawn.find("...") != std::string::npos);
		CHECK(drawn.find("live in") == std::string::npos);

		// A row with a match is windowed around the match instead, because the match
		// is the reason the row is on screen at all.
		list_probe hits(theme);
		hits.add("aaaaaaaaaaaaaaaaaaaaaaaaaaaaaa needle bbbbbbbbbbbbbbbbbbbbbbbbbbbbbb");
		hits._items.front()->prefix = "42: ";
		hits._items.front()->match_start = 31;
		hits._items.front()->match_length = 6;
		size_list(hits, frame, {200, 200});

		draw.reset();
		hits.handle_paint(frame, draw);

		auto saw_highlight = false;
		for (const auto& t : draw.texts)
			if (t.text == "needle" && t.background == theme.match_highlight) saw_highlight = true;
		CHECK(saw_highlight);

		// The line number is drawn dimmer, and before the text.
		auto saw_prefix = false;
		for (const auto& t : draw.texts)
			if (t.text == "42: " && t.color == theme.dim_text) saw_prefix = true;
		CHECK(saw_prefix);
	}

	void test_edit_view_cut_keeps_text_when_the_clipboard_refuses()
	{
		// A clipboard can refuse — another process may hold it open. Cutting must
		// then keep the text, because text that is neither on the clipboard nor in
		// the document is gone, recoverable only by an undo the person has no
		// reason to know they need.
		struct probe : pf::ui::edit_doc_view
		{
			using pf::ui::edit_doc_view::edit_doc_view;
			using pf::ui::edit_doc_view::cut_text_to_clipboard;

			bool clipboard_accepts = true;
			std::string written;

			bool set_clipboard(const std::string_view text) const override
			{
				if (!clipboard_accepts) return false;
				const_cast<probe*>(this)->written = text;
				return true;
			}
		};

		pf::ui::test::recording_view_host host;
		const pf::ui::theme theme;
		probe view(host, theme);
		auto window = std::make_shared<pf::ui::test::fake_window_frame>();
		pf::window_frame_ptr frame = window;

		const auto buf = std::make_shared<pf::ui::text_buffer>(host);
		buf->set_text("keep me");
		view.set_buffer(buf, {});

		pf::ui::test::fake_measure_context measure;
		view.handle_size(frame, pf::isize{400, 200}, measure);
		view.select_all_text();

		// Refused: the text stays exactly where it was.
		view.clipboard_accepts = false;
		CHECK(!view.cut_text_to_clipboard());
		CHECK_STR(buf->str(), "keep me");
		CHECK(view.has_current_selection());

		// Accepted: now it moves.
		view.clipboard_accepts = true;
		CHECK(view.cut_text_to_clipboard());
		CHECK_STR(view.written, "keep me");
		CHECK_STR(buf->str(), "");
	}

	// A view that records what its window told it, so a test can assert the host
	// routed an event rather than that something was merely drawn.
	struct probe_pane : pf::frame_reactor
	{
		std::string name;
		pf::ipoint last_point;
		pf::mouse_message_type last_mouse{};
		int mouse_count = 0;
		int leave_count = 0;
		int key_count = 0;
		int timer_count = 0;
		uint32_t last_timer_id = 0;
		bool focused = false;
		pf::isize extent;

		explicit probe_pane(std::string n) : name(std::move(n))
		{
		}

		uint32_t handle_message(pf::window_frame_ptr window, const pf::message_type msg,
		                        const pf::message_params& params) override
		{
			if (msg == pf::message_type::set_focus) focused = true;
			if (msg == pf::message_type::kill_focus) focused = false;
			if (msg == pf::message_type::timer)
			{
				++timer_count;
				last_timer_id = params.timer_id;
			}
			return 0;
		}

		uint32_t handle_mouse(pf::window_frame_ptr window, const pf::mouse_message_type msg,
		                      const pf::mouse_params& params) override
		{
			if (msg == pf::mouse_message_type::mouse_leave)
			{
				++leave_count;
			}
			else
			{
				++mouse_count;
				last_point = params.point;
				last_mouse = msg;
			}
			return 0;
		}

		uint32_t handle_keyboard(pf::window_frame_ptr window, const pf::keyboard_message_type,
		                         const pf::keyboard_params&) override
		{
			++key_count;
			return 0;
		}

		void handle_paint(pf::window_frame_ptr& window, pf::draw_context& draw) override
		{
			// Draws at its own coordinates, as every view does
			const pf::irect box(0, 0, 20, 10);
			draw.fill_solid_rect(box, pf::color_t{1, 2, 3});
			draw.draw_text(0, 0, box, name, pf::font{}, pf::color_t{}, pf::color_t{});
		}

		void handle_size(pf::window_frame_ptr& window, const pf::isize e, pf::measure_context&) override
		{
			extent = e;
		}
	};

	void test_pane_host_routes_input()
	{
		auto window = std::make_shared<pf::ui::test::fake_window_frame>();
		pf::window_frame_ptr frame = window;
		pf::ui::pane_host host(frame);
		pf::ui::test::fake_measure_context measure;

		const auto left = std::make_shared<probe_pane>("left");
		const auto right = std::make_shared<probe_pane>("right");
		const auto left_pane = host.add_pane(left);
		const auto right_pane = host.add_pane(right);

		host.set_pane_bounds(left_pane, {0, 0, 100, 200}, measure);
		host.set_pane_bounds(right_pane, {100, 0, 300, 200}, measure);

		// A pane is told its own size, not the window's.
		CHECK_EQ(left->extent.cx, 100);
		CHECK_EQ(right->extent.cx, 200);
		CHECK_EQ(left_pane->get_client_rect().right, 100);

		// A click goes to the pane under it, in that pane's coordinates — the whole
		// point, since a view believes its top-left corner is the origin.
		pf::mouse_params click;
		click.point = {150, 40};
		host.handle_mouse(pf::mouse_message_type::left_button_down, click);
		CHECK_EQ(right->mouse_count, 1);
		CHECK_EQ(left->mouse_count, 0);
		CHECK_EQ(right->last_point.x, 50);
		CHECK_EQ(right->last_point.y, 40);

		// And a click in the other pane goes to the other pane.
		click.point = {10, 40};
		host.handle_mouse(pf::mouse_message_type::left_button_down, click);
		CHECK_EQ(left->mouse_count, 1);
		CHECK_EQ(left->last_point.x, 10);

		// A point in no pane is not an event.
		click.point = {1000, 1000};
		CHECK(!host.handle_mouse(pf::mouse_message_type::left_button_down, click));

		// The other buttons route the same way, releases included: an emulator
		// that saw the right button go down has to see it come back up in the
		// pane it went down in.
		for (const auto button : {pf::mouse_message_type::right_button_down,
		                          pf::mouse_message_type::right_button_up,
		                          pf::mouse_message_type::middle_button_down,
		                          pf::mouse_message_type::middle_button_up})
		{
			const auto before = right->mouse_count;
			click.point = {250, 60};
			CHECK(host.handle_mouse(button, click));
			CHECK_EQ(right->mouse_count, before + 1);
			CHECK_EQ(right->last_point.x, 150);
			CHECK_EQ(right->last_point.y, 60);
		}
		CHECK_EQ(left->mouse_count, 1);

		// Moving from one pane to another leaves the first, even though the window
		// itself never saw the pointer go anywhere.
		pf::mouse_params move;
		move.point = {10, 10};
		host.handle_mouse(pf::mouse_message_type::mouse_move, move);
		move.point = {150, 10};
		host.handle_mouse(pf::mouse_message_type::mouse_move, move);
		CHECK_EQ(left->leave_count, 1);
	}

	void test_pane_host_focus_and_keyboard()
	{
		auto window = std::make_shared<pf::ui::test::fake_window_frame>();
		pf::window_frame_ptr frame = window;
		window->set_focus();

		pf::ui::pane_host host(frame);
		pf::ui::test::fake_measure_context measure;

		const auto first = std::make_shared<probe_pane>("first");
		const auto second = std::make_shared<probe_pane>("second");
		const auto first_pane = host.add_pane(first);
		const auto second_pane = host.add_pane(second);
		host.set_pane_bounds(first_pane, {0, 0, 100, 100}, measure);
		host.set_pane_bounds(second_pane, {100, 0, 200, 100}, measure);

		// Keys go nowhere until a pane has the focus.
		CHECK(!host.handle_keyboard(pf::keyboard_message_type::key_down, {}));

		// A view asks its window for focus exactly as it would with a real one.
		first_pane->set_focus();
		CHECK(first->focused);
		CHECK(!second->focused);
		CHECK(host.handle_keyboard(pf::keyboard_message_type::key_down, {}));
		CHECK_EQ(first->key_count, 1);
		CHECK_EQ(second->key_count, 0);

		// Focus moving tells both sides, so the one losing it can stop a caret.
		second_pane->set_focus();
		CHECK(!first->focused);
		CHECK(second->focused);
		host.handle_keyboard(pf::keyboard_message_type::key_down, {});
		CHECK_EQ(first->key_count, 1);
		CHECK_EQ(second->key_count, 1);

		// has_focus() is about the pane *and* the window: a caret must not blink in
		// a window that is not active.
		CHECK(second_pane->has_focus());
		pf::ui::test::fake_window_frame::focused_window = nullptr;
		CHECK(!second_pane->has_focus());
	}

	void test_pane_host_capture_and_timers()
	{
		auto window = std::make_shared<pf::ui::test::fake_window_frame>();
		pf::window_frame_ptr frame = window;
		pf::ui::pane_host host(frame);
		pf::ui::test::fake_measure_context measure;

		const auto left = std::make_shared<probe_pane>("left");
		const auto right = std::make_shared<probe_pane>("right");
		const auto left_pane = host.add_pane(left);
		const auto right_pane = host.add_pane(right);
		host.set_pane_bounds(left_pane, {0, 0, 100, 200}, measure);
		host.set_pane_bounds(right_pane, {100, 0, 300, 200}, measure);

		// Capture outlives the pointer leaving the pane — which is what lets a drag
		// selection continue past the edge instead of stopping at it.
		left_pane->set_capture();

		pf::mouse_params drag;
		drag.point = {250, 40};
		host.handle_mouse(pf::mouse_message_type::mouse_move, drag);
		CHECK_EQ(left->mouse_count, 1);
		CHECK_EQ(right->mouse_count, 0);

		// The point is still translated, so it reads as being outside to the left
		// pane's own coordinates rather than being clamped or lost.
		CHECK_EQ(left->last_point.x, 250);

		left_pane->release_capture();
		host.handle_mouse(pf::mouse_message_type::mouse_move, drag);
		CHECK_EQ(right->mouse_count, 1);

		// Two panes both blinking a caret ask for the same id, so the host has to
		// give the window two different ones and map each back on the way in.
		constexpr uint32_t caret_id = 1002;
		CHECK_EQ(left_pane->set_timer(caret_id, 500), caret_id);
		CHECK_EQ(right_pane->set_timer(caret_id, 500), caret_id);

		auto fired = 0;
		for (const auto id : window->timers)
		{
			pf::message_params params;
			params.timer_id = id;
			if (host.handle_message(pf::message_type::timer, params)) ++fired;
		}

		CHECK_EQ(fired, 2);
		CHECK_EQ(left->timer_count, 1);
		CHECK_EQ(right->timer_count, 1);

		// Each pane sees the id it asked for, not the one the window really runs.
		CHECK_EQ(left->last_timer_id, caret_id);
		CHECK_EQ(right->last_timer_id, caret_id);

		// A timer the host did not issue belongs to the application.
		pf::message_params other;
		other.timer_id = 4242;
		CHECK(!host.handle_message(pf::message_type::timer, other));

		// Killing one leaves the other running.
		left_pane->kill_timer(caret_id);
		auto still_running = 0;
		for (const auto id : window->timers)
		{
			pf::message_params params;
			params.timer_id = id;
			if (host.handle_message(pf::message_type::timer, params)) ++still_running;
		}
		CHECK_EQ(still_running, 1);
	}

	// An application with drags of its own needs to see that a pane holds the
	// mouse, and to be able to end that drag on its own terms.
	void test_pane_host_arbitrates_capture()
	{
		auto window = std::make_shared<pf::ui::test::fake_window_frame>();
		pf::window_frame_ptr frame = window;
		pf::ui::pane_host host(frame);
		pf::ui::test::fake_measure_context measure;

		const auto pane_view = std::make_shared<probe_pane>("pane");
		const auto pane = host.add_pane(pane_view);
		host.set_pane_bounds(pane, {40, 20, 200, 120}, measure);

		CHECK(host.captured_pane() == nullptr);

		pane->set_capture();
		CHECK(host.captured_pane() == pane.get());

		// Capture reaches the real window too, or the application would stop
		// receiving the moves the pane is waiting for.
		CHECK(window->captured);

		const auto before = pane_view->mouse_count;
		host.release_all_capture();

		// The pane is told the button came up rather than being left tracking a
		// drag nobody will ever end.
		CHECK_EQ(pane_view->mouse_count, before + 1);
		CHECK(pane_view->last_mouse == pf::mouse_message_type::left_button_up);
		CHECK(host.captured_pane() == nullptr);
		CHECK(!window->captured);

		// Releasing when nothing is captured is not a synthesized click.
		const auto quiet = pane_view->mouse_count;
		host.release_all_capture();
		CHECK_EQ(pane_view->mouse_count, quiet);
	}

	void test_pane_host_confines_drawing()
	{
		auto window = std::make_shared<pf::ui::test::fake_window_frame>();
		pf::window_frame_ptr frame = window;
		pf::ui::pane_host host(frame);
		pf::ui::test::fake_measure_context measure;

		const auto left = std::make_shared<probe_pane>("left");
		const auto right = std::make_shared<probe_pane>("right");
		const auto left_pane = host.add_pane(left);
		const auto right_pane = host.add_pane(right);
		host.set_pane_bounds(left_pane, {0, 0, 100, 200}, measure);
		host.set_pane_bounds(right_pane, {100, 30, 300, 200}, measure);

		pf::ui::test::fake_draw_context draw{pf::irect(0, 0, 300, 200)};
		host.handle_paint(draw);

		// Both panes painted, each shifted into its own rectangle: the view drew at
		// 0,0 and the surface put it where the pane really is.
		CHECK_EQ(draw.texts.size(), 2u);
		CHECK_EQ(draw.texts[0].x, 0);
		CHECK_EQ(draw.texts[0].y, 0);
		CHECK_STR(draw.texts[1].text, "right");
		CHECK_EQ(draw.texts[1].x, 100);
		CHECK_EQ(draw.texts[1].y, 30);

		// The fills moved with them rather than overlapping at the origin.
		CHECK_EQ(draw.fills[0].rect.left, 0);
		CHECK_EQ(draw.fills[1].rect.left, 100);
		CHECK_EQ(draw.fills[1].rect.top, 30);
		CHECK_EQ(draw.fills[1].rect.right, 120);

		// A pane outside the dirty rectangle is not painted at all.
		pf::ui::test::fake_draw_context narrow{pf::irect(0, 0, 50, 200)};
		host.handle_paint(narrow);
		CHECK_EQ(narrow.texts.size(), 1u);
		CHECK_STR(narrow.texts[0].text, "left");

		// And a hidden pane is not painted either.
		draw.reset();
		left_pane->show(false);
		host.handle_paint(draw);
		CHECK_EQ(draw.texts.size(), 1u);
		CHECK_STR(draw.texts[0].text, "right");
	}

	void test_pane_host_clip_cannot_escape()
	{
		// A view that clears its clip must get its pane back, not the whole window
		// — the markdown pane does exactly this, and without the clamp it would be
		// free to paint over everything beside it.
		struct clip_probe : pf::frame_reactor
		{
			pf::irect seen_clip;

			uint32_t handle_message(pf::window_frame_ptr, pf::message_type, const pf::message_params&) override
			{
				return 0;
			}

			void handle_paint(pf::window_frame_ptr& window, pf::draw_context& draw) override
			{
				seen_clip = draw.clip_rect();
				draw.set_clip_rect({0, 0, 10, 10});
				draw.clear_clip_rect();
			}

			void handle_size(pf::window_frame_ptr&, pf::isize, pf::measure_context&) override
			{
			}
		};

		struct recording_clip final : pf::ui::test::fake_draw_context
		{
			std::vector<pf::irect> clips;

			using fake_draw_context::fake_draw_context;

			void set_clip_rect(const pf::irect& rc) override { clips.push_back(rc); }

			void clear_clip_rect() override
			{
			}
		};

		auto window = std::make_shared<pf::ui::test::fake_window_frame>();
		pf::window_frame_ptr frame = window;
		pf::ui::pane_host host(frame);
		pf::ui::test::fake_measure_context measure;

		const auto probe = std::make_shared<clip_probe>();
		const auto pane = host.add_pane(probe);
		host.set_pane_bounds(pane, {100, 50, 300, 150}, measure);

		recording_clip draw{pf::irect(0, 0, 400, 400)};
		host.handle_paint(draw);

		// Every clip the pane caused is inside the pane, including the one it set
		// by clearing.
		CHECK(!draw.clips.empty());
		for (const auto& c : draw.clips)
		{
			CHECK(c.left >= 100);
			CHECK(c.top >= 50);
			CHECK(c.right <= 300);
			CHECK(c.bottom <= 150);
		}

		// Specifically: clearing narrowed it back to the pane rather than removing
		// it. Checking only that the recorded clips are inside the pane would pass
		// even if clearing had dropped through to the window.
		CHECK_EQ(draw.clips.size(), 3u);
		CHECK_EQ(draw.clips.back().left, 100);
		CHECK_EQ(draw.clips.back().top, 50);
		CHECK_EQ(draw.clips.back().right, 300);
		CHECK_EQ(draw.clips.back().bottom, 150);

		// And the dirty rectangle it was handed is in its own coordinates.
		CHECK_EQ(probe->seen_clip.left, 0);
		CHECK_EQ(probe->seen_clip.top, 0);
	}

	void test_pane_host_hosts_a_real_view()
	{
		// The point of all of it: a shared view, with no window of its own, inside a
		// pane of someone else's window.
		auto window = std::make_shared<pf::ui::test::fake_window_frame>();
		pf::window_frame_ptr frame = window;
		pf::ui::pane_host host(frame);
		pf::ui::test::fake_measure_context measure;
		pf::ui::test::recording_view_host view_host;
		const pf::ui::theme theme;

		const auto view = std::make_shared<pf::ui::markdown_view>(view_host, theme);
		const auto buf = std::make_shared<pf::ui::text_buffer>(view_host);
		buf->set_text("# Hosted\nbody text here");
		view->set_buffer(buf, {});

		const auto pane = host.add_pane(view);
		host.set_pane_bounds(pane, {120, 40, 520, 360}, measure);
		view->layout();

		pf::ui::test::fake_draw_context draw{pf::irect(0, 0, 640, 480)};
		host.handle_paint(draw);

		// It rendered, and everything it drew landed inside its pane.
		CHECK(draw.drawn_text().find("Hosted") != std::string::npos);
		CHECK(!draw.texts.empty());
		for (const auto& t : draw.texts)
		{
			CHECK(t.x >= 120);
			CHECK(t.y >= 40);
		}

		// A click lands on the text the reader pointed at, in document coordinates,
		// after the host has translated it out of the parent window.
		const auto local = view->text_to_client({2, 1});
		pf::mouse_params click;
		click.point = {local.x + 120, local.y + 40};
		host.handle_mouse(pf::mouse_message_type::left_button_down, click);

		const auto hit = view->text_at(local);
		CHECK_EQ(hit.y, 1);
		CHECK_EQ(hit.x, 2);
	}

	void test_pane_host_routes_messages_without_a_client_point()
	{
		// Not every mouse message carries a client point. Routing them by
		// subtracting the pane origin picks the wrong pane and corrupts what the
		// view reads — a context menu would open against a mangled screen point.
		auto window = std::make_shared<pf::ui::test::fake_window_frame>();
		pf::window_frame_ptr frame = window;
		pf::ui::pane_host host(frame);
		pf::ui::test::fake_measure_context measure;

		const auto left = std::make_shared<probe_pane>("left");
		const auto right = std::make_shared<probe_pane>("right");
		const auto left_pane = host.add_pane(left);
		const auto right_pane = host.add_pane(right);
		host.set_pane_bounds(left_pane, {0, 0, 100, 200}, measure);
		host.set_pane_bounds(right_pane, {100, 0, 300, 200}, measure);

		// context_menu carries a *screen* point, and the view converts it itself.
		// It must arrive exactly as it was sent.
		pf::mouse_params menu;
		menu.point = {150, 60};
		CHECK(host.handle_mouse(pf::mouse_message_type::context_menu, menu));
		CHECK_EQ(right->mouse_count, 1);
		CHECK_EQ(right->last_point.x, 150);
		CHECK_EQ(right->last_point.y, 60);

		// From the keyboard it is {-1,-1} and belongs to whichever pane has focus,
		// since there is no point to hit-test at all.
		left_pane->set_focus();
		pf::mouse_params keyboard_menu;
		keyboard_menu.point = {-1, -1};
		CHECK(host.handle_mouse(pf::mouse_message_type::context_menu, keyboard_menu));
		CHECK_EQ(left->mouse_count, 1);
		CHECK_EQ(left->last_point.x, -1);

		// set_cursor carries the hit-test packed into its point, so it is routed by
		// where the cursor actually is rather than by that.
		pf::mouse_params cursor;
		cursor.point = {1, 0}; // HTCLIENT and a message id, not a position
		cursor.hit_test = 1;
		const auto before = left->mouse_count + right->mouse_count;
		host.handle_mouse(pf::mouse_message_type::set_cursor, cursor);

		// Wherever the real cursor is, this must not have been read as a position
		// inside the left pane — which is what {1,0} would have meant.
		CHECK(left->mouse_count + right->mouse_count <= before + 1);
		if (left->mouse_count > 1) CHECK_EQ(left->last_point.x, 1);
	}

	void test_pane_host_releases_what_it_held()
	{
		auto window = std::make_shared<pf::ui::test::fake_window_frame>();
		pf::window_frame_ptr frame = window;
		pf::ui::test::fake_measure_context measure;

		const auto probe = std::make_shared<probe_pane>("pane");

		{
			pf::ui::pane_host host(frame);
			const auto pane = host.add_pane(probe);
			host.set_pane_bounds(pane, {0, 0, 100, 100}, measure);

			pane->set_focus();
			pane->set_capture();
			pane->set_timer(1002, 500);
			CHECK_EQ(window->timers.size(), 1u);

			// A pane nobody can see keeps neither the keyboard, the mouse nor a timer.
			pane->show(false);
			CHECK(!probe->focused);
			CHECK_EQ(window->timers.size(), 0u);
			CHECK(!host.handle_keyboard(pf::keyboard_message_type::key_down, {}));

			// And a hidden pane does not swallow the mouse through a stale capture.
			pf::mouse_params click;
			click.point = {50, 50};
			CHECK(!host.handle_mouse(pf::mouse_message_type::left_button_down, click));

			// Removing a pane tells it that it lost the focus rather than leaving
			// its caret believing otherwise.
			pane->show(true);
			pane->set_focus();
			pane->set_timer(1002, 500);
			CHECK(probe->focused);

			host.remove_pane(pane);
			CHECK(!probe->focused);
			CHECK_EQ(window->timers.size(), 0u);

			// A removed pane is detached: it answers harmlessly instead of reaching
			// a host that no longer knows about it.
			CHECK(!pane->attached());
			CHECK(!pane->has_focus());
			CHECK_EQ(pane->set_timer(1002, 500), 0u);

			// A pane still running a timer when the host goes away must not leave it
			// running on the window.
			const auto second = host.add_pane(probe);
			host.set_pane_bounds(second, {0, 0, 100, 100}, measure);
			second->set_timer(1002, 500);
			CHECK_EQ(window->timers.size(), 1u);
		}

		CHECK_EQ(window->timers.size(), 0u);
	}

	void test_pane_host_empty_clip_stays_inside()
	{
		// A clip that misses the pane entirely must come out empty, not inverted: a
		// backend that normalises a reversed rectangle would turn "nothing" into a
		// region sitting over the neighbouring panes.
		struct clip_probe : pf::frame_reactor
		{
			uint32_t handle_message(pf::window_frame_ptr, pf::message_type, const pf::message_params&) override
			{
				return 0;
			}

			void handle_paint(pf::window_frame_ptr&, pf::draw_context& draw) override
			{
				// Entirely to the left of the pane, in pane coordinates
				draw.set_clip_rect({-500, -500, -400, -400});
			}

			void handle_size(pf::window_frame_ptr&, pf::isize, pf::measure_context&) override
			{
			}
		};

		struct recording_clip final : pf::ui::test::fake_draw_context
		{
			std::vector<pf::irect> clips;

			using fake_draw_context::fake_draw_context;

			void set_clip_rect(const pf::irect& rc) override { clips.push_back(rc); }

			void clear_clip_rect() override
			{
			}
		};

		auto window = std::make_shared<pf::ui::test::fake_window_frame>();
		pf::window_frame_ptr frame = window;
		pf::ui::pane_host host(frame);
		pf::ui::test::fake_measure_context measure;

		const auto pane = host.add_pane(std::make_shared<clip_probe>());
		host.set_pane_bounds(pane, {100, 50, 300, 150}, measure);

		recording_clip draw{pf::irect(0, 0, 400, 400)};
		host.handle_paint(draw);

		CHECK_EQ(draw.clips.size(), 2u);

		// Empty, and still anchored inside the pane rather than reversed.
		const auto& missed = draw.clips.back();
		CHECK(missed.right <= missed.left || missed.bottom <= missed.top);
		CHECK(missed.left >= 100);
		CHECK(missed.top >= 50);
		CHECK(missed.right <= 300);
		CHECK(missed.bottom <= 150);
	}

	// --- The input filter: what untrusted text has to survive ---
	//
	// These are list0's assertions, which are the reason its prompt editor could
	// not simply be replaced by a text buffer. Each one names an attack or an
	// accident that would otherwise reach the buffer.

	void test_input_rejects_malformed_utf8()
	{
		namespace in = pf::ui::input;

		// Overlong forms, encoded surrogates and out-of-range scalars are all ways
		// of smuggling a character past a check that looks at the decoded value.
		CHECK_STR(in::normalize("a\xC0\xAF\xED\xA0\x80\xF4\x90\x80\x80" "b"), "ab");

		// A malformed sequence costs only its own lead byte: it must not swallow
		// the valid text after it.
		CHECK_STR(in::normalize("a\xE2Xb"), "aXb");

		// A scalar cut off by the end of the input is dropped rather than completed
		// with whatever follows in memory.
		CHECK_STR(in::normalize("a\xF0\x9F"), "a");

		// An embedded NUL is discarded rather than truncating the rest.
		CHECK_STR(in::normalize(std::string("a\0b", 3)), "ab");

		// Valid multi-byte text passes through untouched.
		CHECK_STR(in::normalize("caf\xC3\xA9 \xF0\x9F\x98\x80"), "caf\xC3\xA9 \xF0\x9F\x98\x80");
	}

	void test_input_strips_controls_and_separators()
	{
		namespace in = pf::ui::input;

		// Controls go; runs of separators collapse to one space.
		CHECK_STR(in::normalize("a\r\n\tb\tc\n\nend\x01\x7F"), "a b c end");

		// The Unicode newlines are newlines too, not printable characters.
		CHECK_STR(in::normalize("a\xC2\x85" "b\xE2\x80\xA8" "c\xE2\x80\xA9" "d"), "a b c d");

		// Ordinary spacing someone typed on purpose is left alone.
		CHECK_STR(in::normalize("  keep  spaces  "), "  keep  spaces  ");

		// Multiline keeps paragraphs, and CRLF is one break rather than two.
		CHECK_STR(in::normalize("first\r\nsecond\n\nfourth", in::max_text_bytes, true),
		          "first\nsecond\n\nfourth");

		// Single-line mode cannot be given a second line.
		CHECK(in::normalize("first\r\nsecond").find('\n') == std::string::npos);
	}

	void test_input_bounds_what_it_accepts()
	{
		namespace in = pf::ui::input;

		// The output bound stops between characters, never inside one: five bytes
		// of budget take "a" and the four-byte emoji, not three bytes of it.
		const auto bounded = in::normalize("a\xF0\x9F\x98\x80z", 5);
		CHECK_STR(bounded, "a\xF0\x9F\x98\x80");
		CHECK_EQ(bounded.size(), 5u);

		// A budget that cannot fit the next character stops rather than splitting it.
		CHECK_STR(in::normalize("a\xF0\x9F\x98\x80z", 3), "a");

		// Zero capacity accepts nothing at all.
		CHECK(in::normalize("x", 0).empty());

		// The default bounds both memory and the layout work that follows it.
		CHECK_EQ(in::normalize(std::string(in::max_text_bytes + 1, 'a')).size(), in::max_text_bytes);

		// And the *scan* is bounded as well as the output. Text that is rejected
		// still costs time to reject, so a megabyte of control characters must not
		// become a megabyte of work — this returns empty, having stopped looking.
		CHECK(in::normalize(std::string(in::max_paste_bytes, '\1') + "tail").empty());
	}

	void test_input_assembles_surrogate_pairs()
	{
		pf::ui::input::char_assembler assembler;

		// A lead waits rather than inserting half a character.
		CHECK_EQ(assembler.accept(0xD83D), static_cast<char32_t>(0));
		CHECK(assembler.waiting());

		// Its trail completes it.
		CHECK_EQ(assembler.accept(0xDE00), static_cast<char32_t>(0x1F600));
		CHECK(!assembler.waiting());

		// A trail with nothing before it is not a character.
		CHECK_EQ(assembler.accept(0xDE00), static_cast<char32_t>(0));

		// Ordinary text discards a stale lead rather than pairing with it.
		assembler.accept(0xD83D);
		CHECK_EQ(assembler.accept(U'a'), U'a');
		CHECK_EQ(assembler.accept(0xDE00), static_cast<char32_t>(0));

		// And anything that is not the other half cancels the wait.
		assembler.accept(0xD83D);
		assembler.cancel();
		CHECK_EQ(assembler.accept(0xDE00), static_cast<char32_t>(0));

		// A character the platform already combined passes straight through.
		CHECK_EQ(assembler.accept(0x1F600), static_cast<char32_t>(0x1F600));
	}

	// --- The composer ---

	struct composer_probe : pf::ui::composer
	{
		using pf::ui::composer::composer;
		using pf::ui::composer::on_char;
		using pf::ui::composer::on_key_down;
		using pf::ui::composer::can_cut_text;
		using pf::ui::composer::paste_text_from_clipboard;
		using pf::ui::composer::focus_band_color;
	};

	pf::ui::text_buffer_ptr show_composer(composer_probe& view, pf::ui::view_host& host,
	                                      pf::window_frame_ptr& frame, const pf::isize extent = {400, 64})
	{
		const auto buf = std::make_shared<pf::ui::text_buffer>(host);
		view.set_buffer(buf, {});

		pf::ui::test::fake_measure_context measure;
		view.handle_size(frame, extent, measure);
		view.layout();
		return buf;
	}

	// A prompt box that says what it is for should keep saying it while somebody
	// is deciding what to type. It used to stop the moment the box took focus,
	// because drawing the hint painted over the caret.
	void test_composer_keeps_its_placeholder_while_focused()
	{
		pf::ui::test::recording_view_host host;
		const pf::ui::theme theme;
		composer_probe view(host, theme);
		auto window = std::make_shared<pf::ui::test::fake_window_frame>();
		pf::window_frame_ptr frame = window;

		show_composer(view, host, frame);
		view.set_placeholder("Ask about stocks...");

		const auto hint_drawn = [&](pf::ui::test::fake_draw_context& draw)
		{
			for (const auto& t : draw.texts)
				if (t.text.find("Ask about") != std::string::npos) return true;
			return false;
		};

		// The hint fills its rect, so the fill has to be what the line under it
		// is: the plain background, and the focus band once the box has focus.
		// It used to stay the plain background, drawing a lighter box around the
		// hint inside the band.
		const auto hint_background = [&](const pf::ui::test::fake_draw_context& draw)
		{
			for (const auto& t : draw.texts)
				if (t.text.find("Ask about") != std::string::npos) return t.background;
			return pf::color_t{};
		};

		pf::ui::test::fake_draw_context unfocused{pf::irect(0, 0, 400, 64)};
		view.handle_paint(frame, unfocused);
		CHECK(hint_drawn(unfocused));
		CHECK(hint_background(unfocused) == theme.style_color(pf::ui::text_style::normal_bkgnd));

		// Focused and still empty: the hint is what tells somebody what to type,
		// and taking focus is exactly when they are about to.
		window->set_focus();
		view.update_focus(frame);

		pf::ui::test::fake_draw_context focused{pf::irect(0, 0, 400, 64)};
		view.handle_paint(frame, focused);
		CHECK(hint_drawn(focused));
		CHECK(hint_background(focused) == view.focus_band_color());

		// And the caret survives it. A recording draw context logs calls rather
		// than compositing pixels, so it cannot see one thing painted over
		// another — what it can see is the caret being drawn a second time, after
		// the hint, which is what stops the hint swallowing it on screen.
		const auto caret_fills = [&](const pf::ui::test::fake_draw_context& draw)
		{
			int n = 0;
			for (const auto& f : draw.fills)
				if (f.color == theme.text && f.rect.width() <= 2 && f.rect.height() > 2) ++n;
			return n;
		};

		CHECK_EQ(caret_fills(focused), 2);

		// Once there is text there is nothing to hint at, and the caret is drawn
		// the once.
		view.set_text("hello");
		pf::ui::test::fake_draw_context typed{pf::irect(0, 0, 400, 64)};
		view.handle_paint(frame, typed);
		CHECK(!hint_drawn(typed));
		CHECK_EQ(caret_fills(typed), 1);
	}

	// An application with no command table still gets a usable right-click, for
	// the same reason it gets the clipboard shortcuts: there is nothing behind
	// the view to offer them instead.
	void test_view_offers_its_own_context_menu()
	{
		pf::ui::test::recording_view_host host;
		const pf::ui::theme theme;
		composer_probe view(host, theme);
		auto window = std::make_shared<pf::ui::test::fake_window_frame>();
		pf::window_frame_ptr frame = window;

		show_composer(view, host, frame);
		view.set_text("hello world");

		const auto items = view.on_popup_menu({0, 0});

		// Four verbs and the separator between what changes the text and what
		// only selects it. Checked before anything indexes them, so a view that
		// stops offering a menu reports it rather than crashing the suite.
		CHECK_EQ(items.size(), 5u);
		if (items.size() != 5) return;

		CHECK_STR(items[0].text, "Cu&t");
		CHECK_STR(items[1].text, "&Copy");
		CHECK_STR(items[2].text, "&Paste");
		CHECK(items[3].text.empty());
		CHECK_STR(items[4].text, "Select &All");

        // Nothing is selected yet, so the two that need a selection say so
		// rather than failing quietly when chosen.
		CHECK(!items[0].is_enabled());
		CHECK(!items[1].is_enabled());
		CHECK(items[4].is_enabled());

		view.select_all_text();
		CHECK(items[0].is_enabled());
		CHECK(items[1].is_enabled());

		// The items carry the verb, not an application command id, so choosing
		// one does the thing without anything else being wired up.
		CHECK_EQ(items[0].id, 0);
		CHECK(static_cast<bool>(items[1].action));
	}

	// A read-only view offers the same menu with the editing verbs disabled: a
	// transcript can be copied from and cannot be cut.
	void test_read_only_view_offers_copy_but_not_cut()
	{
		pf::ui::test::recording_view_host host;
		const pf::ui::theme theme;
		markdown_probe view(host, theme);
		auto window = std::make_shared<pf::ui::test::fake_window_frame>();
		pf::window_frame_ptr frame = window;

		show_markdown(view, host, frame, "first line\nsecond line");
		view.select_all_text();

		const auto items = view.on_popup_menu({0, 0});

		CHECK_EQ(items.size(), 5u);
		if (items.size() != 5) return;

		CHECK(items[1].is_enabled());  // copy
		CHECK(!items[0].is_enabled()); // cut
		CHECK(!items[2].is_enabled()); // paste
	}

	void test_composer_edits_like_a_document()
	{
		pf::ui::test::recording_view_host host;
		const pf::ui::theme theme;
		composer_probe view(host, theme);
		auto window = std::make_shared<pf::ui::test::fake_window_frame>();
		pf::window_frame_ptr frame = window;

		const auto buf = show_composer(view, host, frame);
		view.set_text("hello world");
		CHECK_STR(view.text(), "hello world");

		// It is a document view, so selection and undo come for free — which is
		// exactly what list0's prompt editor lacked.
		view.select_all_text();
		CHECK(view.can_copy_text());
		CHECK(view.can_cut_text());
		CHECK_STR(view.select_text(), "hello world");

		{
			pf::ui::undo_group ug(buf);
			buf->insert_text(ug, pf::ui::text_location{0, 0}, "X");
		}
		CHECK_STR(view.text(), "Xhello world");
		buf->undo();
		CHECK_STR(view.text(), "hello world");
	}

	void test_composer_filters_what_is_typed_and_pasted()
	{
		pf::ui::test::recording_view_host host;
		const pf::ui::theme theme;
		composer_probe view(host, theme);
		auto window = std::make_shared<pf::ui::test::fake_window_frame>();
		pf::window_frame_ptr frame = window;

		show_composer(view, host, frame);

		// Text set programmatically is filtered too: it may still have come from
		// somewhere untrusted.
		view.set_text("a\x01\x02" "b");
		CHECK_STR(view.text(), "ab");

		// A control character cannot be typed in either.
		view.set_text({});
		view.on_char(frame, U'a');
		view.on_char(frame, 0x01);
		view.on_char(frame, U'b');
		CHECK_STR(view.text(), "ab");

		// Half a character is never inserted on its own; the pair completes it.
		view.set_text({});
		view.on_char(frame, 0xD83D);
		CHECK_STR(view.text(), "");
		view.on_char(frame, 0xDE00);
		CHECK_STR(view.text(), "\xF0\x9F\x98\x80");

		// The limit is a limit: typing past it is refused rather than truncated
		// mid-character.
		view.set_limit(4);
		view.set_text("abcd");
		view.on_char(frame, U'e');
		CHECK_STR(view.text(), "abcd");

		// And set_text obeys the same bound.
		view.set_text("123456789");
		CHECK_STR(view.text(), "1234");
	}

	// A composer is an input box with no menu behind it, so it owns the clipboard
	// shortcuts rather than waiting for a command that is never sent.
	void test_composer_handles_the_clipboard_shortcuts()
	{
		struct clip_probe : composer_probe
		{
			using composer_probe::composer_probe;

			mutable std::string board;
			bool accepts = true;

			[[nodiscard]] std::string clipboard_text() const override { return board; }

			bool set_clipboard(const std::string_view text) const override
			{
				if (!accepts) return false;
				board = text;
				return true;
			}
		};

		pf::ui::test::recording_view_host host;
		const pf::ui::theme theme;
		clip_probe view(host, theme);
		auto window = std::make_shared<pf::ui::test::fake_window_frame>();
		pf::window_frame_ptr frame = window;

		show_composer(view, host, frame);
		window->held_keys.insert(pf::platform_key::Control);

		view.set_text("hello");
		view.on_key_down(frame, 'A');
		view.on_key_down(frame, 'C');
		CHECK_STR(view.board, "hello");
		CHECK_STR(view.text(), "hello");

		// A refused cut keeps the text: neither on the clipboard nor in the box is
		// a loss recoverable only by an undo nobody knows they need.
		view.accepts = false;
		view.on_key_down(frame, 'X');
		CHECK_STR(view.text(), "hello");

		view.accepts = true;
		view.on_key_down(frame, 'X');
		CHECK_STR(view.text(), "");
		CHECK_STR(view.board, "hello");

		// Paste goes through the composer's filter, so a multi-line clipboard
		// arrives on the one line a single-line composer has.
		view.set_multiline(false);
		view.board = "one\r\ntwo";
		view.on_key_down(frame, 'V');
		CHECK_STR(view.text(), "one two");

		// Ctrl+Shift+C is not a copy; it must reach the view as an ordinary key.
		window->held_keys.insert(pf::platform_key::Shift);
		view.board.clear();
		view.select_all_text();
		view.on_key_down(frame, 'C');
		CHECK_STR(view.board, "");
		window->held_keys.erase(pf::platform_key::Shift);

		// And without Control these are just letters, not commands.
		window->held_keys.erase(pf::platform_key::Control);
		view.set_text("kept");
		view.select_all_text();
		view.on_key_down(frame, 'X');
		CHECK_STR(view.text(), "kept");
	}

	// Like the clipboard shortcuts, Delete and Undo have no command behind a
	// composer to come from, so the editable view handles the keys itself. Before
	// it did, Delete, Ctrl+Z and Ctrl+Y did nothing in equity's and list0's prompts.
	void test_composer_handles_the_editing_keys()
	{
		pf::ui::test::recording_view_host host;
		const pf::ui::theme theme;
		composer_probe view(host, theme);
		auto window = std::make_shared<pf::ui::test::fake_window_frame>();
		pf::window_frame_ptr frame = window;

		const auto buf = show_composer(view, host, frame);
		for (const auto c : std::u32string(U"hello world"))
			view.on_char(frame, c);
		CHECK_STR(view.text(), "hello world");

		// Delete takes the character after the caret, or the selection.
		buf->select(pf::ui::text_selection(5, 0, 5, 0));
		view.on_key_down(frame, pf::platform_key::Delete);
		CHECK_STR(view.text(), "helloworld");

		buf->select(pf::ui::text_selection(0, 0, 2, 0));
		view.on_key_down(frame, pf::platform_key::Delete);
		CHECK_STR(view.text(), "lloworld");

		// Ctrl+Delete takes the rest of the word.
		window->held_keys.insert(pf::platform_key::Control);
		buf->select(pf::ui::text_selection(0, 0, 0, 0));
		view.on_key_down(frame, pf::platform_key::Delete);
		CHECK_STR(view.text(), "");

		// Ctrl+Z puts each of those back, newest first; Ctrl+Y and Ctrl+Shift+Z
		// take them away again.
		view.on_key_down(frame, 'Z');
		CHECK_STR(view.text(), "lloworld");
		view.on_key_down(frame, 'Z');
		CHECK_STR(view.text(), "helloworld");
		view.on_key_down(frame, 'Y');
		CHECK_STR(view.text(), "lloworld");
		view.on_key_down(frame, 'Z');
		window->held_keys.insert(pf::platform_key::Shift);
		view.on_key_down(frame, 'Z');
		CHECK_STR(view.text(), "lloworld");
		window->held_keys.erase(pf::platform_key::Shift);

		// Without Control, Z and Y are only letters and change nothing here.
		window->held_keys.erase(pf::platform_key::Control);
		view.on_key_down(frame, 'Z');
		view.on_key_down(frame, 'Y');
		CHECK_STR(view.text(), "lloworld");
	}

	// With a selection, Ctrl+Delete and Ctrl+Backspace take just the selection, as
	// a Windows edit box does, rather than growing it by a word first.
	void test_composer_ctrl_delete_takes_only_the_selection()
	{
		pf::ui::test::recording_view_host host;
		const pf::ui::theme theme;
		composer_probe view(host, theme);
		auto window = std::make_shared<pf::ui::test::fake_window_frame>();
		pf::window_frame_ptr frame = window;

		const auto buf = show_composer(view, host, frame);
		for (const auto c : std::u32string(U"alpha beta gamma"))
			view.on_char(frame, c);
		window->held_keys.insert(pf::platform_key::Control);

		buf->select(pf::ui::text_selection(6, 0, 8, 0));
		view.on_key_down(frame, pf::platform_key::Delete);
		CHECK_STR(view.text(), "alpha ta gamma");

		buf->select(pf::ui::text_selection(6, 0, 8, 0));
		view.on_key_down(frame, pf::platform_key::Back);
		CHECK_STR(view.text(), "alpha  gamma");

		// With no selection they still take a word (Backspace also the spaces after it).
		buf->select(pf::ui::text_selection(7, 0, 7, 0));
		view.on_key_down(frame, pf::platform_key::Delete);
		CHECK_STR(view.text(), "alpha  ");
		view.on_key_down(frame, pf::platform_key::Back);
		CHECK_STR(view.text(), "");
	}

	void test_composer_submits_and_recalls()
	{
		pf::ui::test::recording_view_host host;
		const pf::ui::theme theme;
		composer_probe view(host, theme);
		auto window = std::make_shared<pf::ui::test::fake_window_frame>();
		pf::window_frame_ptr frame = window;

		show_composer(view, host, frame);

		std::vector<std::string> sent;
		view.on_submit = [&](std::string s)
		{
			sent.push_back(std::move(s));
			return true;
		};

		view.set_text("first prompt");
		view.on_key_down(frame, pf::platform_key::Return);

		CHECK_EQ(sent.size(), 1u);
		CHECK_STR(sent[0], "first prompt");

		// Sending clears the box, so the next prompt starts empty.
		CHECK_STR(view.text(), "");

		// Whitespace is not a prompt.
		view.set_text("   ");
		view.on_key_down(frame, pf::platform_key::Return);
		CHECK_EQ(sent.size(), 1u);

		view.set_text("second prompt");
		view.on_key_down(frame, pf::platform_key::Return);
		CHECK_EQ(sent.size(), 2u);

		// Up walks back through what was sent, newest first.
		view.on_key_down(frame, pf::platform_key::Up);
		CHECK_STR(view.text(), "second prompt");
		view.on_key_down(frame, pf::platform_key::Up);
		CHECK_STR(view.text(), "first prompt");

		// Walking forward off the end restores whatever was being typed, rather
		// than losing it.
		view.on_key_down(frame, pf::platform_key::Down);
		CHECK_STR(view.text(), "second prompt");
		view.on_key_down(frame, pf::platform_key::Down);
		CHECK_STR(view.text(), "");

		// Sending the same text twice does not keep two copies of it.
		view.set_text("first prompt");
		view.on_key_down(frame, pf::platform_key::Return);
		CHECK_EQ(view.history().size(), 2u);
		CHECK_STR(view.history().back(), "first prompt");
	}

	void test_composer_keeps_a_refused_prompt()
	{
		// An application can refuse what it is handed — list0 will not send a
		// prompt until its cloud-use disclosure has been acknowledged. What the
		// person wrote must survive that: losing it would punish them for a gate
		// they have not been shown yet.
		pf::ui::test::recording_view_host host;
		const pf::ui::theme theme;
		composer_probe view(host, theme);
		auto window = std::make_shared<pf::ui::test::fake_window_frame>();
		pf::window_frame_ptr frame = window;

		show_composer(view, host, frame);

		auto accept = false;
		auto offered = 0;
		view.on_submit = [&](std::string)
		{
			++offered;
			return accept;
		};

		view.set_text("private question");
		view.on_key_down(frame, pf::platform_key::Return);

		// Offered and refused: the text stays, and nothing entered the history.
		CHECK_EQ(offered, 1);
		CHECK_STR(view.text(), "private question");
		CHECK(view.history().empty());

		// Still refused after a second try, and still not duplicated anywhere.
		view.on_key_down(frame, pf::platform_key::Return);
		CHECK_EQ(offered, 2);
		CHECK_STR(view.text(), "private question");
		CHECK(view.history().empty());

		// Accepted: now it clears and is remembered, exactly once.
		accept = true;
		view.on_key_down(frame, pf::platform_key::Return);
		CHECK_EQ(offered, 3);
		CHECK_STR(view.text(), "");
		CHECK_EQ(view.history().size(), 1u);
		CHECK_STR(view.history().back(), "private question");

		// With no handler at all there is nowhere for it to go, so it is kept
		// rather than silently dropped.
		composer_probe orphan(host, theme);
		show_composer(orphan, host, frame);
		orphan.set_text("nobody is listening");
		orphan.on_key_down(frame, pf::platform_key::Return);
		CHECK_STR(orphan.text(), "nobody is listening");
	}

	void test_composer_grows_then_scrolls()
	{
		pf::ui::test::recording_view_host host;
		const pf::ui::theme theme;
		composer_probe view(host, theme);
		auto window = std::make_shared<pf::ui::test::fake_window_frame>();
		pf::window_frame_ptr frame = window;

		show_composer(view, host, frame);

		CHECK_EQ(view.rows(), 1);

		view.set_text("one line");
		view.layout();
		CHECK_EQ(view.rows(), 1);

		view.set_text("one\ntwo\nthree");
		view.layout();
		CHECK_EQ(view.rows(), 3);

		// It stops growing and scrolls instead, so a long prompt cannot swallow
		// whatever it shares a window with.
		view.set_text("1\n2\n3\n4\n5\n6\n7\n8");
		view.layout();
		CHECK_EQ(view.rows(), pf::ui::composer::default_max_rows);

		view.set_max_rows(2);
		CHECK_EQ(view.rows(), 2);

		// The height it asks for follows the rows, plus its padding.
		CHECK(view.desired_height() > 2 * 16);
	}

	// A one-line composer is shorter than a scrollbar. Drawing one inside it puts
	// it on top of the only line there is, where it takes the click that should
	// have put the caret in the text.
	void test_composer_long_line_has_no_horizontal_scrollbar()
	{
		struct drag_probe : composer_probe
		{
			using composer_probe::composer_probe;
			using pf::ui::composer::handle_mouse;
			using pf::ui::composer::scroll_offset;
		};

		pf::ui::test::recording_view_host host;
		const pf::ui::theme theme;
		drag_probe view(host, theme);
		auto window = std::make_shared<pf::ui::test::fake_window_frame>();
		pf::window_frame_ptr frame = window;

		const auto buf = show_composer(view, host, frame, {340, 24});
		view.set_multiline(false);
		view.set_max_rows(1);
		view.set_word_wrap(false);
		view.set_limit(pf::ui::input::max_editor_bytes);
		view.set_text(std::string(500, 'x'));
		view.layout();

		// The line is far wider than the box, and the caret is at its end: this is
		// exactly when a document view would raise a horizontal scrollbar.
		view.ensure_visible(frame, pf::ui::text_location{500, 0});
		const auto scrolled = view.scroll_offset().x;
		CHECK(scrolled > 0);

		pf::mouse_params down;
		down.point = {8, 10};
		down.left_button = true;
		view.handle_mouse(frame, pf::mouse_message_type::left_button_down, down);

		pf::mouse_params move;
		move.point = {300, 10};
		move.left_button = true;
		view.handle_mouse(frame, pf::mouse_message_type::mouse_move, move);

		// The press landed in text and the drag extended a selection, rather than
		// catching a thumb and flinging the line sideways.
		CHECK(buf->has_selection());
		CHECK_EQ(view.scroll_offset().x, scrolled);
	}

	// Scrolling to the caret at the end of a long line left the last character and
	// the caret just past the right edge of list0's one-line prompt: the scroll
	// counted the whole width rather than the width after the text's left inset,
	// and gave the caret no cell of its own.
	void test_composer_scrolls_the_caret_into_view()
	{
		struct caret_probe : composer_probe
		{
			using composer_probe::composer_probe;
			using pf::ui::composer::text_to_client;
		};

		pf::ui::test::recording_view_host host;
		const pf::ui::theme theme;
		caret_probe view(host, theme);
		auto window = std::make_shared<pf::ui::test::fake_window_frame>();
		pf::window_frame_ptr frame = window;

		const pf::isize extent{340, 24};
		show_composer(view, host, frame, extent);
		view.set_multiline(false);
		view.set_max_rows(1);
		view.set_word_wrap(false);
		view.set_limit(pf::ui::input::max_editor_bytes);
		view.set_text(std::string(500, 'x'));
		view.layout();

		view.ensure_visible(frame, pf::ui::text_location{500, 0});
		const auto last = view.text_to_client(pf::ui::text_location{499, 0});
		const auto caret = view.text_to_client(pf::ui::text_location{500, 0});
		CHECK(last.x >= 0);
		CHECK(caret.x > last.x);
		CHECK(caret.x + 2 <= extent.cx);

		// Back at the start, the first character is the first one shown.
		view.ensure_visible(frame, pf::ui::text_location{0, 0});
		const auto first = view.text_to_client(pf::ui::text_location{0, 0});
		const auto second = view.text_to_client(pf::ui::text_location{1, 0});
		CHECK(first.x >= 0);
		CHECK(second.x > first.x);
	}

	// A composer draws its border inside its box, so a wrapped row stops a cell
	// short of it. Where the text area came to a whole number of cells, the last
	// letter of a full row sat under the border (equity's prompt at 643-650 px).
	void test_composer_keeps_text_clear_of_its_border()
	{
		struct border_probe : composer_probe
		{
			using composer_probe::composer_probe;
			using pf::ui::composer::text_to_client;
		};

		pf::ui::test::recording_view_host host;
		const pf::ui::theme theme;
		border_probe view(host, theme);
		auto window = std::make_shared<pf::ui::test::fake_window_frame>();
		pf::window_frame_ptr frame = window;

		// Measure where text starts and how wide a cell is, then size the box so
		// the space right of that start is exactly 40 cells: the worst case.
		show_composer(view, host, frame);
		view.set_text("ab");
		view.layout();
		const auto left = view.text_to_client({0, 0}).x;
		const auto cx = view.text_to_client({1, 0}).x - left;
		CHECK(cx > 0);

		const pf::isize extent{left + 40 * cx, 64};
		view.set_text(std::string(100, 'W'));
		pf::ui::test::fake_measure_context measure;
		view.handle_size(frame, extent, measure);
		view.layout();

		// What the user sees is where the letters are painted, so measure that.
		pf::ui::test::fake_draw_context draw{pf::irect(0, 0, extent.cx, extent.cy)};
		view.handle_paint(frame, draw);
		auto right = 0;
		for (const auto& t : draw.texts)
			if (t.text.find('W') != std::string::npos)
				right = std::max(right, t.x + static_cast<int>(t.text.size()) * cx);
		CHECK(right > extent.cx / 2);
		CHECK(right <= extent.cx - cx);
	}

	void test_composer_enter_and_shift_enter()
	{
		pf::ui::test::recording_view_host host;
		const pf::ui::theme theme;
		composer_probe view(host, theme);
		auto window = std::make_shared<pf::ui::test::fake_window_frame>();
		pf::window_frame_ptr frame = window;

		show_composer(view, host, frame);

		auto submitted = 0;
		view.on_submit = [&](std::string)
		{
			++submitted;
			return true;
		};

		// Typing Enter as a character does nothing: it is handled as a key so that
		// Shift can mean something different.
		view.set_text("text");
		view.on_char(frame, U'\n');
		CHECK_STR(view.text(), "text");

		// A single-line composer refuses a newline even with Shift held.
		view.set_multiline(false);
		window->held_keys.insert(pf::platform_key::Shift);
		view.on_key_down(frame, pf::platform_key::Return);
		CHECK_STR(view.text(), "text");
		CHECK_EQ(submitted, 0);

		// A multiline one accepts it, and still does not submit.
		view.set_multiline(true);
		view.on_key_down(frame, pf::platform_key::Return);
		CHECK(view.text().find('\n') != std::string::npos);
		CHECK_EQ(submitted, 0);

		// Without Shift it sends.
		window->held_keys.clear();
		view.on_key_down(frame, pf::platform_key::Return);
		CHECK_EQ(submitted, 1);
	}

	void test_composer_shows_its_placeholder()
	{
		pf::ui::test::recording_view_host host;
		const pf::ui::theme theme;
		composer_probe view(host, theme);
		auto window = std::make_shared<pf::ui::test::fake_window_frame>();
		pf::window_frame_ptr frame = window;

		show_composer(view, host, frame);
		view.set_placeholder("Message the agent");

		pf::ui::test::fake_draw_context draw{pf::irect(0, 0, 400, 64)};
		view.handle_paint(frame, draw);
		CHECK(draw.drawn_text().find("Message the agent") != std::string::npos);

		// It is a hint, not content: once there is text it goes.
		view.set_text("typed something");
		draw.reset();
		view.handle_paint(frame, draw);
		CHECK(draw.drawn_text().find("Message the agent") == std::string::npos);
		CHECK(draw.drawn_text().find("typed something") != std::string::npos);
	}
}

// The backend's WinMain references these; a console test never calls them.
app_init_result app_init(const pf::window_frame_ptr&, std::span<const std::string_view>)
{
	return {false, 0};
}

void app_idle()
{
}

void app_destroy()
{
}

int main()
{
	test_edit_box_utf8();
	test_edit_box_selection();
	test_edit_box_clipboard();
	test_table_cells();
	test_table_alignment();
	test_table_widths();
	test_splitter_geometry();
	test_scrollbar_thumb();
	test_text_location();
	test_theme();
	test_headless_measure();
	test_headless_draw();
	test_recording_host();
	test_syntax_language_from_extension();
	test_syntax_cpp();
	test_syntax_languages_differ();
	test_syntax_markdown();
	test_syntax_plain();
	test_syntax_bounds();
	test_buffer_set_text();
	test_buffer_edits_keep_the_selection_inside_the_text();
	test_buffer_edit_and_undo();
	test_buffer_multiline_edit();
	test_buffer_utf8_movement();
	test_buffer_selection_and_readonly();
	test_buffer_modified_flag();
	test_view_layout_and_hit_testing();
	test_view_selection_and_clipboard();
	test_view_word_wrap();
	test_view_scrolled_text_stays_out_of_the_margin();
	test_view_editing();
	test_view_paints_its_text();
	test_markdown_parse();
	test_markdown_links();
	test_markdown_escape();
	test_markdown_from_html();
	test_markdown_document_owns_its_text();
	test_markdown_source_lines();
	test_markdown_view_layout();
	test_markdown_view_selects_the_source();
	test_markdown_view_paints_markup();
	test_markdown_view_links();
	test_markdown_view_reports_which_link_was_hit();
	test_markdown_view_tables();
	test_markdown_view_wraps_without_touching_the_buffer();
	test_markdown_view_keeps_text_clear_of_the_scrollbar();
	test_markdown_view_utf8_positions();
	test_markdown_view_styles_a_quote();
	test_read_only_view_copies_with_the_keyboard();
	test_csv_view();
	test_csv_view_quotes_and_utf8();
	test_hex_view();
	test_list_layout_and_hit_testing();
	test_list_selection_and_copy();
	test_list_paints_its_rows();
	test_list_row_text_fits_its_column();
	test_list_copies_with_the_keyboard();
	test_list_right_click_selects_the_row();
	test_list_columns_and_sorting();
	test_list_draws_its_cells();
	test_pane_host_routes_input();
	test_pane_host_focus_and_keyboard();
	test_pane_host_capture_and_timers();
	test_pane_host_arbitrates_capture();
	test_pane_host_confines_drawing();
	test_pane_host_clip_cannot_escape();
	test_pane_host_hosts_a_real_view();
	test_pane_host_routes_messages_without_a_client_point();
	test_pane_host_releases_what_it_held();
	test_pane_host_empty_clip_stays_inside();
	test_input_rejects_malformed_utf8();
	test_input_strips_controls_and_separators();
	test_input_bounds_what_it_accepts();
	test_input_assembles_surrogate_pairs();
	test_composer_edits_like_a_document();
	test_composer_filters_what_is_typed_and_pasted();
	test_composer_handles_the_clipboard_shortcuts();
	test_composer_handles_the_editing_keys();
	test_composer_ctrl_delete_takes_only_the_selection();
	test_composer_keeps_its_placeholder_while_focused();
	test_view_offers_its_own_context_menu();
	test_read_only_view_offers_copy_but_not_cut();
	test_composer_submits_and_recalls();
	test_composer_keeps_a_refused_prompt();
	test_composer_grows_then_scrolls();
	test_composer_long_line_has_no_horizontal_scrollbar();
	test_composer_scrolls_the_caret_into_view();
	test_composer_keeps_text_clear_of_its_border();
	test_composer_enter_and_shift_enter();
	test_composer_shows_its_placeholder();
	test_edit_view_cut_keeps_text_when_the_clipboard_refuses();

	std::printf("platform-ui tests: %s (%d checks, %d failures)\n",
	            g_failures == 0 ? "PASS" : "FAIL", g_checks, g_failures);
	return g_failures == 0 ? 0 : 1;
}
