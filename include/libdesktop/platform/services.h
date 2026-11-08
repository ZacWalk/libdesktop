// This file is part of libdesktop, a C++20 platform layer for desktop applications
// Copyright 2026  Zac Walker
// 
// This program is free software; you can redistribute it and / or modify it
// under the terms of the LGPL License either version 2.1 or later.
// License details are available at https://www.gnu.org/licenses/lgpl-2.1.html
// This program is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY

// Purpose: Cross-platform operating-system services, files, settings, threads, and networking.

#pragma once

namespace core
{
	class item_selector;
	class item_set;
	class date_t;
	struct day_t;
	struct index_roots;
	struct count_and_size;
};

namespace platform
{
	// The folders a media library starts from: the user's own and those of the sync clients installed.
	struct local_folders_result
	{
		core::folder_path pictures;
		core::folder_path video;
		core::folder_path music;
		core::folder_path desktop;
		core::folder_path downloads;
		core::folder_path dropbox_photos;
		core::folder_path onedrive_pictures;
		core::folder_path onedrive_video;
		core::folder_path onedrive_music;
	};
}

namespace platform
{
	class window;

	extern std::atomic_bool is_closing;
	extern core::file_path log_path;
	extern core::file_path previous_log_path;

	void start_log();
	core::file_path close_log();

	// The messages the platform layer shows or reports, in English as declared. The application holds
	// a set, translates it with the rest of its text and installs it. The defaults name no
	// application; one that wants its name in them sets its own wording before translating.
	struct messages
	{
		core::text_t error_ole_failed = "OleInitialize Failed";
		core::text_t error_atl_direct3d = "Failed to Initialize Direct3d";
		core::text_t error_winsock_failed = "Failed to initialize Winsock";
		core::text_t error_sse2_needed = "This program requires a processor with SSE2.";
		core::text_t error_unsupported_os = "This program requires Windows 7 or newer.";
		core::text_t error_cannot_continue = "This program has a serious problem and cannot continue.";
		core::text_t title_error = "Error";

		// Accelerator labels are installed with the application text because the portable formatter
		// lives in this library and must use the consumer's translations.
		core::text_t keyboard_alt = "alt";
		core::text_t keyboard_back = "back";
		core::text_t keyboard_backspace = "backspace";
		core::text_t keyboard_browser_back = "browser-back";
		core::text_t keyboard_browser_favorites = "browser-favorites";
		core::text_t keyboard_browser_forward = "browser-forward";
		core::text_t keyboard_browser_home = "browser-home";
		core::text_t keyboard_browser_refresh = "browser-refresh";
		core::text_t keyboard_browser_search = "browser-search";
		core::text_t keyboard_browser_stop = "browser-stop";
		core::text_t keyboard_control = "ctrl";
		core::text_t keyboard_del = "del";
		core::text_t keyboard_down = "down";
		core::text_t keyboard_end = "end";
		core::text_t keyboard_enter = "enter";
		core::text_t keyboard_escape = "escape";
		core::text_t keyboard_f1 = "F1";
		core::text_t keyboard_f10 = "F10";
		core::text_t keyboard_f11 = "F11";
		core::text_t keyboard_f12 = "F12";
		core::text_t keyboard_f2 = "F2";
		core::text_t keyboard_f3 = "F3";
		core::text_t keyboard_f4 = "F4";
		core::text_t keyboard_f5 = "F5";
		core::text_t keyboard_f6 = "F6";
		core::text_t keyboard_f7 = "F7";
		core::text_t keyboard_f8 = "F8";
		core::text_t keyboard_f9 = "F9";
		core::text_t keyboard_home = "home";
		core::text_t keyboard_insert = "insert";
		core::text_t keyboard_left = "left";
		core::text_t keyboard_media_next_track = "media-next-track";
		core::text_t keyboard_media_play_pause = "media-play-pause";
		core::text_t keyboard_media_prev_track = "media-prev-track";
		core::text_t keyboard_media_stop = "media-stop";
		core::text_t keyboard_next = "page-down";
		core::text_t keyboard_oem_4 = "[";
		core::text_t keyboard_oem_6 = "]";
		core::text_t keyboard_oem_minus = "-";
		core::text_t keyboard_oem_plus = "+";
		core::text_t keyboard_or = "OR";
		core::text_t keyboard_prior = "page-up";
		core::text_t keyboard_right = "right";
		core::text_t keyboard_shift = "shift";
		core::text_t keyboard_space = "space";
		core::text_t keyboard_tab = "tab";
		core::text_t keyboard_up = "up";
		core::text_t keyboard_volume_down = "volume-down";
		core::text_t keyboard_volume_mute = "volume-mute";
		core::text_t keyboard_volume_up = "volume-up";
		core::text_t menu_add_fmt = "Add '{}'";
		core::text_t menu_undo = "Undo";
		core::text_t menu_cut = "Cut";
		core::text_t menu_copy = "Copy";
		core::text_t menu_paste = "Paste";
		core::text_t menu_delete = "Delete";
		core::text_t menu_select_all = "Select All";
		core::text_t error_same_file = "The source and destination files are the same file.";
		core::text_t error_many_src_1_dest =
			"Multiple file paths were specified in the source buffer, but only one destination file path.";
		core::text_t error_diff_dir =
			"Rename operation was specified but the destination path is a different directory. Use the move operation instead.";
		core::text_t error_src_root_dir = "The source is a root directory, which cannot be moved or renamed.";
		core::text_t error_op_cancelled = "The operation was canceled.";
		core::text_t error_dest_subtree = "The destination is a subtree of the source.";
		core::text_t error_access_denied_src = "Security options denied access to the source.";
		core::text_t error_path_too_deep = "The source or destination path exceeded or would exceed MAX_PATH.";
		core::text_t error_many_dest =
			"The operation involved multiple destination paths, which can fail in the case of a move operation.";
		core::text_t error_invalid_files = "The path in the source or destination or both was invalid.";
		core::text_t error_dest_same_tree = "The source and destination have the same parent folder.";
		core::text_t error_fld_dest_is_file = "The destination path is an existing file.";
		core::text_t error_file_dest_is_fld = "The destination path is an existing folder.";
		core::text_t error_filename_too_long = "The name of the file exceeds MAX_PATH.";
		core::text_t error_dest_is_cd_rom = "The destination is a read-only CD-ROM, possibly unformatted.";
		core::text_t error_dest_is_dvd = "The destination is a read-only DVD, possibly unformatted.";
		core::text_t error_dest_is_cd_record = "The destination is a writable CD-ROM, possibly unformatted.";
		core::text_t error_file_too_large =
			"The file involved in the operation is too large for the destination media or file system.";
		core::text_t error_src_is_cdrom = "The source is a read-only CD-ROM, possibly unformatted.";
		core::text_t error_src_is_dvd = "The source is a read-only DVD, possibly unformatted.";
		core::text_t error_src_is_cd_record = "The source is a writable CD-ROM, possibly unformatted.";
		core::text_t error_max = "MAX_PATH was exceeded during the operation.";
		core::text_t error_unknown = "An unknown error occurred.";
		core::text_t error_on_dest = "An unspecified error occurred on the destination.";
		core::text_t error_dst_root_dir = "Destination is a root directory and cannot be renamed.";
		core::text_t select_folder = "Select Folder...";
		core::text_t jpeg_best = "JPEG - Best compression for photos";
		core::text_t png_best = "PNG - Best for non photos";
		core::text_t webp_best = "WEBP - Best for the web";
		core::text_t jxl_best = "JPEG XL - Smallest files for photos";
		core::text_t error_connect_scanner = "Could not connect to scanner";
		core::text_t error_scanner = "Scanner reported an error";
		core::text_t error_save_image = "Failed to save image.";
	};

	inline std::atomic<const messages*> installed_messages{nullptr};

	// The installed set, or English until the application installs one.
	inline const messages& text()
	{
		static const messages english;
		const auto* const installed = installed_messages.load(std::memory_order_acquire);
		return installed ? *installed : english;
	}

	inline void install_text(const messages* m)
	{
		installed_messages.store(m, std::memory_order_release);
	}


	std::string OS();

	// The platform identity the daily ping reports, packed by platform::pack_environment. Every value is
	// read fresh: a system fact means "right now", unlike a feature bit, which means "at least once
	// since the last report". platform_environment.h owns each value's meaning and packed layout.
	platform::environment_facts environment();


	core::unique_folders known_folders();
	local_folders_result local_folders();

	void set_desktop_wallpaper(core::file_path path);
	void show_file_properties(const std::vector<core::file_path>& files, const std::vector<core::folder_path>& folders);

	bool has_burner();
	bool burn_to_cd(const std::vector<core::file_path>& files, const std::vector<core::folder_path>& folders);
	void print(const std::vector<core::file_path>& files, const std::vector<core::folder_path>& folders);

	std::string format_number(const std::string& num_text);
	std::string number_dec_sep();
	std::string format_size(const core::file_size& size);
	bool is_valid_file_name(std::string_view name);

	// A failed query is not proof of absence. Access denied, an offline volume or a dropped network
	// share all fail the same call as a deleted file, so they stay `unknown` and a caller that would
	// delete or overwrite on "not there" cannot act on a guess.
	enum class file_presence
	{
		unknown,
		not_found,
		found,
	};

	struct file_attributes_t
	{
		file_presence presence = file_presence::unknown;
		bool is_readonly = false;
		bool is_offline = false;
		bool is_hidden = false;
		bool is_reparse = false;
		uint64_t modified = 0;
		uint64_t created = 0;
		uint64_t size = 0;
		struct identity_t
		{
			uint64_t volume = 0;
			uint64_t index = 0;
			bool valid = false;

			bool operator==(const identity_t&) const = default;
		} identity;

		bool exists() const { return presence == file_presence::found; }
		bool confirmed_missing() const { return presence == file_presence::not_found; }
	};

	struct file_info
	{
		core::folder_path folder = {};
		core::cached name = {};
		file_attributes_t attributes = {};
	};

	struct folder_info
	{
		core::cached name = {};
		file_attributes_t attributes = {};
		bool can_recurse = true;
	};

	struct folder_contents
	{
		std::vector<folder_info> folders;
		std::vector<file_info> files;
		bool success = false;

		folder_contents() noexcept = default;
		folder_contents(const folder_contents&) = delete;
		folder_contents& operator=(const folder_contents&) = delete;
		folder_contents(folder_contents&&) noexcept = default;
		folder_contents& operator=(folder_contents&&) noexcept = default;
	};

	enum class drive_type
	{
		unknown,
		removable,
		fixed,
		remote,
		cdrom,
		device
	};

	struct drive_t
	{
		drive_type type = drive_type::unknown;
		std::string name;
		std::string vol_name;
		std::string file_system;

		core::file_size used;
		core::file_size free;
		core::file_size capacity;
	};

	using drives = std::vector<drive_t>;
	drives scan_drives();

	class file
	{
	public:
		virtual ~file() = default;

		enum class whence
		{
			begin,
			current,
			end
		};

		virtual uint64_t size() const = 0;
		virtual uint64_t read(uint8_t* buf, uint64_t buf_size) const = 0;
		virtual uint64_t write(const uint8_t* data, uint64_t size) = 0;
		// Positional I/O leaves the current stream position unchanged. Backends override these with
		// native positional calls where available; the default keeps older adapters source-compatible.
		virtual uint64_t read_at(uint64_t offset, uint8_t* buf, uint64_t buf_size) const
		{
			const auto old = pos();
			if (old == UINT64_MAX || seek(offset, whence::begin) == UINT64_MAX) return 0;
			const auto result = read(buf, buf_size);
			seek(old, whence::begin);
			return result;
		}
		virtual uint64_t write_at(uint64_t offset, const uint8_t* data, uint64_t size)
		{
			const auto old = pos();
			if (old == UINT64_MAX || seek(offset, whence::begin) == UINT64_MAX) return 0;
			const auto result = write(data, size);
			seek(old, whence::begin);
			return result;
		}
		// Makes earlier writes durable enough to report success. A false result means the caller
		// cannot claim the requested file was fully persisted.
		virtual bool flush() const = 0;
		virtual uint64_t seek(uint64_t pos, whence w) const = 0;
		virtual uint64_t pos() const = 0;
		virtual bool trunc(uint64_t pos) const = 0;
		virtual core::file_path path() const = 0;
	};

	using file_ptr = std::shared_ptr<file>;

	enum class map_mode
	{
		// One view over the whole file. data() is the file; touching all of it makes all of it
		// resident, so this suits random sampling rather than a full scan.
		whole_file,
		// No view until set_window is called. Lets a full scan keep only a bounded window
		// resident, which is both smaller and faster than scanning a whole-file view.
		windowed,
	};

	// A read-only view of a file's bytes backed by the file itself rather than by heap.
	//
	// The pages are clean and shared, so they cost no commit charge and do not appear in the
	// private working set that Task Manager reports as "Memory". They are also evictable without
	// a pagefile write, because the file on disk is already their backing store. That makes this
	// the cheapest way to read a large data file that is scanned once and then sampled at random.
	//
	// Only touched pages become resident, so mapping a whole file and reading a few records out of
	// it costs a few pages rather than the file size.
	class mapped_file : public core::no_copy
	{
	public:
		virtual ~mapped_file() = default;

		virtual uint64_t file_size() const = 0;

		// The current view. Empty for a windowed mapping until set_window succeeds.
		virtual core::cspan data() const = 0;

		// Re-points the view at [offset, offset + len), clamped to the file. The returned span
		// starts exactly at offset even though the underlying view is granularity-aligned. The
		// previous view is invalidated, so callers must not retain spans across a call.
		//
		// Concurrency: data() and file_size() are safe to call from any number of threads, so a
		// whole-file mapping can be shared by readers. set_window mutates the view and is not, so a
		// shared mapping must not be re-windowed while readers hold it.
		virtual core::cspan set_window(uint64_t offset, uint64_t len) = 0;

		// Drops the mapped pages from the working set without unmapping. The data stays valid and
		// faults back in on next touch, trading a soft fault for a smaller reported figure.
		virtual void release_working_set() = 0;
	};

	using mapped_file_ptr = std::shared_ptr<mapped_file>;

	// Null when the file is missing, empty or cannot be mapped.
	mapped_file_ptr map_file(core::file_path path, map_mode mode = map_mode::whole_file);

	enum class file_open_mode
	{
		read,
		write,
		create,
		// Truncates on open but shares read access only, so a second live writer fails with a
		// sharing violation while a closed log can be replaced.
		create_exclusive,
		create_new,
		sequential_scan,
		read_write,
		// Read sources while allowing other readers, writers and renames.
		read_shared,
	};

	// The path form the OS and native third-party libraries take. Windows needs UTF-16, with the
	// \\?\ prefix once the path is long; elsewhere the file system takes the UTF-8 bytes as they
	// are. Portable callers pass it straight through and never inspect it.
#ifdef _WIN32
	using native_path = std::wstring;
#else
	using native_path = std::string;
#endif

	native_path to_file_system_path(core::file_path path);
	native_path to_file_system_path(core::folder_path path);

	// The same path a native API would be given, as UTF-8, for the third-party libraries that take a
	// byte path on every platform. On Windows this still carries the \\?\ prefix for a long path, so
	// it is not interchangeable with core::file_path::str().
	std::string to_utf8_file_system_path(core::file_path path);

	// std::fstream accepts a std::filesystem::path everywhere, but only MSVC accepts a std::wstring.
	// Going through this keeps stream call sites free of any assumption about the native encoding.
	std::filesystem::path to_stream_path(core::file_path path);
	std::filesystem::path to_stream_path(core::folder_path path);

	enum class known_folder
	{
		running_app_folder,
		test_files_folder,
		app_data,
		// The user's roaming application-data root, shared by every application (app_data is this application's own folder).
		roaming_app_data,
		app_cache_data,
		downloads,
		pictures,
		video,
		music,
		documents,
		desktop,
		dropbox_photos,
		onedrive_pictures,
		onedrive_video,
		onedrive_music,
		onedrive_camera_roll,
	};

	core::folder_path known_path(known_folder f);
	file_ptr open_file(core::file_path path, file_open_mode mode);
	enum class file_lock_mode { read, rename_delete };
	// Pins the object without following reparse points. On Windows, both modes exclude writers;
	// read permits renames, while rename_delete also excludes other rename/delete handles.
	file_ptr open_file_lock(core::file_path path, file_lock_mode mode = file_lock_mode::rename_delete);
	uint32_t file_crc32(core::file_path path);
	uint32_t file_crc32(core::file_path path, const core::cancel_token& token);
	std::optional<core::sha256_digest> file_sha256(core::file_path path, const core::cancel_token& token = {});
	std::optional<core::sha256_digest> file_sha256(const file_ptr& file, const core::cancel_token& token = {});

	// The same checksum, with "could not read it" distinguished from "it checksummed to zero". A
	// caller that acts on the answer - Sync deletes a file whose content still matches what it
	// reviewed - cannot treat two failed reads as proof the file is unchanged.
	std::optional<uint32_t> file_crc32_checked(core::file_path path, const core::cancel_token& token);

	// What checksumming whole files has cost this session, for the application's performance summary.
	struct file_hash_counters
	{
		std::atomic_uint64_t computed = 0;
		std::atomic_uint64_t failed = 0;
		std::atomic_uint64_t bytes = 0;
		std::atomic_uint64_t us = 0;
		std::atomic_uint32_t max_us = 0;
	};

	extern file_hash_counters file_hash_perf;

	bool eject(core::folder_path path);


	enum class file_op_result_code
	{
		OK,
		CANCELLED,
		FAILED,
		// A fail-if-exists write refused because the destination is there. That is the answer to the
		// question the caller asked, not a fault, so a run can report it as a file that changed after
		// review rather than as a disk error.
		ALREADY_EXISTS
	};

	struct file_op_result
	{
		file_op_result_code code = file_op_result_code::FAILED;
		std::string error_message;
		core::paths created_files;

		// Set only by replace_file when it renames the replacement into place through a
		// retained handle. This is that same still-open, cache-coherent handle, positioned at
		// the start of the freshly-swapped file, so the file can be re-scanned immediately
		// without a stale by-name reopen. Null when replace_file fell back to a plain move.
		// When set, `modified` holds the file's modified time (in core::date_t ticks, i.e. the
		// value core::date_t wraps) read back from the handle. Stored as a raw tick count rather
		// than a core::date_t because date_t is only forward-declared here (see pch.h include order).
		file_ptr coherent_handle;
		uint64_t modified = 0;
		// Preserves the backend error for portable conditions such as cross_device_link.
		std::error_code error{};

		bool success() const
		{
			return code == file_op_result_code::OK;
		}

		bool failed() const
		{
			return code != file_op_result_code::OK;
		}

		std::string format_error(std::string_view text = {}, std::string_view more_text = {}) const;
	};

	// Identity-pinned operations. A backend that cannot bind the operation to the open token returns
	// FAILED rather than falling back to a check-then-rename/delete sequence.
	file_op_result move_file(const file_ptr& existing, core::file_path destination, bool fail_if_exists);
	file_op_result delete_file(const file_ptr& existing);

	file_op_result delete_items(const std::vector<core::file_path>& files, const std::vector<core::folder_path>& folders,
	                            bool allow_undo);
	// Returns true if deleting the given paths would move them to the Recycle Bin.
	// Returns false when any path is on a location that bypasses the Recycle Bin
	// (for example a UNC network path), causing a permanent, unrecoverable delete.
	bool can_recycle(const std::vector<core::file_path>& files, const std::vector<core::folder_path>& folders);
	// True when reaching the folder may wait on a network: a UNC path or a mapped network drive. Answered
	// from the path and the volume type alone, without touching the location, so a caller can tell
	// whether a presence check is affordable on a thread others are queued behind.
	bool is_network_location(core::folder_path folder);
	// Copies or moves through the shell. Collisions are auto-renamed unless replace_existing is set,
	// which the caller may only do having named the colliding files and had the overwrite confirmed.
	file_op_result move_or_copy(const std::vector<core::file_path>& files, const std::vector<core::folder_path>& folders,
	                            core::folder_path target, bool is_move, bool replace_existing = false);

	file_op_result delete_file(core::file_path path);
	file_op_result make_file_writable(core::file_path path);
	// Applies only the supplied attributes. A timestamp is in FILETIME ticks (the same units as
	// file_attributes_t); an unset field is left unchanged.
	file_op_result set_file_attributes(core::file_path path, std::optional<uint64_t> modified = {},
	                                   std::optional<bool> readonly = {});
	std::optional<double> file_age_seconds(core::file_path path);
	file_attributes_t file_attributes(core::file_path path);
	file_attributes_t file_attributes(core::folder_path path);
	file_op_result copy_file(core::file_path existing, core::file_path destination, bool fail_if_exists,
	                         bool can_create_folder);
	file_op_result move_file(core::file_path existing, core::file_path destination, bool fail_if_exists);
	// Disabling copying lets an application stage a cross-volume fallback behind its own review gate.
	file_op_result move_file(core::file_path existing, core::file_path destination, bool fail_if_exists,
	                         bool allow_copy);
	file_op_result move_file(core::folder_path existing, core::folder_path destination);
	file_op_result replacement_flush_result(bool flushed, std::string error_message = {});
	file_op_result replace_file(core::file_path destination, core::file_path existing, bool create_originals = false,
	                            std::optional<file_attributes_t> expected_destination = {});
	bool exists(core::file_path path);
	bool exists(core::folder_path path);
	file_op_result create_folder(core::folder_path path);
	// True when a new file can be created in the folder. Store package folders are read-only and
	// per-machine installs are read-only without elevation, so writable data must live elsewhere.
	bool is_writable(core::folder_path path);
	bool open(core::file_path path);
	bool open(std::string_view path);
	bool run(std::string_view cmd);
	// Preferred over run(cmd): naming the image removes any ambiguity about which prefix of the
	// command line is the executable (CWE-428).
	bool run(core::file_path exe, std::string_view cmd);
	void show_in_file_browser(core::file_path path);
	void show_in_file_browser(core::folder_path path);

	// Total working set counts clean file-backed pages that are shared with every other process
	// mapping the same file, so a memory-mapped data file inflates it while costing the process no
	// private memory at all. Task Manager's Memory column reports the private working set, so that
	// is the figure to show a user who is comparing the two.
	struct memory_usage_t
	{
		int64_t private_working_set = 0;
		int64_t shared_working_set = 0;
		int64_t working_set = 0;
		int64_t peak_working_set = 0;
		int64_t commit = 0;
	};

	bool memory_usage(memory_usage_t& result);
	core::folder_path temp_folder();
	int display_frequency();


	core::file_path resolve_link(core::file_path path);
	bool created_date(core::file_path path, core::date_t dt);


	struct metadata_result
	{
		std::optional<std::string> title;
		std::vector<std::string> tags;
		std::optional<int> rating;
	};

	// Fails for containers Windows has no property handler for, and transiently on a UNC path
	// under load, so the result names the step that failed rather than collapsing to a bool.
	file_op_result write_shell_tags(core::file_path path, const std::vector<std::string>& tags);
	metadata_result read_shell_metadata(core::file_path path);

	core::count_and_size calc_folder_summary(core::folder_path folder, bool show_hidden, const core::cancel_token& token);
	folder_contents iterate_file_items(core::folder_path folder, bool show_hidden);
	std::vector<folder_info> select_folders(const core::item_selector& selector, bool show_hidden);
	std::vector<file_info> select_files(const core::item_selector& selector, bool show_hidden);

	// Test seam: when set, this predicate overrides cloud-placeholder (offline) detection so
	// unit tests can simulate OneDrive Files On-Demand online-only files without real cloud
	// storage. Returning true marks the given path as offline during folder enumeration.
	extern std::function<bool(const core::file_path&)> test_offline_predicate;

	// Test seam: when set, replace_file asks this rather than the file system whether its destination
	// is there, so a test can give the answer a dropped share or a denied query gives: unknown.
	extern std::function<file_presence(const core::file_path&)> test_replace_destination_presence;

#ifndef _WIN32
	struct linux_copy_file_test_failures
	{
		size_t max_write_bytes = 0;
		bool interrupt_first_write = false;
		int fail_write_after_calls = -1;
		bool fail_destination_close = false;
	};

	struct linux_move_file_test_failures
	{
		bool rename_no_replace_unsupported = false;
		bool link_unsupported = false;
	};

	enum class linux_settings_test_failure
	{
		none,
		after_open,
		after_write,
		before_replace,
	};

	void test_linux_copy_file_failures(linux_copy_file_test_failures failures);
	void test_linux_clear_copy_file_failures();
	void test_linux_move_file_failures(linux_move_file_test_failures failures);
	void test_linux_clear_move_file_failures();
	void test_linux_settings_failure(linux_settings_test_failure failure);
	int test_linux_settings_save_count();
	void test_linux_reset_settings_save_count();

	extern std::function<void(core::file_path)> test_linux_before_claim_file_path;
	extern std::function<void(core::folder_path)> test_linux_before_claim_folder_path;
	extern std::function<void(core::file_path)> test_linux_before_copy_folder_file_path;
	extern std::function<void()> test_linux_before_settings_replace;
#endif

	struct scan_result
	{
		bool success = false;
		core::file_path saved_file_path;
		std::string error_message;
	};

	scan_result scan(core::folder_path save_path);

	enum class get_cached_file_properties_response
	{
		ok,
		fail,
		pending
	};

	// What the Windows Shell's property store says about a file without opening it: all a cloud
	// placeholder can offer before it is downloaded. A value is present only when the store holds its
	// key with a value of the expected type; a text key holding anything else reads as empty text.
	struct shell_properties
	{
		std::optional<std::string> album_artist;
		std::optional<std::string> artist;
		std::optional<std::string> title;
		std::optional<std::string> genre;
		std::optional<std::string> album;
		std::optional<std::string> comment;
		std::optional<uint32_t> year;
		std::optional<uint32_t> track;
		std::optional<uint32_t> width;
		std::optional<uint32_t> height;
		// EXIF numbering, 1 to 8.
		std::optional<uint16_t> orientation;
		// The shell's reading of DateTimeOriginal: a core::date_t timestamp, in UTC.
		std::optional<uint64_t> date_taken;
		std::vector<std::string> keywords;
		// Signed by the hemisphere references; present together or not at all.
		std::optional<double> latitude;
		std::optional<double> longitude;
		// The cached thumbnail's encoded bytes, read whole, or nothing.
		core::blob thumbnail;
	};

	get_cached_file_properties_response read_shell_properties(core::file_path path, shell_properties& properties_out);

	// 32-bit pixels - blue, green, red, alpha - with the top row first and rows packed.
	struct bgra_pixels
	{
		uint32_t width = 0;
		uint32_t height = 0;
		std::vector<uint8_t> pixels;
	};

	// Fetch a thumbnail via the Windows Shell (IShellItemImageFactory) -- the same path Explorer
	// uses. For a cloud-only placeholder this returns the provider's thumbnail WITHOUT hydrating.
	// allow_network=false restricts to the local thumbnail cache; true permits a cloud fetch.
	get_cached_file_properties_response get_shell_thumbnail(core::file_path path, core::sizei requested_extent,
	                                                        bool allow_network, bgra_pixels& thumbnail_out);

	std::string user_name();
	std::string last_os_error();

	// Last-resort feedback for a failure that happens before there is a window or a message loop to
	// show the app's own dialog. Blocks until the user dismisses it.
	void show_startup_failure(std::string_view message);

	// What the process tells the application from outside its message loop, on whichever thread
	// failed: a crash, once its minidump is written (an empty path when none could be), and a
	// termination by Windows' recovery, before which the application may save its state.
	class process_events
	{
	public:
		virtual ~process_events() = default;
		virtual void crash(core::file_path dump_file_path) = 0;
		virtual void save_recovery_state() = 0;
	};

	// Returns a human-readable reason why the file cannot currently be opened for writing
	// (e.g. locked by another process, read-only, access denied), or an empty string if it
	// can be opened. Used to turn opaque metadata-write failures into an actionable message.
	std::string file_write_error(core::file_path path);
	// Bounded wait for a transient lock on an existing file to clear. True when it became writable.
	bool wait_for_unlocked_write(core::file_path path);
	core::file_path temp_file(std::string_view ext = ".tmp", core::folder_path folder = {});

	bool browse_for_folder(core::folder_path& path);
	// Native picker overloads keep the existing services while making the owner explicit.
	bool browse_for_folder(window* owner, core::folder_path& path, std::string_view title = {});
	bool prompt_for_save_path(core::file_path& path);
	bool prompt_for_save_path(window* owner, core::file_path& path);

	// One entry in a file dialog's type list. `extensions` is the shell's own pattern form,
	// semicolon separated, because that is what every platform's dialog takes.
	struct file_dialog_filter
	{
		std::string text;
		std::string extensions;
	};

	// For a file the user names for a task the shell knows nothing about -- a project, a playlist.
	// The first filter is the default. An empty result means the user cancelled. An empty title
	// leaves the platform's own.
	bool prompt_for_open_paths(std::vector<core::file_path>& paths, const std::vector<file_dialog_filter>& filters,
	                           bool allow_multiple);
	bool prompt_for_open_paths(window* owner, std::vector<core::file_path>& paths,
	                           const std::vector<file_dialog_filter>& filters, bool allow_multiple,
	                           std::string_view title = {});
	bool prompt_for_save_path(core::file_path& path, const std::vector<file_dialog_filter>& filters);
	bool prompt_for_save_path(window* owner, core::file_path& path,
	                          const std::vector<file_dialog_filter>& filters, std::string_view title = {});

	// The operating system's own UTF-16 conversion. Nothing in the app calls these: core::utf16_to_utf8
	// and core::utf8_to_utf16 are the conversions it uses, and these exist so a test can check those
	// against an implementation we did not write.
	std::string utf16_to_utf8(std::wstring_view text);
	std::wstring utf8_to_utf16(std::string_view text);

	std::string utf8_to_a(std::string_view utf8);

	// Canonical Unicode composition (NFC). Used to make text that differs only in
	// normalization form (e.g. Korean Hangul as precomposed syllables vs decomposed
	// conjoining jamo) compare and search as equal. ASCII input is returned as-is.
	std::string normalize_nfc(std::string_view text);


#ifndef WINSTORE
	// Downloads https://<host>/<file_name> into the downloads folder, then calls complete with its
	// path if it is correctly signed, or with an empty path.
	void download_and_verify(std::string_view host, std::string_view file_name,
	                         const std::function<void(core::file_path)>& complete);
	file_op_result install(core::file_path installer_path, core::folder_path destination_folder, bool silent,
	                       bool run_app_after_install);
#endif


	// dates
	uint64_t utc_to_local(uint64_t ts);
	uint64_t local_to_utc(uint64_t ts);
	core::date_t utc_to_local(core::date_t timestamp);
	core::date_t local_to_utc(core::date_t timestamp);
	core::date_t dos_date_to_ts(uint16_t dos_date, uint16_t dos_time);
	std::string format_date_time(core::date_t d);
	std::string format_date(core::date_t d);
	std::string format_time(core::date_t d);

	std::string user_language();
}

// These formatters use platform locale/date services and therefore belong to the platform API,
// not the core umbrella.
template <>
struct std::formatter<core::date_t, char> : std::formatter<std::string_view, char>
{
	auto format(const core::date_t& d, std::format_context& ctx) const
	{
		return std::formatter<std::string_view, char>::format(platform::format_date_time(d), ctx);
	}
};

template <>
struct std::formatter<core::file_size, char> : std::formatter<std::string_view, char>
{
	auto format(const core::file_size& size, std::format_context& ctx) const
	{
		return std::formatter<std::string_view, char>::format(platform::format_size(size), ctx);
	}
};

namespace platform
{

	struct open_with_entry
	{
		std::string name;
		std::function<bool(const std::vector<core::file_path>& files, const std::vector<core::folder_path>& folders)>
		invoke;
		int weight = 0;
	};

	std::vector<open_with_entry> assoc_handlers(std::string_view ext);

	core::file_path running_app_path();

	enum class drop_effect
	{
		none,
		copy,
		move,
		link
	};

	class clipboard_bitmap
	{
	public:
		virtual ~clipboard_bitmap() = default;
		virtual file_op_result save(core::folder_path save_path, std::string_view name, bool as_png) = 0;
	};

	using clipboard_bitmap_ptr = std::shared_ptr<clipboard_bitmap>;

	class clipboard_data
	{
	public:
		struct description
		{
			std::string first_name;
			drop_effect preferred_drop_effect = drop_effect::none;
			int count = 0;
			bool has_readonly = false;
		};

		virtual bool has_drop_files() const = 0;
		virtual bool has_bitmap() const = 0;
		virtual description files_description() const = 0;
		virtual core::file_path first_path() const = 0;
		// Every path the drop carries, in the order it carries them. first_path() answers the one a
		// target that opens a single item needs; a target that appends to a list needs all of them.
		virtual std::vector<core::file_path> drop_paths() const = 0;

		virtual file_op_result drop_files(core::folder_path save_path, drop_effect effect) = 0;
		virtual clipboard_bitmap_ptr capture_bitmap() = 0;
		virtual file_op_result save_bitmap(core::folder_path save_path, std::string_view name, bool as_png) = 0;
	};

	drop_effect perform_drag(const std::any& frame_handle, const std::vector<core::file_path>& files,
	                         const std::vector<core::folder_path>& folders);


	using clipboard_data_ptr = std::shared_ptr<clipboard_data>;

	clipboard_data_ptr clipboard();

	// A picture the clipboard offers, made only when a target asks for it, as often as one asks.
	class clipboard_image
	{
	public:
		virtual ~clipboard_image() = default;
		virtual core::sizei dimensions() const = 0;
		// Writes the rows, top first, each dimensions().cx * 4 bytes of blue, green, red and alpha,
		// into pixels, which holds dimensions().cy such rows, zeroed. Throws when it cannot.
		virtual void write_pixels(std::span<uint8_t> pixels) const = 0;
	};

	using clipboard_image_ptr = std::shared_ptr<const clipboard_image>;

	// A null image offers none.
	bool set_clipboard(const std::vector<core::file_path>& files, const std::vector<core::folder_path>& folders,
	                   clipboard_image_ptr image, bool is_move);
	bool set_clipboard(std::string_view text);
	std::optional<std::string> clipboard_text();

#ifndef _WIN32
	// Where clipboard text goes when the clipboard belongs to the window layer, which holds the display
	// connection: libdesktop-render's installs its own when it starts. Until then there is none.
	void install_text_clipboard(std::function<std::optional<std::string>()> read,
	                            std::function<bool(std::string_view)> write);
#endif

	class setting_file
	{
	public:
		virtual bool root_created() const = 0;

		virtual bool write(std::string_view section, std::string_view name, uint32_t v) = 0;
		virtual bool write(std::string_view section, std::string_view name, uint64_t v) = 0;
		virtual bool write(std::string_view section, std::string_view name, std::string_view v) = 0;
		virtual bool write(std::string_view section, std::string_view name, core::cspan cs) = 0;

		virtual bool read(std::string_view section, std::string_view name, uint32_t& v) const = 0;
		virtual bool read(std::string_view section, std::string_view name, uint64_t& v) const = 0;
		virtual bool read(std::string_view section, std::string_view name, std::string& v) const = 0;
		virtual bool read(std::string_view section, std::string_view name, uint8_t* data, size_t& len) const = 0;
	};

	using setting_file_ptr = std::shared_ptr<setting_file>;

	// The single, process-wide settings store: the backend (registry vs INI) is resolved once
	// and all access is synchronised. This is the store all persistence should use.
	setting_file_ptr settings();

	// Low-level settings backends. Prefer settings() above; these are exposed only so tests can
	// exercise each backend directly.
	setting_file_ptr create_registry_settings();
	setting_file_ptr create_registry_settings(std::string_view root_key);
	setting_file_ptr create_ini_file_settings();
	setting_file_ptr create_ini_file_settings(core::folder_path folder);

	// Crash watchdog: a persisted marker recording that a graphics subsystem was active.
	// Set at safe times (subsystem start), cleared on clean shutdown; if a flag is still
	// set at the next launch the previous run crashed while that subsystem was in use, so
	// the app falls back (see apply_gpu_crash_guard). gpu_render disables GPU UI rendering;
	// hw_video_decode disables only hardware video decoding, leaving GPU rendering on.
	enum class crash_guard
	{
		gpu_render,
		hw_video_decode,
	};

	void set_crash_guard(crash_guard g, bool active);
	bool read_crash_guard(crash_guard g);
	void fail_crash_guard(crash_guard g);
	bool crash_guard_failed(crash_guard g);
	void suppress_crash_guard(crash_guard g, bool suppress);
	bool crash_guard_suppressed(crash_guard g);

	template <typename T>
	struct queue
	{
		core::mutex _rw;
		_Guarded_by_(_rw) std::deque<T> _storage;

		bool dequeue(T& result)
		{
			core::exclusive_lock lock_dec(_rw);
			if (_storage.empty()) return false;
			result = std::move(_storage.front());
			_storage.pop_front();
			return true;
		}

		bool enqueue(T f)
		{
			core::exclusive_lock lock_dec(_rw);
			const auto was_empty = _storage.empty();
			_storage.emplace_back(std::move(f));
			return was_empty;
		}

		// One lock and one empty-to-nonempty answer for a whole group, so a producer holding a batch
		// signals its consumer once rather than once per element.
		template <typename C>
		bool enqueue_all(C&& items)
		{
			core::exclusive_lock lock_dec(_rw);
			const auto was_empty = _storage.empty();
			for (auto&& i : items) _storage.emplace_back(std::move(i));
			return was_empty && !_storage.empty();
		}

		void reset_and_enqueue(T f)
		{
			// Superseded tasks own captured surfaces, item lists and shared state, so they are swapped
			// out and destroyed after the lock is released rather than run down inside it.
			std::deque<T> discarded;

			{
				core::exclusive_lock lock_dec(_rw);
				std::swap(discarded, _storage);
				_storage.emplace_back(std::move(f));
			}
		}

		std::deque<T> dequeue_all()
		{
			std::deque<T> result;

			{
				core::exclusive_lock lock_dec(_rw);
				std::swap(result, _storage);
			}

			return result;
		}

		bool empty()
		{
			core::shared_lock lock_dec(_rw);
			return _storage.empty();
		}
	};

	class task_queue
	{
	public:
		class task_t
		{
			std::function<void()> _task;
			int64_t _queued_us = 0;
			int64_t _ready_us = 0;

		public:
			task_t() = default;

			template <class F>
				requires (!std::same_as<std::remove_cvref_t<F>, task_t>)
			task_t(F&& task) : _task(std::forward<F>(task)), _queued_us(core::now_us()), _ready_us(_queued_us)
			{
			}

			void ready_after(const uint32_t delay_ms)
			{
				_ready_us = _queued_us + static_cast<int64_t>(delay_ms) * 1000;
			}

			int64_t queued_at_us() const
			{
				return _queued_us;
			}

			int64_t ready_at_us() const
			{
				return _ready_us;
			}

			uint64_t wait_us(const int64_t start_us = core::now_us()) const
			{
				return start_us > _ready_us ? static_cast<uint64_t>(start_us - _ready_us) : 0;
			}

			void operator()() const
			{
				_task();
			}
		};

		queue<task_t> _q;
		core::thread_event _event;

		// Returned by delay_before_ready_ms when there is nothing to run, so the worker blocks on its
		// events rather than on a duration.
		static constexpr uint32_t wait_indefinitely = ~0u;

		task_queue() : _event(false, false)
		{
		}

		bool dequeue(task_t& result)
		{
			return _q.dequeue(result);
		}

		std::deque<task_t> dequeue_all()
		{
			return _q.dequeue_all();
		}

		void enqueue(task_t f)
		{
			// Signal only the empty-to-nonempty transition; the worker drains the complete batch.
			if (_q.enqueue(std::move(f)))
			{
				_event.set();
			}
		}

		void reset_and_enqueue(task_t f)
		{
			_q.reset_and_enqueue(std::move(f));
			_event.set();
		}

		// Trailing-edge debounce. A newer request pushes the deadline out, so a burst settles once, and
		// the worker spends the delay in its event wait rather than a task spending it asleep on the
		// thread - which is what stopped these queues sharing a worker with anything else.
		// The deadline belongs to the queue rather than the task, so only supersede-on-enqueue queues
		// should use this.
		void enqueue_after(const uint32_t delay_ms, task_t f)
		{
			_run_after_ms.store(core::now_ms() + delay_ms, std::memory_order_relaxed);
			f.ready_after(delay_ms);
			_q.reset_and_enqueue(std::move(f));
			_event.set();
		}

		// 0 means drain now. Every queue that never calls enqueue_after answers 0 whenever it holds
		// anything, which is the behaviour it had before deadlines existed.
		uint32_t delay_before_ready_ms()
		{
			if (_q.empty()) return wait_indefinitely;

			const auto due = _run_after_ms.load(std::memory_order_relaxed);
			const auto now = core::now_ms();
			return due > now ? static_cast<uint32_t>(due - now) : 0;
		}

	private:
		// A core::now_ms stamp; zero leaves the queue ready as soon as it is signalled.
		std::atomic<int64_t> _run_after_ms = 0;
	};

	class threads
	{
		core::mutex _rw;
		std::vector<std::thread> _threads;
		// Latched by clear(), so a thread started on demand can never be created after the join.
		bool _stopped = false;

	public:
		~threads()
		{
			clear();
		}

		// Refuses to start once clear() has run. That refusal is what makes an on-demand start safe from
		// any thread: the caller learns no worker will service its queue, rather than leaking a thread
		// that outlives the objects it captured. There is deliberately no unchecked form.
		template <typename F>
		bool start_if_running(F&& f)
		{
			core::exclusive_lock lock_dec(_rw);
			if (_stopped) return false;
			_threads.emplace_back(std::thread(f));
			return true;
		}

		void clear()
		{
			std::vector<std::thread> threads;

			{
				core::exclusive_lock lock_dec(_rw);
				_stopped = true;
				std::swap(threads, _threads);
			}

			for (auto&& t : threads) t.join();
		}
	};

	class thread_init
	{
		uint32_t _hr = 0;

	public:
		thread_init();
		~thread_init();
	};

	// RAII helper that registers the current thread with the Multimedia Class
	// Scheduler Service (MMCSS) as a "Pro Audio" task so it is not starved by
	// heavier CPU work (notably video decoding) - starvation of the audio thread
	// lets the WASAPI ring underrun and replay its stale contents.
	class media_thread_priority
	{
		void* _task = nullptr;

	public:
		media_thread_priority();
		~media_thread_priority();
	};

	// The thread the application's UI runs on. The entry point names it before anything else runs;
	// services that show system UI, such as the share sheet or a mail draft, are called on it.
	void set_ui_thread();
	bool is_ui_thread();

	extern uint32_t wait_for_timeout;
	uint32_t wait_for(const std::vector<std::reference_wrapper<core::thread_event>>& events, uint32_t timeout_ms,
	                  bool wait_all);
	using attachments_t = std::vector<std::pair<std::string, core::file_path>>;

	enum class mapi_send_result
	{
		sent,
		canceled,
		failed
	};

	mapi_send_result classify_mapi_send_result(uint32_t result_code);
	mapi_send_result mapi_send(std::string_view to, std::string_view subject, std::string_view text,
	                           const attachments_t& attachments);

	// Whether the system share sheet can offer files to other apps. Where it cannot, the command
	// falls back to an email draft rather than offering a sheet that will not open.
	bool can_share_files();

	// Opens the system share sheet for the files, anchored to the application window. UI thread
	// only. The receiving app reads the files after this returns, so they must stay where they are.
	bool share_files(const std::vector<core::file_path>& files, std::string_view title);

	void sync_app_window_enabled();
	uint32_t caret_blink_time();
	uint32_t process_id();
	extern core::thread_event event_exit;

	// Claimed while this launch has not yet reached the user, and released once it has. Answers false
	// when another instance already holds it, which is a concurrent launch rather than a repeat of one.
	bool claim_startup_scope();
	void release_startup_scope();



	////////////////////////////////
	/// Network

	bool is_online();

	using web_params = std::vector<std::pair<std::string, std::string>>;

	enum class web_request_verb
	{
		POST,
		GET
	};

	struct web_request
	{
		std::string command;
		std::string path;

		web_params query;
		web_params headers;
		web_params form_data;

		std::string file_form_data_name;
		std::string file_name;
		core::file_path file_path;

		core::file_path download_file_path;

		web_request_verb verb = web_request_verb::GET;
	};

	struct web_response
	{
		std::string headers;
		std::string body;
		std::string content_type;
		int status_code = 0;
	};

	struct web_host;
	using web_host_ptr = std::shared_ptr<web_host>;

	web_host_ptr connect_to_host(std::string_view host, bool secure = true, int port = 0);
	web_response send_request(const web_host_ptr& host, const web_request& req);
}
