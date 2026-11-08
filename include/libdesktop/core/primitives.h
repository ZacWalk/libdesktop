// This file is part of libdesktop, a C++20 platform layer for desktop applications
// Copyright 2026  Zac Walker
// 
// This program is free software; you can redistribute it and / or modify it
// under the terms of the LGPL License either version 2.1 or later.
// License details are available at https://www.gnu.org/licenses/lgpl-2.1.html
// This program is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY

// Purpose: Core process-local platform primitives and synchronization types.

#pragma once

namespace core
{
	class date_t;
	struct day_t;
}

namespace core
{
	// Written under memory_pool::cs, read from the UI thread for the about box.
	extern std::atomic<size_t> static_memory_usage;
	extern bool sse2_supported;
	extern bool ssse3_supported;
	extern bool crc32_supported;
	extern bool avx2_supported;
	extern bool avx512_supported;
	extern bool neon_supported;
	extern bool arm_crc32_supported;

	void secure_zero(void* ptr, size_t len);

	// False leaves the buffer zeroed; callers must not fall back to its contents.
	bool generate_random_bytes(uint8_t* buffer, size_t len);

	void debug_output(std::string_view message);
	void debug_output(const std::string& message);
	using sha256_digest = std::array<uint8_t, 32>;
	// The app is compiled for an SSE2 baseline, so anything wider has to be selected at run time.
	bool has_avx2();
	void set_thread_description(std::string_view name);
	date_t utc_now();
	day_t to_date(uint64_t timestamp);
	uint64_t from_date(const day_t& day);
	class mutex final : public core::no_copy
	{
		// Opaque OS lock (a Win32 SRWLOCK). Zero is its documented initialised state, so the type
		// is constant-initialised and needs neither a constructor nor a destructor.
		mutable uintptr_t _cs = 0;

	public:
		_Acquires_exclusive_lock_(this)
		void ex_lock() const;

		// For a caller that must never block, such as the crash handler reading state a worker
		// mutates. Answers false rather than waiting; the caller decides what a refused read means.
		_When_(return != 0, _Acquires_exclusive_lock_(this))
		bool try_ex_lock() const;

		_Releases_exclusive_lock_(this)
		void ex_unlock() const;

		_Acquires_shared_lock_(this)
		void sh_lock() const;

		_Releases_shared_lock_(this)
		void sh_unlock() const;

		void unlock() const
		{
			ex_unlock();
		}

		void lock() const
		{
			ex_lock();
		}

		friend class shared_lock;
		friend class exclusive_lock;
	};

	class shared_lock final : public core::no_copy
	{
		const mutex& _rw;
		bool _locked = false;

	public:
		shared_lock(const mutex& rw) : _rw(rw)
		{
			lock();
		}

		~shared_lock() override
		{
			unlock();
		}

		void unlock()
		{
			if (_locked)
			{
				_rw.sh_unlock();
				_locked = false;
			}
		}

		void lock()
		{
			if (!_locked)
			{
				_rw.sh_lock();
				_locked = true;
			}
		}
	};

	class exclusive_lock final : public core::no_copy
	{
		const mutex& _rw;

	public:
		exclusive_lock(const mutex& rw) : _rw(rw)
		{
			_rw.ex_lock();
		}

		~exclusive_lock() override
		{
			_rw.ex_unlock();
		}
	};

	// Never waits. Held by callers that cannot afford to block on a lock another thread may be
	// holding - the crash handler above all, where blocking on the faulting thread's own lock would
	// hang the report instead of producing it.
	class try_exclusive_lock final : public core::no_copy
	{
		const mutex& _rw;
		const bool _locked;

	public:
		try_exclusive_lock(const mutex& rw) : _rw(rw), _locked(rw.try_ex_lock())
		{
		}

		~try_exclusive_lock() override
		{
			if (_locked) _rw.ex_unlock();
		}

		bool locked() const { return _locked; }
	};

	class thread_event
	{
	public:
		// Opaque OS event handle (a Win32 HANDLE), null until create() succeeds. Held as void*
		// rather than std::any so the noexcept signalling path can neither throw nor type-check.
		void* _h = nullptr;

		thread_event() = default;
		~thread_event();
		thread_event(bool manual_reset, bool initial_state);
		void create(bool manual_reset, bool initial_state);

		thread_event(const thread_event& other) = delete;
		void operator=(const thread_event& other) = delete;

		void reset() const noexcept;
		void set() const noexcept;
	};

	uint32_t tick_count();
	// Monotonic uptime in milliseconds, including time spent suspended; unlike tick_count it does
	// not wrap at 32 bits.
	uint64_t tick_count64();
	uint32_t current_thread_id();
	// Append-only arena for the interned string records. One reservation of virtual address space is
	// committed a block at a time, so every record keeps a stable offset from a base that never moves -
	// which is what lets core::cached be a 32-bit handle rather than a pointer.
	struct memory_pool
	{
		mutex cs;
		uint8_t* base = nullptr;
		size_t reserved = 0;
		size_t committed = 0;
		size_t used = 0;

		constexpr static size_t block_size = 1024_z * 1024_z; // commit granularity, and the largest record
		constexpr static size_t alignment = 4;

		// Address space only; pages are committed on demand, and alloc halves this until a reservation
		// succeeds. A 32-bit build gets 2GB in total (the exe is not large-address-aware), so it takes a
		// sixteenth rather than the half a gigabyte a 64-bit build can ignore; at roughly 30 bytes per
		// interned name that still covers a collection far larger than a 32-bit process can index.
		constexpr static size_t reserve_size = sizeof(void*) == 8
			                                      ? 1024_z * 1024_z * 1024_z
			                                      : 128_z * 1024_z * 1024_z;

		void* alloc(size_t size);
	};

	template <typename T>
	class pool_allocator
	{
		memory_pool _pool;
	};
}
