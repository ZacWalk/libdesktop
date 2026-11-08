// This file is part of libdesktop, a C++20 platform layer for desktop applications
// Copyright 2026  Zac Walker
//
// This program is free software; you can redistribute it and / or modify it
// under the terms of the LGPL License either version 2.1 or later.
// License details are available at https://www.gnu.org/licenses/lgpl-2.1.html
// This program is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY

// Purpose: Headless tests for the document control's layout, selection, wrapping and editing seams.

#include "libdesktop/ui/views.h"
#include "libdesktop/ui/test_support.h"
#include "test.h"

#include <memory>
#include <string>

using namespace std::literals;

namespace
{
	struct doc_probe : ui::doc_view
	{
		using ui::doc_view::doc_view;
		using ui::doc_view::text_to_client;
	};

	struct edit_probe : ui::edit_doc_view
	{
		using ui::edit_doc_view::edit_doc_view;
		using ui::edit_doc_view::on_text;
		using ui::edit_doc_view::on_key;
		using ui::edit_doc_view::cut_text_to_clipboard;
	};

	void layout(ui::control& view, render::measure_context& measure, const core::sizei extent)
	{
		view.layout(measure, core::recti(extent));
	}

	void should_round_trip_document_locations()
	{
		ui::test::recording_view_host host;
		const ui::view_theme theme;
		doc_probe view(host, theme);
		auto buffer = std::make_shared<ui::text_buffer>(host);
		buffer->set_text("hello world\nsecond line\nthird");
		view.set_buffer(buffer, ui::syntax::for_language(ui::syntax::language::plain));

		ui::test::fake_measure_context measure;
		layout(view, measure, {400, 320});
		view.layout();

		for (const auto location : {ui::text_location{0, 0}, ui::text_location{6, 0},
		                            ui::text_location{3, 1}, ui::text_location{5, 2}})
		{
			const auto point = view.text_to_client(location);
			const auto back = view.text_at(point);
			assert_equal(location.y, back.y, "document line", "text point round trip");
			assert_equal(location.x, back.x, "document offset", "text point round trip");
		}

		const auto past_end = view.text_at({5000, view.text_to_client({0, 0}).y});
		assert_equal(0, past_end.y, "clamped line", "hit testing past a line");
		assert_equal(11, past_end.x, "clamped offset", "hit testing past a line");
	}

	void should_wrap_without_changing_the_buffer()
	{
		ui::test::recording_view_host host;
		const ui::view_theme theme;
		doc_probe view(host, theme);
		auto buffer = std::make_shared<ui::text_buffer>(host);
		const std::string source = "aaa bbb ccc ddd eee fff ggg hhh iii jjj kkk lll mmm nnn ooo";
		buffer->set_text(source);
		view.set_buffer(buffer, {});
		view.set_word_wrap(true);

		ui::test::fake_measure_context measure;
		layout(view, measure, {160, 320});
		view.layout();

		assert_equal(1, static_cast<int>(buffer->size()), "line count", "wrapping is view state");
		assert_equal_strict(source, buffer->str(), "source text", "wrapping is view state");
		view.set_word_wrap(false);
		view.layout();
		view.set_word_wrap(true);
		view.layout();
		assert_equal(1, static_cast<int>(buffer->size()), "line count after toggling", "wrapping is reversible");
	}

	void should_select_and_copy_document_text()
	{
		ui::test::recording_view_host host;
		const ui::view_theme theme;
		doc_probe view(host, theme);
		auto buffer = std::make_shared<ui::text_buffer>(host);
		buffer->set_text("alpha beta gamma");
		view.set_buffer(buffer, {});
		view.set_selection(ui::text_selection(0, 0, 5, 0));

		assert_equal(true, view.has_current_selection(), "selection", "document selection");
		assert_equal_strict("alpha", view.select_text(), "selected text", "document selection");
		view.select_all_text();
		assert_equal_strict("alpha beta gamma", view.select_text(), "all text", "document selection");
	}

	void should_edit_through_the_document_control()
	{
		ui::test::recording_view_host host;
		const ui::view_theme theme;
		edit_probe view(host, theme);
		auto buffer = std::make_shared<ui::text_buffer>(host);
		buffer->set_text("edit me");
		view.set_buffer(buffer, {});
		buffer->select(ui::text_selection(ui::text_location{7, 0}));
		view.on_text("!");
		assert_equal_strict("edit me!", buffer->str(), "typed text", "editable document view");
		view.on_key(platform::keys::BACK, {});
		assert_equal_strict("edit me", buffer->str(), "backspace", "editable document view");
		platform::key_state control;
		control.control = true;
		view.on_key(U'Z', control);
		assert_equal_strict("edit me!", buffer->str(), "undo", "editable document view");
		view.on_key(U'Y', control);
		assert_equal_strict("edit me", buffer->str(), "redo", "editable document view");
	}

	void should_paint_document_text()
	{
		ui::test::recording_view_host host;
		const ui::view_theme theme;
		doc_probe view(host, theme);
		auto buffer = std::make_shared<ui::text_buffer>(host);
		buffer->set_text("visible text");
		view.set_buffer(buffer, {});
		ui::test::fake_measure_context measure;
		layout(view, measure, {400, 320});
		view.layout();
		ui::test::fake_draw_context draw({0, 0, 400, 320});
		view.paint(draw, false);

		assert_equal(true, draw.drawn_text().find("visible text") != std::string::npos,
		             "painted text", "document drawing");
		assert_equal(true, !draw.fills.empty(), "paint fills", "document drawing");
	}
	void should_not_cut_when_the_clipboard_refuses()
	{
		ui::test::recording_view_host host;
		ui::test::fake_clipboard clipboard;
		const ui::view_theme theme;
		edit_probe view(host, theme, nullptr, &clipboard);
		auto buffer = std::make_shared<ui::text_buffer>(host);
		buffer->set_text("keep me");
		view.set_buffer(buffer, {});
		view.select_all_text();
		clipboard.writable = false;

		assert_equal(false, view.cut_text_to_clipboard(), "refused cut", "clipboard seam");
		assert_equal_strict("keep me", buffer->str(), "document text", "clipboard seam");
		assert_equal(true, view.has_current_selection(), "selection", "clipboard seam");
	}
	void should_keep_caret_in_the_document()
	{
		ui::test::recording_view_host host;
		const ui::view_theme theme;
		doc_probe view(host, theme);
		auto buffer = std::make_shared<ui::text_buffer>(host);
		buffer->set_text("caret");
		view.set_buffer(buffer, {});
		view.on_focus(true, false);
		const auto caret = view.caret_bounds();
		assert_equal(true, caret.width() > 0, "caret width", "caret visibility");
		assert_equal(true, view.caret_visible(), "caret shown", "caret visibility");
		view.blink();
		assert_equal(false, view.caret_visible(), "caret blink", "caret visibility");
		view.on_focus(false, false);
		assert_equal(true, view.caret_bounds().width() > 0, "caret geometry", "caret location remains stable");
	}

	void should_keep_scrolled_text_inside_its_margin()
	{
		ui::test::recording_view_host host;
		const ui::view_theme theme;
		doc_probe view(host, theme);
		auto buffer = std::make_shared<ui::text_buffer>(host);
		buffer->set_text(std::string(200, 'x'));
		view.set_buffer(buffer, {});

		ui::test::fake_measure_context measure;
		layout(view, measure, {400, 320});
		view.set_word_wrap(false);
		view.layout();
		const auto text_left = view.text_to_client({0, 0}).x;
		assert_equal(true, text_left > 0, "document margin");
		view.ensure_visible({200, 0});
		assert_equal(true, view.scroll_offset().x > 0, "long line scrolls horizontally");

		ui::test::fake_draw_context draw({0, 0, 400, 320});
		view.paint(draw, false);
		assert_equal(true, draw.drawn_text().find('x') != std::string::npos, "scrolled text remains drawable");
		assert_equal(true, view.text_to_client({200, 0}).x > view.text_to_client({199, 0}).x,
		             "caret positions remain ordered after scrolling");
	}

	void should_offer_document_context_commands()
	{
		ui::test::recording_view_host host;
		const ui::view_theme theme;
		ui::test::fake_clipboard clipboard;
		edit_probe editable(host, theme, nullptr, &clipboard);
		auto buffer = std::make_shared<ui::text_buffer>(host);
		buffer->set_text("hello world");
		editable.set_buffer(buffer, {});
		editable.select_all_text();

		const auto commands = editable.default_popup_menu();
		assert_equal(5u, static_cast<uint32_t>(commands.size()), "document menu item count");
		if (commands.size() != 5) return;
		assert_equal_strict("Cu&t", commands[0]->text, "cut command");
		assert_equal_strict("&Copy", commands[1]->text, "copy command");
		assert_equal_strict("&Paste", commands[2]->text, "paste command");
		assert_equal(true, commands[0]->enable, "cut enabled with selection");
		assert_equal(true, commands[1]->enable, "copy enabled with selection");
		assert_equal(true, commands[4]->enable, "select-all enabled");

		doc_probe readonly(host, theme, nullptr, &clipboard);
		readonly.set_buffer(buffer, {});
		readonly.select_all_text();
		const auto read_only_commands = readonly.default_popup_menu();
		assert_equal(false, read_only_commands[0]->enable, "read-only cut disabled");
		assert_equal(true, read_only_commands[1]->enable, "read-only copy enabled");
		assert_equal(false, read_only_commands[2]->enable, "read-only paste disabled");
	}

}

void register_ui_doc_view_tests(test_registry& tests)
{
	tests.add("Should round-trip document hit testing"s, should_round_trip_document_locations);
	tests.add("Should wrap without changing document text"s, should_wrap_without_changing_the_buffer);
	tests.add("Should select and copy document text"s, should_select_and_copy_document_text);
	tests.add("Should edit through the document control"s, should_edit_through_the_document_control);
	tests.add("Should keep document caret geometry"s, should_keep_caret_in_the_document);
	tests.add("Should preserve text when cut is refused"s, should_not_cut_when_the_clipboard_refuses);
	tests.add("Should paint document text"s, should_paint_document_text);
	tests.add("Should keep scrolled text inside its margin"s, should_keep_scrolled_text_inside_its_margin);
	tests.add("Should offer document context commands"s, should_offer_document_context_commands);
}