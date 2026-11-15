// This file is part of libdesktop, a C++20 platform layer for desktop applications
// Copyright 2026  Zac Walker
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

// Purpose: Portable embedded-resource registry for the platform layer.

#include "libdesktop/platform/resources.h"

#include <vector>

namespace
{
	std::vector<platform::embedded_resource>& registry()
	{
		static std::vector<platform::embedded_resource> items;
		return items;
	}

	const platform::embedded_resource* find(const std::string_view name)
	{
		for (const auto& item : registry())
			if (item.name == name) return &item;

		return nullptr;
	}
}

void platform::register_embedded_resources(const embedded_resource* items, const size_t count)
{
	auto& all = registry();
	all.insert(all.end(), items, items + count);
}

std::span<const uint8_t> platform::embedded_resource_data(const std::string_view name)
{
	if (const auto* found = find(name)) return {found->data, found->size};

	return {};
}

std::string_view platform::embedded_resource_text(const std::string_view name)
{
	if (const auto* found = find(name)) return {reinterpret_cast<const char*>(found->data), found->size};

	return {};
}
