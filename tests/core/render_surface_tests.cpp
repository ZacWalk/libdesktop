// libdesktop-core's pixel surface and SIMD tests.

#include "libdesktop/core.h"
#include "libdesktop/core/simd.h"
#include "test.h"

using namespace std::literals;

static uint8_t blend_opaque_channel(const uint8_t dest, const float src, const float alpha)
{
	return static_cast<uint8_t>(std::clamp(static_cast<int>(src * alpha + dest * (1.0f - alpha) + 0.5f), 0, 255));
}

static void should_unpremultiply_surface_alpha()
{
	core::surface surface;
	assert_equal(true, surface.alloc(3, 1, core::texture_format::ARGB) != nullptr, "alpha fixture allocated");
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
	core::surface surface;
	auto* const first = surface.alloc(64, 48, core::texture_format::NV12, core::orientation::top_left, 1.0);
	assert_equal(true, first != nullptr, "the surface is allocated");

	assert_equal(true, surface.alloc(64, 48, core::texture_format::NV12, core::orientation::right_top, 2.0) == first,
	             "allocated again at its size, it keeps its block");
	assert_equal(2.0, surface.time(), "and takes the new picture's time");
	assert_equal(true, surface.orientation() == core::orientation::right_top, "and its orientation");

	assert_equal(true, surface.alloc(32, 16, core::texture_format::RGB) != nullptr, "another size is allocated");
	assert_equal(true, surface.dimensions() == core::sizei{32, 16} && surface.size() == surface.stride() * 16,
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
	core::surface stored;
	assert_equal(true, stored.alloc(3, 2, core::texture_format::RGB) != nullptr, "stored fixture allocated");
	const auto at = [](const int x, const int y) { return 0xff000000u | static_cast<uint32_t>(y * 16 + x); };
	for (auto y = 0; y < 2; ++y)
		for (auto x = 0; x < 3; ++x) stored.set_pixel(x, y, at(x, y));

	struct expectation
	{
		core::simple_transform transform;
		core::sizei extent;
		// The stored pixel shown at the displayed top-left, top-right and bottom-left corners.
		core::pointi top_left, top_right, bottom_left;
	};

	const expectation expected[] = {
		{core::simple_transform::flip_h, {3, 2}, {2, 0}, {0, 0}, {2, 1}},
		{core::simple_transform::rot_180, {3, 2}, {2, 1}, {0, 1}, {2, 0}},
		{core::simple_transform::flip_v, {3, 2}, {0, 1}, {2, 1}, {0, 0}},
		{core::simple_transform::transpose, {2, 3}, {0, 0}, {0, 1}, {2, 0}},
		{core::simple_transform::rot_270, {2, 3}, {0, 1}, {0, 0}, {2, 1}},
		{core::simple_transform::transverse, {2, 3}, {2, 1}, {2, 0}, {0, 1}},
		{core::simple_transform::rot_90, {2, 3}, {2, 0}, {2, 1}, {0, 0}},
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

	assert_equal(true, stored.transform(core::simple_transform::none) == nullptr, "none asks for no copy");
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
		auto s = std::make_shared<core::surface>();
		s->alloc(cx, cy, core::texture_format::ARGB, core::orientation::left_bottom);
		s->color_space(core::color_space::rec709_limited);
		return s;
	};

	// Two samples widened to four. Sampling at centres and clamping the outer half sample is what
	// keeps the ends exact; extrapolating instead is what puts a dark fringe round an enlargement.
	{
		const auto src = make_source(2, 1);
		auto* const row = src->pixels_line(0);
		row[0] = row[1] = row[2] = row[3] = 0;
		row[4] = row[5] = row[6] = row[7] = 200;

		core::surface_ptr dst;
		assert_equal(true, core::bilinear_resize(src, dst, {4, 1}), "widen a two sample ramp");
		assert_equal(true, dst->dimensions() == core::sizei{4, 1}, "enlarged dimensions");
		assert_equal(true, dst->format() == core::texture_format::ARGB, "enlarged format");
		assert_equal(true, dst->orientation() == core::orientation::left_bottom, "enlarged orientation");
		assert_equal(true, dst->color_space() == core::color_space::rec709_limited, "enlarged colour space");

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

		core::surface_ptr dst;
		assert_equal(true, core::bilinear_resize(src, dst, {31, 22}), "enlarge a flat surface");

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
		core::surface_ptr dst;
		assert_equal(false, core::area_downscale(src, dst, {32, 8}), "the area filter refuses a mixed resize");
		assert_equal(true, core::bilinear_resize(src, dst, {32, 8}), "bilinear takes a mixed resize");
		assert_equal(true, dst->dimensions() == core::sizei{32, 8}, "mixed resize dimensions");
	}

	// Planar surfaces are four bytes a pixel to this walk; they belong to swscale.
	{
		core::surface nv12;
		nv12.alloc(8, 8, core::texture_format::NV12);
		const auto nv12_view = core::const_surface_ptr(&nv12, [](const core::surface*)
		{
		});

		core::surface_ptr dst;
		assert_equal(false, core::bilinear_resize(nv12_view, dst, {16, 16}), "refuse a planar format");
	}
}

static void should_area_downscale_packed_and_planar_surfaces()
{
	const auto make_source = [](const int cx, const int cy, const core::texture_format format)
	{
		auto s = std::make_shared<core::surface>();
		s->alloc(cx, cy, format, core::orientation::left_bottom);
		s->color_space(core::color_space::rec709_limited);
		return s;
	};

	// A flat source must survive a reduction untouched. Any weight run that does not sum to exactly
	// 256 shifts the level here, which is the failure this whole scheme has to rule out.
	{
		const auto src = make_source(97, 61, core::texture_format::ARGB);

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

		core::surface_ptr dst;
		assert_equal(true, core::area_downscale(src, dst, {13, 9}), "reduce a flat surface");
		assert_equal(true, dst->dimensions() == core::sizei{13, 9}, "reduced dimensions");
		assert_equal(true, dst->format() == core::texture_format::ARGB, "reduced format");
		assert_equal(true, dst->orientation() == core::orientation::left_bottom, "reduced orientation");
		assert_equal(true, dst->color_space() == core::color_space::rec709_limited, "reduced colour space");

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
		const auto src = make_source(64, 64, core::texture_format::RGB);

		for (auto y = 0; y < 64; ++y)
		{
			auto* const row = src->pixels_line(y);
			for (auto x = 0; x < 64; ++x)
			{
				const auto v = static_cast<uint8_t>(((x + y) & 1) ? 200 : 0);
				row[x * 4 + 0] = row[x * 4 + 1] = row[x * 4 + 2] = row[x * 4 + 3] = v;
			}
		}

		core::surface_ptr dst;
		assert_equal(true, core::area_downscale(src, dst, {32, 32}), "reduce a checkerboard");

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
		const auto src = make_source(256, 8, core::texture_format::RGB);

		for (auto y = 0; y < 8; ++y)
		{
			auto* const row = src->pixels_line(y);
			for (auto x = 0; x < 256; ++x)
			{
				row[x * 4 + 0] = row[x * 4 + 1] = row[x * 4 + 2] = static_cast<uint8_t>(x);
				row[x * 4 + 3] = 255;
			}
		}

		core::surface_ptr dst;
		assert_equal(true, core::area_downscale(src, dst, {64, 2}), "reduce a ramp");

		const auto* const row = dst->pixels_line(0);
		assert_equal(2, static_cast<int>(row[0]), "ramp first sample");
		assert_equal(254, static_cast<int>(row[63 * 4]), "ramp last sample");

		auto monotonic = true;
		for (auto x = 1; x < 64; ++x) monotonic = monotonic && row[x * 4] > row[(x - 1) * 4];
		assert_equal(true, monotonic, "ramp stays monotonic");
	}

	// Refusals hand the work back to swscale rather than producing a wrong picture.
	{
		const auto rgb = make_source(8, 8, core::texture_format::RGB);
		core::surface_ptr dst;
		assert_equal(false, core::area_downscale(rgb, dst, {16, 4}), "refuse an enlargement");
		assert_equal(false, core::area_downscale(rgb, dst, {0, 4}), "refuse an empty extent");

		// P010 samples are 16 bit, which the Q8 accumulators cannot hold.
		core::surface p010;
		p010.alloc(8, 8, core::texture_format::P010);
		const auto p010_view = core::const_surface_ptr(&p010, [](const core::surface*)
		{
		});
		assert_equal(false, core::area_downscale(p010_view, dst, {4, 4}), "refuse a 16 bit planar format");

		// NV12 chroma is one pair per 2x2 luma block, so an odd extent has no whole answer.
		core::surface nv12;
		nv12.alloc(8, 8, core::texture_format::NV12);
		const auto nv12_view = core::const_surface_ptr(&nv12, [](const core::surface*)
		{
		});
		assert_equal(false, core::area_downscale(nv12_view, dst, {5, 4}), "refuse an odd nv12 extent");
	}

	// NV12 reduces as two planes: the luma on its own, and the interleaved chroma pair at half the
	// extent. A flat picture must survive both, and U must not bleed into V.
	{
		core::surface nv12;
		nv12.alloc(64, 32, core::texture_format::NV12);
		nv12.color_space(core::color_space::rec601_full);

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

		const auto nv12_view = core::const_surface_ptr(&nv12, [](const core::surface*)
		{
		});

		core::surface_ptr dst;
		assert_equal(true, core::area_downscale(nv12_view, dst, {32, 16}), "reduce an nv12 surface");
		assert_equal(true, dst->format() == core::texture_format::NV12, "reduced nv12 format");
		assert_equal(true, dst->color_space() == core::color_space::rec601_full, "reduced nv12 colour space");

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
		const auto src = make_source(40, 10, core::texture_format::RGB);

		for (auto y = 0; y < 10; ++y)
		{
			auto* const row = src->pixels_line(y);
			for (auto x = 0; x < 40; ++x)
			{
				row[x * 4 + 0] = row[x * 4 + 1] = row[x * 4 + 2] = row[x * 4 + 3] = static_cast<uint8_t>(
					(x == 20) ? 240 : 10);
			}
		}

		core::surface_ptr dst;
		assert_equal(true, core::area_downscale(src, dst, {40, 5}), "reduce one axis only");
		assert_equal(240, static_cast<int>(dst->pixels_line(0)[20 * 4]), "identity axis keeps the column");
		assert_equal(10, static_cast<int>(dst->pixels_line(0)[19 * 4]), "identity axis keeps its neighbour");
	}

	// The AVX2 and SSE2 accumulators must agree byte for byte, or a picture would depend on the CPU
	// it was reduced on. Sizes are deliberately not multiples of the vector widths so both the wide
	// loops and their scalar tails are covered.
	{
		const auto src = make_source(211, 97, core::texture_format::ARGB);
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

		for (const auto extent : {core::sizei{53, 29}, core::sizei{211, 12}, core::sizei{7, 97}, core::sizei{210, 96}})
		{
			core::surface_ptr wide;
			core::surface_ptr baseline;
			assert_equal(true, core::area_downscale(src, wide, extent), "reduce with the selected width");
			assert_equal(true, core::area_downscale_baseline(src, baseline, extent), "reduce with the SSE2 baseline");

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



void register_render_surface_tests(test_registry& tests)
{
	tests.add("Should unpremultiply surface alpha"s, should_unpremultiply_surface_alpha);
	tests.add("Should keep a surface block allocated again at its size"s, should_keep_a_surface_block_allocated_again_at_its_size);
	tests.add("Should transform surfaces for every orientation"s, should_transform_surfaces_for_every_orientation);
	tests.add("Should match SIMD software blends"s, should_match_simd_software_blends);
	tests.add("Should area downscale packed and planar surfaces"s, should_area_downscale_packed_and_planar_surfaces);
	tests.add("Should bilinear resize packed surfaces"s, should_bilinear_resize_packed_surfaces);
}
