// view_base.h — scrolling state shared by every view.
//
// Holds the viewport, the content extent and the scroll offset in pixels, plus the
// two scrollbar widgets. Everything above this deals in lines and characters and
// converts down to pixels here.

#pragma once

#include "ui/widgets.h"

#include <algorithm>

namespace pf::ui
{
	class view_base : public pf::frame_reactor
	{
	protected:
		custom_scrollbar _hscroll{custom_scrollbar::orientation::horizontal};
		custom_scrollbar _vscroll{custom_scrollbar::orientation::vertical};

		pf::isize _view_extent = {};    // viewable area in pixels, set on size
		pf::isize _content_extent = {}; // total content size in pixels, set by subclasses
		pf::ipoint _scroll_offset = {}; // current scroll offset in pixels

		void clamp_scroll_offset()
		{
			_scroll_offset.x = std::clamp(_scroll_offset.x, 0, std::max(0, _content_extent.cx - _view_extent.cx));
			_scroll_offset.y = std::clamp(_scroll_offset.y, 0, std::max(0, _content_extent.cy - _view_extent.cy));
		}

		[[nodiscard]] pf::irect client_rect() const
		{
			return pf::irect(0, 0, _view_extent.cx, _view_extent.cy);
		}

		[[nodiscard]] int max_scroll_y() const { return std::max(0, _content_extent.cy - _view_extent.cy); }

	public:
		// Exposed so a test can assert what the scrollbar was told without drawing.
		[[nodiscard]] const custom_scrollbar& vert_scrollbar() const { return _vscroll; }

		// Where the content currently sits under the viewport. A host syncing its
		// own scrollbar reads this, and it is what makes "the caret scrolled into
		// view" observable without drawing.
		[[nodiscard]] pf::ipoint scroll_offset() const { return _scroll_offset; }
	};
}
