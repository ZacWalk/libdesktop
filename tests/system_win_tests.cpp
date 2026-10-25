// system_win_tests.cpp - libdesktop-system's Windows backend, checked without a window: time zone
// conversion, extended paths, the ANSI code page, the registry and INI settings stores, the shell's file
// operations and their results, number formats, file names, network locations and a pasted bitmap's
// staging. They came with the code from Diffractor's test_platform_win.cpp, whose window and rendering
// tests stay with it, and use the same assertions.

#include "libdesktop/system.h"
#include "libdesktop/platform_win.h"
#include "system_test.h"

namespace
{
	constexpr uint64_t test_ticks_per_ms = 10'000;
	constexpr uint64_t test_ticks_per_minute = 60'000 * test_ticks_per_ms;

	struct tabulation_case
	{
		std::string_view name;
		TIME_ZONE_INFORMATION zone;
		// 2024's transitions as UTC instants, and as the local readings the rules name.
		std::vector<uint64_t> instants;
		std::vector<uint64_t> readings;
	};

	TIME_ZONE_INFORMATION make_zone(const LONG bias, const LONG daylight_bias, const SYSTEMTIME daylight,
	                                const SYSTEMTIME standard)
	{
		TIME_ZONE_INFORMATION zone{};
		zone.Bias = bias;
		zone.DaylightBias = daylight_bias;
		zone.DaylightDate = daylight;
		zone.StandardDate = standard;
		return zone;
	}

	uint64_t stamp(const int month, const int day, const int hour, const int minute, const int second = 0,
	               const int ms = 0)
	{
		return df::date_t(2024, month, day, hour, minute, second).to_int64() + static_cast<uint64_t>(ms) *
			test_ticks_per_ms;
	}

	std::vector<uint64_t> samples_around(const std::vector<uint64_t>& changes, const bool include_gap_edges)
	{
		std::vector<uint64_t> result;
		const auto year_start = stamp(1, 1, 0, 0);

		for (auto hour = 0; hour < 366 * 24; hour += 6)
			result.push_back(year_start + static_cast<uint64_t>(hour) * 60 * test_ticks_per_minute + 1234567);

		for (const auto change : changes)
		{
			const auto at = [change](const int64_t minutes, const int64_t ms)
			{
				return static_cast<uint64_t>(static_cast<int64_t>(change) +
					minutes * static_cast<int64_t>(test_ticks_per_minute) +
					ms * static_cast<int64_t>(test_ticks_per_ms));
			};

			for (auto minute = -180; minute <= 180; ++minute)
				result.push_back(at(minute, 0));

			const auto edges = include_gap_edges ? std::vector<int>{-60, -30, 0, 30, 60} : std::vector<int>{0};
			for (const auto edge : edges)
			{
				for (auto ms = -3; ms <= 3; ++ms)
					result.push_back(at(edge, ms) + 4321);
			}
		}

		return result;
	}
}

static int WINAPI shell_success_w(SHFILEOPSTRUCTW*)
{
	return 0;
}

static int WINAPI shell_aborts_w(SHFILEOPSTRUCTW* op)
{
	op->fAnyOperationsAborted = TRUE;
	return 0;
}

static int WINAPI shell_fails_w(SHFILEOPSTRUCTW*)
{
	return ERROR_ACCESS_DENIED;
}

static int WINAPI shell_aborts_a(SHFILEOPSTRUCTA* op)
{
	op->fAnyOperationsAborted = TRUE;
	return 0;
}

static void should_convert_dates_with_their_own_dst_offset()
{
	TIME_ZONE_INFORMATION zone{};
	zone.Bias = 300;
	zone.DaylightBias = -60;
	zone.DaylightDate = {0, 3, 0, 2, 2, 0, 0, 0};
	zone.StandardDate = {0, 11, 0, 1, 2, 0, 0, 0};
	constexpr uint64_t fraction = 1234567;

	for (const auto month : {1, 7})
	{
		const auto utc = df::date_t(2024, month, 15, 12, 0, 0).to_int64() + fraction;
		const auto local = df::date_t(2024, month, 15, month == 1 ? 7 : 8, 0, 0).to_int64() + fraction;
		assert_equal(local, platform::utc_to_local(utc, &zone), "conversion uses the timestamp's season");
		assert_equal(utc, platform::local_to_utc(local, &zone), "reverse conversion preserves every tick");
	}

	const auto after_jump = df::date_t(2024, 3, 10, 3, 30, 0).to_int64();
	assert_equal(df::date_t(2024, 3, 10, 7, 30, 0).to_int64(), platform::local_to_utc(after_jump, &zone),
	             "an unambiguous time after the spring transition uses daylight bias");
	assert_equal(uint64_t{0}, platform::utc_to_local(0, &zone), "an absent timestamp remains absent");
}

// Dates are converted per item in search, grouping and summaries, and the system conversion costs
// microseconds, so each year's offsets are tabulated from the system once. The tables must agree
// with the system to the tick, including at the millisecond a transition takes effect, for zones
// whose rules span the new year, move by half an hour, or change just before midnight. Shifting a
// tabulated boundary by one millisecond fails this test.
static void should_tabulate_time_zone_offsets_like_the_system()
{
	const std::vector<tabulation_case> cases = {
		{
			"northern", make_zone(300, -60, {0, 3, 0, 2, 2, 0, 0, 0}, {0, 11, 0, 1, 2, 0, 0, 0}),
			{stamp(3, 10, 7, 0), stamp(11, 3, 6, 0)}, {stamp(3, 10, 2, 0), stamp(11, 3, 2, 0)}
		},
		{
			"southern", make_zone(-600, -60, {0, 10, 0, 1, 2, 0, 0, 0}, {0, 4, 0, 1, 3, 0, 0, 0}),
			{stamp(4, 6, 16, 0), stamp(10, 5, 16, 0)}, {stamp(4, 7, 3, 0), stamp(10, 6, 2, 0)}
		},
		{
			"half-hour daylight", make_zone(-630, -30, {0, 10, 0, 1, 2, 0, 0, 0}, {0, 4, 0, 1, 2, 0, 0, 0}),
			{stamp(4, 6, 15, 0), stamp(10, 5, 15, 30)}, {stamp(4, 7, 2, 0), stamp(10, 6, 2, 0)}
		},
		{
			"before midnight", make_zone(-120, -60, {0, 3, 4, 5, 23, 59, 59, 999}, {0, 10, 5, 5, 1, 0, 0, 0}),
			{stamp(3, 28, 21, 59, 59, 999), stamp(10, 24, 22, 0)},
			{stamp(3, 28, 23, 59, 59, 999), stamp(10, 25, 1, 0)}
		},
		{"no daylight", make_zone(-330, 0, {}, {}), {}, {}},
	};

	for (const auto& c : cases)
	{
		for (const auto instant : c.instants)
		{
			const auto earlier = instant - test_ticks_per_ms;
			assert_equal(true, platform::utc_to_local(earlier, &c.zone) - earlier !=
			             platform::utc_to_local(instant, &c.zone) - instant,
			             std::format("{}: the system changes offset at the stated instant", c.name));
		}

		for (const auto to_local : {true, false})
		{
			const auto stamps = samples_around(to_local ? c.instants : c.readings, !to_local);
			const auto tabulated = platform::tabulated_time_zone_conversions(stamps, c.zone, to_local);
			auto mismatches = 0;
			std::string first;

			for (auto i = 0u; i < stamps.size(); ++i)
			{
				const auto expected = to_local
					                      ? platform::utc_to_local(stamps[i], &c.zone)
					                      : platform::local_to_utc(stamps[i], &c.zone);
				if (tabulated[i] != expected && mismatches++ == 0)
					first = std::format(" first at {}: {} != {}", stamps[i], tabulated[i], expected);
			}

			assert_equal(0, mismatches, std::format("{} {} tabulation matches the system{}", c.name,
			                                        to_local ? "UTC-to-local" : "local-to-UTC", first));
		}
	}
}

// The tables serve whatever zone this machine is in, through the same rule selection as the
// per-call conversion: the year's dynamic rules, or standard time when adjustment is turned off.
static void should_convert_the_current_zone_like_the_system()
{
	DYNAMIC_TIME_ZONE_INFORMATION current{};
	assert_equal(true, GetDynamicTimeZoneInformation(&current) != TIME_ZONE_ID_INVALID, "current zone is known");

	for (const auto year : {2006, 2007, 2019, 2024})
	{
		TIME_ZONE_INFORMATION rules{};
		assert_equal(true, GetTimeZoneInformationForYear(static_cast<USHORT>(year), &current, &rules) != FALSE,
		             "the year's rules are known");
		if (current.DynamicDaylightTimeDisabled)
		{
			rules.StandardDate.wMonth = 0;
			rules.DaylightDate.wMonth = 0;
		}

		auto mismatches = 0;
		const auto start = df::date_t(year, 1, 1, 0, 0, 0).to_int64() + 7654321;

		for (auto half_day = 0; half_day < 730; ++half_day)
		{
			const auto ts = start + static_cast<uint64_t>(half_day) * 720 * test_ticks_per_minute;
			if (platform::utc_to_local(ts) != platform::utc_to_local(ts, &rules)) ++mismatches;
			if (platform::local_to_utc(ts) != platform::local_to_utc(ts, &rules)) ++mismatches;
		}

		assert_equal(0, mismatches, std::format("{} converts as the system does", year));
	}
}

// Converting each date through the system took about five microseconds; a large collection paid
// that for every date in every search and grouping. Removing the year cache fails this test.
static void should_tabulate_local_time_once_per_year()
{
	const auto start = df::date_t(2023, 1, 1, 0, 0, 0).to_int64();
	constexpr uint64_t step = 31'536'000'000ull;
	const auto before = platform::time_zone_system_queries();
	uint64_t total = 0;

	for (auto i = 0u; i < 10000; ++i)
	{
		const auto ts = start + i * step;
		total += platform::utc_to_local(ts) / 10000000;
		total += platform::local_to_utc(ts) / 10000000;
	}

	const auto queries = platform::time_zone_system_queries() - before;
	assert_equal(true, queries <= 1000,
	             std::format("{} system conversions for 20000 dates (checksum {})", queries, total));
}

static void should_pass_bare_save_dialog_extensions()
{
	assert_equal(L"png", platform::dialog_default_extension(".png"), "the leading dot is not passed to the dialog");
	assert_equal(L"webp", platform::dialog_default_extension("webp"), "an already bare extension is unchanged");
	assert_equal(L"", platform::dialog_default_extension(""), "an absent extension remains absent");
}

static void should_convert_extended_file_system_paths()
{
	const auto long_unc = std::string("\\\\server\\share\\") + std::string(MAX_PATH, 'x');
	const auto converted = platform::to_file_system_path(df::folder_path(long_unc));
	assert_equal(std::wstring(L"\\\\?\\UNC\\server\\share\\") + std::wstring(MAX_PATH, L'x'), converted,
	             "long UNC path");

	const auto extended = std::wstring(L"\\\\?\\C:\\") + std::wstring(MAX_PATH, L'x');
	assert_equal(extended, platform::to_file_system_path(df::folder_path(str::utf16_to_utf8(extended))),
	             "extended path unchanged");
}

// The measured length excludes the terminator a string_view does not carry, so converting with -1
// both over-read the view and asked for one wchar more than the measurement allowed. The call then
// failed and left the buffer zeroed, which made every ordinary path convert to a run of NULs.
static void should_convert_utf8_to_ansi()
{
	assert_equal("photo.jpg", platform::utf8_to_a("photo.jpg"), "an ordinary name converts");
	assert_equal(9_z, platform::utf8_to_a("photo.jpg").size(), "and is not padded with terminators");
	assert_equal(true, platform::utf8_to_a("").empty(), "an empty name converts to an empty name");

	// The source has no terminator of its own: the conversion must respect the view's length rather
	// than read past it.
	const std::string backing("C:\\photos\\holiday.jpgTRAILING");
	assert_equal("C:\\photos\\holiday.jpg", platform::utf8_to_a(std::string_view(backing).substr(0, 21)),
	             "a view stops where it says it stops");

	// The console commands run with a UTF-8 C locale, where each of these characters takes three
	// bytes. An output sized at two bytes per UTF-16 unit truncated a name that is more CJK than
	// ASCII, and a truncated conversion answers empty - so the file could not be opened by name.
	const std::string cjk = "D:\\\xe5\x8b\x95\xe7\x94\xbb\\\xe6\x9d\xb1\xe4\xba\xac\xe6\x97\x85\xe8\xa1\x8c"
		"\xe3\x81\xae\xe6\x80\x9d\xe3\x81\x84\xe5\x87\xba.mp4";
	assert_equal(cjk, platform::utf8_to_a(cjk), "a name of three-byte characters converts whole");
}

// A registry key of the test's own under HKEY_CURRENT_USER, removed with everything under it - and its
// parent, once nothing else is left there.
class registry_test_root
{
	std::string _root;
	std::wstring _root_w;

public:
	explicit registry_test_root(const std::string_view name)
	{
		_root = std::format("Software\\LibDesktopTest\\{}-{}", name, platform::tick_count());
		_root_w = str::utf8_to_utf16(_root);
	}

	~registry_test_root()
	{
		RegDeleteTreeW(HKEY_CURRENT_USER, _root_w.c_str());
		// Fails while another root is still there, which is what it should do.
		RegDeleteKeyW(HKEY_CURRENT_USER, L"Software\\LibDesktopTest");
	}

	registry_test_root(const registry_test_root&) = delete;
	registry_test_root& operator=(const registry_test_root&) = delete;

	const std::string& path() const
	{
		return _root;
	}

	std::wstring subkey(const std::string_view section) const
	{
		return str::utf8_to_utf16(std::format("{}\\{}", _root, section));
	}
};

static void should_persist_to_registry()
{
	const registry_test_root root("strings");
	const auto archive = platform::create_registry_settings(root.path());

	const std::vector<std::string> vals = {
		"Hello World"s,
		"\r\n\t hello"s,
		"Доброго ранку!"s,
		"Japanese こんにちは世界"s,
		"Доброго ранку!"s,
		std::string(64, 'x')
	};

	for (const auto& expected : vals)
	{
		std::string actual;
		archive->write({}, "test", expected);
		archive->read({}, "test", actual);

		assert_equal_strict(expected, actual, "Persist To Registry");
	}
}

static void should_validate_registry_value_types_and_sizes()
{
	const registry_test_root root("malformed");
	const auto section = std::format("test-malformed-{}", platform::tick_count());
	const auto sectionW = root.subkey(section);
	HKEY key = nullptr;
	assert_equal(static_cast<int>(ERROR_SUCCESS),
	             static_cast<int>(RegCreateKeyExW(HKEY_CURRENT_USER, sectionW.c_str(), 0, nullptr,
	                                              REG_OPTION_NON_VOLATILE, KEY_ALL_ACCESS, nullptr, &key, nullptr)),
	             "create registry test section");
	const df::scope_exit close_key([&key] { if (key) RegCloseKey(key); });

	constexpr wchar_t text[] = L"text";
	constexpr uint16_t short_number = 42;
	constexpr wchar_t unterminated[] = {L'r', L'a', L'w'};
	assert_equal(static_cast<int>(ERROR_SUCCESS),
	             static_cast<int>(RegSetValueExW(key, L"wrong-number-type", 0, REG_SZ,
	                                             reinterpret_cast<const BYTE*>(text), sizeof(text))),
	             "write wrong-number-type");
	assert_equal(static_cast<int>(ERROR_SUCCESS),
	             static_cast<int>(RegSetValueExW(key, L"short-number", 0, REG_DWORD,
	                                             reinterpret_cast<const BYTE*>(&short_number),
	                                             sizeof(short_number))),
	             "write short-number");
	assert_equal(static_cast<int>(ERROR_SUCCESS),
	             static_cast<int>(RegSetValueExW(key, L"unterminated-string", 0, REG_SZ,
	                                             reinterpret_cast<const BYTE*>(unterminated),
	                                             sizeof(unterminated))),
	             "write unterminated-string");
	assert_equal(static_cast<int>(ERROR_SUCCESS),
	             static_cast<int>(RegSetValueExW(key, L"wrong-binary-type", 0, REG_SZ,
	                                             reinterpret_cast<const BYTE*>(text), sizeof(text))),
	             "write wrong-binary-type");
	RegCloseKey(key);
	key = nullptr;

	{
		const auto archive = platform::create_registry_settings(root.path());
		uint32_t number = 99;
		assert_equal(false, archive->read(section, "wrong-number-type", number), "reject numeric type");
		assert_equal(99u, number, "preserve numeric output after wrong type");
		assert_equal(false, archive->read(section, "short-number", number), "reject short numeric value");
		assert_equal(99u, number, "preserve numeric output after short value");

		std::string string_value;
		assert_equal(true, archive->read(section, "unterminated-string", string_value),
		             "read unterminated registry string");
		assert_equal("raw"s, string_value, "unterminated registry string value");

		uint8_t binary[16] = {};
		auto binary_size = std::size(binary);
		assert_equal(false, archive->read(section, "wrong-binary-type", binary, binary_size),
		             "reject binary type");
	}

}

static void should_preserve_writability_probe_collisions()
{
	const auto folder = _temps.next_folder("write-probe");
	const auto name = std::format("df-write-probe-{:08x}.tmp", static_cast<uint32_t>(GetCurrentProcessId()));
	const auto collision = folder.combine_file(name);
	write_test_file(collision, "existing file");

	assert_equal(true, platform::is_writable(folder), "a probe collision does not make a writable folder read-only");
	assert_equal("existing file", read_test_file(collision), "the probe must not truncate or delete an existing file");
	assert_equal(1_z, platform::iterate_file_items(folder, true).files.size(), "the probe cleans up only its own file");
}

static void should_initialize_unicode_ini_settings()
{
	const auto folder = _temps.next_folder("unicode-ini");
	const auto ini = folder.combine_file(std::format("{}.ini", df::app_file_stem()));
	const auto settings = platform::create_ini_file_settings(folder);
	const std::string value = "C:\\\xe6\x9d\xb1\xe4\xba\xac\\\xf0\x9f\x8e\xac.mp4";
	assert_equal(true, settings->write("paths", "movie", value), "Unicode setting is written");
	const auto bytes = df::blob_from_file(ini);
	assert_equal(true, bytes.size() >= 2 && bytes[0] == 0xff && bytes[1] == 0xfe,
	             "new INI files explicitly select Unicode independent of the ANSI code page");

	const auto reopened = platform::create_ini_file_settings(folder);
	std::string read;
	assert_equal(true, reopened->read("paths", "movie", read), "setting remains readable after reopening");
	assert_equal(value, read, "the complete Unicode path survives");
	assert_equal(false, reopened->root_created(), "existing configuration is not replaced");
}

// Earlier builds wrote each value as it was, unquoted and unescaped, into a file with no BOM. Those
// values read back unchanged, which is why the escape for a line break cannot be a backslash.
static void should_read_ini_values_earlier_builds_wrote()
{
	const auto folder = _temps.next_folder("earlier-ini");
	const auto ini = str::utf8_to_utf16(folder.combine_file(std::format("{}.ini", df::app_file_stem())).str());
	assert_equal(true, WritePrivateProfileStringW(L"index", L"folder", LR"(C:\new\raw)", ini.c_str()) != 0,
	             "an earlier build's folder is written");
	assert_equal(true, WritePrivateProfileStringW(L"index", L"share", LR"(\\server\share)", ini.c_str()) != 0,
	             "and its network share");

	const auto settings = platform::create_ini_file_settings(folder);
	std::string read;
	assert_equal(true, settings->read("index", "folder", read), "the folder is read");
	assert_equal(std::string(R"(C:\new\raw)"), read, "with its backslashes as written");
	assert_equal(true, settings->read("index", "share", read), "the share is read");
	assert_equal(std::string(R"(\\server\share)"), read, "with its leading backslashes");
}

static void should_double_terminate_shell_destination_buffers()
{
	const auto target = df::folder_path("C:\\photos");
	const auto wide_destination = platform::probe_shell_destination_w(target);
	const auto narrow_destination = platform::probe_shell_destination_a(target);

	assert_equal(L'\0', wide_destination[wide_destination.size() - 1], "wide destination final terminator");
	assert_equal(L'\0', wide_destination[wide_destination.size() - 2], "wide destination penultimate terminator");
	assert_equal('\0', narrow_destination[narrow_destination.size() - 1], "narrow destination final terminator");
	assert_equal('\0', narrow_destination[narrow_destination.size() - 2], "narrow destination penultimate terminator");
}

static void should_classify_an_aborted_shell_operation_as_cancelled()
{
	assert_equal(static_cast<int>(platform::file_op_result_code::CANCELLED),
	             static_cast<int>(platform::probe_shell_file_operation_w(shell_aborts_w).result.code),
	             "wide shell abort is cancellation");
	assert_equal(static_cast<int>(platform::file_op_result_code::CANCELLED),
	             static_cast<int>(platform::probe_shell_file_operation_a(shell_aborts_a).result.code),
	             "narrow shell abort is cancellation");
	assert_equal(static_cast<int>(platform::file_op_result_code::OK),
	             static_cast<int>(platform::probe_shell_delete_operation(shell_success_w, true).result.code),
	             "zero and no abort is success");
	assert_equal(static_cast<int>(platform::file_op_result_code::FAILED),
	             static_cast<int>(platform::probe_shell_delete_operation(shell_fails_w, true).result.code),
	             "nonzero shell result is failure");
}

static void should_let_windows_confirm_recycle_that_would_destroy()
{
	const auto recycle_flags = platform::probe_shell_delete_flags(true);
	assert_equal(true, (recycle_flags & FOF_ALLOWUNDO) != 0, "requested recycle allows undo");
	assert_equal(true, (recycle_flags & FOF_WANTNUKEWARNING) != 0,
	             "requested recycle keeps Windows permanent-delete warning");
	assert_equal(true, (recycle_flags & FOF_SILENT) != 0, "requested recycle suppresses progress UI");
	assert_equal(false, (recycle_flags & FOF_NOCONFIRMATION) != 0,
	             "requested recycle lets Windows ask before permanent fallback");

	const auto permanent_flags = platform::probe_shell_delete_flags(false);
	assert_equal(true, (permanent_flags & FOF_NOCONFIRMATION) != 0,
	             "explicit permanent delete suppresses duplicate shell confirmation");
	assert_equal(false, (permanent_flags & FOF_ALLOWUNDO) != 0, "explicit permanent delete does not allow undo");
	assert_equal(false, (permanent_flags & FOF_WANTNUKEWARNING) != 0,
	             "explicit permanent delete was already confirmed by the application");
}

static void should_read_coherent_number_format_snapshots()
{
	platform::set_number_format_probe_snapshots({
		                                            {L".", L",", 1, 1},
		                                            {L",", L".", 1, 1},
	                                            },
	                                            true);
	const df::scope_exit restore([] { platform::clear_number_format_probe_snapshots(); });

	assert_equal("1,234", platform::format_number("1234"), "the first format uses one complete snapshot");
	assert_equal(",", platform::number_dec_sep(), "invalidation during refresh remains pending for the next read");
	assert_equal("1.234", platform::format_number("1234"), "the second format uses the next complete snapshot");
	assert_equal(",", platform::number_dec_sep(), "the separator belongs to the same refreshed snapshot");
}

static void should_reject_unusable_file_names()
{
	assert_equal(true, platform::is_valid_file_name("holiday 2024.jpg"), "an ordinary name is usable");
	assert_equal(true, platform::is_valid_file_name("caf\xc3\xa9 \xe6\x97\xa5.jpg"), "non-ascii is usable");
	assert_equal(true, platform::is_valid_file_name("console.txt"), "a reserved name as a prefix is usable");

	assert_equal(false, platform::is_valid_file_name(""), "an empty name is not usable");
	assert_equal(false, platform::is_valid_file_name("a?b.jpg"), "a reserved character is not usable");
	assert_equal(false, platform::is_valid_file_name("a\tb.jpg"), "a control character is not usable");
	assert_equal(false, platform::is_valid_file_name("trailing."), "a trailing dot is not usable");
	assert_equal(false, platform::is_valid_file_name("trailing "), "a trailing space is not usable");
	assert_equal(false, platform::is_valid_file_name("CON"), "a device name is not usable");
	assert_equal(false, platform::is_valid_file_name("con.txt"), "a device name with an extension is not usable");
	assert_equal(false, platform::is_valid_file_name("Lpt9.jpeg"), "device names are case insensitive");
}

static void should_publish_pasted_bitmap_after_closing_stage()
{
	const auto folder = _temps.folder().combine(std::format("pasted-bitmap-{}", platform::tick_count()));
	assert_equal(true, platform::create_folder(folder).success(), "create bitmap paste temp folder");

	const uint32_t pixels[] = {
		0xffff0000, 0xff00ff00,
		0xff0000ff, 0xffffffff
	};
	const auto bitmap = CreateBitmap(2, 2, 1, 32, pixels);
	assert_equal(true, bitmap != nullptr, "create test bitmap");
	const df::scope_exit delete_bitmap([bitmap] { DeleteObject(bitmap); });

	const auto result = save_bitmap_info(folder, "pasted", true, bitmap);

	assert_equal(true, result.success(), "bitmap save succeeds");
	assert_equal(1, static_cast<int>(result.created_files.files.size()), "one published file");
	const auto final_path = result.created_files.files.front();
	assert_equal(true, final_path.exists(), "published file exists");

	const auto attributes = GetFileAttributesW(platform::to_file_system_path(final_path).c_str());
	assert_equal(false, attributes == INVALID_FILE_ATTRIBUTES, "published file attributes read");
	assert_equal(false, (attributes & FILE_ATTRIBUTE_TEMPORARY) != 0, "published file is not temporary");

	auto stage_count = 0;
	for (const auto& entry : std::filesystem::directory_iterator(platform::to_file_system_path(folder)))
	{
		const auto name = entry.path().filename().wstring();
		if (name.starts_with(str::utf8_to_utf16(df::app_file_stem()) + L"_")) ++stage_count;
	}
	assert_equal(0, stage_count, "owned stage file was removed or published");
}

// The recent-folder list must not wait on a share that has dropped, so a network location is
// recognised from its spelling and volume type without being reached.
static void should_recognise_network_locations()
{
	assert_equal(true, platform::is_network_location(df::folder_path(R"(\\server\share\photos)")),
	             "a UNC share is a network location");
	assert_equal(false, platform::is_network_location(_temps.folder()), "the local temporary folder is not");
}

void register_win_tests(test_registry& tests)
{
	tests.add("Should convert dates with their own DST offset"s, should_convert_dates_with_their_own_dst_offset);
	tests.add("Should tabulate time zone offsets like the system"s, should_tabulate_time_zone_offsets_like_the_system);
	tests.add("Should convert the current zone like the system"s, should_convert_the_current_zone_like_the_system);
	tests.add("Should tabulate local time once per year"s, should_tabulate_local_time_once_per_year);
	tests.add("Should pass bare save dialog extensions"s, should_pass_bare_save_dialog_extensions);
	tests.add("Should convert extended file system paths"s, should_convert_extended_file_system_paths);
	tests.add("Should convert utf8 to ansi"s, should_convert_utf8_to_ansi);
	tests.add("Should persist strings in registry"s, should_persist_to_registry);
	tests.add("Should validate registry value types and sizes"s, should_validate_registry_value_types_and_sizes);
	tests.add("Should preserve writability probe collisions"s, should_preserve_writability_probe_collisions);
	tests.add("Should initialize Unicode INI settings"s, should_initialize_unicode_ini_settings);
	tests.add("Should read INI values earlier builds wrote"s, should_read_ini_values_earlier_builds_wrote);
	tests.add("Should double-terminate shell destination buffers"s, should_double_terminate_shell_destination_buffers);
	tests.add("Should classify an aborted shell operation as cancelled"s, should_classify_an_aborted_shell_operation_as_cancelled);
	tests.add("Should let Windows confirm recycle that would destroy"s, should_let_windows_confirm_recycle_that_would_destroy);
	tests.add("Should read coherent number format snapshots"s, should_read_coherent_number_format_snapshots);
	tests.add("Should reject unusable file names"s, should_reject_unusable_file_names);
	tests.add("Should publish pasted bitmap after closing stage"s, should_publish_pasted_bitmap_after_closing_stage);
	tests.add("Should recognise network locations"s, should_recognise_network_locations);
}
