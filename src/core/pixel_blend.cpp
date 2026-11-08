// This file is part of libdesktop, a C++20 platform layer for desktop applications
// Copyright 2026  Zac Walker
//
// This program is free software; you can redistribute it and / or modify it
// under the terms of the LGPL License either version 2.1 or later.
// License details are available at https://www.gnu.org/licenses/lgpl-2.1.html
// This program is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY

// Purpose: Scalar and runtime-dispatched SIMD BGRA pixel blending.

#include "libdesktop/core.h"
#include "libdesktop/core/cpu_features.h"

#include <algorithm>
#include <stdexcept>

#if defined(_M_X64) || defined(_M_IX86) || defined(__x86_64__) || defined(__i386__)
#include <emmintrin.h>
#endif

namespace
{
	uint32_t blend_channel(const uint32_t background, const uint32_t foreground, const uint32_t alpha)
	{
		return (background * (255 - alpha) + foreground * alpha + 127) / 255;
	}

	void blend_scalar(std::span<uint32_t> pixels, const core::color32 foreground)
	{
		const auto red = core::get_r(foreground);
		const auto green = core::get_g(foreground);
		const auto blue = core::get_b(foreground);
		const auto alpha = core::get_a(foreground);

		for (auto& pixel : pixels)
		{
			pixel = blend_channel(pixel & 0xff, blue, alpha) |
				(blend_channel((pixel >> 8) & 0xff, green, alpha) << 8) |
				(blend_channel((pixel >> 16) & 0xff, red, alpha) << 16) |
				0xff000000;
		}
	}

	void interpolate_scalar(const std::span<const uint32_t> first,
	                        const std::span<const uint32_t> second,
	                        const std::span<uint32_t> destination, const uint16_t fraction)
	{
		const uint32_t inverse = 256 - fraction;
		for (size_t index = 0; index < destination.size(); ++index)
		{
			uint32_t pixel{};
			for (int shift = 0; shift < 32; shift += 8)
				pixel |= (((first[index] >> shift & 0xff) * inverse +
					(second[index] >> shift & 0xff) * fraction + 128) >> 8) << shift;
			destination[index] = pixel;
		}
	}

#if defined(_M_X64) || defined(_M_IX86) || defined(__x86_64__) || defined(__i386__)
	__m128i divide_by_255(const __m128i value)
	{
		return _mm_srli_epi16(_mm_add_epi16(_mm_add_epi16(value, _mm_set1_epi16(1)),
		                                           _mm_srli_epi16(value, 8)), 8);
	}

	void blend_sse2(std::span<uint32_t> pixels, const core::color32 foreground)
	{
		const __m128i zero = _mm_setzero_si128();
		const __m128i alpha = _mm_set1_epi16(static_cast<short>(core::get_a(foreground)));
		const __m128i inverse = _mm_set1_epi16(static_cast<short>(255 - core::get_a(foreground)));
		const __m128i rounding = _mm_set1_epi16(127);
		const __m128i foreground_words = _mm_setr_epi16(
			static_cast<short>(core::get_b(foreground)), static_cast<short>(core::get_g(foreground)),
			static_cast<short>(core::get_r(foreground)), 255,
			static_cast<short>(core::get_b(foreground)), static_cast<short>(core::get_g(foreground)),
			static_cast<short>(core::get_r(foreground)), 255);
		const __m128i opaque = _mm_set1_epi32(static_cast<int>(0xff000000));
		size_t index = 0;
		for (; index + 4 <= pixels.size(); index += 4)
		{
			const __m128i background = _mm_loadu_si128(
				reinterpret_cast<const __m128i*>(pixels.data() + index));
			const auto blend = [&](const __m128i words)
			{
				const __m128i sum = _mm_add_epi16(
					_mm_add_epi16(_mm_mullo_epi16(words, inverse),
					              _mm_mullo_epi16(foreground_words, alpha)), rounding);
				return divide_by_255(sum);
			};
			const __m128i result = _mm_or_si128(
				_mm_packus_epi16(blend(_mm_unpacklo_epi8(background, zero)),
				                 blend(_mm_unpackhi_epi8(background, zero))), opaque);
			_mm_storeu_si128(reinterpret_cast<__m128i*>(pixels.data() + index), result);
		}
		blend_scalar(pixels.subspan(index), foreground);
	}

	void interpolate_sse2(const std::span<const uint32_t> first,
	                      const std::span<const uint32_t> second,
	                      const std::span<uint32_t> destination, const uint16_t fraction)
	{
		const __m128i zero = _mm_setzero_si128();
		const __m128i second_weight = _mm_set1_epi16(static_cast<short>(fraction));
		const __m128i first_weight = _mm_set1_epi16(static_cast<short>(256 - fraction));
		const __m128i rounding = _mm_set1_epi16(128);
		size_t index = 0;
		for (; index + 4 <= destination.size(); index += 4)
		{
			const __m128i first_pixels = _mm_loadu_si128(
				reinterpret_cast<const __m128i*>(first.data() + index));
			const __m128i second_pixels = _mm_loadu_si128(
				reinterpret_cast<const __m128i*>(second.data() + index));
			const auto interpolate = [&](const __m128i first_words, const __m128i second_words)
			{
				return _mm_srli_epi16(_mm_add_epi16(
					_mm_add_epi16(_mm_mullo_epi16(first_words, first_weight),
					              _mm_mullo_epi16(second_words, second_weight)), rounding), 8);
			};
			const __m128i result = _mm_packus_epi16(
				interpolate(_mm_unpacklo_epi8(first_pixels, zero), _mm_unpacklo_epi8(second_pixels, zero)),
				interpolate(_mm_unpackhi_epi8(first_pixels, zero), _mm_unpackhi_epi8(second_pixels, zero)));
			_mm_storeu_si128(reinterpret_cast<__m128i*>(destination.data() + index), result);
		}
		interpolate_scalar(first.subspan(index), second.subspan(index), destination.subspan(index), fraction);
	}
#endif

}

#if defined(LIBDESKTOP_PIXEL_AVX2_TU)
namespace core
{
	void blend_constant_bgra_avx2(std::span<uint32_t> pixels, color32 foreground);
	void interpolate_bgra_avx2(std::span<const uint32_t> first,
	                           std::span<const uint32_t> second,
	                           std::span<uint32_t> destination, uint16_t fraction);
}
#endif

bool core::has_sse2() noexcept
{
	return core::detail::has_sse2();
}

void core::blend_constant_bgra(const std::span<uint32_t> pixels, const color32 foreground,
                               const pixel_backend backend)
{
	if (pixels.empty() || get_a(foreground) == 0) return;
	if (get_a(foreground) == 255)
	{
		const uint32_t value = get_b(foreground) | (get_g(foreground) << 8) |
			(get_r(foreground) << 16) | 0xff000000;
		std::ranges::fill(pixels, value);
		return;
	}
#if defined(LIBDESKTOP_PIXEL_AVX2_TU)
	if (backend != pixel_backend::scalar &&
		((backend == pixel_backend::automatic || backend == pixel_backend::avx2) && core::has_avx2()))
	{
		blend_constant_bgra_avx2(pixels, foreground);
		return;
	}
#endif
#if defined(_M_X64) || defined(_M_IX86) || defined(__x86_64__) || defined(__i386__)
	if (backend != pixel_backend::scalar &&
		((backend == pixel_backend::automatic || backend == pixel_backend::sse2 ||
		  backend == pixel_backend::avx2) && core::has_sse2()))
	{
		blend_sse2(pixels, foreground);
		return;
	}
#endif
	blend_scalar(pixels, foreground);
}

void core::blend_constant_bgra(const std::span<uint32_t> pixels, const color& foreground,
                               const pixel_backend backend)
{
	blend_constant_bgra(pixels, foreground.rgba(), backend);
}

void core::interpolate_bgra(std::span<const uint32_t> first,
                            std::span<const uint32_t> second,
                            std::span<uint32_t> destination, const uint16_t fraction,
                            const pixel_backend backend)
{
	if (fraction > 256)
		throw std::invalid_argument("BGRA interpolation fraction must be in [0, 256].");
	const size_t count = (std::min)({first.size(), second.size(), destination.size()});
	first = first.first(count);
	second = second.first(count);
	destination = destination.first(count);
	if (count == 0) return;
#if defined(LIBDESKTOP_PIXEL_AVX2_TU)
	if (backend != pixel_backend::scalar &&
		((backend == pixel_backend::automatic || backend == pixel_backend::avx2) && core::has_avx2()))
	{
		interpolate_bgra_avx2(first, second, destination, fraction);
		return;
	}
#endif
#if defined(_M_X64) || defined(_M_IX86) || defined(__x86_64__) || defined(__i386__)
	if (backend != pixel_backend::scalar &&
		((backend == pixel_backend::automatic || backend == pixel_backend::sse2 ||
		  backend == pixel_backend::avx2) && core::has_sse2()))
	{
		interpolate_sse2(first, second, destination, fraction);
		return;
	}
#endif
	interpolate_scalar(first, second, destination, fraction);
}
