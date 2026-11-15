// This file is part of libdesktop, a C++20 platform layer for desktop applications
// Copyright 2026  Zac Walker
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

// Purpose: Application data embedded in the executable by the build system.

#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

namespace platform
{
	// Data files compiled into an executable by libdesktop_add_app(EMBED ...).
	// The pointed-to bytes must outlive the process; generated tables use static storage.
	struct embedded_resource
	{
		std::string_view name;
		const uint8_t* data;
		size_t size;
	};

	void register_embedded_resources(const embedded_resource* items, size_t count);

	// Empty when the name is not registered. The text form is null-terminated by the generator.
	std::span<const uint8_t> embedded_resource_data(std::string_view name);
	std::string_view embedded_resource_text(std::string_view name);
}
