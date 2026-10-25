// This file is part of libdesktop, a C++20 platform layer for desktop applications
// Copyright 2026  Zac Walker
// 
// This program is free software; you can redistribute it and / or modify it
// under the terms of the LGPL License either version 2.1 or later.
// License details are available at https://www.gnu.org/licenses/lgpl-2.1.html
// This program is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY

// Purpose: Reading a JSON file into a RapidJSON document, for util_json.h.

#include "libdesktop/system.h"
#include "libdesktop/util_json.h"

#include "rapidjson/istreamwrapper.h"

df::util::json::json_doc df::util::json::json_from_file(const file_path path)
{
	std::ifstream ifs(platform::to_stream_path(path));
	rapidjson::BasicIStreamWrapper<std::ifstream> isw(ifs);
	json_doc d;
	d.ParseStream(isw);
	return d;
}
