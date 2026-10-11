// view_doc_edit.h — an editable document view.
//
// Character input, backspace, delete, tab, undo and the clipboard verbs, all
// expressed as text buffer edits. What a right-click offers is left to the
// application: see doc_view::on_popup_menu.

#pragma once

#include "ui/view_doc.h"

namespace pf::ui
{
	class edit_doc_view : public doc_view
	{
	public:
		edit_doc_view(view_host& host, const theme& th, view_context* context = nullptr) : doc_view(host, th, context)
		{
		}

		~edit_doc_view() override = default;

		[[nodiscard]] bool allows_text_drag() const override { return _doc && !_doc->is_read_only(); }

		[[nodiscard]] bool is_editable() const override { return true; }

	protected:
		[[nodiscard]] bool can_cut_text() const override
		{
			return _doc->has_selection();
		}

		[[nodiscard]] bool can_paste_text() const override
		{
			return text_buffer::can_paste();
		}

		[[nodiscard]] bool can_delete_text() const override
		{
			return _doc->has_selection() || _doc->query_editable();
		}

		// Copy first, and only remove the text once the clipboard has taken it.
		// edit_cut() deletes and *then* returns what it deleted, so handing its
		// result to a clipboard that refuses it loses the text from both places —
		// recoverable only by an undo the person has no reason to know they need.
		bool cut_text_to_clipboard() override
		{
			if (!_doc->has_selection() || !_doc->query_editable())
				return false;

			if (!set_clipboard(_doc->copy()))
				return false;

			_doc->edit_delete();
			return true;
		}

		bool paste_text_from_clipboard() override
		{
			if (!text_buffer::can_paste())
				return false;
			_doc->edit_paste(clipboard_text());
			return true;
		}

		bool delete_selected_text() override
		{
			if (!_doc->query_editable())
				return false;
			_doc->edit_delete();
			return true;
		}

		void on_char(pf::window_frame_ptr& window, const char32_t c) override
		{
			if (window->is_key_down_async(pf::platform_key::LButton) ||
				window->is_key_down_async(pf::platform_key::RButton))
				return;

			if (!_doc->query_editable())
				return;

			if (c == pf::platform_key::Return)
			{
				undo_group ug(_doc);
				const auto pos = _doc->delete_text(ug, _doc->selection());
				_doc->select(_doc->insert_text(ug, pos, u8'\n'));
			}
			else if (c > 31 && c != 0x7F)
			{
				undo_group ug(_doc);
				const auto pos = _doc->delete_text(ug, _doc->selection());
				_doc->select(_doc->insert_text(ug, pos, pf::utf8_encode(c)));
			}
		}

		bool on_key_down(pf::window_frame_ptr& window, const unsigned int vk) override
		{
			namespace pk = pf::platform_key;
			const bool ctrl = window->is_key_down(pk::Control);
			const bool shift = window->is_key_down(pk::Shift);
			const bool alt = window->is_key_down(pk::Alt);

			// Only keys the command table does not claim reach here
			if (vk == pk::Back && !ctrl && !alt)
			{
				_doc->edit_delete_back();
				return true;
			}
			if (vk == pk::Back && ctrl)
			{
				// With a selection, Ctrl+Backspace takes just the selection, as in a
				// Windows edit box, rather than growing it by a word first.
				if (!_doc->has_selection())
					_doc->move_word_left(true);
				if (_doc->has_selection())
					_doc->edit_delete();
				return true;
			}
			if (vk == pk::Tab && !shift)
			{
				_doc->edit_tab();
				return true;
			}
			if (vk == pk::Tab && shift)
			{
				_doc->edit_untab();
				return true;
			}

			// Like Ctrl+A/C/X/V in text_view: a view with no command table behind it
			// gets no Delete or Undo command either, so these keys are its own. Where
			// a table does claim them its accelerators answer first and none of
			// these is reached.
			if (vk == pk::Delete && !ctrl && !shift && !alt)
			{
				_doc->edit_delete();
				return true;
			}
			if (vk == pk::Delete && ctrl && !shift && !alt)
			{
				if (!_doc->has_selection())
					_doc->move_word_right(true);
				if (_doc->has_selection())
					_doc->edit_delete();
				return true;
			}
			if (ctrl && !alt && (vk == 'Z' || vk == 'Y'))
			{
				if (!_doc->query_editable())
					return true;
				if (vk == 'Z' && !shift)
					_doc->edit_undo();
				else if ((vk == 'Y' && !shift) || (vk == 'Z' && shift))
					_doc->edit_redo();
				return true;
			}

			return doc_view::on_key_down(window, vk);
		}
	};
}
