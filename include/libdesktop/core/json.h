// This file is part of libdesktop, a C++20 platform layer for desktop applications
// Copyright 2026  Zac Walker
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

// Purpose: JSON parsing and generation. Provides simple JSON document
// reading and writing for configuration and API responses.

#pragma once

#define RAPIDJSON_HAS_STDSTRING 1

#include <rapidjson/document.h>

namespace core::json
{
	// Values arrive from untrusted network responses: RapidJSON's typed getters assert rather than
	// check, and those asserts compile out in release, so the type must be verified here. That
	// includes the container: FindMember asserts IsObject(), so a document that failed to parse
	// aborts before any value check runs.
	inline std::string safe_string(const rapidjson::GenericValue<rapidjson::UTF8<char>>& json, const char* name)
	{
		if (!json.IsObject()) return {};

		const auto found = json.FindMember(name);
		return found != json.MemberEnd() && found->value.IsString() ? core::safe_string(found->value.GetString())
		                                                            : std::string{};
	}

	// Returns an empty object rather than asserting when the member is missing or not an object.
	inline const rapidjson::GenericValue<rapidjson::UTF8<char>>& safe_object(
		const rapidjson::GenericValue<rapidjson::UTF8<char>>& json, const char* name)
	{
		static const rapidjson::GenericValue<rapidjson::UTF8<char>> empty(rapidjson::kObjectType);
		if (!json.IsObject()) return empty;

		const auto found = json.FindMember(name);
		return found != json.MemberEnd() && found->value.IsObject() ? found->value : empty;
	}

	using json_doc = rapidjson::GenericDocument<rapidjson::UTF8<char>>;
}
