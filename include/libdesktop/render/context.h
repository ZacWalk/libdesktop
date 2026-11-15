// This file is part of libdesktop, a C++20 platform layer for desktop applications
// Copyright 2026  Zac Walker
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

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

		int operator*(const double scale) const noexcept { return core::round(n * scale * 10.0); }
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
	// The colour grade one pixel takes, on the gamma-encoded RGB it is stored as, each channel 0 to 1: white
	// balance gains that keep a neutral's luma; the tone curve on BT.709 luma - sRGB's own primaries - with
	// every channel scaled by the same ratio, so a change of tone leaves saturation alone; saturation and
	// vibrance about that luma, vibrance weighted to the least saturated colours and away from skin; and last
	// a fit into the cube that desaturates towards the luma rather than clipping a channel, which keeps hue
	// and lightness. The curve holds the tone at i / (curve_len - 1) and is read between entries.
	// color_adjust::apply and the software renderer share this; texture_transform.hlsli repeats it.
	template <typename T>
	void grade_color(T& r, T& g, T& b, const T* const curve, const int curve_len, const T saturation, const T vibrance,
	                 const T red_gain, const T green_gain, const T blue_gain) noexcept
	{
		r *= red_gain;
		g *= green_gain;
		b *= blue_gain;

		const auto y = T(0.2126) * r + T(0.7152) * g + T(0.0722) * b;
		const auto position = std::clamp(y, T(0), T(1)) * static_cast<T>(curve_len - 1);
		const auto index = std::min(static_cast<int>(position), curve_len - 2);
		const auto luma = curve[index] + (curve[index + 1] - curve[index]) * (position - static_cast<T>(index));

		if (y > T(1e-6))
		{
			const auto ratio = luma / y;
			r *= ratio;
			g *= ratio;
			b *= ratio;
		}
		else
		{
			r += luma - y;
			g += luma - y;
			b += luma - y;
		}

		auto factor = saturation;

		if (vibrance != T(0))
		{
			const auto high = std::max(r, std::max(g, b));
			const auto low = std::min(r, std::min(g, b));
			const auto chroma = high > T(1e-6) ? std::clamp((high - low) / high, T(0), T(1)) : T(0);
			// The hue on the opponent axes, red at 0, against skin's at 25 degrees, wrapped to a half turn. A grey
			// has no hue - nor chroma for the factor to scale - and atan2(0, 0) is undefined on a GPU.
			constexpr auto skin_hue = T(0.43633231);
			constexpr auto skin_width = T(0.31415927);
			constexpr auto turn = T(6.28318531);
			auto from_skin = std::atan2(T(0.8660254) * (g - b), r - T(0.5) * (g + b)) - skin_hue;
			from_skin -= turn * std::round(from_skin / turn);
			const auto skin = chroma > T(0) ? std::exp(-(from_skin * from_skin) / (skin_width * skin_width)) : T(0);
			factor *= T(1) + vibrance * (T(1) - chroma) * (T(1) - chroma) * (T(1) - T(0.7) * skin);
		}

		r = luma + (r - luma) * factor;
		g = luma + (g - luma) * factor;
		b = luma + (b - luma) * factor;

		if (luma >= T(1))
		{
			r = g = b = T(1);
			return;
		}

		if (luma <= T(0))
		{
			r = g = b = T(0);
			return;
		}

		auto fit = T(1);

		for (const auto c : {r, g, b})
		{
			if (c > T(1)) fit = std::min(fit, (T(1) - luma) / (c - luma));
			else if (c < T(0)) fit = std::min(fit, luma / (luma - c));
		}

		r = luma + (r - luma) * fit;
		g = luma + (g - luma) * fit;
		b = luma + (b - luma) * fit;
	}

	class color_adjust
	{
	public:
		static constexpr int curve_len = 0x1000;

		color_adjust() noexcept;

		// Each control from -1 to 1, 0 leaving the picture alone; values past that range are held to it.
		void color_params(double vibrance, double saturation, double darks, double midtones, double lights,
		                  double contrast, double brightness, double temperature = 0, double tint = 0);
		// Rows of the source's own format, a byte or sixteen bits a channel, written to dst with the fourth
		// channel unchanged. False for a planar source, and when the token stopped it part-way, which leaves
		// the rows it never reached holding whatever the destination allocation came with. A cancelled
		// result is not a picture.
		bool apply(const core::const_surface_ptr& src, uint8_t* dst, size_t dst_stride,
		           const core::cancel_token& token) const;
		void populate_texture_transform(struct texture_transform& transform) const;
		// The tone curve at x, 0 to 1, as apply reads it.
		double tone(double x) const noexcept;

	private:
		std::array<double, curve_len> _curve{};
		double _saturation = 1;
		double _vibrance = 0;
		double _red_gain = 1;
		double _green_gain = 1;
		double _blue_gain = 1;
	};

	struct texture_transform
	{
		static constexpr int curve_len = 0x100;

		// The tone curve at i / (curve_len - 1), read between entries as grade_color reads it.
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

		bool has_changes() const { return has_perspective || has_color_changes; }
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
		core::chroma_siting chroma_siting = core::chroma_siting::center;
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
		core::chroma_siting _chroma = core::chroma_siting::center;

		virtual ~texture() = default;

		const core::sizei dimensions() const { return _dimensions; }

		core::texture_format format() const { return _format; }

		bool has_alpha() const { return core::has_alpha(_format); }

		core::sizei source_extent() const { return _src_extent.is_empty() ? _dimensions : _src_extent; }

		virtual texture_update_result update(const video_frame_ptr& frame) = 0;
		virtual texture_update_result update(const core::const_surface_ptr& surface) = 0;
		virtual texture_update_result update(core::sizei dims, core::texture_format format,
		                                     core::orientation orientation, const uint8_t* pixels, size_t stride,
		                                     size_t buffer_size) = 0;

		// Uploads with a full mip chain, for a source that will be minified a long way - a projected
		// panorama samples thousands of pixels of sphere into one viewport. Separate from update()
		// because every other texture in the app is drawn at or near its own size and would pay for a
		// chain it never reads. A backend with no answer refuses, and the caller rasterises instead.
		virtual texture_update_result update_mipped(const core::const_surface_ptr& /*surface*/)
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

	enum class paint_kind
	{
		solid,
		linear,
		radial
	};

	// What fills a shape: one colour, or a gradient between two - along the line from start to end, or out
	// from start, its centre, to radius. A gradient holds its end colours beyond its ends, one with no length
	// is its end colour throughout, and colours mix premultiplied, so one fading to transparent keeps its hue.
	// Points are the context's pixels, as the shape's are.
	struct paint
	{
		paint_kind kind = paint_kind::solid;
		// The solid colour, or the gradient's at start.
		core::color color;
		// The gradient's colour at end, or at radius from start.
		core::color end_color;
		core::pointd start;
		core::pointd end;
		double radius = 0.0;

		paint() = default;

		paint(const core::color c) : color(c), end_color(c) {}

		static paint linear(const core::pointd from, const core::color from_color, const core::pointd to,
		                    const core::color to_color)
		{
			paint result(from_color);
			result.kind = paint_kind::linear;
			result.end_color = to_color;
			result.start = from;
			result.end = to;
			return result;
		}

		static paint radial(const core::pointd centre, const double r, const core::color centre_color,
		                    const core::color edge_color)
		{
			paint result(centre_color);
			result.kind = paint_kind::radial;
			result.end_color = edge_color;
			result.start = centre;
			result.radius = r;
			return result;
		}

		bool is_visible() const { return color.a > 0.0f || (kind != paint_kind::solid && end_color.a > 0.0f); }

		// How far along the gradient a point is: 0 at start, 1 at end or radius, and past them beyond.
		double position(const core::pointd p) const
		{
			if (kind == paint_kind::linear)
			{
				const auto along = end - start;
				const auto length_sq = along.dot(along);
				return length_sq > 1e-12 ? (p - start).dot(along) / length_sq : 1.0;
			}

			if (kind == paint_kind::radial)
			{
				return radius > 1e-6 ? (p - start).dist({}) / radius : 1.0;
			}

			return 0.0;
		}

		// The colour at a gradient position, held to its ends and mixed premultiplied.
		core::color at(const double position) const
		{
			if (kind == paint_kind::solid) return color;

			const auto t = static_cast<float>(std::clamp(position, 0.0, 1.0));
			const auto a = color.a + (end_color.a - color.a) * t;
			if (a <= 0.0f) return {color.r, color.g, color.b, 0.0f};

			const auto mix = [&](const float from, const float to)
			{
				return (from * color.a + (to * end_color.a - from * color.a) * t) / a;
			};
			return {mix(color.r, end_color.r), mix(color.g, end_color.g), mix(color.b, end_color.b), a};
		}
	};

	// How a polygon is drawn: its fill, then a border centred on its outline - half inside, half outside -
	// mitred at its corners, or bevelled where a mitre would reach past four times the border's half width.
	struct polygon_style
	{
		paint fill;
		core::color border;
		double border_width = 0.0;
		// Anti-aliased edges. Crisp ones fill exactly the pixels whose centres are inside, so polygons that
		// share an edge meet with neither gap nor overlap, as a mesh of them needs.
		bool smooth = true;

		bool has_border() const { return border.a > 0.0f && border_width > 0.0; }
	};

	// A polygon a vertices object draws: an outline, read during the update and not kept, and its style.
	struct vertex_polygon
	{
		std::span<const core::pointd> outline;
		polygon_style style;
	};

	// Geometry a scene draws by reference, so an owner can change it and re-present the scene with
	// redraw() rather than recording the scene again. Each update replaces what the last one drew.
	class vertices
	{
	public:
		virtual ~vertices() = default;
		// Bars, drawn in order, each set off the wall by its shadow. The software renderer draws the classic bars:
		// raised over a soft shadow and brightening toward their centres. Direct3D draws each as a rounded block of
		// coloured glass, a little narrower than its rect - flat-faced, bevelled and lit from the upper left - whose
		// soft shadow falls down and to the right, tinted with its colour: the backends' one deliberate difference.
		// The hardware backend draws up to 1,024.
		virtual void update_bars(std::span<const vertex_rect> bars) = 0;
		// Flat rectangles, drawn in order. The hardware backend draws up to 16,384 of them.
		virtual void update_rects(std::span<const vertex_rect> rects) = 0;
		// Polygons, drawn in order as draw_polygon draws them.
		virtual void update_polygons(std::span<const vertex_polygon> polygons) = 0;
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
	                                                     const uint16_t* const cluster_map, const uint32_t text_length,
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
		virtual core::sizei measure_text(std::string_view text, render::font_face font, render::text_style style,
		                                 int cx, int cy = 0) = 0;
		virtual int text_line_height(render::font_face font) = 0;
		// From the top of a line of this face to its baseline, in context pixels: the font's ascent with
		// any line gap above it, so text_line_height less this is the descent. Every text backend knows
		// its font and answers exactly; the default is the usual Latin proportion, for contexts that only
		// stand in for one.
		virtual int text_baseline(render::font_face font) { return core::mul_div(text_line_height(font), 4, 5); }
		virtual text_layout_ptr create_text_layout(render::font_face font) = 0;

		// The sizes below at a UI scale of 1, which a window multiplies by its display's scale and the user's
		// font size.
		static constexpr int base_icon_cxy = 18;
		static constexpr int base_padding2 = 8;
		static constexpr int base_padding1 = 4;
		static constexpr int base_handle_cxy = 12;
		static constexpr int base_scroll_width = 20;

		// Measures at a UI scale, as a window does at its display's scale and the user's font size, and as a
		// picture drawn at another size than a window's does.
		void scale_metrics(const double ui_scale)
		{
			scale_factor = ui_scale;
			icon_cxy = core::round(base_icon_cxy * ui_scale);
			padding2 = core::round(base_padding2 * ui_scale);
			padding1 = core::round(base_padding1 * ui_scale);
			handle_cxy = core::round(base_handle_cxy * ui_scale);
			scroll_width = core::round(base_scroll_width * ui_scale);
		}

		double scale_factor = 1.0;

		int icon_cxy = base_icon_cxy;
		int padding2 = base_padding2;
		int padding1 = base_padding1;
		int handle_cxy = base_handle_cxy;
		int scroll_width = base_scroll_width;
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

		// The UI's highlight shape: its colour emphasized and its soft edge a little outside bounds, the same on
		// every renderer. A shape that must be exactly its bounds and colour is a draw_polygon.
		virtual void draw_rounded_rect(core::recti bounds, core::color c, int radius) = 0;
		// A polygon - convex or concave, but not crossing itself - closed from its last point to its first, and
		// filled and bordered as style says. An outline that encloses nothing draws nothing.
		virtual void draw_polygon(std::span<const core::pointd> outline, const polygon_style& style) = 0;
		virtual void draw_rect(core::recti bounds, core::color c) = 0;
		virtual void draw_line(core::pointd a, core::pointd b, core::color c, double width = 1.0) = 0;
		virtual void draw_polyline(std::span<const core::pointd> points, core::color c, double width = 1.0) = 0;
		virtual void draw_ellipse(core::recti bounds, core::color c, double width = 1.0) = 0;
		virtual void fill_ellipse(core::recti bounds, core::color c) = 0;
		virtual void draw_text(std::string_view text, core::recti bounds, render::font_face font,
		                       render::text_style style, core::color c, core::color bg) = 0;
		virtual void draw_text(std::string_view text, const std::vector<text_highlight_t>& highlights,
		                       core::recti bounds, render::font_face font, render::text_style style, core::color clr,
		                       core::color bg) = 0;
		virtual void draw_text(const text_layout_ptr& tl, core::recti bounds, core::color clr, core::color bg) = 0;
		virtual void draw_border(core::recti inside, core::recti outside, core::color c_inside,
		                         core::color c_outside) = 0;
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
		virtual bool draw_panorama(const texture_ptr& /*t*/, core::recti /*dst*/, const panorama_params& /*params*/,
		                           float /*alpha*/)
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

		// The damage the next render covers when it re-presents the scene already recorded - because its
		// textures or vertices changed underneath it - region, or the whole client when region is empty. A
		// backend that redraws every pixel anyway has nothing to limit.
		virtual void reset_damage(core::recti /*region*/ = {}) {}

		virtual void resize(core::sizei size) = 0;
		virtual bool is_valid() const = 0;

		// Releases every reference this context holds on the swap-chain back buffer. Must be
		// called before the buffers are resized - a still-bound render target view makes that fail.
		// No-op on a CPU backend.
		virtual void release_back_buffer_references() {}

		// Software-rendering extensions (only implemented by the CPU backend used for layered
		// bubble popups; no-ops on a hardware backend).
		virtual void set_layer_alpha(int /*alpha*/) {}
	};

	using draw_context_device_ptr = std::shared_ptr<draw_context_device>;

	// Uploads a CPU surface for one draw, deliberately leaving texture lifetime with the call.
	void draw_surface(draw_context& dc, const core::const_surface_ptr& surface, core::recti dst, float alpha = 1.0f,
	                  texture_sampler sampler = texture_sampler::bilinear);

	class scoped_clip final : public core::no_copy
	{
		draw_context& _dc;

	public:
		scoped_clip(draw_context& dc, const core::recti bounds) : _dc(dc) { _dc.clip_bounds(bounds); }

		~scoped_clip() override { _dc.restore_clip(); }
	};

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
