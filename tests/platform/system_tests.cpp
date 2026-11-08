// libdesktop-platform's suite: the application-facing services.

#include "libdesktop/platform.h"
#include "test.h"
#include "platform/test_support.h"

#include <clocale>

using namespace std::literals;



static void should_name_files_after_the_application()
{
	// The log and the temporary names are the application's name in lower case.
	assert_equal_strict("libdesktop-tests"sv, core::app_file_stem(), "file stem");
	assert_equal_strict("libdesktop-tests.log"sv, platform::log_path.name(), "log");
	assert_equal_strict("libdesktop-tests.previous.log"sv, platform::previous_log_path.name(), "previous log");
	assert_equal(true, platform::temp_file(".tmp"sv, _temps.folder()).name().sv().starts_with("libdesktop-tests_"),
	             "temporary name");
}

// A query that fails is not proof the file is gone: a caller that deletes or overwrites on
// "not there" must be able to tell a removed file from one it simply could not read.
static void should_report_file_presence()
{
	const auto scratch = _temps.next_folder("file-presence");
	const auto present = scratch.combine_file("present.txt");
	{
		std::ofstream fs(platform::to_stream_path(present));
		fs << "content";
	}

	const auto found = platform::file_attributes(present);
	assert_equal(true, found.exists(), "existing file is found");
	assert_equal(false, found.confirmed_missing(), "existing file is not missing");

	const auto missing = platform::file_attributes(scratch.combine_file("missing.txt"));
	assert_equal(false, missing.exists(), "removed file does not exist");
	assert_equal(true, missing.confirmed_missing(), "removed file is confirmed missing");

	// A path under a folder that is not there is absent for the same reason, not a failure.
	const auto missing_folder = platform::file_attributes(scratch.combine("gone").combine_file("missing.txt"));
	assert_equal(true, missing_folder.confirmed_missing(), "file under a missing folder is confirmed missing");

	// An empty file must not read as absent just because it has no bytes.
	const auto empty_path = scratch.combine_file("empty.txt");
	{
		std::ofstream fs(platform::to_stream_path(empty_path));
	}
	const auto empty = platform::file_attributes(empty_path);
	assert_equal(true, empty.exists(), "empty file exists");
	assert_equal(0, static_cast<int>(empty.size), "empty file has no bytes");

	assert_equal(true, platform::file_attributes(scratch).exists(), "existing folder is found");
	assert_equal(true, platform::file_attributes(scratch.combine("gone")).confirmed_missing(),
	             "removed folder is confirmed missing");

	// Enumeration only ever reports what it found, so those records are never left unknown.
	const auto contents = platform::iterate_file_items(scratch, false);
	assert_equal(2, static_cast<int>(contents.files.size()), "both files enumerated");
	assert_equal(true, std::ranges::all_of(contents.files, [](const platform::file_info& f)
	             {
		             return f.attributes.exists();
	             }),
	             "enumerated files are found");

	assert_equal(false, platform::file_attributes_t{}.exists(), "unqueried attributes do not exist");
	assert_equal(false, platform::file_attributes_t{}.confirmed_missing(),
	             "unqueried attributes are not confirmed missing");
}

static void should_map_files()
{
	const auto scratch = _temps.next_folder("map-file");
	const auto path = scratch.combine_file("mapped.txt");

	// Larger than one allocation granularity so a window can be placed past the first boundary.
	std::string content;
	content.reserve(200'000);
	for (auto i = 0; content.size() < 200'000; ++i) content += std::format("line {}\n", i);
	write_test_file(path, content);

	const auto whole = platform::map_file(path);
	assert_equal(true, whole != nullptr, "whole file maps");
	assert_equal(content.size(), static_cast<size_t>(whole->file_size()), "mapped size matches the file");
	assert_equal(content.size(), whole->data().size, "whole view covers the file");
	assert_equal(0, memcmp(whole->data().data, content.data(), content.size()), "mapped bytes match the file");

	// Trimmed pages must still read back correctly - the mapping stays valid, it just faults in.
	whole->release_working_set();
	assert_equal(0, memcmp(whole->data().data, content.data(), content.size()), "bytes survive a working-set release");

	const auto windowed = platform::map_file(path, platform::map_mode::windowed);
	assert_equal(true, windowed != nullptr, "windowed file maps");
	assert_equal(true, windowed->data().empty(), "windowed mapping has no view until a window is set");

	// An offset that is not a multiple of the allocation granularity must still return the exact
	// bytes asked for, because the granularity alignment is the mapping's business, not a caller's.
	constexpr uint64_t odd_offset = 70'001;
	constexpr uint64_t window_len = 4'096;
	const auto window = windowed->set_window(odd_offset, window_len);
	assert_equal(static_cast<size_t>(window_len), window.size, "window is the requested length");
	assert_equal(0, memcmp(window.data, content.data() + odd_offset, window_len), "window starts at the offset");

	// A window running past the end is clamped rather than refused.
	const auto tail = windowed->set_window(content.size() - 10, 4'096);
	assert_equal(10_z, tail.size, "window past the end is clamped to the file");
	assert_equal(0, memcmp(tail.data, content.data() + content.size() - 10, 10), "clamped window holds the tail");

	assert_equal(true, windowed->set_window(content.size(), 16).empty(), "window at the end is empty");

	assert_equal(true, platform::map_file(scratch.combine_file("missing.txt")) == nullptr,
	             "a missing file does not map");

	// An empty file cannot be mapped at all, which is an answer rather than a fault.
	const auto empty_path = scratch.combine_file("empty.txt");
	write_test_file(empty_path, {});
	assert_equal(true, platform::map_file(empty_path) == nullptr, "an empty file does not map");
}

// The write used to open the destination with create, truncating whatever was there before the
// first byte was written: a short write or a full disk left a partial file where a whole one used
// to be. A zero-length write compounded it, reporting success when the file could not be opened at
// all because zero of zero bytes had been written.
static void should_save_a_blob_without_truncating_the_destination()
{
	const auto scratch = _temps.next_folder("blob-save");
	const auto path = scratch.combine_file("payload.bin");

	const core::blob whole = {1, 2, 3, 4};
	assert_equal(true, platform::blob_save_to_file(whole, path), "a blob is saved");
	assert_equal(uint64_t{4}, platform::file_attributes(path).size, "and lands whole");

	const core::blob empty;
	assert_equal(true, platform::blob_save_to_file(empty, path), "an empty blob replaces it");
	assert_equal(uint64_t{0}, platform::file_attributes(path).size, "with an empty file");

	// Nothing can be written here, so nothing is reported as written.
	const auto missing = scratch.combine("no-such-folder").combine_file("payload.bin");
	assert_equal(false, platform::blob_save_to_file(whole, missing), "an unwritable destination fails");
	assert_equal(false, platform::blob_save_to_file(empty, missing), "and does so for an empty blob too");

	// The staging file is moved into place, not left beside it.
	const auto contents = platform::iterate_file_items(scratch, true);
	assert_equal(1_z, contents.files.size(), "the save leaves only the destination behind");
}

static void should_hash_a_file_with_sha256()
{
	const auto scratch = _temps.next_folder("sha256");
	const auto path = scratch.combine_file("payload.bin");
	write_test_file(path, "abc");

	const auto digest = platform::file_sha256(path);
	assert_equal(true, digest.has_value(), "a readable file has a SHA-256 digest");
	if (digest)
	{
		const core::sha256_digest expected = {
			0xba, 0x78, 0x16, 0xbf, 0x8f, 0x01, 0xcf, 0xea,
			0x41, 0x41, 0x40, 0xde, 0x5d, 0xae, 0x22, 0x23,
			0xb0, 0x03, 0x61, 0xa3, 0x96, 0x17, 0x7a, 0x9c,
			0xb4, 0x10, 0xff, 0x61, 0xf2, 0x00, 0x15, 0xad
		};
		assert_equal(true, *digest == expected, "the SHA-256 bytes are standard");
	}
	{
		const auto ordinary = platform::open_file(path, platform::file_open_mode::read);
		assert_equal(true, ordinary != nullptr, "a restrictive ordinary reader is held");
		assert_equal(true, platform::file_sha256(path) == digest, "hashing requires no delete access");
	}
	const auto reader = platform::open_file_lock(path, platform::file_lock_mode::read);
	assert_equal(true, reader != nullptr, "a content verification token opens");
	if (reader)
	{
		reader->seek(1, platform::file::whence::begin);
		assert_equal(true, platform::file_sha256(reader) == digest, "hashing uses the held file");
		assert_equal(uint64_t{1}, reader->pos(), "positional hashing preserves the caller's platform::cursor");
		const auto moved = scratch.combine_file("renamed.bin");
		assert_equal(true, platform::move_file(path, moved, true, false).success(),
			"a read token permits renaming");
		assert_equal(true, platform::file_sha256(reader) == digest, "the renamed held platform::file still hashes");
		assert_equal(false, platform::file_sha256(path).has_value(), "the old pathname is not reopened");
	}
	assert_equal(false, platform::file_sha256(core::file_path(scratch.text())).has_value(),
		"a directory is not hashed as an empty platform::file");
}

// An INI line cannot hold a line break. A setting holding one item per line - the folders a collection
// adds, say - came back as its first line after a restart, and a later line holding '=' came back as a
// setting of its own.
static void should_keep_line_breaks_in_ini_settings()
{
	const auto folder = _temps.next_folder("ini-line-breaks");
	const std::vector<std::string> values = {
		"\"C:\\Photos\"\n\"C:\\Foto's\"",
		"C:\\Photos\r\nD:\\Raw\r\n",
		"\n",
		"first\nsecond=line",
		// The escape written as text is text again when read.
		"\x10" "n stays as written",
		// Nor is a backslash an escape: settings hold Windows paths.
		"C:\\new\\raw \\n \\\\server\\share",
	};

	const auto settings = platform::create_ini_file_settings(folder);
	for (size_t i = 0; i < values.size(); ++i)
	{
		assert_equal(true, settings->write("lines", std::format("value{}", i), values[i]), "the value is written");
	}

	const auto reopened = platform::create_ini_file_settings(folder);
	for (size_t i = 0; i < values.size(); ++i)
	{
		std::string read;
		assert_equal(true, reopened->read("lines", std::format("value{}", i), read), "the value is read after a restart");
		assert_equal(values[i], read, "exactly as it was written");
	}

	std::string stray;
	assert_equal(false, reopened->read("lines", "second", stray), "no line of a value is read as a setting");
}

// A value cleared to nothing read back as missing, so the default it replaced came back after a
// restart: a removed favourite search, or a blanked artist name a batch edit then wrote into files.
static void should_read_back_an_empty_setting()
{
	const auto folder = _temps.next_folder("ini-empty");
	const auto settings = platform::create_ini_file_settings(folder);
	assert_equal(true, settings->write("values", "cleared", std::string_view{}), "an empty value is written");
	assert_equal(true, settings->write("values", "count", std::string_view{}), "and an empty number");

	const auto reopened = platform::create_ini_file_settings(folder);
	std::string text = "default";
	assert_equal(true, reopened->read("values", "cleared", text), "an empty value is read after a restart");
	assert_equal(std::string{}, text, "as empty, not as the default");

	std::string missing = "default";
	assert_equal(false, reopened->read("values", "never", missing), "a value never written is still missing");
	assert_equal("default"s, missing, "and leaves the default");

	uint32_t number = 7;
	assert_equal(false, reopened->read("values", "count", number), "an empty number keeps its default");
	assert_equal(7u, number, "untouched");
}

// A destination that could not be checked was taken for one not there, so the backup asked for was
// skipped while the replacement still went through - an original lost with no recovery point.
static void should_not_replace_unchecked_destination_without_its_backup()
{
	const auto folder = _temps.next_folder("replace-unknown");
	const auto destination = folder.combine_file("photo.jpg");
	const auto replacement = folder.combine_file("photo.new");
	const auto backup = folder.combine_file("photo.original.jpg");
	write_test_file(destination, "original");
	write_test_file(replacement, "replacement");

	platform::test_replace_destination_presence = [](const core::file_path&) { return platform::file_presence::unknown; };
	const core::scope_exit restore([] { platform::test_replace_destination_presence = {}; });

	assert_equal(true, platform::replace_file(destination, replacement, true).failed(), "the replace is refused");
	assert_equal("original"s, read_test_file(destination), "the destination is untouched");
	assert_equal(true, platform::exists(replacement), "the replacement is kept");
	assert_equal(false, platform::exists(backup), "and no backup was begun");

	platform::test_replace_destination_presence = {};
	assert_equal(true, platform::replace_file(destination, replacement, true).success(),
	             "a destination that is checked is replaced");
	assert_equal("replacement"s, read_test_file(destination), "with the new bytes");
	assert_equal("original"s, read_test_file(backup), "after its backup is made");
}

static std::string replace_file_bytes(const core::file_path path, const std::string_view initial, const int64_t start,
                                      const int64_t replace, const std::string_view replacement)
{
	write_test_file(path, initial);
	{
		platform::buffered_file file(platform::open_file(path, platform::file_open_mode::read_write));
		assert_equal(true, file.insert(std::bit_cast<const uint8_t*>(replacement.data()),
		                               static_cast<int64_t>(replacement.size()), start, replace),
		             "replacement succeeds");
	}
	return read_test_file(path);
}

static void should_replace_bytes_in_files()
{
	const auto scratch = _temps.next_folder("file-insert");
	const auto path = scratch.combine_file("payload.bin");

	assert_equal("0123X789", replace_file_bytes(path, "0123456789", 4, 3, "X"),
	             "shrinking keeps prefix and tail");
	assert_equal("0123ABCDE789", replace_file_bytes(path, "0123456789", 4, 3, "ABCDE"),
	             "growing keeps prefix and tail");
	assert_equal("0123XYZ789", replace_file_bytes(path, "0123456789", 4, 3, "XYZ"),
	             "equal-size replacement");
	assert_equal("X3456789", replace_file_bytes(path, "0123456789", 0, 3, "X"),
	             "replace at beginning");
	assert_equal("012345X", replace_file_bytes(path, "0123456789", 6, 4, "X"),
	             "replace through end");
	assert_equal("012345X", replace_file_bytes(path, "012345", 6, 0, "X"),
	             "append with empty tail");

	std::string large;
	large.reserve(core::sixty_four_k + 32);
	for (auto i = 0; i < static_cast<int>(core::sixty_four_k) + 32; ++i) large += static_cast<char>('A' + i % 26);
	const auto large_actual = replace_file_bytes(path, large, 8, 3, "x");
	auto large_expected = large;
	large_expected.replace(8, 3, "x");
	assert_equal(large_expected, large_actual, "shrinking tail crosses a 64 KiB block boundary");

	write_test_file(path, "0123456789");
	{
		platform::buffered_file file(platform::open_file(path, platform::file_open_mode::read_write));
		assert_equal(false, file.insert(reinterpret_cast<const uint8_t*>("X"), 1, 20, 1), "invalid range fails");
	}
	assert_equal("0123456789", read_test_file(path), "failed replacement leaves bytes unchanged");
}

static void should_convert_local_dates_with_date_specific_rules()
{
	assert_equal(uint64_t{0}, platform::utc_to_local(0), "an absent UTC timestamp remains absent");
	assert_equal(uint64_t{0}, platform::local_to_utc(0), "an absent local timestamp remains absent");
	const std::array days = {
		core::day_t{2024, 1, 15, 12, 0, 0},
		core::day_t{2024, 7, 15, 12, 0, 0},
		core::day_t{2024, 3, 10, 3, 30, 0},
		core::day_t{2024, 11, 3, 3, 30, 0}
	};
	for (const auto& day : days)
	{
		std::tm local{};
		local.tm_year = day.year - 1900;
		local.tm_mon = day.month - 1;
		local.tm_mday = day.day;
		local.tm_hour = day.hour;
		local.tm_min = day.minute;
		local.tm_isdst = -1;
		const auto unix_time = std::mktime(&local);
		constexpr uint64_t fraction = 1234567;
		const auto expected = (static_cast<int64_t>(unix_time) + 11644473600ll) * 10000000ll + fraction;
		const auto wall = core::date_t(day.year, day.month, day.day, day.hour, day.minute, 0).to_int64() + fraction;
		assert_equal(expected, platform::local_to_utc(wall), "local conversion matches the date-specific timezone rule");
	}
}

static void should_signal_and_replace_pending_queue_work()
{
	platform::queue<int> values;
	assert_equal(true, values.enqueue(1), "first enqueue signals empty transition");
	assert_equal(false, values.enqueue(2), "additional queued work does not signal again");

	int value = 0;
	assert_equal(true, values.dequeue(value), "first value dequeued");
	assert_equal(false, values.enqueue(3), "queue remains nonempty until the final value is dequeued");
	assert_equal(true, values.dequeue(value), "second value dequeued");
	assert_equal(true, values.dequeue(value), "third value dequeued");
	assert_equal(true, values.enqueue(4), "enqueue signals after queue becomes empty again");

	platform::task_queue tasks;
	auto executed = 0;
	tasks.enqueue([&executed] { executed = 1; });
	tasks.reset_and_enqueue([&executed] { executed = 2; });
	for (const auto& task : tasks.dequeue_all()) task();
	assert_equal(2, executed, "reset_and_enqueue retains only the latest pending task");

	tasks.enqueue_after(1000, [] {});
	const auto delayed = tasks.dequeue_all();
	assert_equal(1, static_cast<int>(delayed.size()), "delayed work remains queued");
	const auto ready_at = delayed.front().ready_at_us();
	assert_equal(1'000'000, static_cast<int>(ready_at - delayed.front().queued_at_us()),
	             "enqueue_after moves readiness by the requested delay");
	assert_equal(0, static_cast<int>(delayed.front().wait_us(ready_at - 1)),
	             "the intentional delay is not queue wait");
	assert_equal(250, static_cast<int>(delayed.front().wait_us(ready_at + 250)),
	             "time after the deadline is queue wait");
}

#ifndef _WIN32
static void should_allow_the_caller_to_own_copy_fallback()
{
	const auto root = _temps.next_folder("rename-only");
	const auto source = root.combine_file("source.txt");
	const auto destination = root.combine_file("destination.txt");
	write_test_file(source, "original");
	platform::test_linux_move_file_failures({true, true});
	const core::scope_exit restore([] { platform::test_linux_clear_move_file_failures(); });
	const auto refused = platform::move_file(source, destination, true, false);
	assert_equal(true, refused.failed() && static_cast<bool>(refused.error), "the unavailable rename retains its error");
	assert_equal(true, platform::exists(source) && !platform::exists(destination), "rename-only never falls back to copying");
	assert_equal(true, platform::move_file(source, destination, true).success(), "the existing move policy still copies");
	assert_equal(true, !platform::exists(source) && platform::exists(destination), "the default completes the move");
	assert_equal_strict("original", read_test_file(destination), "the copied contents are intact");
}

#endif

void register_platform_tasks_tests(test_registry& tests);
void register_platform_media_tests(test_registry& tests);
void register_platform_services_tests(test_registry& tests);
void register_platform_shell_tests(test_registry& tests);
#ifdef _WIN32
void register_platform_system_win_tests(test_registry& tests);
void register_platform_render_win_tests(test_registry& tests);
void register_os_tests(test_registry& tests);
#endif

void register_platform_system_tests(test_registry& tests)
{
	tests.add("Should name files after the application"s, should_name_files_after_the_application);
	tests.add("Should report file presence"s, should_report_file_presence);
	tests.add("Should map files"s, should_map_files);
	tests.add("Should save a blob without truncating the destination"s, should_save_a_blob_without_truncating_the_destination);
	tests.add("Should hash a file with SHA-256"s, should_hash_a_file_with_sha256);
	tests.add("Should keep line breaks in INI settings"s, should_keep_line_breaks_in_ini_settings);
	tests.add("Should read back an empty setting"s, should_read_back_an_empty_setting);
	tests.add("Should not replace an unchecked destination without its backup"s,
		should_not_replace_unchecked_destination_without_its_backup);
	tests.add("Should replace bytes in files"s, should_replace_bytes_in_files);
	tests.add("Should convert local dates with date specific rules"s, should_convert_local_dates_with_date_specific_rules);
	tests.add("Should signal and replace pending queue work"s, should_signal_and_replace_pending_queue_work);
#ifdef _WIN32
	register_platform_system_win_tests(tests);
	register_platform_render_win_tests(tests);
#else
	tests.add("Should allow the caller to own copy fallback"s, should_allow_the_caller_to_own_copy_fallback);
#endif
}
