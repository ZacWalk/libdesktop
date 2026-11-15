// This file is part of libdesktop, a C++20 platform layer for desktop applications
// Copyright 2026  Zac Walker
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#pragma once

#include <windows.h>

// FILETIME conversion is shared by the core timestamp primitives and the Windows platform
// services. The implementation lives with core so the two libraries do not carry copies.
FILETIME ts_to_ft(uint64_t timestamp);
uint64_t ft_to_ts(const FILETIME& file_time);
