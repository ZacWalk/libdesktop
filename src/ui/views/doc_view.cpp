// This file is part of libdesktop, a C++20 platform layer for desktop applications
// Copyright 2026  Zac Walker
//
// This program is free software; you can redistribute it and / or modify it
// under the terms of the LGPL License either version 2.1 or later.
// License details are available at https://www.gnu.org/licenses/lgpl-2.1.html
// This program is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY

// Purpose: Layout, input, selection, editing support and drawing for text-buffer document controls.

#include "libdesktop/ui/views/doc_view.h"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <functional>
#include <limits>
#include <memory>
#include <utility>

namespace
{
	size_t utf8_truncate(const std::string_view text, const int max_codepoints) noexcept
	{
		if (max_codepoints <= 0) return 0;
		int count = 0;
		for (size_t i = 0; i < text.size(); ++i)
		{
			if (!core::is_utf8_continuation(text[i]) && ++count > max_codepoints)
				return i;
		}
		return text.size();
	}

	template <typename Scrollbar>
	void draw_scrollbar(Scrollbar& scrollbar, render::draw_context& draw, const core::recti bounds,
	                    const ui::view_theme& theme)
	{
		if constexpr (requires { scrollbar.draw(draw, bounds, theme); })
			scrollbar.draw(draw, bounds, theme);
		else
			scrollbar.draw(draw, bounds);
	}

	template <typename Scrollbar>
	void set_scrollbar_scale(Scrollbar& scrollbar, const double scale)
	{
		if constexpr (requires { scrollbar.set_dpi_scale(scale); })
			scrollbar.set_dpi_scale(scale);
		else
			scrollbar.set_scale(scale);
	}

	std::shared_ptr<ui::command> view_command(const std::string_view text, std::function<void()> invoke,
	                                           const bool enabled)
	{
		auto result = std::make_shared<ui::command>();
		result->text = text;
		result->invoke = std::move(invoke);
		result->enable = enabled;
		return result;
	}

	template <typename Scrollbar>
	void begin_scrollbar_tracking(Scrollbar& scrollbar, const core::pointi point, const core::recti bounds,
	                             ui::control_host* host)
	{
		if (!scrollbar.hit_test(point, bounds)) return;
		scrollbar._tracking = true;
		scrollbar._tracking_start_mouse = scrollbar._orient == Scrollbar::orientation::vertical ? point.y : point.x;
		scrollbar._tracking_start_pos = scrollbar._position;
		if (host) host->capture_pointer(true);
	}
}

namespace ui
{
	doc_view::doc_view(view_host& host, const view_theme& th, view_context* context, text_clipboard* clipboard)
		: text_view(host, th, context, clipboard),
		  _highlight(syntax::for_language(syntax::language::plain))
	{
	}

	doc_view::doc_view(const view_theme& th, view_context* context, text_clipboard* clipboard)
		: text_view(th, context, clipboard),
		  _highlight(syntax::for_language(syntax::language::plain))
	{
	}

	doc_view::~doc_view()
	{
		stop_caret_blink();
	}

	std::vector<command_ptr> doc_view::on_popup_menu(const core::pointi client_pt)
	{
		return _context ? _context->popup_menu_items(*this, client_pt) : default_popup_menu();
	}

	std::vector<command_ptr> doc_view::default_popup_menu()
	{
		std::vector<command_ptr> items;
		items.push_back(view_command("Cu&t", [this] { (void)cut_text_to_clipboard(); }, can_cut_text()));
		items.push_back(view_command("&Copy", [this] { (void)copy_text_to_clipboard(); }, can_copy_text()));
		items.push_back(view_command("&Paste", [this] { (void)paste_text_from_clipboard(); }, can_paste_text()));
		items.push_back(std::make_shared<command>());
		items.push_back(view_command("Select &All", [this] { select_all_text(); }, _doc && _doc->size() > 0));
		return items;
	}

	text_location doc_view::adjust_for_removal(const text_selection& selection, const text_location& target)
	{
		if (!(selection._end < target)) return target;

		auto result = target;
		if (target.y == selection._end.y)
			result.x = selection._start.x + (target.x - selection._end.x);
		result.y = target.y - (selection._end.y - selection._start.y);
		return result;
	}

	void doc_view::set_buffer(const text_buffer_ptr& document, highlight_fn highlight)
	{
		_scroll_offset = {};
		_doc = document;
		_highlight = highlight ? std::move(highlight) : syntax::for_language(syntax::language::plain);
		reset_parse_cookies();

		_drag_selection = false;
		_drag_text_pending = false;
		_drag_text = false;
		_caret_visible = false;
		_wrap_breaks.clear();
		_wrap_offsets.clear();
		_wrap_line_y.clear();
		_total_visual_rows = 0;
		mark_wrap_dirty_all();
		invalidate();
	}

	void doc_view::mark_wrap_dirty(const int start, const int end)
	{
		const auto lo = std::max(0, std::min(start, end));
		const auto hi = std::max(start, end);
		_wrap_dirty_first = _wrap_dirty_first < 0 ? lo : std::min(_wrap_dirty_first, lo);
		_wrap_dirty_last = std::max(_wrap_dirty_last, hi);
	}

	text_selection doc_view::current_selection() const
	{
		return _doc ? _doc->selection() : text_selection{};
	}

	void doc_view::set_selection(const text_selection& selection)
	{
		if (_doc) _doc->select(selection);
	}

	bool doc_view::has_current_selection() const
	{
		return _doc && _doc->has_selection();
	}

	std::string doc_view::select_text() const
	{
		return has_current_selection() ? _doc->copy() : std::string{};
	}

	void doc_view::select_all()
	{
		if (_doc) set_selection(_doc->all());
	}

	void doc_view::reset_parse_cookies() const
	{
		_parse_cookies.assign(_doc ? _doc->size() : 0, invalid_cookie);
		_cookies_valid_to = static_cast<int>(_parse_cookies.size());
	}

	bool doc_view::cookie_valid(const int line_index) const
	{
		return line_index >= 0 && line_index < _cookies_valid_to && line_index < static_cast<int>(_parse_cookies.size()) &&
			_parse_cookies[line_index] != invalid_cookie;
	}

	void doc_view::discard_cookies_below(const int line_index) const
	{
		const auto begin = std::clamp(_cookies_valid_to, 0, static_cast<int>(_parse_cookies.size()));
		const auto end = std::clamp(line_index, begin, static_cast<int>(_parse_cookies.size()));
		if (end > begin)
			std::fill(_parse_cookies.begin() + begin, _parse_cookies.begin() + end, invalid_cookie);
		_cookies_valid_to = std::max(_cookies_valid_to, line_index);
	}

	void doc_view::set_parse_cookie(const int line_index, const uint32_t cookie) const
	{
		if (line_index < 0 || line_index >= static_cast<int>(_parse_cookies.size())) return;
		discard_cookies_below(line_index);
		_parse_cookies[line_index] = cookie;
		_cookies_valid_to = std::max(_cookies_valid_to, line_index + 1);
	}

	uint32_t doc_view::highlight_cookie(const int line_index) const
	{
		if (!_doc || line_index < 0 || line_index >= static_cast<int>(_doc->size())) return 0;
		if (_parse_cookies.size() != _doc->size()) reset_parse_cookies();

		auto i = line_index;
		const auto scan_limit = std::max(0, i - 1000);
		while (i >= scan_limit && !cookie_valid(i)) --i;
		if (i < scan_limit)
			i = scan_limit;
		else
			++i;

		if (i > line_index) return _parse_cookies[line_index];
		discard_cookies_below(i);

		int block_count = 0;
		while (i <= line_index)
		{
			const auto cookie = i > 0 ? _parse_cookies[i - 1] : 0;
			(*_doc)[i].render(_highlight_buf);
			_parse_cookies[i] = _highlight(cookie, _highlight_buf, {}, block_count);
			assert(_parse_cookies[i] != invalid_cookie);
			++i;
		}

		_cookies_valid_to = std::max(_cookies_valid_to, i);
		return _parse_cookies[line_index];
	}

	uint32_t doc_view::highlight_line(const uint32_t cookie, const std::string_view line_text,
	                                  const std::span<text_block> buf, int& block_count) const
	{
		const auto result = _highlight(cookie, line_text, buf, block_count);

		if (_doc && _doc->spell_check() && !buf.empty())
		{
			const auto len = static_cast<int>(line_text.size());
			auto can_spell_check_style = [](const text_style style)
			{
				switch (style)
				{
				case text_style::normal_text:
				case text_style::md_heading1:
				case text_style::md_heading2:
				case text_style::md_heading3:
				case text_style::md_bold:
				case text_style::md_italic:
				case text_style::md_link_text:
					return true;
				default:
					return false;
				}
			};
			auto append_block = [](std::vector<text_block>& blocks, const int position, const text_style style)
			{
				if (!blocks.empty())
				{
					if (blocks.back()._char_pos == position)
					{
						blocks.back()._color = style;
						return;
					}
					if (blocks.back()._color == style) return;
				}
				if (position == 0 && style == text_style::normal_text) return;
				blocks.push_back({position, style});
			};

			_spell_blocks.clear();
			auto append_segment = [&](const int start, const int end, const text_style base_style)
			{
				if (start >= end) return;
				if (!can_spell_check_style(base_style))
				{
					append_block(_spell_blocks, start, base_style);
					return;
				}

				int position = start;
				while (position < end)
				{
					int next = position + 1;
					auto style = base_style;
					if (spell::is_word_byte(line_text[position]))
					{
						while (next < end && spell::is_word_byte(line_text[next])) ++next;
						if (!spell::check_word(line_text.substr(position, next - position)))
							style = text_style::error_text;
					}
					else
					{
						while (next < end && !spell::is_word_byte(line_text[next])) ++next;
					}
					append_block(_spell_blocks, position, style);
					position = next;
				}
			};

			if (block_count > 0)
			{
				append_segment(0, buf[0]._char_pos, text_style::normal_text);
				for (int i = 0; i < block_count; ++i)
				{
					const auto start = buf[i]._char_pos;
					const auto end = i + 1 < block_count ? buf[i + 1]._char_pos : len;
					append_segment(start, end, buf[i]._color);
				}
			}
			else
				append_segment(0, len, text_style::normal_text);

			block_count = std::min(static_cast<int>(_spell_blocks.size()), static_cast<int>(buf.size()));
			for (int i = 0; i < block_count; ++i) buf[i] = _spell_blocks[i];
		}

		return result;
	}

	void doc_view::paint(render::draw_context& draw, const bool focused) const
	{
		(void)focused;
		draw_view(draw);
	}

	void doc_view::on_focus(const bool focused, const bool select_all_on_focus)
	{
		text_view::on_focus(focused, select_all_on_focus);
		_focused = focused;
		if (focused)
		{
			if (select_all_on_focus) select_all();
			start_caret_blink();
		}
		else
		{
			stop_caret_blink();
			cancel_text_drag();
			if (_drag_selection)
			{
				_drag_selection = false;
				_drag_sel_timer = 0;
			}
		}
		invalidate_selection();
		update_caret();
		invalidate();
	}

	bool doc_view::on_text(const std::string_view)
	{
		return false;
	}

	bool doc_view::on_key(const int key, const platform::key_state keys)
	{
		if (!_doc) return text_view::on_key(key, keys);
		const bool ctrl = keys.control;
		const bool shift = keys.shift;
		auto handled = [this] { reset_caret_blink(); return true; };
		const auto k = static_cast<char32_t>(key);

		if (k == platform::keys::ESCAPE && (_drag_text || _drag_text_pending))
		{
			cancel_text_drag();
			return handled();
		}

		if (k == platform::keys::LEFT && !ctrl && !shift) { _doc->move_char_left(false); return handled(); }
		if (k == platform::keys::LEFT && !ctrl && shift) { _doc->move_char_left(true); return handled(); }
		if (k == platform::keys::RIGHT && !ctrl && !shift) { _doc->move_char_right(false); return handled(); }
		if (k == platform::keys::RIGHT && !ctrl && shift) { _doc->move_char_right(true); return handled(); }
		if (k == platform::keys::LEFT && ctrl && !shift) { _doc->move_word_left(false); return handled(); }
		if (k == platform::keys::LEFT && ctrl && shift) { _doc->move_word_left(true); return handled(); }
		if (k == platform::keys::RIGHT && ctrl && !shift) { _doc->move_word_right(false); return handled(); }
		if (k == platform::keys::RIGHT && ctrl && shift) { _doc->move_word_right(true); return handled(); }
		if (k == platform::keys::UP && !ctrl && !shift) { _doc->move_lines(-1, false); return handled(); }
		if (k == platform::keys::UP && !ctrl && shift) { _doc->move_lines(-1, true); return handled(); }
		if (k == platform::keys::DOWN && !ctrl && !shift) { _doc->move_lines(1, false); return handled(); }
		if (k == platform::keys::DOWN && !ctrl && shift) { _doc->move_lines(1, true); return handled(); }
		if (k == platform::keys::UP && ctrl) { wrap_scroll_by(-1); return handled(); }
		if (k == platform::keys::DOWN && ctrl) { wrap_scroll_by(1); return handled(); }
		if (k == platform::keys::PRIOR && !shift) { _doc->move_lines(-_screen_lines, false); return handled(); }
		if (k == platform::keys::PRIOR && shift) { _doc->move_lines(-_screen_lines, true); return handled(); }
		if (k == platform::keys::NEXT && !shift) { _doc->move_lines(_screen_lines, false); return handled(); }
		if (k == platform::keys::NEXT && shift) { _doc->move_lines(_screen_lines, true); return handled(); }
		if (k == platform::keys::END && !ctrl && !shift) { _doc->move_line_end(false); return handled(); }
		if (k == platform::keys::END && !ctrl && shift) { _doc->move_line_end(true); return handled(); }
		if (k == platform::keys::HOME && !ctrl && !shift) { _doc->move_line_home(false); return handled(); }
		if (k == platform::keys::HOME && !ctrl && shift) { _doc->move_line_home(true); return handled(); }
		if (k == platform::keys::HOME && ctrl && !shift) { _doc->move_doc_home(false); return handled(); }
		if (k == platform::keys::HOME && ctrl && shift) { _doc->move_doc_home(true); return handled(); }
		if (k == platform::keys::END && ctrl && !shift) { _doc->move_doc_end(false); return handled(); }
		if (k == platform::keys::END && ctrl && shift) { _doc->move_doc_end(true); return handled(); }

		return text_view::on_key(key, keys);
	}

	void doc_view::on_mouse_down(const core::pointi point, const platform::key_state keys)
	{
		take_focus(false);
		on_left_button_down(point, keys);
	}

	void doc_view::on_mouse_move(const core::pointi point, const bool)
	{
		_drag_point = point;
		if (!_doc) return;

		if (_vscroll._tracking)
		{
			set_scroll_pixel(_vscroll.track_to(point, scrollbar_rect()));
			update_caret();
			return;
		}
		if (_hscroll._tracking)
		{
			scroll_to_char(std::clamp(_hscroll.track_to(point, client_rect()), 0,
			                         std::max(0, _doc->max_line_length() - 1)));
			update_caret();
			return;
		}

		if (_drag_text_pending)
		{
			const auto slop = std::max(4, _font_extent.cx / 2);
			if (std::abs(point.x - _drag_text_origin.x) > slop || std::abs(point.y - _drag_text_origin.y) > slop)
			{
				_drag_text_pending = false;
				_drag_text = true;
			}
		}
		if (_drag_text)
		{
			_drop_location = client_to_text(point);
			if (auto* const h = host()) h->set_cursor(platform::cursor::move);
			invalidate();
			return;
		}
		if (_drag_selection)
		{
			const bool on_margin = point.x < margin_width();
			const auto position = client_to_text(point);
			if (_line_selection)
			{
				if (on_margin)
				{
					_doc->select(_doc->line_selection(position, true));
					return;
				}
				_line_selection = _word_selection = false;
			}
			_doc->select(_word_selection ? _doc->word_selection(position, true) : _doc->pos_selection(position, true));
		}

		if (!_drag_selection)
		{
			bool redraw = _vscroll.set_hover(_vscroll.hit_test(point, scrollbar_rect()));
			redraw |= _hscroll.set_hover(_hscroll.hit_test(point, scrollbar_rect()));
			if (redraw) invalidate();
		}
		update_cursor(point);
	}

	void doc_view::on_mouse_up(const core::pointi point, const platform::key_state keys)
	{
		on_left_button_up(point, keys);
	}

	void doc_view::on_double_click(const core::pointi point, const platform::key_state keys)
	{
		on_left_button_double_click(point, keys);
	}

	void doc_view::on_mouse_leave()
	{
		_vscroll._hover_tracking = false;
		bool redraw = _vscroll.set_hover(false);
		redraw |= _hscroll.set_hover(false);
		if (redraw) invalidate();
	}

	bool doc_view::on_wheel(const core::pointi, const int delta, const platform::key_state keys)
	{
		if (keys.control)
		{
			if (_context) _context->on_zoom(delta > 0 ? 2 : -2);
			return true;
		}
		if (!can_scroll()) return false;
		if (_word_wrap && !_wrap_line_y.empty())
			set_scroll_pixel(_scroll_offset.y - delta * _font_extent.cy);
		else
			set_scroll_pixel(_scroll_offset.y - delta * _font_extent.cy);
		update_caret();
		return true;
	}

	std::vector<command_ptr> doc_view::context_commands(const core::pointi point)
	{
		on_right_button_down(point, {});
		return on_popup_menu(point);
	}

	platform::cursor doc_view::cursor_at(const core::pointi point) const
	{
		if (!_doc) return platform::cursor::normal;
		const auto scrollbar = scrollbar_rect();
		if (_vscroll.hit_test(point, scrollbar) || _hscroll.hit_test(point, scrollbar)) return platform::cursor::normal;
		if (point.x < margin_width() || _doc->is_inside_selection(client_to_text(point)) ||
			!allows_drag_selection() || _drag_text)
			return platform::cursor::normal;
		return platform::cursor::text_select;
	}

	core::recti doc_view::caret_bounds() const
	{
		if (!has_caret() || !_doc) return {};
		const auto position = _doc->cursor_pos();
		if (_doc->calc_offset(position.y, position.x) < scroll_char()) return {};
		const auto point = text_to_client(position);
		return {point.x, point.y, point.x + 2, point.y + _font_extent.cy};
	}

	void doc_view::blink()
	{
		if (!has_caret() || !_focused) return;
		_caret_visible = !_caret_visible;
		invalidate(caret_bounds());
	}

	void doc_view::tick()
	{
		if (_drag_selection && _doc)
			on_timer_drag_selection();
	}

	void doc_view::scale_changed()
	{
		text_view::scale_changed();
		mark_wrap_dirty_all();
		layout();
		invalidate();
	}

	void doc_view::on_placed(render::measure_context& measure)
	{
		text_view::on_placed(measure);
		_font_extent.cx = std::max(1, _font_extent.cx);
		_font_extent.cy = std::max(1, _font_extent.cy);
		_screen_chars = _font_extent.cx > 0 ? _view_extent.cx / _font_extent.cx : 0;
		set_scrollbar_scale(_hscroll, measure.scale_factor);
		set_scrollbar_scale(_vscroll, measure.scale_factor);
		mark_wrap_dirty_all();
		layout();
		recalc_vert_scrollbar();
		recalc_horz_scrollbar();
	}
	void doc_view::set_word_wrap(const bool enabled)
	{
		if (_word_wrap == enabled) return;
		_word_wrap = enabled;
		_scroll_offset.x = 0;
		mark_wrap_dirty_all();
		layout();
		recalc_vert_scrollbar();
		recalc_horz_scrollbar();
		_host.invalidate_layout();
		invalidate();
	}

	void doc_view::toggle_word_wrap()
	{
		set_word_wrap(!_word_wrap);
	}

	int doc_view::wrap_width() const
	{
		if (_view_extent.cx <= 0 || _font_extent.cx <= 0) return 80;
		return std::max(1, (_view_extent.cx - text_left() - right_content_padding()) / _font_extent.cx);
	}

	void doc_view::calc_line_wrap_into(const text_line& line, std::vector<int>& breaks)
	{
		breaks.clear();
		const auto width = wrap_width();
		if (width <= 0 || !_doc) return;

		const auto tab_size = _doc->tab_size();
		line.render(_render_buf);
		auto char_advance = [&](const int i, const int column)
		{
			if (_render_buf[i] == '\t') return tab_size - column % tab_size;
			if (core::is_utf8_continuation(_render_buf[i])) return 0;
			return 1;
		};

		const auto text = std::string_view(_render_buf);
		const auto length = static_cast<int>(text.size());
		int column = 0;
		int last_break = -1;
		int column_at_break = 0;
		for (int i = 0; i < length; ++i)
		{
			const auto advance = char_advance(i, column);
			if (text[i] == ' ' || text[i] == '\t')
			{
				last_break = i + 1;
				column_at_break = column + advance;
			}
			if (column > 0 && column + advance > width)
			{
				if (last_break > (breaks.empty() ? 0 : breaks.back()))
				{
					breaks.push_back(last_break);
					column = column - column_at_break + advance;
				}
				else
				{
					breaks.push_back(i);
					column = advance;
				}
				last_break = -1;
				column_at_break = 0;
			}
			else
				column += advance;
		}
	}

	std::span<const int> doc_view::line_breaks(const int line_index) const
	{
		if (_wrap_offsets.empty() || line_index < 0 || line_index >= static_cast<int>(_wrap_offsets.size()) - 1)
			return {};
		return {_wrap_breaks.data() + _wrap_offsets[line_index], _wrap_breaks.data() + _wrap_offsets[line_index + 1]};
	}

	int doc_view::line_break_count(const int line_index) const
	{
		if (_wrap_offsets.empty() || line_index < 0 || line_index >= static_cast<int>(_wrap_offsets.size()) - 1)
			return 0;
		return _wrap_offsets[line_index + 1] - _wrap_offsets[line_index];
	}

	int doc_view::char_to_sub_row(const int line_index, const int char_index) const
	{
		const auto breaks = line_breaks(line_index);
		if (!_word_wrap || breaks.empty()) return 0;
		for (int i = 0; i < static_cast<int>(breaks.size()); ++i)
			if (char_index < breaks[i]) return i;
		return static_cast<int>(breaks.size());
	}

	int doc_view::visual_row_to_line_index(const int visual_row) const
	{
		if (!_word_wrap || _wrap_line_y.empty() || !_doc) return visual_row;
		const auto line_count = std::min(static_cast<int>(_doc->size()), static_cast<int>(_wrap_line_y.size()));
		if (line_count <= 0) return 0;
		int lo = 0;
		int hi = line_count - 1;
		while (lo < hi)
		{
			const auto mid = (lo + hi + 1) / 2;
			if (_wrap_line_y[mid] <= visual_row)
				lo = mid;
			else
				hi = mid - 1;
		}
		return lo;
	}

	int doc_view::line_visual_rows(const int line_index) const
	{
		if (!_word_wrap || _wrap_offsets.empty() || line_index < 0 || line_index >= static_cast<int>(_wrap_offsets.size()) - 1)
			return 1;
		return line_break_count(line_index) + 1;
	}

	void doc_view::ensure_visible(const text_location& point)
	{
		if (!_doc || _font_extent.cy <= 0) return;

		if (_word_wrap && !_wrap_offsets.empty() && point.y >= 0 && point.y < static_cast<int>(_wrap_line_y.size()))
		{
			const auto sub_row = char_to_sub_row(point.y, point.x);
			const auto cursor_vrow = _wrap_line_y[point.y] + sub_row;
			const auto cursor_top = line_offset_vrow(cursor_vrow);
			const auto cursor_bottom = cursor_top + _font_extent.cy;
			const auto visible_height = std::max(0, _view_extent.cy - text_top());
			if (cursor_bottom > _scroll_offset.y + visible_height)
				set_scroll_pixel(cursor_bottom - visible_height);
			else if (cursor_top < _scroll_offset.y)
				set_scroll_pixel(cursor_top);
		}
		else
		{
			const auto cursor_top = line_offset(point.y);
			const auto cursor_bottom = cursor_top + _font_extent.cy;
			const auto visible_height = std::max(0, _view_extent.cy - text_top());
			if (cursor_bottom > _scroll_offset.y + visible_height)
				set_scroll_pixel(cursor_bottom - visible_height);
			else if (cursor_top < _scroll_offset.y)
				set_scroll_pixel(cursor_top);
		}

		if (!_word_wrap && point.y >= 0 && point.y < static_cast<int>(_doc->size()))
		{
			const auto actual_pos = _doc->calc_offset(point.y, point.x);
			int new_offset = scroll_char();
			const auto visible = std::max(1, wrap_width() - 1);
			if (actual_pos > new_offset + visible) new_offset = actual_pos - visible;
			if (actual_pos < new_offset) new_offset = actual_pos;
			if (new_offset >= _doc->max_line_length()) new_offset = _doc->max_line_length() - 1;
			new_offset = std::max(0, new_offset);
			if (scroll_char() != new_offset) scroll_to_char(new_offset);
		}
		update_caret();
	}

	void doc_view::recalc_vert_scrollbar()
	{
		if (!_doc)
		{
			_content_extent = {};
			_vscroll.update(0, 0, 0);
			return;
		}

		if (_word_wrap && _total_visual_rows > 0)
			_content_extent.cy = top_content_padding() + _total_visual_rows * _font_extent.cy + bottom_content_padding();
		else
			_content_extent.cy = top_content_padding() + static_cast<int>(_doc->size()) * _font_extent.cy + bottom_content_padding();
		_content_extent.cx = _view_extent.cx;

		const auto max_y = max_scroll_pixel();
		if (_scroll_offset.y > max_y)
		{
			_scroll_offset.y = max_y;
			_host.invalidate_status();
			_host.invalidate_caret();
		}
		const auto visible_height = std::max(0, _view_extent.cy - text_top());
		_vscroll.update(_content_extent.cy, visible_height, _scroll_offset.y);
	}

	void doc_view::recalc_horz_scrollbar()
	{
		if (!_doc || _word_wrap)
		{
			_scroll_offset.x = 0;
			_hscroll.update(0, 0, 0);
			return;
		}
		if (_screen_chars >= _doc->max_line_length() && _scroll_offset.x > 0)
		{
			_scroll_offset.x = 0;
			_host.invalidate_status();
			_host.invalidate_caret();
		}
		const auto margin_chars = _sel_margin ? 4 : 1;
		_hscroll.update(_doc->max_line_length() + margin_chars, _screen_chars, scroll_char());
	}

	void doc_view::invalidate_lines(int start, int end)
	{
		if (!_doc || start < 0 || start >= static_cast<int>(_doc->size()))
		{
			invalidate();
			return;
		}
		const auto top = _scroll_offset.y - text_top();
		core::recti dirty = client_rect();
		if (end < 0)
			dirty.top = line_offset(start) - top;
		else
		{
			if (end < start) std::swap(start, end);
			end = std::min(end, static_cast<int>(_doc->size()) - 1);
			dirty.top = line_offset(start) - top;
			dirty.bottom = line_offset(end) + line_height(end) - top;
		}
		invalidate(dirty);
	}

	void doc_view::lines_changed(const int start, const int end)
	{
		mark_wrap_dirty(start, end);
		if (start >= 0) _cookies_valid_to = std::min(_cookies_valid_to, start);
		if (_word_wrap)
		{
			_host.invalidate_layout();
			invalidate_lines(start, -1);
		}
		else
			invalidate_lines(start, end);
	}

	void doc_view::line_count_changed(const int at, const int delta)
	{
		if (_wrap_dirty_last > at) mark_wrap_dirty_all();
		splice_parse_cookies(at, delta);
		splice_wrap(at, delta);
		mark_wrap_dirty(at, at + std::max(0, delta));
		invalidate_lines(at, -1);
		_host.invalidate_layout();
	}

	void doc_view::layout()
	{
		if (!_doc || !_word_wrap)
		{
			_wrap_breaks.clear();
			_wrap_offsets.clear();
			_wrap_line_y.clear();
			_total_visual_rows = _doc ? static_cast<int>(_doc->size()) : 0;
			clear_wrap_dirty();
			return;
		}

		const auto line_count = static_cast<int>(_doc->size());
		if (_wrap_dirty_all || static_cast<int>(_wrap_offsets.size()) != line_count + 1)
			full_layout(line_count);
		else if (_wrap_dirty_first >= 0)
		{
			const auto last = std::min(_wrap_dirty_last, line_count - 1);
			for (int i = _wrap_dirty_first; i <= last; ++i)
				relayout_line(i);
		}
		clear_wrap_dirty();
	}

	void doc_view::clear_wrap_dirty()
	{
		_wrap_dirty_all = false;
		_wrap_dirty_first = -1;
		_wrap_dirty_last = -1;
	}

	void doc_view::splice_parse_cookies(const int at, const int delta) const
	{
		const auto count = static_cast<int>(_parse_cookies.size());
		const auto before_edit = static_cast<int>(_doc ? _doc->size() : 0) - delta;
		if (count != before_edit || at < 0 || at >= count)
		{
			reset_parse_cookies();
			return;
		}
		if (delta > 0)
			_parse_cookies.insert(_parse_cookies.begin() + at + 1, delta, invalid_cookie);
		else if (delta < 0)
			_parse_cookies.erase(_parse_cookies.begin() + at + 1, _parse_cookies.begin() + at + 1 - delta);
		_cookies_valid_to = std::min(_cookies_valid_to, at);
	}

	void doc_view::splice_wrap(const int at, const int delta)
	{
		const auto line_count = static_cast<int>(_wrap_offsets.size()) - 1;
		const auto before_edit = static_cast<int>(_doc ? _doc->size() : 0) - delta;
		if (!_word_wrap || line_count <= 0 || line_count != before_edit || at < 0 || at >= line_count ||
			static_cast<int>(_wrap_line_y.size()) != line_count + 1 || (delta < 0 && at + 1 - delta > line_count))
		{
			mark_wrap_dirty_all();
			return;
		}

		if (delta > 0)
		{
			const auto first_row = _wrap_line_y[at + 1];
			for (size_t i = at + 1; i < _wrap_line_y.size(); ++i) _wrap_line_y[i] += delta;
			_wrap_line_y.insert(_wrap_line_y.begin() + at + 1, delta, 0);
			for (int k = 0; k < delta; ++k) _wrap_line_y[at + 1 + k] = first_row + k;
			const auto first_break = _wrap_offsets[at + 1];
			_wrap_offsets.insert(_wrap_offsets.begin() + at + 1, delta, first_break);
		}
		else if (delta < 0)
		{
			const auto removed = -delta;
			const auto first_break = _wrap_offsets[at + 1];
			const auto last_break = _wrap_offsets[at + 1 + removed];
			const auto rows = _wrap_line_y[at + 1 + removed] - _wrap_line_y[at + 1];
			_wrap_breaks.erase(_wrap_breaks.begin() + first_break, _wrap_breaks.begin() + last_break);
			_wrap_offsets.erase(_wrap_offsets.begin() + at + 1, _wrap_offsets.begin() + at + 1 + removed);
			for (size_t i = at + 1; i < _wrap_offsets.size(); ++i) _wrap_offsets[i] -= last_break - first_break;
			_wrap_line_y.erase(_wrap_line_y.begin() + at + 1, _wrap_line_y.begin() + at + 1 + removed);
			for (size_t i = at + 1; i < _wrap_line_y.size(); ++i) _wrap_line_y[i] -= rows;
		}
		_total_visual_rows = _wrap_line_y.empty() ? 0 : _wrap_line_y.back();
	}

	void doc_view::full_layout(const int line_count)
	{
		_wrap_breaks.clear();
		_wrap_offsets.resize(line_count + 1);
		_wrap_line_y.resize(line_count + 1);
		_wrap_line_y[0] = 0;
		for (int i = 0; i < line_count; ++i)
		{
			_wrap_offsets[i] = static_cast<int>(_wrap_breaks.size());
			calc_line_wrap_into((*_doc)[i], _line_breaks_buf);
			_wrap_breaks.insert(_wrap_breaks.end(), _line_breaks_buf.begin(), _line_breaks_buf.end());
			_wrap_line_y[i + 1] = _wrap_line_y[i] + static_cast<int>(_line_breaks_buf.size()) + 1;
		}
		_wrap_offsets[line_count] = static_cast<int>(_wrap_breaks.size());
		_total_visual_rows = _wrap_line_y[line_count];
	}

	void doc_view::relayout_line(const int line_index)
	{
		if (!_doc || line_index < 0 || line_index >= static_cast<int>(_doc->size()) || _wrap_offsets.size() < 2) return;
		calc_line_wrap_into((*_doc)[line_index], _line_breaks_buf);
		const auto begin = _wrap_offsets[line_index];
		const auto end = _wrap_offsets[line_index + 1];
		const auto old_count = end - begin;
		const auto new_count = static_cast<int>(_line_breaks_buf.size());
		if (new_count == old_count)
		{
			std::copy(_line_breaks_buf.begin(), _line_breaks_buf.end(), _wrap_breaks.begin() + begin);
			return;
		}
		_wrap_breaks.erase(_wrap_breaks.begin() + begin, _wrap_breaks.begin() + end);
		_wrap_breaks.insert(_wrap_breaks.begin() + begin, _line_breaks_buf.begin(), _line_breaks_buf.end());
		const auto delta = new_count - old_count;
		for (size_t i = line_index + 1; i < _wrap_offsets.size(); ++i)
		{
			_wrap_offsets[i] += delta;
			_wrap_line_y[i] += delta;
		}
		_total_visual_rows = _wrap_line_y.back();
	}
	int doc_view::client_to_line(const core::pointi& point) const
	{
		const auto line_count = _doc ? static_cast<int>(_doc->size()) : 0;
		if (_font_extent.cy <= 0 || line_count == 0) return 0;

		const auto content_y = point.y - text_top() + _scroll_offset.y;
		if (_word_wrap && !_wrap_line_y.empty())
		{
			const auto visual_row = (content_y - top_content_padding()) / _font_extent.cy;
			return std::clamp(visual_row_to_line_index(std::max(0, visual_row)), 0, line_count - 1);
		}

		const auto line = (content_y - top_content_padding()) / _font_extent.cy;
		return std::clamp(line, 0, line_count - 1);
	}

	text_location doc_view::client_to_text(const core::pointi& point) const
	{
		const auto line_count = _doc ? static_cast<int>(_doc->size()) : 0;
		text_location result{0, client_to_line(point)};
		if (!_doc || _font_extent.cx <= 0 || _font_extent.cy <= 0 || result.y < 0 || result.y >= line_count)
			return result;

		const auto tab_size = _doc->tab_size();
		const auto& line = (*_doc)[result.y];
		line.render(_render_buf);
		const auto length = static_cast<int>(_render_buf.size());

		if (_word_wrap && !_wrap_offsets.empty() && result.y < static_cast<int>(_wrap_offsets.size()) - 1 &&
			result.y < static_cast<int>(_wrap_line_y.size()))
		{
			const auto line_pixel_y = line_offset(result.y);
			const auto pixel_y = point.y - text_top() + _scroll_offset.y;
			const auto breaks = line_breaks(result.y);
			const auto visual_rows = static_cast<int>(breaks.size()) + 1;
			const auto sub_row = std::clamp((pixel_y - line_pixel_y) / _font_extent.cy, 0, visual_rows - 1);
			const auto row_start = sub_row == 0 ? 0 : breaks[sub_row - 1];
			const auto row_end = sub_row < static_cast<int>(breaks.size()) ? breaks[sub_row] : length;
			const auto wanted = std::max(0, (point.x - text_left()) / _font_extent.cx);

			int absolute_column = _doc->calc_offset(result.y, row_start);
			int relative_column = 0;
			int index = row_start;
			while (index < row_end)
			{
				int advance = 0;
				if (_render_buf[index] == '\t')
					advance = tab_size - absolute_column % tab_size;
				else if (!core::is_utf8_continuation(_render_buf[index]))
					advance = 1;
				absolute_column += advance;
				relative_column += advance;
				if (relative_column > wanted) break;
				++index;
			}
			result.x = std::clamp(index, row_start, row_end);
		}
		else
		{
			const auto wanted = std::max(0, scroll_char() + (point.x - text_left()) / _font_extent.cx);
			int column = 0;
			int index = 0;
			while (index < length)
			{
				if (_render_buf[index] == '\t')
					column += tab_size - column % tab_size;
				else if (!core::is_utf8_continuation(_render_buf[index]))
					++column;
				if (column > wanted) break;
				++index;
			}
			result.x = std::clamp(index, 0, length);
		}
		return result;
	}

	core::pointi doc_view::text_to_client(const text_location& point) const
	{
		core::pointi result{};
		if (!_doc || point.y < 0 || point.y >= static_cast<int>(_doc->size())) return result;

		if (_word_wrap && !_wrap_offsets.empty() && point.y < static_cast<int>(_wrap_offsets.size()) - 1 &&
			point.y < static_cast<int>(_wrap_line_y.size()))
		{
			const auto breaks = line_breaks(point.y);
			const auto sub_row = char_to_sub_row(point.y, point.x);
			const auto row_start = sub_row == 0 ? 0 : breaks[sub_row - 1];
			result.y = line_offset_vrow(_wrap_line_y[point.y] + sub_row) - _scroll_offset.y + text_top();

			const auto tab_size = _doc->tab_size();
			(*_doc)[point.y].render(_render_buf);
			int absolute_column = _doc->calc_offset(point.y, row_start);
			int relative_column = 0;
			for (int index = row_start; index < point.x && index < static_cast<int>(_render_buf.size()); ++index)
			{
				int advance = 0;
				if (_render_buf[index] == '\t')
					advance = tab_size - absolute_column % tab_size;
				else if (!core::is_utf8_continuation(_render_buf[index]))
					advance = 1;
				absolute_column += advance;
				relative_column += advance;
			}
			result.x = text_left() + relative_column * _font_extent.cx;
		}
		else
		{
			result.y = line_offset(point.y) - _scroll_offset.y + text_top();
			(*_doc)[point.y].render(_render_buf);
			const auto tab_size = _doc->tab_size();
			int column = 0;
			for (int index = 0; index < point.x && index < static_cast<int>(_render_buf.size()); ++index)
			{
				if (_render_buf[index] == '\t')
					column += tab_size - column % tab_size;
				else if (!core::is_utf8_continuation(_render_buf[index]))
					++column;
			}
			result.x = text_left() + (column - scroll_char()) * _font_extent.cx;
		}
		return result;
	}
	int doc_view::top_content_padding() const
	{
		return _font_extent.cy > 0 ? std::max(1, _font_extent.cy / 2) : 0;
	}

	int doc_view::bottom_content_padding() const
	{
		return _font_extent.cy > 0 ? _font_extent.cy : 0;
	}

	int doc_view::right_content_padding() const
	{
		return 0;
	}

	int doc_view::line_offset(const int line_index) const
	{
		if (_word_wrap && !_wrap_line_y.empty())
		{
			const auto index = std::clamp(line_index, 0, static_cast<int>(_wrap_line_y.size()) - 1);
			return top_content_padding() + _wrap_line_y[index] * _font_extent.cy;
		}
		return top_content_padding() + line_index * _font_extent.cy;
	}

	int doc_view::line_offset_vrow(const int visual_row) const
	{
		return top_content_padding() + visual_row * _font_extent.cy;
	}

	int doc_view::line_height(int line_index) const
	{
		if (_word_wrap && _wrap_offsets.size() > 1)
		{
			line_index = std::clamp(line_index, 0, static_cast<int>(_wrap_offsets.size()) - 2);
			return (line_break_count(line_index) + 1) * _font_extent.cy;
		}
		return _font_extent.cy;
	}

	bool doc_view::line_fully_selected(const int line_index, const int length) const
	{
		if (!_doc || !_doc->has_selection()) return false;
		const auto selection = _doc->selection().normalize();
		const auto starts_before = selection._start.y < line_index ||
			(selection._start.y == line_index && selection._start.x <= 0);
		const auto ends_after = selection._end.y > line_index ||
			(selection._end.y == line_index && selection._end.x >= length);
		return starts_before && ends_after;
	}

	int doc_view::margin_width() const
	{
		if (!_sel_margin) return 1;
		const auto line_count = _doc ? static_cast<int>(_doc->size()) : 0;
		int digits = 1;
		for (auto n = line_count; n >= 10; n /= 10) ++digits;
		return (digits + 1) * _font_extent.cx + 4;
	}

	int doc_view::text_left() const
	{
		return margin_width() + _font_extent.cx;
	}

	core::color32 doc_view::line_bg_color(const int line_index) const
	{
		if (_focused && _doc && line_index == _doc->cursor_pos().y) return document_focus_band_color();
		return _theme.style_color(text_style::normal_bkgnd);
	}

	void doc_view::draw_line(render::draw_context& draw, core::pointi& origin, const core::recti& clip,
	                         const std::string_view chars, const int offset, const int count,
	                         const render::font_face font, const core::color32 text_color,
	                         const core::color32 bg_color) const
	{
		if (count <= 0 || !_doc) return;
		_doc->expanded_chars(chars, offset, count, _expand_buf);
		const auto width = clip.right - origin.x;
		const auto codepoints = core::utf8_codepoint_count(_expand_buf);
		if (width > 0 && _font_extent.cx > 0)
		{
			const auto count_fit = width / _font_extent.cx + 1;
			auto draw_bytes = static_cast<int>(_expand_buf.size());
			auto draw_codepoints = codepoints;
			if (codepoints > count_fit)
			{
				draw_bytes = static_cast<int>(utf8_truncate(_expand_buf, count_fit));
				draw_codepoints = count_fit;
			}

			auto block_clip = clip;
			block_clip.left = std::max(origin.x, clip.left);
			block_clip.right = std::min(clip.right, origin.x + draw_codepoints * _font_extent.cx);
			block_clip.bottom = origin.y + _font_extent.cy;
			if (block_clip.right > block_clip.left)
			{
				draw.draw_rect(block_clip, core::color(bg_color));
				draw.draw_text(std::string_view(_expand_buf.data(), static_cast<size_t>(draw_bytes)), block_clip,
				               font, render::text_style::single_line, core::color(text_color), {});
			}
		}
		origin.x += _font_extent.cx * codepoints;
	}

	void doc_view::draw_line(render::draw_context& draw, core::pointi& origin, const core::recti& clip,
	                         const text_style, const std::string_view chars, const int offset, const int count,
	                         const text_location& text_pos, const render::font_face font,
	                         const core::color32 text_color, const core::color32 bg_color) const
	{
		if (count <= 0 || !_doc) return;
		const auto selection = _doc->selection();
		int selection_begin = 0;
		int selection_end = 0;
		if (selection._start.y > text_pos.y)
			selection_begin = count;
		else if (selection._start.y == text_pos.y)
			selection_begin = std::clamp(selection._start.x - text_pos.x, 0, count);
		if (selection._end.y > text_pos.y)
			selection_end = count;
		else if (selection._end.y == text_pos.y)
			selection_end = std::clamp(selection._end.x - text_pos.x, 0, count);

		if (selection_begin > 0)
			draw_line(draw, origin, clip, chars, offset, selection_begin, font, text_color, bg_color);
		if (selection_begin < selection_end)
			draw_line(draw, origin, clip, chars, offset + selection_begin, selection_end - selection_begin, font,
			          _theme.style_color(text_style::sel_text), _theme.style_color(text_style::sel_bkgnd));
		if (selection_end < count)
			draw_line(draw, origin, clip, chars, offset + selection_end, count - selection_end, font, text_color, bg_color);
	}

	void doc_view::draw_wrapped_line(render::draw_context& draw, const core::recti& rect, const int line_index,
	                                 const render::font_face font) const
	{
		if (!_doc) return;
		const auto bg_color = line_bg_color(line_index);
		const auto& line = (*_doc)[line_index];
		const auto breaks = line_breaks(line_index);
		const auto cookie = highlight_cookie(line_index - 1);
		line.render(_render_buf);
		const auto length = static_cast<int>(_render_buf.size());
		const auto needed = syntax::blocks_for_line(_render_buf.size());
		if (_block_buf.size() < needed) _block_buf.resize(needed);
		const std::span buffer{_block_buf};
		int block_count = 0;
		set_parse_cookie(line_index, highlight_line(cookie, _render_buf, buffer, block_count));

		const auto rows = static_cast<int>(breaks.size()) + 1;
		for (int row = 0; row < rows; ++row)
		{
			const auto row_start = row == 0 ? 0 : breaks[row - 1];
			const auto row_end = row < static_cast<int>(breaks.size()) ? breaks[row] : length;
			const auto row_y = rect.top + row * _font_extent.cy;
			if (row_y >= rect.bottom) break;
			const core::recti row_rect(rect.left, row_y, rect.right, row_y + _font_extent.cy);
			core::pointi origin(row_rect.left, row_y);
			auto draw_clipped = [&](const int start, const int end, const text_style style)
			{
				const auto first = std::max(start, row_start);
				const auto last = std::min(end, row_end);
				if (first < last)
					draw_line(draw, origin, row_rect, style, _render_buf, first, last - first,
					          {first, line_index}, font, _theme.style_color(style), bg_color);
			};

			if (block_count > 0)
			{
				draw_clipped(0, buffer[0]._char_pos, text_style::normal_text);
				for (int i = 0; i < block_count; ++i)
				{
					const auto end = i + 1 < block_count ? buffer[i + 1]._char_pos : length;
					draw_clipped(buffer[i]._char_pos, end, buffer[i]._color);
				}
			}
			else
				draw_clipped(0, length, text_style::normal_text);

			if (row == rows - 1 && _doc->is_inside_selection({length, line_index}))
			{
				const auto selection_x = std::max(origin.x, row_rect.left);
				draw.draw_rect({selection_x, row_rect.top, selection_x + _font_extent.cx, row_rect.bottom},
				               core::color(_theme.style_color(text_style::sel_bkgnd)));
			}
		}
	}

	void doc_view::draw_line(render::draw_context& draw, const core::recti& rect, const int line_index,
	                         const render::font_face font) const
	{
		if (!_doc) return;
		const auto bg_color = line_bg_color(line_index);
		const auto& line = (*_doc)[line_index];
		if (_word_wrap && !_wrap_offsets.empty() && line_index < static_cast<int>(_wrap_offsets.size()) - 1 &&
			line_break_count(line_index) > 0)
		{
			draw_wrapped_line(draw, rect, line_index, font);
			return;
		}
		if (line.empty())
		{
			if (_doc->is_inside_selection({0, line_index}))
				draw.draw_rect({rect.left, rect.top, rect.left + _font_extent.cx, rect.bottom},
				               core::color(_theme.style_color(text_style::sel_bkgnd)));
			return;
		}

		const auto length = static_cast<int>(line.size());
		line.render(_render_buf);
		const auto needed = syntax::blocks_for_line(line.size());
		if (_block_buf.size() < needed) _block_buf.resize(needed);
		const std::span buffer{_block_buf};
		int block_count = 0;
		const auto cookie = highlight_cookie(line_index - 1);
		set_parse_cookie(line_index, highlight_line(cookie, _render_buf, buffer, block_count));
		core::pointi origin(rect.left - scroll_char() * _font_extent.cx, rect.top);

		if (block_count > 0)
		{
			draw_line(draw, origin, rect, text_style::normal_text, _render_buf, 0, buffer[0]._char_pos,
			          {0, line_index}, font, _theme.style_color(text_style::normal_text), bg_color);
			for (int i = 0; i + 1 < block_count; ++i)
				draw_line(draw, origin, rect, buffer[i]._color, _render_buf, buffer[i]._char_pos,
				          buffer[i + 1]._char_pos - buffer[i]._char_pos, {buffer[i]._char_pos, line_index}, font,
				          _theme.style_color(buffer[i]._color), bg_color);
			draw_line(draw, origin, rect, buffer[block_count - 1]._color, _render_buf, buffer[block_count - 1]._char_pos,
			          length - buffer[block_count - 1]._char_pos, {buffer[block_count - 1]._char_pos, line_index}, font,
			          _theme.style_color(buffer[block_count - 1]._color), bg_color);
		}
		else
			draw_line(draw, origin, rect, text_style::normal_text, _render_buf, 0, length, {0, line_index}, font,
			          _theme.style_color(text_style::normal_text), bg_color);

		if (_doc->is_inside_selection({length, line_index}))
		{
			const auto selection_x = std::max(origin.x, rect.left);
			draw.draw_rect({selection_x, rect.top, selection_x + _font_extent.cx, rect.bottom},
			               core::color(_theme.style_color(text_style::sel_bkgnd)));
		}
	}

	void doc_view::draw_margin(render::draw_context& draw, const core::recti& rect, const int line_index,
	                           const render::font_face font) const
	{
		if (!_sel_margin) return;
		auto text_rect = rect;
		text_rect.right -= 4;
		if (_word_wrap && rect.height() > _font_extent.cy) text_rect.bottom = text_rect.top + _font_extent.cy;
		const auto number = std::to_string(line_index + 1);
		const auto measured = draw.measure_text(number, font, render::text_style::single_line, 100000);
		const auto x = text_rect.right - measured.cx;
		const auto y = text_rect.top + (text_rect.height() - measured.cy) / 2;
		draw.draw_rect(text_rect, core::color(_theme.style_color(text_style::sel_margin)));
		draw.draw_text(number, {x, y, text_rect.right, y + measured.cy}, font, render::text_style::single_line,
		               core::color(core::rgba(120, 120, 120)), core::color(_theme.style_color(text_style::sel_margin)));
	}
	void doc_view::draw_view(render::draw_context& draw) const
	{
		const auto client = client_rect();
		const auto clip = draw.clip_bounds();
		const auto line_count = _doc ? static_cast<int>(_doc->size()) : 0;
		const auto margin = margin_width();
		const auto padding_left = margin + _font_extent.cx;

		draw.draw_rect(client, core::color(_theme.style_color(text_style::normal_bkgnd)));
		draw.draw_rect({0, 0, margin, client.bottom},
		               core::color(_theme.style_color(_sel_margin ? text_style::sel_margin : text_style::normal_bkgnd)));

		if (_focused && _doc)
		{
			const auto caret = text_to_client(_doc->cursor_pos());
			const auto pad = focus_band_padding();
			auto band = full_width_band_rect(caret.y - pad, _font_extent.cy + pad * 2);
			band.top = std::max(band.top, text_top());
			band.bottom = std::min(band.bottom, client.bottom);
			if (band.bottom > band.top) draw.draw_rect(band, core::color(document_focus_band_color()));
		}

		if (_doc && line_count > 0)
		{
			int current_line;
			if (_word_wrap && !_wrap_line_y.empty())
			{
				const auto first_row = _font_extent.cy > 0
					                       ? std::max(0, (_scroll_offset.y - top_content_padding()) / _font_extent.cy)
					                       : 0;
				current_line = visual_row_to_line_index(first_row);
			}
			else
				current_line = std::max(0, scroll_line() - 1);
			current_line = std::clamp(current_line, 0, line_count - 1);
			int y = line_offset(current_line) - _scroll_offset.y + text_top();
			while (y < client.bottom && current_line < line_count)
			{
				const auto height = line_height(current_line);
				if (y + height > clip.top && y < clip.bottom)
				{
					draw_margin(draw, {0, y, margin, y + height}, current_line, body_font());
					draw_line(draw, {padding_left, y, client.right, y + height}, current_line, body_font());
				}
				++current_line;
				y += height;
			}
		}

		const auto scrollbar = scrollbar_rect();
		draw_scrollbar(_vscroll, draw, scrollbar, _theme);
		draw_scrollbar(_hscroll, draw, scrollbar, _theme);
		draw_caret(draw);
		draw_drop_marker(draw);
		draw_message_bar(draw);
	}

	void doc_view::draw_drop_marker(render::draw_context& draw) const
	{
		if (!_drag_text) return;
		const auto point = text_to_client(_drop_location);
		const auto client = client_rect();
		if (point.y + _font_extent.cy > client.top && point.y < client.bottom)
			draw.draw_rect({point.x, point.y, point.x + 2, point.y + _font_extent.cy},
			               core::color(_theme.style_color(text_style::md_bullet)));
	}

	void doc_view::draw_caret(render::draw_context& draw) const
	{
		if (!has_caret() || !_focused || !_caret_visible || !_doc) return;
		const auto position = _doc->cursor_pos();
		if (_doc->calc_offset(position.y, position.x) < scroll_char()) return;
		const auto point = text_to_client(position);
		const auto caret = core::recti(point.x, point.y, point.x + 2, point.y + _font_extent.cy);
		const auto client = client_rect();
		if (caret.top < client.bottom && caret.bottom > client.top)
			draw.draw_rect(caret, core::color(_theme.text));
	}

	void doc_view::start_caret_blink()
	{
		if (!has_caret() || !_focused) return;
		_caret_visible = true;
		if (auto* const h = host()) h->caret_timer(true, 530);
	}

	void doc_view::stop_caret_blink()
	{
		_caret_visible = false;
		if (auto* const h = host()) h->caret_timer(false, 0);
	}

	void doc_view::reset_caret_blink()
	{
		if (!has_caret() || !_focused) return;
		_caret_visible = true;
		if (auto* const h = host()) h->caret_timer(true, 530);
	}

	void doc_view::update_caret()
	{
		reset_caret_blink();
		if (auto* const h = host()) h->text_input(_focused && traits.typing, caret_bounds().offset(origin()));
		_host.invalidate_caret();
	}

	void doc_view::set_scroll_pixel(const int new_y)
	{
		const auto max_y = max_scroll_pixel();
		const auto clamped = std::clamp(new_y, 0, max_y);
		if (_scroll_offset.y == clamped) return;
		_scroll_offset.y = clamped;
		recalc_vert_scrollbar();
		_host.invalidate_status();
		invalidate();
	}

	void doc_view::set_scroll_char(const int x)
	{
		_scroll_offset.x = std::max(0, x) * std::max(1, _font_extent.cx);
	}

	int doc_view::scroll_line() const
	{
		return _font_extent.cy > 0 ? _scroll_offset.y / _font_extent.cy : 0;
	}

	int doc_view::scroll_char() const
	{
		return _font_extent.cx > 0 ? _scroll_offset.x / _font_extent.cx : 0;
	}

	void doc_view::scroll_to_char(const int x)
	{
		if (scroll_char() == x) return;
		set_scroll_char(x);
		recalc_horz_scrollbar();
		_host.invalidate_status();
		invalidate();
	}

	void doc_view::scroll_to_line(const int y)
	{
		set_scroll_pixel(y * std::max(1, _font_extent.cy));
	}

	void doc_view::wrap_scroll_by(const int delta)
	{
		if (_word_wrap && !_wrap_line_y.empty() && can_scroll())
			set_scroll_pixel(_scroll_offset.y + delta * _font_extent.cy);
		else
			scroll_by(delta);
	}

	void doc_view::scroll_by(const int delta)
	{
		set_scroll_pixel(_scroll_offset.y + delta * std::max(1, _font_extent.cy));
	}

	bool doc_view::can_scroll() const
	{
		if (_word_wrap && _total_visual_rows > 0)
			return _screen_lines < _total_visual_rows;
		const auto line_count = _doc ? static_cast<int>(_doc->size()) : 0;
		return line_count > 1 && _screen_lines < line_count - 1;
	}

	core::recti doc_view::scrollbar_rect() const
	{
		auto result = client_rect();
		result.top += text_top();
		return result;
	}

	int doc_view::text_top() const
	{
		return status_text().empty() ? 0 : _font_extent.cy + _font_extent.cy / 2;
	}

	core::recti doc_view::client_rect() const
	{
		return {0, 0, _view_extent.cx, _view_extent.cy};
	}

	int doc_view::max_scroll_pixel() const
	{
		return std::max(0, _content_extent.cy - std::max(0, _view_extent.cy - text_top()));
	}

	core::color32 doc_view::document_focus_band_color() const
	{
		return core::darken(_theme.tool_background, 0.12f);
	}

	int doc_view::focus_band_padding() const
	{
		return std::max(1, _font_extent.cy / 6);
	}

	core::recti doc_view::full_width_band_rect(const int top, const int height) const
	{
		return {0, top, _view_extent.cx, top + height};
	}

	void doc_view::draw_message_bar(render::draw_context& draw) const
	{
		const auto text = status_text();
		if (text.empty()) return;
		const auto client = client_rect();
		const auto bar_height = text_top();
		const auto pad_y = _font_extent.cy / 4;
		const auto background = _focused ? _theme.focus_handle : _theme.handle;
		const auto bounds = core::recti(0, 0, client.right, bar_height);
		const auto text_width = core::utf8_codepoint_count(text) * _font_extent.cx;
		const auto text_x = (client.right - text_width) / 2;
		draw.draw_rect(bounds, core::color(background));
		draw.draw_text(text, {text_x, pad_y, text_x + text_width, bar_height}, body_font(),
		               render::text_style::single_line, core::color(_theme.text), core::color(background));
	}

	void doc_view::on_timer_drag_selection()
	{
		if (!_doc || !_drag_selection) return;
		const auto client = client_rect();
		bool changed = false;
		if (_drag_point.y < client.top)
		{
			const auto step = _drag_point.y < client.top - _font_extent.cy ? -3 : -1;
			const auto old = _scroll_offset.y;
			set_scroll_pixel(_scroll_offset.y + step * _font_extent.cy);
			changed |= old != _scroll_offset.y;
		}
		else if (_drag_point.y >= client.bottom)
		{
			const auto step = _drag_point.y >= client.bottom + _font_extent.cy ? 3 : 1;
			const auto old = _scroll_offset.y;
			set_scroll_pixel(_scroll_offset.y + step * _font_extent.cy);
			changed |= old != _scroll_offset.y;
		}
		if (_drag_point.x < client.left)
		{
			const auto old = scroll_char();
			scroll_to_char(std::max(0, old - 1));
			changed |= old != scroll_char();
		}
		else if (_drag_point.x >= client.right)
		{
			const auto old = scroll_char();
			scroll_to_char(old + 1);
			changed |= old != scroll_char();
		}
		if (changed)
		{
			const auto position = client_to_text(_drag_point);
			_doc->select(_line_selection ? _doc->line_selection(position, true)
			                            : (_word_selection ? _doc->word_selection(position, true)
			                                               : _doc->pos_selection(position, true)));
		}
	}

	void doc_view::on_left_button_down(const core::pointi point, const platform::key_state keys)
	{
		if (!_doc) return;
		const auto scrollbar = scrollbar_rect();
		begin_scrollbar_tracking(_vscroll, point, scrollbar, host());
		if (_vscroll._tracking || _hscroll._tracking)
		{
			invalidate();
			return;
		}
		begin_scrollbar_tracking(_hscroll, point, client_rect(), host());
		if (_hscroll._tracking)
		{
			invalidate();
			return;
		}

		if (allows_text_drag() && !keys.shift && !keys.control && point.x >= margin_width() && _doc->has_selection() &&
			_doc->is_inside_selection(client_to_text(point)))
		{
			_drag_text_pending = true;
			_drag_text_origin = point;
			if (auto* const h = host()) h->capture_pointer(true);
			return;
		}

		if (point.x < margin_width())
		{
			if (keys.control)
				_doc->select(_doc->all());
			else if (allows_drag_selection())
			{
				_doc->select(_doc->line_selection(client_to_text(point), keys.shift));
				_drag_sel_timer = 1;
				if (auto* const h = host()) h->capture_pointer(true);
				_word_selection = false;
				_line_selection = true;
				_drag_selection = true;
			}
		}
		else if (allows_drag_selection())
		{
			const auto position = client_to_text(point);
			_doc->select(keys.control ? _doc->word_selection(position, keys.shift) : _doc->pos_selection(position, keys.shift));
			_drag_sel_timer = 1;
			if (auto* const h = host()) h->capture_pointer(true);
			_word_selection = keys.control;
			_line_selection = false;
			_drag_selection = true;
		}
	}

	void doc_view::on_left_button_up(const core::pointi point, const platform::key_state keys)
	{
		if (_vscroll._tracking || _hscroll._tracking)
		{
			_vscroll._tracking = false;
			_hscroll._tracking = false;
			if (auto* const h = host()) h->capture_pointer(false);
			invalidate();
			return;
		}
		if (!_doc) return;
		if (_drag_text_pending)
		{
			_drag_text_pending = false;
			if (auto* const h = host()) h->capture_pointer(false);
			const auto position = client_to_text(point);
			_doc->select(text_selection(position, position));
			invalidate();
			return;
		}
		if (_drag_text)
		{
			_drag_text = false;
			if (auto* const h = host()) h->capture_pointer(false);
			drop_selected_text(client_to_text(point), keys.control);
			return;
		}
		if (_drag_selection)
		{
			const auto position = client_to_text(point);
			_doc->select(_line_selection ? _doc->line_selection(position, true)
			                            : (_word_selection ? _doc->word_selection(position, true)
			                                               : _doc->pos_selection(position, true)));
			_drag_selection = false;
			_drag_sel_timer = 0;
			if (auto* const h = host()) h->capture_pointer(false);
		}
	}

	void doc_view::on_left_button_double_click(const core::pointi point, const platform::key_state keys)
	{
		if (!_doc || _drag_selection || !allows_drag_selection()) return;
		_doc->select(_doc->word_selection(client_to_text(point), keys.shift));
		_drag_sel_timer = 1;
		if (auto* const h = host()) h->capture_pointer(true);
		_word_selection = true;
		_line_selection = false;
		_drag_selection = true;
	}

	void doc_view::on_right_button_down(const core::pointi point, const platform::key_state)
	{
		if (!_doc || !allows_drag_selection()) return;
		const auto position = client_to_text(point);
		if (!_doc->is_inside_selection(position))
		{
			_doc->select(text_selection(position, position));
			ensure_visible(position);
		}
	}

	void doc_view::drop_selected_text(const text_location& target, const bool copy)
	{
		if (!_doc) return;
		const auto selection = _doc->selection();
		if (selection.empty() || !_doc->query_editable()) return;
		if (!(target < selection._start) && !(selection._end < target)) return;
		const auto text = _doc->copy();
		if (text.empty()) return;
		undo_group group(_doc);
		auto insert_at = target;
		if (!copy)
		{
			insert_at = adjust_for_removal(selection, target);
			_doc->delete_text(group, selection);
		}
		const auto end = _doc->insert_text(group, insert_at, text);
		_doc->select(text_selection(insert_at, end));
		_host.invalidate_view();
	}

	void doc_view::cancel_text_drag()
	{
		if (!_drag_text && !_drag_text_pending) return;
		_drag_text = false;
		_drag_text_pending = false;
		if (auto* const h = host()) h->capture_pointer(false);
		_host.invalidate_status();
	}

	void doc_view::invalidate_selection()
	{
		const auto selection = current_selection();
		if (!selection.empty()) invalidate_lines(selection._start.y, selection._end.y);
	}

	void doc_view::update_cursor(const core::pointi point) const
	{
		if (auto* const h = host()) h->set_cursor(cursor_at(point));
	}
}
