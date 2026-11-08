// This file is part of libdesktop, a C++20 platform layer for desktop applications
// Copyright 2026  Zac Walker
// 
// This program is free software; you can redistribute it and / or modify it
// under the terms of the LGPL License either version 2.1 or later.
// License details are available at https://www.gnu.org/licenses/lgpl-2.1.html
// This program is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY

// Purpose: The drawn controls - push, check and radio buttons, command links, the slider, the toolbar
// and the date field - what every control answers as an element of the tree, the input manager that
// finds them in a frame's tree and routes its input to them, and the slider arithmetic, spelling and
// caption-button helpers they share. The text edit is render_edit.cpp's and the menu render_menu.cpp's.

#include "libdesktop/ui.h"
#include "controls_internal.h"

///////////////////////////////////////////////////////////////////////////////////////////////////
// Helpers
///////////////////////////////////////////////////////////////////////////////////////////////////

void ui::drawn::draw_icon(render::draw_context& dc, const icon_index icon, const core::recti bounds, const core::color c)
{
	if (icon == icon_index{} || bounds.is_empty()) return;
	dc.draw_text(icon_to_utf8(icon), bounds, icon_face(icon), render::text_style::single_line_center, c, {});
}

render::font_face ui::icon_face(const icon_index icon, const chrome_icon_set& chrome)
{
	const auto is_caption = icon != icon_index{} &&
		(icon == chrome.minimize || icon == chrome.maximize || icon == chrome.restore || icon == chrome.close);
	return is_caption ? render::font_face::small_icons : render::font_face::icons;
}

void ui::drawn::draw_outline(render::draw_context& dc, const core::recti b, const core::color c, const int width)
{
	if (b.is_empty() || width <= 0) return;
	dc.draw_rect({b.left, b.top, b.right, b.top + width}, c);
	dc.draw_rect({b.left, b.bottom - width, b.right, b.bottom}, c);
	dc.draw_rect({b.left, b.top + width, b.left + width, b.bottom - width}, c);
	dc.draw_rect({b.right - width, b.top + width, b.right, b.bottom - width}, c);
}

// The native buttons were shaded top to bottom; bands keep that without a gradient primitive.
void ui::drawn::fill_gradient(render::draw_context& dc, const core::recti b, const core::color32 top, const core::color32 bottom,
                              const float alpha)
{
	const auto h = b.height();
	if (h <= 0 || b.width() <= 0) return;

	const auto bands = std::min(h, 16);

	for (auto i = 0; i < bands; ++i)
	{
		const auto y0 = b.top + core::mul_div(i, h, bands);
		const auto y1 = b.top + core::mul_div(i + 1, h, bands);
		const auto t = bands > 1 ? static_cast<float>(i) / static_cast<float>(bands - 1) : 0.0f;
		dc.draw_rect({b.left, y0, b.right, y1}, core::color(core::lerp(top, bottom, t), alpha));
	}
}

// Letters, digits, '_' and '\'' make words, in every script: spacing and the general punctuation
// block break them.
bool ui::drawn::is_word_char(const uint32_t cp)
{
	if (cp < 0x80) return std::isalnum(static_cast<int>(cp)) != 0 || cp == '_' || cp == '\'';
	if (cp == 0xA0 || cp == 0xAD) return false;
	if (cp >= 0x2000 && cp <= 0x206F) return false;
	if (cp >= 0x3000 && cp <= 0x303F) return false;
	if (cp == 0xFEFF) return false;
	return true;
}

void ui::drawn::draw_mnemonic_underline(render::draw_context& dc, const mnemonic_label& label, const int x,
                                        const int top, const core::color c)
{
	if (label.offset == std::string::npos || label.offset >= label.text.size()) return;

	const std::string_view text = label.text;
	const auto end = static_cast<size_t>(core::utf8_next(text, static_cast<int>(label.offset)));
	const auto measure = [&dc](const std::string_view s)
	{
		return dc.measure_text(s, render::font_face::dialog, render::text_style::single_line, 100000).cx;
	};
	const auto left = x + measure(text.substr(0, label.offset));
	const auto right = left + measure(text.substr(label.offset, end - label.offset));
	// Just under the baseline, in the descent, where a native menu draws it.
	const auto thickness = std::max(1.0, dc.scale_factor);
	const auto y = top + dc.text_baseline(render::font_face::dialog) + thickness;
	dc.draw_line({static_cast<double>(left), y}, {static_cast<double>(right), y}, c, thickness);
}

void ui::drawn::draw_check_mark(render::draw_context& dc, const core::recti bounds, const core::color c)
{
	if (bounds.is_empty()) return;
	const auto centre = bounds.center();
	const auto cx = static_cast<double>(centre.x);
	const auto cy = static_cast<double>(centre.y);
	const auto unit = std::min(bounds.width(), bounds.height()) / 8.0;
	const std::array<core::pointd, 3> tick{{
		{cx - unit * 2.5, cy},
		{cx - unit * 0.75, cy + unit * 1.75},
		{cx + unit * 2.5, cy - unit * 2.0}}};
	dc.draw_polyline(std::span<const core::pointd>(tick), c, std::max(1.0, dc.scale_factor * 1.5));
}

int ui::slider_value_to_track_x(const core::recti track_bounds, const int value, const int min_value, const int max_value)
{
	const auto range = std::max(1, max_value - min_value);
	const auto clamped = std::clamp(value, min_value, max_value);
	return track_bounds.left + core::round(static_cast<double>(clamped - min_value) * track_bounds.width() / range);
}

int ui::slider_track_x_to_value(const core::recti track_bounds, const int x, const int min_value, const int max_value,
                                const int current_value)
{
	const auto range = std::max(1, max_value - min_value);
	const auto target = std::clamp(x, track_bounds.left, track_bounds.right);
	const auto width = std::max(1, track_bounds.width());
	const auto raw = static_cast<double>(target - track_bounds.left) * range / width;
	const auto distance_to = [&](const int value)
	{
		return std::abs(slider_value_to_track_x(track_bounds, value, min_value, max_value) - target);
	};

	// Start from the proportional value so both ends stay reachable when several values share a pixel,
	// and let a neighbour win only when its painted thumb is strictly closer.
	const auto nearest = std::clamp(min_value + core::round(raw), min_value, max_value);
	auto best = nearest;
	auto best_distance = distance_to(nearest);

	for (const auto offset : {-1, 1, -2, 2})
	{
		const auto value = nearest + offset;
		if (value < min_value || value > max_value) continue;
		const auto distance = distance_to(value);
		if (distance < best_distance)
		{
			best = value;
			best_distance = distance;
		}
	}

	// Pressing the thumb where it is drawn keeps its value.
	const auto current = std::clamp(current_value, min_value, max_value);
	if (distance_to(current) <= best_distance) best = current;
	return best;
}

std::vector<ui::spelling_range> ui::spelling_ranges(const std::string_view text,
                                                    const std::function<bool(std::string_view)>& is_word_valid)
{
	std::vector<spelling_range> result;
	auto word_start = std::string_view::npos;

	const auto check = [&](const size_t start, const size_t end)
	{
		const auto word = text.substr(start, end - start);
		if (!word.empty() && !is_word_valid(word)) result.push_back({std::string(word), start, end});
	};

	auto i = text.begin();

	while (i < text.end())
	{
		const auto pos = static_cast<size_t>(i - text.begin());
		const auto before = i;
		const auto cp = core::pop_utf8_char(i, text.end());
		if (i == before) ++i;

		if (drawn::is_word_char(cp))
		{
			if (word_start == std::string_view::npos) word_start = pos;
		}
		else if (word_start != std::string_view::npos)
		{
			check(word_start, pos);
			word_start = std::string_view::npos;
		}
	}

	if (word_start != std::string_view::npos) check(word_start, text.size());
	return result;
}

std::vector<ui::spelling_range> ui::unknown_words(const std::string_view text, const spelling_dictionary* dictionary)
{
	if (!dictionary) return {};

	return spelling_ranges(text, [dictionary](const std::string_view word)
	{
		return dictionary->is_word_valid(word);
	});
}

ui::mnemonic_label ui::parse_mnemonic(const std::string_view label)
{
	mnemonic_label result;
	result.text.reserve(label.size());

	for (size_t i = 0; i < label.size(); ++i)
	{
		if (label[i] == '&' && i + 1 < label.size())
		{
			const auto next = static_cast<unsigned char>(label[i + 1]);

			if (next == '&')
			{
				result.text += '&';
				++i;
				continue;
			}

			// A marker comes before a word and not inside one, as the application's labels are written:
			// "&OK", "As &JPEG" and "分析(&A)" have one, while "R&B" and "Fitness & Workout" keep their
			// ampersand.
			const auto is_word = [](const unsigned char c) { return std::isalnum(c) || c == '_' || c >= 0x80; };
			const auto begins_word = i == 0 || !is_word(static_cast<unsigned char>(label[i - 1]));

			if (begins_word && is_word(next))
			{
				if (result.key == 0)
				{
					if (next >= 'a' && next <= 'z') result.key = next - 'a' + 'A';
					else if ((next >= 'A' && next <= 'Z') || (next >= '0' && next <= '9')) result.key = next;
					// The underline goes under the character that gives the key, or the first one marked.
					if (result.key != 0 || result.offset == std::string::npos) result.offset = result.text.size();
				}

				continue;
			}
		}

		result.text += label[i];
	}

	return result;
}

bool ui::caption_button_tracker::hover(const int id)
{
	if (hover_id == id) return false;
	hover_id = id;

	// A press belongs to the button it began on; resting anywhere else abandons it.
	if (pressed_id != id) pressed_id = 0;
	return true;
}

bool ui::caption_button_tracker::press(const int id)
{
	const auto changed = hover_id != id || pressed_id != id;
	hover_id = id;
	pressed_id = id;
	return changed;
}

bool ui::caption_button_tracker::leave()
{
	const auto changed = hover_id != 0 || pressed_id != 0;
	hover_id = 0;
	pressed_id = 0;
	return changed;
}

int ui::caption_button_tracker::release(const int id)
{
	const auto invoke_id = pressed_id != 0 && pressed_id == id ? id : 0;
	pressed_id = 0;

	// Maximizing or restoring moves the frame under the pointer, so the next move finds the button
	// again rather than this keeping one that may no longer be there.
	if (invoke_id != 0) hover_id = 0;
	return invoke_id;
}

namespace
{
	// Toolbar buttons are named by id to the window's caption-button tracking, so ids are unique
	// across every toolbar a process makes.
	int next_button_id()
	{
		static int next = 1000;
		return ++next;
	}

	core::color32 text_on(const core::color32 text, const core::color32 background, const bool enabled)
	{
		return enabled ? text : core::average(text, background);
	}
}

///////////////////////////////////////////////////////////////////////////////////////////////////
// Buttons: push, default, command link, check and radio
///////////////////////////////////////////////////////////////////////////////////////////////////

namespace ui
{
	class drawn_button final : public button
	{
	public:
		enum class kind
		{
			push,
			check,
			radio
		};

		kind _kind = kind::push;
		icon_index _icon{};
		// As given, with its mnemonic marker, which window_text reports as a native button's did.
		std::string _text;
		// As drawn.
		std::string _label;
		std::string _details;
		std::function<void()> _invoke;
		std::function<void(bool)> _changed;
		bool _checked = false;
		bool _default = false;
		int _group = radio_group_default;
		bool _pressed = false;
		bool _pressed_inside = false;

		drawn_button(const kind k, const bool is_default, const int group) : _kind(k), _default(is_default),
		                                                                     _group(group)
		{
			traits.first_focus_candidate = true;
			traits.push_button = k == kind::push;
			traits.default_button = k == kind::push && is_default;
			traits.radio_group = k == kind::radio ? group : -1;
		}

		void set_text(const std::string_view text)
		{
			_text = text;
			auto parsed = parse_mnemonic(text);
			_label = std::move(parsed.text);
			mnemonic = parsed.key;
		}

		std::string window_text() const { return _text; }

		void window_text(const std::string_view text)
		{
			if (_text == text) return;
			set_text(text);
			invalidate();
		}

		void set_checked(const bool checked)
		{
			if (_checked == checked) return;
			_checked = checked;
			invalidate();
		}

		bool is_checked() const override { return _checked; }

		void check_from_group(const bool checked) override
		{
			set_checked(checked);
		}

		void notify_changed() override
		{
			if (_changed) _changed(_checked);
		}

		void click() override
		{
			if (!enabled) return;

			switch (_kind)
			{
			case kind::push:
				if (_invoke)
				{
					const auto invoke = _invoke;
					invoke();
				}
				break;
			case kind::check:
				_checked = !_checked;
				invalidate();
				notify_changed();
				break;
			case kind::radio:
				if (const auto s = input()) s->pressed_radio(*this);
				break;
			}
		}

		bool on_key(const int key, const platform::key_state keys) override
		{
			if (keys.alt || keys.control) return false;
			if (key == static_cast<int>(platform::keys::SPACE) || (key == static_cast<int>(platform::keys::RETURN) && _kind == kind::push))
			{
				click();
				return true;
			}
			return false;
		}

		void on_mouse_down(const core::pointi loc, const platform::key_state keys) override
		{
			_pressed = true;
			_pressed_inside = true;
			invalidate();
		}

		void on_mouse_move(const core::pointi loc, const bool captured) override
		{
			if (!captured || !_pressed) return;
			const auto inside = bounds.contains(loc);
			if (inside == _pressed_inside) return;
			_pressed_inside = inside;
			invalidate();
		}

		void on_mouse_up(const core::pointi loc, const platform::key_state keys) override
		{
			const auto fire = _pressed && bounds.contains(loc);
			_pressed = false;
			_pressed_inside = false;
			invalidate();
			if (fire) click();
		}

		core::sizei measure_content(render::measure_context& mc, const int cx) const override
		{
			const auto s = mc.scale_factor;
			const auto padding = drawn::scaled(drawn::button_padding, s);
			const auto icon_cxy = drawn::icon_cxy(s);
			auto cx_result = padding * 2;
			auto cy_result = padding * 2;
			auto icon_width = 0;

			if (_icon != icon_index{}) icon_width += icon_cxy + padding;
			if (_kind != kind::push) icon_width += icon_cxy + padding;

			if (_details.empty())
			{
				const auto text = mc.measure_text(_label, render::font_face::dialog, render::text_style::multiline,
				                                  std::max(1, cx - icon_width - padding * 2));
				cx_result += icon_width + text.cx;
				cy_result += text.cy;
			}
			else
			{
				const auto title = mc.measure_text(_label, render::font_face::title, render::text_style::single_line,
				                                   std::max(1, cx - icon_width - padding * 2));
				const auto details = mc.measure_text(_details, render::font_face::dialog,
				                                     render::text_style::multiline,
				                                     std::max(1, cx - padding * 2));
				cx_result += std::max(details.cx, icon_width + title.cx);
				cy_result += title.cy + details.cy + padding;
			}

			return {cx_result, cy_result};
		}

		// Multi-line text is centred vertically in its box, so a block that should sit at the top is
		// drawn in a box its own height.
		static void draw_block(render::draw_context& dc, const std::string_view text, core::recti box, const render::font_face font,
		                       const render::text_style text_style, const core::color c)
		{
			if (text.empty() || box.is_empty()) return;
			const auto extent = dc.measure_text(text, font, text_style, box.width());
			box.bottom = std::min(box.bottom, box.top + extent.cy);
			dc.draw_text(text, box, font, text_style, c, {});
		}

		void paint(render::draw_context& dc, const bool focused) const override
		{
			const auto s = dc.scale_factor;
			const auto alpha = dc.colors.alpha;
			const auto padding = drawn::scaled(drawn::button_padding, s);
			const auto icon_cxy = drawn::icon_cxy(s);
			const auto pressed = _pressed && _pressed_inside;
			auto client = bounds.inflate(-padding, -padding);
			auto clr_bg = style::colors.button_background;
			auto clr_text = style::colors.dialog_text;

			if (_kind != kind::push)
			{
				clr_bg = focused ? style::colors.dialog_selected_background : dc.colors.background;
				if (focused) dc.draw_rect(bounds, drawn::solid(clr_bg, alpha));

				const auto icon = _kind == kind::check
					                  ? (_checked ? chrome_icons().checkbox_on : chrome_icons().checkbox_off)
					                  : (_checked ? chrome_icons().radio_on : chrome_icons().radio_off);
				const auto top = bounds.center().y - icon_cxy / 2;
				const core::recti icon_bounds(bounds.left + padding, top, bounds.left + padding + icon_cxy, top + icon_cxy);

				if (_checked) dc.draw_rect(icon_bounds, drawn::solid(style::colors.dialog_selected_background, alpha));

				drawn::draw_icon(dc, icon, icon_bounds,
				                 drawn::solid(text_on(clr_text, clr_bg, enabled), alpha));
				client.left += icon_cxy + padding;
			}
			else
			{
				if (!enabled)
				{
					clr_bg = core::average(style::colors.dialog_background, clr_bg);
				}
				else if (_default)
				{
					clr_bg = core::average(style::colors.dialog_selected_background, clr_bg);
				}

				if (hovered && enabled && !pressed) clr_bg = core::lighten(clr_bg, 0.1f);

				if (pressed)
				{
					dc.draw_rect(bounds, drawn::solid(style::colors.dialog_selected_background, alpha));
				}
				else
				{
					const auto c2 = core::emphasize(clr_bg, 0.123f);
					drawn::fill_gradient(dc, bounds, clr_bg, c2, alpha);
					drawn::draw_outline(dc, bounds, drawn::solid(core::darken(clr_bg, 0.22f), alpha), std::max(1, core::round(s)));
				}

				if (focused)
				{
					drawn::draw_outline(dc, bounds, drawn::solid(style::colors.dialog_selected_background, alpha),
					                    std::max(1, drawn::scaled(drawn::focus_padding, s)));
				}
			}

			const auto text_color = drawn::solid(text_on(clr_text, clr_bg, enabled), alpha);

			if (_details.empty())
			{
				if (_icon != icon_index{})
				{
					auto r = client;
					r.right = r.left + icon_cxy + padding;
					drawn::draw_icon(dc, _icon, r, text_color);
					client.left += icon_cxy + padding;
				}

				const auto text_style = _kind == kind::push
					                        ? render::text_style::multiline_center
					                        : render::text_style::multiline;
				const auto extent = dc.measure_text(_label, render::font_face::dialog, text_style,
				                                    std::max(1, client.width()));
				const auto top = client.top + std::max(0, (client.height() - extent.cy) / 2);
				dc.draw_text(_label, {client.left, top, client.right, top + extent.cy}, render::font_face::dialog,
				             text_style, text_color, {});
			}
			else
			{
				const auto icon_width = _icon == icon_index{} ? 0 : icon_cxy + padding;
				const auto title = dc.measure_text(_label, render::font_face::title, render::text_style::single_line,
				                                   std::max(1, client.width() - icon_width));
				const auto left = client.left + std::max(0, (client.width() - (title.cx + icon_width)) / 2);
				core::recti title_bounds(left, client.top, left + icon_width + title.cx, client.top + title.cy);

				if (_icon != icon_index{})
				{
					const core::recti icon_bounds(left, client.top + padding / 2, left + icon_cxy,
					                        client.top + padding / 2 + icon_cxy);
					drawn::draw_icon(dc, _icon, icon_bounds, text_color);
					title_bounds.left += icon_width;
				}

				dc.draw_text(_label, title_bounds, render::font_face::title, render::text_style::single_line, text_color,
				             {});

				auto details_bounds = client;
				details_bounds.top = client.top + title.cy + padding;
				draw_block(dc, _details, details_bounds, render::font_face::dialog, render::text_style::multiline_center,
				           text_color);
			}
		}
	};

	///////////////////////////////////////////////////////////////////////////////////////////////////
	// Slider
	///////////////////////////////////////////////////////////////////////////////////////////////////

	class drawn_slider final : public trackbar
	{
	public:
		int _min = 0;
		int _max = 100;
		int _pos = 0;
		std::function<void(int, bool)> _changed;
		bool _tracking = false;

		int handle_size(const double s) const
		{
			const auto* const mc = metrics();
			return std::max(mc ? mc->padding2 : drawn::scaled(8, s), drawn::scaled(12, s));
		}

		core::recti track_bounds(const double s) const
		{
			const auto handle = handle_size(s);
			const auto track_height = std::max(2, drawn::scaled(4, s));
			const auto center_y = bounds.center().y;
			return {
				bounds.left + handle / 2, center_y - track_height / 2,
				bounds.right - handle / 2, center_y + (track_height + 1) / 2
			};
		}

		void set_value(int v, const bool tracking)
		{
			v = std::clamp(v, _min, std::max(_min, _max));
			if (v == _pos) return;
			_pos = v;
			invalidate();
			if (_changed) _changed(_pos, tracking);
		}

		void step_to(const int v)
		{
			const auto before = _pos;
			set_value(v, false);
			// The native slider closed every keyboard step with a final report, as a drag's release does.
			if (_pos == before && _changed) _changed(_pos, false);
		}

		int get_pos() const { return _pos; }

		// As large as its owner makes it.
		core::sizei measure_content(render::measure_context&, int) const override
		{
			return bounds.extent();
		}

		void set_pos(const int val)
		{
			const auto v = std::clamp(val, _min, std::max(_min, _max));
			if (v == _pos) return;
			_pos = v;
			invalidate();
		}

		void on_mouse_down(const core::pointi loc, const platform::key_state keys) override
		{
			_tracking = true;
			const auto track = track_bounds(scale());
			set_value(slider_track_x_to_value(track, loc.x, _min, _max, _pos), true);
			invalidate();
		}

		void on_mouse_move(const core::pointi loc, const bool captured) override
		{
			if (!captured || !_tracking) return;
			const auto track = track_bounds(scale());
			set_value(slider_track_x_to_value(track, loc.x, _min, _max, _pos), true);
		}

		void on_mouse_up(const core::pointi loc, const platform::key_state keys) override
		{
			if (!_tracking) return;
			_tracking = false;
			invalidate();
			// Last, because settling may replace the controls that own this slider.
			if (_changed) _changed(_pos, false);
		}

		bool on_key(const int key, const platform::key_state keys) override
		{
			if (keys.alt || keys.control) return false;

			const auto page = std::max(1, (_max - _min) / 5);
			const auto k = static_cast<char32_t>(key);

			if (k == platform::keys::LEFT || k == platform::keys::UP) step_to(_pos - 1);
			else if (k == platform::keys::RIGHT || k == platform::keys::DOWN) step_to(_pos + 1);
			else if (k == platform::keys::PRIOR) step_to(_pos - page);
			else if (k == platform::keys::NEXT) step_to(_pos + page);
			else if (k == platform::keys::HOME) step_to(_min);
			else if (k == platform::keys::END) step_to(_max);
			else return false;

			return true;
		}

		bool on_wheel(const core::pointi loc, const int delta, const platform::key_state keys) override
		{
			if (!is_focused() || delta == 0) return false;
			step_to(_pos + (delta > 0 ? 1 : -1));
			return true;
		}

		void paint(render::draw_context& dc, const bool focused) const override
		{
			const auto s = dc.scale_factor;
			const auto alpha = dc.colors.alpha;
			const auto handle = handle_size(s);
			const auto track = track_bounds(s);
			const auto center_y = bounds.center().y;
			const auto value = std::clamp(_pos, _min, std::max(_min, _max));
			const auto handle_x = slider_value_to_track_x(track, value, _min, _max);
			const core::recti handle_bounds(handle_x - handle / 2, center_y - handle / 2, handle_x + (handle + 1) / 2,
			                          center_y + (handle + 1) / 2);

			// Drawn from what the host paints behind it, so the handle stands out on any panel: a fixed
			// shade matched the dialog background and vanished there.
			const auto bg = dc.colors.background;
			const auto fg = dc.colors.foreground;
			core::color handle_color;

			if (_tracking) handle_color = drawn::solid(core::lerp(bg, fg, 0.8f));
			else if (focused) handle_color = drawn::solid(style::colors.dialog_selected_background);
			else if (hovered) handle_color = drawn::solid(core::lerp(bg, fg, 0.6f));
			else handle_color = drawn::solid(core::lerp(bg, fg, 0.4f));

			const auto handle_alpha = enabled ? alpha : alpha * 0.4f;
			dc.draw_rounded_rect(track, core::color(0, alpha / 3.33f), dc.padding1);
			dc.draw_rounded_rect(handle_bounds, handle_color.aa(handle_alpha), dc.padding1);
		}
	};

	///////////////////////////////////////////////////////////////////////////////////////////////////
	// Toolbar
	///////////////////////////////////////////////////////////////////////////////////////////////////

	class drawn_toolbar final : public toolbar
	{
	public:
		struct button_state
		{
			command_ptr command;
			int id = 0;
			bool visible = true;
			bool enable = true;
			bool checked = false;
			bool highlight = false;
			icon_index icon{};
			std::string text;
			// Relative to the toolbar's top left.
			core::recti bounds;
		};

		toolbar_styles _styles;
		std::vector<button_state> _buttons;
		int _hot = -1;
		int _pressed = -1;
		bool _pressed_inside = false;
		core::sizei _extent;
		bool _layout_valid = false;
		int _layout_width = -1;
		double _layout_scale = 0.0;

		void init(const toolbar_styles& styles, const std::vector<command_ptr>& buttons)
		{
			// A press on a toolbar button runs its command and leaves the focus where it was.
			traits.focus_on_click = false;
			_styles = styles;

			for (const auto& b : buttons)
			{
				button_state state;
				state.command = b;
				state.id = b ? next_button_id() : 0;
				_buttons.emplace_back(std::move(state));
			}

			snapshot(true);
		}

		// What the native toolbar held: its button state is updated when the owner says so, not read
		// from the commands on every paint.
		void snapshot(const bool text_changed)
		{
			for (auto& b : _buttons)
			{
				if (!b.command) continue;
				const auto& c = *b.command;
				b.visible = c.visible;
				b.enable = c.enable;
				b.checked = c.checked;
				b.highlight = c.highlight;
				if (text_changed || c.text_can_change) b.text = c.toolbar_text;
				if (text_changed || c.icon_can_change) b.icon = c.icon;
			}
		}

		core::sizei button_size(render::measure_context& mc, const button_state& b, const double s) const
		{
			if (!_styles.button_extent.is_empty())
			{
				return {drawn::scaled(_styles.button_extent.cx, s), drawn::scaled(_styles.button_extent.cy, s)};
			}

			const auto pad = drawn::scaled(6, s);
			const auto gap = drawn::scaled(4, s);
			const auto icon_cxy = drawn::icon_cxy(s);
			const auto has_icon = b.icon != icon_index{};
			const auto line_height = mc.text_line_height(render::font_face::dialog);
			const auto text_cx = b.text.empty()
				                     ? 0
				                     : mc.measure_text(b.text, render::font_face::dialog,
				                                       render::text_style::single_line, 10000).cx;
			const auto content = (has_icon ? icon_cxy : 0) + (has_icon && text_cx > 0 ? gap : 0) + text_cx;
			return {pad + content + pad, std::max(icon_cxy, line_height) + drawn::scaled(10, s)};
		}

		// Places the buttons by mc, which measures their text: whoever asks for the arrangement has one, where
		// the toolbar's frame may not yet have found it.
		void arrange(const int cx, render::measure_context& mc)
		{
			const auto s = mc.scale_factor;
			const auto wrap = _styles.wrap;
			if (_layout_valid && _layout_scale == s && (!wrap || _layout_width == cx)) return;

			const auto separator = drawn::scaled(8, s);
			auto x = 0;
			auto y = 0;
			auto row_height = 0;
			auto max_x = 0;

			for (auto& b : _buttons)
			{
				if (!b.command)
				{
					// A separator takes its room even beside hidden buttons, as the native toolbar's did.
					b.bounds = {x, y, x + separator, y};
					x += separator;
					max_x = std::max(max_x, x);
					continue;
				}

				if (!b.visible)
				{
					b.bounds = {};
					continue;
				}

				const auto size = button_size(mc, b, s);

				if (wrap && x > 0 && x + size.cx > cx)
				{
					x = 0;
					y += row_height;
					row_height = 0;
				}

				b.bounds = {x, y, x + size.cx, y + size.cy};
				x += size.cx;
				row_height = std::max(row_height, size.cy);
				max_x = std::max(max_x, x);
			}

			// Separators span the row they sit in.
			for (auto& b : _buttons)
			{
				if (!b.command) b.bounds.bottom = b.bounds.top + row_height;
			}

			_extent = {max_x, y + row_height};
			_layout_valid = true;
			_layout_scale = s;
			_layout_width = cx;
		}

		core::recti button_rect(const size_t i) const
		{
			return _buttons[i].bounds.offset(bounds.top_left());
		}

		int button_at(const core::pointi loc) const
		{
			for (size_t i = 0; i < _buttons.size(); ++i)
			{
				const auto& b = _buttons[i];
				if (b.command && b.visible && button_rect(i).contains(loc)) return static_cast<int>(i);
			}
			return -1;
		}

		bool can_activate(const int i) const
		{
			if (i < 0 || i >= static_cast<int>(_buttons.size())) return false;
			const auto& b = _buttons[static_cast<size_t>(i)];
			return b.command && b.visible && b.enable;
		}

		void set_hot(const int i)
		{
			if (_hot == i) return;
			if (_hot >= 0 && _hot < static_cast<int>(_buttons.size())) invalidate(button_rect(static_cast<size_t>(_hot)));
			_hot = i;

			core::recti screen_bounds;
			command_ptr command;

			if (i >= 0)
			{
				invalidate(button_rect(static_cast<size_t>(i)));
				screen_bounds = button_rect(static_cast<size_t>(i)).offset(screen_offset());
				command = _buttons[static_cast<size_t>(i)].command;
			}

			if (auto* const h = host())
			{
				if (const auto fh = h->listener()) fh->command_hover(command, screen_bounds);
			}
		}

		void show_menu(const size_t i)
		{
			const auto command = _buttons[i].command;
			if (!command || !command->menu) return;

			const auto screen_bounds = button_rect(i).offset(screen_offset());
			_pressed = static_cast<int>(i);
			_pressed_inside = true;
			invalidate(button_rect(i));

			const auto self = shared_from_this();
			const auto commands = command->menu();
			if (const auto s = input()) s->track_menu(screen_bounds, commands);

			_pressed = -1;
			_pressed_inside = false;
			invalidate();
		}

		void activate(const size_t i)
		{
			const auto command = _buttons[i].command;
			if (!command) return;

			if (command->menu)
			{
				show_menu(i);
			}
			else if (command->invoke)
			{
				const auto invoke = command->invoke;
				invoke();
			}
		}

		// toolbar
		core::sizei measure_toolbar(render::measure_context& mc, const int cx)
		{
			arrange(cx, mc);
			return _extent;
		}

		void update_button_state(const bool resize, const bool text_changed)
		{
			snapshot(text_changed);
			if (resize || text_changed) _layout_valid = false;
			invalidate();
		}

		core::recti button_bounds(const command_ptr& command) const
		{
			for (size_t i = 0; i < _buttons.size(); ++i)
			{
				if (_buttons[i].command == command) return button_rect(i).offset(screen_offset());
			}
			return {};
		}

		core::sizei measure_content(render::measure_context& mc, const int cx) const override
		{
			return const_cast<drawn_toolbar*>(this)->measure_toolbar(mc, cx);
		}

		// control

		bool can_focus() const override
		{
			if (!enabled) return false;
			for (size_t i = 0; i < _buttons.size(); ++i) if (can_activate(static_cast<int>(i))) return true;
			return false;
		}

		void scale_changed() override
		{
			_layout_valid = false;
		}

		void on_placed(render::measure_context& mc) override
		{
			arrange(bounds.width(), mc);
		}

		void on_focus(const bool focused, const bool select_all) override
		{
			if (!focused)
			{
				set_hot(-1);
				return;
			}

			if (!can_activate(_hot))
			{
				for (size_t i = 0; i < _buttons.size(); ++i)
				{
					if (can_activate(static_cast<int>(i)))
					{
						set_hot(static_cast<int>(i));
						break;
					}
				}
			}
		}

		bool on_key(const int key, const platform::key_state keys) override
		{
			if (keys.alt || keys.control) return false;
			const auto k = static_cast<char32_t>(key);

			if (k == platform::keys::LEFT || k == platform::keys::RIGHT)
			{
				const auto count = static_cast<int>(_buttons.size());
				const auto step = k == platform::keys::LEFT ? -1 : 1;
				auto i = _hot;

				for (auto n = 0; n < count; ++n)
				{
					i = (i + step + count) % count;
					if (can_activate(i))
					{
						set_hot(i);
						break;
					}
				}

				return true;
			}

			if ((k == platform::keys::RETURN || k == platform::keys::SPACE || k == platform::keys::DOWN) && can_activate(_hot))
			{
				const auto i = static_cast<size_t>(_hot);
				if (k == platform::keys::DOWN && !_buttons[i].command->menu) return false;
				activate(i);
				return true;
			}

			return false;
		}

		void on_mouse_move(const core::pointi loc, const bool captured) override
		{
			const auto i = button_at(loc);

			if (captured && _pressed >= 0)
			{
				const auto inside = i == _pressed;
				if (inside != _pressed_inside)
				{
					_pressed_inside = inside;
					invalidate(button_rect(static_cast<size_t>(_pressed)));
				}
				return;
			}

			set_hot(i);
		}

		void on_mouse_leave() override
		{
			if (!is_focused()) set_hot(-1);
		}

		void on_mouse_down(const core::pointi loc, const platform::key_state keys) override
		{
			const auto i = button_at(loc);
			if (!can_activate(i)) return;

			const auto index = static_cast<size_t>(i);

			// A button with a menu drops it on the press, as the native drop-down button did.
			if (_buttons[index].command->menu)
			{
				show_menu(index);
				return;
			}

			_pressed = i;
			_pressed_inside = true;
			invalidate(button_rect(index));
		}

		void on_mouse_up(const core::pointi loc, const platform::key_state keys) override
		{
			if (_pressed < 0) return;

			const auto index = static_cast<size_t>(_pressed);
			const auto fire = button_at(loc) == _pressed && can_activate(_pressed);
			_pressed = -1;
			_pressed_inside = false;
			invalidate(button_rect(index));
			if (fire) activate(index);
		}

		int caption_button_at(const core::pointi loc) const override
		{
			if (!enabled) return 0;

			for (size_t i = 0; i < _buttons.size(); ++i)
			{
				const auto& b = _buttons[i];
				if (b.command && b.visible && b.enable && b.command->caption_maximize && button_rect(i).contains(loc))
				{
					return b.id;
				}
			}

			return 0;
		}

		bool has_caption_button(const int id) const override
		{
			return std::ranges::any_of(_buttons, [id](const button_state& b) { return b.command && b.id == id; });
		}

		void invoke_caption(const int id) override
		{
			for (size_t i = 0; i < _buttons.size(); ++i)
			{
				if (_buttons[i].command && _buttons[i].id == id)
				{
					if (can_activate(static_cast<int>(i))) activate(i);
					return;
				}
			}
		}

		void paint(render::draw_context& dc, const bool focused) const override
		{
			const_cast<drawn_toolbar*>(this)->arrange(bounds.width(), dc);

			const auto s = dc.scale_factor;
			const auto alpha = dc.colors.alpha;
			const auto set_ptr = input();
			const auto caption = set_ptr ? set_ptr->caption() : caption_button_tracker{};
			const auto bg_clr = dc.colors.background;
			const auto text_clr = dc.colors.foreground;
			const auto selected_clr = dc.colors.selected;
			const auto line = std::max(1, core::round(s));
			const auto pad = drawn::scaled(6, s);
			const auto gap = drawn::scaled(4, s);
			const auto icon_cxy = drawn::icon_cxy(s);

			for (size_t i = 0; i < _buttons.size(); ++i)
			{
				const auto& b = _buttons[i];
				if (!b.command || !b.visible) continue;

				const auto r = button_rect(i);
				const auto index = static_cast<int>(i);
				const auto is_disabled = !b.enable || !enabled;
				const auto is_hot = !is_disabled && (index == _hot || caption.hover_id == b.id);
				const auto is_focus = is_hot && focused;
				const auto is_pressed = !is_disabled &&
					((index == _pressed && _pressed_inside) || (caption.pressed_id != 0 && caption.pressed_id == b.id));
				const auto is_highlight = b.highlight && !is_disabled;
				const auto accent_bg = style::colors.important_background;
				const auto clr_normal_bg = is_highlight ? accent_bg : bg_clr;
				const auto clr_checked_bg = core::darken(clr_normal_bg, 0.22f);
				const auto clr_selected_bg = is_highlight ? core::darken(accent_bg, 0.22f) : selected_clr;
				const auto clr_hover_bg = core::lighten(clr_normal_bg, 0.33f);
				auto draw_clr = text_clr;

				if (is_disabled)
				{
					draw_clr = core::average(text_clr, bg_clr);
				}
				else if (is_pressed || is_focus)
				{
					dc.draw_rect(r, drawn::solid(clr_selected_bg, alpha));
					drawn::draw_outline(dc, r, drawn::solid(core::emphasize(clr_selected_bg), alpha), line);
				}
				else if (is_hot)
				{
					dc.draw_rect(r, drawn::solid(clr_hover_bg, alpha));
					drawn::draw_outline(dc, r, drawn::solid(core::emphasize(clr_hover_bg), alpha), line);
				}
				else if (b.checked)
				{
					dc.draw_rect(r, drawn::solid(clr_checked_bg, alpha));
					drawn::draw_outline(dc, r, drawn::solid(core::emphasize(clr_checked_bg), alpha), line);
				}
				else if (is_highlight)
				{
					dc.draw_rect(r, drawn::solid(clr_normal_bg, alpha));
				}

				const auto c = drawn::solid(draw_clr, alpha);
				const auto has_icon = b.icon != icon_index{};

				if (b.text.empty())
				{
					drawn::draw_icon(dc, b.icon, r, c);
				}
				else
				{
					auto x = r.left + pad;

					if (has_icon)
					{
						const core::recti icon_bounds(x, r.top, x + icon_cxy, r.bottom);
						drawn::draw_icon(dc, b.icon, icon_bounds, c);
						x = icon_bounds.right + gap;
					}
					else
					{
						const auto text_cx = dc.measure_text(b.text, render::font_face::dialog,
						                                     render::text_style::single_line, 10000).cx;
						x = r.left + std::max(0, (r.width() - text_cx) / 2);
					}

					dc.draw_text(b.text, {x, r.top, r.right, r.bottom}, render::font_face::dialog,
					             render::text_style::single_line, c, {});
				}
			}
		}
	};

	///////////////////////////////////////////////////////////////////////////////////////////////////
	// Date field
	///////////////////////////////////////////////////////////////////////////////////////////////////

	// The order the display locale writes a date in, and the separator it writes between the parts,
	// read from how the platform formats a date whose parts cannot be mistaken for one another.
	static date_field_order locale_date_order(std::string& separator)
	{
		separator = "/";
		const auto text = platform::format_date(core::date_t(2033, 11, 22, 0, 0, 0));
		auto y = text.find("2033");
		auto y_len = 4;
		if (y == std::string::npos)
		{
			y = text.find("33");
			y_len = 2;
		}
		const auto m = text.find("11");
		const auto d = text.find("22");

		if (y == std::string::npos || m == std::string::npos || d == std::string::npos) return date_field_order::dmy;

		const auto first = std::min({y, m, d});
		const auto first_len = first == y ? static_cast<size_t>(y_len) : 2;
		const auto second = std::min({y == first ? std::string::npos : y, m == first ? std::string::npos : m,
		                              d == first ? std::string::npos : d});

		if (second != std::string::npos && second > first + first_len)
		{
			const auto between = text.substr(first + first_len, second - first - first_len);
			if (!between.empty() && between.size() <= 3) separator = between;
		}

		if (y < m && m < d) return date_field_order::ymd;
		if (m < d && d < y) return date_field_order::mdy;
		return date_field_order::dmy;
	}

	class drawn_date_time final : public date_time_control
	{
	public:
		date_edit_model _model;
		std::function<void(core::date_t)> _changed;
		bool _include_time = false;
		std::string _separator;
		mutable std::vector<core::recti> _segment_bounds;

		drawn_date_time(const date_field_order order, const bool include_time) : _model(order, include_time),
		                                                                         _include_time(include_time)
		{
			traits.typing = true;
		}

		size_t date_segment_count() const
		{
			return std::min<size_t>(3, _model.segments().size());
		}

		std::string separator_after(const size_t i) const
		{
			const auto count = _model.segments().size();
			if (i + 1 >= count) return {};
			if (i + 1 < date_segment_count()) return _separator;
			if (i + 1 == date_segment_count()) return " ";
			return ":";
		}

		std::string display_text(std::vector<size_t>* offsets) const
		{
			std::string result;

			for (size_t i = 0; i < _model.segments().size(); ++i)
			{
				if (offsets) offsets->push_back(result.size());
				result += _model.text(i);
				if (offsets) offsets->push_back(result.size());
				result += separator_after(i);
			}

			return result;
		}

		core::recti inner(const double s) const
		{
			const auto padding = drawn::scaled(4, s);
			return bounds.inflate(-padding, 0);
		}

		void update_segment_bounds(render::measure_context& mc) const
		{
			std::vector<size_t> offsets;
			const auto text = display_text(&offsets);
			const auto in = inner(mc.scale_factor);
			std::vector<int> xs;

			if (const auto layout = mc.create_text_layout(render::font_face::dialog))
			{
				layout->update(text, render::text_style::single_line);
				xs = layout->offset_xs(offsets, 10000);
			}

			if (xs.size() != offsets.size())
			{
				xs.clear();
				for (const auto o : offsets)
				{
					xs.push_back(o == 0
						             ? 0
						             : mc.measure_text(std::string_view(text).substr(0, o), render::font_face::dialog,
						                               render::text_style::single_line, 10000).cx);
				}
			}

			_segment_bounds.clear();
			for (size_t i = 0; i + 1 < xs.size(); i += 2)
			{
				_segment_bounds.emplace_back(in.left + xs[i], in.top, in.left + xs[i + 1], in.bottom);
			}
		}

		size_t segment_at(const core::pointi loc) const
		{
			if (auto* const mc = metrics()) update_segment_bounds(*mc);
			if (_segment_bounds.empty()) return 0;

			auto best = size_t{0};
			auto best_distance = std::numeric_limits<int>::max();

			for (size_t i = 0; i < _segment_bounds.size(); ++i)
			{
				const auto& r = _segment_bounds[i];
				const auto distance = loc.x < r.left ? r.left - loc.x : loc.x > r.right ? loc.x - r.right : 0;
				if (distance < best_distance)
				{
					best = i;
					best_distance = distance;
				}
			}

			return best;
		}

		void notify()
		{
			invalidate();
			if (_changed) _changed(_model.value());
		}

		void month_menu()
		{
			if (!_model.active_has_menu()) return;

			std::vector<command_ptr> commands;
			const auto current = _model.field(date_segment::month);

			for (auto m = 1; m <= 12; ++m)
			{
				auto c = std::make_shared<command>();
				c->text = core::month(m, true);
				c->checked = m == current;
				c->invoke = [weak = std::weak_ptr(std::static_pointer_cast<drawn_date_time>(shared_from_this())), m]
				{
					if (const auto self = weak.lock())
					{
						self->_model.field(date_segment::month, m);
						self->notify();
					}
				};
				commands.emplace_back(std::move(c));
			}

			const auto index = _model.active();
			auto anchor = bounds;
			if (auto* const mc = metrics()) update_segment_bounds(*mc);
			if (index < _segment_bounds.size()) anchor = _segment_bounds[index];
			anchor.top = bounds.top;
			anchor.bottom = bounds.bottom;

			if (const auto s = input()) s->track_menu(anchor.offset(screen_offset()), commands);
		}

		// date_time_control
		core::sizei measure_content(render::measure_context& mc, const int cx) const override
		{
			const auto s = mc.scale_factor;
			const auto cx_min = drawn::scaled(160, s);
			const auto line_height = mc.text_line_height(render::font_face::dialog);
			return {_include_time ? cx : std::min(cx_min, cx), line_height + drawn::scaled(8, s)};
		}

		std::string window_text() const
		{
			return display_text(nullptr);
		}

		// control

		void on_focus(const bool focused, const bool select_all) override
		{
			if (!focused) _model.commit_typing();
		}

		bool on_key(const int key, const platform::key_state keys) override
		{
			const auto k = static_cast<char32_t>(key);

			if ((keys.alt && k == platform::keys::DOWN) || k == platform::keys::F4)
			{
				month_menu();
				return true;
			}

			if (keys.alt || keys.control) return false;

			if (k == platform::keys::LEFT || k == platform::keys::RIGHT)
			{
				_model.move(k == platform::keys::LEFT ? -1 : 1);
				invalidate();
				return true;
			}

			if (k == platform::keys::TAB)
			{
				if (!_model.move(keys.shift ? -1 : 1)) return false;
				invalidate();
				return true;
			}

			if (k == platform::keys::UP || k == platform::keys::DOWN)
			{
				_model.step(k == platform::keys::UP ? 1 : -1);
				notify();
				return true;
			}

			if (k == platform::keys::HOME || k == platform::keys::END)
			{
				_model.active(k == platform::keys::HOME ? 0 : _model.segments().size() - 1);
				invalidate();
				return true;
			}

			if (k == platform::keys::BACK || k == platform::keys::DEL)
			{
				_model.commit_typing();
				invalidate();
				return true;
			}

			return false;
		}

		bool on_text(const std::string_view text) override
		{
			auto handled = false;

			for (const auto c : text)
			{
				if (c >= '0' && c <= '9')
				{
					_model.type_digit(c);
					notify();
					handled = true;
				}
				else if (c == ' ' || c == ':' || _separator.find(c) != std::string::npos)
				{
					_model.commit_typing();
					_model.move(1);
					invalidate();
					handled = true;
				}
			}

			return handled;
		}

		bool on_wheel(const core::pointi loc, const int delta, const platform::key_state keys) override
		{
			if (!is_focused() || delta == 0) return false;
			_model.step(delta > 0 ? 1 : -1);
			notify();
			return true;
		}

		void on_mouse_down(const core::pointi loc, const platform::key_state keys) override
		{
			const auto index = segment_at(loc);
			const auto reopen = _model.active() == index && _model.active_has_menu();
			_model.active(index);
			invalidate();
			if (reopen && is_focused()) month_menu();
		}

		platform::cursor cursor_at(const core::pointi loc) const override
		{
			return platform::cursor::normal;
		}

		void paint(render::draw_context& dc, const bool focused) const override
		{
			const auto s = dc.scale_factor;
			const auto alpha = dc.colors.alpha;
			const auto bg = enabled ? style::colors.edit_background
				                : core::average(style::colors.edit_background, style::colors.dialog_background);
			const auto fg = text_on(style::colors.edit_text, bg, enabled);

			if (focused)
			{
				const auto edge = drawn::scaled(drawn::focus_padding, s);
				dc.draw_rect(bounds, drawn::solid(style::colors.dialog_selected_background, alpha));
				dc.draw_rect(bounds.inflate(-edge), drawn::solid(bg, alpha));
			}
			else
			{
				dc.draw_rect(bounds, drawn::solid(bg, alpha));
			}

			update_segment_bounds(dc);

			if (focused && _model.active() < _segment_bounds.size())
			{
				const auto line_height = dc.text_line_height(render::font_face::dialog);
				auto r = _segment_bounds[_model.active()];
				r.top = bounds.center().y - line_height / 2;
				r.bottom = r.top + line_height;
				dc.draw_rect(r, drawn::solid(style::colors.dialog_selected_background, alpha));
			}

			const auto in = inner(s);
			dc.draw_text(display_text(nullptr), in, render::font_face::dialog, render::text_style::single_line,
			             drawn::solid(fg, alpha), {});
		}
	};
}

///////////////////////////////////////////////////////////////////////////////////////////////////
// Making controls, apart from any frame
///////////////////////////////////////////////////////////////////////////////////////////////////

ui::edit_ptr ui::make_edit(const edit_styles& styles, const std::string_view text,
                           std::function<void(const std::string&)> changed)
{
	return make_drawn_edit(styles, text, std::move(changed));
}

ui::trackbar_ptr ui::make_slider(const int min, const int max, std::function<void(int, bool)> changed)
{
	auto c = std::make_shared<drawn_slider>();
	c->_min = min;
	c->_max = std::max(min, max);
	c->_pos = min;
	c->_changed = std::move(changed);
	return c;
}

ui::toolbar_ptr ui::make_toolbar(const toolbar_styles& styles, const std::vector<command_ptr>& buttons)
{
	auto c = std::make_shared<drawn_toolbar>();
	c->init(styles, buttons);
	return c;
}

ui::button_ptr ui::make_button(const std::string_view text, std::function<void()> invoke, const bool default_button)
{
	return make_button(icon_index{}, text, {}, std::move(invoke), default_button);
}

ui::button_ptr ui::make_button(const icon_index icon, const std::string_view title, const std::string_view details,
                               std::function<void()> invoke, const bool default_button)
{
	auto c = std::make_shared<drawn_button>(drawn_button::kind::push, default_button, radio_group_default);
	c->_icon = icon;
	c->set_text(title);
	c->_details = details;
	c->_invoke = std::move(invoke);
	return c;
}

ui::button_ptr ui::make_check_button(const bool val, const std::string_view text, const bool is_radio,
                                     std::function<void(bool)> changed, const int radio_group)
{
	auto c = std::make_shared<drawn_button>(is_radio ? drawn_button::kind::radio : drawn_button::kind::check,
	                                        false, radio_group);
	c->set_text(text);
	c->_checked = val;
	c->_changed = std::move(changed);
	return c;
}

ui::date_time_control_ptr ui::make_date_time_control(const core::date_t val, std::function<void(core::date_t)> changed,
                                                     const bool include_time)
{
	std::string separator;
	const auto order = locale_date_order(separator);
	auto c = std::make_shared<drawn_date_time>(order, include_time);
	c->_separator = separator;
	// The native picker showed the current time for a value it could not hold.
	c->_model.value(val.is_valid() ? val : platform::utc_to_local(core::utc_now()));
	c->_changed = std::move(changed);
	return c;
}

///////////////////////////////////////////////////////////////////////////////////////////////////
// What every control answers, and each kind's own, reaching its one implementation directly
///////////////////////////////////////////////////////////////////////////////////////////////////

void ui::control::focus()
{
	if (input()) take_focus(false);
	else _focus_requested = true;
}

bool ui::control::has_focus() const
{
	return is_focused();
}

core::recti ui::control::window_bounds() const
{
	const auto r = frame_bounds();
	auto* const h = host();
	return h ? core::recti(h->client_to_screen(r.top_left()), r.extent()) : r;
}

void ui::control::options_changed()
{
	scale_changed();
	invalidate();
}

core::sizei ui::control::measure(render::measure_context& mc, const int width_limit) const
{
	const auto extent = measure_content(mc, width_limit) + room * mc.scale_factor;
	return {std::max(min_share > 0 ? width_limit / min_share : 0, extent.cx), extent.cy};
}

void ui::control::layout(render::measure_context& mc, const core::recti bounds_in)
{
	if (bounds == bounds_in) return;
	invalidate();
	bounds = bounds_in;
	on_placed(mc);
	invalidate();
}

// Drawn where the tree draws it, so whatever its host draws over the tree after it is over the control
// too. The control draws in its own coordinates, which a context moves to where they sit.
void ui::control::render(render::draw_context& dc, const core::pointi element_offset) const
{
	_origin = element_offset;
	if (bounds.is_empty()) return;

	const auto clip = dc.clip_bounds();
	if (!clip.is_empty() && !clip.intersects(bounds.offset(element_offset))) return;

	const auto focused = is_focused();

	if (traits.local_coordinates)
	{
		offset_draw_context moved(dc, origin());
		render::scoped_clip clip(moved, core::recti(bounds.extent()));
		paint(moved, focused);
	}
	else if (element_offset == core::pointi{})
	{
		render::scoped_clip clip(dc, bounds);
		paint(dc, focused);
	}
	else
	{
		offset_draw_context moved(dc, element_offset);
		render::scoped_clip clip(moved, bounds);
		paint(moved, focused);
	}
}

void ui::control::enabled_changed()
{
	const auto now = is_enabled() && ancestors_enabled;
	if (enabled == now) return;
	enabled = now;

	// A control that takes no input gives up the focus, to the window it is in.
	if (!enabled)
	{
		if (const auto in = input(); in && in->focused().get() == this) in->clear_focus();
	}

	invalidate();
}

void ui::control::found(const input_manager_ptr& in, const core::pointi origin, const bool ancestors_are_enabled)
{
	_input = in;
	_origin = origin;

	if (ancestors_enabled != ancestors_are_enabled)
	{
		ancestors_enabled = ancestors_are_enabled;
		enabled_changed();
	}
}

void ui::control::lost(const input_manager* in)
{
	if (_input.lock().get() == in) _input.reset();
}

bool ui::control::take_focus_request()
{
	const auto requested = _focus_requested;
	_focus_requested = false;
	return requested;
}

ui::control_host* ui::control::host() const
{
	const auto in = input();
	return in ? in->host() : nullptr;
}

render::measure_context* ui::control::metrics() const
{
	auto* const h = host();
	return h ? h->measure() : nullptr;
}

double ui::control::scale() const
{
	const auto* const mc = metrics();
	return mc ? mc->scale_factor : 1.0;
}

void ui::control::invalidate() const
{
	if (const auto in = input()) in->invalidate(frame_bounds());
}

void ui::control::invalidate(const core::recti r) const
{
	if (const auto in = input()) in->invalidate(r.offset(origin()));
}

bool ui::control::is_focused() const
{
	const auto in = input();
	return in && in->has_focus(*this);
}

void ui::control::take_focus(const bool select_all)
{
	if (const auto in = input()) in->focus(this, select_all);
}

core::pointi ui::control::screen_offset() const
{
	auto* const h = host();
	return h ? h->client_to_screen(origin()) : origin();
}

std::string ui::button::window_text() const
{
	return static_cast<const drawn_button*>(this)->window_text();
}

void ui::button::window_text(const std::string_view text)
{
	static_cast<drawn_button*>(this)->window_text(text);
}

void ui::button::set_checked(const bool checked)
{
	static_cast<drawn_button*>(this)->set_checked(checked);
}

int ui::trackbar::get_pos() const
{
	return static_cast<const drawn_slider*>(this)->get_pos();
}

void ui::trackbar::set_pos(const int val)
{
	static_cast<drawn_slider*>(this)->set_pos(val);
}

core::sizei ui::toolbar::measure_toolbar(render::measure_context& mc, const int cx)
{
	return static_cast<drawn_toolbar*>(this)->measure_toolbar(mc, cx);
}

void ui::toolbar::update_button_state(const bool resize, const bool text_changed)
{
	static_cast<drawn_toolbar*>(this)->update_button_state(resize, text_changed);
}

core::recti ui::toolbar::button_bounds(const command_ptr& command) const
{
	return static_cast<const drawn_toolbar*>(this)->button_bounds(command);
}

std::string ui::date_time_control::window_text() const
{
	return static_cast<const drawn_date_time*>(this)->window_text();
}

///////////////////////////////////////////////////////////////////////////////////////////////////
// Walking the tree
///////////////////////////////////////////////////////////////////////////////////////////////////

namespace
{
	void walk_element(ui::element& e, const core::pointi offset, const bool visible, const bool enabled,
	                  const bool with_hidden, const std::function<void(ui::element&, core::pointi, bool, bool)>& f)
	{
		const auto shown = visible && e.is_visible();
		if (!shown && !with_hidden) return;

		f(e, offset, shown, enabled);

		const auto below_offset = offset + e.children_offset();
		const auto below_enabled = enabled && e.is_enabled();
		e.visit_children([&](const ui::element_ptr& c)
		{
			if (c) walk_element(*c, below_offset, shown, below_enabled, with_hidden, f);
		});
	}
}

void ui::walk_tree(const element_ptr& root, const core::pointi offset, const bool with_hidden,
                   const std::function<void(element&, core::pointi, bool, bool)>& f)
{
	if (root) walk_element(*root, offset, true, true, with_hidden, f);
}

///////////////////////////////////////////////////////////////////////////////////////////////////
// The input manager
///////////////////////////////////////////////////////////////////////////////////////////////////

ui::input_manager::input_manager(control_host* host) : _host(host)
{
}

ui::input_manager::~input_manager() = default;

void ui::input_manager::detach()
{
	if (_caret_timer && _host) _host->caret_timer(false, 0);
	_caret_timer = false;

	// An owner can still hold a control; it only loses the way back to this window.
	for (const auto& c : find_controls(true)) c->lost(this);

	_host = nullptr;
	_focused.reset();
	_hover.reset();
	_captured.reset();
	_system_control.reset();
	_keyboard_focus = false;
}

std::vector<ui::control_ptr> ui::input_manager::find_controls(const bool with_hidden) const
{
	std::vector<control_ptr> result;
	if (!_host) return result;

	const auto h = _host->listener();
	if (!h) return result;

	const auto self = std::const_pointer_cast<input_manager>(shared_from_this());

	h->visit_elements([&](const element_ptr& root, const core::pointi offset)
	{
		walk_tree(root, offset, with_hidden, [&](element& e, const core::pointi origin, bool, const bool ancestors_enabled)
		{
			if (e.kind != element_kind::control) return;
			auto& c = static_cast<control&>(e);
			c.found(self, origin, ancestors_enabled);
			result.push_back(c.shared_from_this());
		});
	});

	return result;
}

std::vector<ui::input_manager::frame_placement> ui::input_manager::layout()
{
	std::vector<frame_placement> frames;
	if (!_host) return frames;

	const auto h = _host->listener();
	if (!h) return frames;

	std::vector<control_ptr> requested;
	auto focused_shown = false;
	auto hover_shown = false;
	auto captured_shown = false;
	const auto focused = _focused.lock();
	const auto hover = _hover.lock();
	const auto captured = _captured.lock();
	const auto self = shared_from_this();

	h->visit_elements([&](const element_ptr& root, const core::pointi offset)
	{
		walk_tree(root, offset, true, [&](element& e, const core::pointi origin, const bool visible,
		                                  const bool ancestors_enabled)
		{
			if (e.kind == element_kind::frame)
			{
				if (auto child = static_cast<frame_element&>(e).placed_frame())
				{
					frames.push_back({std::move(child), e.bounds.offset(origin), visible && !e.bounds.is_empty()});
				}
			}
			else if (e.kind == element_kind::control)
			{
				auto& c = static_cast<control&>(e);
				c.found(self, origin, ancestors_enabled);

				if (visible)
				{
					if (&c == focused.get()) focused_shown = true;
					if (&c == hover.get()) hover_shown = true;
					if (&c == captured.get()) captured_shown = true;
					if (c.take_focus_request()) requested.push_back(c.shared_from_this());
				}
			}
		});
	});

	// What is no longer shown lets go of what it held.
	if (captured && !captured_shown) release_capture();
	if (hover && !hover_shown) set_hover(nullptr);
	if (focused && !focused_shown) focus(nullptr, false);
	release_gone_focus();

	for (const auto& c : requested)
	{
		if (c->can_focus())
		{
			focus(c.get(), false);
			break;
		}
	}

	// A caret that moved takes an input method's window with it.
	update_text_input();
	return frames;
}

void ui::input_manager::invalidate(const core::recti bounds) const
{
	if (_host && !bounds.is_empty()) _host->invalidate(bounds);
}

void ui::input_manager::refresh()
{
	(void)find_controls(true);
	release_gone_focus();
}

void ui::input_manager::release_gone_focus()
{
	// An expired pointer that was given a control still holds its ownership block; one never given a control
	// holds none, so it is ordered before nothing.
	const std::weak_ptr<control> none;
	const auto was_given = _focused.owner_before(none) || none.owner_before(_focused);
	if (!_focused.expired() || !was_given) return;

	_focused.reset();
	if (_keyboard_focus && _host) _host->keyboard_to_frame();
	update_text_input();
}

ui::control_ptr ui::input_manager::control_at(const core::pointi loc) const
{
	control_ptr result;

	// The last one drawn there is the one on top.
	for (const auto& c : find_controls(false))
	{
		if (c->frame_bounds().contains(loc)) result = c;
	}

	return result;
}

bool ui::input_manager::hit(const core::pointi loc) const
{
	return control_at(loc) != nullptr;
}

void ui::input_manager::set_hover(const control_ptr& c)
{
	const auto previous = _hover.lock();
	if (previous == c) return;

	if (previous)
	{
		previous->hovered = false;
		previous->on_mouse_leave();
		previous->invalidate();
	}

	_hover = c;

	if (c)
	{
		c->hovered = true;
		c->invalidate();
	}
}

void ui::input_manager::release_capture()
{
	if (_captured.expired()) return;
	_captured.reset();
	if (_host) _host->capture_pointer(false);
}

bool ui::input_manager::mouse_move(const core::pointi loc)
{
	if (const auto captured = _captured.lock())
	{
		const auto local = loc - captured->origin();
		captured->on_mouse_move(local, true);
		if (_host) _host->set_cursor(captured->cursor_at(local));
		return true;
	}

	const auto c = control_at(loc);
	set_hover(c);
	if (!c) return false;

	const auto local = loc - c->origin();
	if (_host) _host->set_cursor(c->cursor_at(local));
	c->on_mouse_move(local, false);
	return true;
}

bool ui::input_manager::mouse_down(const core::pointi loc, const platform::key_state keys)
{
	cancel_system_mode();
	const auto c = control_at(loc);
	if (!c) return false;
	if (!c->enabled) return true;

	const auto had_focus = has_focus(*c);
	if (c->traits.focus_on_click && c->can_focus()) focus(c.get(), false);
	if (!_host) return true;

	_captured = c;
	_host->capture_pointer(true);
	c->click_took_focus = !had_focus && has_focus(*c);
	c->on_mouse_down(loc - c->origin(), keys);
	c->click_took_focus = false;
	return true;
}

bool ui::input_manager::mouse_up(const core::pointi loc, const platform::key_state keys)
{
	if (const auto captured = _captured.lock())
	{
		release_capture();
		captured->on_mouse_up(loc - captured->origin(), keys);
		return true;
	}

	return control_at(loc) != nullptr;
}

bool ui::input_manager::mouse_double_click(const core::pointi loc, const platform::key_state keys)
{
	cancel_system_mode();
	const auto c = control_at(loc);
	if (!c) return false;
	if (!c->enabled) return true;

	const auto had_focus = has_focus(*c);
	if (c->traits.focus_on_click && c->can_focus()) focus(c.get(), false);
	if (!_host) return true;

	_captured = c;
	_host->capture_pointer(true);
	c->click_took_focus = !had_focus && has_focus(*c);
	c->on_double_click(loc - c->origin(), keys);
	c->click_took_focus = false;
	return true;
}

void ui::input_manager::mouse_leave()
{
	if (_captured.expired()) set_hover(nullptr);
}

bool ui::input_manager::wheel(const core::pointi loc, const int delta, const platform::key_state keys)
{
	const auto c = control_at(loc);
	if (!c || !c->enabled) return false;
	return c->on_wheel(loc - c->origin(), delta, keys);
}

bool ui::input_manager::context_menu(const core::pointi loc)
{
	cancel_system_mode();
	const auto c = control_at(loc);
	if (!c || !c->enabled) return false;

	if (c->traits.focus_on_click && c->can_focus() && _focused.lock() != c) focus(c.get(), false);

	const auto commands = c->context_commands(loc - c->origin());
	if (commands.empty()) return false;

	if (_host) track_menu(core::recti(_host->client_to_screen(loc), core::sizei{}), commands);
	return true;
}

std::optional<platform::cursor> ui::input_manager::cursor(const core::pointi loc) const
{
	if (const auto captured = _captured.lock()) return captured->cursor_at(loc - captured->origin());
	if (const auto c = control_at(loc)) return c->cursor_at(loc - c->origin());
	return std::nullopt;
}

bool ui::input_manager::move_focus(const bool forward)
{
	const auto controls = find_controls(false);
	const auto focused = _focused.lock();

	// A radio group is one stop: entered at its checked button, left by Tab.
	const auto is_stop = [&controls](const control& c)
	{
		if (!c.can_focus()) return false;
		const auto group = c.traits.radio_group;
		if (group < 0 || c.is_checked()) return true;

		return std::ranges::none_of(controls, [&c, group](const auto& other)
		{
			return other.get() != &c && other->traits.radio_group == group && other->is_checked() && other->can_focus();
		});
	};

	std::vector<control*> stops;
	for (const auto& c : controls) if (is_stop(*c) || c == focused) stops.push_back(c.get());
	if (stops.empty()) return false;

	const auto current = std::ranges::find(stops, focused.get());
	size_t next = 0;

	if (current == stops.end())
	{
		next = forward ? 0 : stops.size() - 1;
	}
	else
	{
		const auto index = static_cast<size_t>(current - stops.begin());
		next = forward ? (index + 1) % stops.size() : (index + stops.size() - 1) % stops.size();
	}

	if (stops[next] == focused.get() && !is_stop(*stops[next])) return false;
	focus(stops[next], stops[next]->traits.typing);
	return true;
}

bool ui::input_manager::key_down(const int key, const platform::key_state keys)
{
	if (!_host) return false;

	const auto k = static_cast<char32_t>(key);
	const auto focused = _focused.lock();

	// A pending Alt tap has no focus of its own; notify the control so any other key cancels the tap.
	if (const auto pending = _system_control.lock(); pending && !in_system_mode() && k != platform::keys::ALT)
		(void)pending->on_system_key(key, keys, true);

	if (focused && focused->can_focus())
	{
		if (focused->on_key(key, keys)) return true;

		// The arrows walk a radio group, checking each button they land on.
		const auto group = focused->traits.radio_group;

		if (group >= 0 && !keys.alt && !keys.control &&
			(k == platform::keys::UP || k == platform::keys::DOWN || k == platform::keys::LEFT || k == platform::keys::RIGHT))
		{
			const auto forward = k == platform::keys::DOWN || k == platform::keys::RIGHT;
			std::vector<control_ptr> members;
			for (const auto& c : find_controls(false))
				if (c->traits.radio_group == group && c->can_focus()) members.push_back(c);

			const auto current = std::ranges::find(members, focused);

			if (current != members.end() && members.size() > 1)
			{
				const auto index = static_cast<size_t>(current - members.begin());
				const auto& next = members[forward ? (index + 1) % members.size()
					                           : (index + members.size() - 1) % members.size()];
				focus(next.get(), false);
				next->click();
			}

			return true;
		}
	}

	if (k == platform::keys::TAB && !keys.control && !keys.alt)
	{
		return move_focus(!keys.shift);
	}

	if (!_host->is_dialog()) return false;

	// A dialog's buttons answer to the key their label marks: with Alt, or alone while the focused
	// control takes no typing, as the dialog manager pressed native buttons. A check box or radio
	// button takes the focus as it changes.
	if (!keys.control && (keys.alt || !is_typing()) && k != 0)
	{
		for (const auto& c : find_controls(false))
		{
			if (c->mnemonic == k && c->can_focus())
			{
				if (!c->traits.push_button) focus(c.get(), false);
				c->click();
				return true;
			}
		}
	}

	if (!keys.alt)
	{
		if (k == platform::keys::RETURN && !keys.control)
		{
			for (const auto& c : find_controls(false))
			{
				if (c->traits.default_button && c->enabled)
				{
					c->click();
					return true;
				}
			}
		}
		else if (k == platform::keys::ESCAPE)
		{
			_host->close(true);
			return true;
		}
	}

	return false;
}

bool ui::input_manager::system_key(const int key, const platform::key_state keys, const bool down)
{
	if (!_host) return false;

	for (const auto& c : find_controls(false))
	{
		if (!c->enabled || !c->ancestors_enabled) continue;
		if (c->on_system_key(key, keys, down))
		{
			_system_control = c;
			return true;
		}
	}

	return false;
}

bool ui::input_manager::in_system_mode() const
{
	const auto c = _system_control.lock();
	return c && c->is_visible() && c->in_system_mode();
}

void ui::input_manager::cancel_system_mode()
{
	const auto active = _system_control.lock();
	if (active && active->is_visible()) active->cancel_system_mode();

	// A mouse-opened bar has not necessarily handled a system key yet, so it has no remembered owner.
	if (_host)
		for (const auto& c : find_controls(false))
			if (c != active && c->is_visible() && c->in_system_mode()) c->cancel_system_mode();

	_system_control.reset();
}

bool ui::input_manager::text_input(const std::string_view text)
{
	const auto focused = _focused.lock();
	if (!focused || !focused->can_focus() || !focused->traits.typing) return false;
	return focused->on_text(text);
}

bool ui::input_manager::keyboard_focus(const bool has_focus)
{
	if (!has_focus) cancel_system_mode();
	const auto focused = _focused.lock();
	if (_keyboard_focus == has_focus) return focused != nullptr;
	_keyboard_focus = has_focus;

	if (focused)
	{
		focused->on_keyboard(has_focus);
		focused->invalidate();

		if (_host)
		{
			if (const auto h = _host->listener()) h->focus_changed(has_focus, focused);
		}
	}

	update_text_input();
	return focused != nullptr;
}

void ui::input_manager::clear_focus()
{
	focus(nullptr, false);
}

void ui::input_manager::blink()
{
	const auto focused = _focused.lock();
	if (focused && _keyboard_focus) focused->blink();
}

void ui::input_manager::tick()
{
	for (const auto& c : find_controls(false)) c->tick();
}

void ui::input_manager::scale_changed()
{
	// Hidden ones too, so one shown later is measured at the new scale.
	for (const auto& c : find_controls(true))
	{
		c->scale_changed();
		c->invalidate();
	}
}

bool ui::input_manager::focus_first()
{
	for (const auto& c : find_controls(false))
	{
		if (c->traits.first_focus_candidate && c->enabled)
		{
			focus(c.get(), c->traits.typing);
			return true;
		}
	}

	return false;
}

bool ui::input_manager::focus(const control_ptr& c)
{
	if (!c) return false;
	const auto controls = find_controls(false);
	if (std::ranges::find(controls, c) == controls.end()) return false;
	focus(c.get(), false);
	return true;
}

void ui::input_manager::focus(control* c, const bool select_all)
{
	if (!_host) return;

	const auto previous = _focused.lock();
	const auto next = c ? c->shared_from_this() : control_ptr{};

	if (previous != next)
	{
		_focused = next;

		if (previous)
		{
			previous->on_focus(false, false);
			previous->invalidate();

			if (_keyboard_focus)
			{
				if (const auto h = _host ? _host->listener() : nullptr) h->focus_changed(false, previous);
			}
		}

		if (next)
		{
			next->on_focus(true, select_all);
			next->invalidate();

			if (_keyboard_focus && _host)
			{
				if (const auto h = _host->listener()) h->focus_changed(true, next);
			}
		}
		else if (previous && _keyboard_focus && _host)
		{
			_host->keyboard_to_frame();
		}
	}
	else if (next && select_all)
	{
		next->on_focus(true, true);
		next->invalidate();
	}

	// Taking the window's keyboard focus reports the control's focus through keyboard_focus.
	if (next && !_keyboard_focus && _host) _host->take_keyboard_focus();
	update_text_input();
}

ui::control_ptr ui::input_manager::focused() const
{
	return _focused.lock();
}

bool ui::input_manager::has_focus(const control& c) const
{
	return _keyboard_focus && _focused.lock().get() == &c;
}

bool ui::input_manager::is_typing() const
{
	const auto focused = _focused.lock();
	return _keyboard_focus && focused && focused->can_focus() && focused->traits.typing;
}

bool ui::input_manager::wants_return() const
{
	const auto focused = _focused.lock();
	return _keyboard_focus && focused && focused->traits.wants_return;
}

bool ui::input_manager::is_tracking() const
{
	return !_captured.expired();
}

void ui::input_manager::update_text_input()
{
	const auto typing = is_typing();
	const auto focused = _focused.lock();
	const auto caret = typing ? focused->caret_bounds().offset(focused->origin()) : core::recti{};
	const auto blinks = typing && platform::caret_blink_time() != 0;

	if (_host)
	{
		_host->text_input(typing, caret);

		if (blinks != _caret_timer)
		{
			_caret_timer = blinks;
			_host->caret_timer(blinks, platform::caret_blink_time());
		}
	}
}

void ui::input_manager::caret_moved(const control& c)
{
	if (_focused.lock().get() != &c || !_keyboard_focus || !_host) return;

	_host->text_input(true, c.caret_bounds().offset(c.origin()));

	// Restarting the timer restarts the blink, so a caret that just moved shows straight away.
	if (_caret_timer) _host->caret_timer(true, platform::caret_blink_time());
}

void ui::input_manager::pressed_radio(const control& pressed)
{
	const auto group = pressed.traits.radio_group;
	if (group < 0) return;

	// Every member hears its new state, as each auto-radio button in a native group did.
	std::vector<control_ptr> members;
	for (const auto& c : find_controls(false)) if (c->traits.radio_group == group) members.push_back(c);

	for (const auto& c : members) c->check_from_group(c.get() == &pressed);
	for (const auto& c : members) c->notify_changed();
}

void ui::input_manager::track_menu(const core::recti screen_bounds, const std::vector<command_ptr>& commands)
{
	if (!_host || commands.empty()) return;

	// The menu takes the pointer, so a press that opened it must let go first.
	release_capture();
	_host->track_menu(screen_bounds, commands);
}

int ui::input_manager::caption_button_at(const core::pointi loc) const
{
	if (!_host || !_host->presents_caption_button()) return 0;

	for (const auto& c : find_controls(false))
	{
		if (const auto id = c->caption_button_at(loc - c->origin())) return id;
	}

	return 0;
}

void ui::input_manager::caption_hover(const int id)
{
	const auto previous = _caption.hover_id;
	if (!_caption.hover(id)) return;

	for (const auto& c : find_controls(false))
	{
		if (c->has_caption_button(previous) || c->has_caption_button(id)) c->invalidate();
	}
}

void ui::input_manager::caption_press(const int id)
{
	if (_caption.press(id))
	{
		for (const auto& c : find_controls(false)) if (c->has_caption_button(id)) c->invalidate();
	}
}

void ui::input_manager::caption_release(const int id)
{
	const auto invoke_id = _caption.release(id);
	const auto controls = find_controls(false);

	for (const auto& c : controls)
	{
		if (c->has_caption_button(id)) c->invalidate();
	}

	if (invoke_id == 0) return;

	for (const auto& c : controls)
	{
		if (c->has_caption_button(invoke_id))
		{
			c->invoke_caption(invoke_id);
			return;
		}
	}
}

void ui::input_manager::caption_leave()
{
	const auto hovered = _caption.hover_id;
	const auto pressed = _caption.pressed_id;

	if (_caption.leave())
	{
		for (const auto& c : find_controls(false))
		{
			if (c->has_caption_button(hovered) || c->has_caption_button(pressed)) c->invalidate();
		}
	}
}

core::recti ui::probe_edit_range_bounds(const edit_ptr& edit, const size_t start, const size_t end)
{
	return edit ? drawn_edit_range_bounds(*edit, start, end) : core::recti{};
}

std::vector<ui::command_ptr> ui::probe_edit_context_commands(const edit_ptr& edit, const core::pointi loc)
{
	return edit ? drawn_edit_context_commands(*edit, loc) : std::vector<command_ptr>{};
}
