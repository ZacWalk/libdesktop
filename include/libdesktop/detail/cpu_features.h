// Small, dependency-free CPU feature queries shared by libdesktop's core and
// system backends.  Callers still select a scalar path when a feature is not
// available.

#pragma once

#if defined(_MSC_VER) && (defined(_M_X64) || defined(_M_IX86))
#include <intrin.h>
#endif

namespace libdesktop::detail
{
	inline bool has_sse2() noexcept
	{
#if defined(_M_X64) || defined(__x86_64__)
		return true;
#elif defined(_M_IX86)
		int registers_[4]{};
		__cpuid(registers_, 1);
		return (registers_[3] & (1 << 26)) != 0;
#elif defined(__i386__)
		__builtin_cpu_init();
		return __builtin_cpu_supports("sse2") != 0;
#else
		return false;
#endif
	}

	inline bool has_avx2() noexcept
	{
#if defined(_M_X64) || defined(_M_IX86)
		static const bool available = []
		{
			int registers_[4]{};
			__cpuid(registers_, 0);
			if (registers_[0] < 7) return false;

			__cpuid(registers_, 1);
			constexpr int osxsave = 1 << 27;
			constexpr int avx = 1 << 28;
			if ((registers_[2] & (osxsave | avx)) != (osxsave | avx) ||
				(_xgetbv(0) & 0x6) != 0x6)
				return false;

			__cpuidex(registers_, 7, 0);
			return (registers_[1] & (1 << 5)) != 0;
		}();
		return available;
#elif defined(__x86_64__) || defined(__i386__)
		static const bool available = []
		{
			__builtin_cpu_init();
			return __builtin_cpu_supports("avx2") != 0;
		}();
		return available;
#else
		return false;
#endif
	}
}
