// This file is part of libdesktop, a C++20 platform layer for desktop applications
// Copyright 2026  Zac Walker
// 
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#include "common.hlsli"

float4 main(PS_INPUT input) : SV_Target
{
	float4 cc = dither_color(input.c, input.pos.xy);

	const float d = length(input.uv * 0.6f);
	// fwidth() is abs(ddx) + abs(ddy), a cheaper approximation of the gradient magnitude;
	// length() of the same pair is exact and keeps the falloff even on diagonal edges.
	// max() keeps the interior quads (where d is constant) out of a divide by zero.
	const float pwidth = max(length(float2(ddx(d), ddy(d))), 1e-6f);
	// A 1.5 pixel falloff reads as a really smooth circle; a 1 pixel linear falloff can still look minorly aliased.
	cc.a *= smoothstep(0.0f, 1.5f, (0.5f - d) / pwidth);

	return cc;
}
