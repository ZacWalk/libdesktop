# AGENTS.md

platform-h is the platform layer shared by seven applications (`list0`, `ovrwin`,
`potato`, `rethinkify-app`, `crypto-app`, `stuntcarracer`, `equity-app`). It is
a library with no application of its own.

It ships the `platform-core` (`pf::`) and `platform-ui` (`pf::ui`) static
libraries; [README.md](README.md#platform-h) is the canonical overview of their
contents and aliases.

`platform::platform` remains an alias of `platform-core` because seven repositories
pin this package and acquire it four different ways. Do not remove it.

platform-ui's ownership boundary is described in
[README.md](README.md#srcui--the-presentation-layer).

## Non-negotiables

1. **`src/core/platform.h` includes no OS headers.** Not `windows.h`, not
   transitively. The Windows SDK appears only in `src/core/platform_win.cpp` and
   `src/core/platform_win_audio.cpp`.
2. **The API must be expressible on another OS.** Nothing Win32-shaped may leak
   into a signature: no `HWND`, no `HRESULT`, no hundredths-of-a-decibel volume,
   no resource IDs. Prefer neutral units — linear gain, normalised pan, Hz,
   UTF-8 `string_view`. If a concept only makes sense on one platform, it
   belongs in the app.
3. **App-specific knowledge stays out.** No app names, no resource identifiers,
   no game constants. `config_set_app_name` exists precisely so the layer never
   has to know who is calling.
4. **A change here breaks seven repos.** Adding to `window_frame` or
   `frame_reactor` is a breaking change for every app and their test stubs.
   Build all of them before you consider a change done — see below.
   `platform::platform` is an alias every one of them links; it does not move.
5. **Add a test with a capability.** `tests/platform_tests.cpp` and
   `tests/ui_tests.cpp` are console programs; anything checkable without a
   window belongs in one of them.

## Layout

The file layout is canonical in [README.md](README.md#file-layout).

## Build and test

Run the repository gate after changes here:

```pwsh
.\dd.ps1 test
```

The gate builds and tests the `platform` and `platform-ui` CTest labels. The
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

Five apps — rethinkify-app, list0, ovrwin, potato and stuntcarracer — use a
sibling `../platform-h` checkout automatically, whatever its HEAD, so a local
edit here reaches them with no publish step. equity-app and crypto-app use a
local checkout only when told to (`EQUITY_PLATFORM_SOURCE_DIR`,
`CRYPTO_SOURCE_PLATFORM_H`), and only when its HEAD is exactly their pin.

## Adding a capability

1. Declare it in `src/core/platform.h`, in neutral terms (see rule 2).
2. Implement it in the backend.
3. Add a case to `tests/platform_tests.cpp` if it can run without a window.
4. Rebuild every app. A pure virtual added to `window_frame` must be implemented
   three times, all in this repository: `win_impl` in `platform_win.cpp`,
   `hosted_window` in `pane_host.h` and `fake_window_frame` in `test_support.h`.
   One added to `frame_reactor` breaks every application and `pf::ui::view_base`,
   since each implements it.

## Adding to platform-ui

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
- `at_bottom` and `scroll_to_end` were written in rethinkify's own agent view.
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
  rethinkify-app does this, and nothing more is needed.
- **`pf::ui::pane_host`.** The application keeps its single window and one reactor,
  adds a pane per view, and forwards paint, mouse, keyboard and messages to the
  host, which routes them. list0 and equity-app are shaped this way — each draws
  its panes into rectangles — and could not use any of these views without it.

`window_frame` is an interface, not a window, which is what makes the second
possible; `test_support.h` has implemented one without a window all along.

## Conventions

- Modern C++20 in `snake_case`; `pf::` for everything public.
- Comments explain *why*, in one line. Do not narrate the next line.
- Diagnose failures with `pf::debug_trace` and the real error code rather than
  failing silently.
- `dd.ps1` is duplicated in each repo by design — it has to run before CMake, so
  it cannot come from this package.
