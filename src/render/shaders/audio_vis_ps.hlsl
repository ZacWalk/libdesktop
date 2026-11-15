// This file is part of libdesktop, a C++20 platform layer for desktop applications
// Copyright 2026  Zac Walker
// 
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#include "common.hlsli"

// The audio visualizer's bars, each a block of coloured glass standing off the wall: a rounded rectangle with a flat,
// glossy face - a sheen down its left side, and deeper toward its ends - whose edges are bevelled round, catching the
// light from the upper left and turning from it on the lower right. It casts a soft shadow down and to the right,
// tinted with its colour as light through coloured glass is, and that shadow is what sets each bar apart from the
// wall and from its neighbours. uv is the pixel's place from the centre of the bar's slot and tex_size the slot's
// half size, both in pixels; the colour is the bar's. The result is the bar over its own shadow.

// Where the shadow falls from the bar and over how far it fades, in pixels: the quads reach past the slot by both.
// How deep it is at the bar's opacity, and how much of the bar's colour it carries.
static const float2 shadow_offset = float2(2.0f, 3.0f);
static const float shadow_fade = 8.0f;
static const float shadow_depth = 0.5f;
static const float shadow_tint = 0.25f;

// How much of its slot's width a bar fills, leaving a gap between neighbours; and, as parts of its half width, how
// far its corners are rounded and how far in its bevel reaches.
static const float block_width = 0.8f;
static const float corner_round = 0.55f;
static const float bevel_reach = 0.5f;

// The light, from the upper left and in front of the screen (y down), and halfway between it and the eye.
static const float3 light = float3(-0.485f, -0.573f, 0.661f);
static const float3 halfway = float3(-0.266f, -0.314f, 0.911f);

// How far p is outside a rectangle of half size h whose corners are rounded by r: negative inside.
float rounded_box(const float2 p, const float2 h, const float r)
{
	const float2 q = abs(p) - h + r;
	return length(max(q, 0.0f)) + min(max(q.x, q.y), 0.0f) - r;
}

// The way out of that rectangle from p: straight out through the nearest side, or from the centre of a corner's
// rounding where p is in one.
float2 outward(const float2 p, const float2 h, const float r)
{
	const float2 q = abs(p) - h + r;
	const float2 away = q.x > 0.0f && q.y > 0.0f ? normalize(q) : (q.x > q.y ? float2(1.0f, 0.0f) : float2(0.0f, 1.0f));
	return away * float2(p.x < 0.0f ? -1.0f : 1.0f, p.y < 0.0f ? -1.0f : 1.0f);
}

float4 main(PS_INPUT input) : SV_Target
{
	const float2 p = input.uv;
	const float2 h = float2(input.tex_size.x * block_width, input.tex_size.y);
	const float r = min(h.x * corner_round, h.y);
	const float distance = rounded_box(p, h, r);

	// The surface: level across the face, rounding over into the sides within the bevel's reach of them.
	const float slope = saturate(1.0f + distance / max(h.x * bevel_reach, 0.75f));
	const float2 tilt = outward(p, h, r) * slope * 0.9f;
	const float3 normal = float3(tilt, sqrt(saturate(1.0f - dot(tilt, tilt))));

	const float diffuse = saturate(dot(normal, light));
	const float glint = pow(saturate(dot(normal, halfway)), 40.0f);
	const float across = p.x / max(h.x, 0.5f);
	const float along = abs(p.y) / max(h.y, 0.5f);
	const float sheen = exp(-pow((across + 0.35f) / 0.3f, 2.0f)) * (1.0f - slope);

	float3 glass = input.c.rgb * (0.55f + 0.6f * diffuse) * (1.0f - 0.15f * along * along);
	glass = saturate(glass + sheen * 0.16f + glint * 0.7f);

	const float cover = saturate(0.5f - distance);
	const float body = cover * saturate(0.3f + 0.7f * input.c.a + glint * 0.4f);

	// (1 - d^2)^3 falls away as a blurred edge does, level where the shadow is deepest and where it ends.
	const float d = max(rounded_box(p - shadow_offset, h, r), 0.0f) / shadow_fade;
	const float fall = saturate(1.0f - d * d);
	const float shade = fall * fall * fall * shadow_depth * input.c.a;

	const float alpha = body + shade * (1.0f - body);
	if (alpha <= 0.0f) discard;

	return float4((glass * body + input.c.rgb * shadow_tint * shade * (1.0f - body)) / alpha, alpha);
}
