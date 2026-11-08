// This file is part of libdesktop, a C++20 platform layer for desktop applications
// Copyright 2026  Zac Walker
//
// This program is free software; you can redistribute it and / or modify it
// under the terms of the LGPL License either version 2.1 or later.
// License details are available at https://www.gnu.org/licenses/lgpl-2.1.html
// This program is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY

// Purpose: Headless tests for the portable text models, palette, and clipboard seam.

#include "libdesktop/ui/text/markdown.h"
#include "libdesktop/ui/text/spell.h"
#include "libdesktop/ui/text/table_layout.h"
#include "libdesktop/ui/text/text_buffer.h"
#include "libdesktop/ui/text/text_line.h"
#include "libdesktop/ui/test_support.h"
#include "libdesktop/ui/views/view_theme.h"
#include "test.h"

#include <array>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace
{
	using namespace std::literals;

	void should_split_table_cells()
	{
		namespace tl = ui::table_layout;
		assert_equal_strict("abc", tl::trim_cell("  abc  "sv), "trimmed cell");
		assert_equal_strict("", tl::trim_cell("   "sv), "blank cell");

		std::vector<std::string_view> cells;
		tl::split_pipe_cells(cells, "| a | b |"sv);
		assert_equal(uint32_t{2}, static_cast<uint32_t>(cells.size()), "pipe cell count");
		assert_equal_strict("a", tl::trim_cell(cells[0]), "first pipe cell");
		assert_equal_strict("b", tl::trim_cell(cells[1]), "second pipe cell");

		tl::split_csv_cells(cells, "\"x,y\",z"sv);
		assert_equal(uint32_t{2}, static_cast<uint32_t>(cells.size()), "quoted CSV count");
		assert_equal_strict("x,y", cells[0], "quoted comma");
		assert_equal_strict("z", cells[1], "CSV tail");

		tl::split_csv_cells(cells, "1,2,"sv);
		assert_equal(uint32_t{3}, static_cast<uint32_t>(cells.size()), "trailing CSV field");
		assert_equal_strict("", cells[2], "trailing empty field");

		std::string storage;
		tl::split_csv_cells(cells, "gamma,\"has \"\"quotes\"\"\",ab\"\"cd"sv, &storage);
		assert_equal(uint32_t{3}, static_cast<uint32_t>(cells.size()), "escaped CSV count");
		assert_equal_strict("has \"quotes\"", cells[1], "escaped quote");
		assert_equal_strict("ab\"\"cd", cells[2], "unquoted quote");
	}

	void should_classify_and_cap_table_columns()
	{
		namespace tl = ui::table_layout;
		assert_equal(true, tl::is_right_align_cell("42"sv), "integer alignment");
		assert_equal(true, tl::is_right_align_cell("€99"sv), "currency alignment");
		assert_equal(true, tl::is_right_align_cell("12%"sv), "percent alignment");
		assert_equal(false, tl::is_right_align_cell("12a"sv), "word alignment");

		std::vector<int> widths{4, 60, 3};
		tl::cap_col_widths(widths, 40);
		assert_equal(4, widths[0], "short first column stays natural");
		assert_equal(3, widths[2], "short last column stays natural");
		assert_equal(23, widths[1], "wide column receives remaining room");

		widths = {50, 50, 50};
		tl::cap_col_widths(widths, 4);
		for (const auto width : widths) assert_equal(true, width >= 1, "column keeps one cell");
	}

	void should_measure_and_record_headless_drawing()
	{
		ui::test::fake_measure_context measure;
		assert_equal(8, measure.metrics.char_width, "grid width");
		assert_equal(32, measure.measure_text("abcd"sv, render::font_face::dialog,
		                                     render::text_style::single_line, 100).cx,
		             "ASCII grid measurement");
		assert_equal(24, measure.measure_text("a\xC3\xA9\xE2\x82\xAC"sv, render::font_face::dialog,
		                                     render::text_style::single_line, 100).cx,
		             "UTF-8 grid measurement");

		ui::test::fake_draw_context draw;
		draw.draw_rect(core::recti{0, 0, 10, 10}, core::color(core::rgba(1, 2, 3)));
		draw.draw_text("hello"sv, core::recti{4, 8, 36, 24}, render::font_face::dialog,
		               render::text_style::single_line, core::color(core::rgba(255, 255, 255)),
		               core::color(core::rgba(0, 0, 0, 0)));
		assert_equal(uint32_t{1}, static_cast<uint32_t>(draw.fills.size()), "recorded fill count");
		assert_equal(uint32_t{1}, static_cast<uint32_t>(draw.texts.size()), "recorded text count");
		assert_equal_strict("hello", draw.drawn_text(), "recorded text content");

		std::array<core::pointd, 2> points{{{0, 0}, {4, 4}}};
		draw.draw_line(points[0], points[1], core::color(core::rgba(1, 2, 3)));
		draw.draw_polyline(points, core::color(core::rgba(1, 2, 3)));
		draw.fill_ellipse(core::recti{1, 2, 3, 4}, core::color(core::rgba(1, 2, 3)));
		assert_equal(uint32_t{1}, static_cast<uint32_t>(draw.lines.size()), "recorded line count");
		assert_equal(uint32_t{1}, static_cast<uint32_t>(draw.polylines.size()), "recorded polyline count");
		assert_equal(uint32_t{1}, static_cast<uint32_t>(draw.ellipses.size()), "recorded ellipse count");
	}

	void should_record_view_host_requests()
	{
		ui::test::recording_view_host host;
		host.lines_changed(3, 5);
		host.invalidate_lines(1, 2);
		host.line_count_changed(7, -2);
		host.ensure_visible(ui::text_location{4, 9});
		host.invalidate_view();
		assert_equal(uint32_t{1}, static_cast<uint32_t>(host.changed.size()), "changed request");
		assert_equal(uint32_t{1}, static_cast<uint32_t>(host.invalidated.size()), "invalidate request");
		assert_equal(-2, host.count_changes[0].second, "line count delta");
		assert_equal(true, host.ensured[0] == ui::text_location{4, 9}, "ensure request");
		assert_equal(1, host.invalidate_view_count, "view invalidation count");
		host.reset();
		assert_equal(uint32_t{0}, static_cast<uint32_t>(host.changed.size()), "host reset");
	}

	void should_edit_text_and_undo()
	{
		ui::test::recording_view_host host;
		ui::text_buffer buffer(host);
		assert_equal(uint32_t{1}, static_cast<uint32_t>(buffer.size()), "empty buffer line");
		buffer.set_text("hello world"sv);
		{
			ui::undo_group group(buffer);
			buffer.insert_text(group, ui::text_location{5, 0}, ","sv);
		}
		assert_equal_strict("hello, world", buffer.str(), "inserted text");
		assert_equal(true, buffer.can_undo(), "undo available");
		buffer.undo();
		assert_equal_strict("hello world", buffer.str(), "undo text");
		buffer.redo();
		assert_equal_strict("hello, world", buffer.str(), "redo text");

		{
			ui::undo_group group(buffer);
			buffer.delete_text(group, ui::text_selection{0, 0, 7, 0});
		}
		assert_equal_strict("world", buffer.str(), "deleted range");
		buffer.undo();
		assert_equal_strict("hello, world", buffer.str(), "undo deleted range");

		buffer.select(ui::text_selection{0, 0, 7, 0});
		assert_equal_strict("hello, ", buffer.copy(), "selected text");
		buffer.read_only(true);
		buffer.edit_paste("nope"sv);
		assert_equal_strict("hello, world", buffer.str(), "read-only edit");

		buffer.read_only(false);
		buffer.set_text("clean"sv);
		buffer.set_modified(false);
		{
			ui::undo_group group(buffer);
			buffer.insert_text(group, buffer.end(), "!"sv);
		}
		assert_equal(true, buffer.is_modified(), "modified after edit");
		buffer.undo();
		assert_equal(false, buffer.is_modified(), "undo returns to clean state");
	}

	void should_split_lines_and_move_utf8()
	{
		ui::test::recording_view_host host;
		ui::text_buffer buffer(host);
		buffer.set_text("first\nsecond"sv);
		{
			ui::undo_group group(buffer);
			buffer.insert_text(group, ui::text_location{5, 0}, "\n"sv);
		}
		assert_equal(uint32_t{3}, static_cast<uint32_t>(buffer.size()), "newline split");
		assert_equal(true, !host.count_changes.empty(), "line count notification");
		buffer.undo();
		assert_equal_strict("first\nsecond", buffer.str(), "newline undo");

		{
			ui::undo_group group(buffer);
			buffer.delete_text(group, ui::text_location{0, 1});
		}
		assert_equal_strict("firstsecond", buffer.str(), "newline join");

		buffer.set_text("a\xC3\xA9\xE2\x82\xAC"sv);
		buffer.cursor_pos({0, 0});
		buffer.move_char_right(false);
		assert_equal(1, buffer.cursor_pos().x, "ASCII byte movement");
		buffer.move_char_right(false);
		assert_equal(3, buffer.cursor_pos().x, "two-byte movement");
		buffer.move_char_right(false);
		assert_equal(6, buffer.cursor_pos().x, "three-byte movement");
		buffer.move_char_left(false);
		assert_equal(3, buffer.cursor_pos().x, "UTF-8 backward movement");
	}

	void should_clamp_buffer_locations_after_replacement()
	{
		ui::test::recording_view_host host;
		ui::text_buffer buffer(host);
		buffer.set_text("one\ntwo\nthree"sv);
		buffer.anchor_pos({1, 1});
		buffer.cursor_pos({5, 2});
		buffer.select({1, 1, 5, 2});
		buffer.set_text("x"sv);
		assert_equal(0, buffer.cursor_pos().y, "clamped cursor line");
		assert_equal(0, buffer.anchor_pos().y, "clamped anchor line");
		assert_equal(0, buffer.selection()._end.y, "clamped selection line");

		buffer.set_text("abcdefghij\nnext"sv);
		buffer.cursor_pos({9, 0});
		{
			ui::undo_group group(buffer);
			buffer.delete_text(group, ui::text_selection{0, 0, 8, 0});
		}
		assert_equal_strict("ij\nnext", buffer.str(), "shortened line");
		assert_equal(true, buffer.cursor_pos().x <= 2, "shortened cursor");

		std::string transcript;
		for (int i = 0; i != 40; ++i) transcript += "line " + std::to_string(i) + "\n";
		buffer.set_text(transcript);
		buffer.anchor_pos({0, 0});
		buffer.cursor_pos(buffer.end());
		buffer.select(buffer.all());
		{
			ui::undo_group group(buffer);
			buffer.replace_text(group, buffer.all(), "one\ntwo");
		}
		assert_equal(2u, static_cast<uint32_t>(buffer.size()), "replacement line count");
		assert_equal(true, buffer.selection()._end.y < static_cast<int>(buffer.size()), "replacement selection is inside");
		assert_equal(true, buffer.anchor_pos().y < static_cast<int>(buffer.size()), "replacement anchor is inside");
		assert_equal(true, buffer.cursor_pos().y < static_cast<int>(buffer.size()), "replacement cursor is inside");

		buffer.set_text("ab\ncd");
		buffer.cursor_pos({2, 1});
		buffer.anchor_pos({2, 1});
		{
			ui::undo_group group(buffer);
			buffer.delete_text(group, ui::text_location{0, 1});
		}
		assert_equal_strict("abcd", buffer.str(), "joining lines");
		assert_equal(0, buffer.cursor_pos().y, "joined cursor line");
		assert_equal(0, buffer.anchor_pos().y, "joined anchor line");
	}

	void should_set_text_and_revalidate_locations()
	{
		ui::test::recording_view_host host;
		ui::text_buffer buffer(host);
		assert_equal(1u, static_cast<uint32_t>(buffer.size()), "empty buffer has one line");
		assert_equal(false, buffer.empty(), "empty buffer is still a document");

		buffer.set_text("one\ntwo\nthree");
		assert_equal(3u, static_cast<uint32_t>(buffer.size()), "LF splits lines");
		assert_equal_strict("one\ntwo\nthree", buffer.str(), "LF text");
		buffer.set_text("a\r\nb\r\nc");
		assert_equal(3u, static_cast<uint32_t>(buffer.size()), "CRLF splits lines");
		assert_equal_strict("a\nb\nc", buffer.str(), "CRLF is normalized");
		buffer.set_text("x\n");
		assert_equal(2u, static_cast<uint32_t>(buffer.size()), "trailing newline keeps an empty line");
		buffer.set_text("");
		assert_equal(1u, static_cast<uint32_t>(buffer.size()), "empty replacement keeps one line");

		buffer.set_text("one\ntwo\nthree");
		buffer.anchor_pos({1, 1});
		buffer.cursor_pos({5, 2});
		buffer.select({1, 1, 5, 2});
		buffer.set_text("x");
		assert_equal(0, buffer.cursor_pos().y, "cursor line is clamped");
		assert_equal(0, buffer.anchor_pos().y, "anchor line is clamped");
		assert_equal(0, buffer.selection()._end.y, "selection line is clamped");

		buffer.set_text("alpha\nbeta");
		buffer.cursor_pos({2, 1});
		buffer.set_text("alpha\nbetter");
		assert_equal(1, buffer.cursor_pos().y, "existing cursor line is kept");
		assert_equal(2, buffer.cursor_pos().x, "existing cursor offset is kept");
		buffer.set_text(std::string(500, 'x'));
		assert_equal(500, buffer.max_line_length(), "longest line is recalculated");
		buffer.set_text("short");
		assert_equal(5, buffer.max_line_length(), "shorter line resets the range");
	}

	void should_keep_shared_text_lines_immutable_when_edited()
	{
		auto bytes = std::make_shared<const std::vector<uint8_t>>(std::vector<uint8_t>{'a', 'b', 'c'});
		ui::text_line first(bytes, 0, 2);
		ui::text_line second(bytes, 2, 1);
		std::string text;
		first.render(text);
		assert_equal_strict("ab", text, "shared line render");
		second.render(text);
		assert_equal_strict("c", text, "shared tail render");
		first.update("x"sv);
		second.render(text);
		assert_equal_strict("c", text, "edited line leaves shared bytes");
	}

	void should_keep_default_theme_palette()
	{
		const ui::view_theme theme;
		using style = ui::text_style;
		assert_equal(theme.window_background, ui::view_colors::window_background, "window palette");
		assert_equal(false, theme.style_color(style::code_keyword) == theme.style_color(style::code_comment),
		             "syntax palette");
		assert_equal(false, theme.style_color(style::md_heading1) == theme.style_color(style::md_heading2),
		             "heading palette");
		assert_equal(36, theme.heading_font(1).size, "heading one size");
		assert_equal(32, theme.heading_font(2).size, "heading two size");
		assert_equal(28, theme.heading_font(3).size, "heading three size");

		struct light_theme final : ui::view_theme
		{
			core::color32 style_color(const ui::text_style s) const override
			{
				return s == ui::text_style::normal_text ? core::rgba(0, 0, 0) : view_theme::style_color(s);
			}
		};
		const light_theme light;
		assert_equal(core::rgba(0, 0, 0), light.style_color(style::normal_text), "theme override");
	}

	void should_parse_markdown_models()
	{
		namespace md = ui::md;
		const auto doc = md::parse("# Title\n\nSome *emphasis* and **strong**.\n\n- one\n- two\n\n> quoted\n");
		assert_equal(true, !doc.empty(), "markdown document");
		int kinds = 0;
		bool italic = false;
		bool bold = false;
		std::string all;
		for (const auto& line : doc.lines)
		{
			if (line.kind == md::block::heading1) kinds |= 1;
			if (line.kind == md::block::bullet) kinds |= 2;
			if (line.kind == md::block::quote) kinds |= 4;
			for (const auto& span : line.spans)
			{
				all += span.text;
				italic = italic || span.italic;
				bold = bold || span.bold;
			}
		}
		assert_equal(7, kinds, "markdown block kinds");
		assert_equal(true, italic, "markdown italic span");
		assert_equal(true, bold, "markdown bold span");
		assert_equal(true, all.find("Title") != std::string::npos, "markdown heading text");
		assert_equal(true, all.find('*') == std::string::npos, "markdown markers removed");

		const auto linked = md::parse("see [the docs](https://example.invalid/x) here");
		bool found_link = false;
		for (const auto& line : linked.lines)
			for (const auto& span : line.spans)
				if (!span.link.empty())
				{
					found_link = true;
					assert_equal_strict("the docs", span.text, "link label");
					assert_equal_strict("https://example.invalid/x", span.link, "link target");
				}
		assert_equal(true, found_link, "markdown link");

		const auto escaped = md::parse(md::escape("**not bold** [not a link](x) `not code`"));
		for (const auto& line : escaped.lines)
			for (const auto& span : line.spans)
			{
				assert_equal(false, span.bold, "escaped bold");
				assert_equal(false, span.code, "escaped code");
				assert_equal(true, span.link.empty(), "escaped link");
			}
	}

	void should_parse_markdown_source_and_html()
	{
		namespace md = ui::md;
		md::source_line line;
		bool fence = false;
		md::parse_source_line("## A heading"sv, fence, line);
		assert_equal(line.kind == md::block::heading2, true, "source heading kind");
		assert_equal(3, line.content_start, "source heading offset");

		md::parse_source_line("```cpp"sv, fence, line);
		assert_equal(true, line.fence && fence, "source fence open");
		md::parse_source_line("int x = 1;"sv, fence, line);
		assert_equal(line.kind == md::block::code, true, "source code kind");
		md::parse_source_line("```"sv, fence, line);
		assert_equal(true, line.fence && !fence, "source fence close");
		md::parse_source_line("  - an item"sv, fence, line);
		assert_equal(true, line.kind == md::block::bullet, "source bullet kind");
		assert_equal(4, line.content_start, "source bullet offset");
		md::parse_source_line("12. step"sv, fence, line);
		assert_equal(true, line.kind == md::block::numbered, "source numbered kind");
		assert_equal(4, line.content_start, "source numbered offset");
		const std::string_view link = "see [docs](https://example.invalid/) now";
		md::parse_source_line(link, fence, line);
		bool found_link = false;
		for (const auto& span : line.spans)
		{
			if (span.link.empty()) continue;
			found_link = true;
			assert_equal(5, static_cast<int>(span.text.data() - link.data()), "source link label offset");
			assert_equal(11, static_cast<int>(span.link.data() - link.data()), "source link target offset");
		}
		assert_equal(true, found_link, "source link span");

		const auto reduced = md::from_html("<h1>Heading</h1><p>Body with <b>bold</b> text.</p>");
		const auto reduced_doc = md::parse(reduced);
		std::string text;
		for (const auto& parsed : reduced_doc.lines)
			for (const auto& span : parsed.spans) text += span.text;
		assert_equal(true, text.find("Heading") != std::string::npos, "HTML heading");
		assert_equal(true, text.find('<') == std::string::npos, "HTML tags removed");

		const auto scripted = md::parse(md::from_html("<p>safe</p><script>alert(1)</script>"));
		text.clear();
		for (const auto& parsed : scripted.lines)
			for (const auto& span : parsed.spans) text += span.text;
		assert_equal(true, text.find("alert") == std::string::npos, "script removed");
	}

	void should_keep_markdown_document_text_alive()
	{
		ui::md::document document;
		{
			std::string source = "# owned\n\ntext that must survive\n";
			document = ui::md::parse(std::move(source));
		}
		std::string all;
		for (const auto& line : document.lines)
			for (const auto& span : line.spans) all += span.text;
		assert_equal(true, all.find("owned") != std::string::npos, "owned Markdown heading");
		assert_equal(true, all.find("must survive") != std::string::npos, "owned Markdown body");
		assert_equal(true, ui::md::parse("").empty(), "empty Markdown document");
	}

	void should_use_clipboard_seam_and_spell_helpers()
	{
		ui::test::fake_clipboard clipboard;
		assert_equal(true, clipboard.set_text("copy me"sv), "clipboard write");
		assert_equal_strict("copy me", clipboard.text().value_or(""), "clipboard read");
		clipboard.writable = false;
		assert_equal(false, clipboard.set_text("refused"sv), "clipboard refusal");
		assert_equal_strict("copy me", clipboard.clipboard, "refusal keeps contents");
		assert_equal(true, ui::spell::is_word_byte('a'), "spell ASCII word");
		assert_equal(true, ui::spell::is_word_byte('\xC3'), "spell UTF-8 word");
		assert_equal(false, ui::spell::is_word_byte('-'), "spell punctuation");
		ui::spell::reset();
	}
}

void register_ui_text_tests(test_registry& tests)
{
	tests.add("Should split table cells"s, should_split_table_cells);
	tests.add("Should classify and cap table columns"s, should_classify_and_cap_table_columns);
	tests.add("Should measure and record headless drawing"s, should_measure_and_record_headless_drawing);
	tests.add("Should record text host requests"s, should_record_view_host_requests);
	tests.add("Should edit text and undo"s, should_edit_text_and_undo);
	tests.add("Should split lines and move UTF-8"s, should_split_lines_and_move_utf8);
	tests.add("Should clamp buffer locations"s, should_clamp_buffer_locations_after_replacement);
	tests.add("Should set text and revalidate locations"s, should_set_text_and_revalidate_locations);
	tests.add("Should preserve shared text lines"s, should_keep_shared_text_lines_immutable_when_edited);
	tests.add("Should keep the default view palette"s, should_keep_default_theme_palette);
	tests.add("Should parse Markdown models"s, should_parse_markdown_models);
	tests.add("Should parse Markdown source and HTML"s, should_parse_markdown_source_and_html);
	tests.add("Should keep Markdown document text alive"s, should_keep_markdown_document_text_alive);
	tests.add("Should use clipboard seam and spell helpers"s, should_use_clipboard_seam_and_spell_helpers);
}
