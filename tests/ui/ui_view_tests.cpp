// This file is part of libdesktop, a C++20 platform layer for desktop applications
// Copyright 2026  Zac Walker
//
// This program is free software; you can redistribute it and / or modify it
// under the terms of the LGPL License either version 2.1 or later.
// License details are available at https://www.gnu.org/licenses/lgpl-2.1.html
// This program is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY

// Purpose: Headless tests for scrollbars, splitters, and document-control policies.

#include "libdesktop/ui/views.h"
#include "libdesktop/ui/test_support.h"
#include "test.h"

#include <memory>
#include <string>

using namespace std::literals;

namespace
{
	void should_clamp_splitter_geometry()
	{
		ui::split_element splitter(ui::split_element::orientation::vertical, 0.5);
		const core::recti bounds{0, 0, 200, 100};

		assert_equal(100, splitter.split_pos(bounds), "split position", "splitter geometry");
		assert_equal(true, splitter.hit_test(bounds, {100, 50}), "bar hit", "splitter geometry");
		assert_equal(false, splitter.hit_test(bounds, {10, 50}), "outside bar", "splitter geometry");

		splitter.update_ratio(bounds, {-500, 50});
		assert_equal(true, splitter._ratio >= ui::split_element::min_ratio, "minimum ratio", "splitter geometry");
		splitter.update_ratio(bounds, {5000, 50});
		assert_equal(true, splitter._ratio <= ui::split_element::max_ratio, "maximum ratio", "splitter geometry");

		const auto narrow = splitter.bar_width();
		splitter.set_dpi_scale(2.0);
		assert_equal(true, splitter.bar_width() > narrow, "DPI bar width", "splitter geometry");
	}

	void should_keep_scrollbar_thumb_on_track()
	{
		ui::scrollbar bar(ui::scrollbar::orientation::vertical);
		bar.update(100, 100, 0);
		assert_equal(false, bar.can_scroll(), "content fits", "scrollbar geometry");

		bar.update(1000, 100, 0);
		assert_equal(true, bar.can_scroll(), "content scrolls", "scrollbar geometry");
		const auto top = bar.thumb(0, 200);
		assert_equal(0, top.start, "thumb origin", "scrollbar geometry");
		assert_equal(true, top.length > 0, "thumb length", "scrollbar geometry");

		bar.update(100000, 100, 99900);
		const auto bottom = bar.thumb(0, 200);
		assert_equal(true, bottom.start + bottom.length <= 200, "thumb stays on track", "scrollbar geometry");
		assert_equal(true, bottom.length >= bar.thumb_thickness(), "minimum thumb", "scrollbar geometry");
	}

	struct edit_probe final : ui::edit_doc_view
	{
		using ui::edit_doc_view::edit_doc_view;
		using ui::edit_doc_view::cut_text_to_clipboard;
	};

	void should_copy_from_a_read_only_document()
	{
		ui::test::recording_view_host host;
		const ui::view_theme theme;
		ui::test::fake_clipboard clipboard;
		ui::read_only_doc_view view(host, theme, nullptr, &clipboard);
		auto buffer = std::make_shared<ui::text_buffer>(host);
		buffer->set_text("first line\nsecond line");
		view.set_buffer(buffer, {});
		view.select_all_text();

		assert_equal(true, view.can_copy_text(), "copy enabled", "read-only document");
		assert_equal(false, view.can_cut_text(), "cut disabled", "read-only document");
		assert_equal(true, view.copy_text_to_clipboard(), "copy result", "read-only document");
		assert_equal_strict("first line\r\nsecond line", clipboard.clipboard, "clipboard text", "read-only document");
		assert_equal(false, view.can_paste_text(), "paste disabled", "read-only document");
	}

	void should_not_cut_when_the_clipboard_refuses()
	{
		ui::test::recording_view_host host;
		const ui::view_theme theme;
		ui::test::fake_clipboard clipboard;
		edit_probe view(host, theme, nullptr, &clipboard);
		auto buffer = std::make_shared<ui::text_buffer>(host);
		buffer->set_text("keep me");
		view.set_buffer(buffer, {});
		view.select_all_text();

		clipboard.writable = false;
		assert_equal(false, view.cut_text_to_clipboard(), "refused cut", "editable document");
		assert_equal_strict("keep me", buffer->str(), "text after refused cut", "editable document");
		assert_equal(true, buffer->has_selection(), "selection after refused cut", "editable document");

		clipboard.writable = true;
		assert_equal(true, view.cut_text_to_clipboard(), "accepted cut", "editable document");
		assert_equal_strict("", buffer->str(), "text after accepted cut", "editable document");
		assert_equal_strict("keep me", clipboard.clipboard, "cut clipboard", "editable document");
	}
}

void register_ui_view_tests(test_registry& tests)
{
	tests.add("Should clamp splitter geometry"s, should_clamp_splitter_geometry);
	tests.add("Should keep scrollbar thumbs on track"s, should_keep_scrollbar_thumb_on_track);
	tests.add("Should copy from a read-only document"s, should_copy_from_a_read_only_document);
	tests.add("Should preserve text when cut is refused"s, should_not_cut_when_the_clipboard_refuses);
}