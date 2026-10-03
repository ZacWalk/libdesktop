# AGENTS.md

platform-h is the platform layer shared by every application in this workspace
(`list0`, `ovrwin`, `potato`, `rethinkify-app`, `crypto-app`, `stuntcarracer`,
`equity-app`). It is a library with no application of its own.

It ships **two** static libraries:

| Target | Alias | Source | Contents |
|---|---|---|---|
| `platform-core` | `platform::core`, `platform::platform` | `src/core` | The OS abstraction. Every app links it. |
| `platform-ui` | `platform::ui` | `src/ui` | Reusable presentation: widgets, text views, markdown, the agent chat panel. Opt-in. |

`platform::platform` remains an alias of `platform-core` because seven repositories
pin this package and acquire it four different ways. Do not remove it.

platform-ui operates on a UTF-8 text buffer the application hands it. It never
opens a file, never knows a path, and never names an application.

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

| File | Contents |
|---|---|
| `src/core/platform.h` | The whole `pf::` surface. Declarations, plus inline text/geometry helpers |
| `src/core/platform_common.cpp` | Backend-independent implementation (the embedded-resource registry, `line_splitter`) |
| `src/core/platform_win.cpp` | Win32 backend: `WinMain`, message loop, GDI drawing, WIC, WinINet/WinHTTP, dialogs, clipboard, spell check, child processes |
| `src/core/platform_win_audio.cpp` | XAudio2 backend, isolated from the rest |
| `src/ui/ui.h` | platform-ui umbrella header |
| `src/ui/theme.h` / `.cpp` | The `pf::ui::colors` table, the `theme` metrics, the style palette and the heading fonts |
| `src/ui/text_types.h` | `text_location`, `text_selection`, `text_style`, `text_block`, `highlight_fn` |
| `src/ui/view_host.h` | What a view asks of the application: invalidation, `ensure_visible`, status |
| `src/ui/widgets.h` | `edit_box`, `caret_blinker`, `splitter`, `custom_scrollbar`, `edit_box_widget` |
| `src/ui/table_layout.h` / `.cpp` | Table cell parsing and rendering, shared by the markdown and CSV views |
| `src/ui/syntax.h` / `.cpp` | The syntax highlighters; language choice is the caller's |
| `src/ui/spell.h` / `.cpp` | The process-wide spell checker and its cache |
| `src/ui/text_line.h`, `text_buffer.h` / `.cpp` | The UTF-8 line and the editing model: lines, selection, undo |
| `src/ui/text_input.h` | `pf::ui::input` — filtering and bounding untrusted text before it enters a prompt |
| `src/ui/markdown.h` | `pf::ui::md` — the Markdown model, its parser and the HTML reducer |
| `src/ui/view_base.h` → `view_text.h` → `view_doc.h` | The view hierarchy: scrolling, selection and the clipboard — **including Ctrl+A/C/X/V, which live here rather than in any one view, because a view with no menu behind it gets no copy command** — then the caret, wrap and hit testing |
| `src/ui/view_doc_edit.h`, `view_doc_readonly.h`, `view_markdown.h`, `view_csv.h`, `view_hex.h` | The views an application shows: editable, read-only, rendered Markdown, CSV tables and bytes. `read_only_doc_view` also answers `at_bottom` and `scroll_to_end`, so a transcript that grows can follow its end only when the reader was already there |
| `src/ui/view_composer.h` | `composer` — the prompt box: growing, history, and the input filter in front of it |
| `src/ui/view_list.h` | `list_view` and `list_item` — the panel list: rows, selection, hover, keyboard and copy |
| `src/ui/pane_host.h` | `pane_host` and `hosted_window` — running these views inside one window, for an application that draws its panes into rectangles rather than giving each a child window |
| `src/ui/test_support.h` | Headless fakes — `measure_context`, `draw_context`, `view_host`, `window_frame` |
| `cmake/platform_app.cmake` | `platform_add_app()` — the function apps use to declare themselves |
| `cmake/embed_resources.cmake` | Turns data files into a generated C++ byte-array TU |
| `cmake/app.manifest.in` | The manifest every app gets |
| `tests/platform_tests.cpp` | The platform-core suite (CTest label `platform`) |
| `tests/ui_tests.cpp` | The platform-ui suite (CTest label `platform-ui`) |

## Build and test

This repo uses the vendored dd build system. dd has two modes:

**CLI mode** — the default; each verb runs once and exits:

```pwsh
.\dd.ps1 test                  # build both configs and run the suite
.\dd.ps1 build debug
.\dd.ps1 doctor --json
```

**MCP mode** — `.\dd.ps1 mcp` turns the process into a stdio JSON-RPC server for an
MCP client, adapting typed requests onto CLI mode. It owns stdout for protocol
messages, so it prints no result envelope and rejects `--json`. Register it with
`.\dd.ps1 ide --mcp`.

Project settings live in `dd.psd1`; dependency pins live in
`cmake/dd-dependencies.json` (`dependencies.owner = 'dd'`).

To check nothing downstream broke, from the workspace root:

```pwsh
foreach ($r in 'platform-h','list0','ovrwin','potato','rethinkify-app','crypto-app','stuntcarracer') {
    Push-Location $r; .\dd.ps1 build release; Pop-Location
}
```

`equity-app` also consumes this package, but through its own pinned acquisition
(`EQUITY_PLATFORM_SOURCE_DIR`), which requires a clean tree at the declared pin.
Commit here and move its pin first, then verify it with `.\dd.ps1 verify --yes`.

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
