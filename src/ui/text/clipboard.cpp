// This file is part of libdesktop, a C++20 platform layer for desktop applications
// Copyright 2026  Zac Walker
//
// This program is free software; you can redistribute it and / or modify it
// under the terms of the LGPL License either version 2.1 or later.
// License details are available at https://www.gnu.org/licenses/lgpl-2.1.html
// This program is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY

// Purpose: Adapter from the UI clipboard seam to platform text clipboard services.

#include "libdesktop/ui/text/clipboard.h"
#include "libdesktop/platform/services.h"

namespace ui
{
	namespace
	{
		class system_text_clipboard final : public text_clipboard
		{
		public:
			bool set_text(const std::string_view text) override
			{
				return platform::set_clipboard(text);
			}

			std::optional<std::string> text() override
			{
				return platform::clipboard_text();
			}
		};
	}

	text_clipboard& system_clipboard()
	{
		static system_text_clipboard clipboard;
		return clipboard;
	}
}
