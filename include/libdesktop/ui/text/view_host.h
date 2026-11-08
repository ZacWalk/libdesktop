// This file is part of libdesktop, a C++20 platform layer for desktop applications
// Copyright 2026  Zac Walker
//
// This program is free software; you can redistribute it and / or modify it
// under the terms of the LGPL License either version 2.1 or later.
// License details are available at https://www.gnu.org/licenses/lgpl-2.1.html
// This program is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY

// Purpose: Invalidation and scrolling requests raised by text models.

#pragma once

#include "libdesktop/ui/text_types.h"


namespace ui
{
	class view_host
	{
	public:
		virtual ~view_host() = default;

		// Repaint only — the text of these lines is unchanged.
		virtual void invalidate_lines(int start, int end) = 0;

		// The text of these lines changed, so layout and highlighting must be redone.
		virtual void lines_changed(int start, int end) = 0;

		// Lines were inserted after 'at' (delta > 0) or erased from 'at' + 1
		// (delta < 0). The text of line 'at' changed too.
		virtual void line_count_changed(int at, int delta) = 0;

		// Scroll so this position is on screen.
		virtual void ensure_visible(const ::ui::text_location& pt) = 0;

		// Layout, caret and scrollbar all need recomputing.
		virtual void invalidate_view() = 0;

		// Wrap and content extents need recomputing, but the caret has not moved.
		virtual void invalidate_layout() = 0;

		// The caret moved; nothing else changed. Kept separate from invalidate_view
		// so moving the cursor does not force a relayout of the document.
		virtual void invalidate_caret() = 0;

		// The content extent changed, so the scrollbar needs recomputing.
		virtual void invalidate_scrollbar() = 0;

		// The status text or the focus band changed. Separate again because it
		// repaints chrome around the text rather than the text itself.
		virtual void invalidate_status() = 0;
	};
}
