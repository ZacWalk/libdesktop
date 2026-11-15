// This file is part of libdesktop, a C++20 platform layer for desktop applications
// Copyright 2026  Zac Walker
// 
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#include "common.hlsli"

Texture2D tx : register(t0);

float4 main(PS_INPUT input) : SV_Target
{
	// The glyph atlas is A8 - only the alpha channel carries coverage.
	const float coverage = tx.Sample(tex_sampler, input.uv).a;
	return float4(input.c.rgb, coverage * input.c.a);
}
