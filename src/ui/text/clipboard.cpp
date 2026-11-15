// This file is part of libdesktop, a C++20 platform layer for desktop applications
// Copyright 2026  Zac Walker
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

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
			bool set_text(const std::string_view text) override { return platform::set_clipboard(text); }

			std::optional<std::string> text() override { return platform::clipboard_text(); }
		};
	}

	text_clipboard& system_clipboard()
	{
		static system_text_clipboard clipboard;
		return clipboard;
	}
}
