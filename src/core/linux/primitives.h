// This file is part of libdesktop, a C++20 platform layer for desktop applications
// Copyright 2026  Zac Walker
//
// This program is free software; you can redistribute it and / or modify it
// under the terms of the LGPL License either version 2.1 or later.
// License details are available at https://www.gnu.org/licenses/lgpl-2.1.html
// This program is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY

#pragma once

#include <condition_variable>

namespace core::detail
{
	inline constexpr uint64_t ft_ticks_per_second = core::date_t::intervals_per_second;
	inline constexpr uint64_t ft_epoch_to_unix_seconds = core::date_t::offset_1970 / ft_ticks_per_second;

	// Shared by the core event primitive and platform's multi-event wait.
	struct event_state
	{
		std::mutex mutex;
		std::condition_variable cv;
		bool signalled = false;
		bool manual_reset = false;
	};
}
