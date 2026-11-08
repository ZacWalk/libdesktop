// This file is part of libdesktop, a C++20 platform layer for desktop applications
// Copyright 2026  Zac Walker
// 
// This program is free software; you can redistribute it and / or modify it
// under the terms of the LGPL License either version 2.1 or later.
// License details are available at https://www.gnu.org/licenses/lgpl-2.1.html
// This program is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY

// Purpose: The drawn text edit behind make_edit - one line or many, with its caret,
// selection, scrolling, clipboard, undo, spelling underline and suggestions, context menu, password,
// number and cue styles, and the list of suggestions it drops down - for every window layer.

#include "libdesktop/ui.h"
#include "controls_internal.h"

#include <future>

namespace
{
	constexpr std::string_view password_char = "\xE2\x97\x8F";

	bool starts_with_ignoring_case(const std::string_view text, const std::string_view prefix)
	{
		auto t = text.begin();
		auto p = prefix.begin();

		while (p < prefix.end())
		{
			if (t >= text.end()) return false;
			if (core::to_lower(core::pop_utf8_char(t, text.end())) != core::to_lower(core::pop_utf8_char(p, prefix.end())))
			{
				return false;
			}
		}

		return true;
	}

	// The folders in the folder a typed path names that start with what follows its last separator.
	std::vector<std::string> folder_suggestions(const std::string& typed)
	{
		const auto split = typed.find_last_of("\\/");
		if (split == std::string::npos) return {};

		const auto folder = typed.substr(0, split + 1);
		const auto prefix = std::string_view(typed).substr(split + 1);
		const auto contents = platform::iterate_file_items(core::folder_path(folder), false);
		std::vector<std::string> result;

		for (const auto& f : contents.folders)
		{
			const auto name = f.name.sv();
			if (starts_with_ignoring_case(name, prefix) && name.size() >= prefix.size())
			{
				result.emplace_back(folder + std::string(name));
				if (result.size() >= 32) break;
			}
		}

		std::ranges::sort(result, [](const std::string& l, const std::string& r) { return core::icmp(l, r) < 0; });
		return result;
	}
}

namespace ui
{
	class drawn_edit;

	// The suggestions an edit drops down, in a popup that takes no focus, so typing carries on in the
	// edit while the list follows it.
	class completion_list final : public frame_host, public std::enable_shared_from_this<completion_list>
	{
	public:
		std::weak_ptr<drawn_edit> edit;
		frame_ptr frame;
		std::vector<std::string> items;
		int selected = -1;
		int row_height = 0;
		core::sizei extent;
		render::font_face font = render::font_face::dialog;

		int border(const double s) const
		{
			return std::max(1, core::round(s));
		}

		int row_at(const core::pointi loc) const
		{
			if (row_height <= 0) return -1;
			const auto row = (loc.y - border(1.0)) / row_height;
			return loc.y >= 0 && row >= 0 && row < static_cast<int>(items.size()) ? row : -1;
		}

		void on_window_layout(render::measure_context& mc, const core::sizei e, const bool is_minimized) override
		{
			extent = e;
			row_height = mc.text_line_height(font) + drawn::scaled(6, mc.scale_factor);
		}

		void on_window_paint(render::draw_context& dc) override
		{
			const auto s = dc.scale_factor;
			const auto edge = border(s);
			const auto pad = drawn::scaled(drawn::element_padding, s) / 2;
			row_height = dc.text_line_height(font) + drawn::scaled(6, s);

			dc.clear(drawn::solid(style::colors.edit_background));
			drawn::draw_outline(dc, {0, 0, extent.cx, extent.cy}, drawn::solid(style::colors.dialog_selected_background),
			                    edge);

			for (size_t i = 0; i < items.size(); ++i)
			{
				const core::recti row(edge, edge + static_cast<int>(i) * row_height, extent.cx - edge,
				                edge + static_cast<int>(i + 1) * row_height);
				if (row.top >= extent.cy) break;

				if (static_cast<int>(i) == selected)
				{
					dc.draw_rect(row, drawn::solid(style::colors.dialog_selected_background));
				}

				dc.draw_text(items[i], row.inflate(-pad, 0), font, render::text_style::single_line,
				             drawn::solid(style::colors.edit_text), {});
			}
		}

		void on_mouse_move(const core::pointi loc, const bool is_tracking) override
		{
			const auto row = row_at(loc);
			if (row == selected) return;
			selected = row;
			if (frame) frame->invalidate();
		}

		void on_mouse_left_button_down(const core::pointi loc, const platform::key_state keys) override;
	};

	class drawn_edit final : public edit
	{
	public:
		struct visual_line
		{
			size_t start = 0;
			size_t end = 0;
			// The line ends at a line break or the text's end, rather than where it wrapped.
			bool paragraph_end = true;
			// Every boundary in [start, end], as model offsets, and its x from the line's start.
			std::vector<size_t> offsets;
			std::vector<int> xs;
		};

		edit_styles _styles;
		text_edit_model _model;
		std::function<void(const std::string&)> _changed;
		icon_index _icon{};
		core::color32 _background = style::colors.edit_background;
		std::vector<std::string> _auto_completes;

		mutable std::vector<visual_line> _lines;
		mutable bool _layout_valid = false;
		mutable int _layout_width = -1;
		mutable double _layout_scale = 0.0;
		mutable int _line_height = 0;
		mutable int _scroll_x = 0;
		mutable int _scroll_y = 0;
		int _goal_x = -1;
		bool _caret_visible = true;
		bool _selecting = false;
		int _spelling_ticks = 0;
		std::vector<spelling_range> _unknown;
		std::shared_ptr<std::atomic_bool> _respell = std::make_shared<std::atomic_bool>(false);
		std::shared_ptr<completion_list> _popup;
		std::future<std::vector<std::string>> _folder_listing;
		std::string _folder_listing_for;

		explicit drawn_edit(edit_styles styles) : _styles(std::move(styles)), _model(_styles.multi_line)
		{
			traits.typing = true;
			traits.first_focus_candidate = true;
			traits.wants_return = _styles.multi_line && _styles.want_return;
		}

		~drawn_edit() override
		{
			if (_popup && _popup->frame) _popup->frame->destroy();
		}

		///////////////////////////////////////////////////////////////////////////////////////////
		// Layout
		///////////////////////////////////////////////////////////////////////////////////////////

		core::recti inner_bounds(const double s, const int line_height) const
		{
			const auto padding = drawn::scaled(_styles.rounded_corners ? 8 : 4, s);
			const auto icon_width = _icon == icon_index{}
				                        ? 0
				                        : drawn::icon_cxy(s) + drawn::scaled(drawn::element_padding, s);
			auto r = bounds;
			r.left += padding + icon_width;
			r.right -= padding;

			if (_styles.multi_line)
			{
				r.top += padding;
				r.bottom -= padding;
			}
			else
			{
				const auto cy = std::max(0, (bounds.height() - line_height) / 2);
				r.top += cy;
				r.bottom = std::max(r.top, r.bottom - cy);
			}

			if (r.right < r.left) r.right = r.left;
			return r;
		}

		void layout_paragraph(render::measure_context& mc, const render::text_layout_ptr& layout, const size_t start, const size_t end,
		                      const int width) const
		{
			const auto& text = _model.text();
			std::vector<size_t> offsets;
			std::vector<size_t> shaped_offsets;
			std::string shaped;

			for (auto i = start; i < end; i = _model.next_boundary(i))
			{
				offsets.push_back(i);
				shaped_offsets.push_back(shaped.size());

				if (_styles.password)
				{
					shaped += password_char;
				}
				else
				{
					shaped.append(text, i, _model.next_boundary(i) - i);
				}
			}

			offsets.push_back(end);
			shaped_offsets.push_back(shaped.size());

			std::vector<int> xs;

			if (layout)
			{
				layout->update(shaped, render::text_style::single_line);
				xs = layout->offset_xs(shaped_offsets, 100000);
			}

			if (xs.size() != shaped_offsets.size())
			{
				xs.clear();
				for (const auto o : shaped_offsets)
				{
					xs.push_back(o == 0
						             ? 0
						             : mc.measure_text(std::string_view(shaped).substr(0, o), _styles.font,
						                               render::text_style::single_line, 100000).cx);
				}
			}

			const auto add_line = [&](const size_t first, const size_t last, const bool paragraph_end)
			{
				visual_line line;
				line.start = offsets[first];
				line.end = offsets[last];
				line.paragraph_end = paragraph_end;
				line.offsets.assign(offsets.begin() + static_cast<ptrdiff_t>(first),
				                    offsets.begin() + static_cast<ptrdiff_t>(last) + 1);
				line.xs.reserve(line.offsets.size());
				for (auto i = first; i <= last; ++i) line.xs.push_back(xs[i] - xs[first]);
				_lines.emplace_back(std::move(line));
			};

			if (!_styles.multi_line || width <= 0)
			{
				add_line(0, offsets.size() - 1, true);
				return;
			}

			size_t first = 0;

			while (true)
			{
				auto last = first;
				while (last + 1 < offsets.size() && xs[last + 1] - xs[first] <= width) ++last;

				if (last + 1 >= offsets.size())
				{
					add_line(first, offsets.size() - 1, true);
					break;
				}

				// Wrap after the last space that fits, a space that would just overflow included, or mid-word
				// when a single word is wider than the line.
				size_t wrap = 0;
				for (auto k = std::min(last + 1, offsets.size() - 1); k > first; --k)
				{
					if (offsets[k] > 0 && text[offsets[k] - 1] == ' ')
					{
						wrap = k;
						break;
					}
				}
				if (wrap == 0) wrap = std::max(last, first + 1);

				add_line(first, wrap, false);
				first = wrap;
			}
		}

		void ensure_layout(render::measure_context& mc) const
		{
			const auto s = mc.scale_factor;
			_line_height = std::max(1, mc.text_line_height(_styles.font));
			const auto inner = inner_bounds(s, _line_height);
			const auto width = _styles.multi_line ? std::max(1, inner.width()) : 0;
			if (_layout_valid && _layout_scale == s && _layout_width == width) return;

			_lines.clear();
			const auto& text = _model.text();
			const auto layout = mc.create_text_layout(_styles.font);
			size_t start = 0;

			while (true)
			{
				const auto end = _styles.multi_line ? std::min(text.find('\n', start), text.size()) : text.size();
				layout_paragraph(mc, layout, start, end, width);
				if (end >= text.size()) break;
				start = end + 1;
			}

			_layout_valid = true;
			_layout_scale = s;
			_layout_width = width;
		}

		bool ensure_layout() const
		{
			auto* const mc = metrics();
			if (!mc) return false;
			ensure_layout(*mc);
			return true;
		}

		// The visual line holding an offset. Where a line wrapped, the offset it ends on starts the next.
		size_t line_of(const size_t pos) const
		{
			for (size_t i = 0; i < _lines.size(); ++i)
			{
				const auto& l = _lines[i];
				if (pos >= l.start && (pos < l.end || (pos == l.end && l.paragraph_end))) return i;
			}

			return _lines.empty() ? 0 : _lines.size() - 1;
		}

		static int x_of(const visual_line& l, const size_t pos)
		{
			if (l.xs.empty()) return 0;
			const auto found = std::ranges::lower_bound(l.offsets, pos);
			const auto i = found == l.offsets.end()
				               ? l.offsets.size() - 1
				               : static_cast<size_t>(found - l.offsets.begin());
			return l.xs[i];
		}

		static size_t offset_at(const visual_line& l, const int x)
		{
			if (l.xs.empty()) return l.start;

			const auto found = std::ranges::lower_bound(l.xs, x);
			size_t result;

			if (found == l.xs.begin())
			{
				result = l.offsets.front();
			}
			else if (found == l.xs.end())
			{
				result = l.offsets.back();
			}
			else
			{
				const auto high = static_cast<size_t>(found - l.xs.begin());
				const auto low = high - 1;
				result = x - l.xs[low] <= l.xs[high] - x ? l.offsets[low] : l.offsets[high];
			}

			// The end of a wrapped line is the start of the next, so a press past it stays on this line.
			if (!l.paragraph_end && result == l.end && l.offsets.size() >= 2) result = l.offsets[l.offsets.size() - 2];
			return result;
		}

		int text_width(const visual_line& l) const
		{
			return l.xs.empty() ? 0 : l.xs.back();
		}

		// Where offset 0 of a single line is drawn, scrolled or centred.
		int origin_x(const core::recti& inner) const
		{
			if (!_styles.multi_line && _styles.align_center && !_lines.empty())
			{
				const auto w = text_width(_lines.front());
				if (w < inner.width()) return inner.left + (inner.width() - w) / 2;
			}

			return inner.left - (_styles.multi_line ? 0 : _scroll_x);
		}

		int line_top(const core::recti& inner, const size_t line) const
		{
			return _styles.multi_line ? inner.top + static_cast<int>(line) * _line_height - _scroll_y : inner.top;
		}

		void clamp_scroll(const core::recti& inner) const
		{
			if (_styles.multi_line)
			{
				const auto total = static_cast<int>(_lines.size()) * _line_height;
				_scroll_y = std::clamp(_scroll_y, 0, std::max(0, total - inner.height()));
				_scroll_x = 0;
			}
			else if (!_lines.empty())
			{
				const auto width = std::max(0, inner.width() - 1);
				_scroll_x = std::clamp(_scroll_x, 0, std::max(0, text_width(_lines.front()) - width));
			}
		}

		void scroll_to_caret() const
		{
			auto* const mc = metrics();
			if (!mc) return;
			ensure_layout(*mc);
			if (_lines.empty()) return;

			const auto inner = inner_bounds(mc->scale_factor, _line_height);
			const auto caret = _model.caret();

			if (_styles.multi_line)
			{
				const auto y = static_cast<int>(line_of(caret)) * _line_height;
				if (y < _scroll_y) _scroll_y = y;
				if (y + _line_height > _scroll_y + inner.height()) _scroll_y = y + _line_height - inner.height();
			}
			else
			{
				const auto caret_x = x_of(_lines.front(), caret);
				const auto width = std::max(0, inner.width() - 1);
				if (caret_x - _scroll_x > width) _scroll_x = caret_x - width;
				if (caret_x < _scroll_x) _scroll_x = caret_x;
			}

			clamp_scroll(inner);
		}

		size_t pos_at(const core::pointi loc) const
		{
			auto* const mc = metrics();
			if (!mc) return _model.caret();
			ensure_layout(*mc);
			if (_lines.empty()) return 0;

			const auto inner = inner_bounds(mc->scale_factor, _line_height);
			size_t line = 0;

			if (_styles.multi_line)
			{
				const auto y = loc.y - inner.top + _scroll_y;
				line = y < 0 ? 0 : std::min(static_cast<size_t>(y / _line_height), _lines.size() - 1);
			}

			const auto x0 = _styles.multi_line ? inner.left : origin_x(inner);
			return offset_at(_lines[line], loc.x - x0);
		}

		core::recti range_bounds(const size_t start, const size_t end) const
		{
			auto* const mc = metrics();
			if (!mc) return {};
			ensure_layout(*mc);
			if (_lines.empty()) return {};

			const auto inner = inner_bounds(mc->scale_factor, _line_height);
			const auto line = line_of(start);
			const auto& l = _lines[line];
			const auto x0 = _styles.multi_line ? inner.left : origin_x(inner);
			const auto y = line_top(inner, line);
			return {x0 + x_of(l, start), y, x0 + x_of(l, std::min(end, l.end)), y + _line_height};
		}

		core::recti caret_bounds() const override
		{
			auto* const mc = metrics();
			if (!mc) return {};
			ensure_layout(*mc);
			if (_lines.empty()) return {};

			const auto inner = inner_bounds(mc->scale_factor, _line_height);
			const auto line = line_of(_model.caret());
			const auto x0 = _styles.multi_line ? inner.left : origin_x(inner);
			const auto x = x0 + x_of(_lines[line], _model.caret());
			const auto y = line_top(inner, line);
			return {x, y, x + std::max(1, core::round(mc->scale_factor)), y + _line_height};
		}

		///////////////////////////////////////////////////////////////////////////////////////////
		// Changes
		///////////////////////////////////////////////////////////////////////////////////////////

		void respell()
		{
			_unknown = unknown_words(_model.text(), _styles.spelling);
		}

		void reset_caret()
		{
			_caret_visible = true;
		}

		void caret_changed()
		{
			scroll_to_caret();
			reset_caret();
			invalidate();
			if (const auto s = input()) s->caret_moved(*this);
		}

		// What the user typed, pasted, cut or chose; text set from code is not reported.
		void notify()
		{
			if (!_changed) return;
			const auto changed = _changed;
			changed(_model.text());
		}

		void text_changed(const bool typed)
		{
			_layout_valid = false;
			respell();
			caret_changed();
			if (typed) update_completions();
			notify();
		}

		///////////////////////////////////////////////////////////////////////////////////////////
		// Clipboard
		///////////////////////////////////////////////////////////////////////////////////////////

		bool can_copy() const
		{
			return _model.has_selection() && !_styles.password;
		}

		void copy() const
		{
			if (can_copy()) platform::set_clipboard(_model.selected_text());
		}

		bool cut()
		{
			if (!can_copy()) return false;
			return _model.cut_to_clipboard([](const std::string_view text) { return platform::set_clipboard(text); });
		}

		bool paste()
		{
			return _model.paste_from_clipboard([] { return platform::clipboard_text(); });
		}

		///////////////////////////////////////////////////////////////////////////////////////////
		// Suggestions
		///////////////////////////////////////////////////////////////////////////////////////////

		bool popup_open() const
		{
			return _popup && !_popup->items.empty();
		}

		void close_popup()
		{
			if (!_popup) return;
			_popup->items.clear();
			_popup->selected = -1;
			if (_popup->frame) _popup->frame->show(false);
		}

		void show_popup(std::vector<std::string> items)
		{
			if (items.empty() || !is_focused()) return close_popup();

			auto* const h = host();
			auto* const mc = metrics();
			if (!h || !mc) return;

			if (!_popup)
			{
				_popup = std::make_shared<completion_list>();
				_popup->edit = std::static_pointer_cast<drawn_edit>(shared_from_this());
				_popup->font = _styles.font;
				_popup->frame = h->create_popup(_popup, {
					                                style::colors.edit_background, style::colors.edit_text,
					                                style::colors.dialog_selected_background
				                                });
			}

			if (!_popup->frame) return;

			constexpr size_t max_rows = 8;
			if (items.size() > max_rows * 4) items.resize(max_rows * 4);

			const auto s = mc->scale_factor;
			const auto row_height = mc->text_line_height(_styles.font) + drawn::scaled(6, s);
			const auto edge = std::max(1, core::round(s));
			const auto rows = static_cast<int>(std::min(items.size(), max_rows));
			const auto screen = bounds.offset(screen_offset());
			const auto height = rows * row_height + edge * 2;
			core::recti r(screen.left, screen.bottom, screen.right, screen.bottom + height);

			const auto work = platform::desktop_bounds(true);
			if (!work.is_empty() && r.bottom > work.bottom && screen.top - height >= work.top)
			{
				r = core::recti(screen.left, screen.top - height, screen.right, screen.top);
			}

			_popup->items = std::move(items);
			_popup->selected = -1;
			_popup->row_height = row_height;
			_popup->frame->window_bounds(r, true);
			_popup->frame->invalidate();
		}

		std::vector<std::string> list_matches(const bool all) const
		{
			std::vector<std::string> result;
			const auto& text = _model.text();

			for (const auto& c : _auto_completes)
			{
				if (all || (c.size() > text.size() && starts_with_ignoring_case(c, text))) result.push_back(c);
			}

			return result;
		}

		void update_completions()
		{
			if (!is_focused() || _styles.multi_line) return close_popup();

			if (_styles.file_system_auto_complete)
			{
				const auto& text = _model.text();
				if (text.find_first_of("\\/") == std::string::npos) return close_popup();
				if (_folder_listing.valid() && _folder_listing_for == text) return;

				// Listing a folder can wait on a slow or sleeping drive, so it never runs on the UI thread.
				_folder_listing_for = text;
				_folder_listing = std::async(std::launch::async, [typed = text] { return folder_suggestions(typed); });
				return;
			}

			if (_auto_completes.empty() || _model.text().empty()) return close_popup();
			show_popup(list_matches(false));
		}

		void accept(const std::string& value)
		{
			close_popup();
			_model.select_all();
			_model.insert(value);
			_layout_valid = false;
			respell();
			caret_changed();
			notify();
		}

		bool popup_key(const char32_t k, const platform::key_state keys)
		{
			if (keys.alt || keys.control) return false;

			if (!popup_open())
			{
				// The list drops whole on Down, as the native one did.
				if (k == platform::keys::DOWN && !_styles.multi_line && !_styles.file_system_auto_complete &&
					!_auto_completes.empty())
				{
					show_popup(list_matches(_model.text().empty()));
					return popup_open();
				}

				return false;
			}

			const auto count = static_cast<int>(_popup->items.size());

			if (k == platform::keys::DOWN || k == platform::keys::UP)
			{
				const auto step = k == platform::keys::DOWN ? 1 : -1;
				_popup->selected = std::clamp(_popup->selected + step, -1, count - 1);
				if (_popup->frame) _popup->frame->invalidate();
				return true;
			}

			if (k == platform::keys::RETURN && _popup->selected >= 0 && _popup->selected < count)
			{
				const auto value = _popup->items[static_cast<size_t>(_popup->selected)];
				accept(value);
				return true;
			}

			if (k == platform::keys::ESCAPE)
			{
				close_popup();
				return true;
			}

			return false;
		}

		///////////////////////////////////////////////////////////////////////////////////////////
		// Movement
		///////////////////////////////////////////////////////////////////////////////////////////

		void move_lines(const int count, const bool extend)
		{
			if (!ensure_layout() || _lines.empty()) return;

			const auto line = line_of(_model.caret());
			if (_goal_x < 0) _goal_x = x_of(_lines[line], _model.caret());

			const auto target = std::clamp(static_cast<int>(line) + count, 0, static_cast<int>(_lines.size()) - 1);

			if (target == static_cast<int>(line))
			{
				// Past the first or last line, the caret goes to that end of the text.
				_model.move_to(count < 0 ? 0 : _model.text().size(), extend);
				return;
			}

			_model.move_to(offset_at(_lines[static_cast<size_t>(target)], _goal_x), extend);
		}

		void move_line_end(const bool end, const bool extend)
		{
			if (!_styles.multi_line || !ensure_layout() || _lines.empty())
			{
				end ? _model.move_end(extend) : _model.move_home(extend);
				return;
			}

			const auto& l = _lines[line_of(_model.caret())];
			auto pos = end ? l.end : l.start;

			// The end of a wrapped line is the start of the next; End stops before the space it wrapped at.
			if (end && !l.paragraph_end && l.offsets.size() >= 2) pos = l.offsets[l.offsets.size() - 2];
			_model.move_to(pos, extend);
		}

		int visible_lines() const
		{
			auto* const mc = metrics();
			if (!mc || _line_height <= 0) return 1;
			return std::max(1, inner_bounds(mc->scale_factor, _line_height).height() / _line_height);
		}

		void show_context_menu(const core::pointi loc)
		{
			const auto commands = context_commands(loc);
			if (commands.empty()) return;
			if (const auto s = input()) s->track_menu(core::recti(loc + screen_offset(), core::sizei{}), commands);
		}

		///////////////////////////////////////////////////////////////////////////////////////////
		// edit
		///////////////////////////////////////////////////////////////////////////////////////////

		std::string window_text() const
		{
			return _model.text();
		}

		// Text from code replaces what is there and puts the caret at its end. It is the owner's own
		// value, so it is not reported back as an edit.
		void window_text(const std::string_view text)
		{
			close_popup();

			if (text != _model.text())
			{
				_model.text(std::string(text));
				_layout_valid = false;
				respell();
			}

			_model.move_end();
			caret_changed();
		}

		void select(const int start, const int end)
		{
			const auto size = _model.text().size();
			const auto offset = [size](const int v) { return v < 0 ? size : std::min(static_cast<size_t>(v), size); };
			_model.select(offset(start), offset(end));
			caret_changed();
		}

		void select_all()
		{
			_model.select_all();
			caret_changed();
		}

		void set_icon(const icon_index icon)
		{
			if (_icon == icon) return;
			_icon = icon;
			_layout_valid = false;
			invalidate();
		}

		void set_background(const core::color32 bg)
		{
			if (_background == bg) return;
			_background = bg;
			invalidate();
		}

		void auto_completes(const std::vector<std::string>& texts)
		{
			_auto_completes = texts;
		}

		void enabled_changed() override
		{
			control::enabled_changed();
			if (!enabled) close_popup();
		}

		///////////////////////////////////////////////////////////////////////////////////////////
		// control
		///////////////////////////////////////////////////////////////////////////////////////////

		// As large as its owner makes it.
		core::sizei measure_content(render::measure_context&, int) const override
		{
			return bounds.extent();
		}

		platform::cursor cursor_at(const core::pointi loc) const override
		{
			return platform::cursor::text_select;
		}

		void scale_changed() override
		{
			_layout_valid = false;
		}

		void on_focus(const bool focused, const bool select_all) override
		{
			if (focused)
			{
				if (select_all || _styles.select_all_on_focus) _model.select_all();
				_goal_x = -1;
				reset_caret();
				scroll_to_caret();
			}
			else
			{
				_selecting = false;
				close_popup();
			}
		}

		void on_keyboard(const bool has_keyboard) override
		{
			if (!has_keyboard) close_popup();
			reset_caret();
		}

		void blink() override
		{
			_caret_visible = !_caret_visible;
			const auto caret = caret_bounds();
			if (!caret.is_empty()) invalidate(caret.inflate(1));
		}

		void tick() override
		{
			auto check = _respell->exchange(false);

			// The dictionary loads in the background and says nothing when it is ready, so the edit
			// looks again now and then.
			if (_styles.spelling && ++_spelling_ticks >= render::default_ticks_per_second * 2)
			{
				_spelling_ticks = 0;
				check = true;
			}

			if (check)
			{
				const auto before = _unknown;
				respell();
				if (before != _unknown) invalidate();
			}

			if (_folder_listing.valid() &&
				_folder_listing.wait_for(std::chrono::seconds(0)) == std::future_status::ready)
			{
				auto items = _folder_listing.get();
				if (_folder_listing_for == _model.text()) show_popup(std::move(items));
			}
		}

		bool on_key(const int key, const platform::key_state keys) override
		{
			if (_styles.capture_key_down && _styles.capture_key_down(key, keys)) return true;

			const auto k = static_cast<char32_t>(key);
			if (popup_key(k, keys)) return true;
			if (keys.alt) return false;

			const auto ctrl = keys.control;
			const auto shift = keys.shift;
			const auto vertical = k == platform::keys::UP || k == platform::keys::DOWN || k == platform::keys::PRIOR ||
				k == platform::keys::NEXT;
			auto changed = false;

			if (ctrl && k == U'A') _model.select_all();
			else if (ctrl && (k == U'C' || k == platform::keys::INSERT)) copy();
			else if ((ctrl && k == U'X') || (shift && k == platform::keys::DEL)) changed = cut();
			else if ((ctrl && k == U'V') || (shift && k == platform::keys::INSERT)) changed = paste();
			else if (ctrl && k == U'Z')
			{
				shift ? _model.redo() : _model.undo();
				changed = true;
			}
			else if (ctrl && k == U'Y')
			{
				_model.redo();
				changed = true;
			}
			else if (k == platform::keys::LEFT) ctrl ? _model.move_word_left(shift) : _model.move_left(shift);
			else if (k == platform::keys::RIGHT) ctrl ? _model.move_word_right(shift) : _model.move_right(shift);
			else if (vertical)
			{
				if (!_styles.multi_line) return false;
				const auto page = visible_lines();
				const auto count = k == platform::keys::UP ? -1 : k == platform::keys::DOWN ? 1 : k == platform::keys::PRIOR ? -page : page;
				move_lines(count, shift);
			}
			else if (k == platform::keys::HOME) ctrl ? _model.move_home(shift) : move_line_end(false, shift);
			else if (k == platform::keys::END) ctrl ? _model.move_end(shift) : move_line_end(true, shift);
			else if (k == platform::keys::BACK)
			{
				ctrl ? _model.backspace_word() : _model.backspace();
				changed = true;
			}
			else if (k == platform::keys::DEL)
			{
				ctrl ? _model.delete_word() : _model.delete_forward();
				changed = true;
			}
			else if (k == platform::keys::RETURN)
			{
				// A multi-line edit that does not take Return takes Ctrl+Return as a new line, as the native one did.
				if (!_styles.multi_line || (_styles.want_return ? ctrl : !ctrl)) return false;
				_model.insert("\n");
				changed = true;
			}
			else if (k == platform::keys::APPS || (shift && k == platform::keys::F10))
			{
				show_context_menu(caret_bounds().top_left());
				return true;
			}
			else if (ctrl && (k == platform::keys::TAB || k == platform::keys::ESCAPE))
			{
				return false;
			}
			else
			{
				// Characters arrive as text; a key that is neither is not the edit's.
				return false;
			}

			if (!vertical) _goal_x = -1;
			if (changed) text_changed(true);
			else caret_changed();
			return true;
		}

		bool on_text(const std::string_view text) override
		{
			std::string accepted;
			auto i = text.begin();

			while (i < text.end())
			{
				const auto start = i;
				const auto cp = core::pop_utf8_char(i, text.end());
				if (i == start) break;
				if (cp < 0x20 || cp == 0x7f) continue;
				if (_styles.number && (cp < '0' || cp > '9')) continue;
				accepted.append(start, i);
			}

			if (accepted.empty()) return true;

			_model.insert(accepted);
			_goal_x = -1;
			text_changed(true);
			return true;
		}

		void on_mouse_down(const core::pointi loc, const platform::key_state keys) override
		{
			// The press that focused an edit selecting all on focus leaves that selection, as the
			// native edit did.
			if (click_took_focus && _styles.select_all_on_focus)
			{
				caret_changed();
				return;
			}

			const auto pos = pos_at(loc);
			if (keys.shift) _model.select(_model.anchor(), pos);
			else _model.select(pos, pos);
			_selecting = true;
			_goal_x = -1;
			close_popup();
			caret_changed();
		}

		void on_mouse_move(const core::pointi loc, const bool captured) override
		{
			if (!captured || !_selecting) return;
			const auto anchor = _model.anchor();
			_model.select(anchor, pos_at(loc));
			caret_changed();
		}

		void on_mouse_up(const core::pointi loc, const platform::key_state keys) override
		{
			_selecting = false;
		}

		void on_double_click(const core::pointi loc, const platform::key_state keys) override
		{
			_model.select_word(pos_at(loc));
			_selecting = false;
			_goal_x = -1;
			caret_changed();
		}

		std::vector<command_ptr> context_commands(const core::pointi loc) override
		{
			std::vector<command_ptr> result;
			const auto weak = std::weak_ptr(std::static_pointer_cast<drawn_edit>(shared_from_this()));
			const auto& t = platform::text();

			const auto add = [&result](const std::string_view text, const bool enable, std::function<void()> invoke)
			{
				auto c = std::make_shared<command>();
				c->text = text;
				c->enable = enable;
				c->invoke = std::move(invoke);
				result.emplace_back(std::move(c));
			};

			const auto act = [weak](void (*action)(drawn_edit&))
			{
				return [weak, action]
				{
					if (const auto self = weak.lock()) action(*self);
				};
			};

			const spelling_range* misspelled = nullptr;

			if (_styles.spelling)
			{
				for (const auto& u : _unknown)
				{
					if (range_bounds(u.start, u.end).contains(loc))
					{
						misspelled = &u;
						break;
					}
				}
			}

			if (misspelled)
			{
				const auto word = *misspelled;
				auto suggestions = _styles.spelling->suggest(word.word);
				if (suggestions.size() > 8) suggestions.resize(8);

				for (const auto& suggestion : suggestions)
				{
					add(suggestion, true, [weak, word, suggestion]
					{
						if (const auto self = weak.lock())
						{
							self->_model.select(word.start, word.end);
							self->_model.insert(suggestion);
							self->text_changed(false);
						}
					});
				}

				if (!suggestions.empty()) result.emplace_back(nullptr);

				auto* const dictionary = _styles.spelling;
				const auto respell = _respell;
				add(str_format(t.menu_add_fmt.sv(), word.word), true, [dictionary, respell, word]
				{
					// The dictionary may finish on another thread; the edit looks again on its next tick.
					dictionary->add_word(word.word, [respell] { respell->store(true); });
				});

				result.emplace_back(nullptr);
			}

			if (_model.can_undo())
			{
				add(t.menu_undo.sv(), true, act([](drawn_edit& e)
				{
					e._model.undo();
					e.text_changed(false);
				}));
				result.emplace_back(nullptr);
			}

			add(t.menu_cut.sv(), can_copy(), act([](drawn_edit& e) { if (e.cut()) e.text_changed(false); }));
			add(t.menu_copy.sv(), can_copy(), act([](drawn_edit& e) { e.copy(); }));
			add(t.menu_paste.sv(), true, act([](drawn_edit& e) { if (e.paste()) e.text_changed(false); }));
			add(t.menu_delete.sv(), _model.has_selection(), act([](drawn_edit& e)
			{
				e._model.erase_selection();
				e.text_changed(false);
			}));
			result.emplace_back(nullptr);
			add(t.menu_select_all.sv(), !_model.text().empty(), act([](drawn_edit& e)
			{
				e._model.select_all();
				e.caret_changed();
			}));

			return result;
		}

		///////////////////////////////////////////////////////////////////////////////////////////
		// Paint
		///////////////////////////////////////////////////////////////////////////////////////////

		void paint(render::draw_context& dc, const bool focused) const override
		{
			const auto s = dc.scale_factor;
			const auto alpha = dc.colors.alpha;
			const auto bg = enabled ? _background : core::average(_background, style::colors.dialog_background);
			const auto fg = text_on_background(bg);

			if (_styles.rounded_corners)
			{
				const auto radius = drawn::scaled(drawn::corner_radius, s);
				const auto edge = std::max(1, core::round(2.6 * s));
				const auto inside = bounds.inflate(-edge);
				if (focused) dc.draw_rounded_rect(bounds, drawn::solid(style::colors.dialog_selected_background, alpha), radius);
				dc.draw_rounded_rect(inside, drawn::solid(bg, alpha), std::max(0, radius - edge / 2));
			}
			else if (focused)
			{
				const auto edge = drawn::scaled(drawn::focus_padding, s);
				dc.draw_rect(bounds, drawn::solid(style::colors.dialog_selected_background, alpha));
				dc.draw_rect(bounds.inflate(-edge), drawn::solid(bg, alpha));
			}
			else
			{
				dc.draw_rect(bounds, drawn::solid(bg, alpha));
			}

			if (_icon != icon_index{})
			{
				const auto left = bounds.left + drawn::scaled(drawn::element_padding, s);
				drawn::draw_icon(dc, _icon, {left, bounds.top, left + drawn::icon_cxy(s), bounds.bottom},
				                 drawn::solid(style::colors.edit_text, alpha));
			}

			ensure_layout(dc);

			if (focused) scroll_to_caret();
			const auto inner = inner_bounds(s, _line_height);
			clamp_scroll(inner);

			render::scoped_clip clip(dc, inner);

			const auto text_color = drawn::solid(fg, alpha);
			const auto& text = _model.text();

			if (text.empty())
			{
				if (!focused && !_styles.cue.empty())
				{
					dc.draw_text(_styles.cue, {inner.left, inner.top, inner.right, inner.top + _line_height},
					             _styles.font, render::text_style::single_line, text_color.aa(0.55f * alpha), {});
				}
			}

			const auto selection_start = _model.selection_start();
			const auto selection_end = _model.selection_end();
			const auto space = std::max(2, _line_height / 4);
			const auto squiggle = std::max(1, core::round(s));

			for (size_t i = 0; i < _lines.size(); ++i)
			{
				const auto& l = _lines[i];
				const auto y = line_top(inner, i);
				if (y + _line_height <= inner.top) continue;
				if (y >= inner.bottom) break;

				const auto x0 = _styles.multi_line ? inner.left : origin_x(inner);

				// Only the edit with the focus shows its selection, as a native edit did.
				if (focused && _model.has_selection() && selection_start <= l.end && selection_end >= l.start)
				{
					const auto from = std::max(selection_start, l.start);
					const auto to = std::min(selection_end, l.end);
					auto right = x0 + x_of(l, to);
					// A selection running on past the end of the line shows that it takes the break.
					if (selection_end > l.end) right += space;
					const auto left = x0 + x_of(l, from);
					if (right > left)
					{
						dc.draw_rect({left, y, right, y + _line_height},
						             drawn::solid(style::colors.dialog_selected_background, alpha));
					}
				}

				if (l.end > l.start)
				{
					std::string line_text;

					if (_styles.password)
					{
						for (auto p = l.start; p < l.end; p = _model.next_boundary(p)) line_text += password_char;
					}
					else
					{
						line_text = text.substr(l.start, l.end - l.start);
					}

					dc.draw_text(line_text, {x0, y, x0 + text_width(l) + _line_height, y + _line_height}, _styles.font,
					             render::text_style::single_line, text_color, {});
				}

				for (const auto& u : _unknown)
				{
					if (u.end <= l.start || u.start >= l.end) continue;
					const auto from = x0 + x_of(l, std::max(u.start, l.start));
					const auto to = x0 + x_of(l, std::min(u.end, l.end));
					const auto underline = y + _line_height - squiggle;

					for (auto x = from; x < to; x += squiggle * 2)
					{
						dc.draw_rect({x, underline, std::min(x + squiggle, to), underline + squiggle},
						             core::color(1.0f, 0.0f, 0.0f, alpha));
					}
				}
			}

			if (focused && _caret_visible && enabled && !_lines.empty())
			{
				const auto line = line_of(_model.caret());
				const auto& l = _lines[line];
				const auto x0 = _styles.multi_line ? inner.left : origin_x(inner);
				const auto x = x0 + x_of(l, _model.caret());
				const auto y = line_top(inner, line);
				dc.draw_rect({x, y, x + std::max(1, core::round(s)), y + _line_height}, text_color);
			}
		}

		core::color32 text_on_background(const core::color32 bg) const
		{
			return enabled ? style::colors.edit_text : core::average(style::colors.edit_text, bg);
		}
	};

	void completion_list::on_mouse_left_button_down(const core::pointi loc, const platform::key_state keys)
	{
		const auto row = row_at(loc);
		const auto e = edit.lock();
		if (row < 0 || !e) return;
		const auto value = items[static_cast<size_t>(row)];
		e->accept(value);
	}
}

ui::edit_ptr ui::make_drawn_edit(const edit_styles& styles, const std::string_view text,
                                 std::function<void(const std::string&)> changed)
{
	auto e = std::make_shared<drawn_edit>(styles);
	e->_model.text(std::string(text));
	// A new native edit held its caret at the start of the text it was made with.
	e->_model.select(0, 0);
	e->_changed = std::move(changed);
	e->_auto_completes = styles.auto_complete_list;

	if (styles.spelling)
	{
		styles.spelling->queue_load();
		e->respell();
	}

	return e;
}

core::recti ui::drawn_edit_range_bounds(const edit& e, const size_t start, const size_t end)
{
	return static_cast<const drawn_edit&>(e).range_bounds(start, end);
}

std::vector<ui::command_ptr> ui::drawn_edit_context_commands(edit& e, const core::pointi loc)
{
	return static_cast<drawn_edit&>(e).context_commands(loc);
}

// The edit has one implementation, so its methods reach it directly.

std::string ui::edit::window_text() const
{
	return static_cast<const drawn_edit*>(this)->window_text();
}

void ui::edit::window_text(const std::string_view text)
{
	static_cast<drawn_edit*>(this)->window_text(text);
}

void ui::edit::select(const int start, const int end)
{
	static_cast<drawn_edit*>(this)->select(start, end);
}

void ui::edit::select_all()
{
	static_cast<drawn_edit*>(this)->select_all();
}

void ui::edit::set_icon(const icon_index icon)
{
	static_cast<drawn_edit*>(this)->set_icon(icon);
}

void ui::edit::set_background(const core::color32 bg)
{
	static_cast<drawn_edit*>(this)->set_background(bg);
}

void ui::edit::auto_completes(const std::vector<std::string>& texts)
{
	static_cast<drawn_edit*>(this)->auto_completes(texts);
}
