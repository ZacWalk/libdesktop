// theme.h — the colours, fonts and spacing platform-ui draws with.
//
// `colors` is the raw palette. `theme` is what a view is handed: metrics as data,
// because every consumer wants to set them, and the style-to-colour mapping as a
// virtual, because an application may want its own syntax palette without
// touching a highlighter.

#pragma once

#include "platform.h"
#include "ui/text_types.h"

namespace pf::ui::colors
{
	constexpr pf::color_t handle_color{0x44, 0x44, 0x44};
	constexpr pf::color_t focus_handle_color{40, 100, 220};
	constexpr pf::color_t main_wnd_clr{0x22, 0x22, 0x22};
	constexpr pf::color_t tool_wnd_clr{0x33, 0x33, 0x33};
	constexpr pf::color_t handle_hover_color{0x66, 0x66, 0x66};
	constexpr pf::color_t handle_tracking_color{0x11, 0x66, 0xCC};
	constexpr pf::color_t text_color{0xFF, 0xFF, 0xFF};
	constexpr pf::color_t folder_text_color{0xEE, 0xCC, 0x22};
	constexpr pf::color_t darker_text_color{0xCC, 0xCC, 0xCC};
	constexpr pf::color_t line_color{0x22, 0x55, 0xAA};
	constexpr pf::color_t window_background{33, 33, 33};
}

namespace pf::ui
{
	// What a view is handed to draw itself. Construct one, adjust the metrics,
	// and pass it in; override style_color for a different syntax palette.
	class theme
	{
	public:
		virtual ~theme() = default;

		double dpi_scale = 1.0;

		pf::font list_font{20, pf::font_name::calibri};
		pf::font edit_font{30, pf::font_name::calibri};
		// Text views lay out, hit-test and draw on a code-point grid of
		// measure_char() cells, so text_font must be fixed-pitch: a proportional
		// face overlaps wide glyphs, leaves gaps after narrow ones and puts the
		// caret beside the text.
		pf::font text_font{24, pf::font_name::consolas};

		int list_font_height = 20;
		int text_font_height = 24;

		int padding_x = 5;
		int padding_y = 5;
		int indent = 16;

		int edit_box_margin = 6;
		int edit_box_inner_pad = 4;

		int list_top_pad = 4;
		int list_scroll_pad = 64;

		// Chrome — the colours around the text rather than in it.
		pf::color_t window_background = colors::window_background;
		pf::color_t tool_background = colors::tool_wnd_clr;
		pf::color_t text = colors::text_color;
		pf::color_t dim_text = colors::darker_text_color;
		pf::color_t group_text = colors::folder_text_color;
		pf::color_t line = colors::line_color;
		pf::color_t match_highlight{220, 140, 0};
		pf::color_t header_background = colors::tool_wnd_clr.darken(8);
		pf::color_t handle = colors::handle_color;
		pf::color_t handle_hover = colors::handle_hover_color;
		pf::color_t handle_tracking = colors::handle_tracking_color;
		pf::color_t focus_handle = colors::focus_handle_color;

		// The colour of a run of styled text. The default is the dark palette
		// rethinkify-app shipped; override to repalette without touching a
		// highlighter.
		[[nodiscard]] virtual pf::color_t style_color(text_style s) const;

		// The font a markdown heading is drawn in, for levels 1 to 3; anything else
		// is body text. Headings are the one place a text view stops being a fixed
		// grid, so their sizes are metrics like the rest rather than constants
		// buried in a view.
		[[nodiscard]] virtual pf::font heading_font(int level) const;
	};
}

