// This file is part of libdesktop, a C++20 platform layer for desktop applications
// Copyright 2026  Zac Walker
// 
// This program is free software; you can redistribute it and / or modify it
// under the terms of the LGPL License either version 2.1 or later.
// License details are available at https://www.gnu.org/licenses/lgpl-2.1.html
// This program is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY

// Purpose: libdesktop-render's test program: what an application supplies the module - its name and
// the chrome's glyphs - the tests that need no Windows backend, and the runner. The tests began in
// Diffractor's test_render.cpp, which keeps its own copies, and use its assertions (system_test.h).

#include "libdesktop/render.h"
#include "libdesktop/render_software.h"
#include "libdesktop/util_simd.h"
#include "system_test.h"

#include <cstdio>

const std::string_view s_app_name = "LibDesktop-Render-Tests";

// The glyphs an application's icon font names. None is loaded here, so the chrome draws none.
const ui::chrome_icon_set ui::chrome_icons = {};

#ifdef _WIN32
// The window layer's entry point runs the application this makes. The tests drive the layer
// directly, with no application of their own.
ui::app_ptr create_app(const ui::plat_app_ptr&)
{
	return nullptr;
}
#endif

static uint8_t blend_opaque_channel(const uint8_t dest, const float src, const float alpha)
{
	return static_cast<uint8_t>(std::clamp(static_cast<int>(src * alpha + dest * (1.0f - alpha) + 0.5f), 0, 255));
}

static void should_unpremultiply_surface_alpha()
{
	ui::surface surface;
	assert_equal(true, surface.alloc(3, 1, ui::texture_format::ARGB) != nullptr, "alpha fixture allocated");
	surface.set_pixel(0, 0, 0x80402010u);
	surface.set_pixel(1, 0, 0x00112233u);
	surface.set_pixel(2, 0, 0xff336699u);
	surface.unpremultiply_alpha();
	assert_equal(0x80804020u, surface.get_pixel(0, 0), "associated channels are divided by alpha");
	assert_equal(0u, surface.get_pixel(1, 0), "transparent pixels have defined zero colors");
	assert_equal(0xff336699u, surface.get_pixel(2, 0), "opaque pixels are unchanged");
}

static uint8_t blend_opaque_normalized_channel(const uint8_t dest, const float src, const float alpha)
{
	const auto value = src * alpha + (dest / 255.0f) * (1.0f - alpha);
	return static_cast<uint8_t>(std::clamp(static_cast<int>(value * 255.0f + 0.5f), 0, 255));
}

static void should_match_simd_software_blends()
{
#if defined(COMPILE_SIMD_INTRINSIC)
	constexpr size_t max_pixels = 19;
	alignas(16) std::array<uint8_t, max_pixels * 4> original{};
	alignas(16) std::array<uint8_t, max_pixels * 4> source{};
	alignas(16) std::array<uint8_t, max_pixels> coverage{};

	for (size_t i = 0; i < original.size(); ++i) original[i] = static_cast<uint8_t>(i * 47u + 13u);
	for (size_t i = 0; i < source.size(); ++i) source[i] = static_cast<uint8_t>(i * 29u + 7u);
	for (size_t i = 0; i < coverage.size(); ++i) coverage[i] = static_cast<uint8_t>(i * 61u + 3u);

	constexpr std::array<float, 5> alphas = {0.0f, 0.13f, 0.5f, 0.87f, 1.0f};
	constexpr float sb = 37.0f;
	constexpr float sg = 149.0f;
	constexpr float sr = 231.0f;

	for (size_t count = 0; count <= max_pixels; ++count)
	{
		for (const auto alpha : alphas)
		{
			auto check = [&](auto&& scalar, auto&& simd, const std::string_view message)
			{
				auto expected = original;
				auto actual = original;
				for (size_t i = 0; i < count; ++i) scalar(expected.data() + i * 4, i);
				const auto processed = simd(actual.data());
				assert_equal(count & ~size_t{3}, processed, message);
				for (size_t i = processed; i < count; ++i) scalar(actual.data() + i * 4, i);
				assert_equal(true, expected == actual, message);
			};

			auto blend_solid_pixel = [](uint8_t* dest, const float b, const float g, const float r, const float a)
			{
				dest[0] = blend_opaque_normalized_channel(dest[0], b, a);
				dest[1] = blend_opaque_normalized_channel(dest[1], g, a);
				dest[2] = blend_opaque_normalized_channel(dest[2], r, a);
				dest[3] = 255;
			};

			check([&](uint8_t* dest, size_t) { blend_solid_pixel(dest, sb / 255.0f, sg / 255.0f, sr / 255.0f, alpha); },
			      [&](uint8_t* dest)
			      {
				      return blend_solid_opaque_sse2(dest, count, sb / 255.0f, sg / 255.0f,
				                                     sr / 255.0f, alpha);
			      }, "SIMD solid blend");

			check([&](uint8_t* dest, const size_t i)
			      {
				      blend_solid_pixel(dest, sb / 255.0f, sg / 255.0f,
				                        sr / 255.0f, alpha * coverage[i] / 255.0f);
			      },
			      [&](uint8_t* dest)
			      {
				      return blend_glyph_opaque_sse2(dest, coverage.data(), count, sb / 255.0f,
				                                     sg / 255.0f, sr / 255.0f, alpha);
			      }, "SIMD glyph blend");

			for (const auto has_alpha : {false, true})
			{
				check([&](uint8_t* dest, const size_t i)
				      {
					      const auto* src = source.data() + i * 4;
					      const auto pixel_alpha = (has_alpha ? src[3] / 255.0f : 1.0f) * alpha;
					      dest[0] = blend_opaque_channel(dest[0], src[0], pixel_alpha);
					      dest[1] = blend_opaque_channel(dest[1], src[1], pixel_alpha);
					      dest[2] = blend_opaque_channel(dest[2], src[2], pixel_alpha);
					      dest[3] = 255;
				      }, [&](uint8_t* dest)
				      {
					      return blend_bgra_opaque_sse2(dest, source.data(), count, has_alpha, alpha);
				      }, "SIMD BGRA blend");
			}
		}
	}
#endif
}

// An enlargement is what area_downscale refuses, and it is the case the software backend hits every
// frame when the window is larger than the video. swscale answers it with an RGB->YUV->RGB round
// trip through its scalar full-chroma converter, so this path exists to keep it out of that.
static void should_bilinear_resize_packed_surfaces()
{
	const auto make_source = [](const int cx, const int cy)
	{
		auto s = std::make_shared<ui::surface>();
		s->alloc(cx, cy, ui::texture_format::ARGB, ui::orientation::left_bottom);
		s->color_space(ui::color_space::rec709_limited);
		return s;
	};

	// Two samples widened to four. Sampling at centres and clamping the outer half sample is what
	// keeps the ends exact; extrapolating instead is what puts a dark fringe round an enlargement.
	{
		const auto src = make_source(2, 1);
		auto* const row = src->pixels_line(0);
		row[0] = row[1] = row[2] = row[3] = 0;
		row[4] = row[5] = row[6] = row[7] = 200;

		ui::surface_ptr dst;
		assert_equal(true, ui::bilinear_resize(src, dst, {4, 1}), "widen a two sample ramp");
		assert_equal(true, dst->dimensions() == sizei{4, 1}, "enlarged dimensions");
		assert_equal(true, dst->format() == ui::texture_format::ARGB, "enlarged format");
		assert_equal(true, dst->orientation() == ui::orientation::left_bottom, "enlarged orientation");
		assert_equal(true, dst->color_space() == ui::color_space::rec709_limited, "enlarged colour space");

		const auto* const out = dst->pixels_line(0);
		assert_equal(0, static_cast<int>(out[0]), "first sample keeps the source end");
		assert_equal(50, static_cast<int>(out[4]), "quarter weight");
		assert_equal(150, static_cast<int>(out[8]), "three quarter weight");
		assert_equal(200, static_cast<int>(out[12]), "last sample keeps the source end");
	}

	// A flat surface must survive any enlargement untouched, which a pair of weights not summing to
	// 256 would shift.
	{
		const auto src = make_source(7, 5);

		for (auto y = 0; y < 5; ++y)
		{
			auto* const row = src->pixels_line(y);

			for (auto x = 0; x < 7; ++x)
			{
				row[x * 4 + 0] = 19;
				row[x * 4 + 1] = 140;
				row[x * 4 + 2] = 233;
				row[x * 4 + 3] = 77;
			}
		}

		ui::surface_ptr dst;
		assert_equal(true, ui::bilinear_resize(src, dst, {31, 22}), "enlarge a flat surface");

		auto flat = true;

		for (auto y = 0; y < 22; ++y)
		{
			const auto* const row = dst->pixels_line(y);

			for (auto x = 0; x < 31; ++x)
			{
				flat = flat && row[x * 4 + 0] == 19 && row[x * 4 + 1] == 140 &&
					row[x * 4 + 2] == 233 && row[x * 4 + 3] == 77;
			}
		}

		assert_equal(true, flat, "an enlarged flat surface is unchanged");
	}

	// One axis growing while the other shrinks is neither a reduction nor an enlargement, so
	// area_downscale refuses it and this is what answers it.
	{
		const auto src = make_source(16, 16);
		ui::surface_ptr dst;
		assert_equal(false, ui::area_downscale(src, dst, {32, 8}), "the area filter refuses a mixed resize");
		assert_equal(true, ui::bilinear_resize(src, dst, {32, 8}), "bilinear takes a mixed resize");
		assert_equal(true, dst->dimensions() == sizei{32, 8}, "mixed resize dimensions");
	}

	// Planar surfaces are four bytes a pixel to this walk; they belong to swscale.
	{
		ui::surface nv12;
		nv12.alloc(8, 8, ui::texture_format::NV12);
		const auto nv12_view = ui::const_surface_ptr(&nv12, [](const ui::surface*)
		{
		});

		ui::surface_ptr dst;
		assert_equal(false, ui::bilinear_resize(nv12_view, dst, {16, 16}), "refuse a planar format");
	}
}

static void should_area_downscale_packed_and_planar_surfaces()
{
	const auto make_source = [](const int cx, const int cy, const ui::texture_format format)
	{
		auto s = std::make_shared<ui::surface>();
		s->alloc(cx, cy, format, ui::orientation::left_bottom);
		s->color_space(ui::color_space::rec709_limited);
		return s;
	};

	// A flat source must survive a reduction untouched. Any weight run that does not sum to exactly
	// 256 shifts the level here, which is the failure this whole scheme has to rule out.
	{
		const auto src = make_source(97, 61, ui::texture_format::ARGB);

		for (auto y = 0; y < 61; ++y)
		{
			auto* const row = src->pixels_line(y);
			for (auto x = 0; x < 97; ++x)
			{
				row[x * 4 + 0] = 17;
				row[x * 4 + 1] = 134;
				row[x * 4 + 2] = 251;
				row[x * 4 + 3] = 200;
			}
		}

		ui::surface_ptr dst;
		assert_equal(true, ui::area_downscale(src, dst, {13, 9}), "reduce a flat surface");
		assert_equal(true, dst->dimensions() == sizei{13, 9}, "reduced dimensions");
		assert_equal(true, dst->format() == ui::texture_format::ARGB, "reduced format");
		assert_equal(true, dst->orientation() == ui::orientation::left_bottom, "reduced orientation");
		assert_equal(true, dst->color_space() == ui::color_space::rec709_limited, "reduced colour space");

		for (auto y = 0; y < 9; ++y)
		{
			const auto* const row = dst->pixels_line(y);

			for (auto x = 0; x < 13; ++x)
			{
				assert_equal(17, static_cast<int>(row[x * 4 + 0]), "flat blue");
				assert_equal(134, static_cast<int>(row[x * 4 + 1]), "flat green");
				assert_equal(251, static_cast<int>(row[x * 4 + 2]), "flat red");
				assert_equal(200, static_cast<int>(row[x * 4 + 3]), "flat alpha");
			}
		}
	}

	// An exact 2:1 reduction must be the mean of each 2x2 block, so a checkerboard of 0 and 200
	// answers 100 everywhere. Nearest neighbour or a dropped contribution cannot produce that.
	{
		const auto src = make_source(64, 64, ui::texture_format::RGB);

		for (auto y = 0; y < 64; ++y)
		{
			auto* const row = src->pixels_line(y);
			for (auto x = 0; x < 64; ++x)
			{
				const auto v = static_cast<uint8_t>(((x + y) & 1) ? 200 : 0);
				row[x * 4 + 0] = row[x * 4 + 1] = row[x * 4 + 2] = row[x * 4 + 3] = v;
			}
		}

		ui::surface_ptr dst;
		assert_equal(true, ui::area_downscale(src, dst, {32, 32}), "reduce a checkerboard");

		for (auto y = 0; y < 32; ++y)
		{
			const auto* const row = dst->pixels_line(y);
			for (auto x = 0; x < 32; ++x)
			{
				assert_equal(100, static_cast<int>(row[x * 4 + 1]), "checkerboard mean");
			}
		}
	}

	// A horizontal ramp reduced 4:1 must stay monotonic and keep its ends, which a wrong first index
	// or a truncated run would break.
	{
		const auto src = make_source(256, 8, ui::texture_format::RGB);

		for (auto y = 0; y < 8; ++y)
		{
			auto* const row = src->pixels_line(y);
			for (auto x = 0; x < 256; ++x)
			{
				row[x * 4 + 0] = row[x * 4 + 1] = row[x * 4 + 2] = static_cast<uint8_t>(x);
				row[x * 4 + 3] = 255;
			}
		}

		ui::surface_ptr dst;
		assert_equal(true, ui::area_downscale(src, dst, {64, 2}), "reduce a ramp");

		const auto* const row = dst->pixels_line(0);
		assert_equal(2, static_cast<int>(row[0]), "ramp first sample");
		assert_equal(254, static_cast<int>(row[63 * 4]), "ramp last sample");

		auto monotonic = true;
		for (auto x = 1; x < 64; ++x) monotonic = monotonic && row[x * 4] > row[(x - 1) * 4];
		assert_equal(true, monotonic, "ramp stays monotonic");
	}

	// Refusals hand the work back to swscale rather than producing a wrong picture.
	{
		const auto rgb = make_source(8, 8, ui::texture_format::RGB);
		ui::surface_ptr dst;
		assert_equal(false, ui::area_downscale(rgb, dst, {16, 4}), "refuse an enlargement");
		assert_equal(false, ui::area_downscale(rgb, dst, {0, 4}), "refuse an empty extent");

		// P010 samples are 16 bit, which the Q8 accumulators cannot hold.
		ui::surface p010;
		p010.alloc(8, 8, ui::texture_format::P010);
		const auto p010_view = ui::const_surface_ptr(&p010, [](const ui::surface*)
		{
		});
		assert_equal(false, ui::area_downscale(p010_view, dst, {4, 4}), "refuse a 16 bit planar format");

		// NV12 chroma is one pair per 2x2 luma block, so an odd extent has no whole answer.
		ui::surface nv12;
		nv12.alloc(8, 8, ui::texture_format::NV12);
		const auto nv12_view = ui::const_surface_ptr(&nv12, [](const ui::surface*)
		{
		});
		assert_equal(false, ui::area_downscale(nv12_view, dst, {5, 4}), "refuse an odd nv12 extent");
	}

	// NV12 reduces as two planes: the luma on its own, and the interleaved chroma pair at half the
	// extent. A flat picture must survive both, and U must not bleed into V.
	{
		ui::surface nv12;
		nv12.alloc(64, 32, ui::texture_format::NV12);
		nv12.color_space(ui::color_space::rec601_full);

		const auto stride = nv12.stride();
		auto* const luma = nv12.pixels();
		auto* const chroma = luma + stride * 32;

		for (auto y = 0; y < 32; ++y)
		{
			auto* const row = luma + stride * y;
			for (auto x = 0; x < 64; ++x) row[x] = static_cast<uint8_t>(((x + y) & 1) ? 200 : 0);
		}

		for (auto y = 0; y < 16; ++y)
		{
			auto* const row = chroma + stride * y;
			for (auto x = 0; x < 32; ++x)
			{
				row[x * 2] = 90;
				row[x * 2 + 1] = 210;
			}
		}

		const auto nv12_view = ui::const_surface_ptr(&nv12, [](const ui::surface*)
		{
		});

		ui::surface_ptr dst;
		assert_equal(true, ui::area_downscale(nv12_view, dst, {32, 16}), "reduce an nv12 surface");
		assert_equal(true, dst->format() == ui::texture_format::NV12, "reduced nv12 format");
		assert_equal(true, dst->color_space() == ui::color_space::rec601_full, "reduced nv12 colour space");

		const auto dst_stride = dst->stride();
		auto luma_correct = true;

		for (auto y = 0; y < 16; ++y)
		{
			const auto* const row = dst->pixels() + dst_stride * y;
			for (auto x = 0; x < 32; ++x) luma_correct = luma_correct && row[x] == 100;
		}

		assert_equal(true, luma_correct, "nv12 luma is the 2x2 mean");

		auto chroma_correct = true;

		for (auto y = 0; y < 8; ++y)
		{
			const auto* const row = dst->pixels() + dst_stride * 16 + dst_stride * y;
			for (auto x = 0; x < 16; ++x)
			{
				chroma_correct = chroma_correct && row[x * 2] == 90 && row[x * 2 + 1] == 210;
			}
		}

		assert_equal(true, chroma_correct, "nv12 chroma keeps u and v apart");
	}

	// Same size on one axis is still a reduction; the identity run must not be rejected or shifted.
	{
		const auto src = make_source(40, 10, ui::texture_format::RGB);

		for (auto y = 0; y < 10; ++y)
		{
			auto* const row = src->pixels_line(y);
			for (auto x = 0; x < 40; ++x)
			{
				row[x * 4 + 0] = row[x * 4 + 1] = row[x * 4 + 2] = row[x * 4 + 3] = static_cast<uint8_t>(
					(x == 20) ? 240 : 10);
			}
		}

		ui::surface_ptr dst;
		assert_equal(true, ui::area_downscale(src, dst, {40, 5}), "reduce one axis only");
		assert_equal(240, static_cast<int>(dst->pixels_line(0)[20 * 4]), "identity axis keeps the column");
		assert_equal(10, static_cast<int>(dst->pixels_line(0)[19 * 4]), "identity axis keeps its neighbour");
	}

	// The AVX2 and SSE2 accumulators must agree byte for byte, or a picture would depend on the CPU
	// it was reduced on. Sizes are deliberately not multiples of the vector widths so both the wide
	// loops and their scalar tails are covered.
	{
		const auto src = make_source(211, 97, ui::texture_format::ARGB);
		auto seed = 0x9e3779b9u;

		for (auto y = 0; y < 97; ++y)
		{
			auto* const row = src->pixels_line(y);

			for (auto x = 0; x < 211 * 4; ++x)
			{
				seed = seed * 1664525u + 1013904223u;
				row[x] = static_cast<uint8_t>(seed >> 24);
			}
		}

		for (const auto extent : {sizei{53, 29}, sizei{211, 12}, sizei{7, 97}, sizei{210, 96}})
		{
			ui::surface_ptr wide;
			ui::surface_ptr baseline;
			assert_equal(true, ui::area_downscale(src, wide, extent), "reduce with the selected width");
			assert_equal(true, ui::area_downscale_baseline(src, baseline, extent), "reduce with the SSE2 baseline");

			auto identical = true;

			for (auto y = 0; y < extent.cy; ++y)
			{
				identical = identical && memcmp(wide->pixels_line(y), baseline->pixels_line(y),
				                                static_cast<size_t>(extent.cx) * 4u) == 0;
			}

			assert_equal(true, identical, std::format("SIMD widths agree at {}x{}", extent.cx, extent.cy));
		}
	}
}

static void should_animate_alpha_between_values()
{
	// The gate is off when the CPU software backend is active, so force the animated
	// behaviour under test and restore whatever the running renderer selected.
	const auto restore_animations = ui::animations_enabled;
	const df::scope_exit restore_scope([restore_animations] { ui::animations_enabled = restore_animations; });
	ui::animations_enabled = true;

	ui::animate_alpha alpha;
	alpha.reset(0.0f, 1.0f);
	assert_equal(0.0f, alpha.val(), "fade-in starts at zero");
	assert_equal(1.0f, alpha.target(), "fade-in targets one");
	assert_equal(true, alpha.step(), "fade-in advances");
	assert_equal(true, alpha.val() > 0.0f && alpha.val() < 1.0f, "fade-in interpolates");

	alpha.target(0.0f);
	assert_equal(true, alpha.val() > 0.0f, "retarget preserves current value");
	assert_equal(true, alpha.step(), "fade-out advances");

	for (auto i = 0; i < 100; ++i) alpha.step();
	assert_equal(0.0f, alpha.val(), "fade-out reaches target");
	assert_equal(false, alpha.step(), "completed animation stops");

	alpha.reset(0.0005f, 0.0f);
	assert_equal(true, alpha.step(), "final snap requests a frame");
	assert_equal(0.0f, alpha.val(), "final snap reaches target");
	assert_equal(false, alpha.step(), "final snap completes once");
}

// A fade used to advance a fixed fraction per frame, so the same fade ran twice as fast on a 120Hz
// display as on a 60Hz one. It now advances by elapsed time, so equal wall-clock time must leave it
// in the same place whatever the refresh rate that produced it.
static void should_fade_at_the_same_rate_on_any_refresh_rate()
{
	const auto restore_animations = ui::animations_enabled;
	const auto restore_factor = ui::animation_step_factor;
	const df::scope_exit restore_scope([restore_animations, restore_factor]
	{
		ui::animations_enabled = restore_animations;
		ui::animation_step_factor = restore_factor;
	});
	ui::animations_enabled = true;

	const auto fade_for = [](const int frames_per_second, const double seconds)
	{
		const auto dt = 1.0 / frames_per_second;
		ui::animation_step_factor = static_cast<float>(1.0 - std::exp(-ui::alpha_fade_rate * dt));

		ui::animate_alpha alpha;
		alpha.reset(0.0f, 1.0f);

		for (auto i = 0; i < static_cast<int>(seconds * frames_per_second); ++i) alpha.step();

		return alpha.val();
	};

	constexpr auto quarter_second = 0.25;
	const auto at_60 = fade_for(60, quarter_second);
	const auto at_120 = fade_for(120, quarter_second);
	const auto at_30 = fade_for(30, quarter_second);

	assert_equal(true, at_60 > 0.1f && at_60 < 0.999f, "a quarter second is mid-fade");
	assert_equal(true, std::abs(at_120 - at_60) < 0.02f, "120Hz matches 60Hz at the same elapsed time");
	assert_equal(true, std::abs(at_30 - at_60) < 0.02f, "30Hz matches 60Hz at the same elapsed time");

	// The replaced constant: a 60Hz frame must still close a third of the remaining distance, so
	// nothing about the feel of a fade changes on the refresh rate almost every display reports.
	ui::animation_step_factor = static_cast<float>(1.0 - std::exp(-ui::alpha_fade_rate / 60.0));
	assert_equal(true, std::abs(ui::animation_step_factor - 0.333f) < 0.001f, "60Hz reproduces the old step");
}

static void should_skip_alpha_animation_when_disabled()
{
	// CPU software rendering cannot afford per-frame fades: every alpha reaches its target
	// immediately so draws stay on the opaque fast paths and no extra frames are requested.
	const auto restore_animations = ui::animations_enabled;
	const df::scope_exit restore_scope([restore_animations] { ui::animations_enabled = restore_animations; });
	ui::animations_enabled = false;

	ui::animate_alpha alpha;
	alpha.reset(0.0f, 1.0f);
	assert_equal(1.0f, alpha.val(), "fade-in starts complete");
	assert_equal(false, alpha.step(), "fade-in requests no frame");

	alpha.target(0.0f);
	assert_equal(0.0f, alpha.val(), "fade-out completes immediately");
	assert_equal(false, alpha.step(), "fade-out requests no frame");
}

// The neutral parameter set is the one a picture with no edits is rendered through, so any drift in
// the tone curve, the YUV round trip or the rounding shows up here as a picture the app quietly
// altered. Truncating instead of rounding in adjust_color moves every channel a level and fails this.
static void should_leave_colour_unchanged_when_neutral()
{
	const auto src = std::make_shared<ui::surface>();
	assert_equal(true, src->alloc(16, 4, ui::texture_format::ARGB), "neutral source allocated");

	for (auto y = 0; y < 4; ++y)
	{
		auto* const row = src->pixels_line(y);

		for (auto x = 0; x < 16; ++x)
		{
			row[x * 4 + 0] = static_cast<uint8_t>(x * 17);
			row[x * 4 + 1] = static_cast<uint8_t>(255 - x * 17);
			row[x * 4 + 2] = static_cast<uint8_t>((x * 37 + y * 11) & 0xff);
			row[x * 4 + 3] = 255;
		}
	}

	const auto dst = std::make_shared<ui::surface>();
	assert_equal(true, dst->alloc(16, 4, ui::texture_format::ARGB), "neutral destination allocated");

	ui::color_adjust adjust;
	adjust.color_params(0, 0, 0, 0, 0, 0, 0);
	adjust.apply(src, dst->pixels(), dst->stride(), {});

	auto worst = 0;
	auto bias = 0;
	auto samples = 0;

	for (auto y = 0; y < 4; ++y)
	{
		const auto* const in = src->pixels_line(y);
		const auto* const out = dst->pixels_line(y);

		for (auto i = 0; i < 16 * 4; ++i)
		{
			const auto delta = static_cast<int>(out[i]) - static_cast<int>(in[i]);
			worst = std::max(worst, std::abs(delta));
			bias += delta;
			++samples;
		}
	}

	// One level of slack for the RGB -> YUV -> RGB round trip; two would hide a real curve error.
	assert_equal(true, worst <= 1, "neutral adjustment leaves the picture alone");

	// The residual must also be unbiased. A worst-case bound alone cannot see truncation, which costs
	// at most one level but costs it in the same direction on every pixel, darkening the whole picture.
	assert_equal(true, std::abs(bias) * 8 <= samples, "the neutral residual is not biased in one direction");
}

// Full desaturation is the one adjustment with an answer that can be stated exactly, so it pins the
// chroma path independently of the tone curve.
static void should_desaturate_to_grey()
{
	const auto src = std::make_shared<ui::surface>();
	assert_equal(true, src->alloc(8, 1, ui::texture_format::ARGB), "chroma source allocated");

	auto* const row = src->pixels_line(0);

	for (auto x = 0; x < 8; ++x)
	{
		row[x * 4 + 0] = static_cast<uint8_t>(x * 30);
		row[x * 4 + 1] = static_cast<uint8_t>(200 - x * 20);
		row[x * 4 + 2] = static_cast<uint8_t>(40 + x * 25);
		row[x * 4 + 3] = 255;
	}

	const auto dst = std::make_shared<ui::surface>();
	assert_equal(true, dst->alloc(8, 1, ui::texture_format::ARGB), "chroma destination allocated");

	ui::color_adjust adjust;
	adjust.color_params(0, -1.0, 0, 0, 0, 0, 0);
	adjust.apply(src, dst->pixels(), dst->stride(), {});

	const auto* const out = dst->pixels_line(0);

	for (auto x = 0; x < 8; ++x)
	{
		const auto b = static_cast<int>(out[x * 4 + 0]);
		const auto g = static_cast<int>(out[x * 4 + 1]);
		const auto r = static_cast<int>(out[x * 4 + 2]);

		assert_equal(true, std::abs(r - g) <= 1 && std::abs(g - b) <= 1,
		             "saturation of -1 leaves every pixel grey");
		assert_equal(255u, static_cast<uint32_t>(out[x * 4 + 3]), "alpha survives desaturation");
	}
}

#ifdef _WIN32
void register_render_win_tests(test_registry& tests);
#endif

static void register_tests(test_registry& tests)
{
#ifdef _WIN32
	register_render_win_tests(tests);
#endif
	tests.add("Should unpremultiply surface alpha"s, should_unpremultiply_surface_alpha);
	tests.add("Should match SIMD software blends"s, should_match_simd_software_blends);
	tests.add("Should area downscale packed and planar surfaces"s, should_area_downscale_packed_and_planar_surfaces);
	tests.add("Should bilinear resize packed surfaces"s, should_bilinear_resize_packed_surfaces);
	tests.add("Should animate alpha between values"s, should_animate_alpha_between_values);
	tests.add("Should fade at the same rate on any refresh rate"s, should_fade_at_the_same_rate_on_any_refresh_rate);
	tests.add("Should skip alpha animation when disabled"s, should_skip_alpha_animation_when_disabled);
	tests.add("Should leave colour unchanged when neutral"s, should_leave_colour_unchanged_when_neutral);
	tests.add("Should desaturate to grey"s, should_desaturate_to_grey);
}

int main(const int argc, char* argv[])
{
	// What an application sets up before it uses the module: a UTF-8 C locale, which the backend's
	// narrow conversions follow, set as Diffractor sets it; and the thread's platform state - COM's
	// apartment on Windows, which the shell and WIC need.
#ifdef _WIN32
	std::setlocale(LC_ALL, "en_US.UTF-8");
#else
	std::setlocale(LC_CTYPE, "C.UTF-8");
#endif
	const platform::thread_init thread;

	// The desktop UI belongs to the thread an application's entry point marks as its UI thread:
	// textures recycle only there, and Debug builds assert it throughout.
	platform::set_ui_thread();

	const std::string_view filter = argc > 1 ? argv[1] : "*";

	struct registry final : test_registry
	{
		std::vector<std::pair<std::string, std::function<void()>>> tests;

		void add(std::string name, std::function<void()> f) override
		{
			tests.emplace_back(std::move(name), std::move(f));
		}
	} all;

	register_tests(all);

	auto run = 0;
	auto failed = 0;

	for (const auto& [name, test] : all.tests)
	{
		if (!str::wildcard_icmp(name, filter)) continue;

		++run;
		const auto checks_before = test_assert_count;
		std::string failure;

		try
		{
			test();
			if (test_assert_count == checks_before) failure = "asserted nothing";
		}
		catch (const test_assert_exception& e)
		{
			failure = e.message;
		}
		catch (const std::exception& e)
		{
			failure = std::format("threw {}", e.what());
		}

		if (failure.empty())
		{
			std::printf("PASS  %s\n", name.c_str());
		}
		else
		{
			++failed;
			std::printf("FAIL  %s: %s\n", name.c_str(), failure.c_str());
		}
	}

	for (const auto& error : _temps.delete_temps())
	{
		++failed;
		std::printf("FAIL  temporary files: %s\n", error.c_str());
	}

	std::printf("\n%d tests, %d failed, %d checks\n", run, failed, test_assert_count);
	return failed == 0 && run > 0 ? 0 : 1;
}
