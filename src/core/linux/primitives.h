// This file is part of libdesktop, a C++20 platform layer for desktop applications
// Copyright 2026  Zac Walker
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

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
