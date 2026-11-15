// This file is part of libdesktop, a C++20 platform layer for desktop applications
// Copyright 2026  Zac Walker
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

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
