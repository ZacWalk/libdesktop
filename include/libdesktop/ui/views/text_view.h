// This file is part of libdesktop, a C++20 platform layer for desktop applications
// Copyright 2026  Zac Walker
//
// This program is free software; you can redistribute it and / or modify it
// under the terms of the LGPL License either version 2.1 or later.
// License details are available at https://www.gnu.org/licenses/lgpl-2.1.html
// This program is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY

// Purpose: The control-backed text view contract shared by document views.

#pragma once

#include "libdesktop/ui/text/text_buffer.h"
#include "libdesktop/ui/text/view_host.h"
#include "libdesktop/ui/text/clipboard.h"
#include "libdesktop/ui/views/view_base.h"

#include <string>
#include <vector>

namespace ui
{
	class doc_view;

	// Application policy which is deliberately outside the document model: status
	// text, Escape and zoom, and a context menu supplied by the owning pane.
	class view_context
	{
	public:
		virtual ~view_context() = default;

		[[nodiscard]] virtual std::string_view status_text() const { return {}; }
		virtual void on_escape() {}
		virtual void on_zoom(int delta) { (void)delta; }
		virtual std::vector<command_ptr> popup_menu_items(doc_view& view, const core::pointi& at)
		{
			(void)view;
			(void)at;
			return {};
		}
	};

	class text_view : public view_base
	{
	protected:
		// Kept as a view_host so the text buffer's notifications reach the view; the control host remains
		// available through control::host() for drawing input.
		view_host& _host;
		view_context* _context = nullptr;
		text_clipboard* _clipboard = nullptr;

		bool _focused = false;
		core::sizei _font_extent = {10, 10};
		int _screen_lines = 0;
		mutable render::font_face _body_face = render::font_face::dialog;
		mutable bool _body_face_valid = false;
		bool _caret_visible = true;

		text_view(view_host& host, const view_theme& theme, view_context* context = nullptr,
		          text_clipboard* clipboard = nullptr);
		text_view(const view_theme& theme, view_context* context = nullptr,
		          text_clipboard* clipboard = nullptr);

		[[nodiscard]] static view_host& default_view_host();
		[[nodiscard]] render::font_face body_face() const;
		void set_scroll_char(int value) { _scroll_offset.x = value * std::max(1, _font_extent.cx); }
		void notify_view_invalidated() const;

		// Derived document views provide their actual pixels through this hook.
		void draw_view(render::draw_context& dc) const override { (void)dc; }

		[[nodiscard]] virtual bool has_caret() const { return false; }
		[[nodiscard]] virtual bool allows_drag_selection() const { return true; }
		[[nodiscard]] virtual bool allows_text_drag() const { return false; }

		virtual void select_all() = 0;
		[[nodiscard]] virtual std::string select_text() const = 0;

		virtual void scroll_content_to_top() { _scroll_offset = {}; }
		virtual void scroll_content_to_end() { set_scroll_pixel(max_scroll_y()); }

		void on_escape();
		void zoom(int delta);
		void draw_message_bar(render::draw_context& dc) const;

	public:
		~text_view() override = default;

		[[nodiscard]] int scroll_line() const { return _font_extent.cy > 0 ? _scroll_offset.y / _font_extent.cy : 0; }
		[[nodiscard]] int scroll_char() const { return _font_extent.cx > 0 ? _scroll_offset.x / _font_extent.cx : 0; }
		[[nodiscard]] core::sizei font_extent() const { return _font_extent; }
		[[nodiscard]] bool focused() const { return _focused; }
		[[nodiscard]] virtual std::string_view status_text() const;
		[[nodiscard]] int message_bar_height() const;

		// The face is cached because layout and paint ask for it for every line.
		[[nodiscard]] virtual render::font_face body_font() const { return body_face(); }
		[[nodiscard]] virtual const render::font_desc& body_font_desc() const { return _theme.text_font; }

		[[nodiscard]] virtual text_selection current_selection() const = 0;
		virtual void set_selection(const text_selection& selection) = 0;
		[[nodiscard]] virtual bool has_current_selection() const = 0;
		[[nodiscard]] virtual bool can_copy_text() const { return has_current_selection(); }
		[[nodiscard]] virtual bool can_cut_text() const { return false; }
		[[nodiscard]] virtual bool can_paste_text() const { return false; }
		[[nodiscard]] virtual bool can_delete_text() const { return false; }

		virtual bool copy_text_to_clipboard();
		virtual bool cut_text_to_clipboard() { return false; }
		virtual bool paste_text_from_clipboard() { return false; }
		virtual bool delete_selected_text() { return false; }
		void select_all_text() { select_all(); }

		[[nodiscard]] virtual bool word_wrap() const { return false; }
		virtual void set_word_wrap(bool enabled) { (void)enabled; }
		virtual void toggle_word_wrap() {}
		virtual void layout() {}
		virtual void update_caret() {}
		virtual void recalc_horz_scrollbar();
		virtual void recalc_vert_scrollbar();

		// These are virtual so a model can keep its own host notifications while
		// the default path invalidates the control in the element tree.
		virtual void ensure_visible(const text_location& point);
		virtual void invalidate_lines(int start, int end);
		virtual void lines_changed(int start, int end);
		virtual void line_count_changed(int at, int delta);

		[[nodiscard]] virtual std::string clipboard_text() const;
		virtual bool set_clipboard(std::string_view text) const;

		[[nodiscard]] virtual std::vector<command_ptr> default_popup_menu();
		[[nodiscard]] virtual std::vector<command_ptr> on_popup_menu(core::pointi client_point);

		// The control events the view handles.
		void on_placed(render::measure_context& mc) override;
		void on_focus(bool focused, bool select_all) override;
		void on_keyboard(bool has_keyboard) override;
		bool on_key(int key, platform::key_state keys) override;
		bool on_text(std::string_view text) override;
		bool on_wheel(core::pointi loc, int delta, platform::key_state keys) override;
		std::vector<command_ptr> context_commands(core::pointi loc) override;
		platform::cursor cursor_at(core::pointi loc) const override;
		core::recti caret_bounds() const override { return {}; }
		void blink() override;
		void scale_changed() override;

		void start_caret_blink();
		void reset_caret_blink();
		void stop_caret_blink();
		[[nodiscard]] bool caret_visible() const { return _caret_visible; }

		void scroll_to_top();
		void scroll_to_line(int line);
		void set_scroll_pixel(int new_y);
		[[nodiscard]] int text_top() const { return message_bar_height(); }
		void scroll_by(int delta);
		[[nodiscard]] int line_content_offset(int line) const;
		[[nodiscard]] int centered_text_top(int top, int height) const;
		[[nodiscard]] int centered_text_top(core::recti rect) const
		{
			return centered_text_top(rect.top, rect.height());
		}
		[[nodiscard]] core::recti full_width_band_rect(int top, int height) const
		{
			return {0, top, _view_extent.cx, top + height};
		}
		[[nodiscard]] core::color32 focus_band_color() const;
		[[nodiscard]] int focus_band_padding() const;

		template <typename AdvanceFn>
		static void calc_word_breaks_into(std::vector<int>& breaks, std::string_view text, int max_cols,
		                                  AdvanceFn&& char_advance)
		{
			breaks.clear();
			const auto len = static_cast<int>(text.size());
			int col = 0;
			int last_break_opportunity = -1;
			int col_at_break = 0;

			for (int i = 0; i < len; ++i)
			{
				const auto advance = char_advance(i, col);
				if (text[static_cast<size_t>(i)] == ' ' || text[static_cast<size_t>(i)] == '\t')
				{
					last_break_opportunity = i + 1;
					col_at_break = col + advance;
				}

				if (col > 0 && col + advance > max_cols)
				{
					if (last_break_opportunity > (breaks.empty() ? 0 : breaks.back()))
					{
						breaks.push_back(last_break_opportunity);
						col = col - col_at_break + advance;
					}
					else
					{
						breaks.push_back(i);
						col = advance;
					}
					last_break_opportunity = -1;
					col_at_break = 0;
				}
				else
				{
					col += advance;
				}
			}
		}
	};
}