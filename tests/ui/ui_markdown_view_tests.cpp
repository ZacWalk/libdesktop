// This file is part of libdesktop, a C++20 platform layer for desktop applications
// Copyright 2026  Zac Walker
//
// This program is free software; you can redistribute it and / or modify it
// under the terms of the LGPL License either version 2.1 or later.
// License details are available at https://www.gnu.org/licenses/lgpl-2.1.html
// This program is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY

// Purpose: Headless tests for the source-preserving Markdown document control.

#include "libdesktop/ui/views.h"
#include "libdesktop/ui/test_support.h"
#include "test.h"

#include <algorithm>
#include <memory>
#include <string>
#include <string_view>

namespace
{
	bool is_utf8_continuation(const char byte)
	{
		return (static_cast<unsigned char>(byte) & 0xc0u) == 0x80u;
	}

	struct markdown_probe : ui::markdown_view
	{
		using ui::markdown_view::markdown_view;
	};

	ui::text_buffer_ptr show_markdown(markdown_probe& view, ui::view_host& host, const std::string_view text,
	                                  const core::sizei extent = {480, 320})
	{
		auto buffer = std::make_shared<ui::text_buffer>(host);
		buffer->set_text(text);
		view.bounds = {0, 0, extent.cx, extent.cy};
		view.set_buffer(buffer, {});

		ui::test::fake_measure_context measure;
		view.on_placed(measure);
		return buffer;
	}

	void test_markdown_view_layout()
	{
		ui::test::recording_view_host host;
		const ui::view_theme theme;
		markdown_probe view(host, theme);
		const auto buffer = show_markdown(view, host, "# Title\nbody text\n## Second\nmore body");

		assert_equal(4, static_cast<int>(buffer->size()), "source line count");

		const auto title_top = view.text_to_client({0, 0}).y;
		const auto body_top = view.text_to_client({0, 1}).y;
		const auto second_top = view.text_to_client({0, 2}).y;
		assert_equal(true, title_top < body_top, "heading precedes body");
		assert_equal(true, body_top < second_top, "lines remain ordered");
		assert_equal(true, second_top - body_top > body_top - title_top, "heading has extra height");

		for (const auto location : {ui::text_location{0, 0}, ui::text_location{4, 1},
		                            ui::text_location{3, 2}, ui::text_location{9, 3}})
		{
			const auto point = view.text_to_client(location);
			const auto back = view.text_at(point);
			assert_equal(location.y, back.y, "text round-trip line");
			assert_equal(location.x, back.x, "text round-trip byte");
		}

		assert_equal(true, view.allows_drag_selection(), "Markdown supports drag selection");
	}

	void test_markdown_view_selects_source()
	{
		ui::test::recording_view_host host;
		const ui::view_theme theme;
		markdown_probe view(host, theme);
		const auto buffer = show_markdown(view, host, "# Title\n**bold** words");

		view.set_selection(ui::text_selection(0, 0, 7, 0));
		assert_equal(true, view.can_copy_text(), "selected source can copy");
		assert_equal_strict("# Title", view.select_text(), "copy retains Markdown markers");

		view.select_all_text();
		assert_equal_strict("# Title\r\n**bold** words", view.select_text(), "select all retains source");
		assert_equal(false, view.can_cut_text(), "read-only Markdown cannot cut");
		assert_equal(false, view.can_paste_text(), "read-only Markdown cannot paste");
		assert_equal(static_cast<int>(buffer->size()), 2, "selection does not edit buffer");
	}

	void test_markdown_view_paints_markup()
	{
		ui::test::recording_view_host host;
		const ui::view_theme theme;
		markdown_probe view(host, theme);
		show_markdown(view, host, "# Title\n- item one\n\n| a | b |\n| - | - |\n| 1 | 2 |\n");

		ui::test::fake_draw_context draw{core::recti(0, 0, 480, 320)};
		view.paint(draw, false);
		const auto drawn = draw.drawn_text();
		assert_equal(true, drawn.find("Title") != std::string::npos, "heading text is drawn");
		assert_equal(true, drawn.find("item one") != std::string::npos, "list text is drawn");

		const auto marker_color = core::color(theme.style_color(ui::text_style::md_marker));
		const auto heading_color = core::color(theme.style_color(ui::text_style::md_heading1));
		bool saw_marker = false;
		bool saw_heading = false;
		for (const auto& text : draw.texts)
		{
			if (text.text.find('#') != std::string::npos && text.color == marker_color) saw_marker = true;
			if (text.color == heading_color) saw_heading = true;
		}
		assert_equal(true, saw_marker, "Markdown marker is visible");
		assert_equal(true, saw_heading, "heading uses heading style");

		bool saw_separator = false;
		for (const auto& text : draw.texts)
			if (text.text.find("---") != std::string::npos) saw_separator = true;
		assert_equal(true, saw_separator, "table separator is drawn");
	}

	void test_markdown_view_links()
	{
		ui::test::recording_view_host host;
		const ui::view_theme theme;
		markdown_probe view(host, theme);
		show_markdown(view, host, "intro\nsee [docs](https://example.invalid/x) now\n");

		const auto link_point = view.text_to_client({6, 1});
		const auto url_point = view.text_to_client({14, 1});
		const auto plain_point = view.text_to_client({1, 1});
		assert_equal_strict("https://example.invalid/x", view.link_at(link_point), "link text hit");
		assert_equal_strict("https://example.invalid/x", view.link_at(url_point), "link URL hit");
		assert_equal_strict("", view.link_at(plain_point), "plain text is not a link");
		assert_equal_strict("", view.link_at(view.text_to_client({2, 0})), "other line has no link");

		std::string followed;
		assert_equal(false, view.has_link_handler(), "link handler starts empty");
		view.set_link_handler([&](const std::string_view target) { followed = target; });
		assert_equal(true, view.has_link_handler(), "link handler is installed");

		view.on_mouse_down(link_point, {});
		view.on_mouse_up(link_point, {});
		assert_equal_strict("https://example.invalid/x", followed, "click invokes link handler");

		followed.clear();
		view.on_mouse_down(plain_point, {});
		view.on_mouse_up(link_point, {});
		assert_equal_strict("", followed, "moved press is a drag");

		followed.clear();
		view.set_selection(ui::text_selection{});
		view.on_mouse_down(plain_point, {});
		view.on_mouse_move(link_point, true);
		view.on_mouse_up(link_point, {});
		assert_equal(true, view.has_current_selection(), "drag selects text");
		assert_equal_strict("", followed, "selection drag does not follow link");

		followed.clear();
		view.set_selection(ui::text_selection{});
		view.on_mouse_down({link_point.x, link_point.y + 5000}, {});
		view.on_mouse_up({link_point.x, link_point.y + 5000}, {});
		assert_equal_strict("", followed, "empty space below has no link");
		assert_equal_strict("", view.link_at({5000, link_point.y}), "empty space beside text has no link");
	}

	void test_markdown_view_reports_which_link_was_hit()
	{
		ui::test::recording_view_host host;
		const ui::view_theme theme;
		markdown_probe view(host, theme);
		show_markdown(view, host, "see [first](x:1) and [second](x:2)\nplain line\nagain [third](x:1)\n");

		const auto first = view.link_hit_at(view.text_to_client({5, 0}));
		assert_equal_strict("x:1", first.target, "first link target");
		assert_equal(0, first.line, "first link line");

		const auto second = view.link_hit_at(view.text_to_client({24, 0}));
		assert_equal_strict("x:2", second.target, "second link target");
		assert_equal(0, second.line, "second link line");
		assert_not_equal(core::to_string(first.span), core::to_string(second.span), "link span occurrence");

		const auto third = view.link_hit_at(view.text_to_client({8, 2}));
		assert_equal_strict("x:1", third.target, "third link target");
		assert_equal(2, third.line, "third link line");

		const auto miss = view.link_hit_at({5000, view.text_to_client({0, 1}).y});
		assert_equal_strict("", miss.target, "link miss target");
		assert_equal(-1, miss.line, "link miss line");
		assert_equal(-1, miss.span, "link miss span");
	}

	void test_markdown_view_tables()
	{
		ui::test::recording_view_host host;
		const ui::view_theme theme;
		markdown_probe view(host, theme);
		const auto buffer = show_markdown(view, host,
		                                 "| name | note |\n| ---- | ---- |\n| caf\xC3\xA9 | ok |\nafter");

		ui::test::fake_draw_context draw{core::recti(0, 0, 480, 320)};
		view.paint(draw, false);
		for (const auto& text : draw.texts)
			assert_equal(true, text.text.empty() || !is_utf8_continuation(text.text.front()),
			             "table drawing starts on a codepoint");

		const auto row_y = view.text_to_client({0, 2}).y;
		assert_equal(2, view.text_at({0, row_y}).y, "table row hit line");
		assert_equal(0, view.text_at({0, row_y}).x, "table left hit offset");
		assert_equal(static_cast<int>((*buffer)[2].size()), view.text_at({470, row_y}).x, "table right hit offset");
		assert_equal_strict("", view.link_at({100, row_y}), "table has no link hit");

		view.set_selection(ui::text_selection(0, 2, 0, 3));
		draw.reset();
		view.paint(draw, false);
		const auto selected_background = core::color(theme.style_color(ui::text_style::sel_bkgnd));
		bool saw_selected_cell = false;
		for (const auto& text : draw.texts)
		{
			if (text.text.find("ok") == std::string::npos) continue;
			for (const auto& fill : draw.fills)
			{
				if (fill.color == selected_background &&
				    fill.rect.left <= text.clip.left && fill.rect.right >= text.clip.right &&
				    fill.rect.top <= text.clip.top && fill.rect.bottom >= text.clip.bottom)
				{
					saw_selected_cell = true;
					break;
				}
			}
		}
		assert_equal(true, saw_selected_cell, "selected table row uses selection background");
	}

	void test_markdown_view_keeps_text_clear_of_the_scrollbar()
	{
		ui::test::recording_view_host host;
		const ui::view_theme theme;
		markdown_probe view(host, theme);
		const core::sizei extent{200, 100};
		show_markdown(view, host, std::string(100, 'x'), extent);

		const auto first_row = view.text_to_client({0, 0}).y;
		int right = 0;
		for (int i = 0; i < 100; ++i)
		{
			const auto point = view.text_to_client({i, 0});
			if (point.y == first_row) right = std::max(right, point.x + 8);
		}

		assert_equal(true, right > extent.cx / 2, "wrapped row uses most of the width");
		assert_equal(true, right <= extent.cx - ui::scrollbar::base_thumb_thickness -
		                              2 * ui::scrollbar::base_edge_margin,
		             "wrapped row leaves scrollbar room");
	}

	void test_markdown_view_wraps_without_touching_the_buffer()
	{
		ui::test::recording_view_host host;
		const ui::view_theme theme;
		markdown_probe view(host, theme);
		const std::string source =
			"- aaa bbb ccc ddd eee fff ggg hhh iii jjj kkk lll mmm nnn ooo ppp qqq rrr\nafter";
		const auto buffer = show_markdown(view, host, source, {160, 320});

		assert_equal(2, static_cast<int>(buffer->size()), "wrapped source line count");
		assert_equal_strict(source, buffer->str(), "wrapping does not edit source");

		const auto wrapped_top = view.text_to_client({0, 0}).y;
		const auto next_top = view.text_to_client({0, 1}).y;
		assert_equal(true, next_top - wrapped_top > 16 * 3, "wrapped line pushes following line");

		const auto tail_offset = static_cast<int>(source.find("rrr"));
		const auto tail = view.text_to_client({tail_offset, 0});
		assert_equal(true, tail.x > view.text_to_client({0, 0}).x, "continuation is indented");
		assert_equal(true, tail.y > wrapped_top, "continuation is on a later row");
		const auto back = view.text_at(tail);
		assert_equal(0, back.y, "wrapped hit line");
		assert_equal(tail_offset, back.x, "wrapped hit byte");
	}

	void test_markdown_view_utf8_positions()
	{
		ui::test::recording_view_host host;
		const ui::view_theme theme;
		markdown_probe view(host, theme);
		show_markdown(view, host, "h\xC3\xA9llo w\xC3\xB6rld\n");

		const auto point = view.text_to_client({1, 0});
		assert_equal(view.text_to_client({0, 0}).x + 8, point.x, "UTF-8 character occupies one column");
		assert_equal(1, view.text_at(point).x, "hit lands on UTF-8 character start");

		const auto after = view.text_to_client({3, 0});
		assert_equal(view.text_to_client({0, 0}).x + 16, after.x, "byte offsets keep display columns");
		assert_equal(3, view.text_at(after).x, "hit after UTF-8 character is a boundary");
	}

	void test_markdown_view_styles_a_quote()
	{
		struct quote_theme : ui::view_theme
		{
			[[nodiscard]] core::color32 style_color(const ui::text_style style) const override
			{
				return style == ui::text_style::md_quote ? core::rgba(0x60, 0xa0, 0xff)
				                                      : ui::view_theme::style_color(style);
			}
		};

		const ui::view_theme plain;
		assert_equal(true, plain.style_color(ui::text_style::md_quote) ==
		                       plain.style_color(ui::text_style::normal_text),
		             "default quote colour is normal text");

		quote_theme theme;
		ui::test::recording_view_host host;
		markdown_probe view(host, theme);
		show_markdown(view, host, "> asked something\nanswered something\n");

		ui::test::fake_draw_context draw{core::recti(0, 0, 480, 320)};
		view.paint(draw, false);
		const auto quote_color = core::color(theme.style_color(ui::text_style::md_quote));
		const auto normal_color = core::color(theme.style_color(ui::text_style::normal_text));
		bool quoted = false;
		bool plain_run = false;
		for (const auto& text : draw.texts)
		{
			if (text.text.find("asked") != std::string::npos)
			{
				quoted = true;
				assert_equal(true, text.color == quote_color, "quote style reaches its text");
			}
			if (text.text.find("answered") != std::string::npos)
			{
				plain_run = true;
				assert_equal(true, text.color == normal_color, "normal style remains normal");
			}
		}
		assert_equal(true, quoted, "quoted text is drawn");
		assert_equal(true, plain_run, "plain text is drawn");
	}
	void test_markdown_view_copies_with_keyboard()
	{
		ui::test::recording_view_host host;
		const ui::view_theme theme;
		ui::test::fake_clipboard clipboard;
		markdown_probe view(host, theme, nullptr, &clipboard);
		const auto buffer = show_markdown(view, host, "first line\nsecond line");

		platform::key_state control;
		control.control = true;
		assert_equal(true, view.on_key('A', control), "Ctrl+A selects Markdown source");
		assert_equal(true, view.has_current_selection(), "keyboard selection");
		assert_equal(true, view.on_key('C', control), "Ctrl+C is handled");
		assert_equal_strict("first line\r\nsecond line", clipboard.clipboard, "keyboard copy");

		assert_equal(true, view.on_key('X', control), "Ctrl+X is handled");
		assert_equal_strict("first line\nsecond line", buffer->str(), "read-only keyboard cut preserves source");
	}


}

void register_ui_markdown_view_tests(test_registry& tests)
{
	tests.add(std::string("Should lay out Markdown source"), test_markdown_view_layout);
	tests.add(std::string("Should select Markdown source"), test_markdown_view_selects_source);
	tests.add(std::string("Should paint Markdown markup"), test_markdown_view_paints_markup);
	tests.add(std::string("Should activate Markdown links"), test_markdown_view_links);
	tests.add(std::string("Should report Markdown link occurrences"), test_markdown_view_reports_which_link_was_hit);
	tests.add(std::string("Should draw Markdown tables"), test_markdown_view_tables);
	tests.add(std::string("Should leave Markdown rows clear of scrollbars"), test_markdown_view_keeps_text_clear_of_the_scrollbar);
	tests.add(std::string("Should wrap Markdown without editing it"), test_markdown_view_wraps_without_touching_the_buffer);
	tests.add(std::string("Should keep Markdown UTF-8 positions"), test_markdown_view_utf8_positions);
	tests.add(std::string("Should style Markdown quotes"), test_markdown_view_styles_a_quote);
	tests.add(std::string("Should copy Markdown source with the keyboard"), test_markdown_view_copies_with_keyboard);
}