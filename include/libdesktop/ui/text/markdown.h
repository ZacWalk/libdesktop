// This file is part of libdesktop, a C++20 platform layer for desktop applications
// Copyright 2026  Zac Walker
//
// This program is free software; you can redistribute it and / or modify it
// under the terms of the LGPL License either version 2.1 or later.
// License details are available at https://www.gnu.org/licenses/lgpl-2.1.html
// This program is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY

// Purpose: Markdown document model, parser, and HTML-to-Markdown reducer.

#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace ui::md
{
	enum class block : uint8_t
	{
		blank,
		paragraph,
		heading1,
		heading2,
		heading3,
		quote,
		bullet,
		numbered,
		code,
		rule,
	};

	struct span
	{
		std::string_view text;
		std::string_view link;
		bool bold = false;
		bool italic = false;
		bool code = false;
	};

	struct line
	{
		block kind = block::paragraph;
		std::vector<span> spans;
	};

	struct source_line
	{
		block kind = block::paragraph;
		bool fence = false;
		int content_start = 0;
		std::vector<span> spans;
	};

	struct document
	{
		std::shared_ptr<const std::string> text;
		std::vector<line> lines;
		std::vector<std::shared_ptr<const std::string>> fragments;

		[[nodiscard]] bool empty() const { return lines.empty(); }
	};

	std::string escape(std::string_view text);
	void parse_inline(std::string_view text, std::vector<span>& out);
	void parse_source_line(std::string_view text, bool& in_fence, source_line& out);
	document parse(std::string source);
	std::string from_html(std::string_view html);
}
