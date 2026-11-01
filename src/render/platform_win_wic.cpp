// This file is part of libdesktop, a C++20 platform layer for desktop applications
// Copyright 2026  Zac Walker
// 
// This program is free software; you can redistribute it and / or modify it
// under the terms of the LGPL License either version 2.1 or later.
// License details are available at https://www.gnu.org/licenses/lgpl-2.1.html
// This program is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY

// Purpose: Windows Imaging Component integration. Provides fallback image decoding
// for formats not handled by specialized decoders, and rasterises icon glyphs and styled
// text blocks - subtitles - into surfaces with DirectWrite.

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

namespace
{
	// Glyph coverage in each run's colour, gathered into two planes the size of the block: how much of
	// each pixel the text covers, and the colour of the run that covers it most.
	class styled_glyph_renderer final : public IDWriteTextRenderer
	{
		IDWriteFactory* _factory = nullptr;
		int _width = 0;
		int _height = 0;
		std::vector<uint8_t>& _coverage;
		std::vector<uint32_t>& _colors;
		const std::vector<std::pair<IUnknown*, uint32_t>>& _effects;
		std::atomic<ULONG> _ref = 1;

	public:
		styled_glyph_renderer(IDWriteFactory* factory, const int width, const int height, std::vector<uint8_t>& coverage,
		                      std::vector<uint32_t>& colors, const std::vector<std::pair<IUnknown*, uint32_t>>& effects) :
			_factory(factory), _width(width), _height(height), _coverage(coverage), _colors(colors), _effects(effects)
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
		                                       const DWRITE_GLYPH_RUN_DESCRIPTION*, IUnknown* effect) override
		{
			if (!glyphRun || glyphRun->glyphCount == 0) return S_OK;

			auto color = 0xFFFFFFFFu;

			for (const auto& [known, known_color] : _effects)
			{
				if (known == effect)
				{
					color = known_color;
					break;
				}
			}

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

			std::vector<uint8_t> texture(static_cast<size_t>(w) * h * 3);

			if (FAILED(analysis->CreateAlphaTexture(DWRITE_TEXTURE_CLEARTYPE_3x1, &bounds, texture.data(),
				static_cast<uint32_t>(texture.size()))))
			{
				return S_OK;
			}

			for (auto y = 0; y < h; ++y)
			{
				const auto dy = bounds.top + y;
				if (dy < 0 || dy >= _height) continue;

				const auto* src = texture.data() + static_cast<size_t>(y) * w * 3;

				for (auto x = 0; x < w; ++x, src += 3)
				{
					const auto dx = bounds.left + x;
					if (dx < 0 || dx >= _width) continue;

					const auto a = (src[0] + src[1] + src[2]) / 3;
					const auto i = static_cast<size_t>(dy) * _width + dx;

					if (a > 0 && a >= _coverage[i])
					{
						_coverage[i] = static_cast<uint8_t>(a);
						_colors[i] = color;
					}
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

	// A run's drawing effect is only an identity: the renderer looks its colour up by address.
	class text_run_effect final : public IUnknown
	{
		std::atomic<ULONG> _ref = 1;

	public:
		HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override
		{
			if (riid == __uuidof(IUnknown))
			{
				*ppv = static_cast<IUnknown*>(this);
				AddRef();
				return S_OK;
			}

			*ppv = nullptr;
			return E_NOINTERFACE;
		}

		ULONG STDMETHODCALLTYPE AddRef() override { return ++_ref; }
		ULONG STDMETHODCALLTYPE Release() override { return --_ref; }
	};

	// The largest coverage within radius pixels, as a square: two passes, and at a few pixels as round as
	// an outline needs to look.
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
				for (auto dx = std::max(0, x - radius); dx <= std::min(width - 1, x + radius); ++dx) m = std::max(m, row[dx]);
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
}

ui::surface_ptr platform::render_styled_text(const styled_text& spec)
{
	if (spec.font_px < 1.0f) return {};

	struct run_range
	{
		uint32_t start = 0;
		uint32_t length = 0;
		const text_run* run = nullptr;
	};

	std::wstring text;
	std::vector<run_range> ranges;

	for (const auto& r : spec.runs)
	{
		const auto run_text = str::utf8_to_utf16(r.text);
		ranges.push_back({static_cast<uint32_t>(text.size()), static_cast<uint32_t>(run_text.size()), &r});
		text += run_text;
	}

	if (text.find_first_not_of(L" \t\r\n") == std::wstring::npos) return {};

	ComPtr<IDWriteFactory> factory;

	if (FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
		reinterpret_cast<IUnknown**>(factory.GetAddressOf()))) || !factory)
	{
		return {};
	}

	const auto family = str::utf8_to_utf16(spec.font_family.empty() ? std::string_view("Arial") : spec.font_family);
	ComPtr<IDWriteTextFormat> format;

	if (FAILED(factory->CreateTextFormat(family.c_str(), nullptr, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL,
		DWRITE_FONT_STRETCH_NORMAL, spec.font_px, L"", &format)))
	{
		return {};
	}

	format->SetTextAlignment(spec.alignment == 0
		                         ? DWRITE_TEXT_ALIGNMENT_LEADING
		                         : spec.alignment == 2
		                         ? DWRITE_TEXT_ALIGNMENT_TRAILING
		                         : DWRITE_TEXT_ALIGNMENT_CENTER);
	format->SetWordWrapping(spec.wrap_width > 0 ? DWRITE_WORD_WRAPPING_WRAP : DWRITE_WORD_WRAPPING_NO_WRAP);

	// Declared before the layout, so they outlive the references it takes on them.
	std::vector<std::unique_ptr<text_run_effect>> effects;
	std::vector<std::pair<IUnknown*, uint32_t>> effect_colors;
	ComPtr<IDWriteTextLayout> layout;

	constexpr auto unbounded = 100000.0f;
	const auto max_width = spec.wrap_width > 0 ? static_cast<float>(spec.wrap_width) : unbounded;

	if (FAILED(factory->CreateTextLayout(text.c_str(), static_cast<uint32_t>(text.size()), format.Get(), max_width,
		unbounded, &layout)))
	{
		return {};
	}

	for (const auto& r : ranges)
	{
		if (r.length == 0) continue;

		const DWRITE_TEXT_RANGE range{r.start, r.length};
		if (r.run->bold) layout->SetFontWeight(DWRITE_FONT_WEIGHT_BOLD, range);
		if (r.run->italic) layout->SetFontStyle(DWRITE_FONT_STYLE_ITALIC, range);

		const auto& effect = effects.emplace_back(std::make_unique<text_run_effect>());
		layout->SetDrawingEffect(effect.get(), range);
		effect_colors.emplace_back(effect.get(), r.run->color);
	}

	DWRITE_TEXT_METRICS metrics{};
	if (FAILED(layout->GetMetrics(&metrics))) return {};

	// The box hugs the text, so its lines align against one another rather than an arbitrary width.
	const auto box_width = std::max(1.0f, std::ceil(metrics.widthIncludingTrailingWhitespace));
	const auto box_height = std::max(1.0f, std::ceil(metrics.height));
	layout->SetMaxWidth(box_width);
	layout->SetMaxHeight(box_height);

	DWRITE_OVERHANG_METRICS overhang{};
	layout->GetOverhangMetrics(&overhang);

	const auto margin = static_cast<int>(std::ceil(std::max(0.0f, spec.outline_px))) + 2;
	const auto beyond = [](const float v) { return static_cast<int>(std::ceil(std::max(0.0f, v))); };
	const auto left = margin + beyond(overhang.left);
	const auto top = margin + beyond(overhang.top);
	const auto width = left + static_cast<int>(box_width) + margin + beyond(overhang.right);
	const auto height = top + static_cast<int>(box_height) + margin + beyond(overhang.bottom);

	// A block no picture could show is refused rather than allocated.
	if (width <= 0 || height <= 0 || width > 8192 || height > 8192) return {};

	std::vector<uint8_t> coverage(static_cast<size_t>(width) * height);
	std::vector<uint32_t> colors(coverage.size(), 0xFFFFFFFFu);
	styled_glyph_renderer renderer(factory.Get(), width, height, coverage, colors, effect_colors);

	if (FAILED(layout->Draw(nullptr, &renderer, static_cast<float>(left), static_cast<float>(top)))) return {};

	const auto radius = static_cast<int>(std::lround(std::max(0.0f, spec.outline_px)));
	const auto outline = radius > 0 ? dilate(coverage, width, height, radius) : std::vector<uint8_t>(coverage.size());

	auto surface = std::make_shared<ui::surface>();
	if (!surface->alloc(width, height, ui::texture_format::ARGB, ui::orientation::top_left)) return {};
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

			// Straight alpha, text over its outline: a = t + o(1 - t), each colour weighted by its share. The
			// coverages are in 255 * 255 units, so the sums are kept in 64 bits.
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

	return surface;
}
