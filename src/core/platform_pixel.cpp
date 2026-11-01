// Core BGRA pixel operations.  The AVX2 implementation is kept in a separate
// translation unit so selecting it at run time never changes this TU's code
// generation baseline.

#include "platform.h"
#include "libdesktop/detail/cpu_features.h"

#include <algorithm>

#if defined(_M_X64) || defined(_M_IX86) || defined(__x86_64__) || defined(__i386__)
#include <emmintrin.h>
#endif

namespace
{
	std::uint32_t blend_channel(const std::uint32_t background, const std::uint32_t foreground,
	                            const std::uint32_t alpha)
	{
		return (background * (255 - alpha) + foreground * alpha + 127) / 255;
	}

	void blend_scalar(std::span<std::uint32_t> pixels, const pf::color_t foreground)
	{
		for (auto& pixel : pixels)
			pixel = blend_channel(pixel & 0xff, foreground.b, foreground.a) |
				(blend_channel((pixel >> 8) & 0xff, foreground.g, foreground.a) << 8) |
				(blend_channel((pixel >> 16) & 0xff, foreground.r, foreground.a) << 16) |
				0xff000000;
	}

	void interpolate_scalar(const std::span<const std::uint32_t> first,
	                        const std::span<const std::uint32_t> second,
	                        const std::span<std::uint32_t> destination, const std::uint16_t fraction)
	{
		const std::uint32_t inverse = 256 - fraction;
		for (size_t index = 0; index < destination.size(); ++index)
		{
			std::uint32_t pixel{};
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

	void blend_sse2(std::span<std::uint32_t> pixels, const pf::color_t foreground)
	{
		const __m128i zero = _mm_setzero_si128();
		const __m128i alpha = _mm_set1_epi16(foreground.a);
		const __m128i inverse = _mm_set1_epi16(255 - foreground.a);
		const __m128i rounding = _mm_set1_epi16(127);
		const __m128i foreground_words = _mm_setr_epi16(
			foreground.b, foreground.g, foreground.r, 255,
			foreground.b, foreground.g, foreground.r, 255);
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

	void interpolate_sse2(const std::span<const std::uint32_t> first,
	                      const std::span<const std::uint32_t> second,
	                      const std::span<std::uint32_t> destination, const std::uint16_t fraction)
	{
		const __m128i zero = _mm_setzero_si128();
		const __m128i second_weight = _mm_set1_epi16(fraction);
		const __m128i first_weight = _mm_set1_epi16(256 - fraction);
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

#if defined(PF_PIXEL_AVX2_TU)
namespace pf
{
	void blend_constant_bgra_avx2(std::span<std::uint32_t> pixels, color_t foreground);
	void interpolate_bgra_avx2(std::span<const std::uint32_t> first,
	                           std::span<const std::uint32_t> second,
	                           std::span<std::uint32_t> destination, std::uint16_t fraction);
}
#endif

bool pf::has_sse2() noexcept
{
	return libdesktop::detail::has_sse2();
}

bool pf::has_avx2() noexcept
{
	return libdesktop::detail::has_avx2();
}

void pf::blend_constant_bgra(const std::span<std::uint32_t> pixels, const color_t foreground,
                             const pixel_backend backend)
{
	if (pixels.empty() || foreground.a == 0) return;
	if (foreground.a == 255)
	{
		const std::uint32_t value = foreground.b | (foreground.g << 8) |
			(foreground.r << 16) | 0xff000000;
		std::ranges::fill(pixels, value);
		return;
	}
#if defined(PF_PIXEL_AVX2_TU)
	if (backend != pixel_backend::scalar &&
		((backend == pixel_backend::automatic || backend == pixel_backend::avx2) && has_avx2()))
	{
		blend_constant_bgra_avx2(pixels, foreground);
		return;
	}
#endif
#if defined(_M_X64) || defined(_M_IX86) || defined(__x86_64__) || defined(__i386__)
	if (backend != pixel_backend::scalar &&
		((backend == pixel_backend::automatic || backend == pixel_backend::sse2 ||
		  backend == pixel_backend::avx2) && has_sse2()))
	{
		blend_sse2(pixels, foreground);
		return;
	}
#endif
	blend_scalar(pixels, foreground);
}

void pf::interpolate_bgra(std::span<const std::uint32_t> first,
                          std::span<const std::uint32_t> second,
                          std::span<std::uint32_t> destination, const std::uint16_t fraction,
                          const pixel_backend backend)
{
	if (fraction > 256)
		throw std::invalid_argument("BGRA interpolation fraction must be in [0, 256].");
	const size_t count = (std::min)({first.size(), second.size(), destination.size()});
	first = first.first(count);
	second = second.first(count);
	destination = destination.first(count);
	if (count == 0) return;
#if defined(PF_PIXEL_AVX2_TU)
	if (backend != pixel_backend::scalar &&
		((backend == pixel_backend::automatic || backend == pixel_backend::avx2) && has_avx2()))
	{
		interpolate_bgra_avx2(first, second, destination, fraction);
		return;
	}
#endif
#if defined(_M_X64) || defined(_M_IX86) || defined(__x86_64__) || defined(__i386__)
	if (backend != pixel_backend::scalar &&
		((backend == pixel_backend::automatic || backend == pixel_backend::sse2 ||
		  backend == pixel_backend::avx2) && has_sse2()))
	{
		interpolate_sse2(first, second, destination, fraction);
		return;
	}
#endif
	interpolate_scalar(first, second, destination, fraction);
}
