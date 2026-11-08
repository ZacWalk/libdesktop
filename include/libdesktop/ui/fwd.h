// This file is part of libdesktop, a C++20 platform layer for desktop applications
// Copyright 2026  Zac Walker
// 
// This program is free software; you can redistribute it and / or modify it
// under the terms of the LGPL License either version 2.1 or later.
// License details are available at https://www.gnu.org/licenses/lgpl-2.1.html
// This program is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY

// Purpose: Opaque UI enums, forward declarations, and shared UI pointer aliases.

#pragma once

enum class icon_index;
// The application's grouping of its commands, which a command carries for it. The type is the UI's,
// its values the application's, as icon_index's are; value-initialized it is the application's none.
enum class command_group;

// The application's commands, by which an element asks its host to run one. The type is the UI's, its
// values the application's, as icon_index's are; value-initialized it is the application's none.
enum class commands;
namespace ui
{
	class app;
	class bubble_frame;
	class button;
	class control;
	class element;
	class edit;
	class command;
	class toolbar;
	class trackbar;
	class date_time_control;
	class frame_host;
	class frame;
	class measure_context;

	using app_ptr = std::shared_ptr<app>;
	using weak_app_ptr = std::weak_ptr<app>;
	using bubble_window_ptr = std::shared_ptr<bubble_frame>;
	using button_ptr = std::shared_ptr<button>;
	using control_ptr = std::shared_ptr<control>;
	using element_ptr = std::shared_ptr<element>;
	using edit_ptr = std::shared_ptr<edit>;
	using command_ptr = std::shared_ptr<command>;
	using toolbar_ptr = std::shared_ptr<toolbar>;
	using trackbar_ptr = std::shared_ptr<trackbar>;
	using date_time_control_ptr = std::shared_ptr<date_time_control>;
	using frame_host_weak_ptr = std::weak_ptr<frame_host>;
	using frame_ptr = std::shared_ptr<frame>;
	using frame_weak_ptr = std::weak_ptr<frame>;
}
