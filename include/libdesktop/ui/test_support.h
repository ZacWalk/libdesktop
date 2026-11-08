// This file is part of libdesktop, a C++20 platform layer for desktop applications
// Copyright 2026  Zac Walker
//
// This program is free software; you can redistribute it and / or modify it
// under the terms of the LGPL License either version 2.1 or later.
// License details are available at https://www.gnu.org/licenses/lgpl-2.1.html
// This program is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY

// Purpose: Headless render, host, and clipboard fakes for UI model tests.

#pragma once

#include "libdesktop/ui/controls.h"
#include "libdesktop/ui/text/clipboard.h"
#include "libdesktop/ui/text/view_host.h"

#include <algorithm>
#include <cstdint>
#include <memory>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace ui::test
{
	// A fixed character cell keeps layout tests independent of the installed font.
	struct grid_metrics
	{
		int char_width = 8;
		int char_height = 16;

		[[nodiscard]] static int codepoint_count(const std::string_view text)
		{
			int count = 0;
			for (auto it = text.begin(); it != text.end();)
			{
				const auto before = it;
				(void)core::pop_utf8_char(it, text.end());
				if (it == before) break;
				++count;
			}
			return count;
		}

		[[nodiscard]] core::sizei measure(const std::string_view text) const
		{
			return {codepoint_count(text) * char_width, char_height};
		}
	};

	class fake_measure_context : public render::measure_context
	{
	public:
		grid_metrics metrics;

		fake_measure_context() = default;
		explicit fake_measure_context(const grid_metrics m) : metrics(m) {}

		core::sizei measure_text(std::string_view text, render::font_face, render::text_style, int,
		                         int = 0) override
		{
			return metrics.measure(text);
		}

		[[nodiscard]] core::sizei measure_text(std::string_view text, render::font_face, render::text_style,
		                                      int, int = 0) const
		{
			return metrics.measure(text);
		}

		[[nodiscard]] core::sizei measure_text(std::string_view text, render::font_face) const
		{
			return metrics.measure(text);
		}

		[[nodiscard]] core::sizei measure_char(render::font_face) const
		{
			return {metrics.char_width, metrics.char_height};
		}

		int text_line_height(render::font_face) override { return metrics.char_height; }
		[[nodiscard]] int text_line_height(render::font_face) const { return metrics.char_height; }
		render::text_layout_ptr create_text_layout(render::font_face) override { return nullptr; }
	};

	// Records drawing instead of requiring a window or graphics device.
	class fake_draw_context : public render::draw_context
	{
	public:
		struct text_draw
		{
			int x = 0;
			int y = 0;
			std::string text;
			core::color color;
			core::color background;
			core::recti clip;
			render::font_face font = render::font_face::dialog;
		};

		struct rect_fill
		{
			core::recti rect;
			core::color color;
		};

		struct line_draw
		{
			core::pointd a;
			core::pointd b;
			core::color color;
			double width = 1.0;
		};

		struct polyline_draw
		{
			std::vector<core::pointd> points;
			core::color color;
			double width = 1.0;
		};

		struct ellipse_draw
		{
			core::recti bounds;
			core::color color;
			double width = 1.0;
			bool filled = false;
		};

		struct polygon_fill
		{
			std::vector<core::pointd> points;
			core::color color;
		};

		struct shadow_draw
		{
			core::recti bounds;
			int width = 0;
			float alpha = 0.0f;
		};

		grid_metrics metrics;
		std::vector<text_draw> texts;
		std::vector<rect_fill> fills;
		std::vector<line_draw> lines;
		std::vector<polyline_draw> polylines;
		std::vector<ellipse_draw> ellipses;
		std::vector<polygon_fill> polygons;
		std::vector<shadow_draw> shadows;

		fake_draw_context() : _clip(0, 0, 1024, 768) {}
		explicit fake_draw_context(const core::recti clip) : _clip(clip) {}

		void reset()
		{
			texts.clear();
			fills.clear();
			lines.clear();
			polylines.clear();
			ellipses.clear();
			polygons.clear();
			shadows.clear();
			_clip_stack.clear();
		}

		[[nodiscard]] std::string drawn_text() const
		{
			std::string result;
			for (const auto& text : texts) result += text.text;
			return result;
		}

		void clear(const core::color color) override { fills.push_back({_clip, color}); }
		void draw_rounded_rect(const core::recti bounds, const core::color color, int) override
		{
			fills.push_back({bounds, color});
		}
		void draw_rect(const core::recti bounds, const core::color color) override
		{
			fills.push_back({bounds, color});
		}
		void fill_polygon(const std::span<const core::pointd> points, const core::color color) override
		{
			polygons.push_back({std::vector<core::pointd>(points.begin(), points.end()), color});
		}
		void draw_shadow(const core::recti bounds, const int width, const float alpha) override
		{
			shadows.push_back({bounds, width, alpha});
		}

		void draw_text(const std::string_view text, const core::recti bounds, const render::font_face font,
		               render::text_style, const core::color color, const core::color background) override
		{
			texts.push_back({bounds.left, bounds.top, std::string(text), color, background, bounds, font});
		}

		void draw_text(const std::string_view text, const std::vector<render::text_highlight_t>&, const core::recti bounds,
		               const render::font_face font, render::text_style, const core::color color,
		               const core::color background) override
		{
			texts.push_back({bounds.left, bounds.top, std::string(text), color, background, bounds, font});
		}

		void draw_text(const render::text_layout_ptr&, const core::recti bounds, const core::color color,
		               const core::color background) override
		{
			texts.push_back({bounds.left, bounds.top, {}, color, background, bounds, render::font_face::dialog});
		}

		void draw_border(const core::recti inside, const core::recti outside, const core::color c_inside,
		                 const core::color c_outside) override
		{
			fills.push_back({outside, c_outside});
			fills.push_back({inside, c_inside});
		}

		void draw_texture(const render::texture_ptr&, core::recti, float, render::texture_sampler) override {}
		void draw_texture(const render::texture_ptr&, core::recti, core::recti, float, render::texture_sampler,
		                  float) override {}
		void draw_texture(const render::texture_ptr&, const core::quadd&, core::recti, float,
		                  render::texture_sampler) override {}
		void draw_texture(const render::texture_ptr&, const core::quadd&, core::recti, float,
		                  render::texture_sampler, const render::texture_transform&) override {}
		void draw_vertices(const render::vertices_ptr&) override {}

		core::sizei measure_text(std::string_view text, render::font_face, render::text_style, int,
		                         int = 0) override
		{
			return metrics.measure(text);
		}
		core::sizei measure_text(std::string_view text, render::font_face) const
		{
			return metrics.measure(text);
		}
		core::sizei measure_char(render::font_face) const
		{
			return {metrics.char_width, metrics.char_height};
		}
		int text_line_height(render::font_face) override { return metrics.char_height; }
		render::texture_ptr create_texture() override { return nullptr; }
		render::vertices_ptr create_vertices() override { return nullptr; }
		render::text_layout_ptr create_text_layout(render::font_face) override { return nullptr; }

		core::recti clip_bounds() const override { return _clip; }
		void clip_bounds(const core::recti bounds) override
		{
			_clip_stack.push_back(_clip);
			_clip = bounds;
		}
		void restore_clip() override
		{
			if (!_clip_stack.empty())
			{
				_clip = _clip_stack.back();
				_clip_stack.pop_back();
			}
		}

		void draw_line(const core::pointd a, const core::pointd b, const core::color color,
		               const double width = 1.0) override
		{
			lines.push_back({a, b, color, width});
		}
		void draw_polyline(const std::span<const core::pointd> points, const core::color color,
		                   const double width = 1.0) override
		{
			polylines.push_back({std::vector<core::pointd>(points.begin(), points.end()), color, width});
		}
		void draw_ellipse(const core::recti bounds, const core::color color, const double width = 1.0) override
		{
			ellipses.push_back({bounds, color, width, false});
		}
		void fill_ellipse(const core::recti bounds, const core::color color) override
		{
			ellipses.push_back({bounds, color, 0.0, true});
		}

	private:
		core::recti _clip;
		std::vector<core::recti> _clip_stack;
	};

	class recording_view_host : public view_host
	{
	public:
		struct line_range
		{
			int start = 0;
			int end = 0;
		};

		std::vector<line_range> invalidated;
		std::vector<line_range> changed;
		std::vector<std::pair<int, int>> count_changes;
		std::vector<text_location> ensured;
		int invalidate_view_count = 0;
		int invalidate_layout_count = 0;
		int invalidate_caret_count = 0;
		int invalidate_scrollbar_count = 0;
		int invalidate_status_count = 0;

		void reset()
		{
			invalidated.clear();
			changed.clear();
			count_changes.clear();
			ensured.clear();
			invalidate_view_count = 0;
			invalidate_layout_count = 0;
			invalidate_caret_count = 0;
			invalidate_scrollbar_count = 0;
			invalidate_status_count = 0;
		}

		void invalidate_lines(const int start, const int end) override { invalidated.push_back({start, end}); }
		void lines_changed(const int start, const int end) override { changed.push_back({start, end}); }
		void line_count_changed(const int at, const int delta) override { count_changes.emplace_back(at, delta); }
		void ensure_visible(const text_location& pt) override { ensured.push_back(pt); }
		void invalidate_view() override { ++invalidate_view_count; }
		void invalidate_layout() override { ++invalidate_layout_count; }
		void invalidate_caret() override { ++invalidate_caret_count; }
		void invalidate_scrollbar() override { ++invalidate_scrollbar_count; }
		void invalidate_status() override { ++invalidate_status_count; }
	};

	class fake_control_host final : public control_host
	{
	public:
		fake_measure_context measure_context;
		bool focused = false;
		bool focus = false;
		int focus_count = 0;
		bool captured = false;
		bool capture = false;
		int capture_count = 0;
		platform::cursor cursor = platform::cursor::normal;
		core::pointi screen_origin;
		std::vector<core::recti> invalidated;
		std::vector<core::recti> invalidations;
		bool text_input_active = false;
		bool text_input_enabled = false;
		core::recti text_input_caret;
		core::recti caret;
		bool caret_timer_active = false;
		bool caret_timer_on = false;
		uint32_t caret_timer_interval = 0;
		uint32_t caret_interval = 0;
		core::recti tracked_menu_bounds;
		std::vector<command_ptr> tracked_menu;
		std::vector<command_ptr> last_menu;
		bool closed = false;
		bool close_cancel = false;
		bool dialog = false;

		void reset()
		{
			focused = focus = false;
			focus_count = 0;
			captured = capture = false;
			capture_count = 0;
			invalidated.clear();
			invalidations.clear();
			tracked_menu.clear();
			last_menu.clear();
			text_input_active = text_input_enabled = false;
			caret_timer_active = caret_timer_on = false;
			closed = false;
			close_cancel = false;
		}

		render::measure_context* measure() override { return &measure_context; }
		void invalidate(const core::recti bounds) override
		{
			invalidated.push_back(bounds);
			invalidations.push_back(bounds);
		}
		bool has_keyboard_focus() const override { return focused || focus; }
		void take_keyboard_focus() override { focused = focus = true; ++focus_count; }
		void capture_pointer(const bool value) override
		{
			captured = capture = value;
			++capture_count;
		}
		void set_cursor(const platform::cursor value) override { cursor = value; }
		core::pointi client_to_screen(const core::pointi loc) const override
		{
			return {loc.x + screen_origin.x, loc.y + screen_origin.y};
		}
		void track_menu(const core::recti bounds, const std::vector<command_ptr>& commands) override
		{
			tracked_menu_bounds = bounds;
			tracked_menu = commands;
			last_menu = commands;
		}
		frame_ptr create_popup(const frame_host_weak_ptr&, const render::color_style&) override { return nullptr; }
		std::shared_ptr<frame_host> listener() const override { return nullptr; }
		void text_input(const bool active, const core::recti caret) override
		{
			text_input_active = text_input_enabled = active;
			text_input_caret = this->caret = caret;
		}
		void caret_timer(const bool on, const uint32_t interval_ms) override
		{
			caret_timer_active = caret_timer_on = on;
			caret_timer_interval = caret_interval = interval_ms;
		}
		bool is_dialog() const override { return dialog; }
		void close(const bool cancel) override { closed = true; close_cancel = cancel; }
	};

	class fake_clipboard final : public text_clipboard
	{
	public:
		std::string clipboard;
		bool writable = true;
		bool readable = true;

		bool set_text(const std::string_view text) override
		{
			if (!writable) return false;
			clipboard.assign(text);
			return true;
		}

		std::optional<std::string> text() override
		{
			if (!readable) return std::nullopt;
			return clipboard;
		}
	};
}
