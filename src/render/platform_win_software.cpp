// This file is part of libdesktop, a C++20 platform layer for desktop applications
// Copyright 2026  Zac Walker
// 
// This program is free software; you can redistribute it and / or modify it
// under the terms of the LGPL License either version 2.1 or later.
// License details are available at https://www.gnu.org/licenses/lgpl-2.1.html
// This program is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY

// Purpose: The Windows half of the CPU software backend: the GDI present target (BitBlt, and
// UpdateLayeredWindow for layered popups), the DirectWrite text engine that rasterises glyph alpha
// bitmaps and blends them into the canvas, and the drop shadow from the module's resources. The draw
// context itself is portable (render_software_context.cpp).

#include "libdesktop/render.h"
#include "libdesktop/platform_win.h"
#include "libdesktop/platform_win_render.h"
#include "libdesktop/platform_win_visual.h"

#include "libdesktop/render_software.h"
#include "libdesktop/platform_win_render_res.h"


//////////////////////////////////////////////////////////////////////////////////////////////
// The drop shadow, decoded from the module's PNG resource into a straight-alpha BGRA surface.
//////////////////////////////////////////////////////////////////////////////////////////////

ui::surface_ptr ui::load_shadow_art()
{
	// The same decoder the hardware backend uses for the same resource. WIC is optional - startup
	// continues without it - so decoding here through the app's own PNG path is what stops the two
	// backends drawing different chrome on a machine that has no WIC.
	return decode_embedded(load_resource(IDB_SHADOW, L"PNG"));
}

//////////////////////////////////////////////////////////////////////////////////////////////
// gdi_present_target - the whole of what the software backend needs Windows for. A DIB section
// is a plain BGRA buffer that GDI is also willing to blit from, which is why the rasterizer can
// write into it directly and the present costs no copy.
//////////////////////////////////////////////////////////////////////////////////////////////

class gdi_present_target final : public ui::software_present_target
{
	HWND _hwnd = nullptr;
	bool _layered = false;

	HDC _mem_dc = nullptr;
	HBITMAP _dib = nullptr;
	HGDIOBJ _old_bitmap = nullptr;
	uint8_t* _bits = nullptr;
	sizei _extent;

	// The device context held for the duration of one present, so a tiled frame takes one
	// GetDC rather than one per tile.
	HDC _present_dc = nullptr;

	// UpdateLayeredWindow needs premultiplied alpha in its own DIB. It is kept across presents so a
	// layered window does not allocate a buffer, a DC and a DIB section on every frame.
	HDC _layered_dc = nullptr;
	HBITMAP _layered_dib = nullptr;
	HGDIOBJ _layered_old_bitmap = nullptr;
	uint8_t* _layered_bits = nullptr;
	sizei _layered_size;

public:
	gdi_present_target(const HWND hwnd, const bool layered) : _hwnd(hwnd), _layered(layered)
	{
	}

	~gdi_present_target() override
	{
		release_buffer();
	}

	bool is_layered() const override
	{
		return _layered;
	}

	ui::software_buffer acquire_buffer(const sizei extent) override
	{
		if (_dib && _extent == extent && _bits)
		{
			return {_bits, _extent, _extent.cx * 4};
		}

		release_buffer();

		if (extent.cx < 1 || extent.cy < 1)
		{
			return {};
		}

		BITMAPINFO bmi = {};
		bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
		bmi.bmiHeader.biWidth = extent.cx;
		bmi.bmiHeader.biHeight = -extent.cy; // top-down
		bmi.bmiHeader.biPlanes = 1;
		bmi.bmiHeader.biBitCount = 32;
		bmi.bmiHeader.biCompression = BI_RGB;

		_mem_dc = CreateCompatibleDC(nullptr);

		if (_mem_dc)
		{
			void* bits = nullptr;
			_dib = CreateDIBSection(_mem_dc, &bmi, DIB_RGB_COLORS, &bits, nullptr, 0);

			if (_dib && bits)
			{
				_old_bitmap = SelectObject(_mem_dc, _dib);
				_bits = static_cast<uint8_t*>(bits);
				_extent = extent;
			}
			else
			{
				release_buffer();
			}
		}

		return {_bits, _extent, _extent.cx * 4};
	}

	void release_buffer() override
	{
		if (_mem_dc)
		{
			if (_old_bitmap) SelectObject(_mem_dc, _old_bitmap);
			DeleteDC(_mem_dc);
			_mem_dc = nullptr;
			_old_bitmap = nullptr;
		}

		if (_dib)
		{
			DeleteObject(_dib);
			_dib = nullptr;
		}

		_bits = nullptr;
		_extent = {};

		free_layered_dib();
	}

	bool begin_present() override
	{
		if (!_bits || !_hwnd) return false;

		// A layered window is presented whole by UpdateLayeredWindow against the screen, so it
		// needs no device context of its own for the tile loop.
		if (_layered) return true;

		_present_dc = GetDC(_hwnd);
		return _present_dc != nullptr;
	}

	void present_tile(const recti tile, const pointi buffer_origin) override
	{
		if (!_present_dc) return;

		BitBlt(_present_dc, tile.left, tile.top, tile.width(), tile.height(),
		       _mem_dc, tile.left - buffer_origin.x, tile.top - buffer_origin.y, SRCCOPY);
	}

	void end_present(const int layer_alpha) override
	{
		if (_present_dc)
		{
			ReleaseDC(_hwnd, _present_dc);
			_present_dc = nullptr;
			return;
		}

		if (_layered) present_layered(layer_alpha);
	}

private:
	void free_layered_dib()
	{
		if (_layered_dc)
		{
			if (_layered_old_bitmap) SelectObject(_layered_dc, _layered_old_bitmap);
			DeleteDC(_layered_dc);
			_layered_dc = nullptr;
			_layered_old_bitmap = nullptr;
		}

		if (_layered_dib)
		{
			DeleteObject(_layered_dib);
			_layered_dib = nullptr;
		}

		_layered_bits = nullptr;
		_layered_size = {};
	}

	void present_layered(const int layer_alpha)
	{
		if (!_bits || _extent.cx < 1 || _extent.cy < 1)
		{
			return;
		}

		if (!_layered_bits || _layered_size != _extent)
		{
			free_layered_dib();

			BITMAPINFO bmi = {};
			bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
			bmi.bmiHeader.biWidth = _extent.cx;
			bmi.bmiHeader.biHeight = -_extent.cy;
			bmi.bmiHeader.biPlanes = 1;
			bmi.bmiHeader.biBitCount = 32;
			bmi.bmiHeader.biCompression = BI_RGB;

			_layered_dc = CreateCompatibleDC(nullptr);

			if (!_layered_dc)
			{
				return;
			}

			void* bits = nullptr;
			_layered_dib = CreateDIBSection(_layered_dc, &bmi, DIB_RGB_COLORS, &bits, nullptr, 0);

			if (!_layered_dib || !bits)
			{
				free_layered_dib();
				return;
			}

			_layered_old_bitmap = SelectObject(_layered_dc, _layered_dib);
			_layered_bits = static_cast<uint8_t*>(bits);
			_layered_size = _extent;
		}

		// UpdateLayeredWindow requires premultiplied alpha.
		const auto* src = _bits;
		auto* dst = _layered_bits;

		for (auto i = 0; i < _extent.cx * _extent.cy; ++i, src += 4, dst += 4)
		{
			const auto a = src[3];
			dst[0] = static_cast<uint8_t>(src[0] * a / 255);
			dst[1] = static_cast<uint8_t>(src[1] * a / 255);
			dst[2] = static_cast<uint8_t>(src[2] * a / 255);
			dst[3] = a;
		}

		const auto screen_dc = GetDC(nullptr);

		if (!screen_dc)
		{
			return;
		}

		RECT wr = {};

		if (!GetWindowRect(_hwnd, &wr))
		{
			ReleaseDC(nullptr, screen_dc);
			return;
		}

		POINT pt_dst = {wr.left, wr.top};
		POINT pt_src = {0, 0};
		SIZE size = {_extent.cx, _extent.cy};
		BLENDFUNCTION bf = {AC_SRC_OVER, 0, static_cast<BYTE>(layer_alpha), AC_SRC_ALPHA};

		UpdateLayeredWindow(_hwnd, screen_dc, &pt_dst, &size, _layered_dc, &pt_src, 0, &bf, ULW_ALPHA);

		ReleaseDC(nullptr, screen_dc);
	}
};

//////////////////////////////////////////////////////////////////////////////////////////////
// software_text_renderer - IDWriteTextRenderer that alpha-blends glyphs into the canvas.
//////////////////////////////////////////////////////////////////////////////////////////////

class software_text_renderer final : public df::no_copy, public IDWriteTextRenderer
{
	// What the current draw goes to: bound by the engine before each call, so a renderer kept across
	// frames never holds on to a context or a canvas.
	ui::draw_context* _dc = nullptr;
	ui::software_canvas* _canvas = nullptr;
	font_renderer_ptr _font;
	int _spacing = 2;
	int _line_height = 0;
	int _base_line_height = 0;

	df::hash_map<uint64_t, render_char_result> _glyph_cache;
	glyph_face_keys _glyph_keys;

	ui::color _clr;
	std::vector<ui::text_highlight_t> _highlights;

	std::atomic<int> _ref_count = 0;

public:
	explicit software_text_renderer(font_renderer_ptr font)
		: _font(std::move(font))
	{
		_line_height = _font->calc_line_height();
		_base_line_height = _font->calc_base_line_height();
	}

	~software_text_renderer() override = default;

	font_renderer_ptr font() const { return _font; }

	void bind(ui::draw_context& dc, ui::software_canvas& canvas)
	{
		_dc = &dc;
		_canvas = &canvas;
	}

	void draw_text(const std::string_view text, const recti bounds, const ui::style::text_style style,
	               const ui::color c, const ui::color bg)
	{
		_clr = c;
		ui::clear_text_highlights(_highlights);
		_font->draw(_dc, this, text, bounds, style, c, bg);
	}

	void draw_text(const std::string_view text, const std::vector<ui::text_highlight_t>& highlights, const recti bounds,
	               const ui::style::text_style style, const ui::color clr, const ui::color bg)
	{
		_clr = clr;
		const auto converted = ui::utf8_to_utf16(text, highlights);
		_highlights = converted.highlights;

		_font->draw(_dc, this, converted.text, bounds, style, clr, bg, _highlights);
	}

	void draw_layout(const std::shared_ptr<text_layout_impl>& text, const recti bounds, const ui::color clr,
	                 const ui::color bg)
	{
		_clr = clr;
		ui::clear_text_highlights(_highlights);
		font_renderer::draw(_dc, this, text->_layout.Get(), bounds, clr, bg);
	}

	const render_char_result& glyph(const uint16_t index, const DWRITE_GLYPH_RUN* glyph_run)
	{
		// Key by the face the index belongs to and by the run's em size, so a fallback glyph
		// (e.g. Hangul from Malgun Gothic) cannot collide in the cache with a same-indexed glyph
		// of the primary face, and a raster made for one font size is never reused at another.
		const auto key = _glyph_keys.key(glyph_run ? glyph_run->fontFace : nullptr,
		                                 glyph_run ? glyph_run->fontEmSize : 0.0f, index);
		const auto found = _glyph_cache.find(key);
		if (found != _glyph_cache.end()) return found->second;
		return _glyph_cache[key] = _font->render_glyph(index, _spacing, glyph_run);
	}

	// IUnknown ------------------------------------------------------------------------------

	HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppvObject) override
	{
		if (riid == __uuidof(IUnknown) || riid == __uuidof(IDWritePixelSnapping) ||
			riid == __uuidof(IDWriteTextRenderer))
		{
			*ppvObject = static_cast<IDWriteTextRenderer*>(this);
			AddRef();
			return S_OK;
		}

		*ppvObject = nullptr;
		return E_NOINTERFACE;
	}

	ULONG STDMETHODCALLTYPE AddRef() override { return static_cast<ULONG>(++_ref_count); }

	ULONG STDMETHODCALLTYPE Release() override
	{
		// The renderer is owned by the draw context's map, so Release never destroys it. The count is
		// clamped at zero so an unbalanced Release cannot report a huge ULONG.
		auto n = _ref_count.load(std::memory_order_relaxed);
		while (n > 0 && !_ref_count.compare_exchange_weak(n, n - 1, std::memory_order_relaxed))
		{
		}
		return static_cast<ULONG>(n > 0 ? n - 1 : 0);
	}

	// IDWritePixelSnapping ------------------------------------------------------------------

	HRESULT STDMETHODCALLTYPE IsPixelSnappingDisabled(void*, BOOL* isDisabled) override
	{
		*isDisabled = FALSE;
		return S_OK;
	}

	HRESULT STDMETHODCALLTYPE GetCurrentTransform(void*, DWRITE_MATRIX* transform) override
	{
		*transform = {1, 0, 0, 1, 0, 0};
		return S_OK;
	}

	HRESULT STDMETHODCALLTYPE GetPixelsPerDip(void*, FLOAT* pixelsPerDip) override
	{
		*pixelsPerDip = 1.0f;
		return S_OK;
	}

	// IDWriteTextRenderer -------------------------------------------------------------------

	HRESULT STDMETHODCALLTYPE DrawGlyphRun(void*, const FLOAT baselineOriginX, const FLOAT baselineOriginY,
	                                       DWRITE_MEASURING_MODE, const DWRITE_GLYPH_RUN* glyphRun,
	                                       const DWRITE_GLYPH_RUN_DESCRIPTION* glyphRunDescription,
	                                       IUnknown*) override
	{
		if (!glyphRun) return S_OK;

		const auto text_position = glyphRunDescription ? glyphRunDescription->textPosition : 0u;
		const auto text_length = glyphRunDescription ? glyphRunDescription->stringLength : glyphRun->glyphCount;
		const auto* const cluster_map = glyphRunDescription ? glyphRunDescription->clusterMap : nullptr;
		const auto cluster_spans = _highlights.empty()
			                           ? std::vector<ui::text_cluster_span>{}
			                           : ui::text_cluster_spans_for_glyph_run(
				                           glyphRun->glyphCount, cluster_map, text_length, text_position);

		const auto ty = std::floor(baselineOriginY + 0.5f);
		const auto is_left_to_right = (glyphRun->bidiLevel & 0x01) == 0;

		auto tx = 0.0f;

		for (auto i = 0u; i < glyphRun->glyphCount; ++i)
		{
			const auto c = glyphRun->glyphIndices[i];
			const auto ax = glyphRun->glyphAdvances[i];
			auto sx = tx;
			auto sy = ui::glyph_top_from_baseline(static_cast<float>(ty), _base_line_height, 0.0f);

			if (glyphRun->glyphOffsets)
			{
				sx += glyphRun->glyphOffsets[i].advanceOffset;
				sy = ui::glyph_top_from_baseline(static_cast<float>(ty), _base_line_height,
				                                  glyphRun->glyphOffsets[i].ascenderOffset);
			}

			sx = is_left_to_right ? baselineOriginX + sx - _spacing : baselineOriginX - sx - ax - _spacing;

			if (c != 0)
			{
				auto clr = _clr;
				if (!_highlights.empty() && i < cluster_spans.size())
				{
					clr = ui::text_cluster_color(_clr, cluster_spans[i], _highlights);
				}

				const auto& g = glyph(c, glyphRun);
				_canvas->blend_glyph(df::round(sx), df::round(sy), g.pixels.data(), {g.cx, g.cy}, clr);
			}

			tx += ax;
		}

		return S_OK;
	}

	HRESULT STDMETHODCALLTYPE DrawUnderline(void*, FLOAT, FLOAT, const DWRITE_UNDERLINE*, IUnknown*) override
	{
		return S_OK;
	}

	HRESULT STDMETHODCALLTYPE DrawStrikethrough(void*, FLOAT, FLOAT, const DWRITE_STRIKETHROUGH*, IUnknown*) override
	{
		return S_OK;
	}

	HRESULT STDMETHODCALLTYPE
	DrawInlineObject(void*, FLOAT, FLOAT, IDWriteInlineObject*, BOOL, BOOL, IUnknown*) override
	{
		return S_OK;
	}
};

//////////////////////////////////////////////////////////////////////////////////////////////
// dwrite_text_engine - the software context's text, through the factories' DirectWrite fonts.
//////////////////////////////////////////////////////////////////////////////////////////////

class dwrite_text_engine final : public ui::software_text_engine
{
	factories_ptr _f;
	int _base_font_size = ui::default_font_size;
	std::map<ui::style::font_face, std::shared_ptr<software_text_renderer>> _renderers;

	font_renderer_ptr font(const ui::style::font_face face) const
	{
		return _f ? _f->font_face(face, _base_font_size) : nullptr;
	}

	software_text_renderer* renderer_for(ui::draw_context& dc, ui::software_canvas& canvas,
	                                     const ui::style::font_face face)
	{
		auto found = _renderers.find(face);

		if (found == _renderers.end())
		{
			const auto fr = font(face);
			if (!fr) return nullptr;
			found = _renderers.emplace(face, std::make_shared<software_text_renderer>(fr)).first;
		}

		found->second->bind(dc, canvas);
		return found->second.get();
	}

public:
	dwrite_text_engine(factories_ptr f, const int base_font_size) : _f(std::move(f)), _base_font_size(base_font_size)
	{
	}

	void base_font_size(const int size) override
	{
		if (_base_font_size == size) return;
		_base_font_size = size;
		_renderers.clear();
	}

	sizei measure_text(const std::string_view text, const ui::style::font_face face, const ui::style::text_style style,
	                   const int cx, const int cy) override
	{
		const auto fr = font(face);
		return fr ? fr->measure(text, style, cx, cy) : sizei{};
	}

	int text_line_height(const ui::style::font_face face) override
	{
		const auto fr = font(face);
		return fr ? static_cast<int>(fr->calc_line_height()) : 0;
	}

	ui::text_layout_ptr create_text_layout(const ui::style::font_face face) override
	{
		const auto fr = font(face);
		return fr ? std::make_shared<text_layout_impl>(fr, face) : nullptr;
	}

	void draw_text(ui::draw_context& dc, ui::software_canvas& canvas, const std::string_view text, const recti bounds,
	               const ui::style::font_face face, const ui::style::text_style style, const ui::color c,
	               const ui::color bg) override
	{
		auto* const tr = renderer_for(dc, canvas, face);
		if (tr) tr->draw_text(text, bounds, style, c, bg);
	}

	void draw_text(ui::draw_context& dc, ui::software_canvas& canvas, const std::string_view text,
	               const std::vector<ui::text_highlight_t>& highlights, const recti bounds,
	               const ui::style::font_face face, const ui::style::text_style style, const ui::color c,
	               const ui::color bg) override
	{
		auto* const tr = renderer_for(dc, canvas, face);
		if (tr) tr->draw_text(text, highlights, bounds, style, c, bg);
	}

	void draw_text(ui::draw_context& dc, ui::software_canvas& canvas, const ui::text_layout_ptr& layout,
	               const recti bounds, const ui::color c, const ui::color bg) override
	{
		const auto t = std::dynamic_pointer_cast<text_layout_impl>(layout);
		if (!t) return;
		auto* const tr = renderer_for(dc, canvas, t->_font);
		if (tr) tr->draw_layout(t, bounds, c, bg);
	}
};

draw_context_device_ptr create_software_draw_context(const factories_ptr& f, const HWND hwnd, const bool layered,
                                                     const int base_font_size)
{
	return ui::create_software_draw_context(std::make_shared<dwrite_text_engine>(f, base_font_size),
	                                        std::make_shared<gdi_present_target>(hwnd, layered), base_font_size);
}

draw_context_device_ptr create_software_draw_context(const factories_ptr& f,
                                                     std::shared_ptr<ui::software_present_target> target,
                                                     const int base_font_size)
{
	return ui::create_software_draw_context(std::make_shared<dwrite_text_engine>(f, base_font_size),
	                                        std::move(target), base_font_size);
}

// A private factories instance, as probe_font_cache makes one: the engine needs DirectWrite alone, so
// this touches neither the application's renderer nor its font cache.
ui::software_text_engine_ptr ui::create_text_engine(const int base_font_size)
{
	auto f = std::make_shared<factories>();

	if (FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
		reinterpret_cast<IUnknown**>(f->dwrite.GetAddressOf()))) || !f->dwrite)
	{
		return nullptr;
	}

	return std::make_shared<dwrite_text_engine>(std::move(f), base_font_size);
}
