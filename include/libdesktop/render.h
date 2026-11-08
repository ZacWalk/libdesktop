// This file is part of libdesktop, a C++20 platform layer for desktop applications
// Copyright 2026  Zac Walker
// 
// This program is free software; you can redistribute it and / or modify it
// under the terms of the LGPL License either version 2.1 or later.
// License details are available at https://www.gnu.org/licenses/lgpl-2.1.html
// This program is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY

// Purpose: render's umbrella; platform services and the rendering interfaces.

#pragma once

#include "libdesktop/platform.h"

namespace render { using namespace std::literals; }
#include "libdesktop/render/counters.h"
#include "libdesktop/render/context.h"
#include "libdesktop/render/text.h"
#include "libdesktop/render/probes.h"
#include "libdesktop/render/device.h"
