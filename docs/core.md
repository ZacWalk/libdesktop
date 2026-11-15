# libdesktop::core

`libdesktop::core` is the vocabulary the other three libraries and their
applications are written in. It opens no window and touches no file, so a
model, a parser or a codec adapter can link it alone, and its suite runs
anywhere a C++20 compiler does.

## Using it

Link `libdesktop::core` and include `libdesktop/core.h`, which brings in the
compiler dialect, the standard library the code is written against and the
core headers in dependency order. `core/base64.h`, `core/cpu_features.h`,
`core/kdtree.h`, `core/log.h`, `core/mcp.h` and `core/simd.h` are included where
they are used. Each of the four namespaces has `std::literals` in it.

RapidJSON, header-only, is fetched at the pin in `cmake/dd-dependencies.json`;
a consumer that carries the same revision points
`FETCHCONTENT_SOURCE_DIR_RAPIDJSON` at its copy, so a program holds one. On
Linux core links threads. MSVC consumers are compiled with `/utf-8`.

## Headers

Each header's comments are its reference; this is where to look.

| Header | What it holds |
|---|---|
| **Foundation** | |
| `core.h` | The umbrella |
| `core/dialect.h` | What differs between MSVC and GCC or Clang, so portable code keeps one spelling: SAL annotations compiled away, and the MSVC CRT names the code uses |
| `core/util.h` | The application's name, the log, `blob`, `cspan`, `file_size`, `scope_exit`, `no_copy`, `cancel_token`, translatable `text_t`, clocks and performance timers |
| `core/primitives.h` | Locks (`mutex`, `shared_lock`, `exclusive_lock`, `try_exclusive_lock`), `thread_event`, tick counts, UTC time, secure random bytes and the CPU feature flags |
| `core/cpu_features.h` | Run-time SSE2 and AVX2 queries |
| `core/log.h` | `log_func`, which logs a scope's start and exit |
| **Text and data** | |
| `core/strings.h` | UTF-8, UTF-16 and UTF-32 conversion, code-point stepping and truncation, case folding, comparison, natural ordering, wildcards, splitting, trimming, number formatting, and interned strings (`cached`) |
| `core/base64.h` | Base64 encoding and decoding |
| `core/json.h` | RapidJSON documents, and readers that check a value's type before taking it |
| `core/mcp.h` | A Model Context Protocol server: the tools an application offers an agent, their schemas, calls and results |
| **Paths, dates and collections** | |
| `core/path.h` | `file_path` and `folder_path`, their comparison and hashing, and `is_server` |
| `core/selector.h` | `item_selector`: files chosen by path pattern, with or without subfolders |
| `core/date.h` | `date_t`, a point in time, and `day_t`, a calendar date and time of day |
| `core/map.h` | Hash maps and sets keyed by paths and by case-folded text |
| `core/kdtree.h` | A k-d tree over caller-owned points: nearest neighbour and rectangle queries |
| **Geometry and pixels** | |
| `core/geometry.h` | Points, sizes, rectangles, affine transforms and quads, in integers and doubles |
| `core/pixels.h` | Colours (`color32`, `color`), pixel formats, orientation, colour space and chroma siting, `surface` and `image`, blending, resizing and tone mapping |
| `core/simd.h` | SSE, AVX and NEON wrappers for the pixel and CRC loops |

## The application's name

The application calls `core::set_app_name(name)` before anything else: the
platform names its log, settings, app-data folder, temporary files and locks
from it ([platform.md](platform.md#names-made-from-the-applications-name)).
`core::app_file_stem()` is the name in lower case, the stem of those files.

`core::log(context, message)` writes a line through the sink the platform
installs when the application starts (`platform::start_log`); with no sink the
line goes nowhere, and a Debug build also sends it to the debugger.
`core::log_once` keeps a message that repeats for every item of a batch to one
line. `core::trace` is for the debugger alone, and does nothing in Release.
`core::set_log_observer` is told each line as it is written, beside the sink -
what an agent session keeps for its client to read; it must be quick, and must
not log.

## Serving an agent

`core::mcp::server` serves an application's tools to an agent over the Model
Context Protocol: newline-delimited JSON-RPC, a line in and a message out, over a
transport that is the caller's. On Windows and Linux `platform::run` serves it on
stdin and stdout ([platform.md](platform.md#the-application)). It speaks both
revisions in use: 2026-07-28, whose requests carry their revision and the
client's capabilities in `_meta` and which `server/discover` describes, and the
`initialize` handshake of the three before it. It offers tools only.

A `core::mcp::tool` has a name, a description for the model, JSON Schemas for its
input and output, hints - read-only, destructive, idempotent, reaching the
network - and a handler. The handler runs on whatever thread the server is
connected to post to - the UI thread, from `platform::run` - and must not block:
it holds the `core::mcp::call`, which carries the arguments, hears cancellation,
reports progress, and is finished once, from any thread, with a text or image
result and, where the tool has structure, the same as JSON. Arguments are checked
against the input schema first - types, properties, required, enumerations,
bounds, `anyOf` and `oneOf` - and an argument the schema refuses is answered as
the tool's error, which the model can correct. A message larger than 4 MiB, more
than 32 calls at once, a malformed request or an unknown method or tool is a
protocol error. A cancelled call, and every call running when the input closes,
is answered with nothing.

## Text

Text is UTF-8 everywhere; UTF-16 appears only where an operating system asks for
it, and the conversions are core's. A conversion never fails: an invalid
sequence or a lone surrogate becomes U+FFFD, while a surrogate pair encoded as
two UTF-8 sequences is kept as the character it pairs to. The code-point helpers
(`utf8_next`, `utf8_prev`, `utf8_truncate`, `utf8_codepoint_count`) step whole
code points, so a cut never lands inside one.

`icmp` and the other case-insensitive comparisons fold case beyond ASCII;
natural ordering compares runs of digits by value. `core::cached` is an
interned string: a 32-bit handle into an arena reserved once, so a record never
moves and equal handles are equal text.

## Paths

`file_path` and `folder_path` are UTF-8, typed so a file cannot be passed where
a folder is meant, and built from interned components. Either separator is
accepted and the preferred one written. Whether two spellings that differ only
in case are one path is the file system's rule, fixed at compile time:
case-insensitive on Windows and case-sensitive on Linux. Case-folded comparison
stays right for file types, tags and other vocabulary on both, but not for a
container keyed by path: key it with the path types' own comparison and hash,
as the maps in `core/map.h` do. Turning a path into the form an OS call takes
is [platform's](platform.md#files-and-settings).

## Dates

`date_t` counts 100-nanosecond intervals since 1601-01-01 UTC, the Windows
FILETIME unit, on every platform, so stored dates mean the same everywhere. A
calendar date out of range is no date on either backend: a year from 1601 to
30827, a day the month has, a time of day before 24:00:00, with a month or day
of 0 read as 1, rather than rolled into another day. Local time and formatting
ask the operating system, so they are platform's (`platform::utc_to_local`,
`platform::format_date`), as are the `std::formatter`s for `date_t` and
`file_size`.

## Colours and pixels

Two packings meet in every drawing:

- `core::color32` keeps red in the low byte: memory order R, G, B, A.
  `core::rgba(r, g, b, a)` builds one with alpha 255 unless told otherwise;
  `core::rgb(r, g, b)` leaves alpha at 0, which draws nothing once it becomes a
  `core::color`. `core::color` is the same colour as four floats.
- A `core::surface` holds rows top first, B, G, R, A in memory: a pixel read as
  a `uint32_t` is 0xAARRGGBB. Its alpha is straight, not premultiplied.

Writing a `color32` into a surface, or reading one out, swaps red and blue
unless it goes through `core::bgr` or `core::abgr`. A pixel of an ARGB surface
with alpha 0 is invisible once the surface is drawn as a texture.

A wide surface, `texture_format::RGBX16` or `RGBA16`, holds four 16-bit
channels, red first, the fourth 65535 or straight alpha: Direct3D's
R16G16B16A16, which a texture uploads it as. `get_pixel` and `set_pixel`
convert to and from a packed pixel's B, G, R, A; `to_8_bit` rounds a wide
surface to a byte a channel, and `to_16_bit` widens a packed one exactly.

An NV12 or P010 surface is planar: luma rows, then a row of interleaved chroma
for every two of them. It has no pixel to address, so `get_pixel`, `set_pixel`,
`clear`, `draw` and `swap_rb` leave it alone, and `pixel_difference` compares
each plane as it is stored. Its `chroma_siting` says where each chroma sample
lies among the luma it covers - with the left column, as MPEG-2, H.264 and HEVC
site 4:2:0, centred as JPEG does, or as a stream states - so whatever converts
it reads the chroma there.

`blend_constant_bgra` and `interpolate_bgra` run scalar, SSE2 or AVX2, chosen at
run time; the AVX2 code is compiled in a translation unit of its own, with
`/arch:AVX2` or `-mavx2`, and entered only after `core::has_avx2()`.
`area_downscale` reduces a packed, wide or NV12 surface by averaging areas,
`bilinear_resize` enlarges, `surface::transform` turns and mirrors, and
`tone_map_lut` shows an HDR picture as SDR with the same arithmetic on the CPU
as the Direct3D shaders use.

## Beneath the headers

No core header includes an operating system header. What a process-local
primitive needs from the system is implemented once per platform, in
`src/core/win` and `src/core/linux`: the locks (an SRWLOCK on Windows), thread
events, tick counts, UTC time and its calendar conversion, secure random bytes,
the arena interned strings live in, debug output and thread names. The rest of
core is standard C++.

## Tests

`libdesktop_core_tests`, in `tests/core/`, under the CTest label
`libdesktop-core`, links core alone. It covers text, paths, dates, k-d tree
queries and the surface operations, with every SIMD blend checked against the
scalar one, and the MCP server's handshakes, tool calls, schema checks,
progress and cancellation.

## Known gaps

- A few names are global: `M_PI` (where the C library has none) and `M_PIF`,
  `log_func`, the `df_assert_*` macros and, on GCC and Clang, the MSVC CRT names
  `core/dialect.h` supplies.
- On GCC and Clang, `core/dialect.h`'s `_snscanf_s` is a bounded `sscanf`, and
  can read damaged text differently from MSVC's.
