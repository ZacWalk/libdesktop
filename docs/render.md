# libdesktop-render

libdesktop-render is the desktop UI that began as Diffractor's: the `ui::` types
- surfaces, textures, colours and styles - the draw contexts, frames, controls,
commands and the `ui::app` they host, with the services its backends implement
(`platform_render.h`): the Direct3D 11 and software renderers, DirectWrite text,
WIC, native controls, Media Foundation movie writing and sound, through
libdesktop-os's WASAPI endpoint. Windows is
its backend; on Linux it builds with stand-ins, which run a headless core.

It is written in libdesktop-system and sits beside libdesktop-core (`pf::`) and
libdesktop-ui (`pf::ui::`). Its `ui::` window model and `pf::`'s reactor are
separate on purpose, and merge only when one application needs both.

## Using it

The module is opt-in, and needs libdesktop-system. A consumer turns both on
before it brings libdesktop in, links `libdesktop::render`, and on Windows names
the resource script that includes the module's (below):

```cmake
set(LIBDESKTOP_SYSTEM ON)
set(LIBDESKTOP_RENDER ON)
FetchContent_MakeAvailable(libdesktop)
target_link_libraries(app PRIVATE libdesktop::render)
libdesktop_render_resources("${CMAKE_SOURCE_DIR}/src/app.rc")
```

Built directly, libdesktop builds it by default.

Code includes the umbrella header first: it brings in `libdesktop/system.h`,
then `platform_render.h` and `ui.h`. An application's window layer on Windows
also includes the backend's semi-public headers, which include `<windows.h>`:
`platform_win_render.h` (factories, textures, adapters, device loss),
`platform_win_visual.h`, `platform_win_ui_internal.h` and
`platform_win_render_res.h`.

```cpp
#include "libdesktop/render.h"
#include "libdesktop/render_software.h"   // the software rasterizer, where used
```

### What the application supplies

Everything libdesktop-system asks for - `s_app_name`, a UTF-8 C locale, COM on
the threads that use the shell or WIC - and:

- `const ui::chrome_icon_set ui::chrome_icons`: the glyphs the chrome draws - a
  menu's check mark, check boxes, radio buttons and the caption buttons - as
  code points of the icon font, which `icon_index` names. `icon_index` is opaque
  here: the type is the module's, its values the application's.
- `enum class command_group`, the groups its commands fall in, which a
  `ui::command` carries for it. `ui.h` declares the type and the application
  defines its values, with its "none" first, since a command's group is
  value-initialized.
- `ui::app_ptr create_app(const ui::plat_app_ptr&)`: the application the window
  layer's entry point, `wWinMain`, hosts. It reads its own command line and runs
  its headless modes behind `ui::app`.
- On Windows, a resource script that includes `platform_win_render.rc` inside its
  language block and supplies the two resources the module asks for, under the
  IDs `libdesktop/platform_win_render_res.h` names: `IDI_APP_ICON`, the window
  icon, and `IDF_ICONS`, the icon font, as a `BINARY` resource.
- The hooks it sets before the first window: `ui::decode_embedded_image`, which
  decodes the chrome's embedded PNG, the drop shadow; `ui::make_surface_scaler`,
  the scaler a texture converts video frames and planar surfaces with - a
  renderer without one falls back where it can and refuses otherwise; and
  `ui::yuv_textures_allowed` and `ui::on_yuv_fault`, its latch on YUV textures.
- `platform::set_ui_thread()`, first thing in its entry point. `wWinMain` does
  it on Windows. Textures recycle only on that thread, and Debug builds assert it
  throughout.

The application hands the renderers video as `ui::video_frame`s - a picture its
decode thread prepared, or a hardware texture with the lock its decode device
takes around it - and plays sound through `platform::audio_output`, in the
endpoint's own format. The module never sees a decoder's types. It counts what
painting and Direct3D cost in `df::ui_perf` and `df::gpu_perf`, which the
application may summarise.

On Windows the Direct3D device is made with video support where the driver
allows, and published with its adapter (`publish_render_device`,
`render_device`, `render_adapter` in `platform_win_render.h`) so the
application can decode on it. A hardware picture whose texture belongs to the
render device is copied once, straight from the decoder's surface into the
texture the scene draws; one from another device crosses through a keyed-mutex
texture shared between the two. A YUV picture drawn at full opacity is drawn
with blending off, since it covers what is beneath it. A window's frame tick is a
high-resolution timer, falling back to a standard one where Windows has none.

Text to be drawn as a picture - a subtitle - is set off the UI thread with
`platform::render_styled_text`: runs in their own weight, slant and colour, laid
out together at a size in pixels, wrapped, aligned and outlined, and returned as
a straight-alpha surface the application uploads like any other.

Geometry that moves every frame while the rest of the scene stands still - a
visualizer's bars, a trace, a meter - goes in a `ui::vertices`. A scene draws
one by reference, so the application updates it and calls its frame's
`redraw()`, which presents the scene again without recording it. `update` takes
the audio visualizer's 64 bars, each over a drop shadow; `update_rects` takes
flat rectangles, up to 16,384 of them on the Direct3D backend.

### Options

| Option | Effect |
|---|---|
| `LIBDESKTOP_RENDER` | Builds the module. Off for a consumer unless it sets it; needs `LIBDESKTOP_SYSTEM` |

### Dependencies

On Windows, `fxc` from the Windows SDK, which compiles the shaders at build
time; the build fails at configure without it. The sources name most of their
import libraries with `#pragma comment(lib)`; the target links the rest. On
Linux it needs nothing beyond libdesktop-system. The CRT is the consumer's
choice.

## Resources

A static library carries no resources, so the module's travel as a script the
application's own includes, as MFC's `afxres.rc` is: `src/render/res/` holds
`platform_win_render.rc` - its seven cursors, the drop shadow and its eleven
compiled shaders - with the files it names. The target puts that folder and
the folder the shaders are compiled into on its consumers' include path, which
is what finds them, and `libdesktop_render_resources()` names them all as the
script's dependencies: the resource compiler tracks only the script, so a
changed cursor or shader would otherwise leave the binary with the old copy.

## The Linux stand-ins

`src/render/platform_linux_ui.cpp` supplies what a portable core needs to run
headless: the palette's system colours, the key table, UI-thread identity, a
conventional desktop, focus and input state. It has no window, renderer or
desktop integration. Rasterising an icon glyph or styled text, writing a movie
and opening an audio endpoint answer that nothing is available, so an
application leaves the commands that need them out rather than dimming them.

## Tests

`tests/render_tests.cpp` is a console program, run on both platforms under the
CTest label `libdesktop-render`. Its portable tests - alpha, the SIMD blends,
resampling, animation and colour adjustment - came from Diffractor's
`test_render.cpp`, and `tests/render_win_tests.cpp` adds the Windows backend's -
windows and their placement, device loss and adapters, textures and their pool,
video frames through their interface and copied from a decoder on the render
device, vertices presented again after the scene
was recorded, fonts and text, the native controls, the
caption buttons, the clipboard's formats and the WASAPI mix format - from
`test_platform_win.cpp`, and styled text rasterised for subtitles, its own. They use Diffractor's assertions (`tests/system_test.h`),
so a test moves between the suites unchanged. The program supplies what an
application does (above): its chrome glyphs are empty, since it loads no icon
font, and `create_app` makes nothing. "Should decode embedded chrome images"
installs a decoder of its own; Diffractor's copy checks that the application
installs one. Diffractor keeps its own copies, which test the module through the
application, with its tests that need FFmpeg or its own decoders.

## Known gaps

- Linux has no audio endpoint, so `platform::create_audio_output` answers that
  none is available. libdesktop-os is where one goes, and it would serve
  libdesktop-core's sounds and streams as well.
- The headers rely on the umbrella's includes and order.
- The window classes are named `LIBDESKTOP_*` whichever application registers
  them.
- Linux has no window layer, renderer or desktop integration.
