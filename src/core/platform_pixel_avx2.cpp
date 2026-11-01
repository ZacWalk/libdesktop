// AVX2 BGRA pixel operations.  This file is the only Core source compiled
// with /arch:AVX2; dispatch in platform_pixel.cpp checks the CPU at runtime.

#include "platform.h"

#include <immintrin.h>

namespace
{
	__m256i divide_by_255(const __m256i value)
	{
		return _mm256_srli_epi16(_mm256_add_epi16(_mm256_add_epi16(value, _mm256_set1_epi16(1)),
		                                                       _mm256_srli_epi16(value, 8)), 8);
	}
}

namespace pf
{
	void blend_constant_bgra_avx2(const std::span<std::uint32_t> pixels, const color_t foreground)
	{
		const __m256i zero = _mm256_setzero_si256();
		const __m256i alpha = _mm256_set1_epi16(foreground.a);
		const __m256i inverse = _mm256_set1_epi16(255 - foreground.a);
		const __m256i rounding = _mm256_set1_epi16(127);
		const __m256i foreground_words = _mm256_setr_epi16(
			foreground.b, foreground.g, foreground.r, 255,
			foreground.b, foreground.g, foreground.r, 255,
			foreground.b, foreground.g, foreground.r, 255,
			foreground.b, foreground.g, foreground.r, 255);
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
		pf::blend_constant_bgra(pixels.subspan(index), foreground, pixel_backend::scalar);
	}

	void interpolate_bgra_avx2(const std::span<const std::uint32_t> first,
	                           const std::span<const std::uint32_t> second,
	                           const std::span<std::uint32_t> destination,
	                           const std::uint16_t fraction)
	{
		const __m256i zero = _mm256_setzero_si256();
		const __m256i second_weight = _mm256_set1_epi16(fraction);
		const __m256i first_weight = _mm256_set1_epi16(256 - fraction);
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
		pf::interpolate_bgra(first.subspan(index), second.subspan(index), destination.subspan(index), fraction,
		                     pixel_backend::scalar);
	}
}
