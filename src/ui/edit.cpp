// This file is part of libdesktop, a C++20 platform layer for desktop applications
// Copyright 2026  Zac Walker
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

// Purpose: The drawn text edit behind make_edit - one line or many, with its caret, selection, scrolling and
// scrollbar, clipboard, undo, spelling underline and suggestions, context menu, password, number and cue styles, and
// the list of suggestions it drops down, which completes a typed path from folders listed on a worker - for every
// window layer.

#include "libdesktop/ui.h"
#include "libdesktop/ui/views/scrollbar.h"
#include "controls_internal.h"

namespace
{
	constexpr std::string_view password_char = "\xE2\x97\x8F";
	// The rows a list of suggestions shows at once, and the most it holds.
	constexpr int completion_rows = 8;
	constexpr size_t completion_limit = 32;
	// What one detent of the wheel scrolls: lines of a multi-line edit, rows of a list.
	constexpr int wheel_lines = 3;

	// A distance scaled by num / den, the same either way: core::mul_div rounds a negative product towards zero.
	int scale_distance(const int distance, const int num, const int den)
	{
		const auto magnitude = core::mul_div(std::abs(distance), num, den);
		return distance < 0 ? -magnitude : magnitude;
	}

	bool starts_with_ignoring_case(const std::string_view text, const std::string_view prefix)
	{
		auto t = text.begin();
		auto p = prefix.begin();

		while (p < prefix.end())
		{
			if (t >= text.end()) return false;
			if (core::to_lower(core::pop_utf8_char(t, text.end())) !=
			    core::to_lower(core::pop_utf8_char(p, prefix.end())))
			{
				return false;
			}
		}

		return true;
	}

	// The names of the folders in a typed path's folder, or of a computer's shares where it names one, sorted.
	std::vector<std::string> list_folder_names(const std::string& folder)
	{
		std::vector<std::string> result;
		if (!core::is_path(folder)) return result;

		const core::folder_path path(folder);
		const auto folders = core::is_server(folder) ? platform::iterate_file_items(path, false).folders
		                                             : platform::select_folders(core::item_selector(path), false);
		result.reserve(folders.size());
		for (const auto& f : folders) result.emplace_back(f.name.sv());
		std::ranges::sort(result, [](const std::string& l, const std::string& r) { return core::icmp(l, r) < 0; });
		return result;
	}

	ui::folder_lister& replaced_folder_lister()
	{
		static ui::folder_lister lister;
		return lister;
	}
}

namespace ui
{
	class drawn_edit;

	// The suggestions an edit drops down, in a popup that takes no focus, so typing carries on in the edit while the
	// list follows it. It shows a page of rows at a time, scrolled by the wheel and its scrollbar, and to keep the row
	// the keys choose in view.
	class completion_list final : public frame_host, public std::enable_shared_from_this<completion_list>
	{
	public:
		std::weak_ptr<drawn_edit> edit;
		frame_ptr frame;
		std::vector<std::string> items;
		int selected = -1;
		// The first row shown, and how many rows the popup shows at once.
		int top = 0;
		int rows = 0;
		int row_height = 0;
		double scale = 1.0;
		core::sizei extent;
		render::font_face font = render::font_face::dialog;
		double wheel_pending = 0.0;
		// The scrollbar's thumb is held, from where it was pressed.
		bool dragging = false;
		int drag_start_y = 0;
		int drag_start_top = 0;

		int border() const { return std::max(1, core::round(scale)); }
		int count() const { return static_cast<int>(items.size()); }
		bool scrolls() const { return rows > 0 && count() > rows; }
		int max_top() const { return std::max(0, count() - rows); }

		// The scrollbar's track, inside the border down the right edge.
		core::recti bar_bounds() const
		{
			const auto edge = border();
			return {extent.cx - edge - drawn::scaled(8, scale), edge, extent.cx - edge, edge + rows * row_height};
		}

		core::recti thumb_bounds() const
		{
			scrollbar bar;
			bar.set_dpi_scale(scale);
			bar.update(count() * row_height, rows * row_height, top * row_height);
			const auto track = bar_bounds();
			const auto span = bar.thumb(track.top, track.height());
			return {track.left, span.start, track.right, span.start + span.length};
		}

		bool on_bar(const core::pointi loc) const { return scrolls() && loc.x >= bar_bounds().left; }

		void scroll_to(const int row)
		{
			const auto first = std::clamp(row, 0, max_top());
			if (first == top) return;
			top = first;
			if (frame) frame->invalidate();
		}

		// Scrolls as little as shows the row.
		void reveal(const int row)
		{
			if (row < 0) return;
			if (row < top) scroll_to(row);
			else if (row >= top + rows) scroll_to(row - rows + 1);
		}

		int row_at(const core::pointi loc) const
		{
			if (row_height <= 0 || loc.y < border() || on_bar(loc)) return -1;
			const auto row = top + (loc.y - border()) / row_height;
			return row < std::min(count(), top + rows) ? row : -1;
		}

		void on_window_layout(render::measure_context& mc, const core::sizei e, const bool /*is_minimized*/) override
		{
			extent = e;
			scale = mc.scale_factor;
			row_height = mc.text_line_height(font) + drawn::scaled(6, scale);
		}

		void on_window_paint(render::draw_context& dc) override
		{
			scale = dc.scale_factor;
			const auto edge = border();
			const auto pad = drawn::scaled(drawn::element_padding, scale) / 2;
			row_height = dc.text_line_height(font) + drawn::scaled(6, scale);
			top = std::clamp(top, 0, max_top());

			dc.clear(drawn::solid(style::colors.edit_background));
			drawn::draw_outline(dc, {0, 0, extent.cx, extent.cy},
			                    drawn::solid(style::colors.dialog_selected_background), edge);

			const auto right = scrolls() ? bar_bounds().left : extent.cx - edge;
			const auto last = std::min(count(), top + rows);

			for (auto i = top; i < last; ++i)
			{
				const auto y = edge + (i - top) * row_height;
				const core::recti row(edge, y, right, y + row_height);
				if (row.top >= extent.cy) break;

				if (i == selected) dc.draw_rect(row, drawn::solid(style::colors.dialog_selected_background));

				dc.draw_text(items[static_cast<size_t>(i)], row.inflate(-pad, 0), font, render::text_style::single_line,
				             drawn::solid(style::colors.edit_text), {});
			}

			if (scrolls())
			{
				const auto thumb = thumb_bounds().inflate(-drawn::scaled(2, scale), 0);
				const auto shade = dragging ? style::colors.dialog_selected_background
				                            : core::lerp(style::colors.edit_background, style::colors.edit_text, 0.35f);
				dc.draw_rounded_rect(thumb, drawn::solid(shade), thumb.width() / 2);
			}
		}

		void on_mouse_move(const core::pointi loc, const bool is_tracking) override
		{
			if (dragging)
			{
				if (is_tracking)
				{
					const auto track = bar_bounds();
					if (track.height() > 0)
						scroll_to(drag_start_top + scale_distance(loc.y - drag_start_y, count(), track.height()));
					return;
				}

				// The pointer was taken before the button came up.
				dragging = false;
				if (frame) frame->invalidate();
			}

			if (on_bar(loc)) return;
			const auto row = row_at(loc);
			if (row == selected) return;
			selected = row;
			if (frame) frame->invalidate();
		}

		void on_mouse_left_button_down(const core::pointi loc, const platform::key_state keys) override;

		void on_mouse_left_button_up(const core::pointi /*loc*/, const platform::key_state /*keys*/) override
		{
			if (!dragging) return;
			dragging = false;
			if (frame) frame->invalidate();
		}

		void on_mouse_wheel(const core::pointi /*loc*/, const int delta, const platform::key_state /*keys*/,
		                    bool& was_handled) override
		{
			was_handled = true;
			const auto steps = platform::accumulate_wheel_steps(wheel_pending, delta);
			if (steps != 0) scroll_to(top - steps * wheel_lines);
		}
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
		mutable int _layout_height = -1;
		mutable double _layout_scale = 0.0;
		mutable int _line_height = 0;
		mutable int _scroll_x = 0;
		mutable int _scroll_y = 0;
		// A multi-line edit's text is taller than it, so it wraps beside the scrollbar it shows.
		mutable bool _scrollbar = false;
		// The text's area the caret was last kept in view in. A new size or scale brings the caret of an edit with
		// the focus back into view as it is next drawn; nothing else moves the text the wheel or the scrollbar put.
		mutable core::sizei _seen_extent;
		mutable double _seen_scale = 0.0;
		mutable bool _reveal_caret = false;
		int _goal_x = -1;
		bool _caret_visible = true;
		bool _selecting = false;
		// The scrollbar's thumb is held, from where it was pressed.
		bool _scrolling = false;
		int _scroll_start_mouse = 0;
		int _scroll_start_y = 0;
		bool _scrollbar_hover = false;
		int _spelling_ticks = 0;
		std::vector<spelling_range> _unknown;
		std::shared_ptr<std::atomic_bool> _respell = std::make_shared<std::atomic_bool>(false);
		std::shared_ptr<completion_list> _popup;
		// The folder a typed path names, ending in its separator, and the names of the folders in it, listed once
		// for every name typed there. A folder is listed on a worker, one at a time: _listing is the one in hand.
		std::string _folders_of;
		std::vector<std::string> _folders;
		std::string _listing;

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
			const auto pad = drawn::scaled(_styles.rounded_corners ? 8 : 4, s);
			const auto icon_width = _icon == icon_index{}
			                            ? 0
			                            : drawn::icon_cxy(s) + drawn::scaled(drawn::element_padding, s);
			auto r = bounds;
			r.left += pad + icon_width;
			r.right -= pad;

			if (_styles.multi_line)
			{
				r.top += pad;
				r.bottom -= pad;
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

		// The text's own area: inside the padding and the icon, and left of a multi-line edit's scrollbar.
		core::recti text_bounds(const double s) const
		{
			auto r = inner_bounds(s, _line_height);
			if (_scrollbar) r.right = std::max(r.left, r.right - scrollbar_room(s));
			return r;
		}

		static int scrollbar_width(const double s) { return drawn::scaled(8, s); }
		static int scrollbar_room(const double s) { return scrollbar_width(s) + drawn::scaled(4, s); }

		// Where a multi-line edit's scrollbar thumb runs: down its right side, inside its padding.
		core::recti scrollbar_track(const double s) const
		{
			const auto inner = inner_bounds(s, _line_height);
			return {inner.right - scrollbar_width(s), inner.top, inner.right, inner.bottom};
		}

		int content_height() const { return static_cast<int>(_lines.size()) * _line_height; }

		// Every boundary of a paragraph, as model offsets, and its x from the paragraph's start as it is drawn.
		struct shaped_paragraph
		{
			std::vector<size_t> offsets;
			std::vector<int> xs;
		};

		shaped_paragraph shape_paragraph(render::measure_context& mc, const render::text_layout_ptr& layout,
		                                 const size_t start, const size_t end) const
		{
			const auto& text = _model.text();
			shaped_paragraph result;
			std::vector<size_t> shaped_offsets;
			std::string shaped;

			for (auto i = start; i < end; i = _model.next_boundary(i))
			{
				result.offsets.push_back(i);
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

			result.offsets.push_back(end);
			shaped_offsets.push_back(shaped.size());

			if (layout)
			{
				layout->update(shaped, render::text_style::single_line);
				result.xs = layout->offset_xs(shaped_offsets, 100000);
			}

			if (result.xs.size() != shaped_offsets.size())
			{
				result.xs.clear();
				for (const auto o : shaped_offsets)
				{
					result.xs.push_back(o == 0 ? 0
					                           : mc.measure_text(std::string_view(shaped).substr(0, o), _styles.font,
					                                             render::text_style::single_line, 100000)
					                                 .cx);
				}
			}

			return result;
		}

		void wrap_paragraph(const shaped_paragraph& paragraph, const int width) const
		{
			const auto& text = _model.text();
			const auto& offsets = paragraph.offsets;
			const auto& xs = paragraph.xs;

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

		void wrap_text(const std::vector<shaped_paragraph>& paragraphs, const int width) const
		{
			_lines.clear();
			for (const auto& p : paragraphs) wrap_paragraph(p, width);
		}

		void ensure_layout(render::measure_context& mc) const
		{
			const auto s = mc.scale_factor;
			_line_height = std::max(1, mc.text_line_height(_styles.font));
			const auto inner = inner_bounds(s, _line_height);

			if (inner.extent() != _seen_extent || s != _seen_scale)
			{
				_seen_extent = inner.extent();
				_seen_scale = s;
				_reveal_caret = true;
			}

			const auto width = _styles.multi_line ? std::max(1, inner.width()) : 0;
			const auto height = _styles.multi_line ? inner.height() : 0;
			if (_layout_valid && _layout_scale == s && _layout_width == width && _layout_height == height) return;

			std::vector<shaped_paragraph> paragraphs;
			const auto& text = _model.text();
			const auto layout = mc.create_text_layout(_styles.font);
			size_t start = 0;

			while (true)
			{
				const auto end = _styles.multi_line ? std::min(text.find('\n', start), text.size()) : text.size();
				paragraphs.emplace_back(shape_paragraph(mc, layout, start, end));
				if (end >= text.size()) break;
				start = end + 1;
			}

			wrap_text(paragraphs, width);
			_scrollbar = false;

			// Text taller than the edit wraps again beside the scrollbar it then shows, and narrower it is no shorter.
			if (_styles.multi_line && content_height() > height && width > scrollbar_room(s))
			{
				_scrollbar = true;
				wrap_text(paragraphs, width - scrollbar_room(s));
			}

			_layout_valid = true;
			_layout_scale = s;
			_layout_width = width;
			_layout_height = height;
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
			const auto i = found == l.offsets.end() ? l.offsets.size() - 1
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

		int text_width(const visual_line& l) const { return l.xs.empty() ? 0 : l.xs.back(); }

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
				_scroll_y = std::clamp(_scroll_y, 0, std::max(0, content_height() - inner.height()));
				_scroll_x = 0;
			}
			else if (!_lines.empty())
			{
				const auto width = std::max(0, inner.width() - 1);
				_scroll_x = std::clamp(_scroll_x, 0, std::max(0, text_width(_lines.front()) - width));
			}
		}

		void scroll_to_caret(render::measure_context& mc) const
		{
			ensure_layout(mc);
			_reveal_caret = false;
			if (_lines.empty()) return;

			const auto inner = text_bounds(mc.scale_factor);
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

		void scroll_to_caret() const
		{
			if (auto* const mc = metrics()) scroll_to_caret(*mc);
		}

		// Scrolls a multi-line edit's text by pixels, a positive count down it; whether it moved.
		bool scroll_by(const int dy)
		{
			auto* const mc = metrics();
			if (!mc || !_styles.multi_line || dy == 0) return false;
			ensure_layout(*mc);
			const auto before = _scroll_y;
			_scroll_y += dy;
			clamp_scroll(text_bounds(mc->scale_factor));
			if (_scroll_y == before) return false;
			invalidate();
			// An input method's window follows the caret, which moved with the text.
			if (const auto in = input()) in->caret_moved(*this);
			return true;
		}

		size_t pos_at(const core::pointi loc) const
		{
			auto* const mc = metrics();
			if (!mc) return _model.caret();
			ensure_layout(*mc);
			if (_lines.empty()) return 0;

			const auto inner = text_bounds(mc->scale_factor);
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

			const auto inner = text_bounds(mc->scale_factor);
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

			const auto inner = text_bounds(mc->scale_factor);
			const auto line = line_of(_model.caret());
			const auto x0 = _styles.multi_line ? inner.left : origin_x(inner);
			const auto x = x0 + x_of(_lines[line], _model.caret());
			const auto y = line_top(inner, line);
			return {x, y, x + std::max(1, core::round(mc->scale_factor)), y + _line_height};
		}

		///////////////////////////////////////////////////////////////////////////////////////////
		// The scrollbar
		///////////////////////////////////////////////////////////////////////////////////////////

		core::recti thumb_bounds(const double s) const
		{
			scrollbar bar;
			bar.set_dpi_scale(s);
			bar.update(content_height(), text_bounds(s).height(), _scroll_y);
			const auto track = scrollbar_track(s);
			const auto span = bar.thumb(track.top, track.height());
			return {track.left, span.start, track.right, span.start + span.length};
		}

		// Right of the text, down the whole edit, while it shows a scrollbar.
		bool on_scrollbar(const core::pointi loc) const
		{
			auto* const mc = metrics();
			if (!mc) return false;
			ensure_layout(*mc);
			return _scrollbar && bounds.contains(loc) && loc.x >= text_bounds(mc->scale_factor).right;
		}

		// A press on the scrollbar holds its thumb, or pages towards the press from it. The text and the
		// selection stay as they were.
		bool press_scrollbar(const core::pointi loc)
		{
			if (!on_scrollbar(loc)) return false;

			const auto s = scale();
			const auto thumb = thumb_bounds(s);
			const auto page = std::max(_line_height, text_bounds(s).height() - _line_height);

			if (loc.y < thumb.top) scroll_by(-page);
			else if (loc.y >= thumb.bottom) scroll_by(page);
			else
			{
				_scrolling = true;
				_scroll_start_mouse = loc.y;
				_scroll_start_y = _scroll_y;
				invalidate();
			}

			return true;
		}

		void drag_thumb(const core::pointi loc)
		{
			const auto track = scrollbar_track(scale());
			if (track.height() <= 0) return;
			const auto target = _scroll_start_y +
			                    scale_distance(loc.y - _scroll_start_mouse, content_height(), track.height());
			scroll_by(target - _scroll_y);
		}

		void hover_scrollbar(const bool over)
		{
			if (_scrollbar_hover == over) return;
			_scrollbar_hover = over;
			invalidate();
		}

		///////////////////////////////////////////////////////////////////////////////////////////
		// Changes
		///////////////////////////////////////////////////////////////////////////////////////////

		void respell() { _unknown = unknown_words(_model.text(), _styles.spelling); }

		void reset_caret() { _caret_visible = true; }

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
			report_control_used(*this);
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

		bool can_copy() const { return _model.has_selection() && !_styles.password; }

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

		bool popup_open() const { return _popup && !_popup->items.empty(); }

		void close_popup()
		{
			if (!_popup) return;
			_popup->items.clear();
			_popup->selected = -1;
			_popup->dragging = false;
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
				_popup->frame = h->create_popup(_popup, {style::colors.edit_background, style::colors.edit_text,
				                                         style::colors.dialog_selected_background});
			}

			if (!_popup->frame) return;

			if (items.size() > completion_limit) items.resize(completion_limit);

			const auto s = mc->scale_factor;
			const auto row_height = mc->text_line_height(_styles.font) + drawn::scaled(6, s);
			const auto edge = std::max(1, core::round(s));
			const auto rows = std::min(static_cast<int>(items.size()), completion_rows);
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
			_popup->top = 0;
			_popup->rows = rows;
			_popup->row_height = row_height;
			_popup->scale = s;
			_popup->extent = r.extent();
			_popup->dragging = false;
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

		// The folders listed for the typed path's folder whose names start with what follows its last separator.
		std::vector<std::string> folder_matches() const
		{
			const auto prefix = std::string_view(_model.text()).substr(_folders_of.size());
			std::vector<std::string> result;

			for (const auto& name : _folders)
			{
				if (!starts_with_ignoring_case(name, prefix)) continue;
				result.emplace_back(_folders_of + name);
				if (result.size() >= completion_limit) break;
			}

			return result;
		}

		// Lists a folder on a worker, since a slow or sleeping drive, or a computer on the network, can take seconds
		// to answer. One folder is listed at a time; when it is done, the path typed by then decides what is shown or
		// listed next, so those typed in between are passed over.
		void list_folders(std::string folder)
		{
			if (!_listing.empty()) return;
			_listing = folder;

			auto lister = replaced_folder_lister();
			if (!lister) lister = list_folder_names;

			platform::run_async([edit = std::weak_ptr(std::static_pointer_cast<drawn_edit>(shared_from_this())),
			                     folder = std::move(folder), lister = std::move(lister)]() mutable
			{
				auto names = lister(folder);
				platform::run_ui([edit, folder = std::move(folder), names = std::move(names)]() mutable
				{
					if (const auto self = edit.lock()) self->folders_listed(std::move(folder), std::move(names));
				});
			});
		}

		void folders_listed(std::string folder, std::vector<std::string> names)
		{
			_listing.clear();
			_folders_of = std::move(folder);
			_folders = std::move(names);
			update_completions();
		}

		void update_completions()
		{
			if (!is_focused() || _styles.multi_line) return close_popup();

			if (_styles.file_system_auto_complete)
			{
				const auto& text = _model.text();
				const auto split = text.find_last_of("\\/");
				if (split == std::string::npos) return close_popup();

				auto folder = text.substr(0, split + 1);
				if (folder == _folders_of) return show_popup(folder_matches());

				// Another folder's names would mislead while this one's are listed.
				close_popup();
				list_folders(std::move(folder));
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

		// The list drops whole on Down, as the native one did.
		bool drops_list() const
		{
			return !_styles.multi_line && !_styles.file_system_auto_complete && !_auto_completes.empty() &&
			       !list_matches(_model.text().empty()).empty();
		}

		bool popup_key(const char32_t k, const platform::key_state keys)
		{
			if (keys.alt || keys.control) return false;

			if (!popup_open())
			{
				if (k == platform::keys::DOWN && drops_list())
				{
					show_popup(list_matches(_model.text().empty()));
					return popup_open();
				}

				return false;
			}

			const auto count = static_cast<int>(_popup->items.size());

			if (k == platform::keys::DOWN || k == platform::keys::UP || k == platform::keys::NEXT ||
			    k == platform::keys::PRIOR)
			{
				// Up from the first row goes back to what was typed; a page stops at either end.
				const auto page = std::max(1, _popup->rows);
				const auto paging = k == platform::keys::NEXT || k == platform::keys::PRIOR;
				const auto step = k == platform::keys::DOWN   ? 1
				                  : k == platform::keys::UP   ? -1
				                  : k == platform::keys::NEXT ? page
				                                              : -page;
				_popup->selected = std::clamp(_popup->selected + step, paging ? 0 : -1, count - 1);
				_popup->reveal(_popup->selected);
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
			return std::max(1, text_bounds(mc->scale_factor).height() / _line_height);
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

		std::string window_text() const { return _model.text(); }

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

			// A multi-line edit without the focus shows text it is given from the first line; its caret comes into view
			// with the focus.
			if (_styles.multi_line && !is_focused())
			{
				_scroll_y = 0;
				_reveal_caret = false;
				reset_caret();
				invalidate();
				return;
			}

			caret_changed();
		}

		void select(const int start, const int end)
		{
			const auto size = _model.text().size();
			const auto offset = [size](const int v)
			{
				return v < 0 ? size : std::min(static_cast<size_t>(v), size);
			};
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

		void auto_completes(const std::vector<std::string>& texts) { _auto_completes = texts; }

		void enabled_changed() override
		{
			control::enabled_changed();
			if (!enabled) close_popup();
		}

		///////////////////////////////////////////////////////////////////////////////////////////
		// control
		///////////////////////////////////////////////////////////////////////////////////////////

		// As large as its owner makes it.
		core::sizei measure_content(render::measure_context&, int) const override { return bounds.extent(); }

		platform::cursor cursor_at(const core::pointi loc) const override
		{
			return on_scrollbar(loc) ? platform::cursor::normal : platform::cursor::text_select;
		}

		void scale_changed() override { _layout_valid = false; }

		void on_focus(const bool focused, const bool select_all) override
		{
			if (focused)
			{
				if (select_all || _styles.select_all_on_focus) _model.select_all();
				_goal_x = -1;
				reset_caret();
				// The keyboard brings the caret into view. A press puts it where it lands in the text as shown, so
				// the text is not moved from under the press.
				if (!click_took_focus) scroll_to_caret();
			}
			else
			{
				_selecting = false;
				close_popup();
				// Listed again when the focus comes back, so a folder made meanwhile is offered.
				_folders_of.clear();
				_folders.clear();
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
		}

		bool claims_key(const char32_t key, const platform::key_state keys) const override
		{
			if (keys.alt || keys.control) return false;
			if (!popup_open()) return key == platform::keys::DOWN && drops_list();
			return key == platform::keys::UP || key == platform::keys::DOWN || key == platform::keys::ESCAPE ||
			       (key == platform::keys::RETURN && _popup->selected >= 0);
		}

		bool on_key(const char32_t key, const platform::key_state keys) override
		{
			if (_styles.capture_key_down && _styles.capture_key_down(key, keys)) return true;

			if (popup_key(key, keys)) return true;
			if (keys.alt) return false;

			const auto ctrl = keys.control;
			const auto shift = keys.shift;
			const auto vertical = key == platform::keys::UP || key == platform::keys::DOWN ||
			                      key == platform::keys::PRIOR || key == platform::keys::NEXT;
			auto changed = false;

			if (ctrl && key == U'A') _model.select_all();
			else if (ctrl && (key == U'C' || key == platform::keys::INSERT)) copy();
			else if ((ctrl && key == U'X') || (shift && key == platform::keys::DEL)) changed = cut();
			else if ((ctrl && key == U'V') || (shift && key == platform::keys::INSERT)) changed = paste();
			else if (ctrl && key == U'Z')
			{
				shift ? _model.redo() : _model.undo();
				changed = true;
			}
			else if (ctrl && key == U'Y')
			{
				_model.redo();
				changed = true;
			}
			else if (key == platform::keys::LEFT) ctrl ? _model.move_word_left(shift) : _model.move_left(shift);
			else if (key == platform::keys::RIGHT) ctrl ? _model.move_word_right(shift) : _model.move_right(shift);
			else if (vertical)
			{
				if (!_styles.multi_line) return false;
				const auto page = visible_lines();
				const auto count = key == platform::keys::UP      ? -1
				                   : key == platform::keys::DOWN  ? 1
				                   : key == platform::keys::PRIOR ? -page
				                                                  : page;
				move_lines(count, shift);
			}
			else if (key == platform::keys::HOME) ctrl ? _model.move_home(shift) : move_line_end(false, shift);
			else if (key == platform::keys::END) ctrl ? _model.move_end(shift) : move_line_end(true, shift);
			else if (key == platform::keys::BACK)
			{
				ctrl ? _model.backspace_word() : _model.backspace();
				changed = true;
			}
			else if (key == platform::keys::DEL)
			{
				ctrl ? _model.delete_word() : _model.delete_forward();
				changed = true;
			}
			else if (key == platform::keys::RETURN)
			{
				// A multi-line edit that does not take Return takes Ctrl+Return as a new line, as the native one did.
				if (!_styles.multi_line || (_styles.want_return ? ctrl : !ctrl)) return false;
				_model.insert("\n");
				changed = true;
			}
			else if (key == platform::keys::APPS || (shift && key == platform::keys::F10))
			{
				show_context_menu(caret_bounds().top_left());
				return true;
			}
			else if (ctrl && (key == platform::keys::TAB || key == platform::keys::ESCAPE))
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
			if (press_scrollbar(loc)) return;

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
			if (_scrolling)
			{
				if (captured) drag_thumb(loc);
				return;
			}

			if (!captured) hover_scrollbar(on_scrollbar(loc));
			if (!captured || !_selecting) return;
			const auto anchor = _model.anchor();
			_model.select(anchor, pos_at(loc));
			caret_changed();
		}

		void on_mouse_up(const core::pointi /*loc*/, const platform::key_state /*keys*/) override
		{
			_selecting = false;
			if (!_scrolling) return;
			_scrolling = false;
			invalidate();
		}

		void on_mouse_leave() override { hover_scrollbar(false); }

		// A selection being dragged keeps what it reached, and a dragged scrollbar where it was dragged to.
		void on_capture_lost() override { on_mouse_up({}, {}); }

		bool on_wheel(const core::pointi /*loc*/, const int delta, const platform::key_state keys) override
		{
			auto* const mc = metrics();
			if (!_styles.multi_line || keys.control || !mc) return false;
			ensure_layout(*mc);
			// At either end of its text, the wheel scrolls whatever holds the edit.
			return scroll_by(-scale_distance(delta, wheel_lines * _line_height, 120));
		}

		void on_double_click(const core::pointi loc, const platform::key_state /*keys*/) override
		{
			if (press_scrollbar(loc)) return;
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
				add(t.menu_undo.sv(), true,
				    act([](drawn_edit& e)
				{
					e._model.undo();
					e.text_changed(false);
				}));
				result.emplace_back(nullptr);
			}

			add(t.menu_cut.sv(), can_copy(),
			    act([](drawn_edit& e)
			{
				if (e.cut()) e.text_changed(false);
			}));
			add(t.menu_copy.sv(), can_copy(), act([](drawn_edit& e) { e.copy(); }));
			add(t.menu_paste.sv(), true,
			    act([](drawn_edit& e)
			{
				if (e.paste()) e.text_changed(false);
			}));
			add(t.menu_delete.sv(), _model.has_selection(),
			    act([](drawn_edit& e)
			{
				e._model.erase_selection();
				e.text_changed(false);
			}));
			result.emplace_back(nullptr);
			add(t.menu_select_all.sv(), !_model.text().empty(),
			    act([](drawn_edit& e)
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
				if (focused)
					dc.draw_rounded_rect(bounds, drawn::solid(style::colors.dialog_selected_background, alpha), radius);
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

			// Laid out at a new size, an edit with the focus keeps its caret in view. Otherwise the text stays where
			// the caret, the wheel or the scrollbar left it.
			if (_reveal_caret)
			{
				if (focused) scroll_to_caret(dc);
				_reveal_caret = false;
			}

			const auto inner = text_bounds(s);
			clamp_scroll(inner);
			const auto text_color = drawn::solid(fg, alpha);

			{
				render::scoped_clip clip(dc, inner);
				paint_text(dc, inner, focused, text_color);
			}

			if (_scrollbar)
			{
				const auto thumb = thumb_bounds(s);
				const auto shade = _scrolling ? style::colors.dialog_selected_background
				                              : core::lerp(bg, fg, _scrollbar_hover ? 0.55f : 0.35f);
				dc.draw_rounded_rect(thumb, drawn::solid(shade, alpha), thumb.width() / 2);
			}
		}

		void paint_text(render::draw_context& dc, const core::recti& inner, const bool focused,
		                const core::color text_color) const
		{
			const auto s = dc.scale_factor;
			const auto alpha = dc.colors.alpha;
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

	void completion_list::on_mouse_left_button_down(const core::pointi loc, const platform::key_state /*keys*/)
	{
		if (on_bar(loc))
		{
			const auto thumb = thumb_bounds();
			if (loc.y < thumb.top) scroll_to(top - rows);
			else if (loc.y >= thumb.bottom) scroll_to(top + rows);
			else
			{
				dragging = true;
				drag_start_y = loc.y;
				drag_start_top = top;
				if (frame) frame->invalidate();
			}
			return;
		}

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

void ui::test_folder_lister(folder_lister lister)
{
	replaced_folder_lister() = std::move(lister);
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
