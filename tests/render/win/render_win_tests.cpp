// libdesktop-render's Windows backend tests: Direct3D 11, software textures, video frames, fonts and text.

#include "libdesktop/render.h"
#include "libdesktop/render/software.h"
#include "libdesktop/render/win32.h"
#include "test.h"

using namespace std::literals;

static void should_classify_dxgi_device_loss()
{
	assert_equal(true, is_device_loss_error(DXGI_ERROR_DEVICE_REMOVED), "device removed");
	assert_equal(true, is_device_loss_error(DXGI_ERROR_DEVICE_RESET), "device reset");
	assert_equal(true, is_device_loss_error(DXGI_ERROR_DEVICE_HUNG), "device hung");
	assert_equal(true, is_device_loss_error(DXGI_ERROR_DRIVER_INTERNAL_ERROR), "driver internal error");
	assert_equal(false, is_device_loss_error(DXGI_STATUS_OCCLUDED), "occlusion is not device loss");
	assert_equal(false, is_device_loss_error(E_FAIL), "generic failure is not device loss");
	assert_equal(false, is_device_loss_error(S_OK), "success is not device loss");
}

static void should_recognise_the_basic_render_driver()
{
	assert_equal(true, is_software_adapter(0x1414, 0x8c, 0), "the Microsoft Basic Render Driver");
	assert_equal(true, is_software_adapter(0x10de, 0x2684, DXGI_ADAPTER_FLAG_SOFTWARE),
	             "any adapter that says it is software");
	assert_equal(false, is_software_adapter(0x8086, 0x3ea0, 0), "an Intel integrated GPU");
	assert_equal(false, is_software_adapter(0x10de, 0x2684, 0), "an NVIDIA card");
	assert_equal(false, is_software_adapter(0x1414, 0x5353, 0), "the vendor alone does not make an adapter WARP");
}

// NV12 and P010 are drawn by sampling views of their planes. A device that could only hold the format
// accepted the texture, refused its views, and the picture never appeared.
static void should_need_sampling_for_planar_video()
{
	assert_equal(false, can_sample_texture_format(D3D11_FORMAT_SUPPORT_TEXTURE2D), "holding the format is not enough");
	assert_equal(false, can_sample_texture_format(D3D11_FORMAT_SUPPORT_SHADER_SAMPLE),
	             "nor is sampling a format that cannot be a 2D texture");
	assert_equal(false, can_sample_texture_format(0), "nothing supported");
	assert_equal(true,
	             can_sample_texture_format(D3D11_FORMAT_SUPPORT_TEXTURE2D | D3D11_FORMAT_SUPPORT_SHADER_SAMPLE |
	                                       D3D11_FORMAT_SUPPORT_DECODER_OUTPUT),
	             "a device that samples it");
}

// Intel parts report the 128MB carved out at boot as dedicated memory, and allocate textures from shared
// memory. Read as the card's own, it held every Intel laptop at the 64MB floor, and a photo over 16
// megapixels was shown downscaled even at 100%.
static void should_count_shared_memory_for_an_integrated_gpu()
{
	constexpr uint64_t mb = 1024ull * 1024ull;
	constexpr uint64_t gb = 1024ull * mb;
	constexpr auto laptop = static_cast<int64_t>(16 * gb);
	const auto budget = [](const uint64_t gpu_bytes, const int64_t total_phys)
	{
		return static_cast<uint64_t>(calc_texture_budget(gpu_bytes, total_phys));
	};

	assert_equal(128 * mb + 8 * gb, gpu_texture_memory(128 * mb, 8 * gb, true),
	             "an integrated part uses shared memory");
	assert_equal(8 * gb, gpu_texture_memory(8 * gb, 16 * gb, false), "a discrete card uses its own");
	assert_equal(8 * gb, gpu_texture_memory(0, 8 * gb, false), "an adapter with none of its own still has somewhere");

	assert_equal(128 * mb, budget(gpu_texture_memory(128 * mb, 8 * gb, true), laptop),
	             "an integrated laptop GPU is given the full ceiling");
	assert_equal(64 * mb, budget(128 * mb, laptop), "which the carve-out alone held at the floor");
	assert_equal(64 * mb, budget(512 * mb, laptop), "a small discrete card is still held back");
	assert_equal(128 * mb, budget(8 * gb, laptop), "a large card is held to the ceiling");
	assert_equal(96 * mb, budget(gpu_texture_memory(128 * mb, 768 * mb, true), static_cast<int64_t>(1536 * mb)),
	             "physical memory still bounds a small machine");
	assert_equal(128 * mb, budget(0, 0), "with nothing to ask, the shipped ceiling");
}

static ComPtr<ID3D11Device> create_warp_device(ComPtr<ID3D11DeviceContext>& context)
{
	ComPtr<ID3D11Device> device;

	if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0,
	                             D3D11_SDK_VERSION, &device, nullptr, &context)))
	{
		return {};
	}

	return device;
}

static ComPtr<ID3D11Texture2D> create_plain_texture(ID3D11Device* device, const core::sizei dims)
{
	D3D11_TEXTURE2D_DESC desc = {};
	desc.Width = dims.cx;
	desc.Height = dims.cy;
	desc.MipLevels = 1;
	desc.ArraySize = 1;
	desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
	desc.SampleDesc.Count = 1;
	desc.Usage = D3D11_USAGE_DEFAULT;
	desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;

	ComPtr<ID3D11Texture2D> texture;
	device->CreateTexture2D(&desc, nullptr, &texture);
	return texture;
}

// Thumbnails come and go by the hundred as a listing scrolls, and each was a CreateTexture2D on the UI
// thread. A released texture is handed to the next one of its exact size and format - but never while
// a scene a redraw can replay still draws it, or that redraw would show the new picture in its place.
static void should_recycle_a_texture_only_when_nothing_draws_it()
{
	ComPtr<ID3D11DeviceContext> context;
	const auto device = create_warp_device(context);
	assert_equal(true, device != nullptr, "a WARP device");
	if (!device) return;

	constexpr core::sizei thumb = {64, 48};
	constexpr auto argb = core::texture_format::ARGB;
	const auto thumb_bytes = d3d11_texture_pool::texture_bytes(thumb, argb);
	d3d11_texture_pool pool(thumb_bytes * 8);

	const auto released = create_plain_texture(device.Get(), thumb);
	pool.give({thumb, argb, released, {}});
	assert_equal(true, pool.take({64, 50}, argb).texture == nullptr, "a different size is not handed out");
	assert_equal(true, pool.take(thumb, core::texture_format::RGB).texture == nullptr, "nor a different format");
	assert_equal(true, pool.take(thumb, argb).texture == released, "the same size and format is");
	assert_equal(0_z, pool.size(), "and leaves the pool");

	auto drawn = std::make_shared<const texture_views>();
	pool.give({thumb, argb, released, drawn});
	assert_equal(true, pool.take(thumb, argb).texture == nullptr, "not while a scene still draws it");
	drawn.reset();
	assert_equal(true, pool.take(thumb, argb).texture == released, "but once nothing does");

	std::vector<ComPtr<ID3D11Texture2D>> made;

	for (auto i = 0; i < 9; ++i)
	{
		made.emplace_back(create_plain_texture(device.Get(), thumb));
		pool.give({thumb, argb, made.back(), {}});
	}

	assert_equal(8_z, pool.size(), "the budget holds eight");
	assert_equal(true, pool.bytes() <= thumb_bytes * 8, "and never more than it");
	assert_equal(true, pool.take(thumb, argb).texture == made[1], "the oldest went to make room");

	const auto photograph = create_plain_texture(device.Get(), {128, 128});
	const auto before = pool.size();
	pool.give({{128, 128}, argb, photograph, {}});
	assert_equal(before, pool.size(), "a texture over an eighth of the budget is not kept");

	pool.clear();
	assert_equal(0_z, pool.size(), "a cleared pool holds nothing");
	assert_equal(0_z, pool.bytes(), "and costs nothing");
}

static void should_fall_back_for_missing_glyphs()
{
	constexpr char32_t hangul = U'\uAC00'; // 가

	// "Calibri" is the app's dialog/UI font; "Malgun Gothic" is the Windows Korean font.
	const auto probe = render::probe_glyph_fallback("Calibri", "Malgun Gothic", hangul);

	if (!probe.available)
	{
		// Neither font is guaranteed on every machine. Assert the probe reported that
		// honestly rather than silently passing with nothing checked.
		assert_equal(0, probe.primary_glyph, "an unavailable probe reports no glyph data");
		return;
	}

	// 1. The primary UI font genuinely lacks Hangul -> fallback is mandatory.
	assert_equal(0, probe.primary_glyph, "Calibri has no Hangul glyph (fallback required)");

	// 2. The fallback face maps the same character to a real glyph, and querying
	//    that face for the glyph (what render_glyph SHOULD do) succeeds.
	assert_equal(true, probe.fallback_glyph != 0, "fallback face has a Hangul glyph");
	assert_equal(true, probe.fallback_metrics_ok, "fallback-face metrics query succeeds (correct face)");

	// 3. Querying the PRIMARY face for the same (fallback) glyph index yields the
	//    wrong glyph's metrics - the latent render_glyph bug.
	assert_equal(true, probe.primary_metrics_differ,
	             "primary-face metrics for a fallback glyph are wrong (latent bug)");
}

// Issue #232 - a missing Calibri produced a huge log, because every draw re-ran the family
// lookup and re-logged the failure. Issue #189 - after toggling Large Font some glyphs kept
// drawing at the old size. Both are properties of the same font-face cache: a repeated
// request must be served from the cache, and the requested size must be part of the identity
// so a size change cannot return the previous face.
static void should_cache_font_faces_per_face_and_size()
{
	const auto probe = render::probe_font_cache(16);

	if (!probe.available)
	{
		// No text engine on this machine. Say so rather than passing with nothing checked.
		assert_equal(0, probe.entries_after_first, "an unavailable probe reports no cache entries");
		return;
	}

	assert_equal(1, probe.entries_after_first, "one request caches exactly one face");

	// #232: the repeat is answered from the cache, so no lookup and no log line repeats.
	assert_equal(true, probe.same_request_is_cached, "an identical request is served from the cache");

	// #189: the size is part of the cache identity.
	assert_equal(true, probe.size_change_is_distinct, "a font-size change yields a distinct face");
	assert_equal(true, probe.face_change_is_distinct, "a face-type change yields a distinct face");

	// A settings change resets the fonts; nothing may survive that.
	assert_equal(true, probe.reset_clears_cache, "resetting fonts empties the cache");
}

// #189 again, one level down: a cached glyph raster belongs to a size as well as to a face.
// IDWriteFontFace carries no size - the size lives on the glyph run - so a cache keyed on the
// face alone serves a raster made at the previous font size to text drawn at the current one,
// which is how a popup ended up mixing glyph sizes within one string.
static void should_key_glyph_cache_by_size()
{
	glyph_face_keys keys;

	assert_equal(true, keys.key(nullptr, 16.0f, 42) == keys.key(nullptr, 16.0f, 42),
	             "the same face, size and glyph give the same key");
	assert_equal(true, keys.key(nullptr, 16.0f, 42) != keys.key(nullptr, 24.0f, 42),
	             "a font-size change yields a distinct glyph key");
	assert_equal(true, keys.key(nullptr, 16.0f, 42) != keys.key(nullptr, 16.0f, 43),
	             "a different glyph yields a distinct key");
}

static void should_translate_highlight_spans_to_utf16_clusters()
{
	const auto highlight = core::color(1.0f, 0.0f, 0.0f, 1.0f);
	const auto plain = core::color(0.0f, 0.0f, 1.0f, 1.0f);
	const std::string text = "\xf0\x9f\x98\x80"
							 "caf\xc3\xa9"
							 "X";
	const auto converted = render::utf8_to_utf16(text, {{4, 5, highlight}});

	assert_equal(7_z, converted.text.size(), "emoji is preserved as a surrogate pair");
	assert_equal(0xd83d, static_cast<int>(converted.text[0]), "lead surrogate");
	assert_equal(0xde00, static_cast<int>(converted.text[1]), "trail surrogate");
	assert_equal(1_z, converted.highlights.size(), "one highlight survives conversion");
	assert_equal(2u, converted.highlights[0].offset, "highlight begins after the surrogate pair");
	assert_equal(4u, converted.highlights[0].length, "highlight length is UTF-16 text, not UTF-8 bytes");

	const uint16_t clusters[] = {0, 0, 1};
	const auto first = render::text_cluster_span_for_glyph(0, 2, clusters, 3, 10);
	const auto second = render::text_cluster_span_for_glyph(1, 2, clusters, 3, 10);
	assert_equal(10u, first.offset, "cluster offset includes run text position");
	assert_equal(2u, first.length, "one glyph cluster can span multiple text units");
	assert_equal(12u, second.offset, "the next glyph starts after the multi-unit cluster");
	assert_equal(highlight.r, render::text_cluster_color(plain, first, {{10, 2, highlight}}).r,
	             "a complete cluster is highlighted");
	assert_equal(highlight.r, render::text_cluster_color(plain, first, {{10, 1, highlight}}).r,
	             "a base letter highlight colours its combining-mark cluster");
}

static void should_build_text_cluster_spans_once_per_run()
{
	const uint16_t clusters[] = {0, 0, 1, 2, 2};
	const auto spans = render::text_cluster_spans_for_glyph_run(3, clusters, 5, 4);

	assert_equal(3_z, spans.size(), "one span per glyph");
	assert_equal(4u, spans[0].offset, "first cluster starts at run text position");
	assert_equal(2u, spans[0].length, "first cluster spans two text units");
	assert_equal(6u, spans[1].offset, "second cluster follows");
	assert_equal(1u, spans[1].length, "second cluster is one text unit");
	assert_equal(7u, spans[2].offset, "third cluster follows");
	assert_equal(2u, spans[2].length, "third cluster spans two text units");
}

static void should_apply_positive_ascender_offsets_upward()
{
	assert_equal(80.0f, render::glyph_top_from_baseline(100.0f, 20, 0.0f), "no ascender offset");
	assert_equal(77.0f, render::glyph_top_from_baseline(100.0f, 20, 3.0f), "positive ascender moves up");
	assert_equal(83.0f, render::glyph_top_from_baseline(100.0f, 20, -3.0f), "negative ascender moves down");
}

static void should_clear_text_highlights_between_draws()
{
	std::vector<render::text_highlight_t> highlights = {{0, 3, core::color(1.0f, 0.0f, 0.0f, 1.0f)}};
	render::clear_text_highlights(highlights);
	assert_equal(true, highlights.empty(), "plain/layout draws start without stale highlight intervals");
}

static void should_handle_unavailable_font_layout()
{
	text_layout_impl layout(nullptr, render::font_face::dialog);

	layout.update("text", render::text_style::single_line);
	const auto measured = layout.measure_text(100, 100);
	assert_equal(0, measured.cx, "unavailable font has no measured width");
	assert_equal(0, measured.cy, "unavailable font has no measured height");

	const auto offsets = layout.offset_xs({0, 2, 4}, 100, 100);
	assert_equal(3_z, offsets.size(), "unavailable font still answers each requested offset");
	assert_equal(0, offsets[0], "first unavailable offset");
	assert_equal(0, offsets[1], "middle unavailable offset");
	assert_equal(0, offsets[2], "last unavailable offset");
	assert_equal(true, should_retry_font_renderer(nullptr), "GPU renderer retries a transient null font");
	font_renderer::draw(nullptr, nullptr, layout._layout.Get(), {0, 0, 100, 100}, {}, {});
	assert_equal(true, layout._layout == nullptr, "drawing an unavailable layout leaves it unavailable");
}

static void should_snap_gpu_text_in_pixel_coordinates()
{
	assert_equal(true, render::probe_d3d11_pixel_snapping(),
	             "text callbacks report enabled snapping, identity transform and one pixel per DIP");
}

// Direct3D cuts a polygon into triangles where the software renderer scans it, so the two are held to the
// same picture: crisp ones to the same pixels, smooth ones to the same pixels but at their edges. Each
// shader dithers by a level, which is the tolerance.
static void should_draw_polygons_on_direct3d_as_the_software_renderer_does()
{
	constexpr core::sizei extent{96, 64};
	const core::color white(1.0f, 1.0f, 1.0f, 1.0f);
	// Concave, with many corners turned in, so the triangles must be cut around them.
	const std::array<core::pointd, 12> star{{{48.3, 2.7},
	                                         {55.1, 22.4},
	                                         {75.6, 13.9},
	                                         {62.8, 31.3},
	                                         {84.2, 44.6},
	                                         {58.9, 41.8},
	                                         {50.4, 61.2},
	                                         {41.7, 42.3},
	                                         {14.6, 48.9},
	                                         {33.2, 31.7},
	                                         {16.8, 10.2},
	                                         {40.9, 21.6}}};
	const std::array<core::pointd, 4> box{{{3.3, 3.6}, {28.7, 3.6}, {28.7, 29.4}, {3.3, 29.4}}};
	const auto gradient = render::paint::linear({3.3, 0.0}, core::color(0.9f, 0.2f, 0.1f, 1.0f), {28.7, 0.0},
	                                            core::color(0.1f, 0.3f, 0.9f, 0.6f));

	for (const auto smooth : {false, true})
	{
		const auto draw = [&](render::draw_context& dc)
		{
			dc.clear(core::color(0.0f, 0.0f, 0.0f, 1.0f));
			dc.draw_polygon(star, {.fill = core::color(0.2f, 0.8f, 0.4f, 1.0f), .smooth = smooth});
			dc.draw_polygon(box, {.fill = gradient, .border = white, .border_width = 2.5, .smooth = smooth});
		};

		const auto gpu = render::test_draw_d3d11(extent, draw);
		assert_equal(true, gpu != nullptr, "a WARP device draws the scene");
		if (!gpu) return;

		const auto cpu = std::make_shared<core::surface>();
		const auto context = render::create_surface_context(cpu);
		context->begin_draw(extent, render::default_font_size, {});
		draw(*context);
		context->render();

		auto lit = 0;
		auto differ = 0;

		for (auto y = 0; y < extent.cy; ++y)
		{
			for (auto x = 0; x < extent.cx; ++x)
			{
				const auto a = gpu->get_pixel(x, y);
				const auto b = cpu->get_pixel(x, y);
				if ((b & 0xffffff) != 0) ++lit;

				auto worst = 0;
				for (auto shift = 0; shift < 24; shift += 8)
					worst = std::max(worst, std::abs(static_cast<int>(a >> shift & 0xff) -
					                                 static_cast<int>(b >> shift & 0xff)));
				if (worst > (smooth ? 24 : 2)) ++differ;
			}
		}

		assert_equal(true, lit > 1500, "the scene is drawn");
		// Smooth edges are a fringe on one and coverage on the other, which part only at sharp corners; a crisp
		// pixel differs only where an edge passes within the GPU's sub-pixel precision of its centre.
		assert_equal(true, differ <= (smooth ? 16 : 2),
		             std::format("Direct3D draws what the software renderer draws ({} of {} pixels differ)", differ,
		                         lit));
	}
}

// Polygons a vertices object holds are drawn by the scene that draws it, on Direct3D as on the CPU.
static void should_draw_vertex_polygons_on_direct3d()
{
	const std::array<core::pointd, 3> triangle{{{4.0, 4.0}, {28.0, 4.0}, {4.0, 28.0}}};
	const auto gpu = render::test_draw_d3d11({32, 32}, [&](render::draw_context& dc)
	{
		dc.clear(core::color(0.0f, 0.0f, 0.0f, 1.0f));
		const auto verts = dc.create_vertices();
		const render::vertex_polygon polygons[] = {
			{triangle, {.fill = core::color(0.0f, 0.0f, 1.0f, 1.0f), .smooth = false}}};
		verts->update_polygons(polygons);
		dc.draw_vertices(verts);
	});

	assert_equal(true, gpu != nullptr, "a WARP device draws the scene");
	if (!gpu) return;
	assert_equal(0xff0000ffu, gpu->get_pixel(8, 8) | 0xff000000u, "the polygon is drawn");
	assert_equal(0xff000000u, gpu->get_pixel(24, 24) | 0xff000000u, "and nothing beside it");
}

// A grey picture: level(x, y) is 0 to 255, or 0 to 65535 for a wide format, red, green and blue alike.
static core::surface_ptr make_grey_picture(const core::sizei extent, const core::texture_format format,
                                           const std::function<int(int, int)>& level)
{
	const auto picture = std::make_shared<core::surface>();
	if (!picture->alloc(extent, format)) return {};

	for (auto y = 0; y < extent.cy; ++y)
	{
		for (auto x = 0; x < extent.cx; ++x)
		{
			const auto v = static_cast<uint32_t>(level(x, y));

			if (core::is_wide(format))
			{
				auto* const p = std::bit_cast<uint16_t*>(picture->pixels_line(y)) + x * 4;
				p[0] = p[1] = p[2] = static_cast<uint16_t>(v);
				p[3] = 0xffff;
			}
			else
			{
				picture->set_pixel(x, y, 0xff000000u | v << 16 | v << 8 | v);
			}
		}
	}

	return picture;
}

// Full-range NV12: luma(x, y) for each pixel, and cb and cr for each chroma sample (x, y) of the half-size plane.
static core::surface_ptr make_nv12_picture(const core::sizei extent, const std::function<int(int, int)>& luma,
                                           const std::function<std::pair<int, int>(int, int)>& chroma)
{
	const auto planes = std::make_shared<core::surface>();
	if (!planes->alloc(extent, core::texture_format::NV12)) return {};
	planes->color_space(core::color_space::rec601_full);

	const auto stride = planes->stride();
	auto* const luma_plane = planes->pixels();
	auto* const chroma_plane = luma_plane + stride * extent.cy;

	for (auto y = 0; y < extent.cy; ++y)
		for (auto x = 0; x < extent.cx; ++x) luma_plane[stride * y + x] = static_cast<uint8_t>(luma(x, y));

	for (auto y = 0; y < extent.cy / 2; ++y)
	{
		for (auto x = 0; x < extent.cx / 2; ++x)
		{
			const auto [cb, cr] = chroma(x, y);
			chroma_plane[stride * y + x * 2] = static_cast<uint8_t>(cb);
			chroma_plane[stride * y + x * 2 + 1] = static_cast<uint8_t>(cr);
		}
	}

	return planes;
}

// Draws a picture whole into a target of the given extent with Direct3D, YUV textures allowed.
static core::surface_ptr draw_picture_on_direct3d(const core::const_surface_ptr& picture, const core::sizei extent,
                                                  const render::texture_sampler sampler)
{
	const auto yuv_before = render::yuv_textures_enabled;
	render::yuv_textures_enabled = true;

	auto drawn = render::test_draw_d3d11(extent, [&](render::draw_context& dc)
	{
		dc.clear(core::color(0.0f, 0.0f, 0.0f, 1.0f));
		const auto texture = dc.create_texture();
		texture->update(picture);
		dc.draw_texture(texture, core::recti(0, 0, extent.cx, extent.cy), 1.0f, sampler);
	});

	render::yuv_textures_enabled = yuv_before;
	return drawn;
}

static int red_of(const core::color32 c)
{
	return static_cast<int>(c >> 16 & 0xff);
}

// A picture's chroma is read where its stream sites it. Left-sited chroma lies on every other luma column, so
// the column a sample lies on shows that sample alone; read as centred, that column takes the sample a quarter
// from the one before it. Drawn bilinearly at one to one, the two differ by that blend.
static void should_read_chroma_where_it_is_sited()
{
	constexpr core::sizei extent{8, 4};
	const auto red_across = [&](const core::chroma_siting siting)
	{
		// Neutral and redder chroma samples in turn across the picture, over mid grey.
		const auto planes = make_nv12_picture(extent, [](int, int) { return 128; },
		                                      [](const int x, int) { return std::pair{128, x % 2 ? 160 : 128}; });
		planes->chroma_siting(siting);

		std::array<int, extent.cx> red{};
		const auto gpu = draw_picture_on_direct3d(planes, extent, render::texture_sampler::bilinear);
		if (gpu)
			for (auto x = 0; x < extent.cx; ++x) red[x] = red_of(gpu->get_pixel(x, 1));
		return red;
	};

	const auto left = red_across(core::chroma_siting::left);
	const auto centred = red_across(core::chroma_siting::center);

	// Full-range BT.601: red is luma plus 1.402 times Cr's offset. Column 2 lies on the redder sample (Cr 160)
	// when left-sited, 174; centred, it takes three quarters of it (Cr 152), 162.
	assert_equal(true, std::abs(left[2] - 174) <= 1,
	             std::format("left-sited, column 2 shows the sample ({})", left[2]));
	assert_equal(true, std::abs(centred[2] - 162) <= 1,
	             std::format("centred, column 2 blends it with the one before ({})", centred[2]));
	assert_equal(true, std::abs(left[0] - 128) <= 1 && std::abs(centred[0] - 128) <= 1,
	             std::format("column 0 is the neutral sample either way ({}, {})", left[0], centred[0]));
}

// A picture drawn at a quarter of its size averages the texels under each pixel: stripes one texel white in four
// come out a quarter grey. Read at one point a pixel, they came out black - the point fell between two black
// texels - and as the picture moved, they shimmered between black and grey.
static void should_average_a_picture_drawn_small()
{
	constexpr core::sizei source{96, 96};
	constexpr core::sizei extent{24, 24};
	const auto stripe = [](const int x, int)
	{
		return x % 4 == 0 ? 255 : 0;
	};

	for (const auto& [name, picture] :
	     {std::pair{"RGB", make_grey_picture(source, core::texture_format::RGB, stripe)},
	      std::pair{"NV12", make_nv12_picture(source, stripe, [](int, int) { return std::pair{128, 128}; })}})
	{
		const auto gpu = draw_picture_on_direct3d(picture, extent, render::texture_sampler::bilinear);
		assert_equal(true, gpu != nullptr, "a WARP device draws the scene");
		if (!gpu) return;

		auto off = 0;
		std::string row;

		for (auto y = 0; y < extent.cy; ++y)
			for (auto x = 0; x < extent.cx; ++x)
				if (std::abs(red_of(gpu->get_pixel(x, y)) - 64) > 2) ++off;

		for (auto x = 0; x < extent.cx; ++x) row += std::format(" {}", red_of(gpu->get_pixel(x, 3)));

		assert_equal(0, off, std::format("{}: every pixel is the stripes' average (row 3:{})", name, row));
	}
}

// A colour the 8-bit target cannot hold - a 16-bit texel between two levels - is dithered between them, so a
// gradient of such colours does not band. One it holds exactly - an 8-bit texel drawn at one to one - is the
// same pixel it was.
static void should_dither_only_what_the_target_cannot_hold()
{
	constexpr core::sizei extent{16, 16};
	const auto exact = make_grey_picture(extent, core::texture_format::RGB,
	                                     [](const int x, const int y) { return 100 + (x + y) % 7; });
	const auto exact_drawn = draw_picture_on_direct3d(exact, extent, render::texture_sampler::point);
	assert_equal(true, exact_drawn != nullptr, "a WARP device draws the scene");
	if (!exact_drawn) return;

	auto changed = 0;

	for (auto y = 0; y < extent.cy; ++y)
		for (auto x = 0; x < extent.cx; ++x)
			if (red_of(exact_drawn->get_pixel(x, y)) != 100 + (x + y) % 7) ++changed;

	assert_equal(0, changed, "an 8-bit picture at one to one is drawn exactly");

	// Level 100.5 of 255, held in sixteen bits.
	const auto between = make_grey_picture(extent, core::texture_format::RGBX16, [](int, int) { return 25829; });
	const auto between_drawn = draw_picture_on_direct3d(between, extent, render::texture_sampler::point);
	assert_equal(true, between_drawn != nullptr, "a WARP device draws the scene");
	if (!between_drawn) return;

	auto lower = 0;
	auto upper = 0;

	for (auto y = 0; y < extent.cy; ++y)
	{
		for (auto x = 0; x < extent.cx; ++x)
		{
			const auto red = red_of(between_drawn->get_pixel(x, y));
			if (red == 100) ++lower;
			if (red == 101) ++upper;
		}
	}

	assert_equal(extent.cx * extent.cy, lower + upper, "every pixel is one of the two levels either side");
	assert_equal(true, lower > 64 && upper > 64,
	             std::format("and both are used, half and half ({} and {})", lower, upper));
}

void register_render_win_tests(test_registry& tests)
{
	tests.add("Should read chroma where it is sited"s, should_read_chroma_where_it_is_sited);
	tests.add("Should average a picture drawn small"s, should_average_a_picture_drawn_small);
	tests.add("Should dither only what the target cannot hold"s, should_dither_only_what_the_target_cannot_hold);
	tests.add("Should draw polygons on Direct3D as the software renderer does"s,
	          should_draw_polygons_on_direct3d_as_the_software_renderer_does);
	tests.add("Should draw vertex polygons on Direct3D"s, should_draw_vertex_polygons_on_direct3d);
	tests.add("Should snap GPU text in pixel coordinates"s, should_snap_gpu_text_in_pixel_coordinates);
	tests.add("Should classify DXGI device loss"s, should_classify_dxgi_device_loss);
	tests.add("Should recognise the Microsoft Basic Render Driver as software"s,
	          should_recognise_the_basic_render_driver);
	tests.add("Should need shader sampling for planar video textures"s, should_need_sampling_for_planar_video);
	tests.add("Should count shared memory for an integrated GPU"s, should_count_shared_memory_for_an_integrated_gpu);
	tests.add("Should recycle a texture only when nothing draws it"s,
	          should_recycle_a_texture_only_when_nothing_draws_it);
	tests.add("Issue #219: Should fall back for missing glyphs"s, should_fall_back_for_missing_glyphs);
	tests.add("Issue #232/#189: Should cache font faces per face and size"s, should_cache_font_faces_per_face_and_size);
	tests.add("Issue #189: Should key glyph cache by size"s, should_key_glyph_cache_by_size);
	tests.add("Should translate highlight spans to utf16 clusters"s,
	          should_translate_highlight_spans_to_utf16_clusters);
	tests.add("Should build text cluster spans once per run"s, should_build_text_cluster_spans_once_per_run);
	tests.add("Should apply positive ascender offsets upward"s, should_apply_positive_ascender_offsets_upward);
	tests.add("Should clear text highlights between draws"s, should_clear_text_highlights_between_draws);
	tests.add("Should handle unavailable font layout"s, should_handle_unavailable_font_layout);
}
