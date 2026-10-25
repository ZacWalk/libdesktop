// libdesktop/system.h — libdesktop-system's umbrella header: the compiler dialect, the standard
// library the module is written against, the foundation (df::, str::) and the OS services
// (platform::), in the order they depend on one another.
//
// Include it before any other header of the module; those are not standalone. The headers it leaves
// out - util_base64.h, util_kdtree.h, util_log.h, util_simd.h and the Windows backend's
// platform_win.h and platform_win_process.h - are included where they are used. The application
// defines s_app_name (util.h).

#pragma once

#include "libdesktop/platform_compat.h"

#include <cstdint>
#include <climits>
#include <cmath>
#include <ctime>
#include <clocale>
#include <cassert>
#include <cctype>
#include <cwctype>
#include <cinttypes>
#include <cstring>

#include <bit>
#include <new>
#include <string>
#include <string_view>
#include <charconv>
#include <atomic>
#include <memory>
#include <functional>
#include <vector>
#include <array>
#include <map>
#include <set>
#include <unordered_set>
#include <unordered_map>
#include <tuple>
#include <algorithm>
#include <queue>
#include <iterator>
#include <limits>
#include <locale>
#include <sstream>
#include <fstream>
#include <regex>
#include <numeric>
#include <any>
#include <iomanip>
#include <optional>
#include <span>
#include <variant>
#include <format>
#include <filesystem>

#include <thread>
#include <mutex>

// The module is written against the standard literals ("text"sv, 10ms); a consumer gets them too.
using namespace std::literals;

// glibc's <cmath> defines M_PI as a macro; MSVC only does under _USE_MATH_DEFINES.
#ifndef M_PI
constexpr double M_PI = 3.141592653589793238463;
#endif
constexpr float M_PIF = 3.14159265358979f;

#include "libdesktop/util.h"
#include "libdesktop/util_geometry.h"
#include "libdesktop/util_strings.h"
#include "libdesktop/util_path.h"
#include "libdesktop/platform_environment.h"
#include "libdesktop/platform.h"
#include "libdesktop/util_date.h"
#include "libdesktop/util_selector.h"
#include "libdesktop/util_file.h"
#include "libdesktop/util_json.h"
#include "libdesktop/util_map.h"
