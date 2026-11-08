// This file is part of libdesktop, a C++20 platform layer for desktop applications
// Copyright 2026  Zac Walker
//
// This program is free software; you can redistribute it and / or modify it
// under the terms of the LGPL License either version 2.1 or later.
// License details are available at https://www.gnu.org/licenses/lgpl-2.1.html
// This program is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY

// Purpose: Text-view scrolling, command routing, caret timing, and render metrics.

#include "libdesktop/ui/views/text_view.h"

#include <algorithm>
#include <any>
#include <memory>

namespace
{
	class null_view_host final : public ui::view_host
	{
	public:
		void invalidate_lines(int, int) override {}
		void lines_changed(int, int) override {}
		void line_count_changed(int, int) override {}
		void ensure_visible(const ui::text_location&) override {}
		void invalidate_view() override {}
		void invalidate_layout() override {}
		void invalidate_caret() override {}
		void invalidate_scrollbar() override {}
		void invalidate_status() override {}
	};
}

ui::view_host& ui::text_view::default_view_host()
{
	static null_view_host host;
	return host;
}

ui::text_view::text_view(view_host& host, const view_theme& theme, view_context* const context,
                         text_clipboard* const clipboard) : view_base(theme), _host(host), _context(context),
                                                            _clipboard(clipboard ? clipboard : &system_clipboard())
{
}

ui::text_view::text_view(const view_theme& theme, view_context* const context,
                         text_clipboard* const clipboard) : text_view(default_view_host(), theme, context, clipboard)
{
}

render::font_face ui::text_view::body_face() const
{
	if (!_body_face_valid)
	{
		_body_face = render::make_font(body_font_desc());
		_body_face_valid = true;
	}
	return _body_face;
}

void ui::text_view::notify_view_invalidated() const
{
	_host.invalidate_view();
	invalidate();
}

std::string_view ui::text_view::status_text() const
{
	return _context ? _context->status_text() : std::string_view{};
}

int ui::text_view::message_bar_height() const
{
	return status_text().empty() ? 0 : _font_extent.cy + _font_extent.cy / 2;
}

void ui::text_view::on_placed(render::measure_context& mc)
{
	view_base::on_placed(mc);
	_body_face_valid = false;
	const auto face = body_font();
	const auto measured = mc.measure_text("M", face, render::text_style::single_line, 10000);
	_font_extent.cx = std::max(1, measured.cx);
	_font_extent.cy = std::max(1, mc.text_line_height(face));
	if (measured.cy > 0) _font_extent.cy = std::max(_font_extent.cy, measured.cy);
	_screen_lines = std::max(0, _view_extent.cy / _font_extent.cy);
	recalc_horz_scrollbar();
	recalc_vert_scrollbar();
	_host.invalidate_scrollbar();
	layout();
}

void ui::text_view::on_focus(const bool focused, const bool select_all)
{
	_focused = focused;
	_caret_visible = focused;
	if (focused && select_all) select_all_text();
	if (focused) start_caret_blink();
	else stop_caret_blink();
	if (auto* const h = host()) h->text_input(focused && has_caret(), caret_bounds().offset(origin()));
	_host.invalidate_status();
	invalidate();
}

void ui::text_view::on_keyboard(const bool has_keyboard)
{
	if (!has_keyboard && !_focused) stop_caret_blink();
	if (auto* const h = host()) h->text_input(has_keyboard && _focused && has_caret(), caret_bounds().offset(origin()));
}

void ui::text_view::on_escape()
{
	if (_context) _context->on_escape();
}

void ui::text_view::zoom(const int delta)
{
	if (_context) _context->on_zoom(delta);
}

bool ui::text_view::on_key(const int key, const platform::key_state keys)
{
	const auto k = static_cast<char32_t>(key);
	if (k == platform::keys::ESCAPE)
	{
		on_escape();
		return true;
	}

	if (keys.control && !keys.shift && !keys.alt)
	{
		if (k == platform::keys::OEM_PLUS || k == 0x6B)
		{
			zoom(2);
			return true;
		}
		if (k == platform::keys::OEM_MINUS || k == 0x6D)
		{
			zoom(-2);
			return true;
		}

		switch (k)
		{
		case U'A': select_all_text(); return true;
		case U'C': (void)copy_text_to_clipboard(); return true;
		case U'X': (void)cut_text_to_clipboard(); return true;
		case U'V': (void)paste_text_from_clipboard(); return true;
		default: break;
		}
	}

	if (!keys.control && !keys.shift && !keys.alt)
	{
		if (k == platform::keys::UP)
		{
			scroll_by(-1);
			return true;
		}
		if (k == platform::keys::DOWN)
		{
			scroll_by(1);
			return true;
		}
		if (k == platform::keys::PRIOR)
		{
			scroll_by(-_screen_lines);
			return true;
		}
		if (k == platform::keys::NEXT)
		{
			scroll_by(_screen_lines);
			return true;
		}
	}

	return false;
}

bool ui::text_view::on_text(const std::string_view text)
{
	(void)text;
	return false;
}

bool ui::text_view::on_wheel(const core::pointi loc, const int delta, const platform::key_state keys)
{
	if (keys.control)
	{
		zoom(delta > 0 ? 2 : -2);
		return true;
	}
	if (view_base::on_wheel(loc, delta, keys)) return true;
	if (delta == 0) return false;
	set_scroll_pixel(_scroll_offset.y - delta * std::max(1, _font_extent.cy));
	return true;
}

std::vector<ui::command_ptr> ui::text_view::context_commands(const core::pointi loc)
{
	return on_popup_menu(loc);
}

std::vector<ui::command_ptr> ui::text_view::default_popup_menu()
{
	std::vector<command_ptr> items;

	const auto make = [](const std::string_view text, const std::function<void()>& action, const bool enabled)
	{
		auto command = std::make_shared<ui::command>(text, std::any{}, action, false);
		command->enable = enabled;
		return command;
	};

	items.emplace_back(make("Cu&t", [this] { (void)cut_text_to_clipboard(); }, can_cut_text()));
	items.emplace_back(make("&Copy", [this] { (void)copy_text_to_clipboard(); }, can_copy_text()));
	items.emplace_back(make("&Paste", [this] { (void)paste_text_from_clipboard(); }, can_paste_text()));
	items.emplace_back(std::make_shared<ui::command>());
	items.emplace_back(make("Select &All", [this] { select_all_text(); }, true));
	return items;
}

std::vector<ui::command_ptr> ui::text_view::on_popup_menu(core::pointi client_point)
{
	(void)client_point;
	return default_popup_menu();
}

bool ui::text_view::copy_text_to_clipboard()
{
	const auto text = select_text();
	return !text.empty() && set_clipboard(text);
}

std::string ui::text_view::clipboard_text() const
{
	if (!_clipboard) return {};
	if (const auto text = _clipboard->text()) return *text;
	return {};
}

bool ui::text_view::set_clipboard(const std::string_view text) const
{
	return _clipboard && _clipboard->set_text(text);
}

void ui::text_view::ensure_visible(const text_location& point)
{
	const auto line_top = line_content_offset(point.y);
	const auto line_bottom = line_top + _font_extent.cy;
	const auto visible_height = std::max(0, _view_extent.cy - text_top());

	if (line_bottom > _scroll_offset.y + visible_height)
		set_scroll_pixel(line_bottom - visible_height);
	else if (line_top < _scroll_offset.y)
		set_scroll_pixel(line_top);
}

void ui::text_view::invalidate_lines(const int start, const int end)
{
	_host.invalidate_lines(start, end);
	invalidate();
}

void ui::text_view::lines_changed(const int start, const int end)
{
	_host.lines_changed(start, end);
	invalidate();
}

void ui::text_view::line_count_changed(const int at, const int delta)
{
	_host.line_count_changed(at, delta);
	invalidate();
}

void ui::text_view::recalc_horz_scrollbar()
{
	_scroll_offset.x = std::clamp(_scroll_offset.x, 0, std::max(0, _content_extent.cx - _view_extent.cx));
	_hscroll.update(_content_extent.cx, _view_extent.cx, _scroll_offset.x);
}

void ui::text_view::recalc_vert_scrollbar()
{
	const auto visible_height = std::max(0, _view_extent.cy - text_top());
	_scroll_offset.y = std::clamp(_scroll_offset.y, 0, std::max(0, _content_extent.cy - visible_height));
	_vscroll.update(_content_extent.cy, visible_height, _scroll_offset.y);
}

void ui::text_view::scroll_to_top()
{
	_scroll_offset = {};
	recalc_horz_scrollbar();
	recalc_vert_scrollbar();
	_host.invalidate_status();
	invalidate();
}

void ui::text_view::scroll_to_line(const int line)
{
	set_scroll_pixel(line_content_offset(line));
}

void ui::text_view::set_scroll_pixel(const int new_y)
{
	const auto visible_height = std::max(0, _view_extent.cy - text_top());
	const auto max_y = std::max(0, _content_extent.cy - visible_height);
	const auto clamped = std::clamp(new_y, 0, max_y);
	if (_scroll_offset.y == clamped) return;
	_scroll_offset.y = clamped;
	recalc_vert_scrollbar();
	_host.invalidate_status();
	invalidate();
}

int ui::text_view::line_content_offset(const int line) const
{
	return _font_extent.cy + line * _font_extent.cy;
}

void ui::text_view::scroll_by(const int delta)
{
	set_scroll_pixel(_scroll_offset.y + delta * std::max(1, _font_extent.cy));
}

int ui::text_view::centered_text_top(const int top, const int height) const
{
	return top + std::max(0, (height - _font_extent.cy) / 2);
}

core::color32 ui::text_view::focus_band_color() const
{
	return core::darken(_theme.tool_background, 0.12f);
}

int ui::text_view::focus_band_padding() const
{
	return std::max(1, _font_extent.cy / 6);
}

void ui::text_view::draw_message_bar(render::draw_context& dc) const
{
	const auto text = status_text();
	if (text.empty()) return;

	const auto bar_height = message_bar_height();
	const auto bar = core::recti(0, 0, _view_extent.cx, bar_height);
	const auto bg = _focused ? _theme.focus_handle : _theme.handle;
	dc.draw_rect(bar, core::color(bg, dc.colors.alpha));
	const auto measured = dc.measure_text(text, body_font(), render::text_style::single_line, std::max(1, bar.width()));
	const auto x = std::max(0, (bar.width() - measured.cx) / 2);
	dc.draw_text(text, {x, bar.top, x + measured.cx, bar.bottom}, body_font(), render::text_style::single_line,
	             core::color(_theme.text, dc.colors.alpha), core::color(bg, dc.colors.alpha));
}

void ui::text_view::start_caret_blink()
{
	_caret_visible = true;
	invalidate(caret_bounds().inflate(1));
}

void ui::text_view::reset_caret_blink()
{
	_caret_visible = true;
	invalidate(caret_bounds().inflate(1));
}

void ui::text_view::stop_caret_blink()
{
	_caret_visible = false;
	invalidate(caret_bounds().inflate(1));
}

void ui::text_view::blink()
{
	if (!_focused || !has_caret()) return;
	_caret_visible = !_caret_visible;
	invalidate(caret_bounds().inflate(1));
}

void ui::text_view::scale_changed()
{
	view_base::scale_changed();
	_body_face_valid = false;
	if (auto* const mc = metrics()) on_placed(*mc);
}

platform::cursor ui::text_view::cursor_at(const core::pointi loc) const
{
	const auto cursor = view_base::cursor_at(loc);
	return cursor == platform::cursor::normal ? platform::cursor::text_select : cursor;
}