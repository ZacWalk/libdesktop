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
| Files      | Paths, enumeration, open/save/folder dialogs, config ini, `is_path_within`, `local_app_data_path` |
| Networking | `web_request` / `web_response`, plus an async HTTP client |
| Audio      | `sound_buffer` for a sample held in memory; `audio_stream` for PCM the app generates as it goes |
| Resources  | `embedded_resource_data` / `embedded_resource_text` |
| Timers     | Performance counter, sleep, periodic callbacks |
| Threading  | `run_async`, `run_ui` (marshal to the UI thread), `create_serial_executor` |
| Processes  | `spawn_child_process`, `find_executable`, `quote_command_arg`, `try_lock_instance` |

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

### Per-user storage and single-instance applications

`local_app_data_path()` returns the current user's non-roaming application-data
directory (the LocalAppData Known Folder on Windows), or an empty path on failure.
It neither reads an environment-variable shortcut nor creates an app subdirectory.

`try_lock_instance(name)` attempts a nonblocking, per-user lock shared across login
sessions. Use an app-specific name of 1–128 ASCII letters, digits, `.`, `_` or `-`.
Keep the returned `lock` alive for the application's lifetime. The move-only
`instance_lock_ptr` releases on destruction, on any thread; process termination
also releases the native handle. `already_running` means another holder exists,
whereas a nonempty `error` reports invalid input or an operating-system failure.
Failures are also sent to `debug_trace`, never standard output.

The Windows backend uses a named mutex whose name includes the user's SID.
It holds the object alive without claiming thread ownership, so even a second
attempt on the same thread is rejected rather than recursively acquiring it.

### Background and serialized work

`run_async()` lazily starts four workers and dispatches one queued task per
worker at a time. It works in `app_init`, headless modes, and console programs
without entering `platform_run`; callbacks may overlap, so they must synchronize
shared state. `run_ui()` only queues callbacks: the GUI loop or explicit
`pump_ui_tasks()` calls deliver them on the calling UI thread.

`create_serial_executor()` starts a separate worker immediately, returning
`nullptr` with a diagnostic if creation fails. Its `post(task)` returns false
for an empty task or after shutdown; accepted tasks run FIFO. `stop()` rejects
new work, drains accepted tasks and joins the worker. Destruction also stops
the executor. When called on its own worker, stop/destruction does not self-join:
the queue state survives until its accepted work finishes. Exceptions in tasks
are diagnosed without terminating either executor.

Use a dedicated executor for a serialized database writer or a long-running
queue consumer. Pool jobs may wait for that independent executor, but must not
occupy every pool worker while waiting for more work on the same pool. A
long-running consumer must receive its own shutdown signal before its executor
can finish draining.

Headless return/process teardown drains the async pool; pending UI callbacks are
discarded, not invoked after application state has gone away. GUI shutdown
cancels queued pool work and waits at most five seconds for active jobs, then
detaches any remaining workers. Their queue/dispatcher state stays valid for
late completions, and new background/UI submissions are ignored after shutdown.
The application still owns cancellation of network operations and the lifetime
of objects its tasks reference.

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
.\dd.ps1 build -Config Debug
.\dd.ps1 clean
```

`dd.ps1` locates Visual Studio, enters the MSVC environment, and falls back to
the CMake and Ninja that ship with it, so nothing extra needs installing. To
drive CMake directly: `cmake --preset release && cmake --build --preset release`.

The suite in `tests/` is a console program covering the parts that can be
checked without a window: text conversion, paths, geometry, embedded resources,
line splitting, argument quoting, path containment, instance locks, per-user
storage paths, headless pool/executor concurrency and teardown, and audio at zero volume
(XAudio2 needs no window) — both a sample buffer and the stream queue that apps
pace themselves against. It skips the audio cases when the machine has no
output device.
