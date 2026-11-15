// This file is part of libdesktop, a C++20 platform layer for desktop applications
// Copyright 2026  Zac Walker
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

// Purpose: render's umbrella; platform services and the rendering interfaces.

#pragma once

#include "libdesktop/platform.h"

namespace render
{
	using namespace std::literals;
}
#include "libdesktop/render/counters.h"
#include "libdesktop/render/context.h"
#include "libdesktop/render/text.h"
#include "libdesktop/render/probes.h"
#include "libdesktop/render/device.h"
