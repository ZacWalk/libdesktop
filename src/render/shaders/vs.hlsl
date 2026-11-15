// This file is part of libdesktop, a C++20 platform layer for desktop applications
// Copyright 2026  Zac Walker
// 
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#include "common.hlsli"

PS_INPUT main(VS_INPUT input)
{
	PS_INPUT output;

	// Client space (0..1, y down) to clip space (-1..1, y up) in a single mad.
	output.pos = float4(input.pos.xy * float2(2.0f, -2.0f) + float2(-1.0f, 1.0f), input.pos.z, input.pos.w);
	output.uv = input.uv;
	output.c = input.c;
	output.tex_size = input.tex_size;
	output.c2 = input.c2;

	return output;
}
