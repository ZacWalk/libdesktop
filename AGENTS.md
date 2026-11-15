# AGENTS.md

libdesktop is the platform layer the workspace's applications share, in four
static libraries, and has no application of its own. The [README](README.md)
maps the libraries and has the build and test commands, `docs/` has a document
for each library, and the workspace's root AGENTS.md has who consumes what, at
which pin. Read them rather than a copy kept here.

## Non-negotiables

1. **The layers point one way: core ← platform ← render ← ui.** A library
   includes and calls only those beneath it. What a lower layer needs from
   above is a hook the application sets, never an include.
2. **OS code lives in `src/<library>/win` and `src/<library>/linux` alone.** No
   public header includes an OS header but the semi-public `platform/win32.h`
   and `render/win32.h`, which are for an application's own Windows code.
   `src/ui` has no OS folder: ui is written over `platform::window` and
   render's devices.
3. **The API is expressible on another OS.** Nothing Win32-shaped leaks into a
   portable signature: no `HWND`, no `HRESULT`, no resource ID, no hundredths of
   a decibel. Prefer neutral units - linear gain, normalised pan, Hz, physical
   pixels, UTF-8 `string_view` - and carry a native handle as `std::any`. A
   concept that makes sense on one platform alone belongs in the application.
4. **It names no application.** The name comes from `core::set_app_name`, and
   every file, folder, key and lock is named from it; the platform's messages
   are English and name nothing. Where libdesktop carries an application's
   values - `icon_index`, `command_group`, `commands` - the type is
   libdesktop's and the values the application's, and library code never names
   a value.
5. **Both backends, both configurations.** What a portable header declares is
   implemented on Windows and on Linux. A Linux stand-in answers "not
   available", and a capability query false, never a success it did not have.
   Build Debug as well as Release - an assertion compiles only in Debug - and
   Linux as well as Windows.
6. **No compatibility aliases or forwarding headers.** A rename moves every
   consumer in the same step: no `using old = new`, no header that only
   includes another, no wrapper kept for an old spelling.
7. **The CRT and the character set are the consumer's.** Nothing sets
   `CMAKE_MSVC_RUNTIME_LIBRARY`; every workspace consumer links the static CRT.
   `UNICODE` stays private to the Windows backends, so a consumer keeps its own
   character set - ImageWalker's historical `_MBCS` releases among them.
8. **A capability comes with a test,** in the suite of the library whose code it
   is: a core test links core alone, a render test render alone.
9. **Every source file starts with the MPL-2.0 notice** the others carry:
   libdesktop is MPL-2.0, and its consumers link it statically.

## Where code goes

- **By what it needs.** Nothing of the OS: core. Asks the OS or owns a native
  window: platform. Draws: render. Lays out, takes input through the element
  tree, or is a control or a view: ui.
- **Put a capability at the level that needs it, not the level that found it,**
  which is usually too specific. The clipboard shortcuts belong to `text_view`,
  since a read-only transcript has no menu behind it either; a blockquote has a
  style of its own, `text_style::md_quote`, so a chat panel can mark whose turn
  it is without the Markdown model knowing what a turn is.
- **One consumer does not validate a shared component;** the second is where its
  design is tested. When a second application needs what the first had, ask
  whether the first put it in the right place before copying its shape, and
  read the adopter's tests before replacing its code.
- **When consumers' needs trade off, Diffractor's experience wins.**
- **One tree.** An element names everything it holds in `visit_children`, and
  one that draws anything of its own draws what it holds with
  `render_children`: a control left out is neither drawn nor reached. Controls
  belong to no frame (`ui::make_*`) and measure with the context their layout
  is given, never through a frame, because they are laid out before one finds
  them. [docs/ui.md](docs/ui.md#the-element-tree) has the model.
- **The Windows service layer asks nothing of an application.**
  `src/platform/win/os.h` names no application, writes no log and needs nothing
  of startup: a service takes what it needs - an owner window, a user agent, a
  path - and returns failure as a value, and the `platform::` wrapper adds the
  policy, the application's name and the log.

## Changing an interface

A change here reaches every consumer. Each workspace consumer builds against a
sibling `../libdesktop` checkout, whatever its HEAD, so a local edit reaches it
with no publish step - which does not validate its published pin. Build them
all before calling a change done (the workspace's root AGENTS.md has the gate),
and publish libdesktop before a consumer's pin moves. A pure virtual added to an
interface breaks everything that implements it:

| Interface | Implemented by |
|---|---|
| `platform::window` | `win_window` (`src/platform/win/window.cpp`), `sdl_window` (`src/platform/linux/window_internal.h`) |
| `render::draw_context`, `render::measure_context` | The Direct3D and software contexts, `ui::offset_draw_context`, the fakes in `ui/test_support.h`, the suites' own measure contexts, and consumers' |
| `ui::frame` | `frame_impl` (`src/ui/frame.cpp`), `ui::null_frame` |
| `platform::app`, `ui::app` | Every application |

## Build and test

`.\dd.ps1 test` is the gate: every suite, Debug and Release. The
[README](README.md#building-and-testing) has the commands for one suite, for
tests by name and for Linux; after changing any library, build and test on
Linux too, with the packages the CI workflow installs.

- Each library's suite is in `tests/<library>/`, under the CTest label
  `libdesktop-<library>`; its Windows-only files go in its `win/` folder, which
  builds on Windows alone.
- Every suite uses one harness, `tests/test.h`: Diffractor's assertions, so a
  test moves between its suites and these unchanged, and a test that asserts
  nothing fails. The headless fakes ui tests use are public, in
  `ui/test_support.h`.
- The platform and ui suites share the CTest resource lock
  `libdesktop-clipboard`, because both change the real clipboard; keep it on any
  suite that does. Their fixtures bound retries for a briefly busy clipboard,
  but never skip a failed round trip or change what the API does on failure, so
  in a session that cannot open the clipboard those tests fail with access
  denied.
- dd is vendored unchanged as `dd.ps1` and `.dd/`, with its file hashes in
  [docs/dd-upstream.json](docs/dd-upstream.json): never edit either. Each
  repository carries its own copy by design, since dd runs before CMake and
  cannot come from this package. Project settings live in `dd.psd1`, dependency
  pins (RapidJSON and SDL3) in `cmake/dd-dependencies.json`. dd's
  [two modes](https://github.com/ZacWalk/dd/blob/main/docs/mcp.md#two-modes) -
  CLI, and MCP for an MCP client - and its
  [commands](https://github.com/ZacWalk/dd/blob/main/docs/cli.md#commands) are
  documented in its own repository.

## Conventions

- C++20 in `snake_case`, indented with tabs; each library's names in its own
  namespace, `core`, `platform`, `render` or `ui`. A test seam is named
  `test_*`.
- `.clang-format` is the layout (tabs, Allman braces, 120 columns): `.\dd.ps1 fmt`
  applies it, given clang-format on PATH (Visual Studio installs one in
  `VC\Tools\Llvm\x64\bin`). Every target builds at `/W4` with no warnings, and
  `.clang-tidy` names the bug-finding and performance checks the code is kept
  clean against; a deliberate exception is a `// NOLINT(check): reason` on its
  line.
- Comments explain *why*, in a line. Do not narrate the next line.
- Diagnose a failure with its real error, through `core::log`, rather than
  failing silently.
- Documentation describes what is, in British spelling: no change log, no plan,
  and no history of what something used to be called. The README stays short;
  depth goes in `docs/`.
