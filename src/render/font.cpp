// This file is part of libdesktop, a C++20 platform layer for desktop applications
// Copyright 2026  Zac Walker
//
// This program is free software; you can redistribute it and / or modify it
// under the terms of the LGPL License either version 2.1 or later.
// License details are available at https://www.gnu.org/licenses/lgpl-2.1.html
// This program is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY

// Purpose: Process-wide font descriptors and portable font metrics.

#include "libdesktop/render.h"

#include <limits>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace
{
	struct font_desc_hash
	{
		size_t operator()(const render::font_desc& desc) const noexcept
		{
			size_t result = std::hash<std::string>{}(desc.family);
			const auto mix = [&result](const size_t value)
			{
				result ^= value + static_cast<size_t>(0x9e3779b9) + (result << 6) + (result >> 2);
			};
			mix(std::hash<int>{}(desc.size));
			mix(std::hash<int>{}(desc.weight));
			mix(std::hash<bool>{}(desc.italic));
			mix(std::hash<bool>{}(desc.underline));
			mix(std::hash<bool>{}(desc.strikeout));
			mix(std::hash<bool>{}(desc.cell_size));
			return result;
		}
	};

	std::mutex font_mutex;
	std::unordered_map<render::font_desc, render::font_face, font_desc_hash> font_faces;
	std::vector<render::font_desc> font_descriptions;
}

render::font_face render::make_font(const render::font_desc& desc)
{
	std::scoped_lock lock(font_mutex);
	if (const auto found = font_faces.find(desc); found != font_faces.end()) return found->second;

	const auto index = font_descriptions.size();
	const auto id = static_cast<int>(render::detail::custom_font_base + index);
	const auto face = static_cast<render::font_face>(id);
	font_descriptions.push_back(desc);
	font_faces.emplace(font_descriptions.back(), face);
	return face;
}

bool render::detail::custom_font_description(const render::font_face face, render::font_desc& desc)
{
	const auto id = static_cast<int>(face);
	if (id < custom_font_base) return false;

	std::scoped_lock lock(font_mutex);
	const auto index = static_cast<size_t>(id - custom_font_base);
	if (index >= font_descriptions.size()) return false;
	desc = font_descriptions[index];
	return true;
}

render::font_metrics render::metrics(render::measure_context& mc, const render::font_face face)
{
	render::font_metrics result;
	result.height = std::max(0, mc.text_line_height(face));
	if (result.height == 0) return result;

	// The portable context exposes line height, baseline and advances, not a platform metrics object.
	// These measurements preserve the GDI contract's useful values on every text backend.
	const auto limit = std::numeric_limits<int>::max() / 4;
	const auto cell = mc.measure_text("0", face, render::text_style::single_line, limit, result.height);
	result.avg_char_width = std::max(0, cell.cx);
	// The portable context exposes no glyph ink box; keep the GDI-compatible cell-height value.
	result.x_height = result.height;
	result.ascent = std::clamp(mc.text_baseline(face), 0, result.height);
	result.descent = result.height - result.ascent;
	return result;
}
