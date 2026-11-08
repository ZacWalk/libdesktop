// This file is part of libdesktop, a C++20 platform layer for desktop applications
// Copyright 2026  Zac Walker
//
// This program is free software; you can redistribute it and / or modify it
// under the terms of the LGPL License either version 2.1 or later.
// License details are available at https://www.gnu.org/licenses/lgpl-2.1.html
// This program is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY

// Purpose: Draw and measure contexts, textures, text layout, and render hooks.

#pragma once

namespace render
{
	// Rendering capability consulted by the UI animation helpers.
	extern bool animations_enabled;

	// The text size the scale factor is reckoned from: an application that sets a larger base size scales
	// its whole interface up with it.
	constexpr int default_font_size = 16;
	static constexpr int default_ticks_per_second = 5;

	struct screen_units
	{
		int n;

		int operator*(const double scale) const noexcept
		{
			return core::round(n * scale * 10.0);
		}
	};

	// The widths a form's columns of labels and values align to, shared by its rows as they are measured
	// and drawn.
	struct column_widths
	{
		int c1 = 0;
		int c2 = 0;
		int c3 = 0;
	};
	// What a backend needs to invert a destination pixel back onto the sphere. Packed to the layout
	// pano_project.hlsli declares, and built by panorama_shader_params in ui_panorama.h so the
	// software rasteriser and the shader are handed the same camera.
	struct panorama_params
	{
		float yaw = 0.0f;
		float pitch = 0.0f;
		float tan_half_fov = 0.0f;
		float aspect = 1.0f;

		float longitude_left = 0.0f;
		float u_scale = 1.0f;
		float latitude_top = 0.0f;
		float v_scale = 1.0f;

		// Decides the addressing mode, not the arithmetic: only the blend at the join reads across it.
		bool wraps = false;
	};
	struct color_style
	{
		core::color32 background = 0;
		core::color32 foreground = 0;
		core::color32 selected = 0;

		float alpha = 1.0f;
		float overlay_alpha = 1.0f;
		float bg_alpha = 0.22f;
	};

	enum class font_face : int
	{
		code = 1,
		dialog,
		title,
		mega,
		icons,
		small_icons
	};

	enum class text_style
	{
		none,
		single_line,
		single_line_center,
		single_line_far,
		multiline,
		multiline_center
	};

	enum class texture_sampler
	{
		point,
		bilinear,
		bicubic,
	};

	constexpr std::string_view to_string(const texture_sampler sampler)
	{
		switch (sampler)
		{
		case texture_sampler::point: return "point";
		case texture_sampler::bicubic: return "bicubic";
		case texture_sampler::bilinear: return "bilinear";
		default: break;
		}

		return "?";
	}
	class color_adjust
	{
		static constexpr int curve_len = 0x1000;

		double _curve[curve_len];
		double _saturation = 0;
		double _vibrance = 0;
		double _temperature = 0;
		double _tint = 0;

	public:
		void color_params(double vibrance, double saturation, double darks, double midtones, double lights,
		                  double contrast, double brightness, double temperature = 0, double tint = 0);
		// False when the token stopped it part-way, which leaves the rows it never reached holding
		// whatever the destination allocation came with. A cancelled result is not a picture.
		bool apply(const core::const_surface_ptr& src, uint8_t* dst, size_t dst_stride,
		           const core::cancel_token& token) const;
		void populate_texture_transform(struct texture_transform& transform) const;

	private:
		core::color32 adjust_color(double y, double u, double v, double a) const;
	};

	struct texture_transform
	{
		static constexpr int curve_len = 0x100;

		std::array<float, curve_len> curve{};
		float perspective_horizontal = 0;
		float perspective_vertical = 0;
		float saturation = 1;
		float vibrance = 0;
		float red_gain = 1;
		float green_gain = 1;
		float blue_gain = 1;
		bool has_perspective = false;
		bool has_color_changes = false;

		texture_transform()
		{
			for (auto index = 0; index < curve_len; ++index)
			{
				curve[index] = index / static_cast<float>(curve_len - 1);
			}
		}

		// Lets a backend reuse the transform it already uploaded instead of re-staging the curve.
		bool operator==(const texture_transform&) const = default;

		bool has_changes() const
		{
			return has_perspective || has_color_changes;
		}
	};

	enum class texture_update_result
	{
		tex_created,
		tex_updated,
		failed
	};

	// What a renderer needs to present a video frame: its size, orientation, colour and HDR mapping and -
	// while the decoder still holds the picture on the GPU - the texture it is in and the lock the decode
	// device takes around any use of it. On Windows the texture is an ID3D11Texture2D array and index the
	// slice of it; texture is null for a picture in system memory.
	struct video_picture
	{
		int width = 0;
		int height = 0;
		core::orientation orientation = core::orientation::top_left;
		core::color_space color_space = core::color_space::rec601_limited;
		core::tone_map_lut_ptr tone_map;
		void* texture = nullptr;
		uintptr_t index = 0;
		void (*lock)(void* context) = nullptr;
		void (*unlock)(void* context) = nullptr;
		void* lock_context = nullptr;
	};

	// A decoded video frame, as the application hands one to a texture.
	class video_frame
	{
	public:
		virtual ~video_frame() = default;

		// The picture the decode thread converted for upload, or null while the frame still holds the
		// decoder's own buffers.
		virtual core::const_surface_ptr prepared_surface() const = 0;
		virtual video_picture picture() const = 0;
		// Asked by a renderer that cannot share the decoder's hardware pictures, so from then on the
		// decode thread downloads and converts them.
		virtual void request_cpu_frames() const = 0;
	};

	using video_frame_ptr = std::shared_ptr<const video_frame>;

	// Scales and converts pixels with the application's scaler - FFmpeg's swscale, say - which
	// keeps state from one call to the next, so a texture holds one. make_surface_scaler makes them.
	class surface_scaler
	{
	public:
		virtual ~surface_scaler() = default;

		// A packed surface scaled to dimensions_out.
		virtual bool scale_surface(const core::const_surface_ptr& surface_in, core::surface_ptr& surface_out,
		                           core::sizei dimensions_out, bool high_quality = true) = 0;
		// NV12 or P010 planes converted to packed BGRA at their own size.
		virtual bool convert_yuv_surface(const core::surface& surface_in, const core::surface_ptr& surface_out) = 0;
		// A video frame converted to packed BGRA, a hardware picture downloaded first.
		virtual bool scale_surface(const video_frame& frame, const core::surface_ptr& surface_out) = 0;
	};

	class texture
	{
	public:
		core::sizei _dimensions;
		core::sizei _src_extent;
		core::texture_format _format = core::texture_format::None;
		core::orientation _orientation = core::orientation::top_left;
		core::color_space _cs = core::color_space::rec601_limited;

		virtual ~texture() = default;

		const core::sizei dimensions() const
		{
			return _dimensions;
		}

		core::texture_format format() const
		{
			return _format;
		}

		bool has_alpha() const
		{
			return _format == core::texture_format::ARGB;
		}

		core::sizei source_extent() const
		{
			return _src_extent.is_empty() ? _dimensions : _src_extent;
		}

		virtual texture_update_result update(const video_frame_ptr& frame) = 0;
		virtual texture_update_result update(const core::const_surface_ptr& surface) = 0;
		virtual texture_update_result update(core::sizei dims, core::texture_format format, core::orientation orientation,
		                                     const uint8_t* pixels, size_t stride, size_t buffer_size) = 0;

		// Uploads with a full mip chain, for a source that will be minified a long way - a projected
		// panorama samples thousands of pixels of sphere into one viewport. Separate from update()
		// because every other texture in the app is drawn at or near its own size and would pay for a
		// chain it never reads. A backend with no answer refuses, and the caller rasterises instead.
		virtual texture_update_result update_mipped(const core::const_surface_ptr& surface)
		{
			return texture_update_result::failed;
		}

		virtual bool is_valid() const = 0;
	};

	class text_layout
	{
	public:
		virtual ~text_layout() = default;
		virtual void update(std::string_view text, render::text_style style) = 0;
		virtual core::sizei measure_text(int cx, int cy = 1000) = 0;
		virtual std::vector<int> offset_xs(const std::vector<size_t>& utf8_offsets, int cx, int cy = 1000) = 0;
	};

	struct text_cluster_metric
	{
		uint32_t utf16_length = 0;
		float width = 0.0f;
	};

	inline std::vector<int> logical_text_offset_xs(const std::vector<uint32_t>& utf16_positions,
	                                               const std::vector<text_cluster_metric>& clusters)
	{
		std::vector<std::pair<uint32_t, float>> advances;
		advances.reserve(clusters.size() + 1);
		advances.emplace_back(0u, 0.0f);

		auto utf16_pos = 0u;
		auto advance = 0.0f;

		for (const auto& cluster : clusters)
		{
			utf16_pos += cluster.utf16_length;
			advance += cluster.width;
			advances.emplace_back(utf16_pos, advance);
		}

		std::vector<int> result;
		result.reserve(utf16_positions.size());

		for (const auto position : utf16_positions)
		{
			const auto found = std::ranges::lower_bound(advances, position, {},
			                                            [](const auto& entry) { return entry.first; });
			const auto x = found == advances.end() ? advances.back().second : found->second;
			result.emplace_back(core::round(x));
		}

		return result;
	}

	// A rectangle vertices fill with one colour, flat or raised.
	struct vertex_rect
	{
		core::recti bounds;
		core::color fill;
	};

	// Geometry a scene draws by reference, so an owner can change it and re-present the scene with
	// redraw() rather than recording the scene again. Each update replaces what the last one drew.
	class vertices
	{
	public:
		virtual ~vertices() = default;
		// Raised bars, drawn in order: each over a drop shadow, its fill brightening toward its centre. The
		// hardware backend draws up to 1,024 of them.
		virtual void update_bars(std::span<const vertex_rect> bars) = 0;
		// Flat rectangles, drawn in order. The hardware backend draws up to 16,384 of them.
		virtual void update_rects(std::span<const vertex_rect> rects) = 0;
	};

	using texture_ptr = std::shared_ptr<texture>;
	using vertices_ptr = std::shared_ptr<vertices>;
	using text_layout_ptr = std::shared_ptr<text_layout>;

	struct text_highlight_t
	{
		// For draw_context::draw_text(std::string_view, highlights, ...) these are UTF-8 byte
		// offsets in the supplied text. Backends translate them to UTF-16 before handing text to
		// DirectWrite.
		uint32_t offset = 0;
		uint32_t length = 0;
		core::color clr = {};
	};

	struct utf16_text_highlights
	{
		std::wstring text;
		std::vector<text_highlight_t> highlights;
	};

	inline uint32_t utf16_code_units(const uint32_t cp)
	{
		return cp > 0xffff ? 2u : 1u;
	}

	inline void append_utf16(std::wstring& text, const uint32_t cp)
	{
		if (cp > 0xffff)
		{
			text += static_cast<wchar_t>((cp >> 10) + core::LEAD_OFFSET);
			text += static_cast<wchar_t>((cp & 0x3ff) + core::TRAIL_SURROGATE_MIN);
		}
		else
		{
			text += static_cast<wchar_t>(cp);
		}
	}

	inline uint32_t utf16_position_for_utf8_offset(const std::vector<std::pair<uint32_t, uint32_t>>& positions,
	                                               const uint32_t utf8_offset)
	{
		const auto found = std::ranges::lower_bound(positions, utf8_offset, {},
		                                            [](const auto& entry) { return entry.first; });
		return found == positions.end() ? positions.back().second : found->second;
	}

	inline utf16_text_highlights utf8_to_utf16(const std::string_view text,
	                                          const std::vector<text_highlight_t>& highlights)
	{
		utf16_text_highlights result;
		result.text.reserve(text.size());

		std::vector<std::pair<uint32_t, uint32_t>> positions;
		positions.reserve(text.size() + 1);
		positions.emplace_back(0u, 0u);

		auto i = text.begin();
		auto utf16_pos = 0u;

		while (i < text.end())
		{
			const auto before = i;
			const auto cp = core::pop_utf8_char(i, text.end());
			if (i == before) break;
			append_utf16(result.text, cp);
			utf16_pos += utf16_code_units(cp);
			positions.emplace_back(static_cast<uint32_t>(i - text.begin()), utf16_pos);
		}

		result.highlights.reserve(highlights.size());
		for (const auto& highlight : highlights)
		{
			const auto begin = utf16_position_for_utf8_offset(positions, highlight.offset);
			const auto end_offset = static_cast<uint32_t>(
				std::min<uint64_t>(static_cast<uint64_t>(highlight.offset) + highlight.length, text.size()));
			const auto end = utf16_position_for_utf8_offset(positions, end_offset);
			if (end > begin) result.highlights.push_back({begin, end - begin, highlight.clr});
		}

		return result;
	}

	struct text_cluster_span
	{
		uint32_t offset = 0;
		uint32_t length = 1;
	};

	inline std::vector<text_cluster_span> text_cluster_spans_for_glyph_run(const uint32_t glyph_count,
	                                                                       const uint16_t* const cluster_map,
	                                                                       const uint32_t text_length,
	                                                                       const uint32_t text_position)
	{
		std::vector<text_cluster_span> result;
		result.reserve(glyph_count);

		for (auto glyph = 0u; glyph < glyph_count; ++glyph)
		{
			result.push_back({text_position + glyph, 1});
		}

		if (!cluster_map || text_length == 0) return result;

		std::vector<bool> initialized(glyph_count);
		for (auto text_index = 0u; text_index < text_length; ++text_index)
		{
			const auto glyph = static_cast<uint32_t>(cluster_map[text_index]);
			if (glyph >= glyph_count) continue;

			auto& span = result[glyph];
			const auto position = text_position + text_index;
			if (!initialized[glyph])
			{
				span = {position, 1};
				initialized[glyph] = true;
			}
			else
			{
				span.length = std::max(span.length, position - span.offset + 1);
			}
		}

		return result;
	}

	inline text_cluster_span text_cluster_span_for_glyph(const uint32_t glyph_index, const uint32_t glyph_count,
	                                                     const uint16_t* const cluster_map,
	                                                     const uint32_t text_length,
	                                                     const uint32_t text_position)
	{
		const auto spans = text_cluster_spans_for_glyph_run(glyph_count, cluster_map, text_length, text_position);
		return glyph_index < spans.size() ? spans[glyph_index] : text_cluster_span{text_position + glyph_index, 1};
	}

	inline core::color text_cluster_color(const core::color base, const text_cluster_span cluster,
	                                const std::vector<text_highlight_t>& highlights)
	{
		for (const auto& highlight : highlights)
		{
			const auto begin = highlight.offset;
			const auto end = begin + highlight.length;
			if (cluster.offset >= begin && cluster.offset < end) return highlight.clr;
			if (cluster.offset < end) break;
		}

		return base;
	}

	inline void clear_text_highlights(std::vector<text_highlight_t>& highlights)
	{
		highlights.clear();
	}

	inline float glyph_top_from_baseline(const float baseline_y, const int base_line_height,
	                                     const float ascender_offset)
	{
		return baseline_y - static_cast<float>(base_line_height) - ascender_offset;
	}

	class measure_context : public core::no_copy
	{
	public:
		virtual core::sizei measure_text(std::string_view text, render::font_face font, render::text_style style, int cx,
		                           int cy = 0) = 0;
		virtual int text_line_height(render::font_face font) = 0;
		// From the top of a line of this face to its baseline, in context pixels: the font's ascent with
		// any line gap above it, so text_line_height less this is the descent. Every text backend knows
		// its font and answers exactly; the default is the usual Latin proportion, for contexts that only
		// stand in for one.
		virtual int text_baseline(render::font_face font) { return core::mul_div(text_line_height(font), 4, 5); }
		virtual text_layout_ptr create_text_layout(render::font_face font) = 0;

		double scale_factor = 1.0;

		int icon_cxy = 18;
		int padding2 = 8;
		int padding1 = 4;
		int handle_cxy = 12;
		int scroll_width = 20;
		column_widths col_widths;
	};

	class draw_context : public measure_context
	{
	public:
		double time_now = 0.0;
		color_style colors;
		bool frame_has_focus = false;

		~draw_context() override = default;

		virtual void clear(core::color c) = 0;

		virtual void draw_rounded_rect(core::recti bounds, core::color c, int radius) = 0;
		// Fills a convex polygon; backends anti-alias its edges where their rasterizer does.
		virtual void fill_polygon(std::span<const core::pointd> points, core::color c)
		{
		}
		// Stretches the shadow artwork over bounds, putting its width-pixel band inside the edge;
		// draw_shadow({0, 0, w, h}, 32, 1.0f) is stretch_shadow(shadow, {0, 0, w, h}, 32, 1.0f).
		virtual void draw_shadow(core::recti bounds, int width, float alpha)
		{
		}
		virtual void draw_rect(core::recti bounds, core::color c) = 0;
		virtual void draw_line(core::pointd a, core::pointd b, core::color c, double width = 1.0) = 0;
		virtual void draw_polyline(std::span<const core::pointd> points, core::color c, double width = 1.0) = 0;
		virtual void draw_ellipse(core::recti bounds, core::color c, double width = 1.0) = 0;
		virtual void fill_ellipse(core::recti bounds, core::color c) = 0;
		virtual void draw_text(std::string_view text, core::recti bounds, render::font_face font, render::text_style style,
		                       core::color c, core::color bg) = 0;
		virtual void draw_text(std::string_view text, const std::vector<text_highlight_t>& highlights, core::recti bounds,
		                       render::font_face font, render::text_style style, core::color clr, core::color bg) = 0;
		virtual void draw_text(const text_layout_ptr& tl, core::recti bounds, core::color clr, core::color bg) = 0;
		virtual void draw_border(core::recti inside, core::recti outside, core::color c_inside, core::color c_outside) = 0;
		virtual void draw_texture(const texture_ptr& t, core::recti dst, float alpha = 1.0f,
		                          texture_sampler sampler = texture_sampler::point) = 0;
		virtual void draw_texture(const texture_ptr& t, core::recti dst, core::recti src, float alpha = 1.0f,
		                          texture_sampler sampler = texture_sampler::point, float radius = 0.0) = 0;
		virtual void draw_texture(const texture_ptr& t, const core::quadd& dst, core::recti src, float alpha,
		                          texture_sampler sampler) = 0;
		virtual void draw_texture(const texture_ptr& t, const core::quadd& dst, core::recti src, float alpha,
		                          texture_sampler sampler, const texture_transform& transform) = 0;

		// Draws an equirectangular panorama as the sphere it describes, through the camera in params.
		// False when the backend has no projection of its own, which is the CPU renderer's answer: the
		// caller then rasterises it in software. Optional rather than pure so a backend gains the
		// projection by implementing it, never by being obliged to.
		virtual bool draw_panorama(const texture_ptr& t, core::recti dst, const panorama_params& params, float alpha)
		{
			return false;
		}
		virtual void draw_vertices(const vertices_ptr& v) = 0;

		core::sizei measure_text(std::string_view text, render::font_face font, render::text_style style, int width,
		                   int height = 0) override = 0;
		int text_line_height(render::font_face type) override = 0;

		virtual texture_ptr create_texture() = 0;
		virtual vertices_ptr create_vertices() = 0;
		text_layout_ptr create_text_layout(render::font_face font) override = 0;

		virtual core::recti clip_bounds() const = 0;
		virtual void clip_bounds(core::recti) = 0;
		virtual void restore_clip() = 0;
	};

	// The outcome of flushing a scene. `backend_code` is the graphics API's own result, carried so
	// the window layer can log it and decide whether the device was lost; portable code reads only
	// `failed`. A CPU backend has no device to lose and always succeeds.
	struct present_result
	{
		bool failed = false;
		int32_t backend_code = 0;
	};

	// Colour every render target is cleared to before the scene is drawn. Every backend must use
	// this, otherwise anything the scene does not explicitly paint shows through as black on one
	// backend and as this grey on another.
	constexpr float scene_clear_shade = 0.222f;

	// A draw context backed by a graphics device and a window, rather than the drawing interface
	// alone. Every backend implements this - the Direct3D one, the CPU rasterizer, and whatever a
	// port adds - so it names no graphics API and no window type.
	class draw_context_device : public draw_context
	{
	public:
		virtual void destroy() = 0;
		virtual void update_font_size(int base_font_size) = 0;

		// damage is the region the window layer knows needs repainting; empty means the whole
		// client. It is an optimisation hint only - a backend may redraw more, but the resulting
		// pixels inside damage must not depend on how much was redrawn.
		virtual void begin_draw(core::sizei client_extent, int base_font_size, core::recti damage = {}) = 0;

		virtual present_result render() = 0;

		// Discards any damage limit, so the next render covers the whole client. Needed by callers
		// that re-present an existing scene whose textures changed underneath it.
		virtual void reset_damage()
		{
		}

		virtual void resize(core::sizei size) = 0;
		virtual bool is_valid() const = 0;

		// Releases every reference this context holds on the swap-chain back buffer. Must be
		// called before the buffers are resized - a still-bound render target view makes that fail.
		// No-op on a CPU backend.
		virtual void release_back_buffer_references()
		{
		}

		// Software-rendering extensions (only implemented by the CPU backend used for layered
		// bubble popups; no-ops on a hardware backend).
		virtual void set_layer_alpha(int alpha)
		{
		}
	};

	using draw_context_device_ptr = std::shared_ptr<draw_context_device>;

	// Uploads a CPU surface for one draw, deliberately leaving texture lifetime with the call.
	void draw_surface(draw_context& dc, const core::const_surface_ptr& surface, core::recti dst, float alpha = 1.0f,
	                  texture_sampler sampler = texture_sampler::bilinear);

	class scoped_clip final : public core::no_copy
	{
		draw_context& _dc;

	public:
		scoped_clip(draw_context& dc, const core::recti bounds) : _dc(dc)
		{
			_dc.clip_bounds(bounds);
		}

		~scoped_clip() override
		{
			_dc.restore_clip();
		}
	};
	// Decodes an image the application embeds - the PNGs the chrome draws - with the application's
	// own decoder, so every backend draws the same pixels with or without WIC. The application sets
	// it at startup; until then nothing decodes, and decode_embedded answers null.
	extern std::function<core::surface_ptr(core::cspan data)> decode_embedded_image;
	core::surface_ptr decode_embedded(core::cspan data);

	// Makes a texture's scaler, the application's. The application sets it at startup; a renderer
	// without one converts with its own plain code where it has some, and refuses what it cannot.
	extern std::function<std::unique_ptr<surface_scaler>()> make_surface_scaler;

	// Whether a decoder may hand this backend a planar YUV surface. False for the CPU renderer,
	// which presents BGRA and would only convert it straight back, and false on a device whose
	// driver cannot sample NV12 or has already faulted uploading one. Asked by whoever decodes for
	// a texture; a decode read for its luma or its bytes does not consult it.
	extern bool yuv_textures_enabled;

	// The latch a driver fault clears: false once a driver has faulted uploading a planar texture.
	// The application sets it from what it kept of earlier runs before a backend starts, and a
	// backend reads it then; on_yuv_fault, when set, is called as a fault clears it, for the
	// application to keep.
	extern bool yuv_textures_allowed;
	extern void (*on_yuv_fault)();

	// Whether that surface may be 10-bit P010 as well as NV12. A device can sample NV12 and still
	// refuse P010, and a texture it refuses leaves the picture blank, so 10-bit frames are converted
	// to BGRA instead. Never true without yuv_textures_enabled.
	extern bool p010_textures_enabled;
}
