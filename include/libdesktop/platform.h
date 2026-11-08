// This file is part of libdesktop, a C++20 platform layer for desktop applications
// Copyright 2026  Zac Walker
// 
// This program is free software; you can redistribute it and / or modify it
// under the terms of the LGPL License either version 2.1 or later.
// License details are available at https://www.gnu.org/licenses/lgpl-2.1.html
// This program is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY

// Purpose: platform's umbrella; desktop services and application-facing facilities.

#pragma once

#include "libdesktop/core.h"

namespace platform { using namespace std::literals; }
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
