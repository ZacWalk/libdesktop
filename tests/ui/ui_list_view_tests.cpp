// This file is part of libdesktop, a C++20 platform layer for desktop applications
// Copyright 2026  Zac Walker
//
// This program is free software; you can redistribute it and / or modify it
// under the terms of the LGPL License either version 2.1 or later.
// License details are available at https://www.gnu.org/licenses/lgpl-2.1.html
// This program is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY

// Purpose: Regression tests for list, hexadecimal, and CSV view controls

#include "libdesktop/ui/views/list_view.h"
#include "libdesktop/ui/views/hex_view.h"
#include "libdesktop/ui/views/csv_view.h"
#include "libdesktop/ui/text/text_buffer.h"
#include "libdesktop/ui/test_support.h"
#include "test.h"

#include <format>
#include <memory>
#include <string>

namespace
{

	struct list_probe : ui::list_view
	{
		using ui::list_view::list_view;
		using ui::list_view::_header_height;
		using ui::list_view::_items;
		using ui::list_view::draw_item;

		void add(const std::string_view text, const bool group = false, const int depth = 0)
		{
			auto item = std::make_shared<ui::list_item>();
			item->text = text;
			item->is_group = group;
			item->depth = depth;
			_items.push_back(std::move(item));
		}
	};

	void size_list(ui::list_view& view, const core::sizei extent = {300, 200})
	{
		view.bounds = {0, 0, extent.cx, extent.cy};
		ui::test::fake_measure_context measure;
		view.on_placed(measure);
		view.layout_list();
	}

	std::shared_ptr<ui::text_buffer> show_document(ui::read_only_doc_view& view,
	                                               ui::test::recording_view_host& host,
	                                               std::string_view text,
	                                               core::sizei extent = {480, 320})
	{
		auto buffer = std::make_shared<ui::text_buffer>(host);
		buffer->set_text(text);
		view.set_buffer(buffer, {});
		view.bounds = {0, 0, extent.cx, extent.cy};
		ui::test::fake_measure_context measure;
		view.on_placed(measure);
		view.layout();
		return buffer;
	}

	void should_layout_and_hit_test_rows()
	{
		const ui::view_theme theme;
		list_probe view(theme);
		for (int i = 0; i < 40; ++i) view.add(std::format("row {}", i));
		size_list(view);

		const auto first = view._items.front()->bounds;
		const auto second = view._items[1]->bounds;
		assert_equal(first.bottom, second.top, "rows touch");
		assert_equal(true, first.height() > 0, "rows have height");
		assert_equal(true, view.can_scroll(), "long list scrolls");
		assert_equal(true, view.selection_from_point({10, first.top + 1}) == view._items.front(), "first row hit");
		assert_equal(true, view.selection_from_point({10, second.top + 1}) == view._items[1], "second row hit");
		assert_equal(true, view.selection_from_point({10, -50}) == nullptr, "above rows misses");
		assert_equal(true, view.selection_from_point({10, 100000}) == nullptr, "below rows misses");

		list_probe empty(theme);
		assert_equal(true, empty.selection_from_point({0, 0}) == nullptr, "empty list misses");
	}

	void should_select_navigate_and_copy_rows()
	{
		const ui::view_theme theme;
		list_probe view(theme);
		for (int i = 0; i < 6; ++i) view.add(std::format("row {}", i));
		size_list(view);

		view.set_selected(1);
		assert_equal(1, view.selected_index(), "selected index");
		assert_equal(true, view.is_selected(1), "selected row");
		assert_equal(false, view.is_selected(2), "unselected row");
		assert_equal_strict("row 1", view.selected_rows_text(), "selected row text");

		view.navigate_next(true);
		assert_equal(2, view.selected_index(), "down navigation");
		view.navigate_next(true, false, true);
		view.navigate_next(true, false, true);
		assert_equal(4, view.selected_index(), "extended navigation end");
		assert_equal(true, view.is_selected(2), "extended first");
		assert_equal(true, view.is_selected(3), "extended middle");
		assert_equal(true, view.is_selected(4), "extended last");
		assert_equal(false, view.is_selected(1), "old anchor excluded");
		assert_equal_strict("row 2\r\nrow 3\r\nrow 4", view.selected_rows_text(), "range text");

		view.navigate_next(false);
		assert_equal_strict("row 3", view.selected_rows_text(), "navigation collapses range");
		const auto keep = view.selected_item();
		view._items.insert(view._items.begin(), std::make_shared<ui::list_item>());
		assert_equal(4, view.selected_index(), "selection index is revalidated");
		assert_equal(true, view.selected_item() == keep, "selection identity survives rebuild");

		view.set_selected(static_cast<int>(view._items.size()) - 1);
		view.navigate_next(true);
		assert_equal(static_cast<int>(view._items.size()) - 1, view.selected_index(), "end does not wrap");
		view.set_selected(0);
		view.navigate_next(false);
		assert_equal(0, view.selected_index(), "start does not wrap");
	}

	void should_paint_rows_and_clip_text()
	{
		const ui::view_theme theme;
		list_probe view(theme);
		view.add("a folder", true);
		view.add("a file", false, 1);
		size_list(view);
		view.set_selected(1);

		ui::test::fake_draw_context draw({0, 0, 300, 200});
		view.paint(draw, true);
		assert_equal(true, draw.drawn_text().find("a folder") != std::string::npos, "group is painted");
		assert_equal(true, draw.drawn_text().find("a file") != std::string::npos, "row is painted");

		bool saw_group_color = false;
		for (const auto& text : draw.texts)
			if (text.text == "a folder" && text.color == core::color(theme.group_text)) saw_group_color = true;
		assert_equal(true, saw_group_color, "group colour");

		bool saw_selection = false;
		for (const auto& fill : draw.fills)
			if (fill.color == core::color(theme.handle) || fill.color == core::color(theme.focus_handle)) saw_selection = true;
		assert_equal(true, saw_selection, "selection fill");

		int folder_x = 0;
		int file_x = 0;
		for (const auto& text : draw.texts)
		{
			if (text.text == "a folder") folder_x = text.x;
			if (text.text == "a file") file_x = text.x;
		}
		assert_equal(true, file_x > folder_x, "indent is visible");

		list_probe clipped(theme);
		clipped.add("a name far longer than the column it has to live in");
		size_list(clipped, {200, 200});
		draw.reset();
		clipped.paint(draw, false);
		assert_equal(true, draw.drawn_text().find("...") != std::string::npos, "long row ellipsizes");
		assert_equal(true, draw.drawn_text().find("live in") == std::string::npos, "ellipsis clips suffix");

		list_probe hits(theme);
		hits.add("aaaaaaaaaaaaaaaaaaaaaaaaaaaaaa needle bbbbbbbbbbbbbbbbbbbbbbbbbbbbbb");
		hits._items.front()->prefix = "42: ";
		hits._items.front()->match_start = 31;
		hits._items.front()->match_length = 6;
		size_list(hits, {200, 200});
		draw.reset();
		hits.paint(draw, false);
		bool saw_match = false;
		bool saw_prefix = false;
		for (const auto& text : draw.texts)
		{
			if (text.text == "needle") saw_match = true;
			if (text.text == "42: " && text.color == core::color(theme.dim_text)) saw_prefix = true;
		}
		assert_equal(true, saw_match, "match remains visible");
		assert_equal(true, saw_prefix, "prefix is dimmed");
	}

	void should_sort_columns_and_draw_cells()
	{
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

		const ui::view_theme theme;
		sorting_probe view(theme);
		view.set_columns({{"Sym", 37, false}, {"Price", 35, true}, {"Chg%", 28, true}});
		auto a = std::make_shared<ui::list_item>();
		a->cells = {ui::list_cell{"AAPL"}, ui::list_cell{"182.40"}, ui::list_cell{"+1.2%", theme.match_highlight}};
		view._items.push_back(a);
		auto b = std::make_shared<ui::list_item>();
		b->cells = {ui::list_cell{"MSFT"}, ui::list_cell{"410.15"}, ui::list_cell{"-0.4%", theme.dim_text}};
		view._items.push_back(b);
		size_list(view, {300, 200});

		assert_equal(true, view._header_height > 0, "column header exists");
		const auto rects = view.column_rects(300);
		assert_equal(3u, static_cast<unsigned>(rects.size()), "column count");
		assert_equal(0, rects[0].left, "first column begins at zero");
		assert_equal(rects[0].right, rects[1].left, "columns are adjacent");
		assert_equal(rects[1].right, rects[2].left, "columns remain adjacent");
		assert_equal(300, rects[2].right, "columns cover the list width");
		assert_equal(true, rects[0].width() > rects[2].width(), "weights affect widths");
		assert_equal(0, view.column_at({5, 2}), "first header hit");
		assert_equal(2, view.column_at({rects[2].left + 5, 2}), "last header hit");
		assert_equal(-1, view.column_at({5, view._header_height + 10}), "row is not header");

		assert_equal(-1, view.sort_column(), "initially unsorted");
		view.on_mouse_down({rects[1].left + 5, 2}, {});
		assert_equal(1, view.sort_calls, "sort callback");
		assert_equal(1, view.sorted_column, "sorted column");
		assert_equal(true, view.sorted_ascending, "default ascending");
		view.on_mouse_down({rects[1].left + 5, 2}, {});
		assert_equal(false, view.sort_ascending(), "second click reverses");
		view.on_mouse_down({rects[2].left + 5, 2}, {});
		assert_equal(2, view.sorted_column, "new column selected");
		assert_equal(true, view.sorted_ascending, "new column restarts default");

		ui::test::fake_draw_context draw({0, 0, 400, 200});
		view.set_sort(2, false);
		view.paint(draw, false);
		assert_equal(true, draw.drawn_text().find("Sym") != std::string::npos, "header text");
		assert_equal(true, draw.drawn_text().find("AAPL") != std::string::npos, "first cell");
		assert_equal(true, draw.drawn_text().find("182.40") != std::string::npos, "second cell");
		assert_equal(true, draw.drawn_text().find("+1.2%") != std::string::npos, "third cell");
		bool sorted_marked = false;
		for (const auto& text : draw.texts)
			if (text.text.find("Chg% ▼") != std::string::npos)
				sorted_marked = true;
		assert_equal(true, sorted_marked, "sort marker");
		bool coloured = false;
		for (const auto& text : draw.texts)
			if (text.text == "+1.2%" && text.color == core::color(theme.match_highlight)) coloured = true;
		assert_equal(true, coloured, "cell colour");
		view.set_selected(0);
		assert_equal_strict("AAPL\t182.40\t+1.2%", view.selected_rows_text(), "cell copy format");

		sorting_probe numeric(theme);
		numeric.set_columns({{"Sym", 40, false, true}, {"Price", 30, true, false}});
		size_list(numeric);
		const auto numeric_rects = numeric.column_rects(300);
		numeric.on_mouse_down({numeric_rects[1].left + 5, 2}, {});
		assert_equal(false, numeric.sorted_ascending, "numeric default descending");
	}

	void should_copy_rows_with_control_and_context()
	{
		struct clipboard_list : list_probe
		{
			using list_probe::list_probe;
			mutable std::string board;
			bool set_clipboard(const std::string_view text) const override
			{
				board = text;
				return true;
			}
		};

		const ui::view_theme theme;
		clipboard_list view(theme);
		for (int i = 0; i < 4; ++i) view.add(std::format("row {}", i));
		size_list(view);
		view.set_selected(1);
		view.on_key('C', {.control = true});
		assert_equal_strict("row 1", view.board, "control copy");
		view.board.clear();
		view.navigate_next(true, false, true);
		view.on_key('C', {.control = true});
		assert_equal_strict("row 1\r\nrow 2", view.board, "range copy");
		view.board.clear();
		view.on_key('C', {.control = true, .shift = true});
		assert_equal_strict("", view.board, "shift copy is not swallowed");

		view.set_selected(0);
		auto commands = view.default_popup_menu();
		assert_equal(1u, static_cast<unsigned>(commands.size()), "copy command count");
		assert_equal_strict("&Copy", commands[0]->text, "copy command label");
		assert_equal(true, commands[0]->enable, "copy command enabled");
		view.set_selected(-1);
		commands = view.default_popup_menu();
		assert_equal(false, commands[0]->enable, "copy command disabled");

		view.set_selected(0);
		const auto row_y = [&](const int row) { return view._items[row]->bounds.top + 1; };
		(void)view.context_commands({10, row_y(3)});
		assert_equal(3, view.selected_index(), "context click changes selection");
		view.set_selected(1);
		view.navigate_next(true, false, true);
		view.navigate_next(true, false, true);
		(void)view.context_commands({10, row_y(2)});
		assert_equal_strict("row 1\r\nrow 2\r\nrow 3", view.selected_rows_text(), "context preserves range");
		(void)view.context_commands({10, row_y(3)});
		assert_equal_strict("row 1\r\nrow 2\r\nrow 3", view.selected_rows_text(), "context inside range preserves it");
	}

	void should_draw_csv_and_hex_views()
	{
		ui::test::recording_view_host host;
		const ui::view_theme theme;
		ui::csv_view csv(host, theme);
		const auto buffer = show_document(csv, host, "name,qty,note\nwidget,12,the long one\nbolt,3,short");
		assert_equal(3u, static_cast<unsigned>(csv.table().col_widths.size()), "CSV column count");
		assert_equal(6, csv.table().col_widths[0], "CSV first width");
		assert_equal(12, csv.table().col_widths[2], "CSV third width");
		ui::test::fake_draw_context draw({0, 0, 480, 320});
		csv.paint(draw, false);
		assert_equal(true, draw.drawn_text().find("name") != std::string::npos, "CSV header");
		assert_equal(true, draw.drawn_text().find("widget") != std::string::npos, "CSV row");
		assert_equal(true, draw.drawn_text().find("the long one") != std::string::npos, "CSV long cell");
		assert_equal(true, draw.drawn_text().find("------") != std::string::npos, "CSV separator");
		assert_equal(3u, static_cast<unsigned>(buffer->size()), "CSV line count");
		assert_equal(true, csv.text_at({0, 0}).y >= 0, "CSV hit line");
		assert_equal(0, csv.text_at({0, 100}).x, "CSV left hit");
		assert_equal(true, csv.text_at({470, 100}).x > 0, "CSV right hit");
		assert_equal(true, csv.allows_drag_selection(), "CSV selection enabled");
		csv.set_selection({0, 1, 0, 2});
		assert_equal_strict("widget,12,the long one\r\n", csv.select_text(), "CSV copies source rows");

		ui::test::fake_draw_context quote_draw({0, 0, 480, 320});
		show_document(csv, host, "a,b\n\"x,y\",caf\xC3\xA9");
		assert_equal(4, csv.table().col_widths[1], "CSV counts UTF-8 codepoints");
		csv.paint(quote_draw, false);
		assert_equal(true, quote_draw.drawn_text().find("x,y") != std::string::npos, "CSV keeps quoted comma");
		for (const auto& text : quote_draw.texts)
			assert_equal(true, text.text.empty() || (static_cast<unsigned char>(text.text.front()) & 0xc0u) != 0x80u,
			             "CSV does not split UTF-8");
		show_document(csv, host, "a,b\n\"say \"\"hi\"\"\",z");
		quote_draw.reset();
		csv.paint(quote_draw, false);
		assert_equal(true, quote_draw.drawn_text().find("say \"hi\"") != std::string::npos, "CSV unescapes quotes");
		assert_equal(true, quote_draw.drawn_text().find("\"\"") == std::string::npos, "CSV draws quote once");

		ui::hex_view hex(host, theme);
		show_document(hex, host, std::string("0123456789ABCDEF") + "\n" + std::string("AB\x01"));
		ui::test::fake_draw_context hex_draw({0, 0, 640, 320});
		hex.paint(hex_draw, false);
		const auto hex_text = hex_draw.drawn_text();
		assert_equal(true, hex_text.find("00000000") != std::string::npos, "hex first offset");
		assert_equal(true, hex_text.find("00000010") != std::string::npos, "hex second offset");
		assert_equal(true, hex_text.find("30 31 32 33") != std::string::npos, "hex values");
		assert_equal(true, hex_text.find("0123456789ABCDEF") != std::string::npos, "hex ASCII");
		assert_equal(true, hex_text.find("41 42 01") != std::string::npos, "hex control value");
		assert_equal(true, hex_text.find("AB.") != std::string::npos, "hex control rendering");
		assert_equal(true, hex_text.find('\x01') == std::string::npos, "hex hides control byte");
	}

	// A view lays its rows out from its own corner, so placed anywhere in a tree it must be moved there to
	// draw, and its own coordinates must start at that corner for the pointer and invalidation.
	void should_draw_a_view_where_the_tree_places_it()
	{
		const ui::view_theme theme;
		list_probe view(theme);
		view.add("first row");
		view.bounds = {100, 50, 400, 250};
		ui::test::fake_measure_context measure;
		view.on_placed(measure);
		view.layout_list();

		ui::test::fake_draw_context draw({0, 0, 640, 480});
		view.render(draw, {});
		const auto row = std::find_if(draw.texts.begin(), draw.texts.end(),
		                              [](const auto& text) { return text.text == "first row"; });
		assert_equal(true, row != draw.texts.end(), "the row is drawn");
		if (row == draw.texts.end()) return;
		assert_equal(true, row->x >= 100 && row->x < 400, "the row is drawn within the view horizontally");
		assert_equal(true, row->y >= 50 && row->y < 250, "the row is drawn within the view vertically");
		assert_equal(true, view.origin() == core::pointi{100, 50}, "its coordinates start at its corner");
		assert_equal(true, view.frame_bounds() == core::recti{100, 50, 400, 250}, "it sits at its bounds");

		const auto button = ui::make_button("OK", [] {});
		button->bounds = {100, 50, 180, 80};
		button->render(draw, {10, 20});
		assert_equal(true, button->origin() == core::pointi{10, 20}, "a control draws in its bounds' coordinates");
		assert_equal(true, button->frame_bounds() == core::recti{110, 70, 190, 100}, "and sits at them, moved");
	}
}


void register_ui_list_view_tests(test_registry& tests)
{
	tests.add(std::string("Should lay out and hit-test list rows"), should_layout_and_hit_test_rows);
	tests.add(std::string("Should select, navigate, and copy list rows"), should_select_navigate_and_copy_rows);
	tests.add(std::string("Should paint and clip list rows"), should_paint_rows_and_clip_text);
	tests.add(std::string("Should sort and draw list columns"), should_sort_columns_and_draw_cells);
	tests.add(std::string("Should copy list rows with control and context"), should_copy_rows_with_control_and_context);
	tests.add(std::string("Should draw CSV and hex views"), should_draw_csv_and_hex_views);
	tests.add(std::string("Should draw a view where the tree places it"), should_draw_a_view_where_the_tree_places_it);
}