// This file is part of libdesktop, a C++20 platform layer for desktop applications
// Copyright 2026  Zac Walker
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

// Purpose: The photo editor's colour adjustment: a monotone tone curve built from the darks, midtones,
// lights, contrast and brightness controls, white balance gains, saturation and vibrance - graded on the CPU
// through grade_color at a byte or sixteen bits a channel, and handed to the previews as a texture_transform.

#include "libdesktop/render.h"

namespace
{
	// Fritsch-Carlson monotone cubic through knots that rise strictly. Unlike a natural spline it never
	// overshoots a knot, so the curve can neither fold back on itself nor run past black or white between
	// knots - which a natural spline does, and the clamp after it then flattens into clipped tones.
	class monotone_curve final
	{
		static constexpr int knots = 5;

		std::array<double, knots> _x{};
		std::array<double, knots> _y{};
		std::array<double, knots> _slope{};

		// A one-sided three-point estimate at an end, held to the monotone region.
		static double end_slope(const double h0, const double h1, const double d0, const double d1)
		{
			const auto slope = ((2.0 * h0 + h1) * d0 - h0 * d1) / (h0 + h1);
			if (slope * d0 <= 0.0) return 0.0;
			if (d0 * d1 <= 0.0 && std::abs(slope) > std::abs(3.0 * d0)) return 3.0 * d0;
			return slope;
		}

	public:
		monotone_curve(const std::array<double, knots>& x, const std::array<double, knots>& y) : _x(x), _y(y)
		{
			std::array<double, knots - 1> h{};
			std::array<double, knots - 1> d{};

			for (auto i = 0; i < knots - 1; ++i)
			{
				h[i] = x[i + 1] - x[i];
				d[i] = (y[i + 1] - y[i]) / h[i];
			}

			for (auto i = 1; i < knots - 1; ++i)
			{
				if (d[i - 1] * d[i] <= 0.0) continue;

				const auto w1 = 2.0 * h[i] + h[i - 1];
				const auto w2 = h[i] + 2.0 * h[i - 1];
				_slope[i] = (w1 + w2) / (w1 / d[i - 1] + w2 / d[i]);
			}

			_slope[0] = end_slope(h[0], h[1], d[0], d[1]);
			_slope[knots - 1] = end_slope(h[knots - 2], h[knots - 3], d[knots - 2], d[knots - 3]);
		}

		double operator()(const double x) const
		{
			if (x <= _x[0]) return _y[0];
			if (x >= _x[knots - 1]) return _y[knots - 1];

			auto k = 0;
			while (x > _x[k + 1]) ++k;

			const auto h = _x[k + 1] - _x[k];
			const auto t = (x - _x[k]) / h;
			const auto t2 = t * t;
			const auto t3 = t2 * t;

			return (2.0 * t3 - 3.0 * t2 + 1.0) * _y[k] + (t3 - 2.0 * t2 + t) * h * _slope[k] +
			       (-2.0 * t3 + 3.0 * t2) * _y[k + 1] + (t3 - t2) * h * _slope[k + 1];
		}
	};

	// The steepness k of the tanh S-curve whose slope at its pivot is c, for c > 1: k / tanh(k) = c.
	double contrast_steepness(const double c)
	{
		auto low = 0.0;
		auto high = 40.0;

		for (auto i = 0; i < 64; ++i)
		{
			const auto k = (low + high) * 0.5;
			(k / std::tanh(k) < c ? low : high) = k;
		}

		return (low + high) * 0.5;
	}

	double to_unit(const uint16_t v)
	{
		return v / 65535.0;
	}

	uint16_t from_unit16(const double v)
	{
		return static_cast<uint16_t>(std::clamp(core::round(v * 65535.0), 0, 65535));
	}

	uint8_t from_unit8(const double v)
	{
		return static_cast<uint8_t>(core::byte_clamp(core::round(v * 255.0)));
	}
}

render::color_adjust::color_adjust() noexcept
{
	for (auto i = 0; i < curve_len; ++i)
	{
		_curve[i] = i / static_cast<double>(curve_len - 1);
	}
}

void render::color_adjust::color_params(double vibrance, double saturation, double darks, double midtones,
                                        double lights, double contrast, double brightness, double temperature,
                                        double tint)
{
	for (auto* const control :
	     {&vibrance, &saturation, &darks, &midtones, &lights, &contrast, &brightness, &temperature, &tint})
	{
		*control = std::clamp(*control, -1.0, 1.0);
	}

	_saturation = 1.0 + saturation;
	_vibrance = vibrance;

	// Temperature runs from blue to amber and tint from green to magenta. The gains are scaled together so a
	// neutral keeps its luma: white balance moves the colour, not the exposure, and pushes no highlight past
	// white that grade_color's fit would then have to pull back.
	const auto red = 1.0 + 0.2 * temperature + 0.1 * tint;
	const auto green = 1.0 - 0.2 * tint;
	const auto blue = 1.0 - 0.2 * temperature + 0.1 * tint;
	const auto neutral = 1.0 / (0.2126 * red + 0.7152 * green + 0.0722 * blue);
	_red_gain = red * neutral;
	_green_gain = green * neutral;
	_blue_gain = blue * neutral;

	// Darks, midtones and lights move the curve at a fifth, a half and four fifths by up to a tenth, so
	// the knots always rise. Contrast is an S-curve about the middle grey whose slope there is 2^contrast,
	// rolling off into both ends rather than clipping them, the negative setting the positive's inverse.
	// Brightness bends the midtones by t(1 + a) / (1 + at), which holds black and white where they are and
	// is undone by the opposite setting.
	const monotone_curve shape({0.0, 0.2, 0.5, 0.8, 1.0},
	                           {0.0, 0.2 + darks * 0.1, 0.5 + midtones * 0.1, 0.8 + lights * 0.1, 1.0});
	const auto steepness = std::abs(contrast) > 1e-9 ? contrast_steepness(std::exp2(std::abs(contrast))) : 0.0;
	const auto steepness_tanh = std::tanh(steepness);
	const auto lift = std::exp(1.2 * brightness) - 1.0;

	for (auto i = 0; i < curve_len; ++i)
	{
		auto v = shape(i / static_cast<double>(curve_len - 1));

		if (steepness > 0.0)
		{
			v = contrast > 0.0 ? 0.5 + 0.5 * std::tanh(steepness * (2.0 * v - 1.0)) / steepness_tanh
			                   : 0.5 + 0.5 * std::atanh((2.0 * v - 1.0) * steepness_tanh) / steepness;
		}

		v = v * (1.0 + lift) / (1.0 + lift * v);
		_curve[i] = std::clamp(v, 0.0, 1.0);
	}
}

double render::color_adjust::tone(const double x) const noexcept
{
	const auto position = std::clamp(x, 0.0, 1.0) * (curve_len - 1);
	const auto index = std::min(static_cast<int>(position), curve_len - 2);
	return _curve[index] + (_curve[index + 1] - _curve[index]) * (position - index);
}

bool render::color_adjust::apply(const core::const_surface_ptr& src, uint8_t* const dst, const size_t dst_stride,
                                 const core::cancel_token& token) const
{
	const auto format = src->format();
	const auto wide = core::is_wide(format);

	if (!wide && !core::is_packed(format)) return false;

	const auto dims = src->dimensions();

	for (auto yy = 0; yy < dims.cy; ++yy)
	{
		if (wide)
		{
			// Red first, sixteen bits a channel.
			const auto* s = std::bit_cast<const uint16_t*>(src->pixels() + yy * src->stride());
			auto* d = std::bit_cast<uint16_t*>(dst + yy * dst_stride);

			for (auto xx = 0; xx < dims.cx; ++xx, s += 4, d += 4)
			{
				auto r = to_unit(s[0]);
				auto g = to_unit(s[1]);
				auto b = to_unit(s[2]);
				grade_color(r, g, b, _curve.data(), curve_len, _saturation, _vibrance, _red_gain, _green_gain,
				            _blue_gain);
				d[0] = from_unit16(r);
				d[1] = from_unit16(g);
				d[2] = from_unit16(b);
				d[3] = s[3];
			}
		}
		else
		{
			// Blue first, a byte a channel.
			const auto* s = src->pixels() + yy * src->stride();
			auto* d = dst + yy * dst_stride;

			for (auto xx = 0; xx < dims.cx; ++xx, s += 4, d += 4)
			{
				auto r = s[2] / 255.0;
				auto g = s[1] / 255.0;
				auto b = s[0] / 255.0;
				grade_color(r, g, b, _curve.data(), curve_len, _saturation, _vibrance, _red_gain, _green_gain,
				            _blue_gain);
				// Rounded, not truncated: truncation biases every edited pixel half a level darker.
				d[0] = from_unit8(b);
				d[1] = from_unit8(g);
				d[2] = from_unit8(r);
				d[3] = s[3];
			}
		}

		if (token.is_cancelled())
		{
			return false;
		}
	}

	return true;
}

void render::color_adjust::populate_texture_transform(texture_transform& transform) const
{
	for (auto index = 0; index < texture_transform::curve_len; ++index)
	{
		transform.curve[index] = static_cast<float>(
			tone(index / static_cast<double>(texture_transform::curve_len - 1)));
	}

	transform.saturation = static_cast<float>(_saturation);
	transform.vibrance = static_cast<float>(_vibrance);
	transform.red_gain = static_cast<float>(_red_gain);
	transform.green_gain = static_cast<float>(_green_gain);
	transform.blue_gain = static_cast<float>(_blue_gain);
	transform.has_color_changes = true;
}
