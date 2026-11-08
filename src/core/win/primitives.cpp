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

#include <VersionHelpers.h>
#include <bcrypt.h>
#pragma comment(lib, "bcrypt")

FILETIME ts_to_ft(const uint64_t timestamp)
{
	FILETIME file_time;
	file_time.dwHighDateTime = static_cast<uint32_t>(timestamp >> 32);
	file_time.dwLowDateTime = static_cast<uint32_t>(timestamp & 0xffffffffull);
	return file_time;
}

uint64_t ft_to_ts(const FILETIME& file_time)
{
	return static_cast<__int64>(file_time.dwHighDateTime) << 32 | file_time.dwLowDateTime;
}

std::atomic<size_t> core::static_memory_usage = 0;

void* core::memory_pool::alloc(const size_t size)
{
	static std::bad_alloc OOM;
	core::exclusive_lock lock(cs);

	if (size > block_size) throw OOM;

	if (base == nullptr)
	{
		// Reserve once and never move: handles are offsets from this address for the process lifetime.
		// A 32-bit address space may not have the whole range free in one piece, so settle for a smaller
		// arena rather than failing to start - the size only bounds how many strings can be interned.
		for (auto attempt = reserve_size; attempt >= block_size; attempt /= 2)
		{
			base = std::bit_cast<uint8_t*>(VirtualAlloc(nullptr, attempt, MEM_RESERVE, PAGE_NOACCESS));

			if (base)
			{
				reserved = attempt;
				break;
			}
		}

		if (!base) throw OOM;
	}

	const auto align_size = (size + (alignment - 1)) / alignment * alignment;

	if (align_size > committed - used)
	{
		const auto shortfall = align_size - (committed - used);
		const auto grow = (shortfall + block_size - 1) / block_size * block_size;

		if (grow > reserved - committed) throw OOM;
		if (!VirtualAlloc(base + committed, grow, MEM_COMMIT, PAGE_READWRITE)) throw OOM;

		committed += grow;
		core::static_memory_usage.fetch_add(grow, std::memory_order_relaxed);
	}

	auto* const result = base + used;
	used += align_size;
	return result;
}

uint32_t core::tick_count()
{
	return GetTickCount();
}

uint64_t core::tick_count64()
{
	return GetTickCount64();
}

uint32_t core::current_thread_id()
{
	return GetCurrentThreadId();
}

core::thread_event::thread_event(const bool manual_reset, const bool initial_state)
{
	create(manual_reset, initial_state);
}

void core::thread_event::create(const bool manual_reset, const bool initial_state)
{
	DWORD flags = 0;

	if (manual_reset)
		flags |= CREATE_EVENT_MANUAL_RESET;

	if (initial_state)
		flags |= CREATE_EVENT_INITIAL_SET;

	auto* const h = ::CreateEventEx(nullptr, nullptr, flags, EVENT_ALL_ACCESS);

	if (h == nullptr)
	{
		throw core::app_exception("CreateEventEx failed");
	}

	if (_h) CloseHandle(_h);
	_h = h;
}

core::thread_event::~thread_event()
{
	if (_h)
	{
		CloseHandle(_h);
		_h = nullptr;
	}
}

void core::thread_event::reset() const noexcept
{
	if (_h) ResetEvent(_h);
}

void core::thread_event::set() const noexcept
{
	if (_h) SetEvent(_h);
}


_Acquires_exclusive_lock_(this)
void core::mutex::ex_lock() const
{
	AcquireSRWLockExclusive(std::bit_cast<PSRWLOCK>(&_cs));
}

_When_(return != 0, _Acquires_exclusive_lock_(this))
bool core::mutex::try_ex_lock() const
{
	return TryAcquireSRWLockExclusive(std::bit_cast<PSRWLOCK>(&_cs)) != 0;
}

_Releases_exclusive_lock_(this)
void core::mutex::ex_unlock() const
{
	ReleaseSRWLockExclusive(std::bit_cast<PSRWLOCK>(&_cs));
}

_Acquires_shared_lock_(this)
void core::mutex::sh_lock() const
{
	AcquireSRWLockShared(std::bit_cast<PSRWLOCK>(&_cs));
}

_Releases_shared_lock_(this)
void core::mutex::sh_unlock() const
{
	ReleaseSRWLockShared(std::bit_cast<PSRWLOCK>(&_cs));
}

void core::set_thread_description(const std::string_view name)
{
	if (IsWindows10OrGreater())
	{
		using pfnSetThreadDescription = HRESULT(WINAPI*)(HANDLE, PCWSTR);

		if (!core::is_empty(name))
		{
			// Worker threads name themselves as they start, so the lookup is resolved exactly once.
			static const pfnSetThreadDescription set_thread_description_proc = []
			{
				auto* const kernel32 = GetModuleHandleW(L"kernel32.dll");
				return kernel32
					       ? std::bit_cast<pfnSetThreadDescription>(GetProcAddress(kernel32, "SetThreadDescription"))
					       : nullptr;
			}();

			if (set_thread_description_proc != nullptr)
			{
				set_thread_description_proc(GetCurrentThread(), core::utf8_to_utf16(name).c_str());
			}
		}
	}
}

void core::debug_output(const std::string_view message)
{
	trace(std::string(message));
}

void core::debug_output(const std::string& message)
{
	OutputDebugStringA(std::bit_cast<const char*>(message.c_str()));
}

void core::secure_zero(void* ptr, const size_t len)
{
	SecureZeroMemory(ptr, len);
}

bool core::generate_random_bytes(uint8_t* buffer, const size_t len)
{
	const auto status = BCryptGenRandom(nullptr, buffer, static_cast<ULONG>(len), BCRYPT_USE_SYSTEM_PREFERRED_RNG);

	if (!BCRYPT_SUCCESS(status))
	{
		// Leaving the buffer untouched would hand the caller stale bytes that look random.
		SecureZeroMemory(buffer, len);
		core::log(__FUNCTION__, std::format("BCryptGenRandom failed with status {:#x}", static_cast<uint32_t>(status)));
		return false;
	}

	return true;
}

bool core::sse2_supported = IsProcessorFeaturePresent(PF_XMMI64_INSTRUCTIONS_AVAILABLE) != 0;
bool core::ssse3_supported = IsProcessorFeaturePresent(PF_SSSE3_INSTRUCTIONS_AVAILABLE) != 0;
bool core::crc32_supported = IsProcessorFeaturePresent(PF_SSE4_2_INSTRUCTIONS_AVAILABLE) != 0;
bool core::avx2_supported = IsProcessorFeaturePresent(PF_AVX2_INSTRUCTIONS_AVAILABLE) != 0;
bool core::avx512_supported = IsProcessorFeaturePresent(PF_AVX512F_INSTRUCTIONS_AVAILABLE) != 0;
bool core::neon_supported = IsProcessorFeaturePresent(PF_ARM_NEON_INSTRUCTIONS_AVAILABLE) != 0;
bool core::arm_crc32_supported = IsProcessorFeaturePresent(PF_ARM_V8_CRC32_INSTRUCTIONS_AVAILABLE) != 0;

bool core::has_avx2()
{
	return core::detail::has_avx2();
}

core::date_t core::utc_now()
{
	FILETIME file_time;
	GetSystemTimeAsFileTime(&file_time);
	return core::date_t(ft_to_ts(file_time));
}

core::day_t core::to_date(const uint64_t timestamp)
{
	// perf https://stackoverflow.com/questions/15957805/extract-year-month-day-etc-from-cpp
	const auto file_time = ts_to_ft(timestamp);
	SYSTEMTIME system_time = {};
	FileTimeToSystemTime(&file_time, &system_time);
	return {system_time.wYear, system_time.wMonth, system_time.wDay, system_time.wHour, system_time.wMinute,
	        system_time.wSecond};
}

uint64_t core::from_date(const core::day_t& day)
{
	SYSTEMTIME system_time = {0};

	system_time.wYear = day.year;
	system_time.wMonth = day.month;
	system_time.wDay = day.day;
	system_time.wHour = day.hour;
	system_time.wMinute = day.minute;
	system_time.wSecond = day.second;

	if (system_time.wMonth == 0) system_time.wMonth = 1;
	if (system_time.wDay == 0) system_time.wDay = 1;

	FILETIME file_time;
	return SystemTimeToFileTime(&system_time, &file_time) != 0 ? ft_to_ts(file_time) : 0;
}
