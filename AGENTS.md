# AGENTS.md

libdesktop is the platform layer shared by twelve applications. `dde`,
`libdos`, `potato`, `stuntcarracer`, `lib8bit`'s host, `fast-draw`, `low-ban`
and `zpilot` build on libdesktop-core; `sys-mate` also uses libdesktop-ui.
ImageWalker 3.0 and `ergo-active` use core, ui and system, while ImageWalker's
shared imaging adapters use core and system. Diffractor builds on system,
render and text. It is a library with no application of its own.

It ships six static libraries: `libdesktop-core` (`pf::`), `libdesktop-ui`
(`pf::ui`), `libdesktop-text` (the part of `pf::ui` that is standard C++ alone:
`ui/text_types.h` and `ui/syntax.h`), opt-in `libdesktop-system` (`df::`,
`str::` and `platform::`) and `libdesktop-render` (`ui::`), and `libdesktop-os`
(`os::`), the services libdesktop-core and libdesktop-system share;
[README.md](README.md#libdesktop) is the canonical overview of their contents
and aliases.

libdesktop-core and libdesktop-ui's C++ API is the `pf::` namespace,
`platform.h`, the `ui/` headers and `platform_add_app()`; apps link the CMake
targets `libdesktop::core` and `libdesktop::ui`.

libdesktop-ui's ownership boundary is described in
[README.md](README.md#srcui--the-presentation-layer).

## Non-negotiables

1. **`src/core/platform.h` includes no OS headers.** Not `windows.h`, not
   transitively. The Windows SDK stays in `src/core/platform_win*.cpp`;
   `src/core/platform_audio.cpp` is standard C++ over libdesktop-os.
2. **The API must be expressible on another OS.** Nothing Win32-shaped may leak
   into a signature: no `HWND`, no `HRESULT`, no hundredths-of-a-decibel volume,
   no resource IDs. Prefer neutral units — linear gain, normalised pan, Hz,
   UTF-8 `string_view`. If a concept only makes sense on one platform, it
   belongs in the app.
3. **App-specific knowledge stays out.** No app names, no resource identifiers,
   no game constants. `config_set_app_name` exists precisely so the layer never
   has to know who is calling.
   Windows backend character-set definitions are private; consumers choose
   their own character set rather than inheriting Unicode from core.
4. **A change here reaches every consumer.** Adding to `window_frame` or
   `frame_reactor` is a breaking change for every app and their test stubs.
   Build all of them before you consider a change done — see below.
   `libdesktop::core` and `libdesktop::ui` are the names every one of them
   links; renaming either moves its consumers in the same step.
5. **Add a test with a capability.** `tests/platform_tests.cpp`,
   `tests/ui_tests.cpp` and `tests/text_tests.cpp` are console programs;
   anything checkable without a window belongs in one of them.
6. **libdesktop-text stays standard C++.** It is built on every platform and
   linked by an application on another window model, so `ui/text_types.h`,
   `ui/syntax.h` and `ui/syntax.cpp` include neither `platform.h` nor anything
   else of libdesktop's.

## Layout

The file layout is canonical in [README.md](README.md#file-layout).

## Build and test

Run the repository gate after changes here:

```pwsh
.\dd.ps1 test
```

The gate builds and tests the `libdesktop`, `libdesktop-ui`, `libdesktop-text`,
`libdesktop-system`, `libdesktop-render` and `libdesktop-os` CTest labels, in Debug and Release.
The os and ui suites share a CTest resource lock because both change the real
clipboard during their round trips; preserve that lock when adding suites.
Their fixtures also bound retries for a temporarily busy clipboard; they do
not skip failed round trips or change the production APIs' failure behavior.
libdesktop-system, libdesktop-render and libdesktop-text also build on Linux: run
the commands in the [README](README.md#build-and-test) there too after changing
any of them. The
[README](README.md#build-and-test) has the quick build commands.

This repository vendors the [dd](https://github.com/ZacWalk/dd) build driver
unchanged as `dd.ps1` and `.dd/`, with file hashes recorded in
[docs/dd-upstream.json](docs/dd-upstream.json). Project settings live in
`dd.psd1` and dependency pins in `cmake/dd-dependencies.json`
(`dependencies.owner = 'dd'`); `.\dd.ps1 commands --json` lists this project's
own commands. dd's
[two modes](https://github.com/ZacWalk/dd/blob/main/docs/mcp.md#two-modes) —
CLI, and MCP for an MCP client — and its
[commands](https://github.com/ZacWalk/dd/blob/main/docs/cli.md#commands) are
documented in its own repository.

The workspace's root AGENTS.md has the cross-repository gate for every consumer.

Every workspace consumer uses a sibling `../libdesktop` checkout
automatically, whatever its HEAD, so a local edit reaches them with no publish
step. Local success does not validate their published pins; use the workspace
gate and publish the library before advancing consumer revisions.
Open work, including the candidate second adopters of each module, is in
[docs/todo.md](docs/todo.md).

## Adding a capability

1. Declare it in `src/core/platform.h`, in neutral terms (see rule 2).
2. Implement it in the backend.
3. Add a case to `tests/platform_tests.cpp` if it can run without a window.
4. Rebuild every app. A pure virtual added to `window_frame` must be implemented
   three times, all in this repository: `win_impl` in `platform_win.cpp`,
   `hosted_window` in `pane_host.h` and `fake_window_frame` in `test_support.h`.
   One added to `frame_reactor` breaks every application and `pf::ui::view_base`,
   since each implements it.

## libdesktop-system

The foundation and OS services that began as Diffractor's, described in
[docs/system.md](docs/system.md). It is opt-in: no `pf::` application builds it
unless it sets `LIBDESKTOP_SYSTEM`, so a change here reaches only the
applications that do.

Windows backend character-set definitions are private. A consumer chooses its
own character set; in particular, linking system must not force ImageWalker's
historical `_MBCS` release to compile as Unicode.

1. **The API headers include no OS header.** `platform.h`, the `util*.h` headers
   and `system.h` are portable; the Windows SDK appears only in
   `src/system/platform_win*.cpp` and the backend's semi-public
   `platform_win.h` and `platform_win_process.h`, and Linux's system headers
   only in `src/system/platform_linux*.cpp`.
2. **It names no application.** The application defines `s_app_name`, and every
   file, folder, key and temporary name comes from it. A message names no
   application either; the application rewords its own set.
3. **It implements everything `platform.h` declares, on both backends.** A
   service that only a window layer can implement is declared by that layer.
   A Linux stand-in answers "not available" rather than report a success it
   did not have.
4. **Check Debug as well as Release, and Linux as well as Windows.** An
   assertion compiles only in Debug, so a reference the Release build never sees
   can still tie the module to an application.
5. **Tests move with the code.** `tests/system_tests.cpp` uses Diffractor's
   assertions (`tests/system_test.h`), so a test can move between that suite
   and this one unchanged.

## libdesktop-render

The desktop UI that began as Diffractor's, written in libdesktop-system and
described in [docs/render.md](docs/render.md). Opt-in like libdesktop-system, and
reaching only the applications that set `LIBDESKTOP_RENDER`.

1. **The API headers include no OS header.** `ui.h`, `platform_render.h`,
   `render_software.h` and `render.h` are portable; the Windows SDK appears only
   in `src/render/platform_win*.cpp` and the backend's semi-public
   `platform_win_*.h`, meant for an application's own window layer.
2. **It names no application.** What it needs of one - the chrome's glyphs
   (`ui::chrome_icons`), the application it hosts (`create_app`), the window icon
   and icon font in the application's resource script, its decoders and scaler
   as hooks - the application supplies. It sees no decoder's types: video comes as
   `ui::video_frame`s, sound goes to a `platform::audio_output`.
3. **Its resources travel as a script the application's includes**
   (`src/render/res/platform_win_render.rc`). A new resource goes there, with its
   ID in `platform_win_render_res.h`.
4. **Check Debug as well as Release, and Linux as well as Windows,** as for
   libdesktop-system. The tests mark their thread the UI thread, as an
   application's entry point does, because the layer asserts it.
5. **Tests move with the code.** `tests/render_tests.cpp` and
   `tests/render_win_tests.cpp` use Diffractor's assertions, so a test moves
   between Diffractor's suites and these unchanged.

## libdesktop-os

The OS services libdesktop-core and libdesktop-system share, so each has one
implementation to maintain and to port: files, dialogs, escaped INI storage,
the web client, clipboard text, known folders and the audio endpoint.
`pf::` and `platform::` keep their own
APIs and wrap it; neither exposes it, and no application includes
`libdesktop/os.h`.

1. **It asks nothing of an application.** No `s_app_name`, no `df::log`, no
   `app_wnd()`, no locale it expects someone to have set, and no COM but what
   `os::audio_thread` joins. A service takes what it needs - an owner window, a
   user agent, a path - and reports failure as a value; the wrapper adds the
   stack's policy: libdesktop-system's names, log messages and text clean-up,
   pf::'s defaults. This is why it is a library of its own: libdesktop-system's
   translation units carry its application contract, down to the write probe its
   log makes at startup, and every pf:: app would inherit it.
2. **`os.h` includes no OS header**; `src/os/os_win.cpp` and `os_win_audio.cpp`
   are its Windows backend. Text and paths are UTF-8.
3. **A merge is held to both stacks' outcomes.** Before a service moves here, the
   pf:: and platform:: suites record what each implementation does - whole, cut
   short and failing - and the merged service keeps both, or the difference is
   deliberate and stated in the test that checks it.
4. **Tests.** `tests/os_tests.cpp` tests the service itself, under the CTest label
   `libdesktop-os`; `tests/loopback_http.h` serves the web client offline to all
   three suites.
5. **Audio is one engine, and it is Diffractor's.** `platform::audio_output` is
   the endpoint itself, which Diffractor's player drives directly: its clock
   times the video, `reset()` empties the ring, `write_silence()` keeps a clip's
   tail from replaying. pf::'s sounds and streams are libdesktop-core's mixer
   (`src/core/platform_audio.cpp`) playing through it on a thread of its own.
   So the endpoint is held to Diffractor's suite and gate, and the mixer adapts
   to the endpoint, never the reverse. A new system needs the endpoint alone.

Which services remain to merge, and why each waits, is in [docs/todo.md](docs/todo.md).

## Adding to libdesktop-ui

1. It goes in `src/ui/`, in `pf::ui`, reached as `#include "ui/<file>.h"`.
2. It may use `pf::`. It may not name an application, open a file, know a path,
   or speak a protocol — those belong to the app.
3. Anything pure gets a case in `tests/ui_tests.cpp`. Layout and hit testing are
   testable without a window because `pf::measure_context` is an interface.
4. An app opts in with `platform_add_app(<target> UI ...)`.

### Put it at the level that needs it, not the level that found it

Three capabilities here were first written one level too low, and each was only
noticed when a *second* application adopted the code:

- The clipboard shortcuts were added to `composer`, because a prompt box has no
  menu behind it. Neither does a read-only transcript. They belong to `text_view`.
- `at_bottom` and `scroll_to_end` were written in an application's own agent view.
  Every scrolling transcript wants them; they belong to `read_only_doc_view`.
- A blockquote had no style of its own, so an application could not mean anything
  by one. `text_style::md_quote` exists so a chat panel can mark whose turn it is
  without the markdown model knowing what a turn is.

**One consumer is not enough to validate a shared component.** When a second
application needs something the first already had, ask whether the first put it
in the right place before copying the shape.

## Two ways to host a view

Every view here is a `pf::frame_reactor` and asks its `pf::window_frame` for focus,
capture, timers and repaints. There are two ways to give it one:

- **A child window each.** `window_frame::create_child`, then `set_reactor`.
  dde does this, and nothing more is needed.
- **`pf::ui::pane_host`.** The application keeps its single window and one reactor,
  adds a pane per view, and forwards paint, mouse, keyboard and messages to the
  host, which routes them. An application that draws its panes into rectangles
  could not use any of these views without it. No application here uses it
  today; its last two consumers were removed from the workspace.

`window_frame` is an interface, not a window, which is what makes the second
possible; `test_support.h` has implemented one without a window all along.

## Conventions

- Modern C++20 in `snake_case`; `pf::` for everything public.
- Comments explain *why*, in one line. Do not narrate the next line.
- Diagnose failures with `pf::debug_trace` and the real error code rather than
  failing silently.
- `dd.ps1` is duplicated in each repo by design — it has to run before CMake, so
  it cannot come from this package.
