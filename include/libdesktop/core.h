// This file is part of libdesktop, a C++20 platform layer for desktop applications
// Copyright 2026  Zac Walker
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

// Purpose: core's umbrella; standard C++ types and utilities.

#pragma once

#include "libdesktop/core/dialect.h"

#include <cstdint>
#include <climits>
#include <cmath>
#include <ctime>
#include <chrono>
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

namespace core
{
	using namespace std::literals;
}

#include <thread>
#include <mutex>

#ifndef M_PI
constexpr double M_PI = 3.141592653589793238463;
#endif
constexpr float M_PIF = 3.14159265358979f;

#include "libdesktop/core/util.h"
#include "libdesktop/core/geometry.h"
#include "libdesktop/core/strings.h"
#include "libdesktop/core/path.h"
#include "libdesktop/core/primitives.h"
#include "libdesktop/core/date.h"
#include "libdesktop/core/selector.h"
#include "libdesktop/core/json.h"
#include "libdesktop/core/map.h"
#include "libdesktop/core/pixels.h"
