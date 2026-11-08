// This file is part of libdesktop, a C++20 platform layer for desktop applications
// Copyright 2026  Zac Walker
//
// This program is free software; you can redistribute it and / or modify it
// under the terms of the LGPL License either version 2.1 or later.
// License details are available at https://www.gnu.org/licenses/lgpl-2.1.html
// This program is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY

// Purpose: The source-preserving Markdown document control, including links, headings, wrapping and tables.

#pragma once

#include "libdesktop/ui/text/markdown.h"
#include "libdesktop/ui/text/spell.h"
#include "libdesktop/ui/text/table_layout.h"
#include "libdesktop/ui/views/read_only_doc_view.h"

#include <cstdint>
#include <functional>
#include <span>
#include <string>
#include <vector>

namespace ui
{
	class markdown_view : public read_only_doc_view
	{
	public:
		// Resolving a target is application policy, so the control reports the hit and does not open it.
		using link_fn = std::function<void(std::string_view target)>;

		markdown_view(view_host& host, const view_theme& th, view_context* context = nullptr,
		              text_clipboard* clipboard = nullptr);
		markdown_view(const view_theme& th, view_context* context = nullptr,
		              text_clipboard* clipboard = nullptr);
		~markdown_view() override;

		void set_link_handler(link_fn handler);
		[[nodiscard]] bool has_link_handler() const;

		struct link_hit
		{
			std::string target;
			int line = -1;
			int span = -1;
		};

		[[nodiscard]] link_hit link_hit_at(const core::pointi& point) const;
		[[nodiscard]] std::string link_at(const core::pointi& point) const;

		[[nodiscard]] bool allows_drag_selection() const;

		void set_buffer(const text_buffer_ptr& document, highlight_fn highlight);
		void on_placed(render::measure_context& measure);
		void layout();
		void recalc_vert_scrollbar();

		[[nodiscard]] int line_offset(int line_index) const;
		[[nodiscard]] int line_height(int line_index) const;
		[[nodiscard]] int client_to_line(const core::pointi& point) const;
		[[nodiscard]] text_location client_to_text(const core::pointi& point) const;
		[[nodiscard]] core::pointi text_to_client(const text_location& point) const;
		void ensure_visible(const text_location& point);

		void on_mouse_down(core::pointi point, platform::key_state keys);
		void on_mouse_up(core::pointi point, platform::key_state keys);
		void on_mouse_move(core::pointi point, bool captured);
		[[nodiscard]] platform::cursor cursor_at(core::pointi point) const;
		void paint(render::draw_context& draw, bool focused) const override;

	protected:
		static constexpr int heading_levels = 3;

		struct line_metrics
		{
			int y = 0;
			int height = 0;
			int lead = 0;
			int trail = 0;
			int rows = 0;
			int row_height = 0;
			int char_width = 0;
			int indent = 0;
			int table = -1;
			uint8_t heading = 0;
			bool in_fence = false;
		};

		struct style_run
		{
			int start = 0;
			int length = 0;
			text_style style = text_style::normal_text;
		};

		struct text_run
		{
			int start = 0;
			int length = 0;
			core::color32 color = 0;
		};

		link_fn _on_link;
		core::pointi _press_point{};
		core::sizei _heading_extent[heading_levels] = {};
		render::font_face _heading_fonts[heading_levels] = {};
		std::vector<line_metrics> _lines;
		std::vector<table_layout::table_block> _tables;
		int _total_content_height = 0;

		mutable std::string _line_buf;
		mutable std::string _table_buf;
		mutable std::string _link_buf;
		mutable md::source_line _source_line;
		mutable md::source_line _link_line;
		mutable std::vector<uint8_t> _byte_style;
		mutable std::vector<style_run> _style_runs;
		mutable std::vector<text_run> _runs;
		mutable std::vector<int> _cell_breaks;
		mutable std::vector<std::string_view> _cells;

		[[nodiscard]] int left_pad() const;
		[[nodiscard]] int right_pad() const;
		[[nodiscard]] int laid_out_lines() const;
		static int clamp_offset(int offset, std::string_view text);
		[[nodiscard]] text_location hit_test(const core::pointi& point, bool* on_text) const;
		[[nodiscard]] int content_y_of(int screen_y) const;
		[[nodiscard]] core::sizei font_extent_for(uint8_t heading) const;
		[[nodiscard]] render::font_face font_for(uint8_t heading) const;
		[[nodiscard]] static uint8_t heading_level(md::block kind);
		static bool offset_within(std::string_view line, std::string_view span, int offset);
		static int continuation_indent(const md::source_line& parsed, std::string_view text);

		static text_style style_for_span(const md::span& span, uint8_t heading, md::block kind);
		static bool spell_check_style(text_style style);
		void build_style_runs(std::string_view text, const md::source_line& parsed, uint8_t heading) const;
		void build_text_runs(std::string_view text, text_style style, core::color32 base_color) const;
		void draw_run(render::draw_context& draw, const core::recti& rect, core::pointi& origin,
		              std::string_view text, int absolute_start, render::font_face font,
		              int font_cx, core::color32 color, core::color32 bg_color,
		              int selection_begin, int selection_end, core::color32 selection_text_color,
		              core::color32 selection_bg_color) const;
		void draw_markdown_line(render::draw_context& draw, int top, int right, int line_index,
		                        const line_metrics& metrics) const;

		static bool is_table_row(std::string_view line);
		static bool is_table_separator(std::string_view line);
		static void cell_breaks_fn(std::vector<int>& out, std::string_view text, int col_width);
		[[nodiscard]] table_layout::table_block find_table(int line_hint, int available_columns) const;
		[[nodiscard]] int table_row_visual_rows(int line_index, const table_layout::table_block& table) const;
		int draw_table_line(render::draw_context& draw, int y, int right, int line_index,
		                    const table_layout::table_block& table) const;

		static void wrap_line_into(std::vector<int>& breaks, std::string_view text, int columns);
		static int offset_at_column(std::string_view text, int start, int end, int column);
		static int column_of(std::string_view text, int start, int offset);
		[[nodiscard]] std::span<const int> markdown_line_breaks(int line_index) const;
		[[nodiscard]] int markdown_char_to_sub_row(int line_index, int byte_offset) const;

	};
}