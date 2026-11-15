// This file is part of libdesktop, a C++20 platform layer for desktop applications
// Copyright 2026  Zac Walker
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

// Purpose: One flex-backed, clipped document panel for text, Markdown, dumps and other document contents.

#pragma once

#include "libdesktop/ui/elements.h"

namespace ui
{
	class document_panel : public panel_element
	{
	public:
		explicit document_panel(const element_ptr& content, const element_options& options = flex_item::stretch) :
			panel_element(options)
		{
			flex_container.direction = flex_direction::column;
			flex_container.wrap = flex_wrap::no_wrap;
			flex_container.align_items = flex_align::stretch;
			if (content)
			{
				content->flex.align_self = flex_align::stretch;
				content->flex.grow = 1.0f;
				content->flex.shrink = 1.0f;
				add(content);
			}
		}
	};
}
