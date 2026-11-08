// This file is part of libdesktop, a C++20 platform layer for desktop applications
// Copyright 2026  Zac Walker
// 
// This program is free software; you can redistribute it and / or modify it
// under the terms of the LGPL License either version 2.1 or later.
// License details are available at https://www.gnu.org/licenses/lgpl-2.1.html
// This program is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY

// Purpose: The window plumbing platform_win_ui.cpp builds on - rectangles, window text, the scale
// owner_context hands its windows, and the window base class. Windows-only and internal to it.

#pragma once

#include "libdesktop/platform_win.h"
#include "libdesktop/platform_win_render_res.h"

static constexpr auto ui_base_icon_cxy = 18;

inline int calc_icon_cxy(const double scale_factor)
{
	return df::round(ui_base_icon_cxy * scale_factor);
}

#ifndef GET_X_LPARAM
#define GET_X_LPARAM(lp)                        ((int)(short)LOWORD(lp))
#endif
#ifndef GET_Y_LPARAM
#define GET_Y_LPARAM(lp)                        ((int)(short)HIWORD(lp))
#endif

struct win_rect : RECT
{
	win_rect() noexcept
	{
		left = 0;
		top = 0;
		right = 0;
		bottom = 0;
	}

	win_rect(const recti other) noexcept
	{
		left = other.left;
		top = other.top;
		right = other.right;
		bottom = other.bottom;
	}

	win_rect(const RECT& other) noexcept
	{
		left = other.left;
		top = other.top;
		right = other.right;
		bottom = other.bottom;
	}

	win_rect(const pointi point, const sizei size) noexcept
	{
		right = (left = point.x) + size.cx;
		bottom = (top = point.y) + size.cy;
	}

	win_rect(const int l, const int t, const int r, const int b) noexcept
	{
		left = l;
		top = t;
		right = r;
		bottom = b;
	}

	operator recti() const noexcept
	{
		return {left, top, right, bottom};
	}

	operator LPRECT() noexcept
	{
		return this;
	}

	operator LPCRECT() const noexcept
	{
		return this;
	}

	bool is_empty() const noexcept
	{
		return left >= right || top >= bottom;
	}

	int width() const noexcept
	{
		return right - left;
	}

	int height() const noexcept
	{
		return bottom - top;
	}

	win_rect inflate(const int xy) const noexcept
	{
		return {left - xy, top - xy, right + xy, bottom + xy};
	}

	win_rect inflate(const int x, const int y) const noexcept
	{
		return {left - x, top - y, right + x, bottom + y};
	}

	win_rect offset(const int x, const int y) const noexcept
	{
		return {left + x, top + y, right + x, bottom + y};
	}

	bool intersects(const win_rect& other) const noexcept
	{
		return left < other.right &&
			top < other.bottom &&
			right > other.left &&
			bottom > other.top;
	}

	win_rect intersection(const win_rect& other) const noexcept
	{
		if (!intersects(other)) return {};

		return {
			std::max(left, other.left),
			std::max(top, other.top),
			std::min(right, other.right),
			std::min(bottom, other.bottom)
		};
	}
};

inline std::wstring window_text_w(const HWND h)
{
	const auto len = ::GetWindowTextLength(h);
	if (len <= 0) return {};
	std::wstring result(len + 1, 0);
	// GetWindowTextLength can over-report, so the copied length decides the result. Trusting
	// the reported length instead leaves embedded NULs in strings that reach settings and search.
	const auto copied = GetWindowText(h, result.data(), len + 1);
	result.resize(copied > 0 ? copied : 0, 0);
	return result;
}

inline std::string window_text(const HWND h)
{
	return str::utf16_to_utf8(window_text_w(h));
}

// The base size fonts are created at, before the monitor's scale is applied.
extern int global_base_font_size;

// The scale a window and the popups it opens are drawn at. A child host shares its parent's; a popup
// host has its own, as it gets its own WM_DPICHANGED.
class owner_context
{
public:
	double scale_factor = 1.0;

	explicit owner_context(const double scale_factor_in) : scale_factor(scale_factor_in)
	{
	}

	void update_scale_factor(const double scale_factor_in)
	{
		scale_factor = scale_factor_in;
	}

	int calc_base_font_size() const
	{
		return df::round(scale_factor * global_base_font_size);
	}

	// Combined UI scale used for layout metrics (paddings, gaps, icons). This folds the
	// large-font preference into the DPI scale so that spacing scales proportionally with
	// the text: when large fonts are enabled the whole UI looks like the normal layout
	// zoomed up rather than large text crammed into normal-sized gaps.
	double calc_ui_scale_factor() const
	{
		return scale_factor * global_base_font_size / static_cast<double>(ui::default_font_size);
	}
};

using owner_context_ptr = std::shared_ptr<owner_context>;

// LockWindowUpdate is deliberately absent: it discards drawing while locked and flashes the
// accumulated region on unlock.

struct win_base
{
	HWND m_hWnd = nullptr;
};

class win_impl : public win_base
{
public:
	virtual ~win_impl()
	{
		if (m_hWnd && IsWindow(m_hWnd))
		{
			df::log(__FUNCTION__, "Destroying win_base of valid window");
			SetWindowLongPtr(m_hWnd, GWLP_USERDATA, 0);
			m_hWnd = nullptr; // Prevent double destruction
		}
	}

	virtual LRESULT on_window_message(const HWND hWnd, const UINT uMsg, const WPARAM wParam, const LPARAM lParam)
	{
		return DefWindowProc(hWnd, uMsg, wParam, lParam);
	}

	static LRESULT CALLBACK stProcessWindowMessage(const HWND hwnd, const UINT uMsg, const WPARAM wParam,
	                                               const LPARAM lParam)
	{
		if (uMsg == WM_NCCREATE)
		{
			// Safer pointer handling with validation
			const auto* lpCreate = reinterpret_cast<LPCREATESTRUCT>(lParam);
			if (lpCreate && lpCreate->lpCreateParams)
			{
				const auto pt = static_cast<win_impl*>(lpCreate->lpCreateParams);
				const auto ptr = reinterpret_cast<LONG_PTR>(lpCreate->lpCreateParams);
				// get the pointer to the window from lpCreateParams which was set in CreateWindow
				SetWindowLongPtr(hwnd, GWLP_USERDATA, ptr);

				if (pt)
				{
					pt->m_hWnd = hwnd;
				}
			}
		}

		// get the pointer to the window
		const auto ptr = reinterpret_cast<win_impl*>(GetWindowLongPtr(hwnd, GWLP_USERDATA));

		if (ptr)
		{
			return ptr->on_window_message(hwnd, uMsg, wParam, lParam);
		}
		return DefWindowProc(hwnd, uMsg, wParam, lParam);
	}

	// The frames paint the whole of themselves, so a class has no background brush and no menu.
	static bool register_class(const UINT style, const HICON hIcon, const HCURSOR hCursor, const LPCWSTR lpszClassName)
	{
		WNDCLASSEX wcx = {};
		wcx.cbSize = sizeof(WNDCLASSEX);
		wcx.style = style;
		wcx.lpfnWndProc = stProcessWindowMessage;
		wcx.hInstance = get_resource_instance;
		wcx.hIcon = hIcon;
		wcx.hCursor = hCursor;
		wcx.lpszClassName = lpszClassName;

		if (RegisterClassEx(&wcx) == 0)
		{
			if (GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
			{
				return false;
			}
		}

		return true;
	}
};
