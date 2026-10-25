// This file is part of libdesktop, a C++20 platform layer for desktop applications
// Copyright 2026  Zac Walker
// 
// This program is free software; you can redistribute it and / or modify it
// under the terms of the LGPL License either version 2.1 or later.
// License details are available at https://www.gnu.org/licenses/lgpl-2.1.html
// This program is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY

// Purpose: Windows Imaging Component integration. Provides fallback image decoding
// for formats not handled by specialized decoders.

#include "libdesktop/render.h"

#include "libdesktop/platform_win.h"
#include "libdesktop/platform_win_visual.h"
#include <wincodec.h>
#include <dwrite.h>

// Minimal DirectWrite text renderer that rasterises glyph coverage as opaque white
// (straight-alpha BGRA) into a ui::surface. Replaces the previous Direct2D DrawText path so
// that icon glyphs can be produced without any Direct2D dependency.
namespace
{
	class icon_glyph_renderer final : public IDWriteTextRenderer
	{
		IDWriteFactory* _factory = nullptr;
		ui::surface* _surface = nullptr;
		std::atomic<ULONG> _ref = 1;

	public:
		icon_glyph_renderer(IDWriteFactory* factory, ui::surface* surface) : _factory(factory), _surface(surface)
		{
		}

		HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override
		{
			if (riid == __uuidof(IUnknown) || riid == __uuidof(IDWritePixelSnapping) ||
				riid == __uuidof(IDWriteTextRenderer))
			{
				*ppv = static_cast<IDWriteTextRenderer*>(this);
				AddRef();
				return S_OK;
			}

			*ppv = nullptr;
			return E_NOINTERFACE;
		}

		ULONG STDMETHODCALLTYPE AddRef() override { return ++_ref; }
		ULONG STDMETHODCALLTYPE Release() override { return --_ref; }

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

		HRESULT STDMETHODCALLTYPE DrawGlyphRun(void*, const FLOAT baselineOriginX, const FLOAT baselineOriginY,
		                                       DWRITE_MEASURING_MODE, const DWRITE_GLYPH_RUN* glyphRun,
		                                       const DWRITE_GLYPH_RUN_DESCRIPTION*, IUnknown*) override
		{
			if (!glyphRun || glyphRun->glyphCount == 0) return S_OK;

			ComPtr<IDWriteGlyphRunAnalysis> analysis;

			if (FAILED(_factory->CreateGlyphRunAnalysis(glyphRun, 1.0f, nullptr, DWRITE_RENDERING_MODE_NATURAL,
				DWRITE_MEASURING_MODE_NATURAL, baselineOriginX, baselineOriginY,
				&analysis)))
			{
				return S_OK;
			}

			RECT bounds{};
			if (FAILED(analysis->GetAlphaTextureBounds(DWRITE_TEXTURE_CLEARTYPE_3x1, &bounds))) return S_OK;

			const auto w = bounds.right - bounds.left;
			const auto h = bounds.bottom - bounds.top;
			if (w <= 0 || h <= 0) return S_OK;

			std::vector<uint8_t> coverage(static_cast<size_t>(w) * h * 3);

			if (FAILED(analysis->CreateAlphaTexture(DWRITE_TEXTURE_CLEARTYPE_3x1, &bounds, coverage.data(),
				static_cast<uint32_t>(coverage.size()))))
			{
				return S_OK;
			}

			const auto sw = static_cast<int>(_surface->width());
			const auto sh = static_cast<int>(_surface->height());

			for (auto y = 0; y < h; ++y)
			{
				const auto dy = bounds.top + y;
				if (dy < 0 || dy >= sh) continue;

				auto* const dst = _surface->pixels_line(dy);
				const auto* src = coverage.data() + static_cast<size_t>(y) * w * 3;

				for (auto x = 0; x < w; ++x, src += 3)
				{
					const auto dx = bounds.left + x;
					if (dx < 0 || dx >= sw) continue;

					const auto a = (src[0] + src[1] + src[2]) / 3;
					if (a == 0) continue;

					auto* const p = dst + static_cast<ptrdiff_t>(dx) * 4;
					const auto na = std::max<int>(a, p[3]);
					p[0] = 255;
					p[1] = 255;
					p[2] = 255;
					p[3] = static_cast<uint8_t>(na);
				}
			}

			return S_OK;
		}

		HRESULT STDMETHODCALLTYPE DrawUnderline(void*, FLOAT, FLOAT, const DWRITE_UNDERLINE*, IUnknown*) override
		{
			return S_OK;
		}

		HRESULT STDMETHODCALLTYPE
		DrawStrikethrough(void*, FLOAT, FLOAT, const DWRITE_STRIKETHROUGH*, IUnknown*) override
		{
			return S_OK;
		}

		HRESULT STDMETHODCALLTYPE DrawInlineObject(void*, FLOAT, FLOAT, IDWriteInlineObject*, BOOL, BOOL,
		                                           IUnknown*) override
		{
			return S_OK;
		}
	};
}

ui::const_surface_ptr platform::create_icon_surface(const char32_t ch)
{
	static std::unordered_map<char32_t, ui::const_surface_ptr> cache;
	const auto found = cache.find(ch);

	if (found != cache.end())
	{
		return found->second;
	}

	auto surface_result = std::make_shared<ui::surface>();

	constexpr auto cxy = 160;

	if (!surface_result->alloc(cxy, cxy, ui::texture_format::ARGB, ui::orientation::top_left))
	{
		return surface_result;
	}

	surface_result->make_blank(); // transparent background

	ComPtr<IDWriteFactory> dwrite_factory;
	ComPtr<IDWriteFontCollection> icon_collection;
	ComPtr<IDWriteTextFormat> text_format;
	ComPtr<IDWriteTextLayout> text_layout;

	auto hr = DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(dwrite_factory),
	                              reinterpret_cast<IUnknown**>(dwrite_factory.GetAddressOf()));

	if (SUCCEEDED(hr))
	{
		hr = create_icon_font_collection(dwrite_factory.Get(), &icon_collection);
	}

	if (SUCCEEDED(hr))
	{
		hr = dwrite_factory->CreateTextFormat(icon_font_family(), icon_collection.Get(), DWRITE_FONT_WEIGHT_NORMAL,
		                                      DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
		                                      static_cast<FLOAT>(df::mul_div(148, icon_font_scale_num,
		                                                                     icon_font_scale_den)), L"",
		                                      &text_format);
	}

	if (SUCCEEDED(hr))
	{
		text_format->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
		text_format->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);

		// DirectWrite takes UTF-16, so the code point is encoded here rather than at the call site.
		std::string utf8;
		str::char32_to_utf8(std::back_inserter(utf8), static_cast<uint32_t>(ch));
		const auto icon_text = str::utf8_to_utf16(utf8);
		hr = dwrite_factory->CreateTextLayout(icon_text.c_str(), static_cast<uint32_t>(icon_text.size()),
		                                      text_format.Get(), static_cast<float>(cxy),
		                                      static_cast<float>(cxy), &text_layout);
	}

	if (SUCCEEDED(hr))
	{
		icon_glyph_renderer renderer(dwrite_factory.Get(), surface_result.get());
		hr = text_layout->Draw(nullptr, &renderer, 0.0f, 0.0f);
	}

	if (SUCCEEDED(hr))
	{
		cache[ch] = surface_result;
	}

	return surface_result;
}
