// This file is part of libdesktop, a C++20 platform layer for desktop applications
// Copyright 2026  Zac Walker
// 
// This program is free software; you can redistribute it and / or modify it
// under the terms of the LGPL License either version 2.1 or later.
// License details are available at https://www.gnu.org/licenses/lgpl-2.1.html
// This program is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY

// Purpose: Shared Windows Direct3D, DirectWrite, WIC, and renderer types.

#pragma once

#include "libdesktop/platform/win32.h"

#undef SelectBitmap

#include <d3d11_1.h>
#include <dxgi1_3.h>
#include <wincodec.h>
#include <dwrite.h>
#include <directxmath.h>

struct factories;
using factories_ptr = std::shared_ptr<factories>;
struct ID3D11Device;

namespace render
{
	class software_present_target;
}

constexpr int max_vert_count = 4 * 512;
constexpr int max_index_count = 6 * 512;
constexpr int max_text_len = 2000;

// The scene clear colour is render::scene_clear_shade; the Windows backends spell it unqualified.


bool is_device_loss_error(HRESULT hr);

// The Microsoft Basic Render Driver: WARP presented as a hardware adapter, which is what Windows
// offers when no GPU driver is working. Drawn through, every frame is a full-window CPU redraw.
bool is_software_adapter(uint32_t vendor_id, uint32_t device_id, uint32_t flags);

// Whether a device answering this CheckFormatSupport mask can draw a texture of the format, which for
// NV12 and P010 means sampling views of their planes - holding the format is not enough.
bool can_sample_texture_format(UINT support);

// The memory a GPU allocates textures from. An integrated part shares system memory, and the
// "dedicated" figure it reports is only a carve-out made at boot (128MB on Intel parts).
uint64_t gpu_texture_memory(uint64_t dedicated, uint64_t shared, bool unified_memory);

// The bytes one displayed texture may cost, given that memory (0 when there is no GPU to ask) and
// the machine's physical memory (0 when unknown).
int64_t calc_texture_budget(uint64_t gpu_bytes, int64_t total_phys);

// The adapter the renderer draws with, which a hardware decode device is made on so the two can share
// pictures. Published once the render device exists; a zero LUID while there is none, and on the CPU
// backend.
void publish_render_adapter(LUID luid);
LUID render_adapter();

// The render device itself, for a hardware decoder to decode on when the device carries the video
// interfaces: a decoded picture then reaches the texture the renderer draws in one copy on one device,
// rather than two through a texture shared between devices. Null while there is none, and on the CPU
// backend. Safe from any thread.
void publish_render_device(const ComPtr<ID3D11Device>& device);
ComPtr<ID3D11Device> render_device();

// The adapter with this LUID as Windows lists them now, or null once it is gone - or for a zero LUID.
ComPtr<IDXGIAdapter1> find_adapter_by_luid(LUID luid);

// Private window message posted by a frame when a Direct3D device-loss result is seen, or when a
// hardware window cannot build a Direct3D context at all. It is posted (never sent) so recovery runs
// after the render or resize call stack has unwound and no draw-context or device object is still
// live on the stack.
constexpr UINT WM_RENDER_DEVICE_LOST = WM_APP + 0x3d1;

namespace render
{
	bool probe_d3d11_pixel_snapping();
}
struct char_pos_width
{
	float x = 0.0f;
	float y = 0.0f;
	float cx = 0.0f;
};

struct calc_text_extent_result
{
	std::vector<char_pos_width> pos;
	int cy = 0;
	int cx = 0;
};

struct text_line
{
	int begin = 0;
	int end = 0;
	int pixel_width = 0;
};

// The backend interface is ui's, not Windows'. Imported here so the Windows layer can keep
// spelling it unqualified, which is how it reads in every backend and in the window layer.



render::draw_context_device_ptr d3d11_create_context(const factories_ptr& f, const ComPtr<IDXGISwapChain>& swap_chain,
                                             int base_font_size);
// A texture on the factories' Direct3D device, as every hardware draw context makes them.
render::texture_ptr d3d11_create_texture(const factories_ptr& f);
render::draw_context_device_ptr create_software_draw_context(const factories_ptr& f, HWND hwnd, bool layered,
                                                     int base_font_size);
// One that presents to target rather than to a window: where a test reads back what it drew.
render::draw_context_device_ptr create_software_draw_context(const factories_ptr& f,
                                                     std::shared_ptr<render::software_present_target> target,
                                                     int base_font_size);
constexpr int swap_buffer_count = 2;
constexpr auto back_buffer_format = DXGI_FORMAT_B8G8R8A8_UNORM; // DXGI_FORMAT_B8G8R8A8_UNORM;

// Back buffers are allocated in steps of this many pixels rather than at the exact client size, so
// a window drag or splitter move re-uses the existing buffers instead of reallocating them.
constexpr int back_buffer_quantum = 256;

struct render_char_result
{
	std::vector<uint8_t> pixels;
	int cx = 0;
	int cy = 0;
	int x = 0;
	int glyph = 0;

	bool is_empty() const
	{
		return cx < 1 && cy < 1;
	}
};

class font_renderer
{
public:
	ComPtr<IDWriteFactory> _factory;
	ComPtr<IDWriteFontFace> _face;
	ComPtr<IDWriteTextFormat> _text_format;

	int _font_size = 0;
	bool _underline = false;
	bool _strikeout = false;
	DWRITE_FONT_METRICS _metrics{};


	explicit font_renderer(const ComPtr<IDWriteFactory>& factory, const ComPtr<IDWriteFontFace>& face,
	                       ComPtr<IDWriteTextFormat> text_format, int font_size,
	                       bool underline = false, bool strikeout = false);

	uint32_t calc_line_height() const;
	uint32_t calc_base_line_height() const;
	void configure_decorations(IDWriteTextLayout* layout, uint32_t length) const;

	render_char_result render_glyph(uint16_t glyph_index, int spacing, const DWRITE_GLYPH_RUN* glyph_run) const;
	void draw(render::draw_context*, IDWriteTextRenderer*, std::wstring_view text, core::recti bounds,
	          render::text_style style, core::color color, core::color bg,
	          const std::vector<render::text_highlight_t>& highlights);
	static void draw(render::draw_context*, IDWriteTextRenderer*, IDWriteTextLayout*, core::recti bounds, core::color color,
	                 core::color bg);

	// Same two operations against the layout cache below. Callers hand over UTF-8 because the
	// conversion to UTF-16 is itself per-call work the cache removes.
	core::sizei measure(std::string_view text, render::text_style style, int width, int height);
	void draw(render::draw_context*, IDWriteTextRenderer*, std::string_view text, core::recti bounds,
	          render::text_style style, core::color color, core::color bg);

private:
	// Creating an IDWriteTextLayout runs itemization, font fallback and shaping, and the item grid
	// was paying that for every string of every visible tile on every frame. A layout is bound to
	// this renderer, which already fixes the family and em size, and the only other thing baked into
	// it is the style, so one layout per (text, style) is reusable; the destination rectangle is
	// applied per draw. Draw and measure configure a layout differently, so they do not share one.
	struct layout_key
	{
		std::string text;
		render::text_style style = render::text_style::none;
		bool for_draw = false;
	};

	struct layout_key_view
	{
		std::string_view text;
		render::text_style style = render::text_style::none;
		bool for_draw = false;
	};

	static size_t hash_layout_key(const std::string_view text, const render::text_style style, const bool for_draw)
	{
		auto h = std::hash<std::string_view>{}(text);
		h ^= (static_cast<size_t>(style) * 2 + (for_draw ? 1u : 0u)) * 0x9e3779b97f4a7c15ull;
		return h;
	}

	// Transparent so a lookup can be made from a string_view; copying the caller's text into a key
	// on every hit would put back a per-draw allocation this cache exists to remove.
	struct layout_key_hash
	{
		using is_transparent = void;

		size_t operator()(const layout_key& k) const { return hash_layout_key(k.text, k.style, k.for_draw); }
		size_t operator()(const layout_key_view& k) const { return hash_layout_key(k.text, k.style, k.for_draw); }
	};

	struct layout_key_eq
	{
		using is_transparent = void;

		bool operator()(const layout_key& a, const layout_key& b) const
		{
			return a.for_draw == b.for_draw && a.style == b.style && a.text == b.text;
		}

		bool operator()(const layout_key& a, const layout_key_view& b) const
		{
			return a.for_draw == b.for_draw && a.style == b.style && a.text == b.text;
		}

		bool operator()(const layout_key_view& a, const layout_key& b) const
		{
			return a.for_draw == b.for_draw && a.style == b.style && a.text == b.text;
		}
	};

	struct layout_entry
	{
		ComPtr<IDWriteTextLayout> layout;
		uint64_t used = 0;
	};

	static constexpr size_t max_cached_layouts = 768;

	std::unordered_map<layout_key, layout_entry, layout_key_hash, layout_key_eq> _layouts;
	uint64_t _layout_clock = 0;

	IDWriteTextLayout* cached_layout(std::string_view text, render::text_style style, bool for_draw);
	void trim_layout_cache();
};

using font_renderer_ptr = std::shared_ptr<font_renderer>;

inline bool should_retry_font_renderer(const font_renderer_ptr& font)
{
	return font == nullptr;
}

// A glyph cache must be keyed by the face a glyph index belongs to. Font fallback means one text
// renderer sees several faces, and an index is only meaningful inside its own face, so keying on
// the index alone - or on a proxy such as the face's glyph count - lets one face's glyph resolve
// to another face's raster. The face pointer alone is not a safe key either, because a released
// face can be reallocated at the same address, so each keyed face keeps a reference for as long
// as its id is in use.
//
// The em size is part of the key as well. A face carries no size - the size lives on the glyph
// run - so a renderer that ever sees two sizes (a text layout built before a font-size change,
// drawn after it) would otherwise serve the earlier size's raster for the later size's text and
// mix glyph sizes within a single string.
class glyph_face_keys
{
	std::unordered_map<IDWriteFontFace*, uint32_t> _ids;
	std::vector<ComPtr<IDWriteFontFace>> _faces;

public:
	uint64_t key(IDWriteFontFace* face, const float em_size, const uint16_t glyph_index)
	{
		uint32_t id = 0;

		if (face)
		{
			const auto found = _ids.find(face);

			if (found != _ids.cend())
			{
				id = found->second;
			}
			else
			{
				_faces.emplace_back(face);
				id = static_cast<uint32_t>(_faces.size());
				_ids[face] = id;
			}
		}

		const auto em = std::clamp(std::lround(em_size), 0l, 0xFFFFl);

		return static_cast<uint64_t>(id) << 32 | static_cast<uint64_t>(em) << 16 | glyph_index;
	}

	void clear()
	{
		_ids.clear();
		_faces.clear();
	}
};

class text_layout_impl final : public render::text_layout
{
public:
	explicit text_layout_impl(const font_renderer_ptr& renderer, const render::font_face font) : _renderer(renderer),
		_font(font)
	{
	}

	void update(std::string_view text, render::text_style text_style) override;
	core::sizei measure_text(int cx, int cy) override;
	std::vector<int> offset_xs(const std::vector<size_t>& utf8_offsets, int cx, int cy) override;

	font_renderer_ptr _renderer;
	ComPtr<IDWriteTextLayout> _layout;
	render::font_face _font = render::font_face::dialog;

private:
	// GetMetrics re-shapes the whole string whenever SetMaxWidth or SetMaxHeight dirties the layout, and
	// a flex pass measures against one limit then lays out against another, so both results are kept.
	struct measured_extent
	{
		core::sizei limit;
		core::sizei extent;
		bool valid = false;
	};

	measured_extent _measured[2];
	size_t _measured_next = 0;
	std::vector<size_t> _utf8_offsets;
	std::vector<uint32_t> _utf16_positions;
};

// The views a Direct3D texture is sampled through: the whole picture, or the luma plane of NV12 and
// P010 with the chroma plane beside it. Shared rather than copied, so use_count says how many scenes
// still draw the texture - which is how the texture pool tells a texture nothing will draw again from
// one that a redraw could still replay.
struct texture_views
{
	ComPtr<ID3D11ShaderResourceView> y;
	ComPtr<ID3D11ShaderResourceView> uv;
};

using texture_views_ptr = std::shared_ptr<const texture_views>;

// Released textures of the plain kind - one level, sampled and nothing else - kept for the next texture
// of the same size and format. A scrolling listing makes and drops thumbnails by the hundred, and
// overwriting a texture that already exists costs a fraction of making one: CreateTexture2D, and the
// views after it, on the UI thread. Exact sizes only, because a texture larger than its picture is
// sampled past the picture at its edges. Bounded by bytes, oldest first, and touched only on the UI
// thread.
class d3d11_texture_pool final : core::no_copy
{
public:
	struct entry
	{
		core::sizei dims;
		core::texture_format format = core::texture_format::None;
		ComPtr<ID3D11Texture2D> texture;
		// Null for a texture that was never drawn; it is given views when it next is.
		texture_views_ptr views;
	};

	explicit d3d11_texture_pool(const size_t budget_bytes) : _budget(budget_bytes)
	{
	}

	// Keeps the texture unless it would take more than an eighth of the budget, as a full-size photograph
	// would; the oldest entries make room for it.
	void give(entry e);

	// The oldest entry of this size and format that no scene still draws, or one with no texture.
	entry take(core::sizei dims, core::texture_format format);

	void clear();

	size_t size() const
	{
		return _entries.size();
	}

	size_t bytes() const
	{
		return _bytes;
	}

	static size_t texture_bytes(core::sizei dims, core::texture_format format);

private:
	std::vector<entry> _entries; // oldest first
	size_t _budget = 0;
	size_t _bytes = 0;
};

struct factories
{
	// Once a device exists, the factory that owns its adapter: swap chains must come from it.
	ComPtr<IDXGIFactory1> dxgi;
	ComPtr<IDWriteFactory> dwrite;
	ComPtr<IWICImagingFactory> wic;

	// True when there is no Direct3D 11 hardware device to draw with - none could be created, the
	// default adapter is the Microsoft Basic Render Driver, or the device was lost or could not
	// present - and the app is running with the CPU software rendering backend.
	bool software_mode = false;

	// What the device says it can sample. Read once at init; render::yuv_textures_enabled carries the
	// answer to the decoders, because it also folds in the software backend and the driver-fault latch.
	bool supports_nv12 = false;
	bool supports_p010 = false;

	ComPtr<ID3D11Device> d3d_device;
	ComPtr<ID3D11DeviceContext> d3d_context;
	ComPtr<IDXGIDevice> dxgi_device;

	// Textures released on d3d_device, waiting to be used again. Null while there is no device: the
	// pool goes with the device it belongs to.
	std::unique_ptr<d3d11_texture_pool> texture_pool;

	ComPtr<IDWriteFontCollection> font_collection;

	std::unordered_map<uint64_t, font_renderer_ptr> font_renderers;

	font_renderer_ptr create_font_face(const wchar_t* font_name, int font_height, int weight = 400,
	                                  bool italic = false, bool underline = false, bool strikeout = false,
	                                  bool cell_size = false) const;
	font_renderer_ptr create_icon_font_face(int font_height);
	font_renderer_ptr font_face(render::font_face font, int base_font_size);
	void reset();
	void reset_fonts();

	D3D_FEATURE_LEVEL d3d_feature_level = D3D_FEATURE_LEVEL_1_0_CORE;

	bool init(bool use_gpu);

	// Releases the Direct3D device and switches every draw context created from here on to
	// the CPU software backend. Used when the GPU device is lost at runtime, or when a hardware
	// window cannot present through it.
	void downgrade_to_software();

	void register_fonts() const;
	void unregister_fonts() const;
	void destroy();
};

// The bundled icon font's family name, and its DirectWrite collection. The application registers
// its bytes from memory rather than installing them, so the system font collection does not contain it.
const wchar_t* icon_font_family();
HRESULT create_icon_font_collection(IDWriteFactory* dwrite_factory, IDWriteFontCollection** result);

// Fluent draws its icons smaller within the em than Segoe MDL2 did - across the icons this app
// actually uses, 6.8% shorter and 9.4% narrower - so the em is raised to compensate. 11/10 is the
// most that is safe: it makes the tallest Fluent glyph exactly as tall as the tallest Segoe one
// was, so nothing can overflow a layout box that did not already overflow.
constexpr int icon_font_scale_num = 11;
constexpr int icon_font_scale_den = 10;


using factories_ptr = std::shared_ptr<factories>;
