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
| `libdesktop::render` | `render::`, `libdesktop/render.h` | Draw and measure contexts on a window's surface, the software renderer, text, fonts and textures | Windows (Direct3D 11 and software, DirectWrite), Linux (software, Pango) |
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
the libdesktop targets it links are built. The application owns `main`, and
[platform::run](docs/platform.md#the-application) the lifecycle around it:

```cpp
#include "libdesktop/ui.h"

// A frame draws nothing of its own: its host lays out and paints what the window shows.
struct view final : ui::frame_host
{
	core::sizei extent;
	void on_window_layout(render::measure_context&, const core::sizei e, bool) override { extent = e; }
	void on_window_paint(render::draw_context& dc) override
	{
		dc.draw_rect(core::recti(extent), core::color(core::rgba(245, 247, 249)));
	}
};

struct app final : ui::app, std::enable_shared_from_this<app>
{
	std::shared_ptr<view> content = std::make_shared<view>();
	ui::frame_ptr frame;
	bool init(std::string_view) override
	{
		ui::frame_style style;
		style.drawn_caption = false; // keep the system's title bar: this app draws no caption
		style.client_size = {960, 640};
		frame = ui::create_app_frame(shared_from_this(), platform::settings(), content, style);
		return frame != nullptr;
	}
	// A frame must not outlive the renderer, which shuts down after final_exit.
	void final_exit() override { frame.reset(); }
	bool wants_gpu() const override { return false; }
	// Unused keys answer false, or Alt+F4 and the menus stop working.
	bool key_down(char32_t, platform::key_state) override { return false; }
	bool text_input(std::string_view) override { return false; }
	ui::focus_mode focus_mode() const override { return ui::focus_mode::none; }
	void track_menu(const ui::frame_ptr& parent, const core::recti bounds,
	                const std::vector<ui::command_ptr>& commands) override { if (parent) parent->track_menu(bounds, commands); }
};

int main(int argc, char** argv)
{
	core::set_app_name("My App"); // first: the log, the settings and the app-data folder are named from it
	return platform::run(std::make_shared<app>(), argc, argv);
}
```

## Building and testing

On Windows the vendored [dd](https://github.com/ZacWalk/dd) finds Visual Studio
(with its Desktop C++ workload) and the CMake and Ninja that ship with it:

```powershell
.\dd.ps1 build                                   # Debug and Release
.\dd.ps1 test                                    # every suite, in both configurations
.\dd.ps1 test --label libdesktop-ui              # one suite: libdesktop-core, -platform, -render or -ui
.\build\debug\libdesktop_ui_tests.exe "*menu*"   # the tests whose names match
```

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
and [ui](docs/ui.md); the plan in [docs/todo.md](docs/todo.md); conventions for
contributors and coding agents in [AGENTS.md](AGENTS.md).

## License

LGPL-2.1-or-later; see [LICENSE](LICENSE). A program that links libdesktop
statically, as every consumer does, must let its users relink it against a
modified libdesktop (LGPL-2.1 section 6).
