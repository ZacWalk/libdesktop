// view_composer.h — the box a prompt is typed into.
//
// Two editors were merged here. rethinkify's was a document view, so it already
// had undo, word wrap, word-wise movement, selection and the clipboard; list0's
// had none of those but knew something the other did not — that the text arriving
// in a prompt is untrusted, and has to be filtered and bounded before it becomes
// part of one.
//
// So this is an edit_doc_view with a filter in front of everything that can put
// text in it: typing, pasting, and being set. What is *in* the buffer is therefore
// already safe, and every editing verb above works on it unchanged.
//
// It grows with what is typed and then stops and scrolls, so a long prompt cannot
// swallow whatever it shares a window with.

#pragma once

#include "ui/text_input.h"
#include "ui/view_doc_edit.h"

#include <algorithm>
#include <functional>
#include <string>
#include <vector>

namespace pf::ui
{
	class composer : public edit_doc_view
	{
	public:
		// What happens to a finished prompt, and whether it was taken. A composer
		// whose submission is refused **keeps its text**: the person still has what
		// they wrote, and can act on whatever the refusal was about. An application
		// that always accepts returns true and never notices.
		//
		// list0 is why this is not a void: it refuses a prompt until its cloud-use
		// disclosure has been acknowledged, and has a test asserting the draft
		// survives that refusal.
		using submit_fn = std::function<bool(std::string)>;

		composer(view_host& host, const theme& th, view_context* context = nullptr)
			: edit_doc_view(host, th, context)
		{
			_sel_margin = false;
			_word_wrap = true;
		}

		~composer() override = default;

		submit_fn on_submit;

		static constexpr int default_max_rows = 5;
		static constexpr size_t max_history = 64;

		// --- What the application decides ---

		void set_placeholder(std::string text) { _placeholder = std::move(text); }
		[[nodiscard]] const std::string& placeholder() const { return _placeholder; }

		// How much text may be entered, and whether a newline is one of the things
		// that may be. A single-line composer folds pasted newlines to spaces
		// rather than silently accepting a prompt that is secretly several.
		void set_limit(const size_t bytes) { _limit = std::min(bytes, input::max_editor_bytes); }
		[[nodiscard]] size_t limit() const { return _limit; }

		void set_multiline(const bool enabled) { _multiline = enabled; }
		[[nodiscard]] bool multiline() const { return _multiline; }

		void set_max_rows(const int rows) { _max_rows = std::max(1, rows); }

		// The prompt carries no message bar of its own; whatever it sits under
		// carries the status.
		[[nodiscard]] std::string_view status_text() const override { return {}; }

		// --- Size ---

		// Rows the text needs, capped so the prompt cannot swallow the pane above it
		[[nodiscard]] int rows() const { return std::clamp(total_rows(), 1, _max_rows); }

		[[nodiscard]] int desired_height() const
		{
			return rows() * _font_extent.cy + top_content_padding() + bottom_content_padding();
		}

		// --- Text ---

		[[nodiscard]] std::string text() const { return _doc ? _doc->str() : std::string{}; }

		// Filtered like anything else: text put here programmatically may still
		// have come from somewhere untrusted.
		void set_text(const std::string_view value)
		{
			if (!_doc) return;

			_assembler.cancel();

			{
				undo_group ug(_doc);
				_doc->replace_text(ug, _doc->all(), filter(value));
			}

			move_caret_to_end();
		}

		// Lets the pane above hand over a keystroke that was typed at it
		void type(pf::window_frame_ptr& window, const char32_t ch) { on_char(window, ch); }

		// Offers what is there, if it is anything, and keeps it unless it was taken
		void submit()
		{
			auto content = text();
			if (content.find_first_not_of(" \t\r\n") == std::string::npos) return;
			if (!on_submit) return;

			// Remembered and cleared only on acceptance, so a refused prompt is
			// neither lost from the box nor duplicated into the history.
			if (!on_submit(content)) return;

			remember(content);
			set_text({});
		}

		// --- History ---

		[[nodiscard]] const std::vector<std::string>& history() const { return _history; }

		void clear_history()
		{
			_history.clear();
			_history_pos = -1;
			_draft.clear();
		}

		// --- Input, filtered ---

		// Pasting is where the most text arrives at once and the least of it was
		// typed by the person doing it.
		bool paste_text_from_clipboard() override
		{
			if (!_doc || !_doc->query_editable()) return false;

			const auto filtered = filter(clipboard_text(), remaining_budget());
			if (filtered.empty()) return false;

			_assembler.cancel();
			_doc->edit_paste(filtered);
			return true;
		}

		void handle_paint(pf::window_frame_ptr& window, pf::draw_context& draw) override
		{
			edit_doc_view::handle_paint(window, draw);

			const auto box = client_rect();

			// An empty box says what it is for, focused or not. draw_text fills its
			// rect, which swallows the caret sitting at the start of the line — so
			// the caret goes back on top rather than the hint being hidden whenever
			// somebody is actually about to type into it. The fill is the caret
			// line's own background, which focus turns into the focus band, so the
			// hint does not sit in a box of its own.
			if (!_placeholder.empty() && is_empty())
			{
				const auto y = top_content_padding();
				const auto width = draw.measure_text(_placeholder, body_font()).cx;
				const pf::irect clip(text_left(), y, std::min(box.right, text_left() + width),
				                     y + _font_extent.cy);
				draw.draw_text(clip.left, y, clip, _placeholder, body_font(),
				               _theme.dim_text, line_bg_color(0));

				draw_caret(draw);
			}

			edit_box::draw_border(draw, box, window->has_focus(), _theme.dpi_scale);
		}

		void update_focus(pf::window_frame_ptr& window) override
		{
			edit_doc_view::update_focus(window);

			// Half a character cannot survive the focus leaving
			if (!window->has_focus()) _assembler.cancel();

			// The border and the caret both change with focus
			window->invalidate();
		}

	protected:
		[[nodiscard]] int top_content_padding() const override { return std::max(1, _font_extent.cy / 4); }
		[[nodiscard]] int bottom_content_padding() const override { return std::max(1, _font_extent.cy / 4); }
		// The border is drawn inside the box, so a row stops a cell short of it.
		[[nodiscard]] int right_content_padding() const override { return _font_extent.cx; }

		// A composer is a few rows tall at most, so a horizontal scrollbar drawn
		// inside it sits on top of the line it is meant to help read — and takes
		// the click that should have put the caret there. Scrolling to follow the
		// caret, which the view does anyway, is the whole of what is needed.
		void recalc_horz_scrollbar() override
		{
			if (_word_wrap || !_doc || _screen_chars >= _doc->max_line_length())
				_scroll_offset.x = 0;

			_hscroll.update(0, 0, 0);
		}

		// Every typed character goes through the same filter a paste does, so a
		// control character cannot be typed in either.
		void on_char(pf::window_frame_ptr& window, const char32_t c) override
		{
			// Enter is handled as a key, so Shift can mean "new line"
			if (c == U'\r' || c == U'\n') return;

			if (!_doc || !_doc->query_editable()) return;

			const auto ch = _assembler.accept(c);
			if (ch == 0) return; // waiting for the other half of a surrogate pair

			if (!input::is_printable(ch)) return;
			if (input::utf8_size(ch) > remaining_budget()) return;

			edit_doc_view::on_char(window, ch);
		}

		bool on_key_down(pf::window_frame_ptr& window, const unsigned int vk) override
		{
			namespace pk = pf::platform_key;
			const auto shift = window->is_key_down(pk::Shift);

			// A key is not the other half of a character
			if (vk != pk::Shift && vk != pk::Control) _assembler.cancel();

			// Escape only moves focus, so it is safe to press while a turn is running
			if (vk == pk::Escape)
			{
				on_escape();
				return true;
			}

			if (vk == pk::Return)
			{
				// The base view takes Return itself as "insert a line break"; a bare
				// \n is neither that nor a printable character, so it would vanish.
				if (shift && _multiline && remaining_budget() > 0)
					edit_doc_view::on_char(window, static_cast<char32_t>(pk::Return));
				else if (!shift)
					submit();

				return true;
			}

			// History only takes over at the edges, so the caret can still cross a
			// wrapped prompt
			if (vk == pk::Up && at_first_row()) return recall_history(true);
			if (vk == pk::Down && at_last_row()) return recall_history(false);

			return edit_doc_view::on_key_down(window, vk);
		}

	private:
		std::string _placeholder;
		size_t _limit = input::max_text_bytes;
		bool _multiline = true;
		int _max_rows = default_max_rows;

		input::char_assembler _assembler;

		std::vector<std::string> _history;
		int _history_pos = -1;
		std::string _draft;

		[[nodiscard]] std::string filter(const std::string_view value) const
		{
			return input::normalize(value, _limit, _multiline);
		}

		[[nodiscard]] std::string filter(const std::string_view value, const size_t budget) const
		{
			return input::normalize(value, budget, _multiline);
		}

		// What is left of the limit once the selection about to be replaced is
		// counted back, so replacing a full prompt is not refused for being full.
		[[nodiscard]] size_t remaining_budget() const
		{
			if (!_doc) return 0;

			const auto used = _doc->str().size();
			const auto reclaimed = _doc->has_selection() ? _doc->copy().size() : 0;
			const auto occupied = used > reclaimed ? used - reclaimed : 0;

			return _limit > occupied ? _limit - occupied : 0;
		}

		[[nodiscard]] bool is_empty() const
		{
			return _doc && _doc->size() == 1 && (*_doc)[0].empty();
		}

		[[nodiscard]] int total_rows() const
		{
			if (!_doc) return 1;
			return _word_wrap && _total_visual_rows > 0 ? _total_visual_rows : static_cast<int>(_doc->size());
		}

		[[nodiscard]] int caret_row() const
		{
			const auto pt = _doc->cursor_pos();

			if (_word_wrap && pt.y >= 0 && pt.y < std::ssize(_wrap_line_y))
				return _wrap_line_y[pt.y] + char_to_sub_row(pt.y, pt.x);

			return pt.y;
		}

		[[nodiscard]] bool at_first_row() const { return _doc && caret_row() == 0; }
		[[nodiscard]] bool at_last_row() const { return _doc && caret_row() >= total_rows() - 1; }

		void move_caret_to_end() const
		{
			const auto last = std::max(0, static_cast<int>(_doc->size()) - 1);
			const auto len = static_cast<int>((*_doc)[last].size());
			_doc->select(text_selection(len, last, len, last));
		}

		void remember(const std::string& content)
		{
			std::erase(_history, content);
			_history.push_back(content);

			if (_history.size() > max_history) _history.erase(_history.begin());

			_history_pos = -1;
		}

		// Walks previous prompts, keeping whatever was being typed so it can be
		// restored by walking back off the end.
		bool recall_history(const bool older)
		{
			if (_history.empty()) return false;

			const auto count = static_cast<int>(_history.size());

			if (older)
			{
				if (_history_pos < 0)
				{
					_draft = text();
					_history_pos = count - 1;
				}
				else if (_history_pos > 0)
				{
					--_history_pos;
				}
				else
				{
					return true;
				}
			}
			else
			{
				if (_history_pos < 0) return false;

				if (_history_pos + 1 >= count)
				{
					_history_pos = -1;
					set_text(_draft);
					return true;
				}

				++_history_pos;
			}

			set_text(_history[_history_pos]);
			return true;
		}
	};
}
