// This file is part of libdesktop, a C++20 platform layer for desktop applications
// Copyright 2026  Zac Walker
// 
// This program is free software; you can redistribute it and / or modify it
// under the terms of the LGPL License either version 2.1 or later.
// License details are available at https://www.gnu.org/licenses/lgpl-2.1.html
// This program is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY

// Purpose: The parts of the CPU rasterizer that are not inline - the present target that presents
// nothing, and the probe that proves tiled rasterisation is seamless and that one platform's
// rasterizer agrees with another's.

#include "libdesktop/render.h"
#include "libdesktop/render/software.h"

bool render::animations_enabled = true;

std::string render::gpu_desc = "unknown"s;
std::string render::gpu_id = "unknown"s;
std::string render::d3d_info = "unknown"s;
int render::max_texture_dimension = 16384;
int64_t render::max_texture_bytes = 128ll * 1024ll * 1024ll;
int64_t render::max_decode_bytes = 1024ll * 1024ll * 1024ll;
std::atomic<const char*> render::rendering_func = "";

namespace
{
	class memory_present_target final : public render::software_present_target
	{
		std::vector<uint8_t> _pixels;
		core::sizei _extent;

	public:
		bool is_layered() const override
		{
			return false;
		}

		render::software_buffer acquire_buffer(const core::sizei extent) override
		{
			if (extent.cx < 1 || extent.cy < 1)
			{
				release_buffer();
				return {};
			}

			if (_extent != extent)
			{
				_pixels.assign(static_cast<size_t>(extent.cx) * extent.cy * 4, 0);
				_extent = extent;
			}

			return {_pixels.data(), _extent, _extent.cx * 4};
		}

		void release_buffer() override
		{
			_pixels.clear();
			_pixels.shrink_to_fit();
			_extent = {};
		}

		bool begin_present() override
		{
			return _extent.cx > 0 && _extent.cy > 0;
		}

		void present_tile(core::recti, core::pointi) override
		{
		}

		void end_present(int) override
		{
		}
	};

	class surface_present_target final : public render::software_present_target
	{
		core::surface_ptr _surface;
		core::sizei _extent;

	public:
		explicit surface_present_target(core::surface_ptr surface) : _surface(std::move(surface))
		{
		}

		bool is_layered() const override
		{
			// A CPU surface is also an alpha-bearing render target, unlike a window copy.
			return true;
		}

		render::software_buffer acquire_buffer(const core::sizei extent) override
		{
			if (!_surface || extent.cx < 1 || extent.cy < 1) return {};

			const auto format = _surface->empty() ? core::texture_format::ARGB : _surface->format();
			const auto packed = format == core::texture_format::RGB || format == core::texture_format::ARGB;
			// A surface that already fits is drawn on in place, so what the caller put there survives
			// - a label drawn onto a canvas keeps the canvas. Only a fresh allocation starts blank.
			if (_surface->empty() || _surface->dimensions() != extent || !packed ||
				_surface->orientation() != core::orientation::top_left)
			{
				if (!_surface->alloc(extent, packed ? format : core::texture_format::ARGB, core::orientation::top_left)) return {};
				_surface->make_blank();
			}
			_extent = extent;

			return {_surface->pixels(), _extent, static_cast<int>(_surface->stride())};
		}

		void release_buffer() override
		{
			// The caller owns the surface and must keep its pixels after render() returns.
		}

		bool begin_present() override
		{
			return _surface && !_surface->empty();
		}

		void present_tile(core::recti, core::pointi) override
		{
		}

		void end_present(int) override
		{
		}
	};

	// An even spread across the whole rendering, so every primitive in the scene is represented and
	// the choice of points carries no judgement about which of them matters.
	void sample_into(std::array<uint32_t, render::rasterizer_sample_count>& samples,
	                 const std::vector<uint8_t>& pixels) noexcept
	{
		const auto pixel_count = pixels.size() / 4;
		const auto step = pixel_count / render::rasterizer_sample_count;

		for (auto i = 0; i < render::rasterizer_sample_count; ++i)
		{
			const auto* const p = pixels.data() + (i * step + step / 2) * 4;
			samples[i] = static_cast<uint32_t>(p[0]) | static_cast<uint32_t>(p[1]) << 8 |
				static_cast<uint32_t>(p[2]) << 16 | static_cast<uint32_t>(p[3]) << 24;
		}
	}
}
render::software_present_target_ptr render::create_memory_present_target()
{
	return std::make_shared<memory_present_target>();
}

render::draw_context_device_ptr render::create_surface_context(const core::surface_ptr& target)
{
	if (!target) return {};

	const auto context = render::create_software_draw_context(render::create_text_engine(render::default_font_size),
	                                                         std::make_shared<surface_present_target>(target),
	                                                         render::default_font_size);
	if (context && !target->empty()) context->begin_draw(target->dimensions(), render::default_font_size, {});
	return context;
}

void render::draw_surface(render::draw_context& dc, const core::const_surface_ptr& surface, const core::recti dst,
                          const float alpha, const render::texture_sampler sampler)
{
	if (!surface || surface->empty() || alpha <= 0.0f || dst.is_empty()) return;

	const auto texture = dc.create_texture();
	if (!texture || texture->update(surface) == render::texture_update_result::failed) return;
	dc.draw_texture(texture, dst, alpha, sampler);
}

render::rasterizer_probe render::probe_software_rasterizer()
{
	render::rasterizer_probe result;

	// Deliberately indivisible by the tile edge below, so tiles are clipped on both axes and no
	// primitive lands on a tile boundary by luck.
	constexpr core::sizei extent{203, 141};
	constexpr int tile_edge = 32;
	constexpr uint8_t fill = 0x40;

	const auto source = std::make_shared<core::surface>();
	auto* const source_pixels = source->alloc(19, 13, core::texture_format::ARGB, core::orientation::top_left);
	if (!source_pixels) return result;

	for (auto y = 0; y < 13; ++y)
	{
		auto* const row = source_pixels + static_cast<ptrdiff_t>(y) * source->stride();

		for (auto x = 0; x < 19; ++x)
		{
			row[x * 4 + 0] = static_cast<uint8_t>(x * 11 + y * 3);
			row[x * 4 + 1] = static_cast<uint8_t>(x * 5 + y * 17);
			row[x * 4 + 2] = static_cast<uint8_t>(x * 23 + y * 7);
			row[x * 4 + 3] = static_cast<uint8_t>(128 + ((x + y) & 63));
		}
	}

	// One glyph-shaped alpha mask, so blend_glyph is covered without needing a font.
	constexpr core::sizei glyph_extent{21, 17};
	std::vector<uint8_t> glyph(static_cast<size_t>(glyph_extent.cx) * glyph_extent.cy);
	for (size_t i = 0; i < glyph.size(); ++i) glyph[i] = static_cast<uint8_t>(i * 37u + 11u);

	const core::recti source_rect(0, 0, 19, 13);

	// A parallelogram spelled out rather than produced by core::quadd::rotate: the digest below is
	// compared across platforms, and sin/cos are the C library's answer rather than the
	// rasterizer's.
	core::quadd rotated;
	rotated[0] = {30.5, 92.25};
	rotated[1] = {118.75, 71.5};
	rotated[2] = {139.25, 149.75};
	rotated[3] = {51.0, 170.5};

	// Every primitive that does non-trivial per-pixel arithmetic, positioned to straddle tile edges.
	const auto draw_scene = [&](const render::software_canvas& canvas)
	{
		canvas.fill_rect(core::recti(5, 7, 190, 44), core::color(0.2f, 0.4f, 0.9f, 1.0f));
		canvas.fill_rect(core::recti(11, 15, 170, 39), core::color(0.9f, 0.1f, 0.3f, 0.45f));
		canvas.fill_rect_gradient(core::recti(3, 33, 199, 96), core::color(0.9f, 0.8f, 0.2f, 1.0f),
		                          core::color(0.1f, 0.2f, 0.6f, 0.7f));
		canvas.fill_rounded_rect(core::recti(17, 51, 145, 121), core::color(0.3f, 0.7f, 0.4f, 0.8f), 23);
		canvas.fill_border_gradient(core::recti(41, 63, 161, 111), core::recti(29, 55, 173, 123),
		                            core::color(0.95f, 0.35f, 0.15f, 0.9f), core::color(0.05f, 0.55f, 0.85f, 0.4f));
		canvas.fill_triangle({13.5, 97.25}, {121.75, 71.5}, {87.25, 137.75}, core::color(0.6f, 0.2f, 0.8f, 0.55f));
		canvas.blend_glyph(57, 19, glyph.data(), glyph_extent, core::color(1.0f, 1.0f, 0.4f, 0.85f));
		canvas.blend_glyph(131, 88, glyph.data(), glyph_extent, core::color(0.2f, 0.9f, 1.0f, 0.7f));
		canvas.blit_surface(*source, source_rect, core::recti(63, 29, 187, 119), 0.75f,
		                    texture_sampler::bilinear, true);
		canvas.blit_surface(*source, source_rect, core::recti(9, 83, 101, 133), 0.6f,
		                    texture_sampler::bicubic, true);
		canvas.blit_surface(*source, source_rect, core::recti(151, 5, 199, 67), 1.0f,
		                    texture_sampler::point, true);
		canvas.blit_quad(*source, source_rect, rotated, 0.8f, texture_sampler::bilinear, true);
	};

	std::vector<uint8_t> reference(static_cast<size_t>(extent.cx) * extent.cy * 4, fill);
	std::vector<uint8_t> tiled(static_cast<size_t>(extent.cx) * extent.cy * 4, fill);

	render::software_canvas whole;
	whole._bits = reference.data();
	whole._stride = extent.cx * 4;
	whole._buffer_extent = extent;
	whole._clip = core::recti(0, 0, extent.cx, extent.cy);
	whole._opaque = true;
	draw_scene(whole);

	std::vector<uint8_t> tile_bits(static_cast<size_t>(tile_edge) * tile_edge * 4);

	for (auto ty = 0; ty < extent.cy; ty += tile_edge)
	{
		for (auto tx = 0; tx < extent.cx; tx += tile_edge)
		{
			const auto tile = core::recti(tx, ty, tx + tile_edge, ty + tile_edge)
				.intersection(core::recti(0, 0, extent.cx, extent.cy));

			std::ranges::fill(tile_bits, fill);

			render::software_canvas canvas;
			canvas._bits = tile_bits.data();
			canvas._stride = tile_edge * 4;
			canvas._buffer_extent = {tile_edge, tile_edge};
			canvas._origin = {tx, ty};
			canvas._clip = tile;
			canvas._opaque = true;
			draw_scene(canvas);

			for (auto y = tile.top; y < tile.bottom; ++y)
			{
				memcpy(tiled.data() + (static_cast<ptrdiff_t>(y) * extent.cx + tile.left) * 4,
				       tile_bits.data() + static_cast<ptrdiff_t>(y - ty) * tile_edge * 4 + (tile.left - tx) * 4,
				       static_cast<size_t>(tile.width()) * 4);
			}

			++result.tiles;
		}
	}

	for (size_t i = 0; i < reference.size(); i += 4)
	{
		if (memcmp(reference.data() + i, tiled.data() + i, 4) != 0) ++result.mismatched_pixels;

		const std::array untouched{fill, fill, fill, fill};
		if (memcmp(reference.data() + i, untouched.data(), 4) != 0) ++result.painted_pixels;
	}

	sample_into(result.samples, reference);
	return result;
}