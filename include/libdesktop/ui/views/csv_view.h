// This file is part of libdesktop, a C++20 platform layer for desktop applications
// Copyright 2026  Zac Walker
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

// Purpose: An aligned, source-preserving CSV table built on the document view.

#pragma once

#include "libdesktop/ui/text/table_layout.h"
#include "libdesktop/ui/views/read_only_doc_view.h"

#include <string>
#include <string_view>
#include <vector>

namespace ui
{
	class csv_view : public read_only_doc_view
	{
	public:
		csv_view(view_host& host, const view_theme& theme, view_context* context = nullptr,
		         text_clipboard* clipboard = nullptr);
		csv_view(const view_theme& theme, view_context* context = nullptr, text_clipboard* clipboard = nullptr);
		~csv_view() override;

		[[nodiscard]] bool allows_drag_selection() const override { return true; }
		[[nodiscard]] const table_layout::table_block& table() const { return _table; }

		void set_buffer(const text_buffer_ptr& document, highlight_fn highlight) override;
		void recalc_vert_scrollbar() override;
		void layout() override;
		[[nodiscard]] text_location client_to_text(const core::pointi& point) const override;
		void on_mouse_down(core::pointi point, platform::key_state keys) override;
		void on_mouse_move(core::pointi point, bool captured) override;
		void on_mouse_up(core::pointi point, platform::key_state keys) override;
		void on_double_click(core::pointi point, platform::key_state keys) override;
		bool on_key(char32_t key, platform::key_state keys) override;
		void on_focus(bool focused, bool select_all) override;
		void on_capture_lost() override;
		platform::cursor cursor_at(core::pointi point) const override;

	protected:
		void draw_view(render::draw_context& draw) const override;

	private:
		table_layout::table_block _table;
		table_layout::resize_gesture _column_drag;

		mutable std::string _line_buf;
		mutable std::vector<std::string_view> _cells;
		mutable std::string _cell_text;
		mutable std::vector<int> _cell_breaks;

		[[nodiscard]] int left_pad() const { return _font_extent.cx * 2; }
		static void cell_breaks_fn(std::vector<int>& out, std::string_view text, int col_width);
		[[nodiscard]] int record_visual_rows(int line_index) const;
		void rebuild_table();
		[[nodiscard]] int divider_at(core::pointi point) const;
		void columns_changed();
		bool cancel_column_drag();
	};
}
