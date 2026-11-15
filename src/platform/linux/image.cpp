// This file is part of libdesktop, a C++20 platform layer for desktop applications
// Copyright 2026  Zac Walker
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

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

platform::image_load_result platform::load_image_file(const core::file_path&, const platform::image_load_options&)
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

platform::image_load_result platform::load_image_memory(const std::span<const uint8_t>,
                                                        const platform::image_load_options&)
{
	return unavailable_result();
}

std::optional<platform::image_probe> platform::probe_image_memory(const std::span<const uint8_t>)
{
	return {};
}

platform::image_load_result platform::load_compound_preview(const std::span<const uint8_t>,
                                                            const platform::image_load_options&)
{
	return unavailable_result();
}

std::vector<std::string> platform::installed_image_extensions()
{
	return {};
}
