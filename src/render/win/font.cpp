// This file is part of libdesktop, a C++20 platform layer for desktop applications
// Copyright 2026  Zac Walker
// 
// This program is free software; you can redistribute it and / or modify it
// under the terms of the LGPL License either version 2.1 or later.
// License details are available at https://www.gnu.org/licenses/lgpl-2.1.html
// This program is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY

// Purpose: DirectWrite font rendering. Handles text layout, font loading,
// glyph rendering, and text measurement using DirectWrite API.

#include "libdesktop/render.h"

#include "libdesktop/platform/win32.h"
#include "libdesktop/render/win32.h"

template <typename InterfaceType>
InterfaceType* SafeAcquire(InterfaceType* newObject)
{
	if (newObject != nullptr)
		newObject->AddRef();

	return newObject;
}

static constexpr uint32_t icon_font_collection_id = 19;

// The bundled icon font, not a system one: it is supplied by the application through the
// registration API, so this must be the family name inside that font.
static constexpr auto icon_font_name = L"FluentSystemIcons-Resizable";

static core::cspan icon_font_bytes;
// An installed family the application named instead of bundling bytes; empty while it bundles them.
static std::wstring icon_font_installed_family;

void render::set_icon_font(const core::cspan font_bytes)
{
	icon_font_bytes = font_bytes;
	icon_font_installed_family.clear();
}

void render::set_icon_font_family(const std::string_view family)
{
	icon_font_bytes = {};
	icon_font_installed_family = core::utf8_to_utf16(family);

	// Resolved once here, so every icon - drawn, or rasterised to a surface - uses the same family.
	ComPtr<IDWriteFactory> factory;
	ComPtr<IDWriteFontCollection> system_fonts;
	if (FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
		reinterpret_cast<IUnknown**>(factory.GetAddressOf()))) || !factory ||
		FAILED(factory->GetSystemFontCollection(system_fonts.GetAddressOf())) || !system_fonts)
	{
		return;
	}

	const auto installed = [&](const wchar_t* name)
	{
		UINT32 index = 0;
		BOOL exists = FALSE;
		return SUCCEEDED(system_fonts->FindFamilyName(name, &index, &exists)) && exists;
	};

	if (!installed(icon_font_installed_family.c_str()) && icon_font_installed_family == L"Segoe Fluent Icons" &&
		installed(L"Segoe MDL2 Assets"))
	{
		icon_font_installed_family = L"Segoe MDL2 Assets";
	}
}

class memory_font_file_stream final : public IDWriteFontFileStream
{
public:
	explicit memory_font_file_stream(const core::cspan bytes) : refCount_(0), _bytes(bytes)
	{
	}

	virtual ~memory_font_file_stream() = default;

	// IUnknown methods

	HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppvObject) override
	{
		if (riid == IID_IUnknown ||
			riid == __uuidof(IDWriteFontFileStream))
		{
			*ppvObject = this;
			AddRef();
			return S_OK;
		}
		*ppvObject = nullptr;
		return E_NOINTERFACE;
	}

	ULONG STDMETHODCALLTYPE AddRef() override
	{
		return InterlockedIncrement(&refCount_);
	}

	ULONG STDMETHODCALLTYPE Release() override
	{
		const ULONG newCount = InterlockedDecrement(&refCount_);
		if (newCount == 0)
			delete this;

		return newCount;
	}

	// IDWriteFontFileStream methods
	HRESULT STDMETHODCALLTYPE ReadFileFragment(
		const void** fragmentStart, // [fragmentSize] in bytes
		UINT64 fileOffset,
		UINT64 fragmentSize,
		OUT void** fragmentContext
	) override;

	void STDMETHODCALLTYPE ReleaseFileFragment(void* fragmentContext) override;

	HRESULT STDMETHODCALLTYPE GetFileSize(OUT UINT64* fileSize) override;

	HRESULT STDMETHODCALLTYPE GetLastWriteTime(OUT UINT64* lastWriteTime) override;

	bool is_initialized() const
	{
		return !_bytes.empty();
	}

private:
	ULONG refCount_;
	core::cspan _bytes;
};

// IDWriteFontFileStream methods
HRESULT STDMETHODCALLTYPE memory_font_file_stream::ReadFileFragment(
	const void** fragmentStart, // [fragmentSize] in bytes
	const UINT64 fileOffset,
	const UINT64 fragmentSize,
	OUT void** fragmentContext
)
{
	const auto size = static_cast<UINT64>(_bytes.size);
	// The loader is responsible for doing a bounds check.
	if (_bytes.data && fileOffset <= size &&
		fragmentSize <= size &&
		fileOffset <= size - fragmentSize) // Prevent overflow
	{
		*fragmentStart = _bytes.data + static_cast<size_t>(fileOffset);
		*fragmentContext = nullptr;
		return S_OK;
	}
	*fragmentStart = nullptr;
	*fragmentContext = nullptr;
	return E_FAIL;
}

void STDMETHODCALLTYPE memory_font_file_stream::ReleaseFileFragment(void* fragmentContext)
{
}

HRESULT STDMETHODCALLTYPE memory_font_file_stream::GetFileSize(OUT UINT64* fileSize)
{
	*fileSize = static_cast<UINT64>(_bytes.size);
	return S_OK;
}

HRESULT STDMETHODCALLTYPE memory_font_file_stream::GetLastWriteTime(OUT UINT64* lastWriteTime)
{
	// The concept of last write time does not apply to this loader.
	*lastWriteTime = 0;
	return E_NOTIMPL;
}

class resource_font_file_loader final : public IDWriteFontFileLoader
{
public:
	resource_font_file_loader() : refCount_(0)
	{
	}

	virtual ~resource_font_file_loader() = default;

	ULONG refCount_;

	HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppvObject) override
	{
		if (riid == __uuidof(IUnknown) ||
			riid == __uuidof(IDWriteFontFileLoader))
		{
			AddRef();
			*ppvObject = static_cast<IDWriteFontFileLoader*>(this);
			return S_OK;
		}

		// DirectWrite routinely probes for interfaces this loader does not implement (e.g. remote
		// font support); answering E_NOINTERFACE is the correct and expected outcome, not worth logging.
		*ppvObject = nullptr;
		return E_NOINTERFACE;
	}

	ULONG STDMETHODCALLTYPE AddRef() override
	{
		return InterlockedIncrement(&refCount_);
	}

	ULONG STDMETHODCALLTYPE Release() override
	{
		const ULONG newCount = InterlockedDecrement(&refCount_);
		// do not delete
		return newCount;
	}

	HRESULT STDMETHODCALLTYPE CreateStreamFromKey(
		const void* fontFileReferenceKey, // [fontFileReferenceKeySize] in bytes
		const UINT32 fontFileReferenceKeySize,
		OUT IDWriteFontFileStream** fontFileStream
	) override
	{
		*fontFileStream = nullptr;

		if (fontFileReferenceKeySize != sizeof(uint32_t))
			return E_INVALIDARG;
		(void)fontFileReferenceKey;

		auto* const stream = new(std::nothrow) memory_font_file_stream(icon_font_bytes);

		if (stream == nullptr)
			return E_OUTOFMEMORY;

		if (!stream->is_initialized())
		{
			core::log(__FUNCTION__, "The icon font has not been registered");
			delete stream;
			return E_FAIL;
		}

		*fontFileStream = SafeAcquire(stream);

		return S_OK;
	}
};

// Immortal singleton: registered with the shared DWrite factory and referenced (via
// CreateCustomFontFileReference) by custom font faces. It must outlive every DWrite object
// that depends on it, including during C++ static destruction whose order across translation
// units is undefined. It is therefore heap-allocated once and intentionally never freed
// (its Release() is already a deliberate no-op for the same reason).
static resource_font_file_loader& font_loader = *new resource_font_file_loader();

class resource_font_file_enumerator final : public IDWriteFontFileEnumerator
{
public:
	resource_font_file_enumerator() : refCount_(0), _index(0), _factory(nullptr), _collection_key(nullptr),
	                                  _collection_key_size(0)
	{
	}

	explicit resource_font_file_enumerator(IDWriteFactory* factory,
	                                       const void* collectionKey,
	                                       const UINT32 collectionKeySize) : refCount_(0), _index(0),
	                                                                         _factory(factory),
	                                                                         _collection_key(collectionKey),
	                                                                         _collection_key_size(collectionKeySize)
	{
	}

	virtual ~resource_font_file_enumerator() = default;

	ULONG refCount_;
	int _index = 0;
	IDWriteFactory* _factory = nullptr;
	const void* _collection_key = nullptr;
	UINT32 _collection_key_size = 0;

	HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppvObject) override
	{
		if (riid == __uuidof(IUnknown) ||
			riid == __uuidof(IDWriteFontFileEnumerator))
		{
			AddRef();
			*ppvObject = static_cast<IDWriteFontFileEnumerator*>(this);
			return S_OK;
		}

		*ppvObject = nullptr;
		return E_NOINTERFACE;
	}

	ULONG STDMETHODCALLTYPE AddRef() override
	{
		return InterlockedIncrement(&refCount_);
	}

	ULONG STDMETHODCALLTYPE Release() override
	{
		const ULONG newCount = InterlockedDecrement(&refCount_);
		if (newCount == 0)
			delete this;
		return newCount;
	}

	ComPtr<IDWriteFontFile> _current;

	// IDWriteFontFileEnumerator methods
	HRESULT STDMETHODCALLTYPE MoveNext(OUT BOOL* hasCurrentFile) override
	{
		if (hasCurrentFile) *hasCurrentFile = false;
		_current.Reset();

		auto result = S_FALSE;

		if (_index == 0)
		{
			ComPtr<IDWriteFontFile> current;

			const auto hr = _factory->CreateCustomFontFileReference(
				&icon_font_collection_id,
				sizeof(icon_font_collection_id),
				&font_loader,
				&current);

			core::log(__FUNCTION__, std::format("CreateCustomFontFileReference {:x}", static_cast<uint32_t>(hr)));

			if (SUCCEEDED(hr))
			{
				_current = current;
				result = S_OK;
			}
		}

		if (hasCurrentFile) *hasCurrentFile = result == S_OK;
		_index += 1;
		return result;
	}

	HRESULT STDMETHODCALLTYPE GetCurrentFontFile(OUT IDWriteFontFile** fontFile) override
	{
		_current.CopyTo(fontFile);
		return S_OK;
	}
};

class resource_font_collection_loader final : public IDWriteFontCollectionLoader
{
public:
	resource_font_collection_loader() : refCount_(0)
	{
	}

	virtual ~resource_font_collection_loader() = default;

	ULONG refCount_;

	HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppvObject) override
	{
		if (riid == __uuidof(IUnknown) ||
			riid == __uuidof(IDWriteFontCollectionLoader))
		{
			AddRef();
			*ppvObject = static_cast<IDWriteFontCollectionLoader*>(this);
			return S_OK;
		}

		*ppvObject = nullptr;
		return E_NOINTERFACE;
	}

	ULONG STDMETHODCALLTYPE AddRef() override
	{
		return InterlockedIncrement(&refCount_);
	}

	ULONG STDMETHODCALLTYPE Release() override
	{
		const ULONG newCount = InterlockedDecrement(&refCount_);
		// Do not delete this
		return newCount;
	}

	// IDWriteFontCollectionLoader methods
	HRESULT STDMETHODCALLTYPE CreateEnumeratorFromKey(
		IDWriteFactory* factory,
		const void* collectionKey, // [collectionKeySize] in bytes
		const UINT32 collectionKeySize,
		OUT IDWriteFontFileEnumerator** fontFileEnumerator
	) override
	{
		auto* const enumerator = new(std::nothrow) resource_font_file_enumerator(
			factory, collectionKey, collectionKeySize);
		if (enumerator == nullptr)
		{
			*fontFileEnumerator = nullptr;
			return E_OUTOFMEMORY;
		}
		*fontFileEnumerator = SafeAcquire(enumerator);
		return S_OK;
	}
};

// Immortal singleton (see font_loader above): heap-allocated once and never freed so it
// cannot be destroyed before the DWrite objects that depend on it during process teardown.
static resource_font_collection_loader& font_collection_loader = *new resource_font_collection_loader();


static font_renderer_ptr create_font_renderer(IDWriteFactory* dwrite, IDWriteFontCollection* font_collection,
                                              const wchar_t* font_name, int font_height, int weight = 400,
                                              bool italic = false, bool underline = false, bool strikeout = false,
                                              bool cell_size = false)
{
	uint32_t index = {};
	const auto dwrite_weight = static_cast<DWRITE_FONT_WEIGHT>(std::clamp(weight, 100, 900));
	const auto dwrite_style = italic ? DWRITE_FONT_STYLE_ITALIC : DWRITE_FONT_STYLE_NORMAL;
	BOOL exists = {};
	ComPtr<IDWriteFontFamily> family;
	ComPtr<IDWriteFont> font;
	ComPtr<IDWriteFontFace> font_face;
	ComPtr<IDWriteTextFormat> text_format;

	auto hr = font_collection->FindFamilyName(font_name, &index, &exists);

	if (FAILED(hr))
	{
		core::log(__FUNCTION__, std::format("Failed to find font family {} - FindFamilyName failed: {:x}",
		                                  core::utf16_to_utf8(font_name), static_cast<uint32_t>(hr)));
		return nullptr;
	}

	if (!exists)
	{
		core::log(__FUNCTION__, std::format("Failed to create font {} - font family not found in collection",
		                                  core::utf16_to_utf8(font_name)));
		return nullptr;
	}

	hr = font_collection->GetFontFamily(index, &family);
	if (FAILED(hr))
	{
		core::log(__FUNCTION__, std::format("Failed to create font {} - GetFontFamily failed: {:x}",
		                                  core::utf16_to_utf8(font_name), static_cast<uint32_t>(hr)));
		return nullptr;
	}

	hr = family->GetFirstMatchingFont(dwrite_weight, DWRITE_FONT_STRETCH_NORMAL, dwrite_style, &font);
	if (FAILED(hr))
	{
		core::log(__FUNCTION__, std::format("Failed to create font {} - GetFirstMatchingFont failed: {:x}",
		                                  core::utf16_to_utf8(font_name), static_cast<uint32_t>(hr)));
		return nullptr;
	}

	hr = font->CreateFontFace(&font_face);
	if (FAILED(hr))
	{
		core::log(__FUNCTION__, std::format("Failed to create font {} - CreateFontFace failed: {:x}",
		                                  core::utf16_to_utf8(font_name), static_cast<uint32_t>(hr)));
		return nullptr;
	}

	int actual_font_height = std::max(1, font_height);
	if (cell_size)
	{
		DWRITE_FONT_METRICS metrics{};
		font_face->GetMetrics(&metrics);
		const auto cell = metrics.ascent + metrics.descent + metrics.lineGap;
		if (cell > 0 && metrics.designUnitsPerEm > 0)
		{
			actual_font_height = std::max(1, core::mul_div(font_height, metrics.designUnitsPerEm, cell));
		}
	}

	hr = dwrite->CreateTextFormat(
		font_name,
		font_collection,
		dwrite_weight,
		dwrite_style,
		DWRITE_FONT_STRETCH_NORMAL,
		static_cast<float>(actual_font_height),
		L"", //locale
		&text_format);

	if (FAILED(hr))
	{
		core::log(__FUNCTION__, std::format("Failed to create font {} - CreateTextFormat failed: {:x}",
		                                  core::utf16_to_utf8(font_name), static_cast<uint32_t>(hr)));
		return nullptr;
	}

	core::log(__FUNCTION__, std::format("Created font {}", core::utf16_to_utf8(font_name)));
	return std::make_shared<font_renderer>(dwrite, font_face, text_format, actual_font_height, underline, strikeout);
}


font_renderer_ptr factories::create_icon_font_face(const int font_height)
{
	font_renderer_ptr result;

	if (!icon_font_installed_family.empty())
	{
		result = create_font_face(icon_font_installed_family.c_str(), font_height);
	}
	else
	{
		ComPtr<IDWriteFontCollection> custom_collection;

		const auto hr = dwrite->CreateCustomFontCollection(
			&font_collection_loader,
			&icon_font_collection_id,
			sizeof(icon_font_collection_id),
			&custom_collection);
		if (SUCCEEDED(hr))
		{
			result = create_font_renderer(dwrite.Get(), custom_collection.Get(), icon_font_name, font_height);
		}
		else
		{
			core::log(__FUNCTION__, std::format("CreateCustomFontCollection failed {:x}", static_cast<uint32_t>(hr)));
		}

		if (!result)
		{
			result = create_font_face(icon_font_name, font_height);
		}
	}

	if (!result)
	{
		result = create_font_face(L"Consolas", font_height);
	}

	return result;
}

font_renderer_ptr factories::create_font_face(const wchar_t* font_name, const int font_height, const int weight,
                                             const bool italic, const bool underline, const bool strikeout,
                                             const bool cell_size) const
{
	return create_font_renderer(dwrite.Get(), font_collection.Get(), font_name, font_height, weight, italic, underline,
	                            strikeout, cell_size);
}

const wchar_t* icon_font_family()
{
	return icon_font_installed_family.empty() ? icon_font_name : icon_font_installed_family.c_str();
}

// The icon font is registered from memory rather than installed, so anything laying out an icon
// glyph has to be given this collection - the system one does not contain it. Registering the loader
// here as well as in register_fonts() is what lets a caller with its own factory work before the app's fonts
// are up; the factory is a shared singleton, so the second registration is the expected no-op. An
// installed family the application named is in the system collection.
HRESULT create_icon_font_collection(IDWriteFactory* dwrite_factory, IDWriteFontCollection** result)
{
	if (!icon_font_installed_family.empty()) return dwrite_factory->GetSystemFontCollection(result);

	const auto registered = dwrite_factory->RegisterFontCollectionLoader(&font_collection_loader);

	if (FAILED(registered) && registered != DWRITE_E_ALREADYREGISTERED)
	{
		return registered;
	}

	return dwrite_factory->CreateCustomFontCollection(&font_collection_loader, &icon_font_collection_id,
	                                                  sizeof(icon_font_collection_id), result);
}

void factories::register_fonts() const
{
	if (dwrite)
	{
		auto hr = dwrite->RegisterFontFileLoader(&font_loader);
		core::assert_true(SUCCEEDED(hr));
		core::log(__FUNCTION__, std::format("RegisterFontFileLoader {:x}", static_cast<uint32_t>(hr)));
		hr = dwrite->RegisterFontCollectionLoader(&font_collection_loader);
		core::log(__FUNCTION__, std::format("RegisterFontCollectionLoader {:x}", static_cast<uint32_t>(hr)));
		core::assert_true(SUCCEEDED(hr));
	}
}

void factories::unregister_fonts() const
{
	if (dwrite)
	{
		dwrite->UnregisterFontFileLoader(&font_loader);
		dwrite->UnregisterFontCollectionLoader(&font_collection_loader);
	}
}

render::glyph_fallback_probe render::probe_glyph_fallback(const std::string_view primary_family,
                                                              const std::string_view fallback_family,
                                                              const char32_t code_point)
{
	render::glyph_fallback_probe result;

	ComPtr<IDWriteFactory> factory;

	if (FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
		reinterpret_cast<IUnknown**>(factory.GetAddressOf()))) || !factory)
	{
		return result;
	}

	ComPtr<IDWriteFontCollection> system_fonts;
	factory->GetSystemFontCollection(system_fonts.GetAddressOf());
	if (!system_fonts) return result;

	const auto make_face = [&system_fonts](const std::string_view family) -> ComPtr<IDWriteFontFace>
	{
		const auto name = core::utf8_to_utf16(family);
		ComPtr<IDWriteFontFace> face;
		uint32_t index = 0;
		BOOL exists = FALSE;

		if (SUCCEEDED(system_fonts->FindFamilyName(name.c_str(), &index, &exists)) && exists)
		{
			ComPtr<IDWriteFontFamily> font_family;
			ComPtr<IDWriteFont> font;

			if (SUCCEEDED(system_fonts->GetFontFamily(index, font_family.GetAddressOf())) &&
				SUCCEEDED(font_family->GetFirstMatchingFont(DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
					DWRITE_FONT_STYLE_NORMAL, font.GetAddressOf())))
			{
				font->CreateFontFace(face.GetAddressOf());
			}
		}

		return face;
	};

	const auto primary = make_face(primary_family);
	const auto fallback = make_face(fallback_family);
	if (!primary || !fallback) return result;

	const auto cp = static_cast<uint32_t>(code_point);
	primary->GetGlyphIndices(&cp, 1, &result.primary_glyph);
	fallback->GetGlyphIndices(&cp, 1, &result.fallback_glyph);

	DWRITE_GLYPH_METRICS from_fallback{};
	result.fallback_metrics_ok = SUCCEEDED(
		fallback->GetDesignGlyphMetrics(&result.fallback_glyph, 1, &from_fallback));

	// The latent render_glyph bug: metrics read from the primary face for a glyph index
	// that belongs to the fallback face describe a different glyph, or fail outright.
	DWRITE_GLYPH_METRICS from_primary{};
	const auto primary_hr = primary->GetDesignGlyphMetrics(&result.fallback_glyph, 1, &from_primary);
	result.primary_metrics_differ = FAILED(primary_hr) ||
		from_primary.advanceWidth != from_fallback.advanceWidth ||
		from_primary.verticalOriginY != from_fallback.verticalOriginY;

	result.available = true;
	return result;
}

font_renderer_ptr factories::font_face(const render::font_face type, const int base_font_size)
{
	// Named faces vary with the context's base size; a custom face already carries its pixel size.
	render::font_desc custom;
	const auto is_custom = render::detail::custom_font_description(type, custom);
	const auto key = is_custom
		                 ? (uint64_t{1} << 63) | static_cast<uint32_t>(type)
		                 : static_cast<uint64_t>(static_cast<uint32_t>(type)) |
		                   static_cast<uint64_t>(static_cast<uint32_t>(base_font_size)) << 32;
	const auto found = font_renderers.find(key);

	if (found != font_renderers.end()) return found->second;

	if (!font_collection) dwrite->GetSystemFontCollection(font_collection.GetAddressOf());
	if (!font_collection)
	{
		// Could not obtain the system font collection yet - do not cache a negative
		// result so this key can be retried once the collection becomes available.
		return nullptr;
	}

	font_renderer_ptr result;

	if (is_custom)
	{
		const auto family_name = custom.family.empty() ? std::string("Calibri") : custom.family;
		const auto family = core::utf8_to_utf16(family_name);
		const auto size = std::max(1, custom.size);
		result = create_font_face(family.c_str(), size, custom.weight, custom.italic, custom.underline,
		                          custom.strikeout, custom.cell_size);

		// Windows 10 has the same glyphs under the older MDL2 name.
		if (!result && family_name == "Segoe Fluent Icons")
		{
			result = create_font_face(L"Segoe MDL2 Assets", size, custom.weight, custom.italic, custom.underline,
			                          custom.strikeout, custom.cell_size);
		}
	}
	else
	{
		switch (type)
		{
		case render::font_face::dialog:
			result = create_font_face(L"Calibri", base_font_size);
			break;
		case render::font_face::code:
			result = create_font_face(L"Consolas", base_font_size * 4 / 5);
			break;
		case render::font_face::icons:
			result = create_icon_font_face(core::mul_div(base_font_size, icon_font_scale_num, icon_font_scale_den));
			break;
		case render::font_face::small_icons:
			result = create_icon_font_face(core::mul_div(base_font_size, icon_font_scale_num * 10, icon_font_scale_den * 16));
			break;
		case render::font_face::title:
			result = create_font_face(L"Calibri", base_font_size * 3 / 2);
			break;
		case render::font_face::mega:
			result = create_font_face(L"Calibri", base_font_size * 9 / 4);
			break;
		default:
			break;
		}
	}

	if (!result)
	{
		// A missing family falls back to the platform's ordinary UI faces, as the text stack does today.
		const auto size = is_custom ? std::max(1, custom.size) : base_font_size;
		const auto weight = is_custom ? custom.weight : 400;
		const auto italic = is_custom && custom.italic;
		const auto underline = is_custom && custom.underline;
		const auto strikeout = is_custom && custom.strikeout;
		result = create_font_face(L"Arial", size, weight, italic, underline, strikeout, is_custom && custom.cell_size);
		if (!result) result = create_font_face(L"Tahoma", size, weight, italic, underline, strikeout, is_custom && custom.cell_size);
	}

	// diffractor#232: cache the outcome for this key - including a null result - so a
	// known-bad key (requested font and all fallbacks unavailable) is never retried.
	// This stops the per-draw software renderer path from repeatedly re-running the
	// font lookups and flooding the log with "font family not found" messages.
	font_renderers[key] = result;
	return result;
}

render::font_cache_probe render::probe_font_cache(const int base_font_size)
{
	render::font_cache_probe result;

	// A private factories instance: font_face only needs the text engine, so this never
	// touches the app's renderer or its cache.
	factories f;

	if (FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
		reinterpret_cast<IUnknown**>(f.dwrite.GetAddressOf()))) || !f.dwrite)
	{
		return result;
	}

	constexpr auto face = render::font_face::dialog;

	const auto first = f.font_face(face, base_font_size);
	if (!first) return result;

	result.entries_after_first = static_cast<int>(f.font_renderers.size());

	// diffractor#232: the repeat must come from the cache, so no family lookup and no log line.
	const auto repeat = f.font_face(face, base_font_size);
	result.same_request_is_cached = repeat == first &&
		static_cast<int>(f.font_renderers.size()) == result.entries_after_first;

	// diffractor#189: the size is part of the key, so a Large Font toggle cannot hand back the
	// old-size face (and with it the glyph cache built at that size).
	const auto resized = f.font_face(face, base_font_size * 2);
	result.size_change_is_distinct = resized && resized != first;

	const auto other_face = f.font_face(render::font_face::code, base_font_size);
	result.face_change_is_distinct = other_face && other_face != first;

	f.reset_fonts();
	result.reset_clears_cache = f.font_renderers.empty();

	result.available = true;
	return result;
}

font_renderer::font_renderer(const ComPtr<IDWriteFactory>& factory, const ComPtr<IDWriteFontFace>& face,
                             ComPtr<IDWriteTextFormat> text_format, const int font_size,
                             const bool underline, const bool strikeout) : _factory(factory),
	_face(face), _text_format(std::move(text_format)), _font_size(font_size), _underline(underline),
	_strikeout(strikeout)
{
	_face->GetMetrics(&_metrics);
}

void font_renderer::configure_decorations(IDWriteTextLayout* const layout, const uint32_t length) const
{
	if (!layout || length == 0) return;
	if (_underline) layout->SetUnderline(TRUE, DWRITE_TEXT_RANGE{0, length});
	if (_strikeout) layout->SetStrikethrough(TRUE, DWRITE_TEXT_RANGE{0, length});
}

uint32_t font_renderer::calc_line_height() const
{
	if (_metrics.designUnitsPerEm == 0)
	{
		return _font_size; // Fallback to font size if metrics are invalid
	}
	return core::mul_div(_metrics.ascent + _metrics.descent + _metrics.lineGap, _font_size, _metrics.designUnitsPerEm);
}

uint32_t font_renderer::calc_base_line_height() const
{
	if (_metrics.designUnitsPerEm == 0)
	{
		return _font_size; // Fallback to font size if metrics are invalid
	}
	return core::mul_div(_metrics.ascent + _metrics.lineGap, _font_size, _metrics.designUnitsPerEm);
}

// https://stackoverflow.com/questions/5995293/get-single-glyph-metrics-net

render_char_result font_renderer::render_glyph(const uint16_t glyph_index, const int spacing,
                                               const DWRITE_GLYPH_RUN* glyph_run) const
{
	render_char_result result{};

	const auto line_height = core::mul_div(_metrics.ascent + _metrics.descent + _metrics.lineGap, _font_size,
	                                     _metrics.designUnitsPerEm);
	const auto base_line_height = core::mul_div(_metrics.ascent, _font_size, _metrics.designUnitsPerEm);

	DWRITE_GLYPH_METRICS glyph_metrics{};

	// Query metrics from the glyph run's OWN face, not the primary font face
	// (_face). For a fallback run (e.g. Hangul rendered via Malgun Gothic) the
	// glyph_index belongs to that fallback face; querying _face here would fail
	// for out-of-range indices (dropping the glyph) or return a different glyph's
	// metrics. The line box below intentionally stays on _face for a consistent
	// baseline across mixed-font text.
	const auto glyph_face = glyph_run->fontFace ? glyph_run->fontFace : _face.Get();

	if (SUCCEEDED(glyph_face->GetDesignGlyphMetrics(&glyph_index, 1, &glyph_metrics)))
	{
		constexpr float glyph_advance = 0;

		DWRITE_GLYPH_OFFSET glyphOffset{};
		glyphOffset.advanceOffset = 0.0f;
		glyphOffset.ascenderOffset = 0.0f;

		DWRITE_GLYPH_RUN run{};
		run.fontFace = glyph_run->fontFace;
		run.fontEmSize = glyph_run->fontEmSize;
		run.glyphCount = 1;
		run.glyphIndices = &glyph_index;
		run.glyphAdvances = &glyph_advance;
		run.glyphOffsets = &glyphOffset;
		run.isSideways = FALSE;
		run.bidiLevel = 0;

		ComPtr<IDWriteGlyphRunAnalysis> analysis;
		if (SUCCEEDED(_factory->CreateGlyphRunAnalysis(
			&run,
			1.0f,
			nullptr,
			DWRITE_RENDERING_MODE_NATURAL,
			DWRITE_MEASURING_MODE_NATURAL,
			0,
			0,
			&analysis)))
		{
			RECT bbox{};

			if (SUCCEEDED(analysis->GetAlphaTextureBounds(DWRITE_TEXTURE_CLEARTYPE_3x1, &bbox)))
			{
				RECT bbox2; // { 0, -char_height, char_width, 0 };
				bbox2.left = -spacing;
				bbox2.right = bbox.right + spacing; // -bbox.left) + spacing;
				bbox2.top = -base_line_height;
				bbox2.bottom = bbox2.top + line_height;

				const auto char_width = bbox2.right - bbox2.left;
				const auto char_top = core::mul_div(glyph_metrics.verticalOriginY, _font_size, _metrics.designUnitsPerEm);
				const auto buffer_len = char_width * line_height * 3;

				if (buffer_len > 0)
				{
					const auto buffer = core::unique_alloc<uint8_t>(buffer_len);

					if (buffer)
					{
						memset(buffer.get(), 0, buffer_len);

						if (SUCCEEDED(
							analysis->CreateAlphaTexture(DWRITE_TEXTURE_CLEARTYPE_3x1, &bbox2, buffer.get(), buffer_len
							)))
						{
							result.pixels.resize(char_width * line_height);
							result.cx = char_width;
							result.cy = line_height;
							result.x = char_top;
							result.glyph = glyph_index;

							memset(result.pixels.data(), 0, char_width * line_height);

							for (auto y = 0; y < line_height; ++y)
							{
								const auto dest = result.pixels.data() + y * char_width;
								auto src = buffer.get() + char_width * 3 * y;

								for (auto x = 0; x < char_width; ++x)
								{
									// Bounds check to prevent buffer overrun
									if (dest + x < result.pixels.data() + result.pixels.size() &&
										src + 2 < buffer.get() + buffer_len)
									{
										dest[x] = (*src + *(src + 1) + *(src + 2)) / 3;
									}
									src += 3;
								}
							}
						}
					}
				}
			}
		}
	}

	return result;
}

static void configure_layout(const ComPtr<IDWriteTextLayout>& layout, const render::text_style& style)
{
	auto word_wrapping = DWRITE_WORD_WRAPPING_NO_WRAP;
	auto text_alignment = DWRITE_TEXT_ALIGNMENT_LEADING;

	switch (style)
	{
	case render::text_style::none: break;
	case render::text_style::single_line: break;
	case render::text_style::single_line_center:
		text_alignment = DWRITE_TEXT_ALIGNMENT_CENTER;
		break;
	case render::text_style::single_line_far:
		text_alignment = DWRITE_TEXT_ALIGNMENT_TRAILING;
		break;
	case render::text_style::multiline:
		word_wrapping = DWRITE_WORD_WRAPPING_WRAP;
		break;
	case render::text_style::multiline_center:
		text_alignment = DWRITE_TEXT_ALIGNMENT_CENTER;
		word_wrapping = DWRITE_WORD_WRAPPING_WRAP;
		break;
	default: ;
	}

	layout->SetWordWrapping(word_wrapping);
	layout->SetTextAlignment(text_alignment);
	layout->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);

	constexpr DWRITE_TRIMMING trimmingOpt = {DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0};
	layout->SetTrimming(&trimmingOpt, nullptr);
}

void text_layout_impl::update(const std::string_view text, const render::text_style text_style)
{
	if (!_renderer)
	{
		_layout.Reset();
		return;
	}

	const auto textw = core::utf8_to_utf16(text);

	for (auto& m : _measured) m.valid = false;
	_measured_next = 0;
	_utf8_offsets.clear();
	_utf16_positions.clear();
	_utf8_offsets.emplace_back(0);
	_utf16_positions.emplace_back(0);

	auto i = text.begin();
	auto utf16_pos = 0u;
	while (i != text.end())
	{
		auto next = i;
		const auto cp = core::pop_utf8_char(next, text.end());
		if (next == i) break;
		utf16_pos += cp > 0xffff ? 2u : 1u;
		i = next;
		_utf8_offsets.emplace_back(static_cast<size_t>(i - text.begin()));
		_utf16_positions.emplace_back(utf16_pos);
	}

	ComPtr<IDWriteTextLayout> layout;
	const auto hr = _renderer->_factory->CreateTextLayout(textw.data(), static_cast<int>(textw.size()),
	                                                      _renderer->_text_format.Get(), 0, 0, &layout);

	if (SUCCEEDED(hr))
	{
		configure_layout(layout, text_style);
		_renderer->configure_decorations(layout.Get(), static_cast<uint32_t>(textw.size()));
		_layout = layout;
	}
	else
	{
		_layout.Reset();
		core::log(__FUNCTION__, std::format("CreateTextLayout failed: {:x}", static_cast<uint32_t>(hr)));
	}
}

core::sizei text_layout_impl::measure_text(const int cx, const int cy)
{
	for (const auto& m : _measured)
	{
		if (m.valid && m.limit.cx == cx && m.limit.cy == cy) return m.extent;
	}

	if (_layout)
	{
		_layout->SetMaxWidth(static_cast<float>(cx));
		_layout->SetMaxHeight(static_cast<float>(cy));

		DWRITE_TEXT_METRICS metrics{};
		const auto hr = _layout->GetMetrics(&metrics);

		if (SUCCEEDED(hr))
		{
			const core::sizei extent{core::round_up(metrics.width), core::round_up(metrics.height)};
			_measured[_measured_next] = {{cx, cy}, extent, true};
			_measured_next = (_measured_next + 1) % std::size(_measured);
			return extent;
		}
	}

	return {};
}

std::vector<int> text_layout_impl::offset_xs(const std::vector<size_t>& utf8_offsets, const int cx, const int cy)
{
	if (!_layout)
	{
		std::vector<int> result;
		result.resize(utf8_offsets.size());
		return result;
	}

	_layout->SetMaxWidth(static_cast<float>(cx));
	_layout->SetMaxHeight(static_cast<float>(cy));

	UINT32 cluster_count = 0;
	auto hr = _layout->GetClusterMetrics(nullptr, 0, &cluster_count);
	if (hr != E_NOT_SUFFICIENT_BUFFER && FAILED(hr)) return {};

	std::vector<DWRITE_CLUSTER_METRICS> dwrite_clusters(cluster_count);
	hr = _layout->GetClusterMetrics(dwrite_clusters.data(), cluster_count, &cluster_count);
	if (FAILED(hr)) return {};

	std::vector<render::text_cluster_metric> clusters;
	clusters.reserve(cluster_count);
	for (const auto& cluster : dwrite_clusters)
	{
		clusters.push_back({cluster.length, cluster.width});
	}

	std::vector<uint32_t> positions;
	positions.reserve(utf8_offsets.size());
	for (const auto offset : utf8_offsets)
	{
		const auto found = std::ranges::lower_bound(_utf8_offsets, offset);
		const auto index = found == _utf8_offsets.end()
			                   ? _utf16_positions.size() - 1
			                   : static_cast<size_t>(found - _utf8_offsets.begin());
		positions.emplace_back(_utf16_positions[index]);
	}

	return render::logical_text_offset_xs(positions, clusters);
}

void font_renderer::draw(render::draw_context* dc, IDWriteTextRenderer* tr, const std::wstring_view text,
                         const core::recti bounds, const render::text_style style, const core::color color,
                         const core::color bg,
                         const std::vector<render::text_highlight_t>& highlights)
{
	if (text.empty() || text.size() > INT_MAX)
	{
		return; // Nothing to draw for invalid input
	}

	ComPtr<IDWriteTextLayout> layout;
	const auto hr = _factory->CreateTextLayout(text.data(), static_cast<int>(text.size()), _text_format.Get(), 0.0f,
	                                           0.0f,
	                                           &layout);

	if (SUCCEEDED(hr))
	{
		configure_layout(layout, style);
		configure_decorations(layout.Get(), static_cast<uint32_t>(text.size()));
		draw(dc, tr, layout.Get(), bounds, color, bg);
	}
}

void font_renderer::draw(render::draw_context* dc, IDWriteTextRenderer* tr, IDWriteTextLayout* layout, const core::recti bounds,
                         const core::color color, const core::color bg)
{
	if (!layout)
	{
		core::log_once(__FUNCTION__, "No text layout is available to draw");
		return;
	}

	layout->SetMaxWidth(static_cast<float>(bounds.width()));
	layout->SetMaxHeight(static_cast<float>(bounds.height()));

	if (bg.a > 0.0f)
	{
		DWRITE_TEXT_METRICS m{};
		const auto hr = layout->GetMetrics(&m);

		if (SUCCEEDED(hr))
		{
			const core::rectd bb{bounds.left + m.left, bounds.top + m.top, m.width, m.height};
			dc->draw_rounded_rect(bb.round().inflate(2), bg, dc->padding1);
		}
	}

	layout->Draw(nullptr, tr,
	             static_cast<float>(bounds.left),
	             static_cast<float>(bounds.top));
}

void font_renderer::trim_layout_cache()
{
	// Drop the least recently used half rather than one entry, so a working set larger than the cap
	// costs one rebuild per eviction round instead of one per lookup.
	std::vector<uint64_t> stamps;
	stamps.reserve(_layouts.size());
	for (const auto& [key, entry] : _layouts) stamps.emplace_back(entry.used);

	const auto middle = stamps.begin() + stamps.size() / 2;
	std::ranges::nth_element(stamps, middle);
	const auto cutoff = *middle;

	std::erase_if(_layouts, [cutoff](const auto& kv) { return kv.second.used <= cutoff; });
}

IDWriteTextLayout* font_renderer::cached_layout(const std::string_view text, const render::text_style style,
                                                const bool for_draw)
{
	core::assert_true(platform::is_ui_thread());

	// The returned pointer is only valid until the next lookup, which may evict it. Nothing reached
	// from layout->Draw draws text, so a layout cannot be evicted while it is being drawn.
	const auto found = _layouts.find(layout_key_view{text, style, for_draw});

	if (found != _layouts.end())
	{
		found->second.used = ++_layout_clock;
		return found->second.layout.Get();
	}

	const auto textw = core::utf8_to_utf16(text);

	if (textw.empty() || textw.size() > INT_MAX)
	{
		return nullptr;
	}

	ComPtr<IDWriteTextLayout> layout;

	if (FAILED(_factory->CreateTextLayout(textw.data(), static_cast<int>(textw.size()), _text_format.Get(), 0.0f, 0.0f,
	                                      &layout)) || !layout)
	{
		return nullptr;
	}

	if (for_draw)
	{
		configure_layout(layout, style);
	}
	else
	{
		// Matches the wstring measure overload: wrapping only, so a measured extent keeps meaning
		// what it did before the cache existed.
		const auto no_wrap = style != render::text_style::multiline &&
			style != render::text_style::multiline_center;
		layout->SetWordWrapping(no_wrap ? DWRITE_WORD_WRAPPING_NO_WRAP : DWRITE_WORD_WRAPPING_WRAP);
	}
	configure_decorations(layout.Get(), static_cast<uint32_t>(textw.size()));

	if (_layouts.size() >= max_cached_layouts)
	{
		trim_layout_cache();
	}

	auto& entry = _layouts[layout_key{std::string(text), style, for_draw}];
	entry.layout = layout;
	entry.used = ++_layout_clock;
	return entry.layout.Get();
}

core::sizei font_renderer::measure(const std::string_view text, const render::text_style style, const int width,
                             int height)
{
	if (height == 0) height = 1000;

	auto* const layout = cached_layout(text, style, false);
	if (!layout) return {};

	layout->SetMaxWidth(static_cast<float>(width));
	layout->SetMaxHeight(static_cast<float>(height));

	DWRITE_TEXT_METRICS metrics{};

	if (FAILED(layout->GetMetrics(&metrics))) return {};

	return {core::round_up(metrics.width), core::round_up(metrics.height)};
}

void font_renderer::draw(render::draw_context* dc, IDWriteTextRenderer* tr, const std::string_view text,
                         const core::recti bounds, const render::text_style style, const core::color color,
                         const core::color bg)
{
	auto* const layout = cached_layout(text, style, true);
	if (layout) draw(dc, tr, layout, bounds, color, bg);
}
