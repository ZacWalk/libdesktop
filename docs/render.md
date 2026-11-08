# libdesktop-render

libdesktop-render is the desktop UI that began as Diffractor's: the `ui::` types
- surfaces, textures, colours and styles - the draw contexts, frames, the element
tree, controls, commands and the `ui::app` they host, with the services its backends implement
(`platform_render.h`). Its controls and menus are drawn by the module itself on
both platforms. On Windows its windows are Win32's, drawn with Direct3D 11 or
the software renderer and DirectWrite text, with WIC, Media Foundation movie
writing and sound through libdesktop-os's WASAPI endpoint. On Linux they are
SDL3's - Wayland or X11 - drawn with the software renderer and Pango text.

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
target_link_libraries(app PRIVATE libdesktop::render libdesktop::render_main)
libdesktop_render_resources("${CMAKE_SOURCE_DIR}/src/app.rc")
```

`libdesktop::render_main` is the entry point. On Linux it is an object of its own,
never a member of the archive, so the linker cannot take some other library's
`main` in its place; on Windows `wWinMain` is the archive's and the target adds
nothing.

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
#include "libdesktop/ui_elements.h"       // the element tree, where used
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
  layer's entry point hosts - `wWinMain` on Windows, and on Linux the `main` in
  `libdesktop::render_main`, which the application links itself (below). It reads
  its own command line and runs its headless modes behind `ui::app`.
- On Windows, a resource script that includes `platform_win_render.rc` inside its
  language block and supplies the two resources the module asks for, under the
  IDs `libdesktop/platform_win_render_res.h` names: `IDI_APP_ICON`, the window
  icon, and `IDF_ICONS`, the icon font, as a `BINARY` resource. On Linux it hands
  the icon font's bytes to `ui::icon_font_data` instead.
- The hooks it sets before the first window: `ui::decode_embedded_image`, which
  decodes the chrome's embedded PNG, the drop shadow; `ui::make_surface_scaler`,
  the scaler a texture converts video frames and planar surfaces with - a
  renderer without one falls back where it can and refuses otherwise; and
  `ui::yuv_textures_allowed` and `ui::on_yuv_fault`, its latch on YUV textures.
- `platform::set_ui_thread()`, first thing in its entry point. The window layer's
  entry point does it. Textures recycle only on that thread, and Debug builds
  assert it throughout.

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
`redraw()`, which presents the scene again without recording it. Both updates
take `ui::vertex_rect`s, a rectangle and its fill: `update_bars` draws each
raised - over a drop shadow, its fill brightening toward its centre - up to
1,024 on the Direct3D backend; `update_rects` draws them flat, up to 16,384.

### Options

| Option | Effect |
|---|---|
| `LIBDESKTOP_RENDER` | Builds the module. Off for a consumer unless it sets it; needs `LIBDESKTOP_SYSTEM` |

### Dependencies

On Windows, `fxc` from the Windows SDK, which compiles the shaders at build
time; the build fails at configure without it. The sources name most of their
import libraries with `#pragma comment(lib)`; the target links the rest. On
Linux, Pango (`pangoft2`), fontconfig and FreeType through pkg-config, and SDL3
at the pin in `cmake/dd-dependencies.json`, built static; SDL3 loads the X11 and
Wayland libraries at run time and needs their development packages to build (the
CI workflow lists them). The CRT is the consumer's choice.

## Resources

A static library carries no resources, so the module's travel as a script the
application's own includes, as MFC's `afxres.rc` is: `src/render/res/` holds
`platform_win_render.rc` - its seven cursors, the drop shadow and its eleven
compiled shaders - with the files it names. The target puts that folder and
the folder the shaders are compiled into on its consumers' include path, which
is what finds them, and `libdesktop_render_resources()` names them all as the
script's dependencies: the resource compiler tracks only the script, so a
changed cursor or shader would otherwise leave the binary with the old copy.

## Frames

A `ui::frame` is a window, or a region of one, that draws what its host - a
`ui::frame_host` - draws. The application's window
(`platform_app::create_app_frame`), a dialog (`create_dlg(host)`), a view's
pane, a popup list, a menu's popup and a bubble (`create_frame(host, style)`)
are all frames, and any of them can make frames of its own. `frame_style` says
what kind of window a frame is: a region of its parent or a popup, drawn with
Direct3D or the software renderer, taking the keyboard on a press (`can_focus`)
or not, ticking, taking drops, or `layered` - a popup with per-pixel alpha that
the pointer passes through, faded in and out where animations run, which is what
a bubble draws in. A `container` frame draws nothing of its own: it has no
surface, only frames placed in it, and the background shows where they leave
it uncovered. An application's window can be one - Diffractor's holds its title
bar, its view and its status bar, each a surface of its own, so no surface is
drawn for what another covers. `ui::null_frame` stands in for a frame that does
not exist, and a test overrides what it records.

Each platform has one class for them all - `frame_impl` on Windows, `frame_node`
on Linux - which differ by kind only where the system does: the application's
window has the drawn caption and the system's messages, a dialog a modal wait,
and a pane takes the keyboard with a press, pans and pinches, and leaves its
menus to the frame it is in.

The bubble (`ui::create_drawn_bubble`, `render_bubble.cpp`) is the same on
every platform: content placed beside what it points at - `ui::place_bubble`
keeps it inside the work area - with its arrow and shadow, in a layered popup
its `ui::bubble_host` makes.

## The element tree

What a frame shows is built from `ui::element`s (`ui_elements.h`), each measured,
laid out, drawn and hit tested. A container, `ui::elements`, lays its children out
as a flexbox does - wrapping, growing, shrinking and aligning them within their
minimums and maximums - in logical units its measure context scales once. Text,
links, icons, dividers, padding and the drawn controls are elements of the
module's; an application adds its own.

A hit test walks the tree and hands the pointer to a `ui::controller`, which the
`ui::element_host` - the frame host a tree is drawn in - keeps while the pointer
stays inside the region the test says the answer holds for: `hit_test_context`
cuts out of that region everything tested first and passed over, so the pointer
reaching it tests again. A controller's own bounds are never narrowed by what
covers it, Escape cancels a press a controller holds so the release that follows
performs nothing, and an element asks its host for work rather than doing it - a
redraw, a layout, a fresh hit test, a tooltip or an animation tick
(`ui::host_invalid`) - which the host may coalesce with its own. `ui::scroller` is the scroll bar such a host draws, with its
section bands and the anchor that keeps content in place across a layout.

The application's command ids reach the tree as `commands`, a type whose values
are the application's, as `icon_index`'s are, and whose value-initialized id is
none. A link bound to one asks its host to invoke it and whether it is checked, so
the host decides what invoking means: a dialog may refuse.

The drawn controls are elements of the tree (`ui::element_kind::control`). A
`ui::control` measures as large as it asks, plus any `room`, and at least its
`min_share` of the width it is measured in, so a row of them - the buttons along
a dialog's foot - is laid out by flex like the rest of the tree. It measures and
arranges itself with the measure context its layout is given (`on_placed(mc)`,
`toolbar::measure_toolbar(mc, cx)`), never one it reaches through its frame: a
control is laid out before any frame has found it. It draws itself where the
tree draws it, in its layer, and is hit and focused through the tree too, so an
element that holds anything names it in `visit_children`. What an element holds
is what it offers everything else to by default - drawing (`render_children`),
the wheel, a press, a hover or a tooltip (`controller_from_location`) and
whether a press is a control's rather than ground that moves a dialog
(`is_control_area`) - each moved by its `children_offset`; an element that draws
anything of its own draws its children too. A hidden element hides what it
holds, and a disabled one (`element::enable(false)`) disables it: a control
takes input only when it and every element above it are enabled, and gives up
the focus when it no longer does. An element that owns a frame of its own - a
map - is a `ui::frame_element`, whose frame the frame it is in places after
each layout.

The application's colours are one palette, `ui::style::colors` (a
`style::theme`), which the platform layer installs at startup with the few
colours the running system theme decides.

## Controls and menus

The controls - `make_edit`, `make_button`, `make_check_button`, `make_slider`,
`make_toolbar` and `make_date_time_control` - are drawn by the module, the same
on both platforms (`render_controls.h`), and belong to no frame: whatever tree
holds one is where it is shown. Each kind - `button`, `edit`, `trackbar`,
`toolbar` and `date_time_control` - has one implementation, private to the
module, so what an owner asks of a control is answered directly; `ui::control`'s
virtual methods are the behaviour the kinds differ in, and what a frame needs to
know of a kind is plain data (`control_traits`). A frame is not a control.

Each frame has a `ui::input_manager` - which control has the keyboard, which the
pointer is over and which holds it - and serves it through `ui::control_host`:
fonts, invalidation, the keyboard, pointer capture, popups and a caret timer. It
owns no controls. It finds them by walking the elements the frame's host offers
(`frame_host::visit_elements`), as it needs them, so Tab follows the tree, a
control under a hidden element is passed over, and one taken out of the tree is
no longer reached; each walk records where a control sits in its frame
(`origin`) and whether everything above it is enabled. The frame feeds the
manager its input before the frame host sees it. There are no child windows for
controls: one window holds the keyboard, and the manager says which control has
it, notifying `frame_host::focus_changed` as a native control's focus did; a
pane, a frame of its own, reports through `pane_focus_changed`. Because a
control and its frame share the window, the keyboard can move between them
without Windows or SDL seeing a change: the frame focusing itself
(`frame::focus`, or a press on the ground of a pane that takes the keyboard)
takes it from the control, and a focused control that is disabled or taken out
of the tree gives it back, each told to the frame host as the frame gaining the
keyboard with no control holding it (`control_host::keyboard_to_frame`). The
window brings its controls up to date before each paint reads what changed, so a
control enabled or disabled since is drawn with it. A dialog keeps where the
keyboard was with `ui::save_focus()` and gives it back with
`ui::restore_focus()`.

They do what Diffractor uses, not all that a native control did. An edit holds
UTF-8 text with `\n` line breaks on both platforms, and `select` takes byte
offsets; it reports typing, not text the application sets. It shows its
selection while it has the keyboard, underlines the words the application's
`spelling_dictionary` rejects and offers its suggestions, and drops down
completions or folders. Buttons read their `&` mnemonics, which a dialog's
keyboard presses: an `&` marks the letter after it only at the start of a word,
so `R&B` keeps its ampersand, and `&&` is one. Radio buttons check one per group.
Sliders report while dragging and once released. A toolbar shows a command's
menu when its button is pressed, and its maximize button answers as the frame's
caption button, so Windows 11 offers snap layouts over it. While a control that
takes typing has the keyboard, the application sees only Escape, Tab, Return
and the arrows up and down.

Menus are drawn as well (`ui::track_drawn_menu`), in popups the window layer
makes (`ui::menu_host`), with the keyboard, submenus and the work area's limits
handled by the menu. A menu is a column of rows, each measured, laid out, drawn
and hit by itself: a command, with its check, accelerator text and submenu; a
separator; a row of buttons (`command::buttons`), as cut, copy and paste are
along the top of a context menu - an element container that flex lays out, each
button's name under its icon in an equal share of the menu, with a line between
neighbours, as a command bar draws them - which the keyboard moves along with
Left and Right; and an element of the tree (`command::content`, a `ui::element`
as a bubble shows), which is shown, not chosen. A dimmed command says nothing of
why: its `disabled_reason` is for a toolbar's bubble. Dialogs stay windows of
their own.

## The Linux window layer

`src/render/platform_linux_window.cpp` is the counterpart of
`platform_win_ui.cpp`, on SDL3. Each frame is an SDL window, or a popup on the
one it opens from; the software renderer draws the scene, and presents the tiles
that changed to the window's surface. A window asks for that surface while it is
still hidden: SDL can answer the first request by moving the window to the GPU,
which on X11 recreates it, hiding it and showing it again, and a menu takes its
popup being hidden as being dismissed. `platform_linux_text.cpp` sets text with
Pango - layouts, glyph fallback, the icon font, styled subtitles - and
`platform_linux_main.cpp` is the entry point, which also gives the clipboard to
SDL. `platform_linux_ui.cpp` holds the palette's system colours, the key table
and UI-thread identity. Writing a movie and opening an audio endpoint answer
that nothing is available, so an application leaves the commands that need them
out rather than dimming them.

## Tests

`tests/render_tests.cpp` is a console program, run on both platforms under the
CTest label `libdesktop-render`. Its portable tests - alpha, the SIMD blends,
resampling, animation and colour adjustment - came from Diffractor's
`test_render.cpp`. `tests/render_controls_tests.cpp` drives the drawn controls,
menus and bubble through a host of its own, on both platforms: editing, focus,
keys, spelling, buttons, sliders, toolbars, dates, menus and their rows, bubble
placement and painting, and controls laid out as elements.
`tests/render_elements_tests.cpp` covers the element tree: flex layout, the
scroller's track, thumb and anchors, and the controller an element host keeps -
reused, escaped and bounded by its hit test. They began as Diffractor's, which
keeps its copies.
`tests/render_win_tests.cpp` adds the Windows backend's -
windows and their placement, device loss and adapters, textures and their pool,
video frames through their interface and copied from a decoder on the render
device, vertices presented again after the scene
was recorded, fonts and text, the
caption button, the clipboard's formats and the WASAPI mix format - from
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
- The drawn controls expose nothing to screen readers, which the native ones
  did. That waits until someone asks for it.
- A mnemonic is pressed by its key code, so only a marked ASCII letter or digit
  answers; a Cyrillic one is shown but has no key.
- The Linux window layer has run under SDL's offscreen driver; menus and
  completion popups have not yet been tried on a desktop Wayland compositor,
  which places popups itself and may let a tooltip popup take no input.
- The headers rely on the umbrella's includes and order.
- The window classes are named `LIBDESKTOP_*` whichever application registers
  them.
