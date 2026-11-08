// This file is part of libdesktop, a C++20 platform layer for desktop applications
// Copyright 2026  Zac Walker
//
// This program is free software; you can redistribute it and / or modify it
// under the terms of the LGPL License either version 2.1 or later.
// License details are available at https://www.gnu.org/licenses/lgpl-2.1.html
// This program is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY

// Purpose: The text-buffer document control: wrapping, selection, editing support, spelling and syntax drawing.

#pragma once

#include "libdesktop/ui/syntax.h"
#include "libdesktop/ui/text/spell.h"
#include "libdesktop/ui/text/text_buffer.h"
#include "libdesktop/ui/views/text_view.h"

#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace ui
{
	constexpr auto TIMER_DRAGSEL = 1001;

	class doc_view : public text_view
	{
	protected:
		bool _drag_selection = false;
		bool _word_selection = false;
		bool _line_selection = false;
		bool _sel_margin = true;
		uint32_t _drag_sel_timer = 0;

		// Dragging the selection itself: pending until the mouse actually moves.
		bool _drag_text_pending = false;
		bool _drag_text = false;
		core::pointi _drag_text_origin{};
		text_location _drop_location{};
		core::pointi _drag_point{};

		text_buffer_ptr _doc;

		int _screen_chars = 0;
		int _total_visual_rows = 0;

		// Text layout scratch buffers.
		mutable std::vector<text_block> _block_buf;
		mutable std::vector<text_block> _spell_blocks;
		mutable std::string _expand_buf;
		mutable std::string _render_buf; // line text being drawn / hit-tested
		mutable std::string _highlight_buf; // line text being parsed by the highlighter

		// Word wrap state.
		bool _word_wrap = true;
		std::vector<int> _wrap_breaks; // all break points, concatenated across all lines
		std::vector<int> _wrap_offsets;
		std::vector<int> _wrap_line_y; // prefix sum: _wrap_line_y[i] = first visual row of doc line i
		std::vector<int> _line_breaks_buf; // scratch for one line's break points

		// Lines whose text changed since the last layout; -1 means none.
		int _wrap_dirty_first = -1;
		int _wrap_dirty_last = -1;
		bool _wrap_dirty_all = true;

		// Syntax highlighting — owned by the view, not the document.
		highlight_fn _highlight;
		mutable std::vector<uint32_t> _parse_cookies;
		mutable int _cookies_valid_to = 0; // cookies at or above this line are stale

	public:
		doc_view(view_host& host, const view_theme& th, view_context* context = nullptr,
		         text_clipboard* clipboard = nullptr);
		doc_view(const view_theme& th, view_context* context = nullptr,
		         text_clipboard* clipboard = nullptr);
		~doc_view() override;

		// Whether this view accepts edits. The application uses it to choose its menu.
		[[nodiscard]] virtual bool is_editable() const { return false; }

		// The buffer on show, and where a click lands in it. Both are public so an
		// application can build a context menu about the text under the cursor.
		[[nodiscard]] const text_buffer_ptr& buffer() const { return _doc; }
		[[nodiscard]] text_location text_at(const core::pointi& client_pt) const { return client_to_text(client_pt); }

		// What a right-click offers is the application's command table, when supplied,
		// or the view's own verbs when it is hosted without one.
		std::vector<command_ptr> on_popup_menu(core::pointi client_pt) override;
		[[nodiscard]] std::vector<command_ptr> default_popup_menu() override;

		[[nodiscard]] virtual bool has_caret() const { return true; }
		// Views that lay text out non-uniformly can override hit testing and keep this
		// enabled; the default is the fixed-grid document behaviour.
		[[nodiscard]] virtual bool allows_drag_selection() const { return true; }
		[[nodiscard]] virtual bool allows_text_drag() const { return false; }

		[[nodiscard]] static text_location adjust_for_removal(const text_selection& sel,
		                                                     const text_location& target);

		// Show a buffer, and say how to highlight it. The view cannot choose a
		// highlighter itself because that depends on where the text came from.
		virtual void set_buffer(const text_buffer_ptr& d, highlight_fn highlight);

		void mark_wrap_dirty_all() { _wrap_dirty_all = true; }
		void mark_wrap_dirty(int start, int end);

		// --- Text selection overrides (coordinate with the document) ---
		[[nodiscard]] text_selection current_selection() const override;
		void set_selection(const text_selection& sel) override;
		[[nodiscard]] bool has_current_selection() const override;
		[[nodiscard]] std::string select_text() const override;
		void select_all() override;

		// --- Syntax highlighting (view-owned) ---
		void reset_parse_cookies() const;
		[[nodiscard]] bool cookie_valid(int line_index) const;
		void discard_cookies_below(int line_index) const;
		void set_parse_cookie(int line_index, uint32_t cookie) const;
		uint32_t highlight_cookie(int line_index) const;
		uint32_t highlight_line(uint32_t cookie, std::string_view line_text,
		                       std::span<text_block> buf, int& block_count) const;

		// --- The control surface ---
		void paint(render::draw_context& draw, bool focused) const override;
		void on_focus(bool focused, bool select_all) override;
		bool on_key(int key, platform::key_state keys) override;
		bool on_text(std::string_view text) override;
		void on_mouse_down(core::pointi point, platform::key_state keys) override;
		void on_mouse_move(core::pointi point, bool captured) override;
		void on_mouse_up(core::pointi point, platform::key_state keys) override;
		void on_double_click(core::pointi point, platform::key_state keys) override;
		void on_mouse_leave() override;
		bool on_wheel(core::pointi point, int delta, platform::key_state keys) override;
		std::vector<command_ptr> context_commands(core::pointi point) override;
		platform::cursor cursor_at(core::pointi point) const override;
		core::recti caret_bounds() const override;
		void blink() override;
		void tick() override;
		void scale_changed() override;
		void on_placed(render::measure_context& measure) override;

		// --- App-level interface ---
		[[nodiscard]] bool word_wrap() const override { return _word_wrap; }
		void set_word_wrap(bool enabled) override;
		void toggle_word_wrap() override;
		[[nodiscard]] int wrap_width() const;
		void calc_line_wrap_into(const text_line& line, std::vector<int>& breaks);
		std::span<const int> line_breaks(int line_index) const;
		int line_break_count(int line_index) const;
		int char_to_sub_row(int line_index, int char_index) const;
		int visual_row_to_line_index(int visual_row) const;
		int line_visual_rows(int line_index) const;
		void ensure_visible(const text_location& point) override;
		void recalc_vert_scrollbar() override;
		void recalc_horz_scrollbar() override;
		void invalidate_lines(int start, int end) override;
		void lines_changed(int start, int end) override;
		void line_count_changed(int at, int delta) override;
		void layout() override;

	protected:
		// The following are virtual layout seams for markdown and table views. They
		// describe the same positions used by paint, hit testing and scrolling.
		[[nodiscard]] virtual int client_to_line(const core::pointi& point) const;
		[[nodiscard]] virtual text_location client_to_text(const core::pointi& point) const;
		[[nodiscard]] virtual core::pointi text_to_client(const text_location& point) const;
		[[nodiscard]] virtual int top_content_padding() const;
		[[nodiscard]] virtual int bottom_content_padding() const;
		[[nodiscard]] virtual int right_content_padding() const;
		[[nodiscard]] virtual int line_offset(int line_index) const;
		[[nodiscard]] int line_offset_vrow(int visual_row) const;
		[[nodiscard]] virtual int line_height(int line_index) const;
		[[nodiscard]] bool line_fully_selected(int line_index, int length) const;
		[[nodiscard]] int margin_width() const;
		[[nodiscard]] int text_left() const;
		[[nodiscard]] core::color32 line_bg_color(int line_index) const;

		void draw_line(render::draw_context& draw, core::pointi& origin, const core::recti& clip,
		               std::string_view chars, int offset, int count,
		               render::font_face font, core::color32 text_color, core::color32 bg_color) const;
		void draw_line(render::draw_context& draw, core::pointi& origin, const core::recti& clip,
		               text_style style, std::string_view chars, int offset, int count,
		               const text_location& text_pos, render::font_face font,
		               core::color32 text_color, core::color32 bg_color) const;
		void draw_wrapped_line(render::draw_context& draw, const core::recti& rect, int line_index,
		                       render::font_face font) const;
		void draw_line(render::draw_context& draw, const core::recti& rect, int line_index,
		               render::font_face font) const;
		void draw_margin(render::draw_context& draw, const core::recti& rect, int line_index,
		                 render::font_face font) const;
		virtual void draw_view(render::draw_context& draw) const;
		virtual void draw_caret(render::draw_context& draw) const;
		void draw_drop_marker(render::draw_context& draw) const;

		void start_caret_blink();
		void stop_caret_blink();
		void reset_caret_blink();
		void update_caret();

		void set_scroll_pixel(int new_y);
		void set_scroll_char(int x);
		[[nodiscard]] int scroll_line() const;
		[[nodiscard]] int scroll_char() const;
		void scroll_to_char(int x);
		void scroll_to_line(int y);
		void wrap_scroll_by(int delta);
		void scroll_by(int delta);
		[[nodiscard]] bool can_scroll() const;
		[[nodiscard]] core::recti scrollbar_rect() const;
		[[nodiscard]] int text_top() const;
		[[nodiscard]] core::recti client_rect() const;
		[[nodiscard]] int max_scroll_pixel() const;
		[[nodiscard]] core::color32 document_focus_band_color() const;
		[[nodiscard]] int focus_band_padding() const;
		[[nodiscard]] core::recti full_width_band_rect(int top, int height) const;
		void draw_message_bar(render::draw_context& draw) const;

		void clear_wrap_dirty();
		void splice_parse_cookies(int at, int delta) const;
		void splice_wrap(int at, int delta);
		void full_layout(int line_count);
		void relayout_line(int line_index);

		void on_timer_drag_selection();
		void on_left_button_down(core::pointi point, platform::key_state keys);
		void on_left_button_up(core::pointi point, platform::key_state keys);
		void on_left_button_double_click(core::pointi point, platform::key_state keys);
		void on_right_button_down(core::pointi point, platform::key_state keys);
		void drop_selected_text(const text_location& target, bool copy);
		void cancel_text_drag();
		void invalidate_selection();
		void update_cursor(core::pointi point) const;
	};
}