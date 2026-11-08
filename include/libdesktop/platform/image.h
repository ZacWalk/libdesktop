// This file is part of libdesktop, a C++20 platform layer for desktop applications
// Copyright 2026  Zac Walker
//
// This program is free software; you can redistribute it and / or modify it
// under the terms of the LGPL License either version 2.1 or later.
// License details are available at https://www.gnu.org/licenses/lgpl-2.1.html
// This program is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY

// Purpose: Bounded platform image decoding and compound-document previews.

#pragma once

#include "libdesktop/core.h"

namespace platform
{
	// Source pixels and returned BGRA bytes are separate budgets. A zero budget means no limit.
	struct image_load_options
	{
		uint64_t max_pixels = 0;
		uint64_t max_decoded_bytes = 0;
		uint32_t target_width = 0;
		uint32_t target_height = 0;
		bool thumbnail = false;
		bool apply_orientation = false;
		std::function<bool()> cancelled;
		std::function<void(uint64_t decoded_bytes, uint64_t source_bytes)> progress;
	};

	enum class image_load_error
	{
		none,
		unavailable,
		invalid_data,
		too_large,
		cancelled,
		io_error
	};

	struct image_load_result
	{
		core::surface_ptr image;
		image_load_error error = image_load_error::none;
		std::string diagnostic;
		// Original dimensions in the returned orientation, before reduction.
		uint32_t source_width = 0;
		uint32_t source_height = 0;
		uint16_t stored_orientation = 1;

		[[nodiscard]] bool success() const { return image != nullptr && error == image_load_error::none; }
	};

	struct image_probe
	{
		uint32_t width = 0;
		uint32_t height = 0;
		uint32_t frame_count = 0;
		uint16_t orientation = 1;
		std::string container;
		bool has_alpha = false;
	};

	// The plain forms decode whatever they are given, for images the application trusts (its own
	// resources and bundled files). The forms taking options also refuse a source over 100,000 pixels on
	// a side, apply the option limits, cancellation and progress, and say why a load failed.
	core::surface_ptr load_image_file(const core::file_path& path);
	image_load_result load_image_file(const core::file_path& path, image_load_options options);
	std::optional<image_probe> probe_image_file(const core::file_path& path);

	core::surface_ptr load_image_memory(std::span<const uint8_t> data);
	image_load_result load_image_memory(std::span<const uint8_t> data, image_load_options options);
	std::optional<image_probe> probe_image_memory(std::span<const uint8_t> data);
	std::vector<std::string> installed_image_extensions();

	// Extract and rasterise an OLE SummaryInformation PIDSI_THUMBNAIL metafile preview. Source
	// dimensions retain the stored logical extent, just as a regular image result does.
	image_load_result load_compound_preview(std::span<const uint8_t> data, image_load_options options = {});
}
