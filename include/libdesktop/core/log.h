// This file is part of libdesktop, a C++20 platform layer for desktop applications
// Copyright 2026  Zac Walker
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

// Purpose: Scoped function entry/exit logging. Emits a start and an exit log
// line for the lifetime of the guard.

#pragma once

struct log_func
{
	const std::string_view context;
	const std::string_view context2;

	log_func(const std::string_view c, const std::string_view c2 = {}) : context(c), context2(c2)
	{
		if (!core::is_empty(context2))
		{
			core::log(context, std::format("start {}", context2));
		}
		else
		{
			core::log(context, "start");
		}
	}

	~log_func()
	{
		if (!core::is_empty(context2))
		{
			core::log(context, std::format("exit {}", context2));
		}
		else
		{
			core::log(context, "exit");
		}
	}
};
