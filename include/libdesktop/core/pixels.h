// This file is part of libdesktop, a C++20 platform layer for desktop applications
// Copyright 2026  Zac Walker
// 
// This program is free software; you can redistribute it and / or modify it
// under the terms of the LGPL License either version 2.1 or later.
// License details are available at https://www.gnu.org/licenses/lgpl-2.1.html
// This program is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY

// Purpose: Pixel formats, surfaces, images, colours, and pixel operations.

#pragma once

namespace core
{
	class context;
	class image;
	class surface;
	using surface_ptr = std::shared_ptr<surface>;
	using const_surface_ptr = std::shared_ptr<const surface>;
	using image_ptr = std::shared_ptr<image>;
	using const_image_ptr = std::shared_ptr<const image>;
	enum class image_format
	{
		Unknown = 0,
		JPEG,
		PNG,
		WEBP,
		JXL,
	};

	enum class texture_format
	{
		None,
		RGB,
		ARGB,
		NV12,
		P010
	};

	// True when one pixel is four contiguous bytes, so a software reader may walk the surface as
	// colour values. NV12 and P010 are planar: their luma plane read as packed pixels is grey and
	// tiles, which is a defect that looks like a bad projection rather than like a wrong format.
	constexpr bool is_packed(const texture_format format) noexcept
	{
		return format == texture_format::RGB || format == texture_format::ARGB;
	}
	// Selects the YUV->RGB conversion applied to NV12/P010 textures. A single value
	// encodes both the colour matrix (BT.601/709/2020) and the signal range: the
	// *_limited variants are the usual video ranges (Y 16-235), rec601_full is
	// JPEG/JFIF (Y 0-255). See compute_yuv_matrix().
	enum class color_space : uint8_t
	{
		rec601_limited,
		rec601_full,
		rec709_limited,
		rec709_full,
		rec2020_limited,
		rec2020_full,
	};

	// Affine YUV->RGB transform: a 3x3 matrix plus a bias column, stored row-major.
	struct yuv_rgb_matrix
	{
		float m[12];
	};

	// Builds the transform for the requested colour space and range, so one matrix handles
	// BT.601/709/2020 and limited/full range. Both draw backends share it: the D3D11 path
	// feeds it to the yuv_params constant buffer, the software path applies it per pixel.
	// The limited-range BT.601 case reproduces the previously hard-coded coefficients.
	inline yuv_rgb_matrix compute_yuv_matrix(const color_space cs, const bool is_p010 = false) noexcept
	{
		double kr, kb;
		bool full;

		switch (cs)
		{
		case color_space::rec709_limited: kr = 0.2126;
			kb = 0.0722;
			full = false;
			break;
		case color_space::rec709_full: kr = 0.2126;
			kb = 0.0722;
			full = true;
			break;
		case color_space::rec2020_limited: kr = 0.2627;
			kb = 0.0593;
			full = false;
			break;
		case color_space::rec2020_full: kr = 0.2627;
			kb = 0.0593;
			full = true;
			break;
		case color_space::rec601_full: kr = 0.299;
			kb = 0.114;
			full = true;
			break;
		case color_space::rec601_limited:
		default: kr = 0.299;
			kb = 0.114;
			full = false;
			break;
		}

		const double kg = 1.0 - kr - kb;
		const double vr = 2.0 * (1.0 - kr);
		const double ug = -2.0 * kb * (1.0 - kb) / kg;
		const double vg = -2.0 * kr * (1.0 - kr) / kg;
		const double ub = 2.0 * (1.0 - kb);

		const double y_scale = full ? 1.0 : (255.0 / 219.0);
		const double y_off = full ? 0.0 : (16.0 / 255.0);
		const double c_scale = full ? 1.0 : (255.0 / 224.0);
		constexpr double c_off = 0.5;

		const double a = y_scale;
		const double ay = -y_scale * y_off;

		yuv_rgb_matrix r{};
		// R = a*Y + (vr*c_scale)*V + bias
		r.m[0] = static_cast<float>(a);
		r.m[1] = 0.0f;
		r.m[2] = static_cast<float>(vr * c_scale);
		r.m[3] = static_cast<float>(ay - vr * c_scale * c_off);
		// G = a*Y + (ug*c_scale)*U + (vg*c_scale)*V + bias
		r.m[4] = static_cast<float>(a);
		r.m[5] = static_cast<float>(ug * c_scale);
		r.m[6] = static_cast<float>(vg * c_scale);
		r.m[7] = static_cast<float>(ay - (ug + vg) * c_scale * c_off);
		// B = a*Y + (ub*c_scale)*U + bias
		r.m[8] = static_cast<float>(a);
		r.m[9] = static_cast<float>(ub * c_scale);
		r.m[10] = 0.0f;
		r.m[11] = static_cast<float>(ay - ub * c_scale * c_off);

		// P010 stores its 10 significant bits at the top of each 16 bit word, so a R16_UNORM /
		// R16G16_UNORM view returns code * 64 / 65535 where the coefficients above expect
		// code / 1023 - every sample arrives 0.096% low. The transform is affine, so the
		// correction is a scale of the three sample columns; the bias column stays put.
		// Callers that already normalise by 65472 must leave is_p010 false.
		if (is_p010)
		{
			constexpr auto p010_scale = static_cast<float>(65535.0 / 65472.0);

			for (auto row = 0; row < 3; ++row)
			{
				r.m[row * 4 + 0] *= p010_scale;
				r.m[row * 4 + 1] *= p010_scale;
				r.m[row * 4 + 2] *= p010_scale;
			}
		}

		return r;
	}

	enum class orientation : uint8_t
	{
		none = 0,
		top_left = 1,
		top_right,
		bottom_right,
		bottom_left,
		left_top,
		right_top,
		right_bottom,
		left_bottom
	};

	constexpr bool flips_xy(const orientation o) noexcept
	{
		return o == orientation::left_top || o == orientation::right_top || o == orientation::right_bottom || o ==
			orientation::left_bottom;
	}

	constexpr bool is_inverted(const orientation o) noexcept
	{
		return o == orientation::top_right || o == orientation::bottom_right || o == orientation::right_top || o ==
			orientation::right_bottom;
	}

	constexpr simple_transform to_simple_transform(const orientation orientation) noexcept
	{
		switch (orientation)
		{
		case orientation::top_left: return core::simple_transform::none;
		case orientation::top_right: return core::simple_transform::flip_h;
		case orientation::bottom_right: return core::simple_transform::rot_180;
		case orientation::bottom_left: return core::simple_transform::flip_v;
		case orientation::left_top: return core::simple_transform::transpose;
		case orientation::right_top: return core::simple_transform::rot_270;
		case orientation::right_bottom: return core::simple_transform::transverse;
		case orientation::left_bottom: return core::simple_transform::rot_90;
		default: break;
		}

		return core::simple_transform::none;
	};

	constexpr simple_transform to_simple_transform_inv(const orientation orientation) noexcept
	{
		switch (orientation)
		{
		case orientation::top_left: return core::simple_transform::none;
		case orientation::top_right: return core::simple_transform::flip_h;
		case orientation::bottom_right: return core::simple_transform::rot_180;
		case orientation::bottom_left: return core::simple_transform::flip_v;
		case orientation::left_top: return core::simple_transform::transpose;
		case orientation::right_top: return core::simple_transform::rot_90;
		case orientation::right_bottom: return core::simple_transform::transverse;
		case orientation::left_bottom: return core::simple_transform::rot_270;
		default: break;
		}

		return core::simple_transform::none;
	};

	sizei scale_dimensions(sizei dims, int limit, bool dont_scale_up = false) noexcept;
	sizei scale_dimensions(sizei dims, sizei limit, bool dont_scale_up = false) noexcept;
	recti scale_dimensions(sizei dims, recti limit, bool dont_scale_up = false) noexcept;

	// Area-average reduction, allocating dst. Handles the packed 32 bit formats and NV12, whose two
	// planes reduce independently. Answers false for anything that is not a pure reduction of one of
	// those, which is the caller's cue to keep using swscale.
	bool area_downscale(const const_surface_ptr& src, surface_ptr& dst, sizei dst_extent);

	// Same, pinned to the SSE2 baseline. Only a test uses this, to prove the wider path agrees on a
	// machine that would otherwise exercise just one of the two.
	bool area_downscale_baseline(const const_surface_ptr& src, surface_ptr& dst, sizei dst_extent);

	// Separable bilinear resize of a packed 32 bit surface, allocating dst. For the cases
	// area_downscale refuses - an enlargement, or one axis growing while the other shrinks. A pure
	// reduction belongs to area_downscale, which is both cheaper and correct for it.
	bool bilinear_resize(const const_surface_ptr& src, surface_ptr& dst, sizei dst_extent);

	////////////////////////////////////////////////////////////////////////////////////
	// Pixels Conversions

	using color32 = uint32_t;

	constexpr color32 rgb(const uint32_t r, const uint32_t g, const uint32_t b) noexcept
	{
		return r | g << 8 | b << 16;
	};

	constexpr color32 rgba(const uint32_t r, const uint32_t g, const uint32_t b, const uint32_t a = 255) noexcept
	{
		return r | g << 8 | b << 16 | a << 24;
	};

	constexpr color32 saturate_rgba(const int r, const int g, const int b, const int a) noexcept
	{
		return core::byte_clamp(r) | core::byte_clamp(g) << 8 | core::byte_clamp(b) << 16 | core::byte_clamp(a) << 24;
	};

	constexpr color32 saturate_rgba(const uint32_t r, const uint32_t g, const uint32_t b, const uint32_t a) noexcept
	{
		return core::byte_clamp(r) | core::byte_clamp(g) << 8 | core::byte_clamp(b) << 16 | core::byte_clamp(a) << 24;
	};

	constexpr color32 saturate_rgba(const float r, const float g, const float b, const float a) noexcept
	{
		const auto rr = static_cast<int>(r * 255.f);
		const auto gg = static_cast<int>(g * 255.f);
		const auto bb = static_cast<int>(b * 255.f);
		const auto aa = static_cast<int>(a * 255.f);
		return saturate_rgba(rr, gg, bb, aa);
	};

	constexpr color32 saturate_rgba(const double r, const double g, const double b, const double a) noexcept
	{
		const auto rr = static_cast<int>(r * 255.0);
		const auto gg = static_cast<int>(g * 255.0);
		const auto bb = static_cast<int>(b * 255.0);
		const auto aa = static_cast<int>(a * 255.0);
		return saturate_rgba(rr, gg, bb, aa);
	};

	constexpr uint32_t get_a(const color32 c) noexcept
	{
		return 0xffu & c >> 24;
	}

	constexpr uint32_t get_r(const color32 c) noexcept
	{
		return 0xffu & c;
	}

	constexpr uint32_t get_g(const color32 c) noexcept
	{
		return 0xffu & c >> 8;
	}

	constexpr uint32_t get_b(const color32 c) noexcept
	{
		return 0xffu & c >> 16;
	}

	// Selects the scalar or explicitly requested SIMD implementation of the BGRA row operations.
	// Automatic dispatch uses the SSE2 baseline and checks core::has_avx2() before entering AVX2 code.
	enum class pixel_backend
	{
		automatic,
		scalar,
		sse2,
		avx2
	};

	bool has_sse2() noexcept;

	void blend_constant_bgra(std::span<uint32_t> pixels, color32 foreground,
	                         pixel_backend backend = pixel_backend::automatic);

	// Fraction is in [0, 256]. An out-of-range value throws std::invalid_argument. Source rows are
	// clipped to the destination length, and destination elements beyond that prefix are untouched.
	void interpolate_bgra(std::span<const uint32_t> first, std::span<const uint32_t> second,
	                      std::span<uint32_t> destination, uint16_t fraction,
	                      pixel_backend backend = pixel_backend::automatic);

	constexpr color32 darken(const color32 c, const float ff) noexcept
	{
		const auto rr = static_cast<float>(get_r(c)) / 255.0f;
		const auto gg = static_cast<float>(get_g(c)) / 255.0f;
		const auto bb = static_cast<float>(get_b(c)) / 255.0f;
		const auto aa = static_cast<float>(get_a(c)) / 255.0f;

		return saturate_rgba(rr - rr * ff, gg - gg * ff, bb - bb * ff, aa);
	}

	constexpr color32 lighten(const color32 c, const float ff) noexcept
	{
		const auto rr = static_cast<float>(get_r(c)) / 255.0f;
		const auto gg = static_cast<float>(get_g(c)) / 255.0f;
		const auto bb = static_cast<float>(get_b(c)) / 255.0f;
		const auto aa = static_cast<float>(get_a(c)) / 255.0f;

		return saturate_rgba(rr + rr * ff, gg + gg * ff, bb + bb * ff, aa);
	}


	constexpr color32 emphasize(const color32 c, const float ff = 0.22f) noexcept
	{
		const bool is_bright = get_b(c) > 0x80 || get_g(c) > 0x80 || get_r(c) > 0x80;
		return is_bright ? darken(c, ff) : lighten(c, ff);
	}

	constexpr color32 abgr(const color32 c, const uint32_t a = 0xFF) noexcept
	{
		return ((c >> 16) & 0xFF) | ((c << 16) & 0xFF0000) | (c & 0x0000FF00) | (static_cast<uint32_t>(a) << 24);
	}

	constexpr color32 bgr(const color32 c) noexcept
	{
		return ((c >> 16) & 0xFF) | ((c << 16) & 0xFF0000) | (c & 0x0000FF00);
	}

	constexpr color32 average(const color32 c1, const color32 c2) noexcept
	{
		return rgba(
			(get_r(c1) + get_r(c2)) / 2,
			(get_g(c1) + get_g(c2)) / 2,
			(get_b(c1) + get_b(c2)) / 2,
			(get_a(c1) + get_a(c2)) / 2);
	}

	constexpr uint32_t ilerp(const uint32_t a, const uint32_t b, const int t)
	{
		if (t < 0) return a;
		if (t > 255) return b;
		const auto tu = static_cast<uint32_t>(t);
		const auto c16 = a * (255u - tu) + b * tu;
		return c16 >> 8;
	}

	constexpr color32 lerp(const color32 c1, const color32 c2, const int t) noexcept
	{
		return saturate_rgba(
			ilerp(get_r(c1), get_r(c2), t),
			ilerp(get_g(c1), get_g(c2), t),
			ilerp(get_b(c1), get_b(c2), t),
			ilerp(get_a(c1), get_a(c2), t));
	}

	inline color32 lerp(const color32 c1, const color32 c2, const float t) noexcept
	{
		return lerp(c1, c2, core::round(t * 255.0f));
	}

	inline color32 lerp(const color32 c1, const color32 c2, const double t) noexcept
	{
		return lerp(c1, c2, core::round(t * 255.0));
	}

	int calc_scale_down_factor(sizei sizeIn, sizei sizeOut) noexcept;

	constexpr int calc_stride(const int cx, const int bytes_per_pixel) noexcept
	{
		return (cx * bytes_per_pixel + 15) & ~15;
	}

	inline double calc_mega_pixels(const double x, const double y) noexcept
	{
		return x * y / core::one_mega_pixel;
	}

	class histogram final : public core::no_copy
	{
	public:
		histogram() noexcept
		{
			clear();
		}

		static constexpr int max_value = 0x100;
		static constexpr int alloc_size = sizeof(int) * max_value;

		histogram& operator=(const histogram& other) noexcept
		{
			if (this == &other) return *this;
			_max = other._max;
			memcpy_s(_r, alloc_size, other._r, alloc_size);
			memcpy_s(_g, alloc_size, other._g, alloc_size);
			memcpy_s(_b, alloc_size, other._b, alloc_size);

			return *this;
		}

		void clear() noexcept
		{
			_max = 0;
			memset(_r, 0, alloc_size);
			memset(_g, 0, alloc_size);
			memset(_b, 0, alloc_size);
		}

		int _r[max_value];
		int _g[max_value];
		int _b[max_value];
		int _max;
	};

	enum class pixel_difference_result
	{
		unknown,
		equal,
		not_equal
	};

	// The colour transform that shows an HDR picture - PQ or HLG, BT.2020 - as the BT.709 SDR picture
	// an SDR display can show, sampled on a cube of R'G'B' signal. It is applied to what the YUV matrix
	// produces: by the GPU as a 3D texture, and by the CPU with the same trilinear arithmetic, so both
	// backends draw the same picture.
	struct tone_map_lut
	{
		int size = 0;
		// size^3 RGBA samples of 16 bits each. Red varies fastest, then green, then blue.
		std::vector<uint16_t> rgba;

		// A row of R'G'B' signal, three 16-bit samples a pixel, through the cube as a trilinear texture
		// sample takes it - 0 and 65535 land on the end samples' centres - into pixels as a packed RGB
		// surface stores them, blue in the low byte. Uses SIMD where the build has it.
		void apply(const uint16_t* rgb48, size_t pixels, color32* out) const;
		// The same arithmetic without SIMD, as other architectures run it; kept callable so the two can
		// be compared.
		void apply_baseline(const uint16_t* rgb48, size_t pixels, color32* out) const;
	};

	using tone_map_lut_ptr = std::shared_ptr<const tone_map_lut>;

	class surface final : public std::enable_shared_from_this<surface>
	{
		sizei _dimensions;
		size_t _stride = 0;
		size_t _size = 0;
		double _time = 0.0;
		std::unique_ptr<uint8_t, core::free_delete> _pixels;
		texture_format _format = texture_format::None;
		core::orientation _orientation = core::orientation::top_left;
		core::color_space _cs = core::color_space::rec601_limited;
		tone_map_lut_ptr _tone_map;

	public:
		surface() noexcept = default;
		~surface() noexcept = default;

		surface(const surface&) noexcept = delete;
		surface& operator=(const surface&) noexcept = delete;
		surface(surface&&) noexcept = default;
		surface& operator=(surface&&) noexcept = default;

		void clear()
		{
			_pixels.reset();
			_dimensions.cx = 0;
			_dimensions.cy = 0;
			_size = 0;
			_cs = core::color_space::rec601_limited;
			_tone_map.reset();
			_format = texture_format::None;
			_time = 0.0;
			_orientation = core::orientation::top_left;
		}

		void make_blank() const
		{
			memset(_pixels.get(), 0, _size);
		}

		size_t size() const
		{
			return _size;
		}

		bool empty() const
		{
			return _pixels == nullptr || _dimensions.cx == 0 || _dimensions.cy == 0;
		}

		const sizei dimensions() const noexcept
		{
			return _dimensions;
		}

		uint32_t width() const noexcept
		{
			return _dimensions.cx;
		}

		uint32_t height() const noexcept
		{
			return _dimensions.cy;
		}

		size_t stride() const noexcept
		{
			return _stride;
		}

		texture_format format() const noexcept
		{
			return _format;
		}

		double time() const noexcept
		{
			return _time;
		}

		const core::orientation orientation() const noexcept
		{
			return _orientation;
		}

		void orientation(const core::orientation ori) noexcept
		{
			_orientation = ori;
		}

		core::color_space color_space() const noexcept
		{
			return _cs;
		}

		void color_space(const core::color_space cs) noexcept
		{
			_cs = cs;
		}

		// Set only on HDR planes, whose YUV matrix produces a signal an SDR display cannot show as it is.
		const tone_map_lut_ptr& tone_map() const noexcept
		{
			return _tone_map;
		}

		void tone_map(tone_map_lut_ptr lut) noexcept
		{
			_tone_map = std::move(lut);
		}

		uint8_t* pixels() noexcept
		{
			return _pixels.get();
		}

		const uint8_t* pixels() const noexcept
		{
			return _pixels.get();
		}

		const uint8_t* pixels_line(const int y) const noexcept
		{
			core::assert_true(y < _dimensions.cy);
			return _pixels.get() + y * _stride;
		}

		uint8_t* pixels_line(const int y) noexcept
		{
			core::assert_true(y < _dimensions.cy);
			return _pixels.get() + y * _stride;
		}

		uint8_t* alloc(const sizei s, const texture_format fmt, const core::orientation ori = orientation::top_left,
		               const double time = 0.0)
		{
			return alloc(s.cx, s.cy, fmt, ori, time);
		}

		static int calc_bytes_per_pixel(texture_format fmt)
		{
			return 4;
		}

		uint8_t* alloc(const int cx, const int cy, const texture_format fmt,
		               const core::orientation ori = orientation::top_left, const double time = 0.0)
		{
			if (cx < 1 || cy < 1)
			{
				core::assert_true(false);
				const auto message = std::format("invalid pixel size {}x{}", cx, cy);
				core::log(__FUNCTION__, message);
				throw app_exception(message);
			}

			// NV12/P010 are planar 4:2:0: a full-resolution luma plane followed by a
			// half-height interleaved chroma plane, so the buffer is 1.5x the luma size.
			// (calc_bytes_per_pixel stays 4 for the RGBA copy/draw helpers.)
			const auto is_yuv = fmt == texture_format::NV12 || fmt == texture_format::P010;
			const auto bytes_per_luma = fmt == texture_format::P010 ? 2 : (fmt == texture_format::NV12 ? 1 : 4);

			_stride = calc_stride(cx, bytes_per_luma);
			const auto size = is_yuv ? (_stride * cy * 3_z / 2_z) : (_stride * cy);

			// Allocated again at the size it already has, a surface keeps its block and is written over: at a
			// video frame's size a fresh block is fresh pages to fault in and clear, every frame.
			if (!_pixels || size != _size) _pixels = core::unique_alloc<uint8_t>(size + 64_z);
			_size = size;
			_dimensions.cx = cx;
			_dimensions.cy = cy;
			_format = fmt;
			_time = time;
			_orientation = ori;
			_tone_map.reset();
			return _pixels.get();
		}

		void copy(const surface& s, const recti r)
		{
			if (s.format() == texture_format::NV12 || s.format() == texture_format::P010 ||
				r.left < 0 || r.top < 0 || r.right > s._dimensions.cx || r.bottom > s._dimensions.cy ||
				r.width() < 1 || r.height() < 1)
			{
				throw app_exception("invalid surface crop"s);
			}

			if (!alloc(r.extent(), s.format(), s.orientation(), s.time())) throw std::bad_alloc();

			const ptrdiff_t stride_out = _stride;
			const ptrdiff_t stride_in = s._stride;
			const auto copy_bytes = r.width() * 4_z;
			const auto* const p = s._pixels.get() + r.left * 4_z + r.top * stride_in;

			for (int y = 0; y < _dimensions.cy; ++y)
			{
				memcpy_s(_pixels.get() + y * stride_out, stride_out, p + y * stride_in, copy_bytes);
			}
		}

		void draw(const surface& s, const pointi location, const recti src) const
		{
			const ptrdiff_t stride_this = _stride;
			const ptrdiff_t stride_src = s._stride;
			const ptrdiff_t copy_bytes_len = src.width() * calc_bytes_per_pixel(_format);

			for (int y = 0; y < src.height(); ++y)
			{
				memcpy_s(
					_pixels.get() + (y + location.y) * stride_this + location.x * 4,
					stride_this,
					s._pixels.get() + (y + src.top) * stride_src + src.left * 4,
					copy_bytes_len);
			}
		}

		void swap_rb();
		void unpremultiply_alpha();

		void set_pixel(const int x, const int y, const color32 c) const
		{
			if (x < 0 || x >= _dimensions.cx || y < 0 || y >= _dimensions.cy) return;
			auto* const line = std::bit_cast<color32*>(_pixels.get() + y * _stride);
			line[x] = c;
		}

		color32 get_pixel(const int x, const int y) const
		{
			if (x < 0 || x >= _dimensions.cx || y < 0 || y >= _dimensions.cy) return 0;
			const auto* const line = std::bit_cast<const color32*>(_pixels.get() + y * _stride);
			return line[x];
		}

		void clear(color32 clr) const;
		// The packed pixels turned or mirrored as the transform names; null for planar surfaces and for
		// none. The result keeps this surface's orientation tag for the caller to restate.
		surface_ptr transform(simple_transform t) const;



		pixel_difference_result pixel_difference(const const_surface_ptr& image) const;
	};


	class image final : public std::enable_shared_from_this<image>
	{
		core::blob _data;
		sizei _dimensions;
		image_format _format = image_format::Unknown;
		mutable core::orientation _orientation = core::orientation::top_left;

	public:
		image() noexcept = default;
		~image() noexcept = default;

		// Images are only ever owned through const_image_ptr; a copy would deep-copy the encoded blob.
		image(const image&) = delete;
		image& operator=(const image&) = delete;
		image(image&&) noexcept = default;
		image& operator=(image&&) noexcept = default;

		image(core::blob&& data, const sizei d, const image_format f, const core::orientation orientation) noexcept :
			_data(std::move(data)), _dimensions(d), _format(f), _orientation(orientation)
		{
		}

		image(const core::cspan data, const sizei d, const image_format f, const core::orientation orientation) noexcept :
			_data(data.begin(), data.end()), _dimensions(d), _format(f), _orientation(orientation)
		{
		}

		friend bool operator==(const image& lhs, const image& rhs)
		{
			return lhs._data == rhs._data
				&& lhs._dimensions == rhs._dimensions;
		}

		friend bool operator!=(const image& lhs, const image& rhs)
		{
			return !(lhs == rhs);
		}

		bool empty() const
		{
			return _format == image_format::Unknown || _data.empty() || _dimensions.cx == 0 || _dimensions.cy == 0;
		}

		const sizei dimensions() const noexcept
		{
			return _dimensions;
		}

		image_format format() const
		{
			return _format;
		}

		uint32_t width() const noexcept
		{
			return _dimensions.cx;
		}

		uint32_t height() const noexcept
		{
			return _dimensions.cy;
		}

		core::orientation orientation() const noexcept
		{
			return _orientation;
		}

		void orientation(const core::orientation ori) const noexcept
		{
			_orientation = ori;
		}

		const core::blob& data() const noexcept
		{
			return _data;
		}

		void clear() noexcept
		{
			_data.clear();
			_dimensions.cx = 0;
			_dimensions.cy = 0;
		}

		std::string_view extension() const
		{
			switch (_format)
			{
			case image_format::JPEG: return "jpg";
			case image_format::PNG: return "png";
			case image_format::WEBP: return "webp";
			case image_format::JXL: return "jxl";
			case image_format::Unknown: break;
			default: break;
			}

			core::assert_true(false); // Unknown extension
			return "x";
		}
	};

	inline bool is_empty(const const_surface_ptr& s)
	{
		return s == nullptr || s->empty();
	}

	inline bool is_valid(const const_surface_ptr& s)
	{
		return !is_empty(s);
	}

	inline bool is_empty(const const_image_ptr& i)
	{
		return i == nullptr || i->empty();
	}

	inline bool is_valid(const const_image_ptr& s)
	{
		return !is_empty(s);
	}

	inline bool is_jpeg(const const_image_ptr& i)
	{
		return i != nullptr && i->format() == image_format::JPEG;
	}


	class color
	{
	public:
		constexpr static float color_epsilon = 1.0f / 255.0f;

		float r = 0.0f;
		float g = 0.0f;
		float b = 0.0f;
		float a = 0.0f;

		color() = default;

		explicit color(const color32 rgb) :
			r(get_r(rgb) / 255.0f),
			g(get_g(rgb) / 255.0f),
			b(get_b(rgb) / 255.0f),
			a(get_a(rgb) / 255.0f)
		{
		}

		color(const color32 rgb, const float a) :
			r(get_r(rgb) / 255.0f),
			g(get_g(rgb) / 255.0f),
			b(get_b(rgb) / 255.0f),
			a(a)
		{
			core::assert_true(a < 1.1f);
		}

		color(const float xx, const float yy, const float zz, const float ww) :
			r(xx),
			g(yy),
			b(zz),
			a(ww)
		{
		}

		color inverse() const
		{
			return {1.0f - r, 1.0f - g, 1.0f - b, a};
		}

		color a_min(const float aa) const
		{
			return {r, g, b, std::min(a, aa)};
		}

		color32 rgb() const
		{
			return saturate_rgba(r, g, b, 0.0f);
		}

		color32 rgba() const
		{
			return saturate_rgba(r, g, b, a);
		}

		color operator-(const color other) const
		{
			return {r - other.r, g - other.g, b - other.b, a - other.a};
		}

		float abs_sum() const
		{
			return std::abs(r) +
				std::abs(g) +
				std::abs(b) +
				std::abs(a);
		}

		color& operator+=(const color other)
		{
			r += other.r;
			g += other.g;
			b += other.b;
			a += other.a;
			return *this;
		}

		color operator*(const float x) const
		{
			return color(r * x, g * x, b * x, a * x);
		}

		friend bool operator==(const color lhs, const color rhs)
		{
			return core::equiv(lhs.r, rhs.r)
				&& core::equiv(lhs.g, rhs.g)
				&& core::equiv(lhs.b, rhs.b)
				&& core::equiv(lhs.a, rhs.a);
		}

		friend bool operator!=(const color lhs, const color rhs)
		{
			return !(lhs == rhs);
		}

		void merge(const color other)
		{
			const auto src_a = a;
			const auto dst_a = other.a;
			const auto dst_contribution = (1.0f - src_a) * dst_a;
			const auto out_a = src_a + dst_contribution;

			if (out_a <= 0.0f)
			{
				r = g = b = a = 0.0f;
				return;
			}

			r = (r * src_a + other.r * dst_contribution) / out_a;
			g = (g * src_a + other.g * dst_contribution) / out_a;
			b = (b * src_a + other.b * dst_contribution) / out_a;
			a = out_a;
		}

		static constexpr float sat_f(const float x)
		{
			return std::clamp(x, 0.0f, 1.0f);
		}

		color scale(const float x) const
		{
			return {sat_f(r * x), sat_f(g * x), sat_f(b * x), a};
		}

		color average(const color other) const
		{
			return {
				(r + other.r) / 2.0f,
				(g + other.g) / 2.0f,
				(b + other.b) / 2.0f,
				(a + other.a) / 2.0f
			};
		}

		static float femphasize(const float f)
		{
			return (f - 0.5f) * 0.9f + 0.5f;
		}

		color emphasize() const
		{
			return color(femphasize(r), femphasize(g), femphasize(b), a);
		}

		color emphasize(const bool can_emphasize) const
		{
			return can_emphasize ? color(femphasize(r), femphasize(g), femphasize(b), a) : *this;
		}

		static color from_a(const float a)
		{
			core::assert_true(a < 1.1f);
			return {1.0f, 1.0f, 1.0f, a};
		}

		color aa(const float a) const
		{
			core::assert_true(a < 1.1f);
			return {r, g, b, a};
		}
	};

	// Convenience overload for callers that keep colours in the draw layer's floating-point form.
	void blend_constant_bgra(std::span<uint32_t> pixels, const color& foreground,
	                         pixel_backend backend = pixel_backend::automatic);

	inline bool is_alpha_zero(const float x)
	{
		return fabs(x) <= color::color_epsilon;
	}
	constexpr std::string_view to_string(const texture_format format)
	{
		switch (format)
		{
		case texture_format::ARGB: return "ARGB";
		case texture_format::P010: return "p010";
		case texture_format::NV12: return "NV12";
		case texture_format::RGB: return "RGB";
		default: break;
		}

		return "?";
	}
}
