// This file is part of libdesktop, a C++20 platform layer for desktop applications
// Copyright 2026  Zac Walker
// 
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

// A polygon's gradient, as render::paint::at mixes it for the software renderer: c is the start colour and c2
// the end, each with the coverage of an anti-aliased edge in its alpha; uv is how far along a linear gradient
// the pixel is, in uv.x, or its offset from a radial one's centre in radii, which tex_size.x marks.

#include "common.hlsli"

float4 main(PS_INPUT input) : SV_Target
{
	const float t = saturate(input.tex_size.x > 0.5f ? length(input.uv) : input.uv.x);
	const float a = lerp(input.c.a, input.c2.a, t);
	const float3 premultiplied = lerp(input.c.rgb * input.c.a, input.c2.rgb * input.c2.a, t);
	const float3 rgb = a > 0.0f ? premultiplied / a : input.c.rgb;

	return dither_color(float4(rgb, a), input.pos.xy);
}
