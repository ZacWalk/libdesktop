// This file is part of libdesktop, a C++20 platform layer for desktop applications
// Copyright 2026  Zac Walker
// 
// This program is free software; you can redistribute it and / or modify it
// under the terms of the LGPL License either version 2.1 or later.
// License details are available at https://www.gnu.org/licenses/lgpl-2.1.html
// This program is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY

// Purpose: Image surface management. Handles bitmap memory allocation,
// pixel access, scaling, cropping, format conversions, the CPU lookup of the HDR tone-mapping cube,
// and drawing the application mark.

#include "libdesktop/render.h"

#include "libdesktop/platform.h"
#include "libdesktop/util.h"

#if defined(_M_X64) || defined(_M_IX86)
#include <immintrin.h>
#define DF_X86_SIMD 1
#endif

void ui::surface::clear(const color32 clr) const
{
	for (auto y = 0; y < _dimensions.cy; ++y)
	{
		auto* const line = std::bit_cast<color32*>(_pixels.get() + y * _stride);

		for (int x = 0; x < _dimensions.cx; ++x)
		{
			line[x] = clr;
		}
	}
}

namespace
{
	// Where one signal falls among the cube's samples on one axis: the sample below it, and how far
	// it lies towards the next. 0 and 65535 land on the first and last sample, as a texture sampler
	// places them.
	struct cube_axis
	{
		int index = 0;
		float weight = 0.0f;
	};

	cube_axis locate_in_cube(const uint16_t signal, const int last, const float scale)
	{
		const auto x = static_cast<float>(signal) * scale;
		const auto index = std::min(static_cast<int>(x), last - 1);
		return {index, x - static_cast<float>(index)};
	}

	bool is_complete(const ui::tone_map_lut& lut)
	{
		return lut.size > 1 && lut.rgba.size() >= static_cast<size_t>(lut.size) * lut.size * lut.size * 4;
	}

#if DF_X86_SIMD
	// The four channels of a sample share one register, so the eight corners of a trilinear sample
	// take seven blends rather than twenty-one. The three axes are located together as well, and the
	// two samples either side along red sit side by side in memory, so each pair is one read.
	void apply_tone_map_sse2(const ui::tone_map_lut& lut, const uint16_t* const rgb48, const size_t pixels,
	                         ui::color32* const out)
	{
		const auto last = lut.size - 1;
		const auto step_g = static_cast<ptrdiff_t>(lut.size) * 4;
		const auto step_b = step_g * lut.size;
		const auto* const samples = lut.rgba.data();

		const auto zero = _mm_setzero_si128();
		const auto scale = _mm_set1_ps(static_cast<float>(last) / 65535.0f);
		const auto highest_below = _mm_set1_ps(static_cast<float>(last - 1));
		const auto to_byte = _mm_set1_ps(255.0f / 65535.0f);
		const auto half = _mm_set1_ps(0.5f);

		// The sample at `sample` and the one after it along red.
		const auto pair = [zero](const uint16_t* const sample, __m128& lower, __m128& upper)
		{
			const auto both = _mm_loadu_si128(reinterpret_cast<const __m128i*>(sample));
			lower = _mm_cvtepi32_ps(_mm_unpacklo_epi16(both, zero));
			upper = _mm_cvtepi32_ps(_mm_unpackhi_epi16(both, zero));
		};

		const auto lerp = [](const __m128 a, const __m128 b, const __m128 t)
		{
			return _mm_add_ps(a, _mm_mul_ps(_mm_sub_ps(b, a), t));
		};

		for (size_t i = 0; i < pixels; ++i)
		{
			const auto* const px = rgb48 + i * 3;

			// Red and green, then blue, in the low three 16-bit lanes; nothing past the pixel is read.
			uint32_t red_green = 0;
			memcpy(&red_green, px, sizeof(red_green));
			const auto signal = _mm_insert_epi16(_mm_cvtsi32_si128(static_cast<int>(red_green)), px[2], 2);

			const auto position = _mm_mul_ps(_mm_cvtepi32_ps(_mm_unpacklo_epi16(signal, zero)), scale);
			const auto below = _mm_cvtepi32_ps(_mm_cvttps_epi32(_mm_min_ps(position, highest_below)));
			const auto weight = _mm_sub_ps(position, below);

			alignas(16) int32_t index[4];
			_mm_store_si128(reinterpret_cast<__m128i*>(index), _mm_cvttps_epi32(below));
			const auto* const c = samples + (index[2] * step_b + index[1] * step_g + index[0] * 4);

			const auto wr = _mm_shuffle_ps(weight, weight, _MM_SHUFFLE(0, 0, 0, 0));
			const auto wg = _mm_shuffle_ps(weight, weight, _MM_SHUFFLE(1, 1, 1, 1));
			const auto wb = _mm_shuffle_ps(weight, weight, _MM_SHUFFLE(2, 2, 2, 2));

			__m128 c000, c100, c010, c110, c001, c101, c011, c111;
			pair(c, c000, c100);
			pair(c + step_g, c010, c110);
			pair(c + step_b, c001, c101);
			pair(c + step_b + step_g, c011, c111);

			const auto x00 = lerp(c000, c100, wr);
			const auto x10 = lerp(c010, c110, wr);
			const auto x01 = lerp(c001, c101, wr);
			const auto x11 = lerp(c011, c111, wr);
			const auto rgba = lerp(lerp(x00, x10, wg), lerp(x01, x11, wg), wb);

			// R, G, B, A in the lanes, rounded to bytes and stored B, G, R, A.
			auto bytes = _mm_cvttps_epi32(_mm_add_ps(_mm_mul_ps(rgba, to_byte), half));
			bytes = _mm_shuffle_epi32(bytes, _MM_SHUFFLE(3, 0, 1, 2));
			bytes = _mm_packs_epi32(bytes, bytes);
			bytes = _mm_packus_epi16(bytes, bytes);
			out[i] = static_cast<ui::color32>(_mm_cvtsi128_si32(bytes));
		}
	}
#endif
}

void ui::tone_map_lut::apply_baseline(const uint16_t* const rgb48, const size_t pixels, color32* const out) const
{
	if (!is_complete(*this))
	{
		std::fill_n(out, pixels, color32{0});
		return;
	}

	const auto last = size - 1;
	const auto scale = static_cast<float>(last) / 65535.0f;
	const auto step_g = static_cast<ptrdiff_t>(size) * 4;
	const auto step_b = step_g * size;
	const auto* const samples = rgba.data();

	const auto lerp = [](const float a, const float c, const float t) { return a + (c - a) * t; };

	for (size_t i = 0; i < pixels; ++i)
	{
		const auto* const px = rgb48 + i * 3;
		const auto r = locate_in_cube(px[0], last, scale);
		const auto g = locate_in_cube(px[1], last, scale);
		const auto b = locate_in_cube(px[2], last, scale);
		const auto* const c = samples + (b.index * step_b + g.index * step_g + r.index * 4);

		color32 pixel = 0xffu << 24;

		for (auto channel = 0; channel < 3; ++channel)
		{
			const auto at = [c, channel](const ptrdiff_t offset) { return static_cast<float>(c[offset + channel]); };

			const auto x00 = lerp(at(0), at(4), r.weight);
			const auto x10 = lerp(at(step_g), at(step_g + 4), r.weight);
			const auto x01 = lerp(at(step_b), at(step_b + 4), r.weight);
			const auto x11 = lerp(at(step_b + step_g), at(step_b + step_g + 4), r.weight);
			const auto v = lerp(lerp(x00, x10, g.weight), lerp(x01, x11, g.weight), b.weight);
			const auto byte = std::min(static_cast<uint32_t>(v * (255.0f / 65535.0f) + 0.5f), 255u);

			// Red, green and blue to bits 16, 8 and 0: blue in the low byte.
			pixel |= byte << (16 - 8 * channel);
		}

		out[i] = pixel;
	}
}

// The arithmetic of a trilinear 3D texture sample, so the CPU backend's tone mapping matches the
// shader's: the signal is scaled onto the samples, and the eight around it are blended by distance.
void ui::tone_map_lut::apply(const uint16_t* const rgb48, const size_t pixels, color32* const out) const
{
#if DF_X86_SIMD
	if (is_complete(*this))
	{
		apply_tone_map_sse2(*this, rgb48, pixels, out);
		return;
	}
#endif

	apply_baseline(rgb48, pixels, out);
}

namespace
{
	// Area-average reduction.
	//
	// swscale cannot shrink a packed RGB surface cheaply: it has no RGB->RGB scaler, so it converts
	// to planar YUV and back, and because a BGRA source carries no chroma subsampling libswscale
	// forces SWS_FULL_CHR_H_INT (utils.c, "input having non subsampled chroma"), whose output
	// converter yuv2bgra32_full_X_c has no SIMD at all. Measured at ~5% of startup CPU.
	//
	// Weights are quantised to Q8 and each destination sample's run sums to exactly 256, so a whole
	// reduction fits in uint16 accumulators: the largest reachable value is 255 * 256 = 65280. That
	// is what lets both passes stay in 16 bit lanes, which is twice the throughput of 32 bit ones.
	struct axis_weights
	{
		std::vector<int> first;
		std::vector<int> count;
		std::vector<int> offset;
		std::vector<uint16_t> weights;
	};

	axis_weights build_axis_weights(const int src_n, const int dst_n)
	{
		axis_weights result;
		result.first.resize(dst_n);
		result.count.resize(dst_n);
		result.offset.resize(dst_n);

		const auto scale = static_cast<double>(src_n) / static_cast<double>(dst_n);
		result.weights.reserve(static_cast<size_t>(dst_n) * (static_cast<size_t>(scale) + 2u));

		for (auto i = 0; i < dst_n; ++i)
		{
			const auto a = i * scale;
			const auto b = (i + 1) * scale;
			const auto first = std::clamp(static_cast<int>(a), 0, src_n - 1);
			const auto last = std::clamp(static_cast<int>(std::ceil(b)) - 1, first, src_n - 1);

			result.first[i] = first;
			result.count[i] = last - first + 1;
			result.offset[i] = static_cast<int>(result.weights.size());

			// Emitted as differences of a rounded cumulative coverage rather than as rounded
			// individual weights, so the run sums to 256 by construction. Correcting a rounding
			// error afterwards can underflow the largest weight once a run gets long.
			auto previous = 0;

			for (auto j = first; j <= last; ++j)
			{
				const auto edge = std::min(b, static_cast<double>(j + 1));
				const auto covered = std::clamp((edge - a) / (b - a), 0.0, 1.0);
				// The final edge is forced rather than rounded: when `last` was clamped to the last
				// source sample, `b` lies beyond it and the run would otherwise sum to under 256.
				const auto cumulative = j == last ? 256 : std::clamp(static_cast<int>(std::lround(covered * 256.0)),
				                                                     previous, 256);
				result.weights.emplace_back(static_cast<uint16_t>(cumulative - previous));
				previous = cumulative;
			}
		}

		return result;
	}

	void accumulate_row(uint16_t* acc, const uint8_t* src, const size_t n, const uint16_t weight,
	                    [[maybe_unused]] const bool avx2)
	{
		size_t i = 0;

#ifdef DF_X86_SIMD
		if (avx2)
		{
			const auto wv = _mm256_set1_epi16(static_cast<short>(weight));

			for (; i + 16 <= n; i += 16)
			{
				const auto s = _mm_loadu_si128(std::bit_cast<const __m128i*>(src + i));
				const auto v = _mm256_mullo_epi16(_mm256_cvtepu8_epi16(s), wv);
				auto* const dst = std::bit_cast<__m256i*>(acc + i);
				_mm256_storeu_si256(dst, _mm256_add_epi16(_mm256_loadu_si256(dst), v));
			}
		}
		else
		{
			const auto wv = _mm_set1_epi16(static_cast<short>(weight));
			const auto zero = _mm_setzero_si128();

			for (; i + 8 <= n; i += 8)
			{
				const auto s = _mm_loadl_epi64(std::bit_cast<const __m128i*>(src + i));
				const auto v = _mm_mullo_epi16(_mm_unpacklo_epi8(s, zero), wv);
				auto* const dst = std::bit_cast<__m128i*>(acc + i);
				_mm_storeu_si128(dst, _mm_add_epi16(_mm_loadu_si128(dst), v));
			}
		}
#endif

		for (; i < n; ++i)
		{
			acc[i] = static_cast<uint16_t>(acc[i] + src[i] * weight);
		}
	}

	void normalize_row(uint8_t* dst, const uint16_t* acc, const size_t n, [[maybe_unused]] const bool avx2)
	{
		size_t i = 0;

#ifdef DF_X86_SIMD
		if (avx2)
		{
			const auto half = _mm256_set1_epi16(128);

			for (; i + 16 <= n; i += 16)
			{
				auto v = _mm256_loadu_si256(std::bit_cast<const __m256i*>(acc + i));
				v = _mm256_srli_epi16(_mm256_add_epi16(v, half), 8);
				_mm_storeu_si128(std::bit_cast<__m128i*>(dst + i),
				                 _mm_packus_epi16(_mm256_castsi256_si128(v), _mm256_extracti128_si256(v, 1)));
			}
		}
		else
		{
			const auto half = _mm_set1_epi16(128);

			for (; i + 8 <= n; i += 8)
			{
				auto v = _mm_loadu_si128(std::bit_cast<const __m128i*>(acc + i));
				v = _mm_srli_epi16(_mm_add_epi16(v, half), 8);
				_mm_storel_epi64(std::bit_cast<__m128i*>(dst + i), _mm_packus_epi16(v, v));
			}
		}
#endif

		for (; i < n; ++i)
		{
			dst[i] = static_cast<uint8_t>((acc[i] + 128) >> 8);
		}
	}

	// One destination sample from a run of source samples. A packed pixel's four channels ride in one
	// register, and two source pixels are folded per step, so no channel is ever touched on its own.
	// The one- and two-channel forms are the planes of an NV12 surface - luma on its own, and the
	// interleaved chroma pair - which are a quarter and an eighth of the data, so they read directly.
	template <int channels>
	void reduce_sample(uint8_t* dst, const uint8_t* src, const uint16_t* weights, const int count)
	{
		if constexpr (channels == 4)
		{
#ifdef DF_X86_SIMD
			const auto zero = _mm_setzero_si128();
			auto acc = _mm_setzero_si128();
			auto k = 0;

			for (; k + 2 <= count; k += 2)
			{
				const auto s = _mm_loadl_epi64(std::bit_cast<const __m128i*>(src + k * 4));
				const auto w0 = static_cast<short>(weights[k]);
				const auto w1 = static_cast<short>(weights[k + 1]);
				const auto wv = _mm_set_epi16(w1, w1, w1, w1, w0, w0, w0, w0);
				acc = _mm_add_epi16(acc, _mm_mullo_epi16(_mm_unpacklo_epi8(s, zero), wv));
			}

			if (k < count)
			{
				const auto s = _mm_cvtsi32_si128(*std::bit_cast<const int32_t*>(src + k * 4));
				const auto wv = _mm_set1_epi16(static_cast<short>(weights[k]));
				acc = _mm_add_epi16(acc, _mm_mullo_epi16(_mm_unpacklo_epi8(s, zero), wv));
			}

			// The two pixel slots partition the run's 256 total, so folding them cannot overflow.
			acc = _mm_add_epi16(acc, _mm_srli_si128(acc, 8));
			acc = _mm_srli_epi16(_mm_add_epi16(acc, _mm_set1_epi16(128)), 8);
			*std::bit_cast<int32_t*>(dst) = _mm_cvtsi128_si32(_mm_packus_epi16(acc, acc));
			return;
#endif
		}

		uint32_t acc[channels] = {};

		for (auto k = 0; k < count; ++k)
		{
			const auto weight = weights[k];
			for (auto c = 0; c < channels; ++c) acc[c] += src[k * channels + c] * weight;
		}

		for (auto c = 0; c < channels; ++c) dst[c] = static_cast<uint8_t>((acc[c] + 128) >> 8);
	}

	// One 8-bit plane, area averaged. The passes are fused per destination row, so the only buffers
	// are one accumulator row and one reduced row. A separate full intermediate image would be tens
	// of megabytes when the display scaler shrinks a photo to a 4K window.
	template <int channels>
	void area_downscale_plane_impl(const uint8_t* const src_pixels, const size_t src_stride, const sizei src_extent,
	                               uint8_t* const dst_pixels, const size_t dst_stride, const sizei dst_extent,
	                               const bool avx2)
	{
		const auto horizontal = build_axis_weights(src_extent.cx, dst_extent.cx);
		const auto vertical = build_axis_weights(src_extent.cy, dst_extent.cy);

		const auto row_bytes = static_cast<size_t>(src_extent.cx) * channels;
		std::vector<uint16_t> acc(row_bytes);
		std::vector<uint8_t> reduced(row_bytes);

		for (auto y = 0; y < dst_extent.cy; ++y)
		{
			std::ranges::fill(acc, uint16_t{0});

			const auto* const row_weights = vertical.weights.data() + vertical.offset[y];

			for (auto k = 0; k < vertical.count[y]; ++k)
			{
				accumulate_row(acc.data(), src_pixels + (vertical.first[y] + k) * src_stride, row_bytes,
				               row_weights[k], avx2);
			}

			normalize_row(reduced.data(), acc.data(), row_bytes, avx2);

			auto* const dst_row = dst_pixels + static_cast<size_t>(y) * dst_stride;

			for (auto x = 0; x < dst_extent.cx; ++x)
			{
				reduce_sample<channels>(dst_row + x * channels, reduced.data() + horizontal.first[x] * channels,
				                        horizontal.weights.data() + horizontal.offset[x], horizontal.count[x]);
			}
		}
	}

	bool is_reduction(const sizei src_extent, const sizei dst_extent)
	{
		// Reductions only: a box filter that enlarges is nearest neighbour.
		return dst_extent.cx >= 1 && dst_extent.cy >= 1 &&
			dst_extent.cx <= src_extent.cx && dst_extent.cy <= src_extent.cy;
	}

	bool area_downscale_impl(const ui::const_surface_ptr& src, ui::surface_ptr& dst, const sizei dst_extent,
	                         const bool avx2)
	{
		if (!is_valid(src)) return false;

		const auto format = src->format();
		const auto packed = ui::is_packed(format);

		// P010 is 16 bit, which the Q8 accumulators this is built on cannot hold, so it stays with
		// swscale.
		if (!packed && format != ui::texture_format::NV12) return false;

		const auto src_extent = src->dimensions();

		if (!is_reduction(src_extent, dst_extent)) return false;

		// NV12 carries one chroma pair per 2x2 luma block, so both planes only divide cleanly when
		// every axis is even.
		if (!packed && ((src_extent.cx | src_extent.cy | dst_extent.cx | dst_extent.cy) & 1) != 0) return false;

		auto result = std::make_shared<ui::surface>();

		if (!result->alloc(dst_extent.cx, dst_extent.cy, format, src->orientation()))
		{
			return false;
		}

		result->color_space(src->color_space());

		const auto* const src_pixels = src->pixels();
		const auto src_stride = src->stride();
		auto* const dst_pixels = result->pixels();
		const auto dst_stride = result->stride();

		if (packed)
		{
			area_downscale_plane_impl<4>(src_pixels, src_stride, src_extent, dst_pixels, dst_stride, dst_extent,
			                             avx2);
		}
		else
		{
			area_downscale_plane_impl<1>(src_pixels, src_stride, src_extent, dst_pixels, dst_stride, dst_extent,
			                             avx2);

			// The chroma plane is U and V interleaved at half resolution, so it reduces as a
			// two-channel plane of half the extent rather than needing a pass of its own.
			const sizei src_chroma{src_extent.cx / 2, src_extent.cy / 2};
			const sizei dst_chroma{dst_extent.cx / 2, dst_extent.cy / 2};

			area_downscale_plane_impl<2>(src_pixels + src_stride * src_extent.cy, src_stride, src_chroma,
			                             dst_pixels + dst_stride * dst_extent.cy, dst_stride, dst_chroma, avx2);
		}

		dst = std::move(result);
		return true;
	}
}

bool ui::area_downscale(const const_surface_ptr& src, surface_ptr& dst, const sizei dst_extent)
{
	return area_downscale_impl(src, dst, dst_extent, platform::has_avx2());
}

bool ui::area_downscale_baseline(const const_surface_ptr& src, surface_ptr& dst, const sizei dst_extent)
{
	return area_downscale_impl(src, dst, dst_extent, false);
}

namespace
{
	struct lerp_axis
	{
		std::vector<int> first;
		// Q8 share of the second sample. The first takes the remainder, so a pair always sums to 256.
		std::vector<uint16_t> weight;
	};

	lerp_axis build_lerp_axis(const int src_n, const int dst_n)
	{
		lerp_axis result;
		result.first.resize(dst_n);
		result.weight.resize(dst_n);

		for (auto i = 0; i < dst_n; ++i)
		{
			// Sample centres, not edges, so the outer half sample clamps to the border instead of
			// extrapolating past it - which is what puts a dark fringe round an enlarged picture.
			const auto pos = std::clamp(((i + 0.5) * src_n / dst_n) - 0.5, 0.0, static_cast<double>(src_n - 1));
			const auto base = static_cast<int>(pos);

			result.first[i] = base;
			result.weight[i] = static_cast<uint16_t>(std::lround((pos - base) * 256.0));
		}

		return result;
	}

	// Separable bilinear, scalar. It exists for the case area_downscale refuses - an enlargement, or
	// one axis growing while the other shrinks - which swscale answers with an RGB->YUV->RGB round
	// trip through its scalar full-chroma converter, so even a scalar direct pass is well ahead.
	bool bilinear_resize_impl(const ui::const_surface_ptr& src, ui::surface_ptr& dst, const sizei dst_extent)
	{
		if (!is_valid(src) || !ui::is_packed(src->format())) return false;

		const auto src_extent = src->dimensions();

		if (dst_extent.cx < 1 || dst_extent.cy < 1 || src_extent.cx < 1 || src_extent.cy < 1) return false;

		const auto horizontal = build_lerp_axis(src_extent.cx, dst_extent.cx);
		const auto vertical = build_lerp_axis(src_extent.cy, dst_extent.cy);

		auto result = std::make_shared<ui::surface>();

		if (!result->alloc(dst_extent.cx, dst_extent.cy, src->format(), src->orientation()))
		{
			return false;
		}

		result->color_space(src->color_space());

		const auto* const src_pixels = src->pixels();
		const auto src_stride = src->stride();

		for (auto y = 0; y < dst_extent.cy; ++y)
		{
			const auto y0 = vertical.first[y];
			const auto y1 = std::min(y0 + 1, src_extent.cy - 1);
			const auto wy = static_cast<int>(vertical.weight[y]);
			const auto* const row0 = src_pixels + src_stride * y0;
			const auto* const row1 = src_pixels + src_stride * y1;
			auto* const dst_row = result->pixels_line(y);

			for (auto x = 0; x < dst_extent.cx; ++x)
			{
				const auto x0 = horizontal.first[x] * 4;
				const auto x1 = std::min(horizontal.first[x] + 1, src_extent.cx - 1) * 4;
				const auto wx = static_cast<int>(horizontal.weight[x]);

				for (auto c = 0; c < 4; ++c)
				{
					const auto top = row0[x0 + c] * (256 - wx) + row0[x1 + c] * wx;
					const auto bottom = row1[x0 + c] * (256 - wx) + row1[x1 + c] * wx;
					dst_row[x * 4 + c] = static_cast<uint8_t>((top * (256 - wy) + bottom * wy + 32768) >> 16);
				}
			}
		}

		dst = std::move(result);
		return true;
	}
}

bool ui::bilinear_resize(const const_surface_ptr& src, surface_ptr& dst, const sizei dst_extent)
{
	return bilinear_resize_impl(src, dst, dst_extent);
}

ui::surface_ptr ui::surface::transform(const simple_transform t) const
{
	surface_ptr result;

	if (!is_packed(_format)) return result;

	const auto cy = _dimensions.cy;
	const auto cx = _dimensions.cx;

	if (t == simple_transform::rot_90)
	{
		result = std::make_shared<surface>();

		if (result->alloc(cy, cx, _format, _orientation, _time))
		{
			for (int y = 0; y < cy; ++y)
			{
				const auto* const line_src = std::bit_cast<const color32*>(_pixels.get() + y * _stride);

				for (int x = 0; x < cx; ++x)
				{
					auto* const line_dst = std::bit_cast<color32*>(
						result->_pixels.get() + (cx - 1 - x) * result->_stride);
					line_dst[y] = line_src[x];
				}
			}
		}
	}
	else if (t == simple_transform::rot_270)
	{
		result = std::make_shared<surface>();

		if (result->alloc(cy, cx, _format, _orientation, _time))
		{
			for (int y = 0; y < cy; ++y)
			{
				const auto* const line_src = std::bit_cast<const color32*>(_pixels.get() + (cy - 1 - y) * _stride);

				for (int x = 0; x < cx; ++x)
				{
					auto* const line_dst = std::bit_cast<color32*>(result->_pixels.get() + x * result->_stride);
					line_dst[y] = line_src[x];
				}
			}
		}
	}
	else if (t == simple_transform::rot_180)
	{
		result = std::make_shared<surface>();

		if (result->alloc(cx, cy, _format, _orientation, _time))
		{
			for (int y = 0; y < cy; ++y)
			{
				const auto* const line_src = std::bit_cast<const color32*>(_pixels.get() + (cy - 1 - y) * _stride);
				auto* const line_dst = std::bit_cast<color32*>(result->_pixels.get() + y * result->_stride);

				for (int x = 0; x < cx; ++x)
				{
					line_dst[x] = line_src[cx - 1 - x];
				}
			}
		}
	}

	return result;
}

static constexpr int denom = 100000;

sizei ui::scale_dimensions(const sizei dims, const int limit, const bool dont_scale_up) noexcept
{
	const auto sx = df::mul_div(limit, denom, dims.cx);
	const auto sy = df::mul_div(limit, denom, dims.cy);
	const auto s = std::min(sx, sy);

	if (dont_scale_up && s >= denom)
		return dims;

	return {
		std::max(1, df::mul_div(dims.cx, s, denom)),
		std::max(1, df::mul_div(dims.cy, s, denom))
	};
}

sizei ui::scale_dimensions(const sizei dims, const sizei limit, const bool dont_scale_up) noexcept
{
	if (limit.is_empty()) return dims;

	const auto sx = df::mul_div(limit.cx, denom, dims.cx);
	const auto sy = df::mul_div(limit.cy, denom, dims.cy);
	const auto s = std::min(sx, sy);

	if (dont_scale_up && s >= denom)
		return dims;

	return {
		std::max(1, df::mul_div(dims.cx, s, denom)),
		std::max(1, df::mul_div(dims.cy, s, denom))
	};
}

recti ui::scale_dimensions(const sizei dims, const recti limit, const bool dont_scale_up) noexcept
{
	const auto scaled = scale_dimensions(dims, limit.extent(), dont_scale_up);

	const auto x = (limit.width() - scaled.cx) / 2;
	const auto y = (limit.height() - scaled.cy) / 2;

	return {limit.left + x, limit.top + y, limit.left + x + scaled.cx, limit.top + y + scaled.cy};
}

int ui::calc_scale_down_factor(const sizei dims_in, const sizei size_out) noexcept
{
	if (size_out.cx > 0 || size_out.cy > 0)
	{
		if (dims_in.cx / 8 >= size_out.cx && dims_in.cy / 8 >= size_out.cy)
		{
			return 8;
		}
		if (dims_in.cx / 4 >= size_out.cx && dims_in.cy / 4 >= size_out.cy)
		{
			return 4;
		}
		if (dims_in.cx / 2 >= size_out.cx && dims_in.cy / 2 >= size_out.cy)
		{
			return 2;
		}
	}

	return 1;
}
