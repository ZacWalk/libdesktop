// This file is part of libdesktop, a C++20 platform layer for desktop applications
// Copyright 2026  Zac Walker
// 
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

// Purpose: single body for the NV12/P010 pixel shaders. Define USE_BICUBIC before including
// it to get the Catmull-Rom variant; the two shaders differ only in how the luma plane is fetched.
// Define USE_TONE_MAP for the HDR variants, which take the picture through its tone-mapping cube.
// Chroma is read where the stream sites it, a picture drawn small is averaged over each pixel's
// footprint, and the result is dithered to the 8-bit target.

#include "common.hlsli"

// Luma and chroma views onto the same NV12/P010 surface (R8/R16 and R8G8/R16G16).
Texture2D tex_y : register(t0);
Texture2D tex_uv : register(t1);

#ifdef USE_TONE_MAP
// The cube that maps a PQ or HLG signal to the SDR picture, sampled on R'G'B' with red along x and
// blue along z. The CPU backend's ui::tone_map_lut::apply is the same arithmetic, so both draw alike.
Texture3D hdr_lut : register(t2);
// The bilinear state, which is trilinear on a 3D texture. Slot 0 may be the point state.
SamplerState lut_sampler : register(s1);
#endif

// Affine YUV->RGB transform (3x3 matrix + bias column) supplied by the host.
// A single matrix encodes both the colour matrix (BT.601/709/2020) and the signal range (limited 16-235
// vs full 0-255 / JPEG), so one shader handles every case - see compute_yuv_matrix() on the C++ side.
// chroma_shift.xy is where the chroma samples lie, in luma pixels, from where a sampler takes a half-size
// plane's samples to lie: half a pixel across for left-sited chroma (core::chroma_shift_x/y).
// b0 is shared with texture_transform_params; the host binds whichever buffer matches the
// bound shader, and no shader ever includes both declarations.
cbuffer yuv_params : register(b0)
{
	row_major float3x4 yuv_to_rgb_matrix;
	float4 chroma_shift;
};

float4 main(PS_INPUT input) : SV_Target
{
	const float2 duv_dx = ddx(input.uv);
	const float2 duv_dy = ddy(input.uv);

#ifdef USE_BICUBIC
	const float y = beyond_bicubic(input.tex_size, duv_dx, duv_dy)
	                    ? sample_footprint(tex_y, tex_sampler, input.uv, input.tex_size, duv_dx, duv_dy).x
	                    : sample_texture_catmull_rom(tex_y, tex_sampler, input.uv, input.tex_size).x;
#else
	const float y = sample_footprint(tex_y, tex_sampler, input.uv, input.tex_size, duv_dx, duv_dy).x;
#endif

	// Chroma is half resolution on both axes and the eye resolves it far less sharply than
	// luma, so a single bilinear tap is indistinguishable from a 9 tap cubic here - and it
	// keeps the bicubic video path at 10 taps per pixel instead of 18. It is read where the stream
	// sites it, and averaged over the pixel's footprint where the picture is drawn small.
	const float2 chroma_uv = input.uv + chroma_shift.xy / input.tex_size;
	const float2 uv = sample_footprint(tex_uv, tex_sampler, chroma_uv, input.tex_size * 0.5f, duv_dx, duv_dy).xy;

	const float3 rgb = saturate(mul(yuv_to_rgb_matrix, float4(y, uv, 1.0f)));

	// A converted pixel lies between the target's levels, and a 10-bit or tone-mapped one holds more than
	// it can: dithered to the 8-bit target, its gradients do not band.
#ifdef USE_TONE_MAP
	// The cube's samples sit at texel centres, so the signal is scaled onto them: 0 and 1 land on
	// the first and last sample rather than on the cube's outer faces.
	float lut_w, lut_h, lut_d;
	hdr_lut.GetDimensions(lut_w, lut_h, lut_d);
	const float3 lut_uvw = rgb * ((float3(lut_w, lut_h, lut_d) - 1.0f) / float3(lut_w, lut_h, lut_d)) +
		0.5f / float3(lut_w, lut_h, lut_d);
	return dither_color(float4(hdr_lut.SampleLevel(lut_sampler, lut_uvw, 0.0f).rgb, 1.0f) * input.c, input.pos.xy);
#else
	return dither_color(float4(rgb, 1.0f) * input.c, input.pos.xy);
#endif
}
