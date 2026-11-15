// This file is part of libdesktop, a C++20 platform layer for desktop applications
// Copyright 2026  Zac Walker
// 
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

// Purpose: single body for the RGB pixel shaders. Define USE_BICUBIC before including it to
// get the Catmull-Rom variant; the two shaders differ only in how the texel is fetched. A picture
// drawn at less than half its size is averaged over each pixel's footprint, and a colour the 8-bit
// target cannot hold exactly is dithered.

#include "common.hlsli"
#include "texture_transform.hlsli"

Texture2D tx : register(t0);

float4 main(PS_INPUT input) : SV_Target
{
	float2 uv = input.uv;
	const bool inside = transform_uv(uv);
	// Taken before any pixel is discarded: a rate of change needs the pixels beside it.
	const float2 duv_dx = ddx(uv);
	const float2 duv_dy = ddy(uv);
	if (!inside) discard;

#ifdef USE_BICUBIC
	const float4 texel = beyond_bicubic(input.tex_size, duv_dx, duv_dy)
	                         ? sample_footprint(tx, tex_sampler, uv, input.tex_size, duv_dx, duv_dy)
	                         : sample_texture_catmull_rom(tx, tex_sampler, uv, input.tex_size);
#else
	const float4 texel = sample_footprint(tx, tex_sampler, uv, input.tex_size, duv_dx, duv_dy);
#endif

	return dither_inexact(transform_color(texel) * input.c, input.pos.xy);
}
