// This file is part of libdesktop, a C++20 platform layer for desktop applications
// Copyright 2026  Zac Walker
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

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
