// This file is part of libdesktop, a C++20 platform layer for desktop applications
// Copyright 2026  Zac Walker
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

// Purpose: Read-only document view policy layered over the shared document view.

#pragma once

#include "libdesktop/ui/views/doc_view.h"

#include <algorithm>

namespace ui
{
	class read_only_doc_view : public doc_view
	{
	public:
		read_only_doc_view(view_host& host, const view_theme& theme, view_context* context = nullptr,
		                   text_clipboard* clipboard = nullptr) : doc_view(host, theme, context, clipboard)
		{
			traits.typing = false;
			traits.focus_on_click = true;
		}

		read_only_doc_view(const view_theme& theme, view_context* context = nullptr,
		                   text_clipboard* clipboard = nullptr) : doc_view(theme, context, clipboard)
		{
			traits.typing = false;
			traits.focus_on_click = true;
		}

		~read_only_doc_view() override = default;

		[[nodiscard]] bool has_caret() const override { return false; }
		[[nodiscard]] bool allows_drag_selection() const override { return false; }

		[[nodiscard]] bool at_bottom() const
		{
			return _scroll_offset.y >= std::max(0, max_scroll_pixel() - _font_extent.cy);
		}

		void scroll_to_end() { scroll_content_to_end(); }

		void set_word_wrap(bool /*enabled*/) override {}
		void toggle_word_wrap() override {}

		void recalc_horz_scrollbar() override
		{
			_scroll_offset.x = 0;
			_hscroll.update(0, 0, 0);
		}

		bool on_wheel(const core::pointi loc, const int delta, const platform::key_state keys) override
		{
			(void)loc;
			if (keys.control)
			{
				if (_context) _context->on_zoom(delta > 0 ? 2 : -2);
				return true;
			}
			if (delta == 0 || max_scroll_pixel() <= 0) return false;
			set_scroll_pixel(_scroll_offset.y - delta * std::max(1, _font_extent.cy));
			return true;
		}

		bool on_key(const char32_t key, const platform::key_state keys) override
		{
			(void)keys; // Shift does not turn a read-only viewport into a caret editor.
			if (key == platform::keys::UP)
			{
				scroll_by(-1);
				return true;
			}
			if (key == platform::keys::DOWN)
			{
				scroll_by(1);
				return true;
			}
			if (key == platform::keys::PRIOR)
			{
				scroll_by(-_screen_lines);
				return true;
			}
			if (key == platform::keys::NEXT)
			{
				scroll_by(_screen_lines);
				return true;
			}
			if (key == platform::keys::HOME)
			{
				scroll_content_to_top();
				recalc_vert_scrollbar();
				_host.invalidate_status();
				invalidate();
				return true;
			}
			if (key == platform::keys::END)
			{
				scroll_content_to_end();
				return true;
			}
			// NOLINTNEXTLINE(bugprone-parent-virtual-call): read-only, it skips doc_view's caret keys
			return text_view::on_key(key, keys);
		}

	protected:
		void scroll_content_to_top() override
		{
			_scroll_offset = {};
			recalc_vert_scrollbar();
			_host.invalidate_status();
		}

		void scroll_content_to_end() override { set_scroll_pixel(max_scroll_pixel()); }

		[[nodiscard]] int max_scroll_pixel() const
		{
			return std::max(0, _content_extent.cy - (_view_extent.cy - text_top()));
		}

		static int safe_cols(const int width, const int char_width)
		{
			return char_width > 0 ? std::max(1, width / char_width) : 1;
		}
	};
}
