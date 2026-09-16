# platform-h

[![Build](https://github.com/ZacWalk/platform-h/actions/workflows/build.yml/badge.svg)](https://github.com/ZacWalk/platform-h/actions/workflows/build.yml)

A small C++20 platform layer for desktop applications: one OS-free header,
`platform.h`, and a backend that implements it. Everything the applications in
this workspace need from an operating system — windows, drawing, fonts, menus,
input, clipboard, files, HTTP, audio, timers, threading, child processes — is
declared behind the `pf::` namespace, so the application code never sees a
`HWND`.

There is currently one backend, Win32/MSVC.

## Architecture

### `platform.h` — the platform-independent API

Declarations only, with no OS headers. If a declaration cannot be implemented
somewhere other than Windows, it does not belong here.

| Category   | Key types / functions |
|------------|----------------------|
| Text       | UTF-8/16/32 conversion, `pop_utf8_char`, `utf8_truncate`, surrogate handling with `U+FFFD` substitution |
| Windowing  | `window_frame`, `frame_reactor`, the `app_init` / `app_idle` / `app_destroy` callbacks |
| Drawing    | `draw_context` — shapes, text, bitmaps, clipping; `present_pixels` for app-owned frame loops |
| Fonts      | Creation, measurement (thread-local measurement DC), metrics |
| Menus      | Menu tree with accelerators, built at runtime |
| Input      | Mouse, keyboard (`key_down` / `key_up` / `char_input`), focus, caret |
| Clipboard  | Copy / paste text |
| Files      | Paths, enumeration, open/save/folder dialogs, config ini, `is_path_within` |
| Networking | `web_request` / `web_response`, plus an async HTTP client |
| Audio      | `sound_buffer` for a sample held in memory; `audio_stream` for PCM the app generates as it goes |
| Resources  | `embedded_resource_data` / `embedded_resource_text` |
| Timers     | Performance counter, sleep, periodic callbacks |
| Threading  | `run_async`, `run_ui` (marshal to the UI thread) |
| Processes  | `spawn_child_process`, `find_executable`, `quote_command_arg` |

### Backends

- **`platform_win.cpp`** — the Win32 implementation: `WinMain` and the message
  loop, GDI drawing, WIC image decoding, WinINet (synchronous) and WinHTTP
  (asynchronous) networking, common dialogs, clipboard, spell checking.
- **`platform_win_audio.cpp`** — XAudio2, kept separate so the audio dependency
  stays isolated.
- **`platform_common.cpp`** — the parts with no OS dependency at all; compiles
  on every target.

## Usage

Applications implement three callbacks:

```cpp
app_init_result app_init(const pf::window_frame_ptr& frame,
                         std::span<const std::string_view> args);
void app_idle();
void app_destroy();
```

The platform owns the message loop and calls these at the appropriate times. An
application that needs its own frame loop — a game — returns a `main_loop`
callable in `app_init_result` and pumps messages with `pf::platform_events()`.

### Child processes

`spawn_child_process()` starts a tool and moves bytes to and from its standard
streams: three pipes, a reader thread each for stdout and stderr, whole lines
reassembled with `pf::line_splitter`, and every callback marshalled to the UI
thread. It has no idea what those lines mean — a protocol belongs in the app.
`find_executable()` resolves a bare name through `PATH` and `PATHEXT` without
ever searching the current directory, and `quote_command_arg()` quotes by the
`CommandLineToArgvW` rules so an argument cannot be split or injected.

### Bounded HTTP acquisition

`connect_to_host()` accepts a hostname (not a URL), defaults to HTTPS and port
443, and accepts an optional user-agent. `send_request()` accepts origin-form
paths such as `/v1/items`, optional encoded query pairs, raw GET/POST bodies,
multipart forms/uploads, and file downloads. Header names/values, multipart
names and paths are validated before sending; invalid requests never become
diagnostic strings containing URLs, credentials or response bodies.

For acquisition from an untrusted remote endpoint, opt in explicitly:

```cpp
pf::web_request request;
request.path = "/v1/items";
request.follow_redirects = false;
request.use_cookies = false;
request.max_response_bytes = 4 * 1024 * 1024;
request.max_header_bytes = 32 * 1024;
request.timeout_ms = 10000;
const auto response = pf::send_request(host, request);
if (response.error != pf::web_response_error::none) {
    // Discard this acquisition. No partial body is returned.
}
```

`web_response_error` distinguishes `invalid_request`, `transport` (including
timeouts, truncated responses and file I/O failures), and `response_limit`.
HTTP statuses such as 302, 404 and 500 are valid responses, not transport errors.
An obtained status is retained on limits and failures. A native rejection before
headers become available (including the native header-size limit) can leave
`status_code` zero; the platform does not invent an unavailable status.
`headers` contains the raw initial CRLF-delimited header block. Limits count its
bytes and body bytes after transfer decoding, independently; HTTP framing and
trailers consumed by the OS are not returned. Declared lengths are checked
before body allocation/file creation, and every read is checked before adding
bytes to memory or a file. A failed download may leave a partial file no larger
than the body limit; callers should discard it.

Defaults preserve the legacy WinINet path: redirects/cookies enabled, zero
limits, 30-second operation timeouts. Redirect/cookie opt-outs set per-request
WinINet flags; disabling cookies also rejects an explicit `Cookie` header.
Setting either size limit or a nonzero timeout selects an isolated WinHTTP
request. This avoids WinINet's silent acceptance of incomplete chunks and
provides cancellable operation deadlines and a native initial-header limit.
The public call remains synchronous; its private completion/cancellation
plumbing is separate from the asynchronous HTTP client. No shared host/global
timeout options are mutated. A zero timeout in this mode means 30 seconds per
operation, not a whole-transfer deadline (a progressing transfer may last longer).
Cookies in bounded mode, when enabled, are scoped to that single request and
its redirects, never shared with the browser or subsequent requests.
Certificate and hostname verification remain enabled for HTTPS; no
certificate-error bypass flags are set.

The added aggregate members preserve source defaults, but change the binary
layout of `web_request` and `web_response`: rebuild the library and every
consumer together; do not mix old binaries with new headers.

## Consuming it from an app

The library is a CMake package exporting `platform::platform`. Apps pull it in
with `FetchContent` and declare themselves with `platform_add_app()`:

```cmake
# Build against a sibling checkout when one exists; otherwise fetch the pin.
if(NOT DEFINED FETCHCONTENT_SOURCE_DIR_PLATFORM_H
   AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/../platform-h/CMakeLists.txt")
    set(FETCHCONTENT_SOURCE_DIR_PLATFORM_H "${CMAKE_CURRENT_SOURCE_DIR}/../platform-h" CACHE PATH "")
endif()

include(FetchContent)
FetchContent_Declare(platform_h
    GIT_REPOSITORY https://github.com/ZacWalk/platform-h.git
    GIT_TAG main)
FetchContent_MakeAvailable(platform_h)

platform_add_app(myapp
    SOURCES     src/main.cpp
    ICON        src/res/myapp.ico
    DESCRIPTION "My application"
    OUTPUT_NAME myapp-64
    EMBED       src/res/master.css)
```

`CMAKE_MSVC_RUNTIME_LIBRARY` must be set *before* `FetchContent_MakeAvailable`,
so the platform layer is built against the same CRT as the app.

## Resources and manifests

No app writes a `.rc` by hand and no app defines resource IDs.

- **Icon, version info** — declared as `platform_add_app()` arguments. The `.rc`
  is generated into the build tree and compiled by `rc`.
- **Manifest** — generated from `cmake/app.manifest.in` (per-monitor-V2 DPI,
  UTF-8 active code page, long paths, segment heap, common controls v6) and
  handed to the linker. Pass `MANIFEST <file>` to supply your own, or
  `MANIFEST NONE` to opt out.
- **Data files** — `EMBED` turns each file into a C++ byte array in a generated
  translation unit that self-registers before `main`. Look them up by file name,
  not by ID, and with no OS involvement:

  ```cpp
  const std::string_view css = pf::embedded_resource_text("master.css");
  const std::span<const uint8_t> blob = pf::embedded_resource_data("logo.png");
  ```

  `pf::platform_load_text_resource(int)` remains for Win32 `RT_HTML` resources
  but is the legacy path; it cannot work on another backend.

## Build and test

From an x64 Developer PowerShell:

```
.\dd.ps1 test            # build and run the unit suite (the default command)
.\dd.ps1 test -TestFilter '^platform_http$'  # only the offline HTTP regressions
.\dd.ps1 build -Config Debug
.\dd.ps1 clean
```

`dd.ps1` locates Visual Studio, enters the MSVC environment, and falls back to
the CMake and Ninja that ship with it, so nothing extra needs installing. To
drive CMake directly: `cmake --preset release && cmake --build --preset release`.

The suite in `tests/` is a console program covering the parts that can be
checked without a window: text conversion, paths, geometry, embedded resources,
line splitting, argument quoting, path containment, and audio at zero volume
(XAudio2 needs no window) — both a sample buffer and the stream queue that apps
pace themselves against. It skips the audio cases when the machine has no
output device.

CTest also runs a bounded, raw-TCP loopback HTTP fixture (GET/POST byte
fidelity, headers/user-agent, redirects, cookie isolation, declared/chunked/EOF
limits, truncation, timeouts, multipart uploads and downloads), and configures,
builds and runs a separate offline `FetchContent` consumer. Tests open no
windows and contact no external providers. HTTPS defaults are checked at the
API boundary and in backend flags; there is no local trusted TLS fixture, so
the suite does not claim a live HTTPS handshake test.
