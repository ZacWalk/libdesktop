// This file is part of libdesktop, a C++20 platform layer for desktop applications
// Copyright 2026  Zac Walker
//
// This program is free software; you can redistribute it and / or modify it
// under the terms of the LGPL License either version 2.1 or later.
// License details are available at https://www.gnu.org/licenses/lgpl-2.1.html
// This program is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY

// Purpose: URL, charset and UTF-8 conversion helpers.

#pragma once

namespace platform
{
	// URL resolution follows the legacy Windows URL helper for ordinary HTTP/file URLs. The base and
	// result are UTF-8; an empty result means the platform could not resolve the input.
	std::string resolve_url(std::string_view base, std::string_view relative);

	// Returns a Windows/IANA-compatible numeric code page. Zero means an unknown label; UTF-8 is
	// 65001 on Windows and Linux even though Linux performs conversion through iconv names.
	uint32_t charset_to_codepage(std::string_view charset);

	// Converts bytes from codepage to UTF-8. Code page zero and UTF-8 leave bytes unchanged. The
	// Linux backend guarantees UTF-8, Latin-1 and the iconv encodings installed by the host.
	std::string transcode_to_utf8(std::string_view bytes, uint32_t codepage);

	// Percent-encodes bytes other than RFC 3986's unreserved characters.
	std::string url_encode(std::string_view input);
}
