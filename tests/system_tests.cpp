// system_tests.cpp — libdesktop-system's tests: the foundation and the OS services, checked without
// a window, on Windows and Linux. The tests came with the code from Diffractor's test_util.cpp and
// run here as they ran there; run with a filter (wildcards allowed) to run some of them.

#include "libdesktop/system.h"
#include "libdesktop/util_base64.h"
#include "libdesktop/util_kdtree.h"
#include "libdesktop/util_simd.h"
#include "system_test.h"

#include <cstdio>

// The application's name, which util.h asks the application to define.
const std::string_view s_app_name = "LibDesktop-Tests";

static void should_name_files_after_the_application()
{
	// The log and the temporary names are the application's name in lower case.
	assert_equal_strict("libdesktop-tests"sv, df::app_file_stem(), "file stem");
	assert_equal_strict("libdesktop-tests.log"sv, df::log_path.name(), "log");
	assert_equal_strict("libdesktop-tests.previous.log"sv, df::previous_log_path.name(), "previous log");
	assert_equal(true, platform::temp_file(".tmp"sv, _temps.folder()).name().sv().starts_with("libdesktop-tests_"),
	             "temporary name");
}

static void assert_assertion_fails(const std::function<void()>& f, const std::string_view name)
{
	auto failed = false;

	try
	{
		f();
	}
	catch (const test_assert_exception&)
	{
		failed = true;
	}

	assert_equal(true, failed, name);
}

static void should_icmp_natural()
{
	// Test basic numeric comparison - the key bug fix
	// Files like 43_100 should come after 43_99, not between 43_10 and 43_11
	assert_equal(true, str::icmp_natural("43_09", "43_10") < 0, "43_09 < 43_10");
	assert_equal(true, str::icmp_natural("43_10", "43_11") < 0, "43_10 < 43_11");
	assert_equal(true, str::icmp_natural("43_10", "43_100") < 0, "43_10 < 43_100");
	assert_equal(true, str::icmp_natural("43_99", "43_100") < 0, "43_99 < 43_100");
	assert_equal(true, str::icmp_natural("43_100", "43_101") < 0, "43_100 < 43_101");

	// Verify the order reported in the bug is fixed
	assert_equal(true, str::icmp_natural("43_09", "43_100") < 0, "43_09 < 43_100");
	assert_equal(true, str::icmp_natural("43_11", "43_100") < 0, "43_11 < 43_100");

	// Test equality
	assert_equal(0, str::icmp_natural("file10", "file10"), "equal strings");
	assert_equal(0, str::icmp_natural("", ""), "empty strings");

	// Test case insensitivity
	assert_equal(0, str::icmp_natural("File10", "file10"), "case insensitive");
	assert_equal(0, str::icmp_natural("FILE10", "file10"), "case insensitive upper");

	// Test basic natural ordering
	assert_equal(true, str::icmp_natural("file1", "file2") < 0, "file1 < file2");
	assert_equal(true, str::icmp_natural("file2", "file10") < 0, "file2 < file10");
	assert_equal(true, str::icmp_natural("file9", "file10") < 0, "file9 < file10");
	assert_equal(true, str::icmp_natural("file10", "file11") < 0, "file10 < file11");
	assert_equal(true, str::icmp_natural("file19", "file20") < 0, "file19 < file20");
	// Issue #197: "43_100" sorted between "43_10" and "43_11" (lexicographic instead of numeric).
	assert_equal(true, str::icmp_natural("file99", "file100") < 0, "file99 < file100");
	assert_equal(true, str::icmp_natural("file100", "file1000") < 0, "file100 < file1000");

	// Test reverse ordering
	assert_equal(true, str::icmp_natural("file10", "file9") > 0, "file10 > file9");
	assert_equal(true, str::icmp_natural("file100", "file99") > 0, "file100 > file99");

	// Test with different prefixes
	assert_equal(true, str::icmp_natural("a10", "b1") < 0, "a10 < b1");
	assert_equal(true, str::icmp_natural("img001", "img002") < 0, "img001 < img002");
	assert_equal(true, str::icmp_natural("img009", "img010") < 0, "img009 < img010");

	// Test numbers at the start
	assert_equal(true, str::icmp_natural("1file", "2file") < 0, "1file < 2file");
	assert_equal(true, str::icmp_natural("9file", "10file") < 0, "9file < 10file");
	assert_equal(true, str::icmp_natural("10file", "100file") < 0, "10file < 100file");

	// Test multiple number groups
	assert_equal(true, str::icmp_natural("file1-1", "file1-2") < 0, "file1-1 < file1-2");
	assert_equal(true, str::icmp_natural("file1-9", "file1-10") < 0, "file1-9 < file1-10");
	assert_equal(true, str::icmp_natural("file1-10", "file2-1") < 0, "file1-10 < file2-1");

	// Test leading zeros
	assert_equal(true, str::icmp_natural("file007", "file7") > 0, "file007 > file7 (more leading zeros)");
	assert_equal(true, str::icmp_natural("file07", "file007") < 0,
	             "file07 < file007 (fewer leading zeros)");
	assert_equal(0, str::icmp_natural("file007", "file007"), "same with leading zeros");

	// Test purely numeric strings
	assert_equal(true, str::icmp_natural("1", "2") < 0, "1 < 2");
	assert_equal(true, str::icmp_natural("9", "10") < 0, "9 < 10");
	assert_equal(true, str::icmp_natural("99", "100") < 0, "99 < 100");
	assert_equal(true, str::icmp_natural("999", "1000") < 0, "999 < 1000");

	// Test strings with no numbers
	assert_equal(true, str::icmp_natural("abc", "abd") < 0, "abc < abd");
	assert_equal(true, str::icmp_natural("abc", "abcd") < 0, "abc < abcd");
	assert_equal(0, str::icmp_natural("abc", "ABC"), "abc == ABC (case insensitive)");

	// Test image sequence patterns (common use case)
	assert_equal(true, str::icmp_natural("DSC_0001.jpg", "DSC_0002.jpg") < 0, "DSC sequence");
	assert_equal(true, str::icmp_natural("DSC_0099.jpg", "DSC_0100.jpg") < 0, "DSC sequence 99-100");
	assert_equal(true, str::icmp_natural("IMG_9999.png", "IMG_10000.png") < 0, "IMG sequence overflow");

	// A run longer than nineteen digits does not fit a uint64_t. Accumulating the value wrapped it,
	// and the wrapped result compared as something small - so a long serial number, a hash or a
	// phone-camera timestamp landed at an arbitrary point in the listing. Leading zeros have
	// already been consumed by this point, so the longer run is simply the larger number.
	assert_equal(true, str::icmp_natural("id_99999999999999999999", "id_100000000000000000000") < 0,
	             "twenty digits beats twenty");
	assert_equal(true, str::icmp_natural("id_18446744073709551615", "id_18446744073709551616") < 0,
	             "one past the widest value a uint64 holds still orders after it");
	assert_equal(true, str::icmp_natural("id_9", "id_99999999999999999999999999") < 0,
	             "a short run orders before a very long one");
	assert_equal(0, str::icmp_natural("id_99999999999999999999", "id_99999999999999999999"),
	             "and two of the same are equal");

	// 2^64 wraps to zero, so any comparison that forms the value puts it below a bare 1.
	assert_equal(true, str::icmp_natural("id_1", "id_18446744073709551616") < 0,
	             "two to the sixty-four is larger than one, not smaller");
	assert_equal(true, str::icmp_natural("id_18446744073709551616", "id_1") > 0, "and the reverse holds");
}

// EXIF text can arrive as UTF-16, and which way round it is decides whether it reads as words or
// as mojibake. Testing only the even bytes for zero recognised big-endian alone - so a
// little-endian value, which is what Windows writes, was read a byte at a time.
static void should_detect_utf16_either_way_round()
{
	const auto is_utf16 = [](const std::initializer_list<uint8_t> bytes)
	{
		const std::vector<uint8_t> v(bytes);
		return str::is_utf16(v.data(), static_cast<int>(v.size()));
	};

	// "Hi" with no mark, each way round.
	assert_equal(true, is_utf16({'H', 0, 'i', 0}), "unmarked little-endian is recognised");
	assert_equal(true, is_utf16({0, 'H', 0, 'i'}), "and so is unmarked big-endian");

	// Marked, each way round.
	assert_equal(true, is_utf16({0xff, 0xfe, 'H', 0}), "a little-endian mark is recognised");
	assert_equal(true, is_utf16({0xfe, 0xff, 0, 'H'}), "and so is a big-endian mark");

	// UTF-32 LE opens with the UTF-16 LE mark and must not be taken for it.
	assert_equal(false, is_utf16({0xff, 0xfe, 0x00, 0x00}), "a utf-32 mark is not utf-16");

	// Plain bytes stay plain.
	assert_equal(false, is_utf16({'H', 'e', 'l', 'l', 'o'}), "ascii is not utf-16");
	assert_equal(false, is_utf16({0xe2, 0x82, 0xac, 'x'}), "utf-8 is not utf-16");
	assert_equal(false, is_utf16({'H'}), "and a single byte cannot be");

	// Recognising either order is only half of it: the text is decoded the same way round it was
	// recognised, and a mark is not part of the value. Copied as it lay, big-endian text came out
	// backwards and a marked value kept an invisible U+FEFF at its start.
	const auto decode = [](const std::initializer_list<uint8_t> bytes)
	{
		const std::vector<uint8_t> v(bytes);
		return str::utf16_from_bytes(v.data(), static_cast<int>(v.size()));
	};

	assert_equal(true, decode({'H', 0, 'i', 0}) == u"Hi", "unmarked little-endian decodes");
	assert_equal(true, decode({0, 'H', 0, 'i'}) == u"Hi", "and so does unmarked big-endian");
	assert_equal(true, decode({0xff, 0xfe, 'H', 0, 'i', 0}) == u"Hi", "a little-endian mark is not part of the text");
	assert_equal(true, decode({0xfe, 0xff, 0, 'H', 0, 'i'}) == u"Hi", "and a big-endian mark sets the order");
}

static void should_cancel_superseded_tokens()
{
	std::atomic_int version = 0;
	const df::cancel_token first(version);
	const auto first_copy = first;

	assert_equal(false, first.is_cancelled(), "current token is active");
	assert_equal(false, first_copy.is_cancelled(), "copied current token is active");

	const df::cancel_token second(version);
	assert_equal(true, first.is_cancelled(), "new generation cancels previous token");
	assert_equal(true, first_copy.is_cancelled(), "new generation cancels copies of previous token");
	assert_equal(false, second.is_cancelled(), "new generation remains active");
}

static void should_parse_files_directly_under_the_root()
{
	const auto root = df::folder_path(df::windows_path_semantics ? "c:\\" : "/");
	const auto direct_text = df::windows_path_semantics ? "c:\\photo.jpg" : "/photo.jpg";
	const auto direct = df::file_path(direct_text);
	const auto combined = root.combine_file("photo.jpg");

	assert_equal(root.text(), direct.folder().text(), "root folder is preserved");
	assert_equal("photo.jpg", direct.name(), "root child keeps its name");
	assert_equal(".jpg", direct.extension(), "root child keeps its extension");
	assert_equal(direct_text, direct.pack(), "root child packs without a trailing folder separator");
	assert_equal(true, direct == combined, "direct and combined root children are equal");
	const df::ihash hash;
	assert_equal(hash(combined), hash(direct), "direct and combined root children hash alike");

	auto reused = root.combine_file("old-name.txt");
	reused.un_pack(direct_text);
	assert_equal(root.text(), reused.folder().text(), "reuse refreshes the folder");
	assert_equal("photo.jpg", reused.name(), "reuse refreshes the name");

	reused.un_pack(std::string(root.text()));
	assert_equal(root.text(), reused.folder().text(), "folder-only unpack keeps the folder");
	assert_equal(true, reused.name().is_empty(), "folder-only unpack clears a stale name");
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

	const df::blob whole = {1, 2, 3, 4};
	assert_equal(true, df::blob_save_to_file(whole, path), "a blob is saved");
	assert_equal(uint64_t{4}, platform::file_attributes(path).size, "and lands whole");

	const df::blob empty;
	assert_equal(true, df::blob_save_to_file(empty, path), "an empty blob replaces it");
	assert_equal(uint64_t{0}, platform::file_attributes(path).size, "with an empty file");

	// Nothing can be written here, so nothing is reported as written.
	const auto missing = scratch.combine("no-such-folder").combine_file("payload.bin");
	assert_equal(false, df::blob_save_to_file(whole, missing), "an unwritable destination fails");
	assert_equal(false, df::blob_save_to_file(empty, missing), "and does so for an empty blob too");

	// The staging file is moved into place, not left beside it.
	const auto contents = platform::iterate_file_items(scratch, true);
	assert_equal(1_z, contents.files.size(), "the save leaves only the destination behind");
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

static std::string replace_file_bytes(const df::file_path path, const std::string_view initial, const int64_t start,
                                      const int64_t replace, const std::string_view replacement)
{
	write_test_file(path, initial);
	{
		df::file file(platform::open_file(path, platform::file_open_mode::read_write));
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
	large.reserve(df::sixty_four_k + 32);
	for (auto i = 0; i < static_cast<int>(df::sixty_four_k) + 32; ++i) large += static_cast<char>('A' + i % 26);
	const auto large_actual = replace_file_bytes(path, large, 8, 3, "x");
	auto large_expected = large;
	large_expected.replace(8, 3, "x");
	assert_equal(large_expected, large_actual, "shrinking tail crosses a 64 KiB block boundary");

	write_test_file(path, "0123456789");
	{
		df::file file(platform::open_file(path, platform::file_open_mode::read_write));
		assert_equal(false, file.insert(reinterpret_cast<const uint8_t*>("X"), 1, 20, 1), "invalid range fails");
	}
	assert_equal("0123456789", read_test_file(path), "failed replacement leaves bytes unchanged");
}

// Verifies the append-only string interning table: one shared immutable copy per unique
// string, identity == handle equality, stability across table growth, and correct
// deduplication under concurrent interning from multiple threads.
static void should_intern_strings()
{
	const auto a = str::cache("interned-example");
	const auto b = str::cache(std::string("interned-example"));
	assert_equal(true, a == b, "same content -> same handle");
	assert_equal(true, a.id == b.id, "identity is handle equality");
	assert_equal("interned-example"s, a.str(), "round-trips content");
	assert_equal(false, str::cache("interned-example") == str::cache("interned-different"),
	             "different content -> different handle");
	assert_equal(true, str::cache(std::string_view{}).is_empty(), "empty interns to empty");

	const std::string largest(platform::memory_pool::block_size -
	                          offsetof(str::cached_string_storage_t, sz) - 1, 'x');
	assert_equal(largest.size(), str::cache(largest).size(), "largest pool record is interned");
	const std::string too_large(largest.size() + 1, 'x');
	assert_equal(true, str::cache(too_large).is_empty(), "oversized pool record is rejected");

	// Many distinct strings force table growth / rehash; handles stay valid, unique and stable.
	constexpr int n = 5000;
	std::vector<str::cached> handles;
	handles.reserve(n);
	for (auto i = 0; i < n; ++i) handles.emplace_back(str::cache(std::format("intern-word-{}", i)));

	df::hash_set<uint32_t> seen;
	for (auto i = 0; i < n; ++i)
	{
		const auto w = std::format("intern-word-{}", i);
		assert_equal(w, handles[i].str(), "content preserved after growth");
		assert_equal(true, str::cache(w) == handles[i], "re-intern returns the same handle");
		seen.insert(handles[i].id);
	}
	assert_equal(n, static_cast<int>(seen.size()), "each distinct string interned exactly once");

	// Concurrent interning of an overlapping set must still yield one handle per string.
	constexpr int thread_count = 8;
	constexpr int word_count = 500;
	std::vector<std::vector<str::cached>> per_thread(thread_count);
	std::vector<std::thread> threads;

	for (auto t = 0; t < thread_count; ++t)
	{
		threads.emplace_back([t, &per_thread]
		{
			auto& out = per_thread[t];
			out.reserve(word_count);
			for (auto i = 0; i < word_count; ++i) out.emplace_back(str::cache(std::format("shared-word-{}", i)));
		});
	}

	for (auto& th : threads) th.join();

	for (auto i = 0; i < word_count; ++i)
	{
		const auto handle = per_thread[0][i];
		for (auto t = 1; t < thread_count; ++t)
		{
			assert_equal(true, per_thread[t][i] == handle, "all threads share one interned handle");
		}
	}
}

// Base64 carries the saved password and the web service payloads, so a padding or alphabet slip
// corrupts stored credentials silently. Every length modulo 3 is covered because the padding rule
// differs per remainder.
static void should_round_trip_base64()
{
	assert_equal_strict("", base64_encode(""sv), "empty encodes to empty");
	assert_equal_strict("TQ==", base64_encode("M"sv), "one byte pads twice");
	assert_equal_strict("TWE=", base64_encode("Ma"sv), "two bytes pad once");
	assert_equal_strict("TWFu", base64_encode("Man"sv), "three bytes need no padding");

	for (auto length = 0u; length < 64u; ++length)
	{
		std::vector<uint8_t> data(length);

		for (auto i = 0u; i < length; ++i)
		{
			data[i] = static_cast<uint8_t>((i * 61u + 7u) & 0xff);
		}

		const auto encoded = base64_encode(data);
		const auto decoded = base64_decode(encoded);

		assert_equal(static_cast<uint32_t>(length), static_cast<uint32_t>(decoded.size()),
		             "decoded length matches");
		assert_equal(true, decoded == data, "base64 round trips every length");
	}

	// Every byte value must survive, not just printable ones.
	std::vector<uint8_t> all(256);
	for (auto i = 0u; i < 256u; ++i) all[i] = static_cast<uint8_t>(i);
	assert_equal(true, base64_decode(base64_encode(all)) == all, "every byte value round trips");
}

static void should_rotate_points()
{
	constexpr auto quarter_turn = 3.14159265358979323846 / 2.0;
	const pointd origin(0, 0);
	const pointd point(2, 3);

	const auto zero = point.rotate(0.0, origin);
	assert_equal(true, df::equiv(point.X, zero.X) && df::equiv(point.Y, zero.Y), "zero-angle identity");

	const auto quarter = pointd(2, 0).rotate(quarter_turn, origin);
	assert_equal(true, df::equiv(0.0, quarter.X) && df::equiv(2.0, quarter.Y), "quarter turn");

	const pointd center(10, 10);
	const auto full = point.rotate(quarter_turn * 4.0, center);
	assert_equal(true, std::abs(point.X - full.X) < 1e-9 && std::abs(point.Y - full.Y) < 1e-9,
	             "full turn around center");

	const auto rotated = point.rotate(quarter_turn, center);
	const auto restored = rotated.rotate(-quarter_turn, center);
	assert_equal(true, std::abs(point.X - restored.X) < 1e-9 && std::abs(point.Y - restored.Y) < 1e-9,
	             "inverse rotation");

	const auto dx_before = point.X - center.X;
	const auto dy_before = point.Y - center.Y;
	const auto dx_after = rotated.X - center.X;
	const auto dy_after = rotated.Y - center.Y;
	assert_equal(true, df::equiv(dx_before * dx_before + dy_before * dy_before,
	                             dx_after * dx_after + dy_after * dy_after), "distance preserved");
}

static void should_split_genre()
{
	// Genre values use ';' as the multi-value separator. Multi-word genres and
	// genres containing '&' or '/' must survive splitting intact.
	const auto parts = str::split("Rock; Pop ; Hip Hop", false, str::is_genre_separator);
	assert_equal(size_t{3}, parts.size(), "genre part count");
	assert_equal("Rock", str::trim(parts[0]), "genre 1");
	assert_equal("Pop", str::trim(parts[1]), "genre 2");
	assert_equal("Hip Hop", str::trim(parts[2]), "genre 3");

	const auto parts2 = str::split("Action & Adventure; R&B/Soul", false, str::is_genre_separator);
	assert_equal(size_t{2}, parts2.size(), "genre part count 2");
	assert_equal("Action & Adventure", str::trim(parts2[0]), "genre with ampersand");
	assert_equal("R&B/Soul", str::trim(parts2[1]), "genre with slash");

	const auto parts3 = str::split("Jazz", false, str::is_genre_separator);
	assert_equal(size_t{1}, parts3.size(), "single genre part count");
	assert_equal("Jazz", str::trim(parts3[0]), "single genre");
}

static void should_extract_url()
{
	constexpr auto input1 = "Visit my website at https://www.example.com for more info.";
	constexpr auto input2 = "Check out this article: http://anotherexample.org/article";
	constexpr auto input3 = "No URLs here.";
	constexpr auto input4 =
		"Quite nice  <a href=\"http://bighugelabs.com/flickr/onblack.php?id=1397504988\"> On Black</a>";

	assert_equal("https://www.example.com", df::url_extract(input1), "extract url");
	assert_equal("http://anotherexample.org/article", df::url_extract(input2), "extract url");
	assert_equal("", df::url_extract(input3), "extract url");
	assert_equal("", df::url_extract(input3), "extract url");
	assert_equal("http://bighugelabs.com/flickr/onblack.php?id=1397504988", df::url_extract(input4),
	             "extract url");

	// A description panel offering a choice of links needs every distinct one, in reading order.
	const auto all = df::url_extract_all(
		"See https://example.com/a and https://example.com/b then https://example.com/a again.");
	assert_equal(size_t{2}, all.size(), "repeated url listed once");
	assert_equal("https://example.com/a", all[0], "first url in source order");
	assert_equal("https://example.com/b", all[1], "second url in source order");
	assert_equal(size_t{0}, df::url_extract_all(input3).size(), "no urls found");
}

static void should_parse_dates()
{
	const auto parsed = [](const std::string_view text)
	{
		df::date_t d;
		return d.parse(text) ? d.date() : df::day_t{0};
	};

	assert_equal(2006, parsed("2006-01-14 15:51:31").year, "an ISO date parses");
	assert_equal(31, parsed("2006-01-14 15:51:31").second, "including its seconds");
	assert_equal(2006, parsed("2006:01:14 15:51:31").year, "an EXIF date parses");
	assert_equal(13, parsed("2011-10-03T02:59:13.000000Z").second, "fractional seconds truncate to the second");

	// The seconds field is read with %lg, which accepts "inf", "nan" and an overflowing exponent.
	// Converting any of those to int is undefined, so they fail the parse instead of reaching it.
	df::date_t d;
	assert_equal(false, d.parse("2006-01-14 15:51:inf"), "infinite seconds is not a date");
	assert_equal(false, d.parse("2006-01-14 15:51:nan"), "seconds that are not a number is not a date");
	assert_equal(false, d.parse("2006-01-14 15:51:1e400"), "an overflowing seconds exponent is not a date");
	assert_equal(false, d.parse("2006-01-14 15:51:-1"), "negative seconds is not a date");
	assert_equal(false, d.parse("2006-01-14 15:51:99"), "seconds past a leap second is not a date");
	assert_equal(true, d.parse("2006-01-14 15:51:60"), "a leap second still is");
}

// A date reads as glibc's sscanf reads it, on every platform. MSVC's read a 0xFF byte inside a number
// as the end of the text, and refused numbers glibc reads: an exponent marker with no digits after it,
// in decimal or hexadecimal, "0x." and "nan(" - and a refused seconds field let a later format read
// the date's fields as other ones.
static void should_read_a_date_alike_on_every_platform()
{
	df::date_t d;
	assert_equal(false, d.parse_exif_date("2005:08:1\xFF 11:42:43"), "a 0xFF byte inside a number ends no date");
	assert_equal(true, d.parse_exif_date("2005:08:17 11:42:43\xFF"), "after the date it is not read");

	const auto second = [](const std::string_view text)
	{
		df::date_t date;
		return date.parse(text) ? date.date().second : -1;
	};

	assert_equal(1, second("2006-01-14 15:51:1e"), "an exponent marker with no digits reads as the number before it");
	assert_equal(1, second("2006-01-14 15:51:1e+"), "with its sign too");
	assert_equal(1, second("2006-01-14 15:51:0x1p"), "and in hexadecimal");
	assert_equal(0, second("2006-01-14 15:51:0x."), "0x and a point with no digit read as 0");
	assert_equal(-1, second("2006-01-14 15:51:nan("), "nan( is a nan, which is no second");
}

// Both backends refuse a field out of range rather than rolling it into another day, so a damaged
// EXIF date is no date on Linux as on Windows - where SystemTimeToFileTime always refused it.
static void should_refuse_a_date_out_of_range_on_every_platform()
{
	df::date_t d;
	assert_equal(false, d.parse_exif_date("0000:00:00 00:00:00"), "a camera's empty date is no date");
	assert_equal(false, d.parse_exif_date("2005:13:01 10:00:00"), "a thirteenth month is not a date");
	assert_equal(false, d.parse_exif_date("2005:04:31 10:00:00"), "nor the 31st of April");
	assert_equal(false, d.parse_exif_date("2005:02:29 10:00:00"), "nor the 29th of February in 2005");
	assert_equal(true, d.parse_exif_date("2004:02:29 10:00:00"), "which 2004 had");
	assert_equal(false, d.parse_exif_date("2005:06:15 24:00:00"), "an hour of 24 is not a time");
	assert_equal(false, d.parse_exif_date("2005:06:15 10:60:00"), "nor a minute of 60");
	assert_equal(false, d.parse_exif_date("2005:06:15 10:00:60"), "nor a leap second");
	assert_equal(false, d.parse_exif_date("1600:12:31 23:59:59"), "a year before 1601 is outside the calendar");

	assert_equal(true, d.parse_exif_date("2005:00:00 12:00:00"), "a month and day of 0 read as 1");
	assert_equal(df::date_t(2005, 1, 1, 12, 0, 0).to_int64(), d.to_int64(), "the 1st of January");
	assert_equal(true, d.parse_exif_date("1969:12:31 23:59:59"), "the second before the Unix epoch is a date");
	assert_equal(true, d.parse_exif_date("1700:03:01 00:00:00"), "and so is a date long before it");
	assert_equal(1700, d.date().year, "which keeps its year");
}

static void should_convert_local_dates_with_date_specific_rules()
{
	assert_equal(uint64_t{0}, platform::utc_to_local(0), "an absent UTC timestamp remains absent");
	assert_equal(uint64_t{0}, platform::local_to_utc(0), "an absent local timestamp remains absent");
	const std::array days = {
		df::day_t{2024, 1, 15, 12, 0, 0},
		df::day_t{2024, 7, 15, 12, 0, 0},
		df::day_t{2024, 3, 10, 3, 30, 0},
		df::day_t{2024, 11, 3, 3, 30, 0}
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
		const auto wall = df::date_t(day.year, day.month, day.day, day.hour, day.minute, 0).to_int64() + fraction;
		assert_equal(expected, platform::local_to_utc(wall), "local conversion matches the date-specific timezone rule");
	}
}

static void should_detect_wildcard()
{
	assert_equal(false, str::is_wildcard(""));
	assert_equal(false, str::is_wildcard("abcdef"));
	assert_equal(true, str::is_wildcard("abc*"));
	assert_equal(false, str::is_wildcard("abc\\*"));
	assert_equal(false, str::is_wildcard("abc\\*ef"));
}

static void should_match_wildcard()
{
	assert_equal(true, str::wildcard_icmp("", ""));
	assert_equal(true, str::wildcard_icmp("", "*"));
	assert_equal(true, str::wildcard_icmp(" ", "*"));
	assert_equal(true, str::wildcard_icmp(" ", " *"));
	assert_equal(false, str::wildcard_icmp(" ", "  *"));

	assert_equal(true, str::wildcard_icmp("hello world", "hello world"));
	assert_equal(true, str::wildcard_icmp("hello ?! world", "hello * world"));
	assert_equal(true, str::wildcard_icmp("hello-xx-world", "hello*world"));
	assert_equal(false, str::wildcard_icmp("hello-xx-world", "hello *world"));
	assert_equal(true, str::wildcard_icmp("hello-xx-world", "*world"));
	assert_equal(true, str::wildcard_icmp("hello-xx-world", "hello*"));

	assert_equal(true, str::wildcard_icmp("HELLO-XX-WORLD", "hello*"));
	assert_equal(true, str::wildcard_icmp("HELLO-XX-WORLD", "hello*world"));


	assert_equal(0, str::icmp("ДОБРОГО РАНКУ", "Доброго ранку"));
	assert_equal(0, str::icmp("ARABIC مرحبا العالم", "Arabic مرحبا العالم"));
	assert_equal(0, str::icmp("JAPANESE こんにちは世界", "Japanese こんにちは世界"));
	assert_equal(0, str::icmp("💉💎👦🏻👓⚡", "💉💎👦🏻👓⚡"));

	assert_equal(true, str::wildcard_icmp("Доброго ранку", "Доброго*"));
	assert_equal(true, str::wildcard_icmp("ДОБРОГО РАНКУ", "Доброго*"));
	assert_equal(true, str::wildcard_icmp("ДОБРОГО РАНКУ", "*ранку"));
	assert_equal(true, str::wildcard_icmp("💉💎👦🏻👓⚡", "*💎*"));
	assert_equal(true, str::wildcard_icmp("💉💎👦🏻👓⚡", "💉*"));
	assert_equal(true, str::wildcard_icmp("Οδός", "οδ*"));
	assert_equal(false, str::wildcard_icmp("Οδός", "sd*"));
}

static void should_trim_strings()
{
	assert_equal("xxx", str::trim_and_cache("xxx\n"), "remove cr lf");
	assert_equal("xxx", str::trim_and_cache("\rxxx\r"), "remove lf");
	assert_equal("xxx", str::trim_and_cache("   xxx\t\t "), "remove space");
}

static void should_format_text()
{
	assert_equal_strict("ac-dc", std::format("{2}{0}-{1}{0}", "c", "d", "a"), "order");
	assert_equal_strict("0.00123", std::format("{}", 0.00123), "double");
	assert_equal_strict("0.001", std::format("{:.3f}", 0.00123), "double");
	assert_equal_strict("5.5", std::format("{}", 5.5000), "double");
	assert_equal_strict("123", std::format("{}", 123), "int");
	assert_equal_strict("0123", std::format("{:04}", 123), "int");
	assert_equal_strict(" 123", std::format("{:4}", 123), "int");
	assert_equal_strict("hex=7b", std::format("hex={:x}", 0x7B), "hex");
	assert_equal_strict("-test-", std::format("-{}-", "test"), "char*");
	assert_equal_strict("-test-", std::format("-{}-", std::string("test")), "string");
	assert_equal_strict("-test-", std::format("-{}-", std::string_view("test")), "string_view");
	assert_equal_strict("33 {} {test}", std::format("{} {{}} {{test}}", 33), "string_view");
	assert_equal_strict("22 x 33", std::format("{} x {}", 22, 33), "string_view");
	assert_equal_strict("0", str::to_hex(uint32_t{0}), "zero hex has one digit");
	assert_equal_strict("10", str::to_hex(uint32_t{0x10}), "interior zero nibble preserved");
	assert_equal_strict("1001", str::to_hex(uint32_t{0x1001}), "interior zero byte preserved");
	assert_equal_strict("100000000", str::to_hex(uint64_t{0x100000000ull}), "64-bit scalar width");
	assert_equal_strict("F0", str::to_hex(uint32_t{0xF0}), "trailing zero nibble preserved");
	assert_equal(0x1001u, str::hex_to_num(str::to_hex(uint32_t{0x1001})), "scalar hex round-trips");
	const uint8_t fixed_width[] = {0x00, 0x10};
	assert_equal_strict("0010", str::to_hex(fixed_width, 2, false, false), "fixed-width hex is unchanged");
}

static void should_reject_invalid_assertion_inputs()
{
	assert_assertion_fails([] { assert_near(1.0, std::numeric_limits<double>::quiet_NaN(), 0.1); },
	                       "NaN actual is rejected");
	assert_assertion_fails([] { assert_near(1.0, 1.0, std::numeric_limits<double>::quiet_NaN()); },
	                       "NaN tolerance is rejected");
	assert_assertion_fails([] { assert_equal(df::date_t(100), df::date_t(110)); },
	                       "distinct invalid dates compare by value");
}

// str::to_string(double) used a 128-byte buffer, and _fcvt_s ends the process - through the CRT's
// invalid-parameter fail-fast, which no crash report records - when the fixed form of the value does
// not fit, from about 1e120 up. A RAW file's maker notes can state a value that large. Restoring the
// short buffer makes this test terminate the run instead of failing an assertion.
static void should_format_a_double_of_any_magnitude()
{
	const auto huge = str::to_string(1e300, 3);
	assert_equal(305_z, huge.size(), "301 integer digits, the point and three decimals");
	assert_equal(true, huge.starts_with('1'), "led by the value's first digit");
	assert_equal(true, huge.ends_with(".000"), "and closed by the decimals asked for");

	const auto lowest = str::to_string(std::numeric_limits<double>::lowest(), 2);
	assert_equal(313_z, lowest.size(), "the sign, all 309 integer digits, the point and two decimals");
	assert_equal(true, lowest.starts_with("-17976931348623157"), "the most negative double formats whole");

	assert_equal("2.5", str::to_string(2.5, -1), "an ordinary value is unchanged");
}

static std::string find_and_format_result(const std::string_view text, const std::string_view sub_string)
{
	const auto r = str::ifind2(text, sub_string, 0);
	auto result = std::string(text);

	if (r.found)
	{
		for (auto i = static_cast<int>(r.parts.size()) - 1; i >= 0; --i)
		{
			const auto part = r.parts[i];
			result.insert(part.offset + part.length, 1, '*');
			result.insert(part.offset, 1, '*');
		}
	}

	return result;
}

static void should_find_text()
{
	assert_equal("*white* on blond", find_and_format_result("white on blond", "white"));
	assert_equal("*whi*te on *bl*ond", find_and_format_result("white on blond", "whi bl"));
	assert_equal("*white* on *blond*", find_and_format_result("white on blond", "white blond"));
	assert_equal("*white* bl on *blond*", find_and_format_result("white bl on blond", "white blond"));

	// Offsets are byte positions of a character start - a match following multi-byte characters
	// must not land on a continuation byte, or the renderer drops the highlight.
	const auto cyrillic = str::ifind2("\xd0\xbf\xd1\x80\xd0\xb8\xd0\xb2\xd0\xb5\xd1\x82", "\xd0\xb2\xd0\xb5", 0);
	assert_equal(true, cyrillic.found, "cyrillic match found");
	assert_equal(6, static_cast<int>(cyrillic.parts[0].offset), "cyrillic match byte offset");

	assert_equal(true, str::ifind2("Οδός", "οδ", 0).found, "Greek case folds consistently");
	assert_equal(true, str::ifind2("crème brûlée", "creme brulee", 0).found,
	             "Latin accent folding remains intentional");
	assert_equal(false, str::ifind2("Οδός", "sd", 0).found, "Greek omicron is not ASCII s");
}

static void should_handle_cyrillic_case_folding()
{
	// Cyrillic uppercase Х (U+0425) should lowercase to х (U+0445)
	constexpr auto upper_ha = "\u0425"; // Х
	constexpr auto lower_ha = "\u0445"; // х

	// to_lower should convert uppercase Cyrillic to lowercase
	assert_equal_strict(lower_ha, str::to_lower(upper_ha), "Cyrillic to_lower");

	// Case-insensitive comparison should treat them as equal
	assert_equal(0, str::icmp(upper_ha, lower_ha), "Cyrillic icmp");

	// Additional Cyrillic pairs
	assert_equal(0, str::icmp("\u0410", "\u0430"), "А == а"); // А/а
	assert_equal(0, str::icmp("\u0411", "\u0431"), "Б == б"); // Б/б
	assert_equal(0, str::icmp("\u042F", "\u044F"), "Я == я"); // Я/я

	// Mixed Cyrillic text comparison (was failing due to towlower() locale dependency)
	// Test individual characters from "Текст" to isolate failures
	// Use \u escapes to avoid source-file encoding issues
	assert_equal(0, str::icmp("\u0422", "\u0442"), "T == t cyrillic"); // Т/т U+0422/U+0442

	// Текст = U+0422 U+0435 U+043A U+0441 U+0442
	// текст = U+0442 U+0435 U+043A U+0441 U+0442
	constexpr auto cyrillic_upper = "\u0422\u0435\u043a\u0441\u0442"; // Текст
	constexpr auto cyrillic_lower = "\u0442\u0435\u043a\u0441\u0442"; // текст
	const auto upper_text = str::to_lower(cyrillic_upper);
	assert_equal_strict(cyrillic_lower, upper_text, "to_lower cyrillic word");

	assert_equal(0, str::icmp(cyrillic_upper, cyrillic_lower), "mixed Cyrillic icmp");
	// МОСКВА = U+041C U+041E U+0421 U+041A U+0412 U+0410
	// москва = U+043C U+043E U+0441 U+043A U+0432 U+0430
	assert_equal(true, str::icmp("\u041c\u041e\u0421\u041a\u0412\u0410",
	                             "\u043c\u043e\u0441\u043a\u0432\u0430") == 0, "MOSKVA icmp");

	// Greek omicron must case-fold as Greek, not as an unrelated ASCII letter.
	assert_equal(0, str::icmp("\u039f", "\u03bf"), "Greek omicron case-folds");
	assert_equal(false, str::icmp("\u039f", "s") == 0, "Greek omicron is not ASCII s");
	assert_equal(false, str::icmp("\u03bf", "y") == 0, "Greek omicron is not ASCII y");
	assert_equal(str::normalize_for_compare(0x03bf), str::normalize_for_compare(0x039f),
	             "Greek omicron normalizes consistently");
	assert_equal(false, str::normalize_for_compare(0x039f) == 's', "Greek omicron does not normalize to ASCII s");
	assert_equal(false, str::normalize_for_compare(0x03bf) == 'y', "Greek omicron does not normalize to ASCII y");

	// Latin Extended pairs
	assert_equal(0, str::icmp("\u00C0", "\u00E0"), "A-grave icmp");
	assert_equal(0, str::icmp("\u00D6", "\u00F6"), "O-umlaut icmp");
	assert_equal(0, str::icmp("\u00D8", "\u00F8"), "O-stroke icmp");
}

static void should_case_fold_korean()
{
	// Hangul has no case - normalisation/lowercasing must be identity.
	constexpr auto family = "\uAC00\uC871"; // 가족
	assert_equal_strict(family, str::to_lower(family), "Korean to_lower identity");
	assert_equal(0, str::icmp(family, family), "Korean icmp equal");

	// A single Hangul syllable normalises to itself for comparison.
	assert_equal(0xAC00, str::normalize_for_compare(0xAC00), "Hangul normalise identity");

	// Different Korean words must not compare equal.
	assert_equal(true, str::icmp(family, "\uC5EC\uD589") != 0, "different Korean words differ");
}

static void should_query_kdtree_bounds()
{
	// A 10x10 integer grid of points; offset encodes the original index.
	kd_points pts;
	for (int gx = 0; gx < 10; ++gx)
	{
		for (int gy = 0; gy < 10; ++gy)
		{
			pts.emplace_back(static_cast<float>(gx), static_cast<float>(gy),
			                 static_cast<uint32_t>(gx * 10 + gy), 0, 0, 0.0f);
		}
	}

	kd_tree tree;
	tree.build(pts); // reorders pts, but offset travels with each point

	// x in {3,4,5} and y in {3,4,5} => 9 points.
	std::vector<kd_coordinates_t> found;
	tree.find_in_bounds(pts, 2.5f, 2.5f, 5.5f, 5.5f, found);
	assert_equal(9, static_cast<int>(found.size()), "window count", "kd bounds");

	for (const auto& c : found)
	{
		assert_equal(true, c.x >= 2.5f && c.x <= 5.5f && c.y >= 2.5f && c.y <= 5.5f, "point in range", "kd bounds");
	}

	// A window far from every point returns nothing.
	std::vector<kd_coordinates_t> none;
	tree.find_in_bounds(pts, 100.0f, 100.0f, 200.0f, 200.0f, none);
	assert_equal(0, static_cast<int>(none.size()), "empty window", "kd bounds");

	// A window covering everything returns all points exactly once.
	std::vector<kd_coordinates_t> all;
	tree.find_in_bounds(pts, -1.0f, -1.0f, 100.0f, 100.0f, all);
	assert_equal(100, static_cast<int>(all.size()), "full window", "kd bounds");
}

static void should_intersect_rectangle_bounds()
{
	rectd out;
	assert_equal(true, rectd::intersect(out, rectd(0, 0, 2, 2), rectd(1, 1, 2, 2)), "overlap intersects");
	assert_equal(true, df::equiv(1.0, out.X) && df::equiv(1.0, out.Y) &&
	             df::equiv(1.0, out.Width) && df::equiv(1.0, out.Height), "overlap result");

	assert_equal(false, rectd::intersect(out, rectd(0, 0, 1, 1), rectd(2, 2, 1, 1)), "separated");
	assert_equal(true, out.is_empty(), "separated result is empty");

	assert_equal(true, rectd::intersect(out, rectd(0, 0, 4, 4), rectd(1, 1, 1, 1)), "contained");
	assert_equal(true, df::equiv(1.0, out.X) && df::equiv(1.0, out.Y) &&
	             df::equiv(1.0, out.Width) && df::equiv(1.0, out.Height), "contained result");

	assert_equal(false, rectd::intersect(out, rectd(0, 0, 1, 1), rectd(1, 0, 1, 1)), "touching edge");
	assert_equal(false, rectd::intersect(out, rectd(0, 0, 0, 1), rectd(0, 0, 1, 1)), "zero-size input");

	rectd symmetric;
	assert_equal(true, rectd::intersect(out, rectd(1, 1, 3, 2), rectd(2, 0, 1, 4)), "asymmetric overlap");
	assert_equal(true, rectd::intersect(symmetric, rectd(2, 0, 1, 4), rectd(1, 1, 3, 2)), "symmetric overlap");
	assert_equal(true, df::equiv(out.X, symmetric.X) && df::equiv(out.Y, symmetric.Y) &&
	             df::equiv(out.Width, symmetric.Width) && df::equiv(out.Height, symmetric.Height),
	             "intersection is symmetric");

	rectd aliased(0, 0, 2, 2);
	assert_equal(true, aliased.intersect(rectd(1, 1, 2, 2)), "aliased output");
	assert_equal(true, df::equiv(1.0, aliased.X) && df::equiv(1.0, aliased.Y) &&
	             df::equiv(1.0, aliased.Width) && df::equiv(1.0, aliased.Height), "aliased result");
}

static void should_find_the_closest_kdtree_point()
{
	// Deliberately elongated (x spans 4, y spans 400): a node's bounding box prunes such a spread
	// far more tightly than a bounding circle, so a pruning error surfaces here as a wrong answer.
	kd_points pts;
	std::vector<std::pair<float, float>> expected;
	uint32_t seed = 12345;
	const auto next_rand = [&seed]
	{
		seed = seed * 1664525u + 1013904223u;
		return static_cast<float>(seed % 100000u) * 0.00001f;
	};

	for (uint32_t i = 0; i < 400; ++i)
	{
		const auto x = next_rand() * 4.0f;
		const auto y = next_rand() * 400.0f;
		pts.emplace_back(x, y, i * 3 + 1, 7, i, 0.0f);
		expected.emplace_back(x, y);
	}

	kd_tree tree;
	tree.build(pts);

	// Query points inside, at the corners of, and far outside the data extent.
	const std::vector<std::pair<float, float>> queries{
		{0.0f, 0.0f}, {2.0f, 200.0f}, {3.9f, 399.0f}, {0.1f, 12.5f}, {-40.0f, -40.0f},
		{900.0f, 900.0f}, {-1000.0f, 25.0f}, {2.0f, 100000.0f}
	};

	for (const auto& [qx, qy] : queries)
	{
		auto best_d2 = std::numeric_limits<double>::max();

		for (const auto& [px, py] : expected)
		{
			const double dx = px - qx, dy = py - qy;
			best_d2 = std::min(best_d2, dx * dx + dy * dy);
		}

		const auto found = tree.find_closest(pts, qx, qy);
		const double fdx = found.x - qx, fdy = found.y - qy;
		const auto found_d2 = fdx * fdx + fdy * fdy;

		assert_equal(true, std::abs(found_d2 - best_d2) <= 1e-6 * std::max(1.0, best_d2),
		             "matches the brute-force nearest", "kd closest");

		// The build reorders coordinates and record fields in lockstep, so the record that comes
		// back must still be the one that owns the coordinate it came back with.
		assert_equal(true, found.id < expected.size(), "record identifies a point", "kd closest");
		assert_equal(true, df::equiv(found.x, expected[found.id].first) &&
		             df::equiv(found.y, expected[found.id].second), "record travels with its coordinate",
		             "kd closest");
		assert_equal(static_cast<int>(found.id * 3 + 1), static_cast<int>(found.offset),
		             "every record field travels together", "kd closest");
	}

	// An empty set has no tree and therefore no answer.
	kd_points none;
	kd_tree empty;
	empty.build(none);
	assert_equal(true, empty.is_empty(), "an empty point set builds no nodes", "kd closest");
}

// The gazetteer stores degrees, and a degree of longitude is only a degree of latitude's worth of
// ground at the equator. Searching the raw plane therefore ranked somewhere far to the east as
// nearer than somewhere just up the road, and at sixty degrees the error is a factor of two.
static void should_weigh_longitude_by_latitude_when_finding_the_closest()
{
	// x is latitude and y is longitude, as the gazetteer tree stores them.
	kd_points pts;
	pts.emplace_back(60.0f, 1.0f, 0, 0, 0, 0.0f); // one degree east: about 56 km at this latitude
	pts.emplace_back(60.8f, 0.0f, 0, 0, 1, 0.0f); // 0.8 degrees north: about 89 km

	kd_tree tree;
	tree.build(pts);

	// Unweighted, the northern point wins on raw degrees (0.8 against 1.0) despite being half as
	// far again on the ground.
	const auto raw = tree.find_closest(pts, 60.0f, 0.0f);
	assert_equal(1u, raw.id, "raw degrees pick the point that is fewer degrees away");

	// cos(60 degrees) is a half, so the eastern point's degree counts for half a degree and it wins
	// - which is the answer the ground agrees with.
	const auto weighted = tree.find_closest(pts, 60.0f, 0.0f, 0.5f);
	assert_equal(0u, weighted.id, "weighting longitude picks the point that is actually nearer");

	// At the equator the weighting is one and nothing changes.
	kd_points equator;
	equator.emplace_back(0.0f, 1.0f, 0, 0, 0, 0.0f);
	equator.emplace_back(0.8f, 0.0f, 0, 0, 1, 0.0f);

	kd_tree equator_tree;
	equator_tree.build(equator);
	assert_equal(1u, equator_tree.find_closest(equator, 0.0f, 0.0f, 1.0f).id,
	             "on the equator a degree is a degree either way");
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

static void should_bucket_performance_latency()
{
	df::latency_counters counters;
	df::record_latency(counters, 16'666);
	df::record_latency(counters, 16'667);
	df::record_latency(counters, 50'000);
	df::record_latency(counters, 100'000);

	assert_equal(3, static_cast<int>(counters.over_16ms.load()), "the frame-budget boundary is inclusive");
	assert_equal(2, static_cast<int>(counters.over_50ms.load()), "fifty millisecond stalls are counted");
	assert_equal(1, static_cast<int>(counters.over_100ms.load()), "hundred millisecond stalls are counted");
}

#ifdef _WIN32
// The Windows backend's own tests, in system_win_tests.cpp.
void register_win_tests(test_registry& tests);
#endif

static void register_tests(test_registry& tests)
{
#ifdef _WIN32
	register_win_tests(tests);
#endif
	tests.add("Should name files after the application"s, should_name_files_after_the_application);
	tests.add("Should natural compare"s, should_icmp_natural);
	tests.add("Should detect utf16 either way round"s, should_detect_utf16_either_way_round);
	tests.add("Should cancel superseded tokens"s, should_cancel_superseded_tokens);
	tests.add("Should parse root child paths"s, should_parse_files_directly_under_the_root);
	tests.add("Should report file presence"s, should_report_file_presence);
	tests.add("Should map files"s, should_map_files);
	tests.add("Should save a blob without truncating the destination"s, should_save_a_blob_without_truncating_the_destination);
	tests.add("Should keep line breaks in INI settings"s, should_keep_line_breaks_in_ini_settings);
	tests.add("Should replace bytes in files"s, should_replace_bytes_in_files);
	tests.add("Should intern strings"s, should_intern_strings);
	tests.add("Should round-trip base64"s, should_round_trip_base64);
	tests.add("Should rotate points"s, should_rotate_points);
	tests.add("Should split genre"s, should_split_genre);
	tests.add("Should extract url"s, should_extract_url);
	tests.add("Should parse dates"s, should_parse_dates);
	tests.add("Should read a date alike on every platform"s, should_read_a_date_alike_on_every_platform);
	tests.add("Should refuse a date out of range on every platform"s, should_refuse_a_date_out_of_range_on_every_platform);
	tests.add("Should convert local dates with date specific rules"s, should_convert_local_dates_with_date_specific_rules);
	tests.add("Should detect wildcard"s, should_detect_wildcard);
	tests.add("Should match wildcard"s, should_match_wildcard);
	tests.add("Should trim strings"s, should_trim_strings);
	tests.add("Should format text"s, should_format_text);
	tests.add("Should reject invalid assertion inputs"s, should_reject_invalid_assertion_inputs);
	tests.add("Should format a double of any magnitude"s, should_format_a_double_of_any_magnitude);
	tests.add("Should find text"s, should_find_text);
	tests.add("Should handle Cyrillic case folding"s, should_handle_cyrillic_case_folding);
	tests.add("Should case-fold Korean"s, should_case_fold_korean);
	tests.add("Should query kd-tree bounds"s, should_query_kdtree_bounds);
	tests.add("Should intersect rectangle bounds"s, should_intersect_rectangle_bounds);
	tests.add("Should find the closest kd-tree point"s, should_find_the_closest_kdtree_point);
	tests.add("Should weigh longitude by latitude when finding the closest"s, should_weigh_longitude_by_latitude_when_finding_the_closest);
	tests.add("Should signal and replace pending queue work"s, should_signal_and_replace_pending_queue_work);
	tests.add("Should bucket performance latency"s, should_bucket_performance_latency);
}

int main(const int argc, char* argv[])
{
	// What an application sets up before it uses the module: a UTF-8 C locale, which the backend's
	// narrow conversions follow, set as Diffractor sets it; and the thread's platform state - COM's
	// apartment on Windows, which the shell and WIC need.
#ifdef _WIN32
	std::setlocale(LC_ALL, "en_US.UTF-8");
#else
	std::setlocale(LC_CTYPE, "C.UTF-8");
#endif
	const platform::thread_init thread;

	const std::string_view filter = argc > 1 ? argv[1] : "*";

	struct registry final : test_registry
	{
		std::vector<std::pair<std::string, std::function<void()>>> tests;

		void add(std::string name, std::function<void()> f) override
		{
			tests.emplace_back(std::move(name), std::move(f));
		}
	} all;

	register_tests(all);

	auto run = 0;
	auto failed = 0;

	for (const auto& [name, test] : all.tests)
	{
		if (!str::wildcard_icmp(name, filter)) continue;

		++run;
		const auto checks_before = test_assert_count;
		std::string failure;

		try
		{
			test();
			if (test_assert_count == checks_before) failure = "asserted nothing";
		}
		catch (const test_assert_exception& e)
		{
			failure = e.message;
		}
		catch (const std::exception& e)
		{
			failure = std::format("threw {}", e.what());
		}

		if (failure.empty())
		{
			std::printf("PASS  %s\n", name.c_str());
		}
		else
		{
			++failed;
			std::printf("FAIL  %s: %s\n", name.c_str(), failure.c_str());
		}
	}

	for (const auto& error : _temps.delete_temps())
	{
		++failed;
		std::printf("FAIL  temporary files: %s\n", error.c_str());
	}

	std::printf("\n%d tests, %d failed, %d checks\n", run, failed, test_assert_count);
	return failed == 0 && run > 0 ? 0 : 1;
}
