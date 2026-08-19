// platform_common.cpp — backend-independent parts of the platform layer.
// Must NOT include OS headers; this file compiles on every target.

#include "platform.h"

#include <vector>

namespace
{
	std::vector<pf::embedded_resource>& registry()
	{
		static std::vector<pf::embedded_resource> items;
		return items;
	}

	const pf::embedded_resource* find(const std::string_view name)
	{
		for (const auto& item : registry())
			if (item.name == name)
				return &item;

		return nullptr;
	}
}

void pf::register_embedded_resources(const embedded_resource* items, const size_t count)
{
	auto& all = registry();
	all.insert(all.end(), items, items + count);
}

std::span<const uint8_t> pf::embedded_resource_data(const std::string_view name)
{
	if (const auto* found = find(name))
		return {found->data, found->size};

	return {};
}

std::string_view pf::embedded_resource_text(const std::string_view name)
{
	if (const auto* found = find(name))
		return {reinterpret_cast<const char*>(found->data), found->size};

	return {};
}
