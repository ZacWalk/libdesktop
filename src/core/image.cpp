// This file is part of libdesktop, a C++20 platform layer for desktop applications
// Copyright 2026  Zac Walker
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

// Purpose: Image surface operations. Implements pixel-level transformations
// including RGB/BGR swapping and pixel difference calculations.

#include "libdesktop/core.h"
#include "libdesktop/core/util.h"
#include "libdesktop/core/simd.h"

#ifdef COMPILE_SIMD_INTRINSIC
#include <emmintrin.h>
#include <tmmintrin.h>

// Separated so SSSE3 can be named for this loop alone. Naming it on swap_rb would let the scalar
// fallback below use SSSE3 as well - on a processor that reached the fallback by not having it.
DF_TARGET_SSSE3 static void swap_rb_ssse3(uint8_t* const pixels, const size_t stride, const int height)
{
	const auto mask = _mm_setr_epi8(0x02, 0x01, 0x00, 0x03, 0x06, 0x05, 0x04, 0x07, 0x0a, 0x09, 0x08, 0x0b, 0x0e, 0x0d,
	                                0x0c, 0x0f);

	for (auto y = 0; y < height; y++)
	{
		const auto dst = pixels + y * stride;
		const auto* end = dst + stride;

		for (auto p = dst; p < end; p += 16)
		{
			const __m128i pixel = _mm_load_si128(std::bit_cast<__m128i*>(p));
			_mm_store_si128(std::bit_cast<__m128i*>(p), _mm_shuffle_epi8(pixel, mask));
		}
	}
}
#endif

void core::surface::swap_rb()
{
	// Red and blue are the first and third bytes of a packed pixel only.
	if (!is_packed(_format) || empty()) return;

#ifdef COMPILE_SIMD_INTRINSIC
	// _mm_shuffle_epi8 is an SSSE3 instruction, so gate on SSSE3 support (not
	// just SSE2 which is the x64 baseline) to avoid an illegal-instruction fault
	// on early x64 CPUs that have SSE2 but lack SSSE3.
	if (core::ssse3_supported)
	{
		const auto dest_stride = _stride;

		if (std::bit_cast<uintptr_t>(_pixels.get()) % 16 == 0 && dest_stride % 16 == 0)
		{
			swap_rb_ssse3(_pixels.get(), dest_stride, _dimensions.cy);
			return;
		}
	}
#endif

	for (auto y = 0; y < _dimensions.cy; y++)
	{
		const auto dst = std::bit_cast<uint32_t*>(pixels_line(y));
		const auto* end = dst + _dimensions.cx;

		for (auto p = dst; p < end; p += 1)
		{
			*p = bgr(*p);
		}
	}
}

void core::surface::unpremultiply_alpha()
{
	if (_format != core::texture_format::ARGB || empty()) return;
	for (auto y = 0; y < _dimensions.cy; ++y)
	{
		auto* row = pixels_line(y);
		for (auto x = 0; x < _dimensions.cx; ++x, row += 4)
		{
			const auto alpha = row[3];
			for (auto channel = 0; channel < 3; ++channel)
				row[channel] = alpha == 0
				                   ? 0
				                   : static_cast<uint8_t>(std::min(255, (row[channel] * 255 + alpha / 2) / alpha));
		}
	}
}

core::pixel_difference_result core::surface::pixel_difference(const core::const_surface_ptr& other) const
{
	if (!other) return core::pixel_difference_result::unknown;

	if (_dimensions != other->dimensions())
	{
		return core::pixel_difference_result::not_equal;
	}

	if (_dimensions.cx < 1 || _dimensions.cy < 1) return core::pixel_difference_result::equal;

	// Rows are compared as their layout stores them, so neither surface is read past its own rows. RGB and
	// ARGB share a layout; any other two formats cannot be compared byte for byte.
	const auto same_layout = _format == other->_format || (is_packed(_format) && is_packed(other->_format));

	if (!same_layout || _format == texture_format::None || !_pixels || !other->_pixels)
	{
		return core::pixel_difference_result::unknown;
	}

	const auto rows_equal = [this, &other](const size_t first_row, const int rows, const size_t row_bytes)
	{
		for (auto y = 0; y < rows; ++y)
		{
			const auto row = first_row + static_cast<size_t>(y);
			if (memcmp(_pixels.get() + row * _stride, other->_pixels.get() + row * other->_stride, row_bytes) != 0)
			{
				return false;
			}
		}

		return true;
	};

	const auto cx = static_cast<size_t>(_dimensions.cx);
	const auto cy = _dimensions.cy;
	const auto pixel_bytes = static_cast<size_t>(bytes_per_pixel(_format));

	if (!rows_equal(0, cy, cx * pixel_bytes)) return core::pixel_difference_result::not_equal;

	if (!has_addressable_pixels(_format))
	{
		// The chroma plane follows the luma rows: one row of interleaved U and V for every two of luma, no
		// more of them than alloc made room for.
		const auto chroma_bytes = std::min({(cx + 1) / 2 * 2 * pixel_bytes, _stride, other->_stride});
		if (!rows_equal(static_cast<size_t>(cy), cy / 2, chroma_bytes)) return core::pixel_difference_result::not_equal;
	}

	return core::pixel_difference_result::equal;
}
