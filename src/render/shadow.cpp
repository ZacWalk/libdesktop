// This file is part of libdesktop, a C++20 platform layer for desktop applications
// Copyright 2026  Zac Walker
//
// This program is free software; you can redistribute it and / or modify it
// under the terms of the LGPL License either version 2.1 or later.
// License details are available at https://www.gnu.org/licenses/lgpl-2.1.html
// This program is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY

// Purpose: Decode the render library's platform-independent drop-shadow artwork.

#include "libdesktop/render.h"
#include "libdesktop/render/software.h"

extern const unsigned char libdesktop_shadow_png[];
extern const std::size_t libdesktop_shadow_png_size;

core::surface_ptr render::load_shadow_art()
{
	return render::decode_embedded({libdesktop_shadow_png, libdesktop_shadow_png_size});
}
