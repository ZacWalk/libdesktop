// This file is part of libdesktop, a C++20 platform layer for desktop applications
// Copyright 2026  Zac Walker
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

// Purpose: platform's umbrella; desktop services and application-facing facilities.

#pragma once

#include "libdesktop/core.h"

namespace platform
{
	using namespace std::literals;
}
#include "libdesktop/platform/environment.h"
#include "libdesktop/platform/services.h"
#include "libdesktop/platform/file.h"
#include "libdesktop/platform/input.h"
#include "libdesktop/platform/app.h"
#include "libdesktop/platform/resources.h"
#include "libdesktop/platform/window.h"
#include "libdesktop/platform/media.h"
#include "libdesktop/platform/tasks.h"
#include "libdesktop/platform/process.h"
#include "libdesktop/platform/image.h"
#include "libdesktop/platform/web.h"
#include "libdesktop/platform/text.h"
#include "libdesktop/platform/shell.h"
#include "libdesktop/platform/dialogs.h"
