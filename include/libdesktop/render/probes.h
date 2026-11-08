// This file is part of libdesktop, a C++20 platform layer for desktop applications
// Copyright 2026  Zac Walker
// 
// This program is free software; you can redistribute it and / or modify it
// under the terms of the LGPL License either version 2.1 or later.
// License details are available at https://www.gnu.org/licenses/lgpl-2.1.html
// This program is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY

// Purpose: Platform rendering and font diagnostic probes.

#pragma once

namespace render
{
	// What the system font stack knows about one character in two named font families.
	// Exposed so tests can assert the glyph-fallback contract (diffractor#219) without
	// reaching into the platform text API themselves.
	struct glyph_fallback_probe
	{
		// False when the query could not run at all (no text engine, or a family is
		// not installed on this machine). Callers must treat the rest as meaningless.
		bool available = false;
		// Glyph index the primary family maps the character to; 0 means "not covered",
		// which is what makes fallback mandatory.
		uint16_t primary_glyph = 0;
		// Glyph index in the fallback family. Non-zero when that family covers it.
		uint16_t fallback_glyph = 0;
		// True when metrics for fallback_glyph read from the fallback face succeeded.
		bool fallback_metrics_ok = false;
		// True when reading metrics for fallback_glyph from the PRIMARY face either
		// fails or returns different metrics -- i.e. the face confusion is real.
		bool primary_metrics_differ = false;
	};

	glyph_fallback_probe probe_glyph_fallback(std::string_view primary_family, std::string_view fallback_family,
	                                          char32_t code_point);

	// What the font-face cache does with repeated requests. Exposed so tests can assert the
	// caching contract without reaching into the renderer: a repeat request must be served
	// from the cache rather than re-running the family lookup and re-logging its failures
	// (diffractor#232), and a font-size change must produce a distinct face so glyphs cached for
	// the old size cannot be reused (diffractor#189).
	struct font_cache_probe
	{
		// False when no text engine could be created; the rest is then meaningless.
		bool available = false;
		// Entries held after the first request. One request must add exactly one entry.
		int entries_after_first = 0;
		// True when the identical request returned the same face and added no entry.
		bool same_request_is_cached = false;
		// True when the same face at a different size returned a different instance.
		bool size_change_is_distinct = false;
		// True when a different face type at the same size returned a different instance.
		bool face_change_is_distinct = false;
		// True when resetting the fonts emptied the cache, so a settings change cannot
		// leave stale faces behind.
		bool reset_clears_cache = false;
	};

	font_cache_probe probe_font_cache(int base_font_size);

	// Test-only probe of the software backend's scratch buffer. The scratch tile stops being
	// reallocated once it reaches its final size, so growing the client no longer reallocates
	// anything. These record how much of a grown client the canvas will actually accept writes for,
	// summed over the tiles the real loop walks, against the buffer that stayed behind - which must
	// not have grown with the window. Whether tiling itself is seamless is a question about the
	// rasterizer rather than the platform, and is render::probe_software_rasterizer.
	struct software_tiling_probe
	{
		int grown_client_pixels = 0;
		int grown_writable_pixels = 0;
		int grown_buffer_pixels = 0;
	};

	software_tiling_probe probe_software_tiling();
}
