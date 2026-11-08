// This file is part of libdesktop, a C++20 platform layer for desktop applications
// Copyright 2026  Zac Walker
//
// This program is free software; you can redistribute it and / or modify it
// under the terms of the LGPL License either version 2.1 or later.
// License details are available at https://www.gnu.org/licenses/lgpl-2.1.html
// This program is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY

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
		~csv_view() override;

		[[nodiscard]] bool allows_drag_selection() const override { return true; }
		[[nodiscard]] const table_layout::table_block& table() const { return _table; }

		void set_buffer(const text_buffer_ptr& document, highlight_fn highlight) override;
		void recalc_vert_scrollbar() override;
		void layout() override;
		[[nodiscard]] text_location client_to_text(const core::pointi& point) const override;

	protected:
		void draw_view(render::draw_context& draw) const override;

	private:
		table_layout::table_block _table;

		mutable std::string _line_buf;
		mutable std::vector<std::string_view> _cells;
		mutable std::string _cell_text;
		mutable std::vector<int> _cell_breaks;

		[[nodiscard]] int left_pad() const { return _font_extent.cx * 2; }
		static void cell_breaks_fn(std::vector<int>& out, std::string_view text, int col_width);
		[[nodiscard]] int record_visual_rows(int line_index) const;
		void rebuild_table();
	};
}