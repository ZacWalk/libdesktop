// This file is part of libdesktop, a C++20 platform layer for desktop applications
// Copyright 2026  Zac Walker
//
// This program is free software; you can redistribute it and / or modify it
// under the terms of the LGPL License either version 2.1 or later.
// License details are available at https://www.gnu.org/licenses/lgpl-2.1.html
// This program is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY

// Purpose: AVX2 implementations for the runtime-dispatched BGRA blend operations.

#include "libdesktop/core.h"

#include <immintrin.h>

namespace
{
	__m256i divide_by_255(const __m256i value)
	{
		return _mm256_srli_epi16(_mm256_add_epi16(_mm256_add_epi16(value, _mm256_set1_epi16(1)),
		                                                       _mm256_srli_epi16(value, 8)), 8);
	}
}

namespace core
{
	void blend_constant_bgra_avx2(const std::span<uint32_t> pixels, const color32 foreground)
	{
		const __m256i zero = _mm256_setzero_si256();
		const __m256i alpha = _mm256_set1_epi16(static_cast<short>(get_a(foreground)));
		const __m256i inverse = _mm256_set1_epi16(static_cast<short>(255 - get_a(foreground)));
		const __m256i rounding = _mm256_set1_epi16(127);
		const __m256i foreground_words = _mm256_setr_epi16(
			static_cast<short>(get_b(foreground)), static_cast<short>(get_g(foreground)),
			static_cast<short>(get_r(foreground)), 255,
			static_cast<short>(get_b(foreground)), static_cast<short>(get_g(foreground)),
			static_cast<short>(get_r(foreground)), 255,
			static_cast<short>(get_b(foreground)), static_cast<short>(get_g(foreground)),
			static_cast<short>(get_r(foreground)), 255,
			static_cast<short>(get_b(foreground)), static_cast<short>(get_g(foreground)),
			static_cast<short>(get_r(foreground)), 255);
		const __m256i opaque = _mm256_set1_epi32(static_cast<int>(0xff000000));
		size_t index = 0;
		for (; index + 8 <= pixels.size(); index += 8)
		{
			const __m256i background = _mm256_loadu_si256(
				reinterpret_cast<const __m256i*>(pixels.data() + index));
			const auto blend = [&](const __m256i words)
			{
				const __m256i sum = _mm256_add_epi16(
					_mm256_add_epi16(_mm256_mullo_epi16(words, inverse),
					                 _mm256_mullo_epi16(foreground_words, alpha)), rounding);
				return divide_by_255(sum);
			};
			const __m256i result = _mm256_or_si256(
				_mm256_packus_epi16(blend(_mm256_unpacklo_epi8(background, zero)),
				                      blend(_mm256_unpackhi_epi8(background, zero))), opaque);
			_mm256_storeu_si256(reinterpret_cast<__m256i*>(pixels.data() + index), result);
		}
		core::blend_constant_bgra(pixels.subspan(index), foreground, pixel_backend::scalar);
	}

	void interpolate_bgra_avx2(const std::span<const uint32_t> first,
	                           const std::span<const uint32_t> second,
	                           const std::span<uint32_t> destination,
	                           const uint16_t fraction)
	{
		const __m256i zero = _mm256_setzero_si256();
		const __m256i second_weight = _mm256_set1_epi16(static_cast<short>(fraction));
		const __m256i first_weight = _mm256_set1_epi16(static_cast<short>(256 - fraction));
		const __m256i rounding = _mm256_set1_epi16(128);
		size_t index = 0;
		for (; index + 8 <= destination.size(); index += 8)
		{
			const __m256i first_pixels = _mm256_loadu_si256(
				reinterpret_cast<const __m256i*>(first.data() + index));
			const __m256i second_pixels = _mm256_loadu_si256(
				reinterpret_cast<const __m256i*>(second.data() + index));
			const auto interpolate = [&](const __m256i first_words, const __m256i second_words)
			{
				return _mm256_srli_epi16(_mm256_add_epi16(
					_mm256_add_epi16(_mm256_mullo_epi16(first_words, first_weight),
					                 _mm256_mullo_epi16(second_words, second_weight)), rounding), 8);
			};
			const __m256i result = _mm256_packus_epi16(
				interpolate(_mm256_unpacklo_epi8(first_pixels, zero), _mm256_unpacklo_epi8(second_pixels, zero)),
				interpolate(_mm256_unpackhi_epi8(first_pixels, zero), _mm256_unpackhi_epi8(second_pixels, zero)));
			_mm256_storeu_si256(reinterpret_cast<__m256i*>(destination.data() + index), result);
		}
		core::interpolate_bgra(first.subspan(index), second.subspan(index), destination.subspan(index), fraction,
		                       pixel_backend::scalar);
	}
}
