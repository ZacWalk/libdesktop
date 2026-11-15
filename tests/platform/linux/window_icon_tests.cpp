// This file is part of libdesktop, a C++20 platform layer for desktop applications
// Copyright 2026 Zac Walker
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

// Purpose: Decode the generated window-icon format without opening a display or a window.

#include "window_internal.h"
#include "test.h"

#include <array>

namespace
{
	void should_decode_window_icon_alpha()
	{
		std::array<uint8_t, 130> data{};
		const auto write = [&data](const size_t offset, const uint32_t value)
		{
			for (auto i = 0u; i < 4; ++i) data[offset + i] = static_cast<uint8_t>(value >> (i * 8));
		};
		data[0] = 'B';
		data[1] = 'M';
		write(2, static_cast<uint32_t>(data.size()));
		write(10, 122);
		write(14, 108);
		write(18, 2);
		write(22, 0xffffffffu);
		data[26] = 1;
		data[28] = 32;
		write(30, 3);
		write(34, 8);
		write(54, 0x00ff0000);
		write(58, 0x0000ff00);
		write(62, 0x000000ff);
		write(66, 0xff000000);
		write(70, 0x73524742);
		write(122, 0x80112233);

		auto* const icon = platform::linux_detail::load_window_icon(data);
		assert_equal(true, icon != nullptr, "V4 window icon decoded");
		if (!icon) return;
		const core::scope_exit destroy([icon] { SDL_DestroySurface(icon); });
		assert_equal(2, icon->w, "window icon width");
		assert_equal(1, icon->h, "top-down window icon height");
		uint8_t r = 0, g = 0, b = 0, a = 0;
		assert_equal(true, SDL_ReadSurfacePixel(icon, 0, 0, &r, &g, &b, &a), "read the icon pixel");
		assert_equal(0x11, static_cast<int>(r), "icon red channel");
		assert_equal(0x22, static_cast<int>(g), "icon green channel");
		assert_equal(0x33, static_cast<int>(b), "icon blue channel");
		assert_equal(0x80, static_cast<int>(a), "icon alpha channel");
		assert_equal(true, SDL_ReadSurfacePixel(icon, 1, 0, &r, &g, &b, &a), "read transparent pixel");
		assert_equal(0, static_cast<int>(a), "transparent icon padding");
	}

	void should_reject_malformed_window_icons()
	{
		assert_equal(true, platform::linux_detail::load_window_icon({}) == nullptr, "missing icon is optional");
		const std::array<uint8_t, 4> truncated{'B', 'M', 0, 0};
		assert_equal(true, platform::linux_detail::load_window_icon(truncated) == nullptr, "truncated BMP rejected");
		const std::array<uint8_t, 8> png{0x89, 'P', 'N', 'G', 13, 10, 26, 10};
		assert_equal(true, platform::linux_detail::load_window_icon(png) == nullptr, "wrong icon format rejected");
	}
}

void register_window_icon_tests(test_registry& tests)
{
	tests.add("Should decode embedded window icon alpha", should_decode_window_icon_alpha);
	tests.add("Should reject malformed embedded window icons", should_reject_malformed_window_icons);
}
