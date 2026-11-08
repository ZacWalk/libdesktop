// This file is part of libdesktop, a C++20 platform layer for desktop applications
// Copyright 2026  Zac Walker
//
// This program is free software; you can redistribute it and / or modify it
// under the terms of the LGPL License either version 2.1 or later.
// License details are available at https://www.gnu.org/licenses/lgpl-2.1.html
// This program is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY

// Purpose: Editable document view policy and text-buffer editing commands.

#pragma once

#include "libdesktop/ui/views/doc_view.h"

namespace ui
{
	class edit_doc_view : public doc_view
	{
	public:
		edit_doc_view(view_host& host, const view_theme& theme, view_context* context = nullptr,
		              text_clipboard* clipboard = nullptr) : doc_view(host, theme, context, clipboard)
		{
			traits.typing = true;
			traits.forwards_unhandled_keys = true;
			traits.focus_on_click = true;
			traits.wants_return = true;
		}


		~edit_doc_view() override = default;

		[[nodiscard]] bool allows_text_drag() const override { return _doc && !_doc->is_read_only(); }
		[[nodiscard]] bool is_editable() const override { return true; }

		bool on_text(const std::string_view text) override
		{
			if (!_doc || !_doc->query_editable() || text.empty()) return false;

			undo_group group(*_doc);
			const auto pos = _doc->delete_text(group, _doc->selection());
			_doc->select(_doc->insert_text(group, pos, text));
			reset_caret_blink();
			return true;
		}

		bool on_key(const int key, const platform::key_state keys) override
		{
			if (!_doc) return doc_view::on_key(key, keys);

			const auto k = static_cast<char32_t>(key);
			if (k == platform::keys::BACK && !keys.control && !keys.alt)
			{
				_doc->edit_delete_back();
				reset_caret_blink();
				return true;
			}
			if (k == platform::keys::BACK && keys.control && !keys.alt)
			{
				if (!_doc->has_selection()) _doc->move_word_left(true);
				if (_doc->has_selection()) _doc->edit_delete();
				reset_caret_blink();
				return true;
			}
			if (k == platform::keys::TAB && !keys.shift)
			{
				_doc->edit_tab();
				reset_caret_blink();
				return true;
			}
			if (k == platform::keys::TAB && keys.shift)
			{
				_doc->edit_untab();
				reset_caret_blink();
				return true;
			}
			if (k == platform::keys::DEL && !keys.control && !keys.shift && !keys.alt)
			{
				_doc->edit_delete();
				reset_caret_blink();
				return true;
			}
			if (k == platform::keys::DEL && keys.control && !keys.shift && !keys.alt)
			{
				if (!_doc->has_selection()) _doc->move_word_right(true);
				if (_doc->has_selection()) _doc->edit_delete();
				reset_caret_blink();
				return true;
			}
			if (keys.control && !keys.alt && (k == U'Z' || k == U'Y'))
			{
				if (!_doc->query_editable()) return true;
				if (k == U'Z' && !keys.shift) _doc->edit_undo();
				else if ((k == U'Y' && !keys.shift) || (k == U'Z' && keys.shift)) _doc->edit_redo();
				reset_caret_blink();
				return true;
			}

			return doc_view::on_key(key, keys);
		}

	protected:
		[[nodiscard]] bool can_cut_text() const override
		{
			return _doc && _doc->has_selection();
		}

		[[nodiscard]] bool can_paste_text() const override
		{
			return _doc && _doc->query_editable() && !clipboard_text().empty();
		}

		[[nodiscard]] bool can_delete_text() const override
		{
			return _doc && (_doc->has_selection() || _doc->query_editable());
		}

		// Copy first: deleting before a refusing clipboard loses text from both
		// places, and an undo is not an acceptable recovery path for a cut command.
		bool cut_text_to_clipboard() override
		{
			if (!_doc || !_doc->has_selection() || !_doc->query_editable()) return false;
			if (!set_clipboard(_doc->copy())) return false;
			_doc->edit_delete();
			return true;
		}

		bool paste_text_from_clipboard() override
		{
			if (!can_paste_text()) return false;
			_doc->edit_paste(clipboard_text());
			return true;
		}

		bool delete_selected_text() override
		{
			if (!_doc || !_doc->query_editable()) return false;
			_doc->edit_delete();
			return true;
		}
	};
}