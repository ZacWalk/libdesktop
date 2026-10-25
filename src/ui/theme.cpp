// theme.cpp — the default dark palette.
//
// Lifted from dde's style_to_color so the presentation is unchanged
// for the application it came from, and so an application adopting
// libdesktop-ui gets something usable without writing a theme first.

#include "platform.h"
#include "ui/theme.h"

namespace pf::ui
{
	pf::color_t theme::style_color(const text_style s) const
	{
		switch (s)
		{
		case text_style::white_space:
		case text_style::main_wnd_clr:
			return colors::main_wnd_clr;
		case text_style::tool_wnd_clr:
			return colors::tool_wnd_clr;
		case text_style::normal_bkgnd:
			return pf::color_t(30, 30, 30);
		case text_style::normal_text:
			return pf::color_t(222, 222, 222);
		case text_style::sel_margin:
			return pf::color_t(44, 44, 44);
		case text_style::code_preprocessor:
			return pf::color_t(133, 133, 211);
		case text_style::code_comment:
			return pf::color_t(128, 222, 128);
		case text_style::code_number:
		case text_style::code_string:
			return pf::color_t(244, 244, 144);
		case text_style::code_operator:
			return pf::color_t(128, 255, 128);
		case text_style::code_keyword:
			return pf::color_t(128, 128, 255);
		case text_style::sel_bkgnd:
			return pf::color_t(88, 88, 88);
		case text_style::sel_text:
			return pf::color_t(255, 255, 255);
		case text_style::error_bkgnd:
			return pf::color_t(128, 0, 0);
		case text_style::error_text:
			return pf::color_t(255, 100, 100);
		case text_style::md_heading1:
			return pf::color_t(100, 200, 255);
		case text_style::md_heading2:
			return pf::color_t(140, 180, 255);
		case text_style::md_heading3:
			return pf::color_t(180, 160, 255);
		case text_style::md_bold:
			return pf::color_t(255, 255, 255);
		case text_style::md_italic:
			return pf::color_t(180, 220, 180);
		case text_style::md_code:
			return pf::color_t(215, 186, 125);
		case text_style::md_link_text:
			return pf::color_t(100, 180, 255);
		case text_style::md_link_url:
			return pf::color_t(120, 120, 120);
		case text_style::md_marker:
			return pf::color_t(80, 80, 80);
		case text_style::md_bullet:
			return pf::color_t(200, 200, 100);
		case text_style::md_quote:
			// Quoted text reads as text by default. An application that uses a
			// quote to mean something — a chat transcript marking whose turn it
			// is — overrides this; a document renderer should not have its
			// blockquotes recoloured for it.
			return pf::color_t(222, 222, 222);
		}

		return pf::color_t(222, 222, 222);
	}

	pf::font theme::heading_font(const int level) const
	{
		switch (level)
		{
		case 1: return {text_font_height + 12, pf::font_name::consolas};
		case 2: return {text_font_height + 8, pf::font_name::consolas};
		case 3: return {text_font_height + 4, pf::font_name::consolas};
		default: return text_font;
		}
	}
}
