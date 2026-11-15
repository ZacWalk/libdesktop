// This file is part of libdesktop, a C++20 platform layer for desktop applications
// Copyright 2026  Zac Walker
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

// Purpose: Base64 encoding and decoding. Provides functions for converting
// binary data to/from base64 text representation.

#pragma once

std::string base64_encode(const uint8_t* input, size_t input_len);
std::vector<uint8_t> base64_decode(const char* input, size_t input_len);

inline std::string base64_encode(const std::string_view s)
{
	return base64_encode(std::bit_cast<const uint8_t*>(s.data()), s.size());
};

inline std::string base64_encode(const std::vector<uint8_t>& s)
{
	return base64_encode(s.data(), s.size());
};

inline std::vector<uint8_t> base64_decode(const std::string_view s)
{
	return base64_decode(s.data(), s.size());
};
