// This file is part of libdesktop, a C++20 platform layer for desktop applications
// Copyright 2026  Zac Walker
// 
// This program is free software; you can redistribute it and / or modify it
// under the terms of the LGPL License either version 2.1 or later.
// License details are available at https://www.gnu.org/licenses/lgpl-2.1.html
// This program is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY

// Purpose: libdesktop-render's umbrella: the foundation and OS services it is written in
// (libdesktop/system.h), the services its backends implement (platform_render.h), and the desktop
// UI's types, drawing interfaces, frames and controls (ui.h). The module's sources take it as their
// precompiled header, and an application takes it as its own.

#pragma once

#include "libdesktop/system.h"
#include "libdesktop/platform_render.h"
#include "libdesktop/ui.h"
