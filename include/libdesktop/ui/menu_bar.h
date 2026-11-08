// This file is part of libdesktop, a C++20 platform layer for desktop applications
// Copyright 2026  Zac Walker
//
// This program is free software; you can redistribute it and / or modify it
// under the terms of the LGPL License either version 2.1 or later.
// License details are available at https://www.gnu.org/licenses/lgpl-2.1.html
// This program is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY

// Purpose: The portable drawn menu bar control and recursive command accelerator dispatch.

#pragma once

#include "libdesktop/ui/controls.h"

namespace ui
{
	// Creates a row of top-level commands. Each command supplies its title and menu through menu().
	control_ptr make_menu_bar(std::vector<command_ptr> menus);

	// Invokes the first enabled, visible command whose keyboard accelerator matches key and keys.
	bool dispatch_accelerator(const std::vector<command_ptr>& commands, char32_t key, platform::key_state keys);
}
