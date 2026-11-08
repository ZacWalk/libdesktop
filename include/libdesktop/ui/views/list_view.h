// This file is part of libdesktop, a C++20 platform layer for desktop applications
// Copyright 2026  Zac Walker
//
// This program is free software; you can redistribute it and / or modify it
// under the terms of the LGPL License either version 2.1 or later.
// License details are available at https://www.gnu.org/licenses/lgpl-2.1.html
// This program is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY

// Purpose: A virtualised, selectable list control with optional weighted columns.

#pragma once

#include "libdesktop/ui/views/view_base.h"
#include "libdesktop/ui/views/view_theme.h"
#include "libdesktop/ui/text/clipboard.h"
#include "libdesktop/ui/command.h"

#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace ui
{
	class view_context;

	// One column of a list with columns. Widths are shares rather than pixels so a
	// resized control keeps the same proportions.
	struct list_column
	{
		std::string title;
		int weight = 1;
		bool right_align = false;
		bool default_ascending = true;
	};

	// One cell of a row. Most cells use the row colour; exceptional cells can carry
	// their own colour (for example a positive or negative change).
	struct list_cell
	{
		std::string text;
		core::color32 color = 0;
		bool has_color = false;

		list_cell() = default;
		list_cell(std::string t) : text(std::move(t)) {}
		list_cell(std::string t, const core::color32 c) : text(std::move(t)), color(c), has_color(true) {}
	};

	struct list_item
	{
		core::recti bounds;
		std::string text;
		std::string prefix;
		std::vector<list_cell> cells;
		int depth = 0;
		bool is_group = false;
		bool expanded = false;
		int match_start = -1;
		int match_length = 0;
		std::shared_ptr<void> data;

		template <typename T>
		[[nodiscard]] std::shared_ptr<T> as() const
		{
			return std::static_pointer_cast<T>(data);
		}

		[[nodiscard]] bool has_match() const { return match_start >= 0 && match_length > 0; }
	};

	using list_item_ptr = std::shared_ptr<list_item>;

	class list_view : public view_base
	{
	protected:
		view_context* _context = nullptr;
		text_clipboard* _clipboard = nullptr;

		std::vector<list_item_ptr> _items;
		list_item_ptr _selected_item;
		list_item_ptr _hover_item;
		bool _focused = false;
		mutable int _selected_index = -1;
		int _anchor_index = -1;
		core::sizei _font_extent = {10, 10};
		mutable render::font_face _list_face = render::font_face::dialog;
		mutable bool _list_face_valid = false;
		int _header_height = 0;
		std::vector<list_column> _columns;
		int _sort_column = -1;
		bool _sort_ascending = true;

		bool _scroll_tracking = false;

		[[nodiscard]] int scrollbar_reserve() const;
		[[nodiscard]] core::recti scrollbar_rect() const;
		[[nodiscard]] core::color text_color(core::color32 value) const;
		[[nodiscard]] render::font_face list_font() const;
		void update_scrollbar();
		void draw_expand_icon(render::draw_context& dc, int cx, int cy, bool expanded) const;
		void draw_cells(render::draw_context& dc, const list_item& item, const core::recti& bounds,
	                       render::font_face font, core::color32 bg) const;
		[[nodiscard]] int centered_row_text_top(const core::recti& bounds) const;
		[[nodiscard]] int first_visible_item() const;
		void extend_selection_to(const list_item_ptr& item);
		void draw_matched_text(render::draw_context& dc, const list_item& item, const core::recti& content_rect,
	                       render::font_face font, core::color32 bg) const;
		void draw_plain_text(render::draw_context& dc, const list_item& item, const core::recti& content_rect,
	                       render::font_face font, core::color32 bg) const;

		// These hooks are application policy, not list mechanics.
		virtual void draw_header(render::draw_context& dc, const core::recti& header_rect) const;
		virtual void on_sort_changed(int column, bool ascending);
		virtual void on_item_selected(const list_item_ptr& item, bool activated);
		virtual void on_item_activated();
		virtual void zoom(int delta);
		[[nodiscard]] virtual core::color32 item_text_color(const list_item& item) const;

	public:
		explicit list_view(const view_theme& theme, view_context* context = nullptr,
		                   text_clipboard* clipboard = nullptr);
		~list_view() override;

		[[nodiscard]] list_item_ptr selected_item() const { return _selected_item; }
		void set_columns(std::vector<list_column> columns);
		[[nodiscard]] const std::vector<list_column>& columns() const { return _columns; }
		[[nodiscard]] int sort_column() const { return _sort_column; }
		[[nodiscard]] bool sort_ascending() const { return _sort_ascending; }
		void set_sort(int column, bool ascending);
		[[nodiscard]] int column_at(core::pointi point) const;
		[[nodiscard]] std::vector<core::recti> column_rects(int width) const;

		[[nodiscard]] int selected_index() const;
		[[nodiscard]] std::pair<int, int> selected_range() const;
		[[nodiscard]] bool is_selected(int index) const;
		void set_selected(int index);
		[[nodiscard]] bool can_copy_rows() const { return _selected_item != nullptr; }
		[[nodiscard]] virtual std::string row_text(const list_item& item) const;
		[[nodiscard]] std::string selected_rows_text() const;
		bool copy_rows() const;
		virtual bool set_clipboard(std::string_view text) const;
		[[nodiscard]] virtual std::vector<command_ptr> default_popup_menu();

		void navigate_next(bool forward, bool skip_groups = false, bool extend = false);
		[[nodiscard]] int scroll_y() const { return _scroll_offset.y; }
		[[nodiscard]] bool can_scroll() const;
		void scroll_to(int offset);
		void ensure_visible(const list_item_ptr& item);
		[[nodiscard]] list_item_ptr selection_from_point(core::pointi point) const;
		void select_list_item(const list_item_ptr& item, bool activated = true);

		// The list is a control: layout and painting are called by the element tree,
		// while these event methods are called by its input manager.
		core::sizei measure_content(render::measure_context& mc, int width_limit) const override;
		void on_placed(render::measure_context& mc) override;
		void paint(render::draw_context& dc, bool focused) const override;
		void on_focus(bool focused, bool select_all) override;
		bool on_key(int key, platform::key_state keys) override;
		bool on_text(std::string_view text) override;
		void on_mouse_down(core::pointi loc, platform::key_state keys) override;
		void on_mouse_move(core::pointi loc, bool captured) override;
		void on_mouse_up(core::pointi loc, platform::key_state keys) override;
		void on_mouse_leave() override;
		bool on_wheel(core::pointi loc, int delta, platform::key_state keys) override;
		std::vector<command_ptr> context_commands(core::pointi loc) override;
		platform::cursor cursor_at(core::pointi loc) const override;
		void scale_changed() override;

		// Kept public for owners that rebuild rows outside the element tree.
		void layout_list(render::measure_context& mc);
		void layout_list();


	protected:
		void draw_item(render::draw_context& dc, const list_item_ptr& item, const core::recti& bounds,
		               bool selected, bool hovered) const;
	};
}