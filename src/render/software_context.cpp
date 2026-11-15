// This file is part of libdesktop, a C++20 platform layer for desktop applications
// Copyright 2026  Zac Walker
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

// Purpose: The CPU draw context. Records the scene, then replays it a tile at a time through one fixed
// 512-square system-memory BGRA buffer walked across the damaged region, so the buffer does not track
// the window size. Where that buffer comes from and where a finished tile goes is the
// render::software_present_target seam, and text is the render::software_text_engine seam, both in
// render_software.h; everything here is portable. Used as the fallback when hardware acceleration is
// unavailable, for dialogs and bubble popups, and for every window on Linux.

#include "libdesktop/render.h"
#include "libdesktop/render/software.h"
#include "libdesktop/core/simd.h"
#include "polygon.h"

//////////////////////////////////////////////////////////////////////////////////////////////
// YUV -> BGRA conversion. NV12/P010 surfaces (produced by the JPEG and video decoders for the
// GPU YUV path) are not directly presentable by the CPU canvas, which expects BGRA. Convert
// them once at texture-update time so the rest of the software draw path is unchanged. The
// affine matrix comes from core::compute_yuv_matrix() so both renderers agree.
//////////////////////////////////////////////////////////////////////////////////////////////

namespace
{
	// Convert an NV12 or P010 4:2:0 surface to a top-down BGRA surface (straight alpha, opaque).
	// The chroma plane follows the full-resolution luma plane at half height, with 2x1-packed
	// interleaved U,V samples shared across each 2x2 luma block (nearest-neighbour chroma upsample).
	void convert_yuv_to_bgra(const core::surface& src, core::surface& dst) noexcept
	{
		const auto dims = src.dimensions();
		const int w = dims.cx;
		const int h = dims.cy;
		const auto stride = src.stride();
		const auto mat = core::compute_yuv_matrix(src.color_space());
		const bool p010 = src.format() == core::texture_format::P010;
		// P010 keeps its 10 significant bits at the top of each 16 bit word, so full scale is
		// 1023 << 6 = 65472, not 65535. Normalising by 65535 leaves every sample 0.096% low.
		const float norm = p010 ? (1.0f / 65472.0f) : (1.0f / 255.0f);

		const auto* const y_base = src.pixels();
		const auto* const c_base = y_base + stride * h; // chroma plane follows luma

		for (int y = 0; y < h; ++y)
		{
			auto* const out = dst.pixels_line(y);
			const auto* const c_row = c_base + (y >> 1) * stride;

			if (p010)
			{
				const auto* const y_row = std::bit_cast<const uint16_t*>(y_base + y * stride);
				const auto* const cp = std::bit_cast<const uint16_t*>(c_row);

				for (int x = 0; x < w; ++x)
				{
					const int cx = (x >> 1) * 2;
					const float yy = static_cast<float>(y_row[x]) * norm;
					const float u = static_cast<float>(cp[cx]) * norm;
					const float v = static_cast<float>(cp[cx + 1]) * norm;

					auto* const px = out + x * 4;
					px[0] = render::to_byte(mat.m[8] * yy + mat.m[9] * u + mat.m[10] * v + mat.m[11]);
					px[1] = render::to_byte(mat.m[4] * yy + mat.m[5] * u + mat.m[6] * v + mat.m[7]);
					px[2] = render::to_byte(mat.m[0] * yy + mat.m[1] * u + mat.m[2] * v + mat.m[3]);
					px[3] = 255;
				}
			}
			else
			{
				const auto* const y_row = y_base + y * stride;

				for (int x = 0; x < w; ++x)
				{
					const int cx = (x >> 1) * 2;
					const float yy = static_cast<float>(y_row[x]) * norm;
					const float u = static_cast<float>(c_row[cx]) * norm;
					const float v = static_cast<float>(c_row[cx + 1]) * norm;

					auto* const px = out + x * 4;
					px[0] = render::to_byte(mat.m[8] * yy + mat.m[9] * u + mat.m[10] * v + mat.m[11]);
					px[1] = render::to_byte(mat.m[4] * yy + mat.m[5] * u + mat.m[6] * v + mat.m[7]);
					px[2] = render::to_byte(mat.m[0] * yy + mat.m[1] * u + mat.m[2] * v + mat.m[3]);
					px[3] = 255;
				}
			}
		}
	}
}

namespace
{
	//////////////////////////////////////////////////////////////////////////////////////////////
	// software_texture - a system-memory surface used as a drawable image source.
	//////////////////////////////////////////////////////////////////////////////////////////////

	class software_texture final : public render::texture
	{
	public:
		core::surface_ptr _surface = std::make_shared<core::surface>();
		// A wide source as it was given, for the graded draw; _surface holds it at a byte a channel.
		core::const_surface_ptr _wide;
		std::unique_ptr<render::surface_scaler> _scaler;

		// Cache of the source scaled to a display size via FFmpeg's SIMD swscale, so full-screen video
		// is scaled once per frame with optimised code instead of a scalar per-pixel bilinear resample.
		std::unique_ptr<render::surface_scaler> _display_scaler;
		core::surface_ptr _display_surface;
		core::sizei _display_dims;
		bool _display_valid = false;
		bool _display_high_quality = false;

		software_texture() = default;

		~software_texture() override { _scaler.reset(); }

		bool is_valid() const override { return core::is_valid(_surface); }

		// Convert an NV12/P010 (YUV 4:2:0) source into the stored BGRA surface. The CPU canvas can only
		// present BGRA, so YUV frames/images that would be colour-converted on the GPU are converted here.
		// render::yuv_textures_enabled is false while this backend is active, so an image decoder never asks
		// for planes; what still arrives is a video frame, or a surface decoded before the device was lost.
		render::texture_update_result update_yuv(const core::surface& src)
		{
			_display_valid = false; // source changed - drop the display-scaled cache
			if (!_scaler && render::make_surface_scaler) _scaler = render::make_surface_scaler();

			const auto dimensions = src.dimensions();
			const auto result = (_surface->empty() || _surface->dimensions() != dimensions)
			                        ? render::texture_update_result::tex_created
			                        : render::texture_update_result::tex_updated;

			if (!_scaler || !_scaler->convert_yuv_surface(src, _surface))
			{
				// alloc returns null rather than throwing when the buffer cannot be reserved, and the
				// conversion below writes straight into it.
				if (!_surface->alloc(dimensions, core::texture_format::RGB, src.orientation(), src.time()))
				{
					return render::texture_update_result::failed;
				}

				_surface->color_space(src.color_space());
				convert_yuv_to_bgra(src, *_surface);
			}

			_dimensions = dimensions;
			_format = core::texture_format::RGB;
			_orientation = src.orientation();
			return result;
		}

		render::texture_update_result update(const core::sizei dimensions, const core::texture_format format,
		                                     const core::orientation orientation, const uint8_t* pixels,
		                                     const size_t stride, const size_t buffer_size) override
		{
			if (dimensions.cx < 1 || dimensions.cy < 1 || !pixels || stride == 0)
			{
				return render::texture_update_result::failed;
			}

			if (core::is_wide(format))
			{
				auto wide = std::make_shared<core::surface>();
				if (!wide->alloc(dimensions, format, orientation)) return render::texture_update_result::failed;

				const auto rows = std::min(static_cast<size_t>(dimensions.cy),
				                           buffer_size ? buffer_size / stride : dimensions.cy);

				for (size_t y = 0; y < rows; ++y)
				{
					memcpy(wide->pixels_line(static_cast<int>(y)), pixels + y * stride,
					       std::min(wide->stride(), stride));
				}

				return update(core::const_surface_ptr(std::move(wide)));
			}

			_wide.reset();
			_display_valid = false; // source changed - drop the display-scaled cache

			const auto result = (_surface->empty() || _surface->dimensions() != dimensions)
			                        ? render::texture_update_result::tex_created
			                        : render::texture_update_result::tex_updated;

			// alloc returns null rather than throwing when the buffer cannot be reserved, and the
			// copy below writes straight into it.
			if (!_surface->alloc(dimensions, format, orientation))
			{
				return render::texture_update_result::failed;
			}

			const auto copy_stride = std::min(_surface->stride(), stride);
			const auto rows = std::min(static_cast<size_t>(dimensions.cy),
			                           buffer_size ? buffer_size / stride : dimensions.cy);

			for (size_t y = 0; y < rows; ++y)
			{
				memcpy(_surface->pixels_line(static_cast<int>(y)), pixels + y * stride, copy_stride);
			}

			_dimensions = dimensions;
			_format = format;
			_orientation = orientation;
			return result;
		}

		render::texture_update_result update(const render::video_frame_ptr& frame) override
		{
			_wide.reset();

			// Converted on the decode thread - to BGRA, since this backend samples no YUV - so presenting
			// it is a copy and nothing more.
			if (const auto prepared = frame->prepared_surface())
			{
				return update(prepared);
			}

			if (!_scaler && render::make_surface_scaler) _scaler = render::make_surface_scaler();

			// A hardware picture is downloaded and converted below on the UI thread, which would repeat
			// for every frame of the clip. The decode thread does it instead once asked, for the rest of
			// the run - this backend never samples the decoder's surfaces.
			if (frame->picture().texture) frame->request_cpu_frames();

			_display_valid = false; // new frame - drop the display-scaled cache

			// The first frame must report tex_created like the two overloads above: tex_updated maps to a
			// redraw, which replays the scene recorded before this texture existed, so the video would never
			// be drawn at all.
			const auto was_empty = _surface->empty();

			if (_scaler && _scaler->scale_surface(*frame, _surface))
			{
				const auto created = was_empty || _dimensions != _surface->dimensions();
				_dimensions = _surface->dimensions();
				_format = _surface->format();
				_orientation = _surface->orientation();
				return created ? render::texture_update_result::tex_created
				               : render::texture_update_result::tex_updated;
			}

			return render::texture_update_result::failed;
		}

		render::texture_update_result update(const core::const_surface_ptr& s) override
		{
			core::assert_true(platform::is_ui_thread());

			if (core::is_valid(s))
			{
				const auto fmt = s->format();

				if (fmt == core::texture_format::NV12 || fmt == core::texture_format::P010)
				{
					_wide.reset();
					return update_yuv(*s);
				}

				if (core::is_wide(fmt))
				{
					// Every draw reads the byte-a-channel copy but a graded one, which samples the sixteen bits
					// so an edit's curve works at the picture's own precision.
					const auto narrow = core::to_8_bit(s);
					if (!narrow) return render::texture_update_result::failed;

					const auto result = update(narrow->dimensions(), narrow->format(), narrow->orientation(),
					                           narrow->pixels(), narrow->stride(), narrow->size());
					if (result != render::texture_update_result::failed) _wide = s;
					return result;
				}

				return update(s->dimensions(), s->format(), s->orientation(), s->pixels(), s->stride(), s->size());
			}

			return render::texture_update_result::failed;
		}

		// Return the source surface scaled to dst (BGRA), cached until the source frame changes. Falls
		// back to the unscaled source if the format is unsupported by swscale.
		const core::surface_ptr& display_scaled(const core::sizei dst, const bool high_quality)
		{
			if (_surface->dimensions() == dst) return _surface;

			if (_display_valid && _display_dims == dst && _display_high_quality == high_quality)
				return _display_surface;

			if (!_display_scaler && render::make_surface_scaler) _display_scaler = render::make_surface_scaler();

			if (_display_scaler && _display_scaler->scale_surface(_surface, _display_surface, dst, high_quality) &&
			    core::is_valid(_display_surface))
			{
				_display_dims = dst;
				_display_high_quality = high_quality;
				_display_valid = true;
				return _display_surface;
			}

			_display_valid = false;
			return _surface;
		}
	};

	using software_texture_ptr = std::shared_ptr<software_texture>;

	// Detect an axis-aligned, upright (non-rotated, non-flipped) quad and return it as a rect, so the
	// common non-rotated texture draw can take the faster rect / swscale path instead of blit_quad.
	static bool quad_upright_rect(const core::quadd& q, core::recti& out)
	{
		const auto p0 = q[0];
		const auto p1 = q[1];
		const auto p2 = q[2];
		const auto p3 = q[3];

		constexpr double eps = 0.01;
		const auto eq = [](const double a, const double b)
		{
			return std::abs(a - b) < eps;
		};

		if (eq(p0.Y, p1.Y) && eq(p2.Y, p3.Y) && eq(p0.X, p3.X) && eq(p1.X, p2.X) && p1.X - p0.X > eps &&
		    p3.Y - p0.Y > eps)
		{
			out = core::recti(core::round(p0.X), core::round(p0.Y), core::round(p1.X), core::round(p2.Y));
			return true;
		}

		return false;
	}

	//////////////////////////////////////////////////////////////////////////////////////////////
	// software_polygon - a polygon as the CPU rasterizer draws it.
	//////////////////////////////////////////////////////////////////////////////////////////////

	// Its outline and its border's pieces, worked out once when it is recorded or updated rather than for
	// every tile it is replayed into.
	class software_polygon
	{
	public:
		std::vector<core::pointd> outline;
		std::vector<core::pointd> border;
		render::paint fill;
		core::color border_color;
		core::recti bounds;
		bool smooth = true;

		// False when it draws nothing.
		bool prepare(const std::span<const core::pointd> points, const render::polygon_style& style)
		{
			border.clear();
			if (!render::polygon::clean_outline(points, outline)) return false;

			fill = style.fill;
			border_color = style.border;
			smooth = style.smooth;
			bounds = render::polygon::bounds(outline, style);
			if (style.has_border())
				render::polygon::border_pieces(render::polygon::corners(outline), style.border_width, border);
			return fill.is_visible() || !border.empty();
		}

		void draw(const render::software_canvas& canvas) const
		{
			if (!canvas._clip.intersects(bounds)) return;

			if (fill.is_visible())
			{
				const uint32_t ends[] = {static_cast<uint32_t>(outline.size())};
				canvas.fill_contours(outline, ends, fill, smooth);
			}

			if (!border.empty())
			{
				thread_local std::vector<uint32_t> ends;
				ends.clear();
				for (auto end = 4u; end <= border.size(); end += 4) ends.push_back(end);
				canvas.fill_contours(border, ends, border_color, smooth);
			}
		}
	};

	//////////////////////////////////////////////////////////////////////////////////////////////
	// software_vertices - the audio visualizer's bars, flat rectangles or polygons.
	//////////////////////////////////////////////////////////////////////////////////////////////

	class software_vertices final : public render::vertices
	{
	public:
		std::vector<core::recti> _rects;
		std::vector<core::color> _colors;
		std::vector<software_polygon> _polygons;
		// Bars are drawn over a shadow with an emphasized centre; plain rectangles are filled as given.
		bool _bars = false;

		// Raised, the classic bars: the glass is the Direct3D backend's alone.
		void update_bars(const std::span<const render::vertex_rect> bars) override
		{
			update_rects(bars);
			_bars = true;
		}

		void update_rects(const std::span<const render::vertex_rect> rects) override
		{
			_bars = false;
			_rects.clear();
			_colors.clear();
			_polygons.clear();

			for (const auto& r : rects)
			{
				_rects.emplace_back(r.bounds);
				_colors.emplace_back(r.fill);
			}
		}

		void update_polygons(const std::span<const render::vertex_polygon> polygons) override
		{
			_bars = false;
			_rects.clear();
			_colors.clear();
			_polygons.resize(polygons.size());

			auto kept = 0u;
			for (const auto& polygon : polygons)
			{
				if (_polygons[kept].prepare(polygon.outline, polygon.style)) ++kept;
			}

			_polygons.resize(kept);
		}
	};

	//////////////////////////////////////////////////////////////////////////////////////////////
	// software_draw_context
	//////////////////////////////////////////////////////////////////////////////////////////////

	// Edge of the fixed scratch tile the software backend rasterises into. The scene is retained, so
	// render() replays it once per tile instead of keeping a buffer the size of the window: 512x512 is
	// 1 MiB, the largest power-of-two square that stays resident in a typical per-core L2.
	constexpr int software_tile_extent = 512;

	class software_draw_context final :
		public render::draw_context_device,
		public std::enable_shared_from_this<software_draw_context>
	{
	public:
		render::software_text_engine_ptr _text;
		render::software_present_target_ptr _target;
		bool _layered = false;
		int _base_font_size = render::default_font_size;
		int _layer_alpha = 255;

		render::software_canvas _canvas;
		std::vector<core::recti> _clip_stack;

		// Recorded command list (retained mode). Each draw call appends a closure; render() replays
		// them into the buffer. This lets redraw() re-present a frame - re-sampling textures updated in
		// place (e.g. video) - without re-running the host's paint logic, matching the Direct3D
		// backend's behaviour so the software renderer is a drop-in replacement.
		struct scene_command
		{
			// What the command can touch, so a tile beyond it is spared the call; empty for one every tile
			// must run, as a clip change must.
			core::recti bounds;
			std::function<void()> run;
		};

		std::vector<scene_command> _scene;
		// True while replay_scene() is running. Some draw paths re-enter the context during replay
		// (e.g. a text engine paints a text background with draw_rounded_rect); when replaying those
		// calls must rasterise immediately rather than append to _scene, which is being iterated.
		bool _replaying = false;

		// The scratch tile, not the window: one tile edge of software_tile_extent unless the client is
		// smaller, or the whole client for a layered surface (which cannot be tiled - see render).
		render::software_buffer _buffer;
		// Window-space position of the tile currently being rasterised.
		core::pointi _buffer_origin;
		// The window client size, which is the space the recorded scene is laid out in.
		core::sizei _client_extent;

		// Region this frame is allowed to touch, set by begin_draw or reset_damage, and whether the scene
		// opens with an opaque clear of the whole client, which leaves the neutral pre-clear nothing to show.
		core::recti _damage;
		bool _scene_opens_opaque = false;

		software_draw_context(render::software_text_engine_ptr text, render::software_present_target_ptr target,
		                      const int base_font_size) :
			_text(std::move(text)), _target(std::move(target)), _base_font_size(base_font_size)
		{
			_layered = _target && _target->is_layered();
			if (_text) _text->base_font_size(base_font_size);
		}

		~software_draw_context() override { free_buffer(); }

		void free_buffer()
		{
			if (_target) _target->release_buffer();
			_buffer = {};
			_buffer_origin = {};
			sync_canvas();
		}

		core::recti tile_bounds() const noexcept
		{
			return {_buffer_origin.x, _buffer_origin.y, _buffer_origin.x + _buffer.extent.cx,
			        _buffer_origin.y + _buffer.extent.cy};
		}

		// The canvas is a view onto the target's buffer, so it must be re-pointed wherever that buffer
		// is acquired or released. Doing it in one place keeps a failed or zero-size allocation from
		// leaving the canvas describing a non-empty surface backed by freed (or null) pixels.
		void sync_canvas()
		{
			_canvas._bits = _buffer.bits;
			_canvas._stride = _buffer.stride;
			_canvas._buffer_extent = _buffer.extent;
			_canvas._origin = _buffer_origin;
			_canvas._clip = tile_bounds();
			_canvas._opaque = !_layered;
		}

		void set_tile_origin(const core::pointi origin)
		{
			_buffer_origin = origin;
			_canvas._origin = origin;
		}

		core::sizei buffer_extent_for(const core::sizei client) const
		{
			// A layered surface is presented whole, so it cannot be tiled and keeps a full-client buffer.
			if (_layered) return client;

			// Capped at the tile so the allocation never tracks the window, floored at what is already
			// allocated so a resize drag cannot reallocate on the way back down.
			const auto edge = [](const int want, const int have)
			{
				return std::max(have, std::min(software_tile_extent, std::max(want, 1)));
			};

			return {edge(client.cx, _buffer.extent.cx), edge(client.cy, _buffer.extent.cy)};
		}

		void ensure_buffer(const core::sizei sz)
		{
			if (!_target)
			{
				_buffer = {};
			}
			else
			{
				_buffer = _target->acquire_buffer(sz);
				_buffer_origin = {};
			}

			sync_canvas();
		}

		// draw_context_device -------------------------------------------------------------------

		bool is_valid() const override { return _buffer.bits != nullptr; }

		void reset_damage(const core::recti region) override
		{
			const core::recti client(0, 0, _client_extent.cx, _client_extent.cy);
			_damage = region.is_empty() || _layered ? client : region.intersection(client);
		}

		void begin_draw(const core::sizei client_extent, const int base_font_size, const core::recti damage) override
		{
			// Through update_font_size, not a bare assignment: recording the size here without
			// discarding the text renderers would leave them built at the previous size while
			// making the later update_font_size call believe it had nothing to do.
			update_font_size(base_font_size);
			_client_extent = client_extent;
			ensure_buffer(buffer_extent_for(client_extent));

			const core::recti client(0, 0, client_extent.cx, client_extent.cy);

			// The tile carries nothing between frames, so a partial repaint needs no retained pixels:
			// every pixel of the damaged region is written each frame, by the scene's own opening clear
			// or by the neutral pre-clear in replay_scene. A layered surface is presented whole.
			_damage = (damage.is_empty() || _layered) ? client : damage.intersection(client);

			_scene_opens_opaque = false;
			_clip_stack.clear();
			// Recording-phase clip: the widest region the frame may touch, so clip_bounds() queries and
			// the clip captured by draw_texture see the window, not whichever tile replay reaches first.
			_canvas._clip = _damage;
			_scene.clear();
		}

		// Replay the recorded command list into the tile. Called for every tile of every present, so that
		// redraw() (which re-presents without re-running the host paint logic) reflects textures whose
		// contents changed in place - e.g. a new video frame decoded into the same software_texture.
		// Correct to run per tile because every primitive derives its colour, coverage and source mapping
		// from its own bounds and uses the clip only as a write mask, which is also why a command whose
		// bounds miss the tile can be passed over.
		void replay_scene(const core::recti tile_clip)
		{
			_clip_stack.clear();
			_canvas._clip = tile_clip;

			// The hardware backend clears the render target before drawing the scene, so anything the
			// scene does not paint shows as this neutral grey rather than black. Layered windows are
			// composited by the shell and must stay transparent where nothing is drawn. A scene that
			// opens by covering the client opaquely leaves nothing to show through, so the pre-clear
			// would only be overwritten.
			if (!_layered && !_scene_opens_opaque)
			{
				_canvas.clear(
					core::color(render::scene_clear_shade, render::scene_clear_shade, render::scene_clear_shade, 1.0f));
			}

			struct replay_scope
			{
				bool& flag;
				~replay_scope() { flag = false; }
			} scope{_replaying};
			_replaying = true;

			// Iterate by index: a re-entrant draw during replay runs immediately (see record_or_run)
			// and never appends, so the size is stable, but this is defensive against reallocation.
			for (size_t i = 0; i < _scene.size(); ++i)
			{
				const auto& command = _scene[i];
				if (command.bounds.is_empty() || command.bounds.intersects(tile_clip)) command.run();
			}
		}

		// Append a draw command that touches no more than bounds while recording, or run it immediately
		// when called re-entrantly during replay (so it does not mutate the _scene vector that
		// replay_scene() is iterating). Empty bounds run on every tile.
		template <typename F> void record_or_run(const core::recti bounds, F&& f)
		{
			if (_replaying) f();
			else _scene.push_back({bounds.is_empty() ? core::recti{} : bounds, std::forward<F>(f)});
		}

		// What a primitive between two points, and as wide as width, can touch.
		static core::recti span_bounds(const core::pointd a, const core::pointd b, const double width)
		{
			const auto reach = width / 2.0 + 2.0;
			const auto limit = [](const double v)
			{
				return static_cast<int>(std::clamp(v, -1e9, 1e9));
			};
			return {limit(std::floor(std::min(a.X, b.X) - reach)), limit(std::floor(std::min(a.Y, b.Y) - reach)),
			        limit(std::ceil(std::max(a.X, b.X) + reach)), limit(std::ceil(std::max(a.Y, b.Y) + reach))};
		}

		// Walk the scratch tile across a region, positioning the canvas for each step. Anchored to the
		// region, not to a global grid: nothing is reused between frames, so a small repaint straddling
		// a grid line should still cost one tile. Shared with the tiling probe so tests drive this loop.
		template <typename F> void for_each_tile(const core::recti region, F&& fn)
		{
			// The step is the tile, so a degenerate one would not advance: fail the frame, never hang.
			if (_buffer.extent.cx < 1 || _buffer.extent.cy < 1) return;

			for (auto y = region.top; y < region.bottom; y += _buffer.extent.cy)
			{
				for (auto x = region.left; x < region.right; x += _buffer.extent.cx)
				{
					const auto tile = core::recti(x, y, x + _buffer.extent.cx, y + _buffer.extent.cy)
					                      .intersection(region);
					if (tile.is_empty()) continue;

					set_tile_origin({x, y});
					fn(tile);
				}
			}
		}

		// A CPU backend has no device to lose, so a frame it cannot present is dropped rather than
		// reported: the caller's remedy for a failure is to fall back to this backend.
		render::present_result render() override
		{
			if (!_buffer.bits || !_target) return {};

			const auto region = _damage.intersection(core::recti(0, 0, _client_extent.cx, _client_extent.cy));
			if (region.is_empty()) return {};

			// A layered surface is presented whole, so it cannot be tiled: its buffer is the full client
			// and begin_draw forces full damage, which makes the loop below a single tile covering
			// everything.
			if (!_target->begin_present()) return {};

			for_each_tile(region, [&](const core::recti tile)
			{
				replay_scene(tile);
				_target->present_tile(tile, _buffer_origin);
			});

			_target->end_present(_layer_alpha);

			return {};
		}

		void resize(const core::sizei client_extent) override
		{
			_client_extent = client_extent;
			ensure_buffer(buffer_extent_for(client_extent));
		}

		void destroy() override
		{
			_scene.clear();
			// The engine's glyph caches go with the context; nothing measures through a destroyed one.
			_text.reset();
			free_buffer();
		}

		void update_font_size(const int base_font_size) override
		{
			// Only rebuild when the size actually changes: this runs on every options change, and
			// discarding the engine's fonts re-rasterises every glyph the next frame needs.
			if (_base_font_size == base_font_size) return;
			_base_font_size = base_font_size;
			if (_text) _text->base_font_size(base_font_size);
		}

		void set_layer_alpha(const int a) override { _layer_alpha = std::clamp(a, 0, 255); }

		// clip ----------------------------------------------------------------------------------

		core::recti clip_bounds() const override { return _canvas._clip; }

		void clip_bounds(const core::recti r) override
		{
			// A re-entrant clip during replay must apply immediately; appending here would grow the
			// command list that replay_scene() is iterating and re-apply the clip on every later frame.
			if (_replaying)
			{
				_clip_stack.push_back(_canvas._clip);
				_canvas._clip = _canvas._clip.intersection(r);
				return;
			}

			// Maintain the clip during recording so clip_bounds() queries are correct, and record the
			// same operation so it is re-applied identically during replay.
			_clip_stack.push_back(_canvas._clip);
			_canvas._clip = _canvas._clip.intersection(r);

			_scene.push_back({{},
			                  [this, r]
			{
				_clip_stack.push_back(_canvas._clip);
				_canvas._clip = _canvas._clip.intersection(r);
			}});
		}

		void restore_clip() override
		{
			if (!_clip_stack.empty())
			{
				_canvas._clip = _clip_stack.back();
				_clip_stack.pop_back();
			}

			if (_replaying)
			{
				return;
			}

			_scene.push_back({{},
			                  [this]
			{
				if (!_clip_stack.empty())
				{
					_canvas._clip = _clip_stack.back();
					_clip_stack.pop_back();
				}
			}});
		}

		// primitives ----------------------------------------------------------------------------

		void clear(const core::color c) override
		{
			const core::recti bounds(0, 0, _client_extent.cx, _client_extent.cy);

			// Recorded as the first command, opaque, and covering the whole client: the neutral pre-clear in
			// replay_scene cannot survive it, so replay_scene skips it.
			if (_scene.empty() && !_replaying && c.a >= 1.0f)
			{
				_scene_opens_opaque = true;
			}

			record_or_run({}, [this, bounds, c]
			{
				// fill_rect quite rightly ignores zero-alpha paint, but a layered render-target clear must
				// discard the previous popup before the next shadow is replayed.
				if (_layered && c.a <= 0.0f) _canvas.clear(c);
				else _canvas.fill_rect(bounds, c);
			});
		}

		void draw_rect(const core::recti bounds, const core::color c) override
		{
			if (bounds.is_empty()) return;
			record_or_run(bounds, [this, bounds, c] { _canvas.fill_rect(bounds, c); });
		}

		void draw_line(const core::pointd a, const core::pointd b, const core::color c, const double width) override
		{
			record_or_run(span_bounds(a, b, width), [this, a, b, c, width] { _canvas.draw_line(a, b, c, width); });
		}

		void draw_polyline(const std::span<const core::pointd> points, const core::color c, const double width) override
		{
			if (points.empty()) return;

			core::recti bounds;
			for (size_t i = 0; i < points.size(); ++i)
				bounds = bounds.make_union(span_bounds(points[i], points[i + 1 < points.size() ? i + 1 : i], width));

			std::vector<core::pointd> copy(points.begin(), points.end());
			record_or_run(bounds, [this, points = std::move(copy), c, width]
			{ _canvas.draw_polyline(std::span<const core::pointd>(points), c, width); });
		}

		void draw_ellipse(const core::recti bounds, const core::color c, const double width) override
		{
			if (bounds.is_empty()) return;
			record_or_run(bounds.inflate(1), [this, bounds, c, width] { _canvas.draw_ellipse(bounds, c, width); });
		}

		void fill_ellipse(const core::recti bounds, const core::color c) override
		{
			if (bounds.is_empty()) return;
			record_or_run(bounds.inflate(1), [this, bounds, c] { _canvas.fill_ellipse(bounds, c); });
		}

		// The hardware backend inflates by 2, fills the body with c.emphasize() and lets the circle
		// pixel shader fade the outside away: the visible shape is a rounded rect whose edge sits at
		// 0.833 * (radius + 2) from the corner centres, which is what is reproduced here. The four
		// corner vertices that carry the un-emphasized colour are all in the faded-out region.
		void draw_rounded_rect(const core::recti bounds, const core::color c, const int radius) override
		{
			const auto edge = 0.83333f * (radius + 2);
			const auto grow = std::max(0, core::round(edge - radius));
			const auto r = core::round(edge);

			record_or_run(bounds.inflate(grow + 1), [this, bounds, c, grow, r]
			{ _canvas.fill_rounded_rect(bounds.inflate(grow), c.emphasize(), r); });
		}

		void draw_polygon(const std::span<const core::pointd> outline, const render::polygon_style& style) override
		{
			auto polygon = std::make_shared<software_polygon>();
			if (!polygon->prepare(outline, style)) return;

			const auto bounds = polygon->bounds;
			record_or_run(bounds, [this, polygon = std::move(polygon)] { polygon->draw(_canvas); });
		}

		void draw_border(const core::recti inside, const core::recti outside, const core::color c_inside,
		                 const core::color c_outside) override
		{
			record_or_run(outside, [this, inside, outside, c_inside, c_outside]
			{ _canvas.fill_border_gradient(inside, outside, c_inside, c_outside); });
		}

		void draw_vertices(const render::vertices_ptr& v) override
		{
			auto vv = std::dynamic_pointer_cast<software_vertices>(v);
			if (!vv) return;

			// Capture the vertices object (not a snapshot) and read its geometry at replay time. The
			// audio visualizer mutates the same vertices object in place each frame and then calls
			// redraw() (which replays without re-recording), so replay must see the latest bars - the
			// same way a video texture updated in place is re-sampled on replay. What it covers changes
			// with it, so it runs on every tile.
			record_or_run({}, [this, vv = std::move(vv)]
			{
				for (const auto& polygon : vv->_polygons)
				{
					polygon.draw(_canvas);
				}

				if (!vv->_bars)
				{
					for (size_t i = 0; i < vv->_rects.size(); ++i)
					{
						_canvas.fill_rect(vv->_rects[i], vv->_colors[i]);
					}

					return;
				}

				// Every bar's shadow, then the bars over all of them, each as a centre gradient - note the bar carries the
				// plain colour at its corners and the emphasized one in the middle, the opposite of draw_rect. A flat bar
				// casts no shadow.
				constexpr int shadow_width = 8;
				constexpr float shadow_alpha = 0.35f;

				for (size_t i = 0; i < vv->_rects.size(); ++i)
				{
					const auto r = vv->_rects[i];
					if (r.height() < 2) continue;
					_canvas.fill_shadow(r.inflate(shadow_width), shadow_width, vv->_colors[i].a * shadow_alpha);
				}

				for (size_t i = 0; i < vv->_rects.size(); ++i)
				{
					const auto c = vv->_colors[i];
					_canvas.fill_rect_gradient(vv->_rects[i], c.emphasize(), c);
				}
			});
		}

		render::vertices_ptr create_vertices() override { return std::make_shared<software_vertices>(); }

		// textures ------------------------------------------------------------------------------

		render::texture_ptr create_texture() override
		{
			core::assert_true(platform::is_ui_thread());
			return std::make_shared<software_texture>();
		}

		// `visible` is the clip as the host saw it when the call was recorded, not the live clip: replay
		// runs per tile, and testing the tile would reject every destination below.
		void draw_texture_impl(const software_texture_ptr& t, const core::recti dst, const core::recti src,
		                       const float alpha, const render::texture_sampler sampler,
		                       const core::recti visible) const
		{
			if (!t || !core::is_valid(t->_surface)) return;

			const auto has_alpha = t->_surface->format() == core::texture_format::ARGB;
			const auto src_dims = t->_surface->dimensions();
			const bool full_src = src.left <= 0 && src.top <= 0 && src.right >= src_dims.cx &&
			                      src.bottom >= src_dims.cy;
			const bool scaling = dst.width() != src.width() || dst.height() != src.height();
			const bool fully_visible = visible.intersection(dst) == dst;

			// Fast path: an opaque image/video scaled from its full source. Scale once with FFmpeg's SIMD
			// swscale to a display-sized surface, then a 1:1 copy - far faster than the scalar per-pixel
			// bilinear resample below. Keep this to wholly visible destinations: a magnified photo can have
			// a very large nominal destination, while direct sampling below only visits the clipped viewport.
			// Point sampling is excluded because swscale has no point filter here and would blur the
			// deliberately crisp source pixels of a heavily magnified image.
			if (!has_alpha && full_src && scaling && fully_visible && alpha >= 0.999f &&
			    sampler != render::texture_sampler::point)
			{
				const auto& scaled = t->display_scaled(dst.extent(), sampler == render::texture_sampler::bicubic);

				if (core::is_valid(scaled) && scaled->dimensions() == dst.extent())
				{
					_canvas.blit_copy(*scaled, dst, alpha, false);
					return;
				}
			}

			_canvas.blit_surface(*t->_surface, src, dst, alpha, sampler, has_alpha);
		}

		void draw_texture(const render::texture_ptr& t, const core::recti dst, const float alpha,
		                  const render::texture_sampler sampler) override
		{
			if (!t) return;
			auto tt = std::dynamic_pointer_cast<software_texture>(t);
			const core::recti src(core::pointi(0, 0), t->dimensions());
			const auto visible = _canvas._clip;
			record_or_run(dst, [this, tt = std::move(tt), dst, src, alpha, sampler, visible]
			{ draw_texture_impl(tt, dst, src, alpha, sampler, visible); });
		}

		void draw_texture(const render::texture_ptr& t, const core::recti dst, const core::recti src, const float alpha,
		                  const render::texture_sampler sampler, const float /*radius*/) override
		{
			auto tt = std::dynamic_pointer_cast<software_texture>(t);
			const auto visible = _canvas._clip;
			record_or_run(dst, [this, tt = std::move(tt), dst, src, alpha, sampler, visible]
			{ draw_texture_impl(tt, dst, src, alpha, sampler, visible); });
		}

		static core::recti quad_bounds(const core::quadd& q)
		{
			core::recti bounds;
			for (auto i = 0; i < 4; ++i) bounds = bounds.make_union(span_bounds(q[i], q[(i + 1) % 4], 0.0));
			return bounds;
		}

		void draw_texture(const render::texture_ptr& t, const core::quadd& dst, const core::recti src,
		                  const float alpha, const render::texture_sampler sampler) override
		{
			auto tt = std::dynamic_pointer_cast<software_texture>(t);
			const auto visible = _canvas._clip;
			record_or_run(quad_bounds(dst), [this, tt = std::move(tt), dst, src, alpha, sampler, visible]
			{
				if (!tt || !core::is_valid(tt->_surface)) return;

				// A non-rotated / non-flipped quad is just a rectangle: take the faster rect path (which
				// includes the swscale fast path for full-screen video).
				core::recti rect;
				if (quad_upright_rect(dst, rect))
				{
					draw_texture_impl(tt, rect, src, alpha, sampler, visible);
					return;
				}

				const auto has_alpha = tt->_surface->format() == core::texture_format::ARGB;
				_canvas.blit_quad(*tt->_surface, src, dst, alpha, sampler, has_alpha);
			});
		}

		void draw_texture(const render::texture_ptr& t, const core::quadd& dst, const core::recti src,
		                  const float alpha, const render::texture_sampler sampler,
		                  const render::texture_transform& transform) override
		{
			auto tt = std::dynamic_pointer_cast<software_texture>(t);
			record_or_run(quad_bounds(dst), [this, tt = std::move(tt), dst, src, alpha, sampler, transform]
			{
				if (!tt || !core::is_valid(tt->_surface)) return;
				const auto& source = tt->_wide ? *tt->_wide : *tt->_surface;
				_canvas.blit_quad(source, src, dst, alpha, sampler, core::has_alpha(source.format()), &transform);
			});
		}

		// text ----------------------------------------------------------------------------------

		void draw_text(const std::string_view text, const core::recti bounds, const render::font_face font,
		               const render::text_style style, const core::color c, const core::color bg) override
		{
			if (bounds.is_empty()) return;
			record_or_run(bounds, [this, text = std::string(text), bounds, font, style, c, bg]
			{
				if (!_canvas._clip.intersects(bounds)) return;
				if (_text) _text->draw_text(*this, _canvas, text, bounds, font, style, c, bg);
			});
		}

		void draw_text(const std::string_view text, const std::vector<render::text_highlight_t>& highlights,
		               const core::recti bounds, const render::font_face font, const render::text_style style,
		               const core::color clr, const core::color bg) override
		{
			if (bounds.is_empty()) return;
			record_or_run(bounds, [this, text = std::string(text), highlights, bounds, font, style, clr, bg]
			{
				if (!_canvas._clip.intersects(bounds)) return;
				if (_text) _text->draw_text(*this, _canvas, text, highlights, bounds, font, style, clr, bg);
			});
		}

		void draw_text(const render::text_layout_ptr& tl, const core::recti bounds, const core::color clr,
		               const core::color bg) override
		{
			core::assert_true(platform::is_ui_thread());
			if (!tl || bounds.is_empty()) return;

			record_or_run(bounds, [this, tl, bounds, clr, bg]
			{
				if (!_canvas._clip.intersects(bounds)) return;
				if (_text) _text->draw_text(*this, _canvas, tl, bounds, clr, bg);
			});
		}

		core::sizei measure_text(const std::string_view text, const render::font_face font,
		                         const render::text_style style, const int width, const int height) override
		{
			return _text ? _text->measure_text(text, font, style, width, height) : core::sizei{};
		}

		int text_line_height(const render::font_face font) override
		{
			return _text ? _text->text_line_height(font) : 0;
		}

		int text_baseline(const render::font_face font) override { return _text ? _text->text_baseline(font) : 0; }

		render::text_layout_ptr create_text_layout(const render::font_face font) override
		{
			core::assert_true(platform::is_ui_thread());
			return _text ? _text->create_text_layout(font) : nullptr;
		}
	};

}

render::draw_context_device_ptr render::create_software_draw_context(render::software_text_engine_ptr text,
                                                                     render::software_present_target_ptr target,
                                                                     const int base_font_size)
{
	return std::make_shared<software_draw_context>(std::move(text), std::move(target), base_font_size);
}

// The scratch buffer stops tracking the window, which only a real draw context can be asked. The
// seamlessness of tiling is render::probe_software_rasterizer.
render::software_tiling_probe render::probe_software_tiling()
{
	render::software_tiling_probe result;

	// Both extents exceed the tile, so the buffer is already at its final size and the target will
	// not reallocate across the grow - the case where the canvas last stopped following the window.
	// Driven through the memory target, which is the seam with no window behind it.
	software_draw_context ctx(nullptr, render::create_memory_present_target(), render::default_font_size);
	constexpr core::sizei started{software_tile_extent + 88, software_tile_extent + 88};
	constexpr core::sizei grown{software_tile_extent * 2 + 376, software_tile_extent + 388};

	ctx.begin_draw(started, render::default_font_size, {});
	ctx.begin_draw(grown, render::default_font_size, {});

	const core::recti client(0, 0, grown.cx, grown.cy);
	result.grown_client_pixels = client.width() * client.height();
	result.grown_buffer_pixels = ctx._buffer.extent.cx * ctx._buffer.extent.cy;

	ctx.for_each_tile(client, [&](const core::recti tile)
	{
		ctx._canvas._clip = tile;
		const auto writable = ctx._canvas.clamp_to_clip(client);
		result.grown_writable_pixels += writable.width() * writable.height();
	});

	return result;
}
