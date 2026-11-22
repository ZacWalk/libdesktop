// This file is part of libdesktop, a C++20 platform layer for desktop applications
// Copyright 2026  Zac Walker
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

// Purpose: Assess the storage backing a path before scheduling concurrent reads.

#pragma once

#include "libdesktop/core.h"

namespace platform
{
	class storage_probe
	{
		core::hash_map<std::string, bool> _volumes;

	public:
		// True only if a local volume reports no mechanical seek cost. Remote and unreported media return false.
		// Worker-owned: reuse for one pass, so a volume is queried once but a later pass can see device changes.
		bool seek_free(core::folder_path folder);
	};
}
