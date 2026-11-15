# libdesktop

[![Build](https://github.com/ZacWalk/libdesktop/actions/workflows/build.yml/badge.svg)](https://github.com/ZacWalk/libdesktop/actions/workflows/build.yml)

A C++20 platform layer for desktop applications on Windows and Linux, in four
static libraries, each built on the one before: an application links the
highest one it uses and gets the ones beneath it.
[Diffractor](https://github.com/ZacWalk/diffractor) is its primary consumer.

| CMake target | Namespace, header | What it holds | Platforms |
|---|---|---|---|
| `libdesktop::core` | `core::`, `libdesktop/core.h` | Strings and UTF-8, paths, dates, geometry, colours and pixel surfaces, containers and small utilities | Standard C++, everywhere |
| `libdesktop::platform` | `platform::`, `libdesktop/platform.h` | The application's lifecycle and event loop, native windows and input, files and settings, the clipboard, dialogs, the shell, child processes, tasks, the web client, audio, images and embedded resources | Windows (Win32), Linux (SDL3) |
| `libdesktop::render` | `render::`, `libdesktop/render.h` | Draw and measure contexts on a window's surface - polygons with borders and gradients, text, textures - re-presenting only the region that changed; the software renderer and fonts | Windows (Direct3D 11 and software, DirectWrite), Linux (software, Pango) |
| `libdesktop::ui` | `ui::`, `libdesktop/ui.h` | The element tree and flex layout, the frame every window, dialog, pane, menu and bubble is, drawn controls, the menu bar, commands, the text model, syntax highlighters and text views | Wherever the layers below build |

## Using it

Fetch libdesktop at a pinned commit, or build against a sibling checkout, and
declare the application with `libdesktop_add_app()`, which generates its
manifest, icon and version resources, registers its `EMBED` files and links
`libdesktop::platform`; the application links `render` or `ui` itself.

```cmake
# A sibling checkout wins, so a local libdesktop edit reaches the app with no commit.
if(NOT DEFINED FETCHCONTENT_SOURCE_DIR_LIBDESKTOP
   AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/../libdesktop/CMakeLists.txt")
    set(FETCHCONTENT_SOURCE_DIR_LIBDESKTOP "${CMAKE_CURRENT_SOURCE_DIR}/../libdesktop" CACHE PATH "")
endif()
include(FetchContent)
FetchContent_Declare(libdesktop
    GIT_REPOSITORY https://github.com/ZacWalk/libdesktop.git
    GIT_TAG        <full commit SHA>)  # the pin records what the app was verified against
FetchContent_MakeAvailable(libdesktop)
libdesktop_add_app(myapp SOURCES src/main.cpp ICON src/res/myapp.ico DESCRIPTION "My application")
target_link_libraries(myapp PRIVATE libdesktop::ui)
```

The CRT is the application's choice: set `CMAKE_MSVC_RUNTIME_LIBRARY` before
`FetchContent_MakeAvailable`, so libdesktop is built against the same one; only
the libdesktop targets it links are built. Include a library's umbrella header
first, or make it the precompiled header: most headers beneath it rely on its
includes and their order. The application owns `main`, and
[platform::run](docs/platform.md#the-application) the lifecycle around it. The
simplest application is a [shell](docs/ui.md#the-shell): one window, a menu bar
over its content, and - for a game or an emulator - a fixed-step loop:

```cpp
#include "libdesktop/ui.h"
#include "libdesktop/ui/shell.h"

struct app final : ui::shell
{
	app() : shell({.title = "My App", .client_size = {960, 640}}) {}

	ui::element_ptr make_content() override
	{
		return std::make_shared<ui::canvas_element>([](render::draw_context& dc, const core::recti bounds)
		{
			const core::pointd corners[] = {{bounds.left + 40.0, bounds.bottom - 40.0},
			                                {bounds.left + bounds.width() / 2.0, bounds.top + 40.0},
			                                {bounds.right - 40.0, bounds.bottom - 40.0}};
			dc.draw_polygon(corners, {.fill = core::color(core::rgba(80, 140, 220)), .border = core::color(core::rgba(20, 40, 80)),
			                          .border_width = 3.0});
		});
	}
};

int main(int argc, char** argv)
{
	core::set_app_name("My App"); // first: the log, the settings and the app-data folder are named from it
	return platform::run(std::make_shared<app>(), argc, argv);
}
```

An application that needs more - several windows, a drawn caption - implements
`ui::app` and a `ui::frame_host` itself ([docs/ui.md](docs/ui.md#the-application)).

## Building and testing

On Windows the vendored [dd](https://github.com/ZacWalk/dd) finds Visual Studio
(with its Desktop C++ workload) and the CMake and Ninja that ship with it:

```powershell
.\dd.ps1 build                                   # Debug and Release
.\dd.ps1 test                                    # every suite, in both configurations
.\dd.ps1 test --label headless                   # the same suites, no visible UI, full console output
.\dd.ps1 test --label libdesktop-ui              # one suite: libdesktop-core, -platform, -render or -ui
.\build\debug\libdesktop_ui_tests.exe "*menu*"   # the tests whose names match
```

The `headless` label includes the real offscreen frame checks, not a GUI smoke
launch. Test presets print each suite's own case/check counts even on success;
CTest's entry count is not the number of assertions. After building, rerun with
`ctest --preset debug -L headless -V --no-tests=error` (or `release`).

All four libraries and suites build on Linux too, less the tests that need
Windows, with GCC 13 or later, Ninja, Pango, and the X11 and Wayland development
packages SDL3 builds against, which the [CI workflow](.github/workflows/build.yml)
lists:

```bash
cmake -S . -B build/linux -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build/linux
ctest --test-dir build/linux --output-on-failure
```

## Documentation

Each library's headers, contracts, backends, tests and known gaps are in
[core](docs/core.md), [platform](docs/platform.md), [render](docs/render.md)
and [ui](docs/ui.md); the rules for contributors and coding agents in
[AGENTS.md](AGENTS.md).

## License

MPL-2.0; see [LICENSE](LICENSE). A program that links libdesktop statically, as
every consumer does, may be under any licence. Changes to libdesktop's own files
stay under the MPL-2.0, and anyone distributing a program that includes
libdesktop must tell its recipients where to get libdesktop's source code. The
vendored dd build driver (`dd.ps1`, `.dd/`) is MIT.
