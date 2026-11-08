// This file is part of libdesktop, a C++20 platform layer for desktop applications
// Copyright 2026  Zac Walker
// 
// This program is free software; you can redistribute it and / or modify it
// under the terms of the LGPL License either version 2.1 or later.
// License details are available at https://www.gnu.org/licenses/lgpl-2.1.html
// This program is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY

// Purpose: Linux text, the counterpart of the DirectWrite engine on Windows. Pango lays text out -
// itemisation, shaping with HarfBuzz, font fallback through fontconfig, wrapping and line metrics - and
// FreeType rasterises each glyph, which is blended into the software canvas as the Windows engine
// blends DirectWrite's. The layout rules are the Windows engine's: six text styles, text centred
// vertically in its box, and characters that do not fit trimmed rather than ellipsised. Also the icon
// surfaces and outlined styled text the Windows text backend makes. The application's icon font is
// registered with fontconfig as an application font.

#include "libdesktop/render.h"
#include "libdesktop/render/software.h"

#include <pango/pangoft2.h>
#include <pango/pangofc-font.h>
#include <pango/pangofc-fontmap.h>
#include <fontconfig/fontconfig.h>
#include <ft2build.h>
#include FT_FREETYPE_H
#include FT_SYNTHESIS_H

#include <climits>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <format>
#include <fstream>
#include <map>
#include <mutex>

namespace
{
	core::cspan icon_font_bytes;
	// An installed family the application named instead of bundling bytes; empty while it bundles them.
	std::string icon_font_installed_family;

	// Pango, fontconfig and FreeType are not thread-safe, and styled text is set on decode threads, so
	// every use of the text stack holds this.
	std::recursive_mutex& text_lock()
	{
		static std::recursive_mutex m;
		return m;
	}

	using text_guard = std::lock_guard<std::recursive_mutex>;

	int ceil_units(const int units)
	{
		return static_cast<int>(std::ceil(units / static_cast<double>(PANGO_SCALE)));
	}

	struct glyph_raster
	{
		std::vector<uint8_t> coverage;
		int width = 0;
		int height = 0;
		// From the pen position right to the raster's left edge, and from the baseline up to its top.
		int left = 0;
		int top = 0;
	};

	// How one Pango font's glyphs are rasterised: its face, at the size and with the load flags
	// fontconfig chose for it, and the rasters made so far.
	struct raster_font
	{
		FT_Face face = nullptr;
		FT_F26Dot6 size = 0;
		FT_Int32 load_flags = FT_LOAD_DEFAULT;
		FT_Render_Mode render_mode = FT_RENDER_MODE_NORMAL;
		bool embolden = false;
		bool transformed = false;
		FT_Matrix matrix{0x10000, 0, 0, 0x10000};
		std::unordered_map<uint32_t, glyph_raster> glyphs;
	};

	// The process's text stack. Every use holds text_lock().
	class text_stack final
	{
	public:
		FT_Library ft = nullptr;
		PangoFontMap* font_map = nullptr;
		PangoContext* context = nullptr;

		// Never destroyed: fonts and layouts must not depend on the order of static teardown.
		static text_stack& get()
		{
			static auto* const instance = new text_stack();
			return *instance;
		}

		// Registered on first use rather than at startup, since the application sets the bytes after
		// the text stack may already exist. Empty when it has none.
		const std::string& icon_family()
		{
			if (!icon_font_installed_family.empty()) return icon_font_installed_family;

			if (!_icon_font_tried && icon_font_bytes.size > 0)
			{
				_icon_font_tried = true;
				register_icon_font();
			}

			return _icon_family;
		}

		const glyph_raster& raster(PangoFont* font, PangoGlyph glyph);

	private:
		std::unordered_map<std::string, FT_Face> _faces;
		std::unordered_map<PangoFont*, std::unique_ptr<raster_font>> _fonts;
		std::string _icon_family;
		bool _icon_font_tried = false;
		const glyph_raster _empty;

		text_stack()
		{
			if (FT_Init_FreeType(&ft) != 0) ft = nullptr;
			font_map = pango_ft2_font_map_new();
			pango_ft2_font_map_set_resolution(PANGO_FT2_FONT_MAP(font_map), 96.0, 96.0);
			context = pango_font_map_create_context(font_map);
		}

		FT_Face face_for(const char* file, int index);
		raster_font& raster_font_for(PangoFont* font);
		void register_icon_font();
	};

	FT_Face text_stack::face_for(const char* file, const int index)
	{
		const auto key = std::format("{}#{}", file, index);
		const auto found = _faces.find(key);
		if (found != _faces.end()) return found->second;

		FT_Face face = nullptr;
		if (!ft || FT_New_Face(ft, file, index, &face) != 0) face = nullptr;
		_faces[key] = face;
		return face;
	}

	raster_font& text_stack::raster_font_for(PangoFont* font)
	{
		const auto found = _fonts.find(font);
		if (found != _fonts.end()) return *found->second;

		// Held, so a freed font's address cannot come back as another font with these rasters.
		g_object_ref(font);
		auto rf = std::make_unique<raster_font>();

		if (PANGO_IS_FC_FONT(font))
		{
			auto* const pattern = pango_fc_font_get_pattern(PANGO_FC_FONT(font));
			FcChar8* file = nullptr;

			if (pattern && FcPatternGetString(pattern, FC_FILE, 0, &file) == FcResultMatch && file)
			{
				int index = 0;
				double pixel_size = 0.0;
				FcPatternGetInteger(pattern, FC_INDEX, 0, &index);
				FcPatternGetDouble(pattern, FC_PIXEL_SIZE, 0, &pixel_size);

				rf->face = face_for(reinterpret_cast<const char*>(file), index);
				rf->size = std::lround(pixel_size * 64.0);

				FcBool hinting = FcTrue;
				FcBool antialias = FcTrue;
				FcBool autohint = FcFalse;
				FcBool embolden = FcFalse;
				int hint_style = FC_HINT_SLIGHT;
				FcPatternGetBool(pattern, FC_HINTING, 0, &hinting);
				FcPatternGetBool(pattern, FC_ANTIALIAS, 0, &antialias);
				FcPatternGetBool(pattern, FC_AUTOHINT, 0, &autohint);
				FcPatternGetBool(pattern, FC_EMBOLDEN, 0, &embolden);
				FcPatternGetInteger(pattern, FC_HINT_STYLE, 0, &hint_style);

				if (!antialias)
				{
					rf->load_flags |= FT_LOAD_TARGET_MONO;
					rf->render_mode = FT_RENDER_MODE_MONO;
				}
				else if (!hinting || hint_style == FC_HINT_NONE)
				{
					rf->load_flags |= FT_LOAD_NO_HINTING;
				}
				else if (hint_style == FC_HINT_SLIGHT)
				{
					rf->load_flags |= FT_LOAD_TARGET_LIGHT;
				}

				if (autohint) rf->load_flags |= FT_LOAD_FORCE_AUTOHINT;
				if (rf->face && FT_HAS_COLOR(rf->face)) rf->load_flags |= FT_LOAD_COLOR;
				rf->embolden = embolden != FcFalse;

				FcMatrix* m = nullptr;
				if (FcPatternGetMatrix(pattern, FC_MATRIX, 0, &m) == FcResultMatch && m)
				{
					rf->transformed = true;
					rf->matrix = {
						static_cast<FT_Fixed>(m->xx * 0x10000), static_cast<FT_Fixed>(m->xy * 0x10000),
						static_cast<FT_Fixed>(m->yx * 0x10000), static_cast<FT_Fixed>(m->yy * 0x10000)
					};
				}
			}
		}

		return *_fonts.emplace(font, std::move(rf)).first->second;
	}

	// A bitmap's coverage resampled by a factor, for the fixed strikes of a colour emoji font.
	std::vector<uint8_t> scale_coverage(const std::vector<uint8_t>& src, const int w, const int h, const int dw,
	                                    const int dh)
	{
		std::vector<uint8_t> result(static_cast<size_t>(dw) * dh);

		for (auto y = 0; y < dh; ++y)
		{
			const auto y0 = y * h / dh;
			const auto y1 = std::max(y0 + 1, (y + 1) * h / dh);

			for (auto x = 0; x < dw; ++x)
			{
				const auto x0 = x * w / dw;
				const auto x1 = std::max(x0 + 1, (x + 1) * w / dw);
				auto sum = 0;
				auto n = 0;

				for (auto sy = y0; sy < y1 && sy < h; ++sy)
					for (auto sx = x0; sx < x1 && sx < w; ++sx, ++n)
						sum += src[static_cast<size_t>(sy) * w + sx];

				result[static_cast<size_t>(y) * dw + x] = static_cast<uint8_t>(n ? sum / n : 0);
			}
		}

		return result;
	}

	const glyph_raster& text_stack::raster(PangoFont* font, const PangoGlyph glyph)
	{
		auto& rf = raster_font_for(font);
		if (!rf.face) return _empty;

		const auto found = rf.glyphs.find(glyph);
		if (found != rf.glyphs.end()) return found->second;

		// Bounded per font: a document in a script with thousands of glyphs would otherwise keep them all.
		if (rf.glyphs.size() > 4096) rf.glyphs.clear();
		auto& result = rf.glyphs[glyph];

		const auto face = rf.face;
		auto scale = 1.0;

		if (FT_Set_Char_Size(face, 0, rf.size, 72, 72) != 0)
		{
			// A bitmap font - colour emoji - has fixed strikes: take the nearest and scale it.
			if (face->num_fixed_sizes <= 0) return result;

			auto best = 0;
			for (auto i = 1; i < face->num_fixed_sizes; ++i)
			{
				if (std::abs(face->available_sizes[i].y_ppem - rf.size) <
					std::abs(face->available_sizes[best].y_ppem - rf.size))
					best = i;
			}

			if (FT_Select_Size(face, best) != 0) return result;
			scale = static_cast<double>(rf.size) / static_cast<double>(face->available_sizes[best].y_ppem);
		}

		FT_Set_Transform(face, rf.transformed ? &rf.matrix : nullptr, nullptr);
		if (FT_Load_Glyph(face, glyph, rf.load_flags) != 0) return result;

		auto* const slot = face->glyph;
		if (rf.embolden && slot->format == FT_GLYPH_FORMAT_OUTLINE) FT_GlyphSlot_Embolden(slot);
		if (slot->format != FT_GLYPH_FORMAT_BITMAP && FT_Render_Glyph(slot, rf.render_mode) != 0) return result;

		const auto& bm = slot->bitmap;
		const auto w = static_cast<int>(bm.width);
		const auto h = static_cast<int>(bm.rows);
		if (w <= 0 || h <= 0 || bm.pitch <= 0) return result;

		std::vector<uint8_t> coverage(static_cast<size_t>(w) * h);

		for (auto y = 0; y < h; ++y)
		{
			const auto* const row = bm.buffer + static_cast<ptrdiff_t>(y) * bm.pitch;
			auto* const out = coverage.data() + static_cast<size_t>(y) * w;

			for (auto x = 0; x < w; ++x)
			{
				switch (bm.pixel_mode)
				{
				case FT_PIXEL_MODE_MONO: out[x] = (row[x >> 3] & (0x80 >> (x & 7))) ? 255 : 0;
					break;
				case FT_PIXEL_MODE_BGRA: out[x] = row[x * 4 + 3];
					break;
				case FT_PIXEL_MODE_GRAY: out[x] = row[x];
					break;
				default: out[x] = 0;
					break;
				}
			}
		}

		if (scale != 1.0)
		{
			const auto dw = std::max(1, static_cast<int>(std::lround(w * scale)));
			const auto dh = std::max(1, static_cast<int>(std::lround(h * scale)));
			result = {
				scale_coverage(coverage, w, h, dw, dh), dw, dh,
				static_cast<int>(std::lround(slot->bitmap_left * scale)),
				static_cast<int>(std::lround(slot->bitmap_top * scale))
			};
		}
		else
		{
			result = {std::move(coverage), w, h, slot->bitmap_left, slot->bitmap_top};
		}

		return result;
	}

	// The font comes from memory, and fontconfig adds application fonts from files, so the bytes are
	// written once to the user's runtime directory under a name derived from their content.
	void text_stack::register_icon_font()
	{
		const std::string_view bytes(reinterpret_cast<const char*>(icon_font_bytes.data), icon_font_bytes.size);
		const auto name = std::format("libdesktop-icons-{:016x}.ttf", std::hash<std::string_view>{}(bytes));
		const auto path = std::filesystem::path(g_get_user_runtime_dir()) / name;

		std::error_code ec;
		if (!std::filesystem::exists(path, ec) || std::filesystem::file_size(path, ec) != bytes.size())
		{
			std::ofstream out(path, std::ios::binary | std::ios::trunc);
			out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));

			if (!out)
			{
				core::log(__FUNCTION__, "the icon font could not be written - icons will not be drawn");
				return;
			}
		}

		const auto file = path.string();
		const auto* const fc_file = reinterpret_cast<const FcChar8*>(file.c_str());

		if (!FcConfigAppFontAddFile(FcConfigGetCurrent(), fc_file))
		{
			core::log(__FUNCTION__, "fontconfig refused the icon font - icons will not be drawn");
			return;
		}

		auto count = 0;
		if (auto* const pattern = FcFreeTypeQuery(fc_file, 0, nullptr, &count))
		{
			FcChar8* family = nullptr;
			if (FcPatternGetString(pattern, FC_FAMILY, 0, &family) == FcResultMatch && family)
			{
				_icon_family = reinterpret_cast<const char*>(family);
			}

			FcPatternDestroy(pattern);
		}

		pango_fc_font_map_config_changed(PANGO_FC_FONT_MAP(font_map));
	}

	bool wraps(const render::text_style style)
	{
		return style == render::text_style::multiline || style == render::text_style::multiline_center;
	}

	// Where a line this wide sits in a box this wide, in Pango units, as DirectWrite aligns it. A line
	// wider than its box starts at the leading edge and is trimmed at the trailing one.
	int align_offset(const render::text_style style, const int box, const int line)
	{
		if (line >= box) return 0;

		switch (style)
		{
		case render::text_style::single_line_center:
		case render::text_style::multiline_center:
			return (box - line) / 2;
		case render::text_style::single_line_far:
			return box - line;
		default:
			return 0;
		}
	}

	PangoFontDescription* make_description(const std::string& family, const int px, const int weight = 400,
	                                      const bool italic = false)
	{
		auto* const d = pango_font_description_new();
		pango_font_description_set_family(d, family.c_str());
		pango_font_description_set_absolute_size(d, std::max(1, px) * PANGO_SCALE);
		pango_font_description_set_weight(d, static_cast<PangoWeight>(std::clamp(weight, 100, 1000)));
		pango_font_description_set_style(d, italic ? PANGO_STYLE_ITALIC : PANGO_STYLE_NORMAL);
		return d;
	}

	PangoLayout* new_layout(const PangoFontDescription* desc)
	{
		auto* const layout = pango_layout_new(text_stack::get().context);
		pango_layout_set_font_description(layout, desc);
		// Baselines a font's own line height apart, gap included, as DirectWrite spaces them.
		pango_layout_set_line_spacing(layout, 1.0f);
		pango_layout_set_wrap(layout, PANGO_WRAP_WORD);
		return layout;
	}

	void set_layout_text(PangoLayout* layout, const std::string_view text)
	{
		if (g_utf8_validate(text.data(), static_cast<gssize>(text.size()), nullptr))
		{
			pango_layout_set_text(layout, text.data(), static_cast<int>(text.size()));
		}
		else
		{
			auto* const valid = g_utf8_make_valid(text.data(), static_cast<gssize>(text.size()));
			pango_layout_set_text(layout, valid, -1);
			g_free(valid);
		}
	}

	void set_font_decorations(PangoLayout* layout, const bool underline, const bool strikeout)
	{
		if (!underline && !strikeout)
		{
			pango_layout_set_attributes(layout, nullptr);
			return;
		}

		auto* const attrs = pango_attr_list_new();
		if (underline)
		{
			auto* const attribute = pango_attr_underline_new(PANGO_UNDERLINE_SINGLE);
			attribute->start_index = 0;
			attribute->end_index = G_MAXUINT;
			pango_attr_list_insert(attrs, attribute);
		}
		if (strikeout)
		{
			auto* const attribute = pango_attr_strikethrough_new(TRUE);
			attribute->start_index = 0;
			attribute->end_index = G_MAXUINT;
			pango_attr_list_insert(attrs, attribute);
		}
		pango_layout_set_attributes(layout, attrs);
		pango_attr_list_unref(attrs);
	}

	void set_box_width(PangoLayout* layout, const render::text_style style, const int cx)
	{
		pango_layout_set_width(layout, wraps(style) && cx > 0 ? cx * PANGO_SCALE : -1);
	}

	core::sizei layout_extent(PangoLayout* layout)
	{
		PangoRectangle logical{};
		pango_layout_get_extents(layout, nullptr, &logical);
		return {ceil_units(logical.width), ceil_units(logical.height)};
	}

	struct line_metrics
	{
		int height = 0;
		int baseline = 0;
	};

	line_metrics line_metrics_of(const PangoFontDescription* desc)
	{
		auto& stack = text_stack::get();
		auto* const font = pango_font_map_load_font(stack.font_map, stack.context, desc);
		if (!font) return {};

		auto* const metrics = pango_font_get_metrics(font, nullptr);
		const auto descent = pango_font_metrics_get_descent(metrics);
		auto h = pango_font_metrics_get_height(metrics);
		if (h <= 0) h = pango_font_metrics_get_ascent(metrics) + descent;
		pango_font_metrics_unref(metrics);
		g_object_unref(font);
		// Any line gap sits above the baseline, where the Windows engine puts it.
		return {PANGO_PIXELS(h), PANGO_PIXELS(h - descent)};
	}

	// The highlight colour of a glyph whose cluster starts at this byte, as the Windows engine colours a
	// glyph by its cluster. Pango indexes text in UTF-8, as the highlights are given.
	core::color glyph_color(const core::color base, const int cluster_byte,
	                      const std::vector<render::text_highlight_t>& highlights)
	{
		if (highlights.empty()) return base;
		return render::text_cluster_color(base, {static_cast<uint32_t>(cluster_byte), 1}, highlights);
	}

	void draw_layout(PangoLayout* layout, const render::text_style style, render::draw_context& dc,
	                 render::software_canvas& canvas, const core::recti bounds, const core::color clr, const core::color bg,
	                 const std::vector<render::text_highlight_t>& highlights)
	{
		auto& stack = text_stack::get();
		set_box_width(layout, style, bounds.width());

		PangoRectangle logical{};
		pango_layout_get_extents(layout, nullptr, &logical);

		const auto box_w = bounds.width() * PANGO_SCALE;
		const auto box_h = bounds.height() * PANGO_SCALE;
		// Centred in its box as DirectWrite's paragraph alignment centres it, unless it overflows: then it
		// starts at the top, and the lines below the box are trimmed.
		const auto overflows = logical.height > box_h;
		const auto top = overflows ? 0 : (box_h - logical.height) / 2;

		if (bg.a > 0.0f)
		{
			auto left = INT_MAX;
			auto right = INT_MIN;
			auto* const lines = pango_layout_get_iter(layout);

			do
			{
				PangoRectangle line{};
				pango_layout_iter_get_line_extents(lines, nullptr, &line);
				const auto x = align_offset(style, box_w, line.width);
				left = std::min(left, x);
				right = std::max(right, x + line.width);
			}
			while (pango_layout_iter_next_line(lines));

			pango_layout_iter_free(lines);

			if (left < right)
			{
				const core::recti box(bounds.left + PANGO_PIXELS_FLOOR(left), bounds.top + PANGO_PIXELS_FLOOR(top),
				                bounds.left + PANGO_PIXELS_CEIL(right),
				                bounds.top + PANGO_PIXELS_CEIL(top + logical.height));
				dc.draw_rounded_rect(box.inflate(2), bg, dc.padding1);
			}
		}

		auto* const iter = pango_layout_get_iter(layout);
		const PangoLayoutLine* current_line = nullptr;
		auto first_line = true;
		auto trimmed = false;

		do
		{
			auto* const run = pango_layout_iter_get_run_readonly(iter);
			if (!run) continue;

			PangoRectangle line{};
			pango_layout_iter_get_line_extents(iter, nullptr, &line);

			const auto* const this_line = pango_layout_iter_get_line_readonly(iter);
			if (this_line != current_line)
			{
				current_line = this_line;
				if (!first_line && overflows && line.y + line.height > box_h) trimmed = true;
				first_line = false;
			}

			if (trimmed) break;

			PangoRectangle run_rect{};
			pango_layout_iter_get_run_extents(iter, nullptr, &run_rect);

			const auto baseline = pango_layout_iter_get_baseline(iter);
			const auto line_left = align_offset(style, box_w, line.width);
			const auto* const glyphs = run->glyphs;
			auto* const font = run->item->analysis.font;
			auto pen = run_rect.x - line.x;

			for (auto i = 0; i < glyphs->num_glyphs; ++i)
			{
				const auto& info = glyphs->glyphs[i];
				const auto x = pen + info.geometry.x_offset;
				pen += info.geometry.width;

				if (info.glyph == PANGO_GLYPH_EMPTY || (info.glyph & PANGO_GLYPH_UNKNOWN_FLAG)) continue;

				// Trimmed by character where it would cross the box's trailing edge, as DirectWrite trims.
				if (line.width > box_w && pen > box_w) continue;

				const auto& g = stack.raster(font, info.glyph);
				if (g.coverage.empty()) continue;

				const auto c = glyph_color(clr, run->item->offset + glyphs->log_clusters[i], highlights);
				const auto px = bounds.left + PANGO_PIXELS(line_left + x) + g.left;
				const auto py = bounds.top + PANGO_PIXELS(top + baseline + info.geometry.y_offset) - g.top;
				canvas.blend_glyph(px, py, g.coverage.data(), {g.width, g.height}, c);
			}
		}
		while (pango_layout_iter_next_run(iter));

		pango_layout_iter_free(iter);
	}

	// Each cluster's advance, summed in logical order as DirectWrite's cluster metrics are: positions
	// along the text, not along the screen, and a position inside a cluster at the cluster's end.
	std::vector<int> logical_offsets(PangoLayout* layout, const std::vector<size_t>& offsets)
	{
		std::map<int, int> clusters;
		auto* const iter = pango_layout_get_iter(layout);

		do
		{
			const auto* const run = pango_layout_iter_get_run_readonly(iter);
			if (!run) continue;

			for (auto i = 0; i < run->glyphs->num_glyphs; ++i)
			{
				clusters[run->item->offset + run->glyphs->log_clusters[i]] += run->glyphs->glyphs[i].geometry.width;
			}
		}
		while (pango_layout_iter_next_run(iter));

		pango_layout_iter_free(iter);

		const auto text_length = std::strlen(pango_layout_get_text(layout));
		std::vector<std::pair<size_t, int>> advances{{0u, 0}};
		auto sum = 0;

		for (auto it = clusters.begin(); it != clusters.end(); ++it)
		{
			sum += it->second;
			const auto next = std::next(it);
			advances.emplace_back(next == clusters.end() ? text_length : static_cast<size_t>(next->first), sum);
		}

		std::vector<int> result;
		result.reserve(offsets.size());

		for (const auto offset : offsets)
		{
			const auto found = std::ranges::lower_bound(advances, offset, {}, [](const auto& a) { return a.first; });
			const auto x = found == advances.end() ? advances.back().second : found->second;
			result.emplace_back(PANGO_PIXELS(x));
		}

		return result;
	}

	class pango_text_layout final : public render::text_layout
	{
	public:
		PangoLayout* _layout = nullptr;
		render::text_style _style = render::text_style::none;
		bool _underline = false;
		bool _strikeout = false;

		explicit pango_text_layout(const PangoFontDescription* desc, const bool underline = false,
		                           const bool strikeout = false) : _underline(underline), _strikeout(strikeout)
		{
			const text_guard guard(text_lock());
			_layout = new_layout(desc);
		}

		~pango_text_layout() override
		{
			const text_guard guard(text_lock());
			g_object_unref(_layout);
		}

		void update(const std::string_view text, const render::text_style style) override
		{
			const text_guard guard(text_lock());
			_style = style;
			set_layout_text(_layout, text);
			set_font_decorations(_layout, _underline, _strikeout);
		}

		core::sizei measure_text(const int cx, const int cy) override
		{
			const text_guard guard(text_lock());
			if (pango_layout_get_character_count(_layout) == 0) return {};
			set_box_width(_layout, _style, cx);
			return layout_extent(_layout);
		}

		std::vector<int> offset_xs(const std::vector<size_t>& utf8_offsets, const int cx, const int cy) override
		{
			const text_guard guard(text_lock());
			set_box_width(_layout, _style, cx);
			return logical_offsets(_layout, utf8_offsets);
		}
	};

	class pango_text_engine final : public render::software_text_engine
	{
		static constexpr size_t face_count = static_cast<size_t>(render::font_face::small_icons) + 1;
		static constexpr size_t max_cached_layouts = 768;

		struct face_font
		{
			PangoFontDescription* desc = nullptr;
			int line_height = 0;
			int baseline = 0;

			void measure()
			{
				const auto m = line_metrics_of(desc);
				line_height = m.height;
				baseline = m.baseline;
			}
		};

		struct cached_layout
		{
			PangoLayout* layout = nullptr;
			uint64_t used = 0;
		};

		int _base_font_size = render::default_font_size;
		std::array<face_font, face_count> _faces{};
		std::unordered_map<int, face_font> _custom_faces;
		bool _faces_built = false;
		std::unordered_map<std::string, cached_layout> _layouts;
		uint64_t _clock = 0;

		void free_faces()
		{
			for (auto& f : _faces)
			{
				if (f.desc) pango_font_description_free(f.desc);
				f = {};
			}

			for (auto& entry : _custom_faces)
			{
				if (entry.second.desc) pango_font_description_free(entry.second.desc);
			}
			_custom_faces.clear();

			for (auto& [key, entry] : _layouts) g_object_unref(entry.layout);
			_layouts.clear();
			_faces_built = false;
		}

		void build_faces()
		{
			free_faces();

			const auto& icons = text_stack::get().icon_family();
			const auto icon_family = icons.empty() ? std::string("Sans") : icons;
			// The Windows faces by name: fontconfig maps Calibri to a metric-compatible face where one is
			// installed, and to the system's sans-serif otherwise.
			const std::string sans = "Calibri, Carlito, Sans";
			const auto base = _base_font_size;

			const auto set = [&](const render::font_face face, const std::string& family, const int px)
			{
				auto& f = _faces[static_cast<size_t>(face)];
				f.desc = make_description(family, px);
				f.measure();
			};

			set(render::font_face::dialog, sans, base);
			set(render::font_face::code, "Consolas, Monospace", base * 4 / 5);
			set(render::font_face::title, sans, base * 3 / 2);
			set(render::font_face::mega, sans, base * 9 / 4);
			set(render::font_face::icons, icon_family, core::mul_div(base, 11, 10));
			set(render::font_face::small_icons, icon_family, core::mul_div(base, 110, 160));
			_faces_built = true;
		}

		const face_font& face(const render::font_face f)
		{
			if (!_faces_built) build_faces();

			render::font_desc custom;
			if (render::detail::custom_font_description(f, custom))
			{
				const auto key = static_cast<int>(f);
				auto [found, inserted] = _custom_faces.try_emplace(key);
				if (inserted)
				{
					auto family = custom.family.empty() ? std::string("Sans") : custom.family;
					if (custom.family == "Segoe Fluent Icons") family += ", Segoe MDL2 Assets, Sans";
					else if (family.find(',') == std::string::npos) family += ", Sans";
					found->second.desc = make_description(family, custom.size, custom.weight, custom.italic);
					found->second.measure();
					if (custom.cell_size && found->second.line_height > 0 && found->second.line_height != custom.size)
					{
						const auto requested = std::max(1, custom.size);
						const auto adjusted = std::max(1, core::round(
							static_cast<double>(requested) * requested / found->second.line_height));
						pango_font_description_free(found->second.desc);
						found->second.desc = make_description(family, adjusted, custom.weight, custom.italic);
						found->second.measure();
					}
				}
				return found->second;
			}

			const auto i = static_cast<size_t>(f);
			return _faces[i < face_count && _faces[i].desc ? i : static_cast<size_t>(render::font_face::dialog)];
		}

		// One layout per (face, text, style), as the Windows engine caches them: building one runs
		// itemisation, fallback and shaping, which the item grid would otherwise pay on every frame.
		PangoLayout* layout_for(const render::font_face f, const std::string_view text,
		                        const render::text_style style)
		{
			auto key = std::format("{}:{}:", static_cast<int>(f), static_cast<int>(style));
			key.append(text);

			const auto found = _layouts.find(key);
			if (found != _layouts.end())
			{
				found->second.used = ++_clock;
				return found->second.layout;
			}

			if (_layouts.size() >= max_cached_layouts) trim_layouts();

			auto* const layout = new_layout(face(f).desc);
			set_layout_text(layout, text);
			render::font_desc custom;
			if (render::detail::custom_font_description(f, custom))
				set_font_decorations(layout, custom.underline, custom.strikeout);
			_layouts[std::move(key)] = {layout, ++_clock};
			return layout;
		}

		// The least recently used half, so a working set over the cap costs one rebuild per round.
		void trim_layouts()
		{
			std::vector<uint64_t> stamps;
			stamps.reserve(_layouts.size());
			for (const auto& [key, entry] : _layouts) stamps.emplace_back(entry.used);

			const auto middle = stamps.begin() + static_cast<ptrdiff_t>(stamps.size() / 2);
			std::ranges::nth_element(stamps, middle);
			const auto cutoff = *middle;

			std::erase_if(_layouts, [cutoff](const auto& kv)
			{
				if (kv.second.used > cutoff) return false;
				g_object_unref(kv.second.layout);
				return true;
			});
		}

	public:
		explicit pango_text_engine(const int base_font_size) : _base_font_size(base_font_size)
		{
		}

		~pango_text_engine() override
		{
			const text_guard guard(text_lock());
			free_faces();
		}

		void base_font_size(const int size) override
		{
			const text_guard guard(text_lock());
			if (_base_font_size == size) return;
			_base_font_size = size;
			free_faces();
		}

		core::sizei measure_text(const std::string_view text, const render::font_face f, const render::text_style style,
		                   const int cx, const int cy) override
		{
			if (text.empty()) return {};
			const text_guard guard(text_lock());
			auto* const layout = layout_for(f, text, style);
			set_box_width(layout, style, cx);
			return layout_extent(layout);
		}

		int text_line_height(const render::font_face f) override
		{
			const text_guard guard(text_lock());
			return face(f).line_height;
		}

		int text_baseline(const render::font_face f) override
		{
			const text_guard guard(text_lock());
			return face(f).baseline;
		}

		render::text_layout_ptr create_text_layout(const render::font_face f) override
		{
			const text_guard guard(text_lock());
			render::font_desc custom;
			const auto has_custom = render::detail::custom_font_description(f, custom);
			return std::make_shared<pango_text_layout>(face(f).desc, has_custom && custom.underline,
			                                           has_custom && custom.strikeout);
		}

		void draw_text(render::draw_context& dc, render::software_canvas& canvas, const std::string_view text,
		               const core::recti bounds, const render::font_face f, const render::text_style style,
		               const core::color c, const core::color bg) override
		{
			if (text.empty()) return;
			const text_guard guard(text_lock());
			draw_layout(layout_for(f, text, style), style, dc, canvas, bounds, c, bg, {});
		}

		void draw_text(render::draw_context& dc, render::software_canvas& canvas, const std::string_view text,
		               const std::vector<render::text_highlight_t>& highlights, const core::recti bounds,
		               const render::font_face f, const render::text_style style, const core::color c,
		               const core::color bg) override
		{
			if (text.empty()) return;
			const text_guard guard(text_lock());
			draw_layout(layout_for(f, text, style), style, dc, canvas, bounds, c, bg, highlights);
		}

		void draw_text(render::draw_context& dc, render::software_canvas& canvas, const render::text_layout_ptr& layout,
		               const core::recti bounds, const core::color c, const core::color bg) override
		{
			const auto t = std::dynamic_pointer_cast<pango_text_layout>(layout);
			if (!t) return;
			const text_guard guard(text_lock());
			draw_layout(t->_layout, t->_style, dc, canvas, bounds, c, bg, {});
		}
	};

	// The largest coverage within radius pixels, as a square: two passes, and at a few pixels as round as
	// an outline needs to look. The same outline the Windows backend draws.
	std::vector<uint8_t> dilate(const std::vector<uint8_t>& coverage, const int width, const int height, const int radius)
	{
		std::vector<uint8_t> across(coverage.size());
		std::vector<uint8_t> result(coverage.size());

		for (auto y = 0; y < height; ++y)
		{
			const auto* row = coverage.data() + static_cast<size_t>(y) * width;
			auto* out = across.data() + static_cast<size_t>(y) * width;

			for (auto x = 0; x < width; ++x)
			{
				uint8_t m = 0;
				for (auto dx = std::max(0, x - radius); dx <= std::min(width - 1, x + radius); ++dx)
					m = std::max(m, row[dx]);
				out[x] = m;
			}
		}

		for (auto y = 0; y < height; ++y)
		{
			auto* out = result.data() + static_cast<size_t>(y) * width;

			for (auto x = 0; x < width; ++x)
			{
				uint8_t m = 0;
				for (auto dy = std::max(0, y - radius); dy <= std::min(height - 1, y + radius); ++dy)
					m = std::max(m, across[static_cast<size_t>(dy) * width + x]);
				out[x] = m;
			}
		}

		return result;
	}

	// Calls f(x, y, raster, cluster_byte) for every glyph of a laid-out block, x and y being where the
	// raster's top-left corner goes, in pixels from the block's origin.
	template <typename F>
	void for_each_glyph(PangoLayout* layout, const int origin_x, const int origin_y, F&& f)
	{
		auto& stack = text_stack::get();
		auto* const iter = pango_layout_get_iter(layout);

		do
		{
			auto* const run = pango_layout_iter_get_run_readonly(iter);
			if (!run) continue;

			PangoRectangle run_rect{};
			pango_layout_iter_get_run_extents(iter, nullptr, &run_rect);
			const auto baseline = pango_layout_iter_get_baseline(iter);
			auto pen = run_rect.x;

			for (auto i = 0; i < run->glyphs->num_glyphs; ++i)
			{
				const auto& info = run->glyphs->glyphs[i];
				const auto x = pen + info.geometry.x_offset;
				pen += info.geometry.width;

				if (info.glyph == PANGO_GLYPH_EMPTY || (info.glyph & PANGO_GLYPH_UNKNOWN_FLAG)) continue;

				const auto& g = stack.raster(run->item->analysis.font, info.glyph);
				if (g.coverage.empty()) continue;

				f(origin_x + PANGO_PIXELS(x) + g.left, origin_y + PANGO_PIXELS(baseline + info.geometry.y_offset) - g.top,
				  g, run->item->offset + run->glyphs->log_clusters[i]);
			}
		}
		while (pango_layout_iter_next_run(iter));

		pango_layout_iter_free(iter);
	}
}

void render::set_icon_font(const core::cspan font_bytes)
{
	const text_guard guard(text_lock());
	icon_font_bytes = font_bytes;
	icon_font_installed_family.clear();
}

void render::set_icon_font_family(const std::string_view family)
{
	const text_guard guard(text_lock());
	icon_font_bytes = {};
	icon_font_installed_family = family;
	// fontconfig picks the first installed family of a list; Windows 10's name for the same glyphs follows.
	if (family == "Segoe Fluent Icons") icon_font_installed_family += ", Segoe MDL2 Assets";
}

render::software_text_engine_ptr render::create_text_engine(const int base_font_size)
{
	return std::make_shared<pango_text_engine>(base_font_size);
}

core::const_surface_ptr render::create_icon_surface(const char32_t ch)
{
	const text_guard guard(text_lock());

	static std::unordered_map<char32_t, core::const_surface_ptr> cache;
	const auto found = cache.find(ch);
	if (found != cache.end()) return found->second;

	auto surface = std::make_shared<core::surface>();
	constexpr auto cxy = 160;

	if (!surface->alloc(cxy, cxy, core::texture_format::ARGB, core::orientation::top_left)) return surface;
	surface->make_blank();

	const auto& family = text_stack::get().icon_family();
	if (family.empty()) return surface;

	std::string utf8;
	core::char32_to_utf8(std::back_inserter(utf8), static_cast<uint32_t>(ch));

	auto* const desc = make_description(family, core::mul_div(148, 11, 10));
	auto* const layout = new_layout(desc);
	set_layout_text(layout, utf8);

	// Centred in the square, as the Windows backend centres its layout box both ways.
	PangoRectangle logical{};
	pango_layout_get_extents(layout, nullptr, &logical);
	const auto x0 = PANGO_PIXELS((cxy * PANGO_SCALE - logical.width) / 2 - logical.x);
	const auto y0 = PANGO_PIXELS((cxy * PANGO_SCALE - logical.height) / 2 - logical.y);

	for_each_glyph(layout, x0, y0, [&](const int gx, const int gy, const glyph_raster& g, int)
	{
		for (auto y = 0; y < g.height; ++y)
		{
			const auto dy = gy + y;
			if (dy < 0 || dy >= cxy) continue;
			auto* const dst = surface->pixels_line(dy);

			for (auto x = 0; x < g.width; ++x)
			{
				const auto dx = gx + x;
				if (dx < 0 || dx >= cxy) continue;

				const auto a = g.coverage[static_cast<size_t>(y) * g.width + x];
				if (a == 0) continue;

				auto* const p = dst + static_cast<ptrdiff_t>(dx) * 4;
				p[0] = 255;
				p[1] = 255;
				p[2] = 255;
				p[3] = std::max(a, p[3]);
			}
		}
	});

	g_object_unref(layout);
	pango_font_description_free(desc);
	cache[ch] = surface;
	return surface;
}

core::surface_ptr render::render_styled_text(const render::styled_text& spec)
{
	if (spec.font_px < 1.0f) return {};

	struct run_range
	{
		int start = 0;
		int end = 0;
		const render::text_run* run = nullptr;
	};

	std::string text;
	std::vector<run_range> ranges;

	for (const auto& r : spec.runs)
	{
		const auto start = static_cast<int>(text.size());
		text += r.text;
		ranges.push_back({start, static_cast<int>(text.size()), &r});
	}

	if (text.find_first_not_of(" \t\r\n") == std::string::npos) return {};
	if (!g_utf8_validate(text.data(), static_cast<gssize>(text.size()), nullptr)) return {};

	const text_guard guard(text_lock());

	const auto family = (spec.font_family.empty() ? std::string("Arial") : spec.font_family) + ", Sans";
	auto* const desc = pango_font_description_new();
	pango_font_description_set_family(desc, family.c_str());
	pango_font_description_set_absolute_size(desc, spec.font_px * PANGO_SCALE);

	auto* const layout = new_layout(desc);
	pango_layout_set_line_spacing(layout, 0.0f);
	pango_layout_set_text(layout, text.data(), static_cast<int>(text.size()));
	if (spec.wrap_width > 0) pango_layout_set_width(layout, spec.wrap_width * PANGO_SCALE);

	auto* const attrs = pango_attr_list_new();

	for (const auto& r : ranges)
	{
		if (r.start == r.end) continue;

		if (r.run->bold)
		{
			auto* const a = pango_attr_weight_new(PANGO_WEIGHT_BOLD);
			a->start_index = static_cast<guint>(r.start);
			a->end_index = static_cast<guint>(r.end);
			pango_attr_list_insert(attrs, a);
		}

		if (r.run->italic)
		{
			auto* const a = pango_attr_style_new(PANGO_STYLE_ITALIC);
			a->start_index = static_cast<guint>(r.start);
			a->end_index = static_cast<guint>(r.end);
			pango_attr_list_insert(attrs, a);
		}
	}

	pango_layout_set_attributes(layout, attrs);
	pango_attr_list_unref(attrs);

	// The box hugs the text, so its lines align against one another rather than an arbitrary width.
	PangoRectangle ink{};
	PangoRectangle logical{};
	pango_layout_get_extents(layout, nullptr, &logical);
	const auto box_width = std::max(1, ceil_units(logical.width));
	pango_layout_set_width(layout, box_width * PANGO_SCALE);
	pango_layout_set_alignment(layout, spec.alignment == 0
		                                   ? PANGO_ALIGN_LEFT
		                                   : spec.alignment == 2
		                                   ? PANGO_ALIGN_RIGHT
		                                   : PANGO_ALIGN_CENTER);
	pango_layout_get_extents(layout, &ink, &logical);
	const auto box_height = std::max(1, ceil_units(logical.height));

	const auto margin = static_cast<int>(std::ceil(std::max(0.0f, spec.outline_px))) + 2;
	const auto beyond = [](const int units) { return std::max(0, ceil_units(units)); };
	const auto left = margin + beyond(logical.x - ink.x);
	const auto top = margin + beyond(logical.y - ink.y);
	const auto width = left + box_width + margin + beyond(ink.x + ink.width - (logical.x + logical.width));
	const auto height = top + box_height + margin + beyond(ink.y + ink.height - (logical.y + logical.height));

	core::surface_ptr surface;

	// A block no picture could show is refused rather than allocated.
	if (width > 0 && height > 0 && width <= 8192 && height <= 8192)
	{
		std::vector<uint8_t> coverage(static_cast<size_t>(width) * height);
		std::vector<uint32_t> colors(coverage.size(), 0xFFFFFFFFu);

		const auto color_at = [&](const int byte)
		{
			for (const auto& r : ranges)
			{
				if (byte >= r.start && byte < r.end) return r.run->color;
			}

			return 0xFFFFFFFFu;
		};

		for_each_glyph(layout, left - PANGO_PIXELS(logical.x), top - PANGO_PIXELS(logical.y),
		               [&](const int gx, const int gy, const glyph_raster& g, const int byte)
		               {
			               const auto color = color_at(byte);

			               for (auto y = 0; y < g.height; ++y)
			               {
				               const auto dy = gy + y;
				               if (dy < 0 || dy >= height) continue;

				               for (auto x = 0; x < g.width; ++x)
				               {
					               const auto dx = gx + x;
					               if (dx < 0 || dx >= width) continue;

					               const auto a = g.coverage[static_cast<size_t>(y) * g.width + x];
					               const auto i = static_cast<size_t>(dy) * width + dx;

					               if (a > 0 && a >= coverage[i])
					               {
						               coverage[i] = a;
						               colors[i] = color;
					               }
				               }
			               }
		               });

		const auto radius = static_cast<int>(std::lround(std::max(0.0f, spec.outline_px)));
		const auto outline = radius > 0 ? dilate(coverage, width, height, radius) : std::vector<uint8_t>(coverage.size());

		surface = std::make_shared<core::surface>();

		if (surface->alloc(width, height, core::texture_format::ARGB, core::orientation::top_left))
		{
			surface->make_blank();

			const auto channel = [](const uint32_t c, const int shift) { return static_cast<int64_t>((c >> shift) & 0xFFu); };
			const auto outline_alpha = channel(spec.outline_color, 24);
			constexpr int64_t full = 255 * 255;

			for (auto y = 0; y < height; ++y)
			{
				auto* const dst = surface->pixels_line(y);

				for (auto x = 0; x < width; ++x)
				{
					const auto i = static_cast<size_t>(y) * width + x;
					const auto c = colors[i];

					// Straight alpha, text over its outline: a = t + o(1 - t), each colour weighted by its
					// share. The coverages are in 255 * 255 units, so the sums are kept in 64 bits.
					const auto t = coverage[i] * channel(c, 24);
					const auto o = outline[i] * outline_alpha;
					const auto a = t * 255 + o * (full - t) / 255;
					if (a <= 0) continue;

					const auto blend = [&](const int64_t text_value, const int64_t outline_value)
					{
						return static_cast<uint8_t>((text_value * t * 255 + outline_value * o * (full - t) / 255) / a);
					};

					auto* const p = dst + static_cast<ptrdiff_t>(x) * 4;
					p[0] = blend(channel(c, 16), channel(spec.outline_color, 16));
					p[1] = blend(channel(c, 8), channel(spec.outline_color, 8));
					p[2] = blend(channel(c, 0), channel(spec.outline_color, 0));
					p[3] = static_cast<uint8_t>(std::min<int64_t>(255, a / full));
				}
			}
		}
		else
		{
			surface.reset();
		}
	}

	g_object_unref(layout);
	pango_font_description_free(desc);
	return surface;
}
