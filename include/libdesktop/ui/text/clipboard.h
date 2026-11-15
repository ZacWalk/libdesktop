// This file is part of libdesktop, a C++20 platform layer for desktop applications
// Copyright 2026  Zac Walker
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

// Purpose: Testable text-only clipboard seam for UI views.

#pragma once

#include <optional>
#include <string>
#include <string_view>

namespace ui
{
	class text_clipboard
	{
	public:
		virtual ~text_clipboard() = default;
		virtual bool set_text(std::string_view text) = 0;
		virtual std::optional<std::string> text() = 0;
	};

	text_clipboard& system_clipboard();
}
