// This file is part of libdesktop, a C++20 platform layer for desktop applications
// Copyright 2026  Zac Walker
//
// This program is free software; you can redistribute it and / or modify it
// under the terms of the LGPL License either version 2.1 or later.
// License details are available at https://www.gnu.org/licenses/lgpl-2.1.html
// This program is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY

// Purpose: Honest Linux stubs for the Windows image service until a decoder backend is available.

#include "libdesktop/platform.h"

namespace
{
	platform::image_load_result unavailable_result()
	{
		platform::image_load_result result;
		result.error = platform::image_load_error::unavailable;
		result.diagnostic = "Image decoding is not available on this platform.";
		return result;
	}
}

core::surface_ptr platform::load_image_file(const core::file_path&)
{
	return {};
}

platform::image_load_result platform::load_image_file(const core::file_path&, const platform::image_load_options)
{
	return unavailable_result();
}

std::optional<platform::image_probe> platform::probe_image_file(const core::file_path&)
{
	return {};
}

core::surface_ptr platform::load_image_memory(const std::span<const uint8_t>)
{
	return {};
}

platform::image_load_result platform::load_image_memory(const std::span<const uint8_t>, const platform::image_load_options)
{
	return unavailable_result();
}

std::optional<platform::image_probe> platform::probe_image_memory(const std::span<const uint8_t>)
{
	return {};
}

platform::image_load_result platform::load_compound_preview(const std::span<const uint8_t>, const platform::image_load_options)
{
	return unavailable_result();
}

std::vector<std::string> platform::installed_image_extensions()
{
	return {};
}
