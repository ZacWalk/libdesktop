// This file is part of libdesktop, a C++20 platform layer for desktop applications
// Copyright 2026  Zac Walker
// 
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

// Purpose: edit-preview transforms (perspective warp and colour grade) applied by
// the RGB pixel shaders. Packing must match shader_transform_params in render/win/d3d11.cpp.
//
//   tone_curve            256 curve entries at i / 255, packed 4 per register
//   perspective_params    x horizontal, y vertical, z has_perspective, w has_color_changes
//   color_params          x saturation, y vibrance, z red gain, w green gain
//   color_params2         x blue gain
//
// b0 is shared with yuv_params; the host binds whichever buffer matches the bound shader,
// and no shader ever includes both declarations.
cbuffer texture_transform_params : register(b0)
{
	float4 tone_curve[64];
	float4 perspective_params;
	float4 color_params;
	float4 color_params2;
};

bool transform_uv(inout float2 uv)
{
	if (perspective_params.z < 0.5)
		return true;

	const float2 normalized = uv - 0.5;
	const float denominator = 1.0 + dot(perspective_params.xy, normalized);
	if (denominator <= 0.0)
		return false;

	uv = 0.5 + normalized / denominator;
	return all(uv >= 0.0) && all(uv <= 1.0);
}

// The tone curve at a luma, read between its entries as grade_color reads them.
float tone_curve_value(const float luminance)
{
	const float position = saturate(luminance) * 255.0f;
	// Unsigned indexing lets the shift/mask replace a signed divide and modulus (fxc X3556).
	const uint index = min((uint)position, 254u);
	const uint next = index + 1u;
	const float low = tone_curve[index >> 2u][index & 3u];
	const float high = tone_curve[next >> 2u][next & 3u];
	return low + (high - low) * (position - (float)index);
}

// grade_color in render/context.h, line for line: the saved picture and the software renderer are graded by
// that template, and a preview that drifted from it would show the user one picture and save another.
float4 transform_color(float4 color)
{
	if (perspective_params.w < 0.5)
		return color;

	const float saturation = color_params.x;
	const float vibrance = color_params.y;
	float3 rgb = color.rgb * float3(color_params.z, color_params.w, color_params2.x);
	const float y = dot(rgb, float3(0.2126, 0.7152, 0.0722));
	const float luma = tone_curve_value(y);

	if (y > 1e-6)
		rgb *= luma / y;
	else
		rgb += luma - y;

	float factor = saturation;

	if (vibrance != 0.0)
	{
		const float high = max(rgb.r, max(rgb.g, rgb.b));
		const float low = min(rgb.r, min(rgb.g, rgb.b));
		const float chroma = high > 1e-6 ? saturate((high - low) / high) : 0.0;
		const float skin_hue = 0.43633231;
		const float skin_width = 0.31415927;
		const float turn = 6.28318531;
		float from_skin = atan2(0.8660254 * (rgb.g - rgb.b), rgb.r - 0.5 * (rgb.g + rgb.b)) - skin_hue;
		from_skin -= turn * round(from_skin / turn);
		const float skin = chroma > 0.0 ? exp(-(from_skin * from_skin) / (skin_width * skin_width)) : 0.0;
		factor *= 1.0 + vibrance * (1.0 - chroma) * (1.0 - chroma) * (1.0 - 0.7 * skin);
	}

	rgb = luma + (rgb - luma) * factor;

	if (luma >= 1.0)
	{
		rgb = 1.0;
	}
	else if (luma <= 0.0)
	{
		rgb = 0.0;
	}
	else
	{
		float fit = 1.0;

		[unroll]
		for (int i = 0; i < 3; ++i)
		{
			if (rgb[i] > 1.0)
				fit = min(fit, (1.0 - luma) / (rgb[i] - luma));
			else if (rgb[i] < 0.0)
				fit = min(fit, luma / (luma - rgb[i]));
		}

		rgb = luma + (rgb - luma) * fit;
	}

	color.rgb = saturate(rgb);
	return color;
}
