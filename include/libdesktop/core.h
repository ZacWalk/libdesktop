// This file is part of libdesktop, a C++20 platform layer for desktop applications
// Copyright 2026  Zac Walker
// 
// This program is free software; you can redistribute it and / or modify it
// under the terms of the LGPL License either version 2.1 or later.
// License details are available at https://www.gnu.org/licenses/lgpl-2.1.html
// This program is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY

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

namespace core { using namespace std::literals; }

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
