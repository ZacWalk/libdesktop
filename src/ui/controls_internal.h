// This file is part of libdesktop, a C++20 platform layer for desktop applications
// Copyright 2026  Zac Walker
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

// Purpose: What the drawn controls share inside the ui library: the metrics and drawing helpers every
// control uses, and the edit edit.cpp makes. Internal to controls.cpp, edit.cpp, menu.cpp and
// menu_bar.cpp.

#pragma once

#include "libdesktop/ui/controls.h"

namespace ui
{
	namespace drawn
	{
		// The native controls' metrics, in device-independent pixels, so the drawn ones keep their size.
		constexpr int base_icon_cxy = 18;
		constexpr int element_padding = 8;
		constexpr int focus_padding = 2;
		constexpr int corner_radius = 6;
		constexpr int button_padding = 8;

		inline int scaled(const int v, const double scale)
		{
			return core::round(v * scale);
		}

		inline int icon_cxy(const double scale)
		{
			return scaled(base_icon_cxy, scale);
		}

		inline core::color solid(const core::color32 c, const float alpha = 1.0f)
		{
			return {c, alpha};
		}

		void draw_icon(render::draw_context& dc, icon_index icon, core::recti bounds, core::color c);
		void draw_outline(render::draw_context& dc, core::recti bounds, core::color c, int width = 1);
		void fill_gradient(render::draw_context& dc, core::recti bounds, core::color32 top, core::color32 bottom,
		                   float alpha);
		bool is_word_char(uint32_t cp);
		// The line under a label's marked character, for the label as dc drew it in one line of the dialog
		// face from x, with its line box's top at top.
		void draw_mnemonic_underline(render::draw_context& dc, const mnemonic_label& label, int x, int top,
		                             core::color c);
		// A check mark stroked inside bounds, for a menu whose application gave the chrome no check glyph.
		void draw_check_mark(render::draw_context& dc, core::recti bounds, core::color c);
	}

	// The edit, which render_edit.cpp makes.
	edit_ptr make_drawn_edit(const edit_styles& styles, std::string_view text,
	                         std::function<void(const std::string&)> changed);
	core::recti drawn_edit_range_bounds(const edit& e, size_t start, size_t end);
	std::vector<command_ptr> drawn_edit_context_commands(edit& e, core::pointi loc);

	// Tells the application's listener (on_control_used) that a control acted for whoever works it.
	void report_control_used(const control& c, const command_ptr& command = {});
}
