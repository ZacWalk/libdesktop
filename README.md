# libdesktop

[![Build](https://github.com/ZacWalk/libdesktop/actions/workflows/build.yml/badge.svg)](https://github.com/ZacWalk/libdesktop/actions/workflows/build.yml)

A small C++20 platform layer for desktop applications, in six static libraries:

| Library | Alias | Contents |
|---|---|---|
| **libdesktop-core** | `libdesktop::core` | One OS-free header, `src/core/platform.h`, and a backend that implements it. Windows, drawing, fonts, menus, input, clipboard, files, HTTP, audio, timers, threading, child processes — all behind `pf::`, so application code never sees a `HWND`. |
| **libdesktop-ui** | `libdesktop::ui` | The reusable presentation layer in `src/ui`: widgets, text and document views, markdown rendering, lists and the prompt composer, in `pf::ui`. Opt in with `platform_add_app(<target> UI ...)`. |
| **libdesktop-text** | `libdesktop::text` | The part of `src/ui` that is standard C++ alone: the text layer's vocabulary (`text_types.h`) and the syntax highlighters (`syntax.h`), in `pf::ui`. libdesktop-ui is written in it; an application on another window model links it alone to colour text it draws itself. |
| **libdesktop-system** | `libdesktop::system` | The foundation and OS services that began as Diffractor's, in `include/libdesktop`: `df::` and `str::` (strings, paths, dates, geometry, containers, hashing, logging, JSON) and `platform::` (files, settings, processes, threads, known folders, the shell, sharing, the web, crash recovery), for Windows and Linux. Opt in with `LIBDESKTOP_SYSTEM`; [docs/system.md](docs/system.md) describes it. |
| **libdesktop-render** | `libdesktop::render` | The desktop UI that began as Diffractor's, written in libdesktop-system: the `ui::` types, draw contexts, frames, controls and commands, the Direct3D 11 and software renderers, DirectWrite text, WIC, native controls, Media Foundation movie writing and sound, for Windows, with Linux stand-ins. Opt in with `LIBDESKTOP_RENDER`; [docs/render.md](docs/render.md) describes it. |
| **libdesktop-os** | `libdesktop::os` | The OS services libdesktop-core and libdesktop-system share, in `os::`: UTF-8/extended-path files and dialogs, escaped INI storage, the web client, clipboard text, known folders and the audio endpoint both stacks' sound plays through. A service here names no application and writes no log, so both call it; neither exposes it to an application. Windows only so far. |

libdesktop-ui operates on a UTF-8 text buffer the application hands it. It never
opens a file, never knows a path, and never names an application — loading and
saving stay in the app.

libdesktop-core, libdesktop-ui and libdesktop-os have one backend, Win32/MSVC; libdesktop-text needs none and builds
everywhere libdesktop does.
libdesktop-system has two: Windows with MSVC and Linux with GCC, and libdesktop-render builds on both,
with stand-ins on Linux. On Windows libdesktop-core and libdesktop-system both link libdesktop-os; its
Linux implementations are still libdesktop-system's own.

[dde](https://github.com/ZacWalk/dde) builds on libdesktop-ui;
[Potato](https://github.com/ZacWalk/potato),
[Stunt Car Racer](https://github.com/ZacWalk/stuntcarracer) and
[libdos](https://github.com/ZacWalk/libdos)'s dui on libdesktop-core; and
[Diffractor](https://github.com/ZacWalk/diffractor) on libdesktop-system,
libdesktop-render and libdesktop-text.
[ImageWalker](https://github.com/ZacWalk/imagewalker)'s shared imaging adapters
use core and system, and its 3.0 GUI also uses ui; the historical WTL releases
retain their own interfaces. [lib8bit](https://github.com/ZacWalk/lib8bit)'s
host, [fast-draw](https://github.com/ZacWalk/fast-draw),
[low-ban](https://github.com/ZacWalk/low-ban) and
[zpilot](https://github.com/ZacWalk/zpilot) use core.
[SysMate](https://github.com/ZacWalk/sys-mate) uses core and ui;
[ergo-active](https://github.com/ZacWalk/ergo-active) also uses system.
Camera and monitoring APIs, printing and zpilot's specialized Direct3D
renderer remain explicit application backends, not alternate window layers.

## Architecture

### `src/core/platform.h` — the platform-independent API

Declarations only, with no OS headers. If a declaration cannot be implemented
somewhere other than Windows, it does not belong here.

| Category   | Key types / functions |
|------------|----------------------|
| Text       | UTF-8/16/32 conversion, `pop_utf8_char`, `utf8_truncate`, surrogate handling with `U+FFFD` substitution |
| Windowing  | `window_frame`, `frame_reactor`, neutral `window_options` (initial/minimum client size and fullscreen), `platform_events`, and the `app_init` / `app_idle` / `app_destroy` callbacks |
| Drawing    | `draw_context` — shapes, text, bitmaps, clipping; shared pixel surfaces and `present_pixels` for app-owned frame loops |
| Fonts      | Creation, measurement (thread-local measurement DC), metrics |
| Menus      | Menu tree with accelerators, built at runtime |
| Input      | Mouse, keyboard (`key_down` / `key_up` / `char_input`), focus, caret |
| Clipboard  | Copy / paste text |
| Files      | UTF-8/extended paths, structured errors, atomic writers, identity/hash services, enumeration, open/save/folder dialogs, explicit INI/registry settings, config ini, `is_path_within`, local/roaming app-data paths |
| Images     | WIC bitmap loading, bounded thumbnail/area decoding, cancellation, progress, original dimensions in the returned orientation, metadata probing and installed-extension discovery; neutral BGRA blending/interpolation and bounded OLE compound-preview rasterisation; native codecs remain libfiles' responsibility |
| Networking | `web_request` / `web_response`, plus an async HTTP client |
| Audio      | `sound_buffer` for a sample held in memory; `audio_stream` for PCM the app generates as it goes |
| Resources  | `embedded_resource_data` / `embedded_resource_text` |
| Timers     | Performance counter, sleep, periodic callbacks |
| Threading  | `run_async`, `run_ui` (marshal to the UI thread), `create_serial_executor` |
| Processes  | `spawn_child_process` with line callbacks, or the lower-level `process_spawn` / `process_read` / `process_write` for binary streams drained by the caller; `find_executable`, `quote_command_arg`, `try_lock_instance` |
| Console    | `write_stdout`, `read_stdin`, `attach_console` for CLI and stdio-protocol modes |

### Backends

- **`platform_win.cpp`** — the Win32 implementation: `WinMain` and the message
  loop, GDI drawing, WinHTTP (asynchronous) networking,
  common dialogs, clipboard, spell checking, the optional tray service and
  second-launch signal. The synchronous web client, files, dialogs, settings,
  clipboard text and known folders are libdesktop-os's, which `pf::` wraps.
- **`platform_win_image.cpp`** — the WIC bitmap adapter, kept separate so
  historical applications and console consumers can load images without
  pulling in the GUI entry point's application callbacks. Probes report
  stored dimensions and the remaining orientation; decoded results report
  original dimensions in their returned orientation. Structured file/probe
  APIs normalize extended paths for literal filenames; legacy loaders retain
  their original filename policy. Each entry point owns a balanced COM
  initialization, including calls made from an existing STA or MTA.
- **`platform_pixel.cpp` / `platform_pixel_avx2.cpp`** — neutral BGRA
  blending and interpolation with scalar, SSE2 and runtime-safe AVX2
  backends; only the private AVX2 translation unit is compiled with
  `/arch:AVX2`.
- **`platform_win_compound.cpp`** — bounded extraction of
  `PIDSI_THUMBNAIL` from an OLE compound document and WMF/EMF rasterisation
  into `pf::bitmap_load_result`, retaining logical source dimensions while
  capping the rendered extent.
- **`platform_audio.cpp`** — the mixer behind `sound_buffer` and
  `audio_stream`, playing through libdesktop-os's audio endpoint. It includes no
  OS header, so only the endpoint is written per system.
- **`platform_common.cpp`** — the parts with no OS dependency at all; compiles
  on every target.

### `src/ui/` — the presentation layer

Everything here is in `pf::ui`. It may use `pf::`; it may not name an
application, open a file, know a path or speak a protocol. A file is reached as
`#include "ui/<file>.h"`. `ui/ui.h` gathers the core building blocks —
`platform.h` and the theme, text model, host, syntax, spell, markdown, widget
and table-layout headers — but no view, so a view's header, such as
`view_markdown.h`, is included explicitly. `text_types.h` and `syntax.h` are
libdesktop-text: they include the standard library alone, compare extensions
and keywords without regard to ASCII case, and are reached by the same spelling.

| Category | Key types / files |
|----------|-------------------|
| Theme | `theme` — the colours, fonts and spacing a view draws with. Metrics are data because every consumer sets them; `style_color` is virtual because an application may want its own palette without touching a highlighter |
| Hosting | `view_host` — what a view asks of the application: invalidation, `ensure_visible`, status. `pane_host` and `hosted_window` for an application that draws panes into rectangles rather than giving each a child window |
| Text model | `text_buffer`, `text_line` — UTF-8 lines, selection, undo. `text_types.h` for `text_location`, `text_selection`, `text_style` |
| Untrusted text | `input::normalize` and `input::char_assembler` — bounding and filtering text before it enters a prompt. Separate from the composer because the filtering is the security-relevant half and is worth testing on its own |
| Views | `view_base` → `text_view` → `doc_view`, then `edit_doc_view` and `read_only_doc_view`. `text_view` owns selection, scrolling and the clipboard *including its shortcuts*, so a view with no menu behind it can still be copied from |
| Rendered views | `markdown_view` over the `md` model, `csv_view`, `hex_view`, `list_view` — all over the source text, so what is on screen is still at a real position in the document and selection keeps working |
| Prompt | `composer` — an `edit_doc_view` that grows with what is typed, keeps a history, filters everything that can put text in it, and keeps a prompt its application refuses |
| Widgets | `edit_box`, `caret_blinker`, `splitter`, `custom_scrollbar`, `table_layout` |
| Support | `syntax` highlighters, `spell` checking, and `test_support.h` — headless fakes for `measure_context`, `draw_context`, `view_host` and `window_frame` |

Layout and hit testing are testable without a window, because
`pf::measure_context` is an interface and `test_support.h` implements one.

### File layout

| File | Contents |
|---|---|
| `src/core/platform.h` | The whole `pf::` surface. Declarations, plus inline text/geometry helpers |
| `src/core/platform_common.cpp` | Backend-independent implementation (the embedded-resource registry, `line_splitter`) |
| `src/core/platform_win.cpp` | Win32 backend: `WinMain`, message loop, GDI drawing, WIC, async WinHTTP, dialogs, clipboard, spell check, child processes |
| `src/core/platform_audio.h` / `.cpp` | The mixer behind `sound_buffer` and `audio_stream`, and the thread that plays it through libdesktop-os's endpoint; no OS header |
| `src/ui/ui.h` | libdesktop-ui umbrella header for the core UI building blocks |
| `src/ui/theme.h` / `.cpp` | The `pf::ui::colors` table, the `theme` metrics, the style palette and the heading fonts |
| `src/ui/text_types.h` | `text_location`, `text_selection`, `text_style`, `text_block`, `highlight_fn` (libdesktop-text) |
| `src/ui/view_host.h` | What a view asks of the application: invalidation, `ensure_visible`, status |
| `src/ui/widgets.h` | `edit_box`, `caret_blinker`, `splitter`, `custom_scrollbar`, `edit_box_widget` |
| `src/ui/table_layout.h` / `.cpp` | Table cell parsing and rendering, shared by the markdown and CSV views |
| `src/ui/syntax.h` / `.cpp` | The syntax highlighters; language choice is the caller's (libdesktop-text) |
| `src/ui/spell.h` / `.cpp` | The process-wide spell checker and its cache |
| `src/ui/text_line.h`, `text_buffer.h` / `.cpp` | The UTF-8 line and the editing model: lines, selection, undo |
| `src/ui/text_input.h` | `pf::ui::input` — filtering and bounding untrusted text before it enters a prompt |
| `src/ui/markdown.h` | `pf::ui::md` — the Markdown model, its parser and the HTML reducer |
| `src/ui/view_base.h` → `view_text.h` → `view_doc.h` | The view hierarchy: scrolling, selection and the clipboard, then the caret, wrap and hit testing. Ctrl+A/C/X/V live here rather than in any one view, because a view with no menu behind it gets no copy command |
| `src/ui/view_doc_edit.h`, `view_doc_readonly.h`, `view_markdown.h`, `view_csv.h`, `view_hex.h` | The views an application shows: editable, read-only, rendered Markdown, CSV tables and bytes. `read_only_doc_view` also answers `at_bottom` and `scroll_to_end`, so a transcript that grows can follow its end only when the reader was already there |
| `src/ui/view_composer.h` | `composer` — the prompt box: growing, history, and the input filter in front of it |
| `src/ui/view_list.h` | `list_view` and `list_item` — the panel list: rows, selection, hover, keyboard and copy |
| `src/ui/pane_host.h` | `pane_host` and `hosted_window` — running these views inside one window, for an application that draws its panes into rectangles rather than giving each a child window |
| `src/ui/test_support.h` | Headless fakes — `measure_context`, `draw_context`, `view_host`, `window_frame` |
| `cmake/platform_app.cmake` | `platform_add_app()` — the function apps use to declare themselves |
| `cmake/embed_resources.cmake` | Turns data files into a generated C++ byte-array TU |
| `cmake/app.manifest.in` | The manifest every app gets |
| `tests/platform_tests.cpp` | The libdesktop-core suite (CTest label `libdesktop`) |
| `tests/ui_tests.cpp` | The libdesktop-ui suite (CTest label `libdesktop-ui`) |
| `tests/text_tests.cpp` | The libdesktop-text suite (CTest label `libdesktop-text`), on Windows and Linux |
| `tests/check.h` | The counting checks the libdesktop-ui and libdesktop-text suites share |
| `include/libdesktop/system.h` | libdesktop-system's umbrella header: the dialect, the standard library and the module's headers, in order |
| `include/libdesktop/util*.h`, `platform.h`, `platform_compat.h`, `platform_environment.h` | The foundation and the services' API, reached as `#include "libdesktop/<file>.h"` |
| `include/libdesktop/platform_win.h`, `platform_win_process.h` | The Windows backend's semi-public headers, for an application's own window layer |
| `src/system/util*.cpp` | The foundation's implementation |
| `src/system/platform_win*.cpp`, `platform_linux*.cpp` | The Windows and Linux backends of the services |
| `cmake/system.cmake` | The libdesktop-system target, and RapidJSON at the pin in `cmake/dd-dependencies.json` |
| `tests/system_tests.cpp`, `tests/system_test.h` | The libdesktop-system suite (CTest label `libdesktop-system`), on Windows and Linux |
| `tests/system_win_tests.cpp`, `tests/system_tests.manifest` | Its Windows backend's own tests, and the manifest that runs them as an application runs |
| `include/libdesktop/render.h` | libdesktop-render's umbrella header: libdesktop-system, then `platform_render.h` and `ui.h` |
| `include/libdesktop/ui.h`, `platform_render.h`, `render_software.h` | The desktop UI's API, the services its backends implement and the software rasterizer |
| `include/libdesktop/platform_win_render.h`, `platform_win_visual.h`, `platform_win_ui_internal.h`, `platform_win_render_res.h` | The Windows backend's semi-public headers, for an application's own window layer, and its resource IDs |
| `src/render/ui.cpp`, `render_*.cpp` | The UI layer's definitions, surfaces, colour and the software rasterizer |
| `src/render/platform_win_*.cpp`, `platform_linux_ui.cpp` | The Windows backend - windows, controls, Direct3D 11, software, fonts, WIC, movie writing, sound - and the Linux stand-ins |
| `src/render/shaders/`, `src/render/res/` | The Direct3D shaders, compiled with fxc at build time, and the resource script an application's own includes, with its cursors and shadow |
| `cmake/render.cmake` | The libdesktop-render target, its shaders and `libdesktop_render_resources()` |
| `tests/render_tests.cpp`, `render_win_tests.cpp`, `render_tests.rc` | The libdesktop-render suite (CTest label `libdesktop-render`), on Windows and Linux |
| `include/libdesktop/os.h` | libdesktop-os: the services both stacks share, reached as `#include "libdesktop/os.h"` by their backends alone |
| `src/os/os_win.cpp` | Its Windows backend: UTF-8/extended-path files and dialogs, escaped INI storage, WinINet/WinHTTP, clipboard text and known folders |
| `src/os/os_win_audio.cpp` | Its WASAPI audio endpoint, the playback devices and the Pro Audio thread class |
| `tests/os_tests.cpp` | The libdesktop-os suite (CTest label `libdesktop-os`) |
| `tests/loopback_http.h` / `.cpp` | A scripted HTTP server on 127.0.0.1, so the web clients are tested offline |

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

Three things about the main window catch every new application:

- `app_init` runs **before** the native window exists. Binding a reactor and
  building the menu there is what it is for; a title, a size or a position set
  there is dropped. Set those when the reactor receives `message_type::create`.
- `message_type::close` does not close anything by itself: the platform returns
  the reactor's answer without the default handling, so a reactor that wants the
  close button, Alt+F4 and `WM_CLOSE` to work calls `window->close()`. Not
  calling it is how an application keeps the window open — to ask about unsaved
  work, say.
- `app_destroy` runs once the GUI loop (or `main_loop`) returns and the task
  queue has stopped: queued tasks are dropped, and running ones are given up to
  five seconds to finish before they are left to end on their own. So nothing
  queued can start on what `app_destroy` frees, but a worker that can run longer
  than that must be stopped or joined before what it uses is freed, and work
  posted from `app_destroy` never runs. COM is still up. It is not called when
  `app_init` returned `start_gui = false`; a headless mode cleans up after
  itself.

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

Five CMake targets are exported: `libdesktop::core`, `libdesktop::ui`,
`libdesktop::text` and, when a consumer sets `LIBDESKTOP_SYSTEM` before bringing the package in,
`libdesktop::system` ([docs/system.md](docs/system.md#using-it)), and with
`LIBDESKTOP_RENDER` as well, `libdesktop::render`
([docs/render.md](docs/render.md#using-it)). The
core and UI API is the `pf::` namespace, `platform.h`, the `ui/` headers and
`platform_add_app()`. Apps pull
the package in with `FetchContent` and declare themselves with
`platform_add_app()`:

```cmake
# Build against a sibling checkout when one exists; otherwise fetch the pin.
if(NOT DEFINED FETCHCONTENT_SOURCE_DIR_LIBDESKTOP
   AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/../libdesktop/CMakeLists.txt")
    set(FETCHCONTENT_SOURCE_DIR_LIBDESKTOP "${CMAKE_CURRENT_SOURCE_DIR}/../libdesktop" CACHE PATH "")
endif()

include(FetchContent)
FetchContent_Declare(libdesktop
    GIT_REPOSITORY https://github.com/ZacWalk/libdesktop.git
    GIT_TAG main)
FetchContent_MakeAvailable(libdesktop)

# UI is opt-in: drop it and the app links libdesktop-core alone.
platform_add_app(myapp UI
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
libdesktop-render's applications are the exception: each keeps its own script,
which includes the module's ([docs/render.md](docs/render.md#resources)).

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

Requires Windows x64 and Visual Studio with the Desktop C++ workload. The
vendored [dd](https://github.com/ZacWalk/dd) runtime locates Visual Studio and
uses the CMake and Ninja that ship with it.

```powershell
.\dd.ps1 build        # both configurations
.\dd.ps1 test         # build and run the libdesktop, libdesktop-ui, libdesktop-text, libdesktop-system, libdesktop-render and libdesktop-os suites
```

`dd.ps1` locates Visual Studio, enters the MSVC environment, and falls back to
the CMake and Ninja that ship with it, so nothing extra needs installing. To
drive CMake directly: `cmake --preset release && cmake --build --preset release`.

On Linux only libdesktop-system, libdesktop-render and libdesktop-text build, with GCC 13 or later:

```bash
cmake -S . -B build/linux -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build/linux
ctest --test-dir build/linux
```

The suite in `tests/` is a console program covering the parts that can be
checked without a window: text conversion, paths, geometry, embedded resources,
line splitting, argument quoting, path containment, instance locks, per-user
storage paths, headless pool/executor concurrency and teardown, the audio mixer
sample by sample with no device, and audio at zero volume through the device —
both a sample buffer and the stream queue that apps pace themselves against. It
skips the device cases when the machine has no output device.

## Known gaps

Found by hand-testing the applications, and not yet closed:

- **Alt keys never reach a reactor.** `WM_SYSKEYDOWN`, `WM_SYSKEYUP` and
  `WM_SYSCHAR` go straight to the default handling, so Alt, Alt+key and F10 open
  the menu bar or the system menu instead. libdos's DOS programs and potato's
  Alt+Left/Alt+Right want them. Forwarding them needs a way for a reactor to say
  it consumed a key — `handle_keyboard`'s result is not read that way today —
  or every application would see Alt+letter and open its menu with it.
- **A text view gives every code point one cell.** Wide characters — CJK, most
  emoji — are drawn one cell wide and the next character overprints their right
  half; hit testing and the caret follow the same grid. A proportional
  `text_font` misbehaves the same way, which is why `theme.h` asks for a
  fixed-pitch face.
- **`draw_bitmap` ignores alpha.** It copies pixels with `StretchDIBits`, so the
  transparent parts of a PNG draw black (potato's pages show it). `AlphaBlend`
  needs premultiplied pixels, and many 32-bit bitmaps carry alpha 0 while meant
  to be opaque, so it needs a per-bitmap "has alpha" decision rather than a
  switch, and a check in every application that draws images.
- **A CSV record is one line.** `csv_view` splits each physical line, so a quoted
  field that contains a line break draws its second half as a new record.
  Joining them changes the line-to-record mapping that selection and hit testing
  rely on.

## Documentation

[AGENTS.md](AGENTS.md) — conventions for contributors and coding agents.
[docs/system.md](docs/system.md) — libdesktop-system: using it, what the
application supplies, the window layer's hooks and the Linux backend.
[docs/render.md](docs/render.md) — libdesktop-render: using it, what the
application supplies, its resources and the Linux stand-ins.
[docs/todo.md](docs/todo.md) — open work and second adopters.

## License

LGPL-2.1-or-later; see [LICENSE](LICENSE). A program that links libdesktop
statically must let its users relink it against a modified libdesktop.
