// widgets.h — edit_box, caret_blinker, splitter, custom_scrollbar, edit_box_widget.

#pragma once

#include "platform.h"
#include "ui/theme.h"

namespace pf::ui
{
struct edit_box
{
	std::string text;
	int cursor_pos = 0;
	int sel_anchor = 0;

	[[nodiscard]] int sel_start() const { return std::min(cursor_pos, sel_anchor); }
	[[nodiscard]] int sel_end() const { return std::max(cursor_pos, sel_anchor); }
	[[nodiscard]] bool has_selection() const { return cursor_pos != sel_anchor; }

	void delete_selection()
	{
		if (!has_selection()) return;
		const auto s = sel_start();
		const auto e = sel_end();
		text.erase(s, e - s);
		cursor_pos = s;
		sel_anchor = s;
	}

	void insert_at_cursor(const std::string_view t)
	{
		if (has_selection()) delete_selection();
		text.insert(cursor_pos, t);
		cursor_pos += static_cast<int>(t.length());
		sel_anchor = cursor_pos;
	}

	void select_all()
	{
		sel_anchor = 0;
		cursor_pos = static_cast<int>(text.length());
	}

	[[nodiscard]] std::string_view selected_text() const
	{
		return std::string_view(text).substr(sel_start(), sel_end() - sel_start());
	}

	bool copy_to_clipboard() const
	{
		return has_selection() && pf::platform_text_to_clipboard(selected_text());
	}

	bool cut_to_clipboard()
	{
		if (!copy_to_clipboard()) return false;
		delete_selection();
		return true;
	}

	// Newlines are stripped: a single-line field cannot represent them
	bool paste_from_clipboard()
	{
		const auto clip = pf::platform_text_from_clipboard();
		if (clip.empty()) return false;

		std::string clean;
		clean.reserve(clip.size());
		for (const auto c : clip)
			if (c != '\r' && c != '\n')
				clean += c;

		insert_at_cursor(clean);
		return true;
	}

	bool delete_forward()
	{
		if (has_selection())
		{
			delete_selection();
			return true;
		}
		if (cursor_pos < static_cast<int>(text.length()))
		{
			const auto next = pf::utf8_next(text, cursor_pos);
			text.erase(cursor_pos, next - cursor_pos);
			return true;
		}
		return false;
	}

	bool delete_back()
	{
		if (has_selection())
		{
			delete_selection();
			return true;
		}
		if (cursor_pos > 0)
		{
			const auto prev = pf::utf8_prev(text, cursor_pos);
			text.erase(prev, cursor_pos - prev);
			cursor_pos = prev;
			sel_anchor = cursor_pos;
			return true;
		}
		return false;
	}

	// Returns true if text was modified
	bool on_char(const char32_t ch)
	{
		if (ch == 0x08) // Backspace
			return delete_back();

		if (ch >= U' ')
		{
			insert_at_cursor(pf::utf8_encode(ch));
			return true;
		}

		return false;
	}

	// Returns true if the key was handled. Sets text_modified if text content changed.
	bool on_key_down(const pf::window_frame_ptr& w, const unsigned int vk, bool& text_modified)
	{
		namespace pk = pf::platform_key;
		const bool shift = w->is_key_down(pk::Shift);
		const bool ctrl = w->is_key_down(pk::Control);
		text_modified = false;

		if (vk == pk::Left)
		{
			if (ctrl) // word-skip left
			{
				auto pos = cursor_pos;
				while (pos > 0 && text[pos - 1] == ' ') pos--;
				while (pos > 0 && text[pos - 1] != ' ') pos--;
				cursor_pos = pos;
			}
			else if (cursor_pos > 0)
			{
				cursor_pos = pf::utf8_prev(text, cursor_pos);
			}
			if (!shift) sel_anchor = cursor_pos;
			return true;
		}

		if (vk == pk::Right)
		{
			const auto len = static_cast<int>(text.length());
			if (ctrl)
			{
				auto pos = cursor_pos;
				while (pos < len && text[pos] != ' ') pos++;
				while (pos < len && text[pos] == ' ') pos++;
				cursor_pos = pos;
			}
			else if (cursor_pos < len)
			{
				cursor_pos = pf::utf8_next(text, cursor_pos);
			}
			if (!shift) sel_anchor = cursor_pos;
			return true;
		}

		if (vk == pk::Home)
		{
			cursor_pos = 0;
			if (!shift) sel_anchor = cursor_pos;
			return true;
		}

		if (vk == pk::End)
		{
			cursor_pos = static_cast<int>(text.length());
			if (!shift) sel_anchor = cursor_pos;
			return true;
		}

		if (vk == pk::Delete)
		{
			text_modified = delete_forward();
			return true;
		}

		return false;
	}

	// --- Common drawing helpers for edit-box-based views ---

	static void draw_border(pf::draw_context& dc, const pf::irect& box, const bool focused,
	                        const double dpi_scale = 1.0)
	{
		const int thickness = std::max(1, static_cast<int>(2 * dpi_scale));
		const auto color = focused ? colors::focus_handle_color : colors::handle_color;
		dc.fill_solid_rect(box.left, box.top, box.width(), thickness, color);
		dc.fill_solid_rect(box.left, box.bottom - thickness, box.width(), thickness, color);
		dc.fill_solid_rect(box.left, box.top, thickness, box.height(), color);
		dc.fill_solid_rect(box.right - thickness, box.top, thickness, box.height(), color);
	}

	void draw_selection(pf::draw_context& dc, const int text_x, const int text_y,
	                    const int char_cy, const pf::font& font) const
	{
		if (!has_selection()) return;
		const std::string_view view = text;
		const auto before_sz = dc.measure_text(view.substr(0, sel_start()), font);
		const auto sel_sz = dc.measure_text(view.substr(sel_start(), sel_end() - sel_start()), font);
		dc.fill_solid_rect(pf::irect(text_x + before_sz.cx, text_y,
		                             text_x + before_sz.cx + sel_sz.cx, text_y + char_cy),
		                   pf::color_t(88, 88, 88));
	}

	void draw_caret(pf::draw_context& dc, const int text_x, const int text_y,
	                const int char_cy, const pf::font& font, const double dpi_scale = 1.0) const
	{
		int caret_x = text_x;
		if (!text.empty() && cursor_pos > 0)
			caret_x += dc.measure_text(std::string_view(text).substr(0, cursor_pos), font).cx;
		const int caret_w = std::max(1, static_cast<int>(2 * dpi_scale));
		dc.fill_solid_rect(caret_x, text_y, caret_w, char_cy, colors::text_color);
	}
};


// caret_blinker — Shared timer-based caret blink logic used by text views and edit-box views
struct caret_blinker
{
	bool visible = false;
	bool active = false;

	static constexpr uint32_t timer_id = 1002;
	static constexpr uint32_t blink_ms = 530;

	void start(const pf::window_frame_ptr& window)
	{
		if (!active)
		{
			visible = true;
			active = true;
			window->set_timer(timer_id, blink_ms);
		}
	}

	void stop(const pf::window_frame_ptr& window)
	{
		if (active)
		{
			active = false;
			visible = false;
			window->kill_timer(timer_id);
		}
	}

	void reset(const pf::window_frame_ptr& window)
	{
		visible = true;
		if (active)
			window->kill_timer(timer_id);
		active = true;
		window->set_timer(timer_id, blink_ms);
	}

	// Returns true if this timer event was handled (caller should invalidate)
	bool on_timer(const uint32_t id)
	{
		if (id == timer_id && active)
		{
			visible = !visible;
			return true;
		}
		return false;
	}
};


struct splitter
{
	enum class orientation { vertical, horizontal };

	orientation _orient;
	double _ratio = 0.5;
	bool _tracking = false;
	bool _hover = false;
	double _dpi_scale = 1.0;

	static constexpr int base_bar_width = 5;
	static constexpr double min_ratio = 0.05;
	static constexpr double max_ratio = 0.95;

	[[nodiscard]] int bar_width() const { return static_cast<int>(base_bar_width * _dpi_scale); }

	void set_dpi_scale(const double s) { _dpi_scale = s; }

	splitter(const orientation o, const double initial_ratio)
		: _orient(o), _ratio(initial_ratio)
	{
	}

	[[nodiscard]] int split_pos(const pf::irect& bounds) const
	{
		if (_orient == orientation::vertical)
			return static_cast<int>(bounds.left + (bounds.right - bounds.left) * _ratio);
		return static_cast<int>(bounds.top + (bounds.bottom - bounds.top) * _ratio);
	}

	[[nodiscard]] pf::irect bar_rect(const pf::irect& bounds) const
	{
		const auto pos = split_pos(bounds);
		const auto bw = bar_width();
		if (_orient == orientation::vertical)
			return {pos - bw, bounds.top, pos + bw, bounds.bottom};
		return {bounds.left, pos - bw, bounds.right, pos + bw};
	}

	[[nodiscard]] bool hit_test(const pf::irect& bounds, const pf::ipoint& pt) const
	{
		return bar_rect(bounds).contains(pt);
	}

	void update_ratio(const pf::irect& bounds, const pf::ipoint& pt)
	{
		if (_orient == orientation::vertical)
		{
			const auto width = bounds.right - bounds.left;
			if (width > 0)
				_ratio = (pt.x - bounds.left) / static_cast<double>(width);
		}
		else
		{
			const auto height = bounds.bottom - bounds.top;
			if (height > 0)
				_ratio = (pt.y - bounds.top) / static_cast<double>(height);
		}

		if (_ratio < min_ratio) _ratio = min_ratio;
		if (_ratio > max_ratio) _ratio = max_ratio;
	}

	[[nodiscard]] pf::color_t color() const
	{
		if (_tracking) return colors::handle_tracking_color;
		if (_hover) return colors::handle_hover_color;
		return colors::handle_color;
	}

	[[nodiscard]] pf::cursor_shape cursor() const
	{
		return _orient == orientation::vertical
			       ? pf::cursor_shape::size_we
			       : pf::cursor_shape::size_ns;
	}

	void draw(pf::draw_context& dc, const pf::irect& bounds) const
	{
		dc.fill_solid_rect(bar_rect(bounds), color());
	}

	bool begin_tracking(const pf::irect& bounds, const pf::ipoint& pt, const pf::window_frame_ptr& window)
	{
		if (!hit_test(bounds, pt)) return false;
		_tracking = true;
		window->set_capture();
		window->set_cursor_shape(cursor());
		return true;
	}

	bool track_to(const pf::irect& bounds, const pf::ipoint& pt, const pf::window_frame_ptr& window)
	{
		if (!_tracking) return false;
		update_ratio(bounds, pt);
		window->invalidate();
		return true;
	}

	void end_tracking(const pf::window_frame_ptr& window)
	{
		if (_tracking)
		{
			_tracking = false;
			window->release_capture();
			window->invalidate();
		}
	}

	bool update_hover(const pf::irect& bounds, const pf::ipoint& pt, const pf::window_frame_ptr& window)
	{
		const auto new_hover = hit_test(bounds, pt);
		if (new_hover == _hover) return false;
		_hover = new_hover;
		if (_hover)
		{
			window->track_mouse_leave();
			window->set_cursor_shape(cursor());
		}
		window->invalidate();
		return true;
	}

	void clear_hover(const pf::window_frame_ptr& window)
	{
		if (_hover)
		{
			_hover = false;
			window->invalidate();
		}
	}
};


struct custom_scrollbar
{
	enum class orientation { vertical, horizontal };

	orientation _orient;
	int _content_size = 0;
	int _page_size = 0;
	int _position = 0;

	bool _hover = false;
	bool _tracking = false;
	bool _hover_tracking = false;
	int _tracking_start_mouse = 0;
	int _tracking_start_pos = 0;

	static constexpr int base_thumb_thickness = 8;
	static constexpr int base_edge_margin = 4;
	static constexpr int base_hover_track_width = 26;
	static constexpr int base_hit_margin = 32;

	double _dpi_scale = 1.0;

	[[nodiscard]] int thumb_thickness() const { return static_cast<int>(base_thumb_thickness * _dpi_scale); }
	[[nodiscard]] int edge_margin() const { return static_cast<int>(base_edge_margin * _dpi_scale); }
	[[nodiscard]] int hover_track_width() const { return static_cast<int>(base_hover_track_width * _dpi_scale); }
	[[nodiscard]] int hit_margin() const { return static_cast<int>(base_hit_margin * _dpi_scale); }

	void set_dpi_scale(const double s) { _dpi_scale = s; }

	custom_scrollbar(const orientation o) : _orient(o)
	{
	}

	[[nodiscard]] bool can_scroll() const
	{
		return _content_size > _page_size && _page_size > 0;
	}

	void update(const int content_size, const int page_size, const int position)
	{
		_content_size = content_size;
		_page_size = page_size;
		_position = position;
	}

	struct thumb_span
	{
		int start = 0;
		int length = 0;
	};

	// The minimum thumb length costs room the proportional position does not know about, so
	// without the clamp the last page of a long document pushes the thumb off the end of the track
	[[nodiscard]] thumb_span thumb(const int track_start, const int track_extent) const
	{
		if (!can_scroll())
			return {track_start, 0};

		const auto length = std::max(pf::mul_div(_page_size, track_extent, _content_size),
		                             thumb_thickness());
		const auto limit = std::max(track_start, track_start + track_extent - length);
		const auto start = std::clamp(pf::mul_div(_position, track_extent, _content_size) + track_start,
		                              track_start, limit);
		return {start, length};
	}

	bool hit_test(const pf::ipoint& pt, const pf::irect& client) const
	{
		if (!can_scroll()) return false;
		if (_orient == orientation::vertical)
			return pt.x >= client.right - hit_margin() && pt.y >= client.top && pt.y < client.bottom;
		return pt.y >= client.bottom - hit_margin() && pt.x >= client.left && pt.x < client.right;
	}

	void draw(pf::draw_context& dc, const pf::irect& client) const
	{
		if (!can_scroll()) return;

		const auto tt = thumb_thickness();
		const auto em = edge_margin();
		const auto htw = hover_track_width();
		const auto scaled_pad = static_cast<int>(10 * _dpi_scale);

		if (_orient == orientation::vertical)
		{
			const auto [y, cy] = thumb(client.top, client.height());
			const auto right = client.right;
			auto x_pad = 0;

			if (_hover || _tracking)
			{
				dc.fill_solid_rect(pf::irect(right - htw, client.top, right, client.bottom),
				                   colors::handle_color);
				x_pad = scaled_pad;
			}

			const auto c = _tracking ? colors::handle_tracking_color : colors::handle_hover_color;
			dc.fill_solid_rect(pf::irect(right - tt - em - x_pad, y,
			                             right - em, y + cy), c);
		}
		else
		{
			const auto [x, cx] = thumb(client.left, client.width());
			const auto bottom = client.bottom;
			auto y_pad = 0;

			if (_hover || _tracking)
			{
				dc.fill_solid_rect(pf::irect(client.left, bottom - htw, client.right, bottom),
				                   colors::handle_color);
				y_pad = scaled_pad;
			}

			const auto c = _tracking ? colors::handle_tracking_color : colors::handle_hover_color;
			dc.fill_solid_rect(pf::irect(x, bottom - tt - em - y_pad,
			                             x + cx, bottom - em), c);
		}
	}

	bool begin_tracking(const pf::ipoint& pt, const pf::irect& client, const pf::window_frame_ptr& window)
	{
		if (!hit_test(pt, client)) return false;
		_tracking = true;
		_tracking_start_mouse = _orient == orientation::vertical ? pt.y : pt.x;
		_tracking_start_pos = _position;
		window->set_capture();
		return true;
	}

	int track_to(const pf::ipoint& pt, const pf::irect& client) const
	{
		if (!_tracking) return _position;
		const int delta_mouse = _orient == orientation::vertical
			                        ? pt.y - _tracking_start_mouse
			                        : pt.x - _tracking_start_mouse;
		const int extent = _orient == orientation::vertical ? client.height() : client.width();
		if (extent <= 0) return _position;
		const auto delta_content = pf::mul_div(delta_mouse, _content_size, extent);
		return std::clamp(_tracking_start_pos + delta_content, 0, _content_size - _page_size);
	}

	void end_tracking(const pf::window_frame_ptr& window)
	{
		if (_tracking)
		{
			_tracking = false;
			window->release_capture();
		}
	}

	bool set_hover(const bool hover)
	{
		if (_hover == hover) return false;
		_hover = hover;
		return true;
	}

	// Unified mouse handling — returns the new scroll position via on_scroll when tracking.
	// Returns true if the message was consumed by the scrollbar.
	bool handle_mouse(const pf::mouse_message_type msg, const pf::ipoint& pt, const pf::irect& client,
	                  const pf::window_frame_ptr& window,
	                  const std::function<void(int)>& on_scroll = {})
	{
		using mt = pf::mouse_message_type;

		if (msg == mt::left_button_down)
		{
			if (begin_tracking(pt, client, window))
			{
				window->invalidate();
				return true;
			}
			return false;
		}
		if (msg == mt::mouse_move)
		{
			if (_tracking)
			{
				const auto new_pos = track_to(pt, client);
				if (on_scroll) on_scroll(new_pos);
				window->invalidate();
				return true;
			}
			if (set_hover(hit_test(pt, client)))
				window->invalidate();
			if (!_hover_tracking)
			{
				window->track_mouse_leave();
				_hover_tracking = true;
			}
			return false;
		}
		if (msg == mt::left_button_up)
		{
			if (_tracking)
			{
				end_tracking(window);
				window->invalidate();
				return true;
			}
			return false;
		}
		if (msg == mt::mouse_leave)
		{
			_hover_tracking = false;
			if (set_hover(false))
				window->invalidate();
			return false;
		}
		return false;
	}
};


// edit_box_widget — Composite of edit_box + caret_blinker for views with an inline text field.
struct edit_box_widget
{
	edit_box edit;
	caret_blinker caret;

	void update_focus(const pf::window_frame_ptr& window, const bool focused)
	{
		if (focused)
			caret.start(window);
		else
			caret.stop(window);
	}

	void reset_caret(const pf::window_frame_ptr& window)
	{
		if (window && window->has_focus())
			caret.reset(window);
	}

	// Returns true if the timer was handled (caller should invalidate the edit rect)
	bool on_timer(const uint32_t id)
	{
		return caret.on_timer(id);
	}

	// Returns true if text content was modified
	bool on_char(const pf::window_frame_ptr& window, const char32_t ch)
	{
		const bool modified = edit.on_char(ch);
		reset_caret(window);
		return modified;
	}

	// Returns true if the key was handled. Sets text_modified if text content changed.
	bool on_key_down(const pf::window_frame_ptr& window, const unsigned int vk, bool& text_modified)
	{
		if (edit.on_key_down(window, vk, text_modified))
		{
			reset_caret(window);
			return true;
		}
		return false;
	}
};


}
