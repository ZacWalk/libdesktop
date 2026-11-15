// This file is part of libdesktop, a C++20 platform layer for desktop applications
// Copyright 2026  Zac Walker
// 
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

// Purpose: types, sampler, filtering and dithering helpers shared by every shader here.

struct VS_INPUT
{
	float4 pos : POSITION;
	float2 uv : TEXCOORD0;
	float4 c : COLOR;
	float2 tex_size : EXTENT;
	float4 c2 : COLOR1;
};

struct PS_INPUT
{
	float4 pos : SV_POSITION;
	float2 uv : TEXCOORD0;
	float4 c : COLOR;
	// Every quad the app emits carries the same texture extent on all four vertices, so
	// interpolating it per pixel only burns rate - the flat value is exact.
	nointerpolation float2 tex_size : EXTENT;
	// A polygon's gradient's end colour; the polygon shader alone reads it.
	float4 c2 : COLOR1;
};

// Slot 0 is bound to either the point or the bilinear state (both clamp on every axis);
// the bicubic paths require the bilinear state because they merge taps in pairs.
SamplerState tex_sampler : register(s0);

// Interleaved gradient noise (Jimenez). Better distributed than the classic sin() hash and
// it does not rely on transcendental precision, which varies wildly between GPUs.
float dither_noise(const float2 screen_pos)
{
	return frac(52.9829189f * frac(dot(screen_pos, float2(0.06711056f, 0.00583715f))));
}

// Breaks up banding in flat and gradient fills. The noise is centred on zero and one LSB
// wide, so it dithers without shifting the colour - the previous hash only ever added
// light (0 .. 1/100 = 2.5 LSB), which visibly lifted every solid fill and rounded rect.
float4 dither_color(const float4 c, const float2 screen_pos)
{
	const float n = (dither_noise(screen_pos) - 0.5f) * (1.0f / 255.0f);
	return float4(c.rgb + n, c.a);
}

// Dithers a picture's colour where the 8-bit target cannot hold it - blended between texels, graded, or
// read from a 16-bit texture - so its gradients do not band, and leaves one it holds exactly as it is: an
// 8-bit texel at one to one is the same pixel with or without the GPU.
float4 dither_inexact(const float4 c, const float2 screen_pos)
{
	const float3 levels = c.rgb * 255.0f;
	return any(abs(levels - round(levels)) > 0.001f) ? dither_color(c, screen_pos) : c;
}

// The most bilinear taps a side sample_footprint takes: every texel counts down to a twelfth of a
// texture's size, and past that the taps spread.
static const float max_footprint_taps = 6.0f;

// A texture read over the footprint of the pixel rather than at its centre. A picture drawn at less than
// half its size, read at one point, skips texels and shimmers as it moves; this spreads a grid of bilinear
// taps across the texels the pixel covers, each blending a block of two by two, so every one of them
// counts - an area average, as the CPU reduces a picture. A footprint of two texels a side or fewer is one
// tap at the pixel's centre, as the sampler alone reads it - point sampling's exact texel at one to one.
// duv_dx and duv_dy are the coordinates' rate of change across the screen, taken before any branch or
// discard; a texture with mips reads each tap from the level its share of the footprint matches.
float4 sample_footprint(in Texture2D<float4> tex, in SamplerState samp, in float2 uv, in float2 tex_size,
                        in float2 duv_dx, in float2 duv_dy)
{
	const float2 footprint = (abs(duv_dx) + abs(duv_dy)) * tex_size;
	// The rate of change is exact only to a few units in the last place, which must not tip a footprint of
	// exactly two texels a tap into another tap and so quad by quad into a different filter.
	const float2 taps = clamp(ceil(footprint * 0.5f - 0.01f), 1.0f, max_footprint_taps);
	const float2 spacing = footprint / (taps * tex_size);
	const float2 first = uv - spacing * (taps - 1.0f) * 0.5f;
	const float shrink = 1.0f / max(taps.x, taps.y);
	float4 sum = 0.0f;

	[loop] for (float y = 0.0f; y < taps.y; y += 1.0f)
	{
		[loop] for (float x = 0.0f; x < taps.x; x += 1.0f)
		{
			sum += tex.SampleGrad(samp, first + spacing * float2(x, y), duv_dx * shrink, duv_dy * shrink);
		}
	}

	return sum / (taps.x * taps.y);
}

// Catmull-Rom's four taps a side cover two texels a side of a picture drawn at half its size, and fewer
// below it: past that a bicubic draw is averaged over its footprint instead.
bool beyond_bicubic(in float2 tex_size, in float2 duv_dx, in float2 duv_dy)
{
	const float2 footprint = (abs(duv_dx) + abs(duv_dy)) * tex_size;
	return max(footprint.x, footprint.y) > 2.0f;
}

// Samples a texture with Catmull-Rom filtering, using 9 texture fetches instead of 16.
// See http://vec3.ca/bicubic-filtering-in-fewer-taps/ for more details
float4 sample_texture_catmull_rom(in Texture2D<float4> tex, in SamplerState samp, in float2 uv, in float2 tex_size)
{
	// We're going to sample a a 4x4 grid of texels surrounding the target UV coordinate. We'll do this by rounding
	// down the sample location to get the exact center of our "starting" texel. The starting texel will be at
	// location [1, 1] in the grid, where [0, 0] is the top left corner.
	const float2 samplePos = uv * tex_size;
	const float2 texPos1 = floor(samplePos - 0.5f) + 0.5f;

	// Compute the fractional offset from our starting texel to our original sample location, which we'll
	// feed into the Catmull-Rom spline function to get our filter weights.
	const float2 f = samplePos - texPos1;

	// Compute the Catmull-Rom weights using the fractional offset that we calculated earlier.
	// These equations are pre-expanded based on our knowledge of where the texels will be located,
	// which lets us avoid having to evaluate a piece-wise function.
	const float2 w0 = f * (-0.5 + f * (1.0 - 0.5 * f));
	const float2 w1 = 1.0 + f * f * (1.5 * f - 2.5);
	const float2 w2 = f * (0.5 + f * (2.0 - 1.5 * f));
	const float2 w3 = f * f * (-0.5 + 0.5 * f);

	// Work out weighting factors and sampling offsets that will let us use bilinear filtering to
	// simultaneously evaluate the middle 2 samples from the 4x4 grid.
	const float2 w12 = w1 + w2;
	const float2 offset12 = w2 / w12;

	// Compute the final UV coordinates we'll use for sampling the texture
	const float2 inv_tex_size = 1.0f / tex_size;
	const float2 texPos0 = (texPos1 - 1.0f) * inv_tex_size;
	const float2 texPos3 = (texPos1 + 2.0f) * inv_tex_size;
	const float2 texPos12 = (texPos1 + offset12) * inv_tex_size;

	// Textures are single-mip, so LOD is always 0; SampleLevel avoids the implicit
	// gradient computation that Sample would perform for each of the 9 taps.
	float4 result = 0.0f;
	result += tex.SampleLevel(samp, float2(texPos0.x, texPos0.y), 0.0f) * w0.x * w0.y;
	result += tex.SampleLevel(samp, float2(texPos12.x, texPos0.y), 0.0f) * w12.x * w0.y;
	result += tex.SampleLevel(samp, float2(texPos3.x, texPos0.y), 0.0f) * w3.x * w0.y;

	result += tex.SampleLevel(samp, float2(texPos0.x, texPos12.y), 0.0f) * w0.x * w12.y;
	result += tex.SampleLevel(samp, float2(texPos12.x, texPos12.y), 0.0f) * w12.x * w12.y;
	result += tex.SampleLevel(samp, float2(texPos3.x, texPos12.y), 0.0f) * w3.x * w12.y;

	result += tex.SampleLevel(samp, float2(texPos0.x, texPos3.y), 0.0f) * w0.x * w3.y;
	result += tex.SampleLevel(samp, float2(texPos12.x, texPos3.y), 0.0f) * w12.x * w3.y;
	result += tex.SampleLevel(samp, float2(texPos3.x, texPos3.y), 0.0f) * w3.x * w3.y;

	// The outer Catmull-Rom lobes are negative, so a hard edge can ring past the source
	// range. Every texture sampled through here is UNORM, so clipping the overshoot back
	// into [0,1] removes dark/bright haloes (and negative alpha) without dulling the edge.
	return saturate(result);
}
