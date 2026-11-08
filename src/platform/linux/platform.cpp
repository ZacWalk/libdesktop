// This file is part of libdesktop, a C++20 platform layer for desktop applications
// Copyright 2026  Zac Walker
// 
// This program is free software; you can redistribute it and / or modify it
// under the terms of the LGPL License either version 2.1 or later.
// License details are available at https://www.gnu.org/licenses/lgpl-2.1.html
// This program is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY

// Purpose: The Linux backend's processes, synchronization, locale and known folders. Windows are in
// window.cpp, and what has no Linux counterpart yet is in desktop.cpp; docs/platform.md describes both.

#include "libdesktop/platform.h"
#include "libdesktop/core/cpu_features.h"
#include "primitives.h"
#include <SDL3/SDL.h>


#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <fcntl.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <sys/random.h>
#include <sys/syscall.h>
#include <linux/fs.h>
#include <linux/limits.h>
#include <ctime>

#ifndef RENAME_NOREPLACE
#define RENAME_NOREPLACE (1 << 0)
#endif

///////////////////////////////////////////////////////////////////////////////////////////////////
// Paths and time
///////////////////////////////////////////////////////////////////////////////////////////////////

// A Linux native path is the UTF-8 bytes, so no conversion is needed. The Windows implementation
// hands back a UTF-16 extended path instead; neither call site knows which.
std::filesystem::path platform::to_stream_path(const core::file_path path)
{
	return {path.str()};
}

std::filesystem::path platform::to_stream_path(const core::folder_path path)
{
	return {std::string(path.text())};
}


///////////////////////////////////////////////////////////////////////////////////////////////////
// Events
///////////////////////////////////////////////////////////////////////////////////////////////////


namespace
{
	// Claimed by whichever thread runs static initialisation - main's - until the entry point names
	// it, matching the Windows build where the UI thread is the one that runs platform::run.
	pthread_t ui_thread = ::pthread_self();
}

void platform::set_ui_thread()
{
	ui_thread = ::pthread_self();
}

bool platform::is_ui_thread()
{
	return ::pthread_equal(::pthread_self(), ui_thread) != 0;
}

uint32_t platform::wait_for_timeout = 0xFFFFFFFF;

// Set only after platform::is_closing, and manual reset, so a late waiter is still released.
core::thread_event platform::event_exit(true, false);

// Provisional: a poll rather than a real multi-object wait. Each event owns its own condition
// variable, so waiting on a set of them needs one shared condition; giving them that is part of
// the same work as replacing the spin lock in this file. The poll is correct, just not efficient.
uint32_t platform::wait_for(const std::vector<std::reference_wrapper<core::thread_event>>& events,
                            const uint32_t timeout_ms, const bool wait_all)
{
	constexpr uint32_t poll_ms = 5;
	const auto deadline = core::now_ms() + (timeout_ms == 0 ? 0 : timeout_ms);

	for (;;)
	{
		size_t signalled_count = 0;

		for (size_t i = 0; i < events.size(); ++i)
		{
			auto* const state = static_cast<core::detail::event_state*>(events[i].get()._h);
			if (state == nullptr) continue;

			std::unique_lock lock(state->mutex);
			if (!state->signalled) continue;

			if (!wait_all)
			{
				if (!state->manual_reset) state->signalled = false;
				return static_cast<uint32_t>(i);
			}

			++signalled_count;
		}

		if (wait_all && signalled_count == events.size() && !events.empty()) return 0;
		if (timeout_ms != 0 && core::now_ms() >= deadline) return wait_for_timeout;

		std::this_thread::sleep_for(std::chrono::milliseconds(poll_ms));
	}
}

void platform::set_crash_guard(crash_guard, bool)
{
}

bool platform::read_crash_guard(crash_guard)
{
	return false;
}

void platform::fail_crash_guard(crash_guard)
{
}

bool platform::crash_guard_failed(crash_guard)
{
	return false;
}

void platform::suppress_crash_guard(crash_guard, bool)
{
}

bool platform::crash_guard_suppressed(crash_guard)
{
	return false;
}

namespace
{
	// Timestamps are Windows FILETIME units -- 100ns intervals since 1601-01-01 UTC -- because
	// core::date_t stores them that way in portable code. See docs/platform.md.
	std::optional<int64_t> local_utc_offset_seconds(const uint64_t ts)
	{
		const auto seconds = static_cast<int64_t>(ts / core::detail::ft_ticks_per_second) -
			static_cast<int64_t>(core::detail::ft_epoch_to_unix_seconds);
		auto when = static_cast<time_t>(seconds);
		tm local = {};
		if (static_cast<int64_t>(when) != seconds || localtime_r(&when, &local) == nullptr)
		{
			core::log_once(__FUNCTION__, "UTC timestamp is outside the supported time range");
			return {};
		}
		return local.tm_gmtoff;
	}
}

uint64_t platform::utc_to_local(const uint64_t ts)
{
	if (ts == 0) return 0;
	const auto seconds = local_utc_offset_seconds(ts);
	if (!seconds) return ts;
	const auto offset = *seconds * static_cast<int64_t>(core::detail::ft_ticks_per_second);
	const auto magnitude = static_cast<uint64_t>(offset < 0 ? -offset : offset);
	if ((offset < 0 && ts < magnitude) || (offset >= 0 && ts > UINT64_MAX - magnitude))
	{
		core::log_once(__FUNCTION__, "Local timestamp is outside the supported time range");
		return ts;
	}
	return offset < 0 ? ts - magnitude : ts + magnitude;
}

uint64_t platform::local_to_utc(const uint64_t ts)
{
	if (ts == 0) return 0;
	const auto seconds = static_cast<int64_t>(ts / core::detail::ft_ticks_per_second) -
		static_cast<int64_t>(core::detail::ft_epoch_to_unix_seconds);
	const auto wall_time = static_cast<time_t>(seconds);
	tm local = {};
	if (static_cast<int64_t>(wall_time) != seconds || !gmtime_r(&wall_time, &local))
	{
		core::log_once(__FUNCTION__, "Local timestamp is outside the supported time range");
		return ts;
	}

	local.tm_isdst = -1;
	errno = 0;
	const auto utc = mktime(&local);
	const auto shifted = static_cast<int64_t>(utc) + static_cast<int64_t>(core::detail::ft_epoch_to_unix_seconds);
	if (errno == EOVERFLOW || shifted < 0 ||
		static_cast<uint64_t>(shifted) > (UINT64_MAX - ts % core::detail::ft_ticks_per_second) / core::detail::ft_ticks_per_second)
	{
		core::log_once(__FUNCTION__, "Cannot convert local timestamp to UTC");
		return ts;
	}
	return static_cast<uint64_t>(shifted) * core::detail::ft_ticks_per_second + ts % core::detail::ft_ticks_per_second;
}

///////////////////////////////////////////////////////////////////////////////////////////////////
// Known folders, following the XDG base directory specification
///////////////////////////////////////////////////////////////////////////////////////////////////

namespace
{
	std::string env_or(const char* name, const std::string& fallback)
	{
		const auto* const value = std::getenv(name);
		return value != nullptr && *value != 0 ? std::string(value) : fallback;
	}

	std::string home_folder()
	{
		return env_or("HOME", "/tmp");
	}
}

core::folder_path platform::known_path(const known_folder f)
{
	const auto home = home_folder();

	switch (f)
	{
	case known_folder::running_app_folder:
		{
			char buffer[PATH_MAX] = {};
			const auto len = ::readlink("/proc/self/exe", buffer, sizeof(buffer) - 1);
			if (len <= 0) return core::folder_path(home);
			const std::string_view exe(buffer, static_cast<size_t>(len));
			const auto slash = exe.find_last_of('/');
			return core::folder_path(slash == std::string_view::npos ? exe : exe.substr(0, slash));
		}
	case known_folder::app_data:
		return core::folder_path(std::format("{}/{}", env_or("XDG_DATA_HOME", home + "/.local/share"), core::app_file_stem()));
	case known_folder::roaming_app_data:
		return core::folder_path(env_or("XDG_CONFIG_HOME", home + "/.config"));
	case known_folder::app_cache_data:
		return core::folder_path(std::format("{}/{}", env_or("XDG_CACHE_HOME", home + "/.cache"), core::app_file_stem()));
	case known_folder::test_files_folder:
		return known_path(known_folder::running_app_folder).combine("test");
	case known_folder::downloads:
		return core::folder_path(env_or("XDG_DOWNLOAD_DIR", home + "/Downloads"));
	case known_folder::pictures:
		return core::folder_path(env_or("XDG_PICTURES_DIR", home + "/Pictures"));
	case known_folder::video:
		return core::folder_path(env_or("XDG_VIDEOS_DIR", home + "/Videos"));
	case known_folder::music:
		return core::folder_path(env_or("XDG_MUSIC_DIR", home + "/Music"));
	case known_folder::documents:
		return core::folder_path(env_or("XDG_DOCUMENTS_DIR", home + "/Documents"));
	case known_folder::desktop:
		return core::folder_path(env_or("XDG_DESKTOP_DIR", home + "/Desktop"));

	// Cloud provider folders are Windows shell locations with no Linux equivalent. An empty path is
	// how the rest of the app already expresses "this location does not exist here".
	case known_folder::dropbox_photos:
	case known_folder::onedrive_pictures:
	case known_folder::onedrive_video:
	case known_folder::onedrive_music:
	case known_folder::onedrive_camera_roll:
		return {};
	}

	return {};
}

bool platform::is_writable(const core::folder_path path)
{
	return ::access(std::string(path.text()).c_str(), W_OK) == 0;
}

bool platform::exists(const core::file_path path)
{
	struct stat st = {};
	return ::stat(path.str().c_str(), &st) == 0 && S_ISREG(st.st_mode);
}

bool platform::exists(const core::folder_path path)
{
	struct stat st = {};
	return ::stat(std::string(path.text()).c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}

///////////////////////////////////////////////////////////////////////////////////////////////////
// Calendar conversion. Timestamps are FILETIME ticks, so the epoch shift is applied on both sides.
///////////////////////////////////////////////////////////////////////////////////////////////////


std::string platform::format_date(const core::date_t d)
{
	const auto day = core::to_date(d._i);
	if (day.is_empty()) return {};
	return std::format("{:04}-{:02}-{:02}", day.year, day.month, day.day);
}

std::string platform::format_date_time(const core::date_t d)
{
	const auto day = core::to_date(d._i);
	if (day.is_empty()) return {};
	return std::format("{:04}-{:02}-{:02} {:02}:{:02}:{:02}", day.year, day.month, day.day, day.hour, day.minute,
	                   day.second);
}

std::string platform::number_dec_sep()
{
	const auto* const conv = ::localeconv();
	return conv != nullptr && conv->decimal_point != nullptr && *conv->decimal_point != 0
		       ? std::string(conv->decimal_point)
		       : ".";
}

std::string platform::utf8_to_a(const std::string_view utf8)
{
	// There is no "ANSI code page" on Linux; the system encoding is UTF-8, so this is the identity.
	return {utf8.begin(), utf8.end()};
}

std::string platform::user_language()
{
	// The environment holds this rather than a locale API, in the precedence the C library itself
	// uses. Values look like "en_GB.UTF-8@euro"; the caller wants just the language and territory,
	// in the same language_TERRITORY shape the Windows implementation returns.
	const char* value = nullptr;

	for (const auto* const name : {"LC_ALL", "LC_MESSAGES", "LANG"})
	{
		const auto* const candidate = std::getenv(name);

		if (candidate != nullptr && *candidate != 0)
		{
			value = candidate;
			break;
		}
	}

	if (value == nullptr) return {};

	std::string result(value);
	const auto cut = result.find_first_of(".@");
	if (cut != std::string::npos) result.erase(cut);

	// "C" and "POSIX" name the absence of a locale, not a language.
	if (result == "C" || result == "POSIX") return {};

	return result;
}

///////////////////////////////////////////////////////////////////////////////////////////////////
// Interning pool: address space is reserved up front and pages are committed as they are used, the
// same shape as the Windows MEM_RESERVE / MEM_COMMIT pair.
///////////////////////////////////////////////////////////////////////////////////////////////////

namespace
{
	platform::linux_move_file_test_failures move_file_failures;

	int rename_no_replace(const std::string& from, const std::string& to)
	{
		if (move_file_failures.rename_no_replace_unsupported)
		{
			errno = EINVAL;
			return -1;
		}

		return static_cast<int>(::syscall(SYS_renameat2, AT_FDCWD, from.c_str(), AT_FDCWD, to.c_str(),
		                                  RENAME_NOREPLACE));
	}

	int link_no_replace(const std::string& from, const std::string& to)
	{
		if (move_file_failures.link_unsupported)
		{
			errno = EPERM;
			return -1;
		}

		return ::link(from.c_str(), to.c_str());
	}
}

platform::file_op_result platform::move_file(const core::file_path existing, const core::file_path destination,
                                             const bool fail_if_exists)
{
	return move_file(existing, destination, fail_if_exists, true);
}

platform::file_op_result platform::move_file(const core::file_path existing, const core::file_path destination,
                                             const bool fail_if_exists, const bool allow_copy)
{
	const auto from = existing.str();
	const auto to = destination.str();
	const auto failure = [](const int error)
	{
		file_op_result result{error == EEXIST ? file_op_result_code::ALREADY_EXISTS :
			file_op_result_code::FAILED, std::string(::strerror(error))};
		result.error = {error, std::generic_category()};
		return result;
	};
	const auto copy_then_unlink = [&]
	{
		const auto copied = copy_file(existing, destination, true, false);
		if (copied.failed()) return copied;

		if (::unlink(from.c_str()) != 0)
		{
			return failure(errno);
		}

		return file_op_result{file_op_result_code::OK};
	};

	if (fail_if_exists)
	{
		if (rename_no_replace(from, to) == 0)
		{
			return {file_op_result_code::OK};
		}

		const auto rename_error = errno;
		if (rename_error == EEXIST) return failure(rename_error);

		if (rename_error == EXDEV && allow_copy)
		{
			return copy_then_unlink();
		}

		if (rename_error != ENOSYS && rename_error != EINVAL)
		{
			return failure(rename_error);
		}

		// rename() has no refusal mode, and a stat-then-rename would silently replace a destination
		// created in the window between them. link() refuses atomically with EEXIST, so the pair is
		// the refusal; the source is only unlinked once the new name is known to be this move's.
		if (link_no_replace(from, to) == 0)
		{
			if (::unlink(from.c_str()) == 0)
			{
				return {file_op_result_code::OK};
			}

			const auto unlink_error = errno;
			::unlink(to.c_str());
			return failure(unlink_error);
		}

		if (errno == EEXIST)
		{
			return failure(errno);
		}

		if (allow_copy &&
			(errno == EXDEV || errno == EPERM || errno == EOPNOTSUPP || errno == EMLINK || errno == ENOSYS))
		{
			return copy_then_unlink();
		}

		return failure(errno);
	}

	if (::rename(from.c_str(), to.c_str()) != 0)
	{
		// rename() cannot cross a filesystem boundary, which the Windows move crosses by copying. A
		// move between two mounts is that copy, then the source removed once the copy is whole. A
		// source that will not go leaves both in place rather than deleting the copy: under Replace
		// the copy is all that remains of what stood at the destination.
		if (errno == EXDEV && allow_copy)
		{
			const auto copied = copy_file(existing, destination, fail_if_exists, false);
			if (copied.failed()) return copied;

			if (::unlink(from.c_str()) != 0)
			{
				return failure(errno);
			}

			return {file_op_result_code::OK};
		}

		return failure(errno);
	}

	return {file_op_result_code::OK};
}

platform::file_op_result platform::move_file(const core::folder_path existing, const core::folder_path destination)
{
	if (rename_no_replace(std::string(existing.text()), std::string(destination.text())) == 0)
		return {file_op_result_code::OK};

	const auto error = errno;
	if (error == EEXIST) return {file_op_result_code::ALREADY_EXISTS};
	return {file_op_result_code::FAILED, std::format("Cannot atomically rename directory: {}", ::strerror(error))};
}

void platform::test_linux_move_file_failures(linux_move_file_test_failures failures)
{
	move_file_failures = failures;
}

void platform::test_linux_clear_move_file_failures()
{
	test_linux_move_file_failures({});
}


///////////////////////////////////////////////////////////////////////////////////////////////////
// Media services unavailable on the SDL/Linux backend.
///////////////////////////////////////////////////////////////////////////////////////////////////

bool platform::can_write_movies()
{
	return false;
}

platform::movie_writer_ptr platform::create_movie_writer(const platform::movie_writer_request&)
{
	return {};
}

platform::audio_output_ptr platform::create_audio_output(std::string_view, double)
{
	return {};
}

std::vector<platform::sound_device> platform::list_audio_playback_devices()
{
	return {};
}


core::pointi platform::cursor_position()
{
	float x = 0;
	float y = 0;
	if (!SDL_GetGlobalMouseState(&x, &y)) return {-1, -1};
	const auto display = SDL_GetPrimaryDisplay();
	const auto scale = display ? std::max(0.5f, SDL_GetDisplayContentScale(display)) : 1.0f;
	return {core::round(x * scale), core::round(y * scale)};
}

core::sizei platform::primary_screen_size()
{
	const auto display = SDL_GetPrimaryDisplay();
	SDL_Rect bounds{};
	if (!display || !SDL_GetDisplayBounds(display, &bounds)) return {};
	const auto scale = std::max(0.5f, SDL_GetDisplayContentScale(display));
	return {core::round(bounds.w * scale), core::round(bounds.h * scale)};
}

int platform::primary_screen_dpi()
{
	const auto display = SDL_GetPrimaryDisplay();
	if (!display) return 96;
	const auto scale = SDL_GetDisplayContentScale(display);
	return scale > 0.0f ? core::round(96.0 * scale) : 96;
}

std::string platform::last_error_message()
{
	return errno ? std::strerror(errno) : std::string{};
}

void platform::show_error(const std::string_view text)
{
	std::fprintf(stderr, "%.*s\n", static_cast<int>(text.size()), text.data());
	std::fflush(stderr);
}

void platform::attach_console()
{
	// Standard streams already belong to the invoking terminal on POSIX.
}

void platform::write_stdout(const std::string_view text)
{
	if (!text.empty()) std::fwrite(text.data(), 1, text.size(), stdout);
	std::fflush(stdout);
}

size_t platform::read_stdin(char* const buffer, const size_t capacity)
{
	return buffer && capacity ? std::fread(buffer, 1, capacity, stdin) : 0;
}
