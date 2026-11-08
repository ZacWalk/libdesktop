// This file is part of libdesktop, a C++20 platform layer for desktop applications
// Copyright 2026  Zac Walker
//
// This program is free software; you can redistribute it and / or modify it
// under the terms of the LGPL License either version 2.1 or later.
// License details are available at https://www.gnu.org/licenses/lgpl-2.1.html
// This program is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY

#include "libdesktop/core.h"
#include "libdesktop/core/cpu_features.h"
#include "primitives.h"

#include <condition_variable>
#include <cstdio>
#include <fcntl.h>
#include <pthread.h>
#include <sched.h>
#include <sys/mman.h>
#include <sys/random.h>
#include <sys/syscall.h>
#include <unistd.h>

namespace
{

	constexpr uintptr_t lock_writer_bit = 1;
	constexpr uintptr_t lock_reader_step = 2;

	std::atomic_ref<uintptr_t> lock_state(const uintptr_t& cs)
	{
		return std::atomic_ref<uintptr_t>(const_cast<uintptr_t&>(cs));
	}
}

std::atomic<size_t> core::static_memory_usage = 0;

uint32_t core::tick_count()
{
	return static_cast<uint32_t>(
		std::chrono::duration_cast<std::chrono::milliseconds>(
			std::chrono::steady_clock::now().time_since_epoch()).count());
}

uint64_t core::tick_count64()
{
	timespec uptime = {};
	if (::clock_gettime(CLOCK_BOOTTIME, &uptime) != 0) return 0;
	return static_cast<uint64_t>(uptime.tv_sec) * 1000ull +
	       static_cast<uint64_t>(uptime.tv_nsec) / 1'000'000ull;
}

uint32_t core::current_thread_id()
{
	return static_cast<uint32_t>(::gettid());
}

core::thread_event::thread_event(const bool manual_reset, const bool initial_state)
{
	create(manual_reset, initial_state);
}

core::thread_event::~thread_event()
{
	delete static_cast<core::detail::event_state*>(_h);
	_h = nullptr;
}

void core::thread_event::create(const bool manual_reset, const bool initial_state)
{
	auto* const state = new core::detail::event_state();
	state->manual_reset = manual_reset;
	state->signalled = initial_state;
	_h = state;
}

void core::thread_event::set() const noexcept
{
	auto* const state = static_cast<core::detail::event_state*>(_h);
	if (state == nullptr) return;

	{
		std::lock_guard lock(state->mutex);
		state->signalled = true;
	}

	if (state->manual_reset) state->cv.notify_all();
	else state->cv.notify_one();
}

void core::thread_event::reset() const noexcept
{
	auto* const state = static_cast<core::detail::event_state*>(_h);
	if (state == nullptr) return;

	std::lock_guard lock(state->mutex);
	state->signalled = false;
}

void* core::memory_pool::alloc(const size_t size)
{
	const core::exclusive_lock lock(cs);

	if (base == nullptr)
	{
		auto want = reserve_size;

		while (want >= block_size)
		{
			auto* const p = ::mmap(nullptr, want, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);

			if (p != MAP_FAILED)
			{
				base = static_cast<uint8_t*>(p);
				reserved = want;
				break;
			}

			want /= 2;
		}

		if (base == nullptr) return nullptr;
	}

	const auto aligned = (size + alignment - 1) & ~(alignment - 1);
	if (used + aligned > reserved) return nullptr;

	while (used + aligned > committed)
	{
		const auto next = committed + block_size;
		if (next > reserved) return nullptr;
		if (::mprotect(base + committed, block_size, PROT_READ | PROT_WRITE) != 0) return nullptr;
		committed = next;
	}

	auto* const result = base + used;
	used += aligned;
	return result;
}

void core::mutex::ex_lock() const
{
	auto state = lock_state(_cs);
	uintptr_t expected = 0;

	while (!state.compare_exchange_weak(expected, lock_writer_bit, std::memory_order_acquire,
	                                    std::memory_order_relaxed))
	{
		expected = 0;
		::sched_yield();
	}
}

void core::mutex::ex_unlock() const
{
	lock_state(_cs).store(0, std::memory_order_release);
}

bool core::mutex::try_ex_lock() const
{
	auto state = lock_state(_cs);
	uintptr_t expected = 0;
	return state.compare_exchange_strong(expected, lock_writer_bit, std::memory_order_acquire,
	                                     std::memory_order_relaxed);
}

void core::mutex::sh_lock() const
{
	auto state = lock_state(_cs);

	for (;;)
	{
		auto current = state.load(std::memory_order_relaxed);

		if ((current & lock_writer_bit) != 0)
		{
			::sched_yield();
			continue;
		}

		if (state.compare_exchange_weak(current, current + lock_reader_step, std::memory_order_acquire,
		                                std::memory_order_relaxed))
		{
			return;
		}
	}
}

void core::mutex::sh_unlock() const
{
	lock_state(_cs).fetch_sub(lock_reader_step, std::memory_order_release);
}

#if defined(__x86_64__) || defined(__i386__)
namespace
{
	// __builtin_cpu_init has to run before any __builtin_cpu_supports query, and these are
	// namespace-scope initialisers, so it is sequenced ahead of them here.
	const bool cpu_features_ready = (__builtin_cpu_init(), true);
}

bool core::sse2_supported = true;
bool core::ssse3_supported = cpu_features_ready && __builtin_cpu_supports("ssse3");
bool core::crc32_supported = cpu_features_ready && __builtin_cpu_supports("sse4.2");
bool core::avx2_supported = cpu_features_ready && __builtin_cpu_supports("avx2");
bool core::avx512_supported = cpu_features_ready && __builtin_cpu_supports("avx512f");
bool core::neon_supported = false;
bool core::arm_crc32_supported = false;
#elif defined(__aarch64__)
bool core::sse2_supported = false;
bool core::ssse3_supported = false;
bool core::crc32_supported = false;
bool core::avx2_supported = false;
bool core::avx512_supported = false;
bool core::neon_supported = true;
#if defined(__ARM_FEATURE_CRC32)
bool core::arm_crc32_supported = true;
#else
bool core::arm_crc32_supported = false;
#endif
#else
bool core::sse2_supported = false;
bool core::ssse3_supported = false;
bool core::crc32_supported = false;
bool core::avx2_supported = false;
bool core::avx512_supported = false;
bool core::neon_supported = false;
bool core::arm_crc32_supported = false;
#endif

bool core::has_avx2()
{
	// The build targets an SSE2 baseline, so wider paths are dispatched at
	// runtime exactly as they are on Windows.
	return core::detail::has_avx2();
}

void core::secure_zero(void* ptr, const size_t len)
{
	::explicit_bzero(ptr, len);
}

bool core::generate_random_bytes(uint8_t* buffer, const size_t len)
{
	size_t filled = 0;

	while (filled < len)
	{
		const auto n = ::getrandom(buffer + filled, len - filled, 0);

		if (n < 0)
		{
			if (errno == EINTR) continue;
			return false;
		}

		filled += static_cast<size_t>(n);
	}

	return true;
}

void core::debug_output(const std::string_view message)
{
	std::fwrite(message.data(), 1, message.size(), stderr);
}

void core::debug_output(const std::string& message)
{
	trace(std::string_view(message));
}

void core::set_thread_description(const std::string_view name)
{
	// pthread_setname_np truncates at 16 bytes including the terminator.
	char buffer[16] = {};
	const auto len = std::min(name.size(), sizeof(buffer) - 1);
	std::memcpy(buffer, name.data(), len);
	::pthread_setname_np(::pthread_self(), buffer);
}

core::date_t core::utc_now()
{
	// To the same 100ns tick the file times carry. Truncating this to whole seconds made a file
	// written during the current second later than the scan that had just read it, so every
	// freshly written file reported as needing a re-scan.
	const auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
		std::chrono::system_clock::now().time_since_epoch()).count();
	return core::date_t(core::detail::ft_epoch_to_unix_seconds * core::detail::ft_ticks_per_second + static_cast<uint64_t>(ns) / 100u);
}

core::day_t core::to_date(const uint64_t ts)
{
	const auto seconds = static_cast<int64_t>(ts / core::detail::ft_ticks_per_second) -
		static_cast<int64_t>(core::detail::ft_epoch_to_unix_seconds);
	auto when = static_cast<time_t>(seconds);
	tm parts = {};
	if (::gmtime_r(&when, &parts) == nullptr) return {};

	return {
		parts.tm_year + 1900, parts.tm_mon + 1, parts.tm_mday,
		parts.tm_hour, parts.tm_min, parts.tm_sec
	};
}

uint64_t core::from_date(const core::day_t& day)
{
	// The fields are checked as the Windows backend's SystemTimeToFileTime checks them, so that a date
	// out of range - a thirteenth month, the 31st of April, the "0000:00:00 00:00:00" a camera writes
	// for no date - is no date on either platform. timegm would roll it into some other day instead,
	// and a year before 1601 wraps through the conversion below into garbage. As on Windows, each
	// field is held in 16 bits and a month or day of 0 reads as 1.
	static_assert(sizeof(time_t) >= 8, "a validated date converts without failing only with a 64-bit time_t");

	const auto year = static_cast<uint16_t>(day.year);
	auto month = static_cast<uint16_t>(day.month);
	auto day_of_month = static_cast<uint16_t>(day.day);
	const auto hour = static_cast<uint16_t>(day.hour);
	const auto minute = static_cast<uint16_t>(day.minute);
	const auto second = static_cast<uint16_t>(day.second);

	if (month == 0) month = 1;
	if (day_of_month == 0) day_of_month = 1;

	if (year < 1601 || year > 30827 || month > 12 || hour > 23 || minute > 59 || second > 59) return 0;

	static constexpr uint16_t days_in_month[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
	const auto leap = (year % 4 == 0 && year % 100 != 0) || year % 400 == 0;
	if (day_of_month > days_in_month[month - 1] + (month == 2 && leap ? 1 : 0)) return 0;

	tm parts = {};
	parts.tm_year = year - 1900;
	parts.tm_mon = month - 1;
	parts.tm_mday = day_of_month;
	parts.tm_hour = hour;
	parts.tm_min = minute;
	parts.tm_sec = second;
	parts.tm_isdst = 0;

	// Before 1970 the seconds are negative, and the unsigned arithmetic below still lands on the right
	// tick: every date that passed the checks lies after FILETIME's epoch.
	const auto seconds = ::timegm(&parts);
	return (static_cast<uint64_t>(seconds) + core::detail::ft_epoch_to_unix_seconds) * core::detail::ft_ticks_per_second;
}
