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

// The window layer's entry point runs the application this makes. The tests drive the layer
// directly, with no application of their own.
ui::app_ptr create_app(const ui::plat_app_ptr&)
{
	return nullptr;
}

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

// A video's decode thread prepares a picture of the same size for every frame. Allocated again at the size it
// has, a surface keeps its block, where a fresh block of a frame's size is fresh pages to fault in and clear.
static void should_keep_a_surface_block_allocated_again_at_its_size()
{
	ui::surface surface;
	auto* const first = surface.alloc(64, 48, ui::texture_format::NV12, ui::orientation::top_left, 1.0);
	assert_equal(true, first != nullptr, "the surface is allocated");

	assert_equal(true, surface.alloc(64, 48, ui::texture_format::NV12, ui::orientation::right_top, 2.0) == first,
	             "allocated again at its size, it keeps its block");
	assert_equal(2.0, surface.time(), "and takes the new picture's time");
	assert_equal(true, surface.orientation() == ui::orientation::right_top, "and its orientation");

	assert_equal(true, surface.alloc(32, 16, ui::texture_format::RGB) != nullptr, "another size is allocated");
	assert_equal(true, surface.dimensions() == sizei{32, 16} && surface.size() == surface.stride() * 16,
	             "at the size it was asked for");
}

static uint8_t blend_opaque_normalized_channel(const uint8_t dest, const float src, const float alpha)
{
	const auto value = src * alpha + (dest / 255.0f) * (1.0f - alpha);
	return static_cast<uint8_t>(std::clamp(static_cast<int>(value * 255.0f + 0.5f), 0, 255));
}

// Each displayed pixel against the stored one it comes from, for every transform an orientation names.
// The mirrored four answered nothing, so a caller turning a photo upright left it mirrored or sideways.
static void should_transform_surfaces_for_every_orientation()
{
	ui::surface stored;
	assert_equal(true, stored.alloc(3, 2, ui::texture_format::RGB) != nullptr, "stored fixture allocated");
	const auto at = [](const int x, const int y) { return 0xff000000u | static_cast<uint32_t>(y * 16 + x); };
	for (auto y = 0; y < 2; ++y)
		for (auto x = 0; x < 3; ++x) stored.set_pixel(x, y, at(x, y));

	struct expectation
	{
		simple_transform transform;
		sizei extent;
		// The stored pixel shown at the displayed top-left, top-right and bottom-left corners.
		pointi top_left, top_right, bottom_left;
	};

	const expectation expected[] = {
		{simple_transform::flip_h, {3, 2}, {2, 0}, {0, 0}, {2, 1}},
		{simple_transform::rot_180, {3, 2}, {2, 1}, {0, 1}, {2, 0}},
		{simple_transform::flip_v, {3, 2}, {0, 1}, {2, 1}, {0, 0}},
		{simple_transform::transpose, {2, 3}, {0, 0}, {0, 1}, {2, 0}},
		{simple_transform::rot_270, {2, 3}, {0, 1}, {0, 0}, {2, 1}},
		{simple_transform::transverse, {2, 3}, {2, 1}, {2, 0}, {0, 1}},
		{simple_transform::rot_90, {2, 3}, {2, 0}, {2, 1}, {0, 0}},
	};

	for (const auto& e : expected)
	{
		const auto name = std::format("transform {}", static_cast<int>(e.transform));
		const auto turned = stored.transform(e.transform);
		assert_equal(true, turned != nullptr, name + " gives a surface");
		if (!turned) continue;

		assert_equal(e.extent.cx, turned->dimensions().cx, name + " width");
		assert_equal(e.extent.cy, turned->dimensions().cy, name + " height");
		const auto right = e.extent.cx - 1;
		const auto bottom = e.extent.cy - 1;
		assert_equal(at(e.top_left.x, e.top_left.y), turned->get_pixel(0, 0), name + " top-left");
		assert_equal(at(e.top_right.x, e.top_right.y), turned->get_pixel(right, 0), name + " top-right");
		assert_equal(at(e.bottom_left.x, e.bottom_left.y), turned->get_pixel(0, bottom), name + " bottom-left");
	}

	assert_equal(true, stored.transform(simple_transform::none) == nullptr, "none asks for no copy");
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

// A scene draws its vertices by reference, so rectangles updated after it was recorded are what the
// next present shows - each update in place of the last - with nothing recorded again.
static void should_present_vertex_rectangles_updated_after_the_scene()
{
	constexpr sizei extent{48, 32};
	const auto target = ui::create_memory_present_target();
	const auto renderer = ui::create_software_draw_context(nullptr, target, ui::default_font_size);

	const auto shade = [&](const int x, const int y)
	{
		const auto buffer = target->acquire_buffer(extent);
		const auto* const p = buffer.bits + y * buffer.stride + x * 4;
		return ui::rgb(p[2], p[1], p[0]);
	};

	const auto verts = renderer->create_vertices();
	const ui::vertex_rect red[] = {{recti(4, 4, 20, 12), ui::color(1.0f, 0.0f, 0.0f, 1.0f)}};
	const ui::vertex_rect blue[] = {{recti(28, 16, 40, 28), ui::color(0.0f, 0.0f, 1.0f, 1.0f)}};

	renderer->begin_draw(extent, ui::default_font_size, {});
	renderer->clear(ui::color(0.0f, 0.0f, 0.0f, 1.0f));
	verts->update_rects(red);
	renderer->draw_vertices(verts);
	renderer->render();

	assert_equal(ui::rgb(255, 0, 0), shade(10, 8), "the rectangle is drawn");
	assert_equal(ui::rgb(0, 0, 0), shade(30, 20), "and nothing beside it");

	verts->update_rects(blue);
	renderer->reset_damage();
	renderer->render();

	assert_equal(ui::rgb(0, 0, 255), shade(30, 20), "a re-present shows the rectangle updated since");
	assert_equal(ui::rgb(0, 0, 0), shade(10, 8), "in place of the one before");

	verts->update_rects({});
	renderer->reset_damage();
	renderer->render();

	assert_equal(ui::rgb(0, 0, 0), shade(30, 20), "and an empty update draws nothing");
}

static void should_bound_the_software_buffer_as_the_client_grows()
{
	const auto probe = platform::probe_software_tiling();

	// Without this the assertion below would still pass if the buffer went back to tracking the
	// window, which is the thing tiling exists to stop.
	assert_equal(true, probe.grown_buffer_pixels < probe.grown_client_pixels / 4,
	             "the buffer stayed bounded while the client grew");
	assert_equal(probe.grown_client_pixels, probe.grown_writable_pixels,
	             "a grown client is writable to its new edges without reallocating the tile");
}

// A subtitle's text: its runs in their own colours over one outline, in straight alpha, so it draws over any
// picture at any scale.
static void should_render_styled_text_with_its_outline()
{
	platform::styled_text text;
	text.runs = {{"Red ", false, false, 0xFF0000FFu}, {"white", true, true, 0xFFFFFFFFu}};
	text.font_px = 40.0f;
	text.outline_px = 3.0f;
	text.outline_color = 0xFF000000u;

	const auto surface = platform::render_styled_text(text);
	assert_equal(true, surface != nullptr, "the text is drawn");
	if (!surface) return;

	assert_equal(true, surface->width() > 80u && surface->height() >= 40u, "into a block that holds it");

	auto red = 0;
	auto white = 0;
	auto outline = 0;
	auto clear = 0;

	for (auto y = 0; y < static_cast<int>(surface->height()); ++y)
	{
		const auto* const line = surface->pixels_line(y);

		for (auto x = 0; x < static_cast<int>(surface->width()); ++x)
		{
			const auto* const p = line + x * 4;
			const auto b = p[0];
			const auto g = p[1];
			const auto r = p[2];
			const auto a = p[3];

			if (a == 0) ++clear;
			else if (a == 255 && r > 200 && g < 60 && b < 60) ++red;
			else if (a == 255 && r > 200 && g > 200 && b > 200) ++white;
			else if (a == 255 && r < 40 && g < 40 && b < 40) ++outline;
		}
	}

	assert_equal(true, red > 20, "the first run in its colour");
	assert_equal(true, white > 20, "the second in its own");
	assert_equal(true, outline > 20, "around them, the outline");
	assert_equal(true, clear > 0, "and clear around that");

	// Straight alpha: a faint edge keeps its colour rather than darkening towards black.
	auto faint_bright = 0;

	for (auto y = 0; y < static_cast<int>(surface->height()); ++y)
	{
		const auto* const line = surface->pixels_line(y);

		for (auto x = 0; x < static_cast<int>(surface->width()); ++x)
		{
			const auto* const p = line + x * 4;
			if (p[3] > 0 && p[3] < 128 && (p[0] > 128 || p[1] > 128 || p[2] > 128)) ++faint_bright;
		}
	}

	text.outline_px = 0.0f;
	const auto unoutlined = platform::render_styled_text(text);
	assert_equal(true, unoutlined != nullptr, "text with no outline");

	if (unoutlined)
	{
		for (auto y = 0; y < static_cast<int>(unoutlined->height()); ++y)
		{
			const auto* const line = unoutlined->pixels_line(y);

			for (auto x = 0; x < static_cast<int>(unoutlined->width()); ++x)
			{
				const auto* const p = line + x * 4;
				if (p[3] > 0 && p[3] < 128 && (p[0] > 128 || p[1] > 128 || p[2] > 128)) ++faint_bright;
			}
		}
	}

	assert_equal(true, faint_bright > 0, "a glyph's soft edge keeps its colour: the alpha is straight");

	text.runs = {{"  \n ", false, false, 0xFFFFFFFFu}};
	assert_equal(true, platform::render_styled_text(text) == nullptr, "blank text draws nothing");

	text.runs = {{"Text", false, false, 0xFFFFFFFFu}};
	text.font_px = 0.5f;
	assert_equal(true, platform::render_styled_text(text) == nullptr, "nor does text too small to see");
}

// The platform's text engine lays text out by one set of rules on every platform: a line measures one
// line height, longer text measures wider, a line break or wrapping adds lines, and a layout's offsets
// advance along the text to its end.
static void should_measure_text_by_one_set_of_rules()
{
	using ui::style::text_style;
	constexpr auto face = ui::style::font_face::dialog;

	const auto engine = ui::create_text_engine(ui::default_font_size);
	assert_equal(true, engine != nullptr, "the platform has a text engine");
	if (!engine) return;

	const auto near = [](const int a, const int b) { return std::abs(a - b) <= 1; };
	const auto line = engine->text_line_height(face);
	assert_equal(true, line >= ui::default_font_size && line <= ui::default_font_size * 2, "a line is a little over the font size");

	const auto one = engine->measure_text("Ag", face, text_style::single_line, 1000, 0);
	assert_equal(true, near(line, one.cy), "one line measures one line height");
	assert_equal(true, one.cx > 0, "and has a width");

	const auto longer = engine->measure_text("Agreeable text", face, text_style::single_line, 1000, 0);
	assert_equal(true, longer.cx > one.cx, "longer text measures wider");
	assert_equal(true, near(line, longer.cy), "on the same one line");

	const auto two = engine->measure_text("Ag\nAg", face, text_style::single_line, 1000, 0);
	assert_equal(true, near(line * 2, two.cy), "a line break adds a line");

	constexpr auto words = "one two three four five six seven eight nine ten";
	const auto wrapped = engine->measure_text(words, face, text_style::multiline, longer.cx, 0);
	assert_equal(true, wrapped.cy >= line * 2 - 1, "multiline text wraps to its width");
	assert_equal(true, wrapped.cx <= longer.cx, "and keeps within it");
	const auto unwrapped = engine->measure_text(words, face, text_style::single_line, longer.cx, 0);
	assert_equal(true, near(line, unwrapped.cy), "single line text does not wrap");
	assert_equal(true, unwrapped.cx > longer.cx, "and is as wide as it is");

	assert_equal(true, engine->measure_text("", face, text_style::single_line, 1000, 0) == sizei{},
	             "nothing measures nothing");

	const auto layout = engine->create_text_layout(face);
	assert_equal(true, layout != nullptr, "a layout");
	if (!layout) return;

	layout->update("abc", text_style::single_line);
	const auto xs = layout->offset_xs({0, 1, 2, 3}, 1000);
	assert_equal(4u, static_cast<uint32_t>(xs.size()), "an x for every offset");
	if (xs.size() != 4) return;

	assert_equal(0, xs[0], "the first at the start");
	assert_equal(true, xs[1] > xs[0] && xs[2] > xs[1] && xs[3] > xs[2], "each later one further along");
	assert_equal(true, near(xs[3], layout->measure_text(1000, 1000).cx), "and the last at the text's end");
}

// Text drawn through a software context lands inside its box, centred where its style centres it, and a
// highlight colours the characters it covers.
static void should_draw_text_inside_its_box()
{
	constexpr sizei extent{240, 80};
	const auto target = ui::create_memory_present_target();
	const auto ctx = ui::create_software_draw_context(ui::create_text_engine(ui::default_font_size), target,
	                                                  ui::default_font_size);
	const recti box(40, 20, 200, 60);

	const auto draw = [&](const std::vector<ui::text_highlight_t>& highlights)
	{
		ctx->begin_draw(extent, ui::default_font_size, {});
		ctx->clear(ui::color(0.0f, 0.0f, 0.0f, 1.0f));
		ctx->draw_text("Hello there", highlights, box, ui::style::font_face::dialog,
		               ui::style::text_style::single_line_center, ui::color(1.0f, 1.0f, 1.0f, 1.0f), {});
		ctx->render();
	};

	draw({});

	auto buffer = target->acquire_buffer(extent);
	recti ink(INT_MAX, INT_MAX, INT_MIN, INT_MIN);
	auto inside = 0;
	auto outside = 0;

	for (auto y = 0; y < extent.cy; ++y)
	{
		for (auto x = 0; x < extent.cx; ++x)
		{
			const auto* const p = buffer.bits + y * buffer.stride + x * 4;
			if (p[1] < 128) continue;

			if (box.contains({x, y})) ++inside;
			else ++outside;

			ink.left = std::min(ink.left, x);
			ink.top = std::min(ink.top, y);
			ink.right = std::max(ink.right, x + 1);
			ink.bottom = std::max(ink.bottom, y + 1);
		}
	}

	assert_equal(true, inside > 40, "the text is drawn");
	assert_equal(0, outside, "inside its box");
	assert_equal(true, std::abs(ink.center().x - box.center().x) <= 3, "centred across it");
	assert_equal(true, std::abs(ink.center().y - box.center().y) <= 6, "and down it");

	const ui::text_highlight_t red{6, 5, ui::color(1.0f, 0.0f, 0.0f, 1.0f)};
	draw({red});
	buffer = target->acquire_buffer(extent);

	auto reds = 0;
	auto whites = 0;

	for (auto y = box.top; y < box.bottom; ++y)
	{
		for (auto x = box.left; x < box.right; ++x)
		{
			const auto* const p = buffer.bits + y * buffer.stride + x * 4;
			if (p[2] > 160 && p[1] < 80) ++reds;
			else if (p[2] > 160 && p[1] > 160) ++whites;
		}
	}

	assert_equal(true, reds > 10, "a highlight colours the word it covers");
	assert_equal(true, whites > 10, "and leaves the rest in the text's colour");
}

#ifdef _WIN32
void register_render_win_tests(test_registry& tests);
#endif
void register_render_controls_tests(test_registry& tests);
void register_render_elements_tests(test_registry& tests);

static void register_tests(test_registry& tests)
{
#ifdef _WIN32
	register_render_win_tests(tests);
#endif
	register_render_controls_tests(tests);
	register_render_elements_tests(tests);
	tests.add("Should unpremultiply surface alpha"s, should_unpremultiply_surface_alpha);
	tests.add("Should keep a surface block allocated again at its size"s,
	          should_keep_a_surface_block_allocated_again_at_its_size);
	tests.add("Should transform surfaces for every orientation"s, should_transform_surfaces_for_every_orientation);
	tests.add("Should match SIMD software blends"s, should_match_simd_software_blends);
	tests.add("Should area downscale packed and planar surfaces"s, should_area_downscale_packed_and_planar_surfaces);
	tests.add("Should bilinear resize packed surfaces"s, should_bilinear_resize_packed_surfaces);
	tests.add("Should animate alpha between values"s, should_animate_alpha_between_values);
	tests.add("Should fade at the same rate on any refresh rate"s, should_fade_at_the_same_rate_on_any_refresh_rate);
	tests.add("Should skip alpha animation when disabled"s, should_skip_alpha_animation_when_disabled);
	tests.add("Should leave colour unchanged when neutral"s, should_leave_colour_unchanged_when_neutral);
	tests.add("Should desaturate to grey"s, should_desaturate_to_grey);
	tests.add("Should present vertex rectangles updated after the scene"s,
	          should_present_vertex_rectangles_updated_after_the_scene);
	tests.add("Should bound the software buffer as the client grows"s,
	          should_bound_the_software_buffer_as_the_client_grows);
	tests.add("Should render styled text with its outline"s, should_render_styled_text_with_its_outline);
	tests.add("Should measure text by one set of rules"s, should_measure_text_by_one_set_of_rules);
	tests.add("Should draw text inside its box"s, should_draw_text_inside_its_box);
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
