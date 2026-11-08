// This file is part of libdesktop, a C++20 platform layer for desktop applications
// Copyright 2026  Zac Walker
//
// This program is free software; you can redistribute it and / or modify it
// under the terms of the LGPL License either version 2.1 or later.
// License details are available at https://www.gnu.org/licenses/lgpl-2.1.html
// This program is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY

// Purpose: Native message boxes, choice, and modal dialogs.

#pragma once

namespace platform
{
	class window;

	enum class message_box_buttons
	{
		ok,
		ok_cancel,
		yes_no,
		yes_no_cancel,
		retry_cancel
	};


	enum class message_box_icon
	{
		none,
		info,
		warning,
		error,
		question
	};

	struct message_box_style
	{
		message_box_buttons buttons = message_box_buttons::ok;
		message_box_icon icon = message_box_icon::none;
	};

	enum class message_box_result
	{
		none,
		ok,
		cancel,
		yes,
		no,
		retry
	};

	message_box_result message_box(window* owner, std::string_view text, std::string_view title,
	                              message_box_style style = {});

	struct choice_definition
	{
		std::string title;
		std::string heading;
		std::string message;
		std::vector<std::pair<int, std::string>> buttons;
		int default_id = 0;
		bool warning = false;
	};

	// Returns the selected button id, or zero when cancelled or unavailable.
	int show_choice(window* owner, const choice_definition& definition);

	enum class modal_field_kind
	{
		text,
		check_box,
		choice,
		search_list,
		action
	};

	using modal_value = std::variant<std::string, bool, int64_t>;

	struct modal_preview
	{
		int width = 0;
		int height = 0;
		std::vector<uint32_t> bgra_pixels;
	};

	struct modal_field
	{
		uint32_t id = 0;
		modal_field_kind kind = modal_field_kind::text;
		std::string label;
		modal_value value = std::string{};
		std::vector<std::pair<std::string, int64_t>> choices;
		std::function<void()> action;
		bool enabled = true;
	};

	struct modal_definition
	{
		std::string title;
		std::string message;
		std::vector<modal_field> fields;
		std::vector<modal_preview> previews;
		std::string accept = "OK";
		std::string cancel = "Cancel";
		std::function<std::optional<std::string>(const std::map<uint32_t, modal_value>&)> validate;
	};

	std::optional<std::map<uint32_t, modal_value>> show_modal_dialog(window* owner, modal_definition definition);

}
