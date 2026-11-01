// Windows-backend interop for applications that deliberately own a native renderer or control.
// Keep this header out of portable platform.h: the returned value is an opaque native-window
// token, and only a Windows-specific translation unit should interpret it.

#pragma once

#include "platform.h"

namespace pf::platform_win
{
	// Returns the native top-level/child window token for a libdesktop frame, or zero when the
	// frame is not a Windows frame. Portable code must not call this accessor.
	uintptr_t native_window_handle(const pf::window_frame_ptr& frame);

	// Changes the native frame mode without making fullscreen a portable window type. False means
	// the frame was not a Windows frame; a valid Windows frame reports true after applying it.
	bool set_fullscreen(const pf::window_frame_ptr& frame, bool fullscreen);
}
