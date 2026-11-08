// This file is part of libdesktop, a C++20 platform layer for desktop applications
// Copyright 2026  Zac Walker
//
// This program is free software; you can redistribute it and / or modify it
// under the terms of the LGPL License either version 2.1 or later.
// License details are available at https://www.gnu.org/licenses/lgpl-2.1.html
// This program is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY

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
