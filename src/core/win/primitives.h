// This file is part of libdesktop, a C++20 platform layer for desktop applications
// Copyright 2026  Zac Walker
//
// This program is free software; you can redistribute it and / or modify it
// under the terms of the LGPL License either version 2.1 or later.
// License details are available at https://www.gnu.org/licenses/lgpl-2.1.html
// This program is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY

#pragma once

#include <windows.h>

// FILETIME conversion is shared by the core timestamp primitives and the Windows platform
// services. The implementation lives with core so the two libraries do not carry copies.
FILETIME ts_to_ft(uint64_t timestamp);
uint64_t ft_to_ts(const FILETIME& file_time);
