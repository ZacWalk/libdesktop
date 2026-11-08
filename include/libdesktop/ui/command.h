// This file is part of libdesktop, a C++20 platform layer for desktop applications
// Copyright 2026  Zac Walker
// 
// This program is free software; you can redistribute it and / or modify it
// under the terms of the LGPL License either version 2.1 or later.
// License details are available at https://www.gnu.org/licenses/lgpl-2.1.html
// This program is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY

// Purpose: UI commands, edit and toolbar styles, and chrome icons.

#pragma once

namespace ui
{
	// The dictionary an edit control checks its words against. The application supplies it, so the
	// control knows nothing of which spell checker that is.
	class spelling_dictionary
	{
	public:
		virtual ~spelling_dictionary() = default;
		virtual void queue_load() = 0;
		virtual bool is_word_valid(std::string_view word) const = 0;
		virtual std::vector<std::string> suggest(std::string_view word) const = 0;
		// done runs once the word is added, or adding it has failed, on whichever thread finished.
		virtual void add_word(std::string_view word, std::function<void()> done) = 0;
	};

	struct edit_styles
	{
		bool align_center = false;
		bool number = false;
		bool password = false;
		bool multi_line = false;
		// Words are checked against this while it is set; it outlives the control.
		spelling_dictionary* spelling = nullptr;
		// A multi-line edit takes Return as a line break, rather than leaving it to the dialog.
		bool want_return = false;
		bool file_system_auto_complete = false;
		bool rounded_corners = false;
		bool select_all_on_focus = false;
		render::font_face font = render::font_face::dialog;
		std::function<bool(int c, platform::key_state keys)> capture_key_down;
		std::vector<std::string> auto_complete_list;
		std::string cue;
	};

	struct toolbar_styles
	{
		// Buttons that do not fit the width go on to another row.
		bool wrap = false;
		core::sizei button_extent = {0, 0};
	};

	class command
	{
	public:
		command() = default;

		command(const icon_index icon, std::any opaque, std::function<void()> invoke)
			: icon(icon), opaque(std::move(opaque)), invoke(std::move(invoke))
		{
		}

		command(const std::string_view t, std::any opaque, std::function<void()> invoke, const bool ch)
			: text(t), checked(ch), opaque(std::move(opaque)), invoke(std::move(invoke))
		{
		}

		std::string text;
		std::string toolbar_text;
		std::string tooltip_text;
		// Why the command is dimmed right now; empty while it is enabled.
		std::string disabled_reason;
		std::string keyboard_accelerator_text;
		std::vector<platform::keyboard_accelerator_t> kba;

		bool visible = true;
		bool enable = true;
		bool checked = false;
		bool checkable = false;
		// Paints the toolbar button with the accent fill so it reads as the one thing asking to be pressed.
		bool highlight = false;
		// Maximizes or restores the window. A toolbar button for it is the frame's maximize caption
		// button, the one Windows 11 offers snap layouts over.
		bool caption_maximize = false;

		bool text_can_change = false;
		bool icon_can_change = false;

		command_group group{};
		// Value-initialized, which is icon_index::none: the UI knows the type and the application
		// its values.
		icon_index icon{};
		core::color32 clr = 0;

		std::any opaque;
		std::function<void()> invoke;
		std::function<std::vector<command_ptr>()> menu;

		// In a menu, the commands shown side by side, each name under its icon, as one row of buttons in
		// place of this one - as cut, copy and paste are along the top of a context menu - each chosen like
		// an item.
		std::vector<command_ptr> buttons;
		// In a menu, an element shown in place of this command's row - a heading, a preview, more about
		// what the menu acts on - which is shown, not chosen.
		element_ptr content;
	};

	// The glyphs the platform's own chrome draws - a menu's check mark, check boxes, radio buttons and
	// the caption buttons - from the icon font, whose code points the application's icon_index names.
	// The application defines it.
	struct chrome_icon_set
	{
		icon_index check{};
		icon_index checkbox_off{};
		icon_index checkbox_on{};
		icon_index radio_off{};
		icon_index radio_on{};
		icon_index close{};
		icon_index maximize{};
		icon_index minimize{};
		icon_index restore{};
	};

	void set_chrome_icons(const chrome_icon_set& icons);
	const chrome_icon_set& chrome_icons();

	// The text that draws an icon: its code point in the icon font, as UTF-8, repeated where asked.
	inline std::string icon_to_utf8(const icon_index i)
	{
		std::string result;
		core::char32_to_utf8(std::back_inserter(result), static_cast<uint32_t>(i));
		return result;
	}

	inline std::string icon_to_utf8(const icon_index i, const size_t repeat)
	{
		const auto single = icon_to_utf8(i);
		std::string text;
		text.reserve(single.size() * repeat);
		for (size_t n = 0; n < repeat; ++n) text += single;
		return text;
	}


}
