// This file is part of libdesktop, a C++20 platform layer for desktop applications
// Copyright 2026  Zac Walker
//
// This program is free software; you can redistribute it and / or modify it
// under the terms of the LGPL License either version 2.1 or later.
// License details are available at https://www.gnu.org/licenses/lgpl-2.1.html
// This program is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY

// Purpose: Draw a read-only document as offset, hexadecimal and printable-byte columns.

#pragma once

#include "libdesktop/ui/views/read_only_doc_view.h"

#include <cstdint>
#include <string>

namespace ui
{
	class hex_view : public read_only_doc_view
	{
	public:
		hex_view(view_host& host, const view_theme& theme, view_context* context = nullptr,
		         text_clipboard* clipboard = nullptr);
		~hex_view() override;

		static constexpr int bytes_per_line = 16;

	protected:
		void draw_view(render::draw_context& draw) const override;

	private:
		static constexpr int offset_chars = 10;
		static constexpr int hex_chars = 50;

		mutable std::string _line_buf;
		mutable std::string _offset_buf;
		mutable std::string _hex_buf;
		mutable std::string _ascii_buf;

		static void append_hex_byte(std::string& out, uint8_t value);
		void build_row(uint32_t file_offset) const;
	};
}