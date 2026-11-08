# libdesktop::render

`libdesktop::render` draws. It binds a device to a `platform::window` and hands
whoever paints the window a draw context - Direct3D 11 or the software renderer
on Windows, the software renderer on Linux - with text set by DirectWrite or by
Pango, fonts and the icon font, textures, video frames and geometry that
re-presents without the scene being recorded again. It also draws into a CPU
surface with no window at all. It neither lays out an interface nor takes input:
that is `ui`, built on it. An application that draws for itself, a game say,
links render and implements `platform::window_events` itself.

## Using it

```cmake
target_link_libraries(app PRIVATE libdesktop::render)
```

`libdesktop/render.h` brings in platform and the render headers but
`render/software.h` and `render/win32.h`, which are included where used. On
Windows the build needs `fxc` from the Windows SDK, which compiles the shaders
(configuration fails without it); the compiled shaders and the drop shadow are
embedded in the library. On Linux it needs Pango (`pangoft2`), fontconfig and
FreeType, through pkg-config, and SDL3.

## Headers

| Header | What it holds |
|---|---|
| `render/device.h` | Choosing the renderer (`startup`), the base font size, and `window_device`, which presents what is drawn to a window |
| `render/context.h` | `measure_context` and `draw_context`, textures and samplers, text layouts and highlights, video frames, vertices, colour adjustment, `draw_surface`, `scoped_clip`, and the hooks an application sets |
| `render/text.h` | Font faces and descriptors, metrics, the icon font, styled text rasterised for subtitles |
| `render/software.h` | The CPU rasterizer: canvas, present targets, the text engine seam, `create_surface_context` |
| `render/counters.h` | GPU identity and budgets, and what painting and Direct3D cost this session (`ui_perf`, `gpu_perf`) |
| `render/probes.h` | Glyph fallback, font cache and tiling probes, for tests |
| `render/win32.h` | Semi-public, including `<windows.h>`: Direct3D, DirectWrite and WIC types, the render device and adapter published for a hardware decoder, device-loss classification |

## Drawing on a window

`render::startup(gpu)` chooses the renderer once, before the first device:
Direct3D 11 where it is asked for and works, otherwise software. For a `ui`
application `ui::create_app_frame` does this, asking for the GPU when
`ui::app::wants_gpu()` does and the `gpu_render` crash guard has not turned it
off for the session. `create_window_device(window, options)` binds a
device to a window; the window's `paint(damage)` calls
`device->paint(damage, draw)`, and its size, scale and timer events go to the
device too. A device skips frames nothing could be seen in - minimized,
cloaked, behind a secure desktop - and repaints whole once it can. `redraw()`
presents the last scene again without recording it. Damage is a hint: a
backend may draw more, but the pixels inside the damage never depend on how
much.

`set_base_font_size(px)` sets the text size, in pixels at 96 DPI, everything is
scaled from (`render::default_font_size`, 16, until set); every device follows
and repaints.

When the Direct3D device is lost, recovery waits until the call that found it
has unwound. The application's `system_event` hears
`graphics_device_lost` first, synchronously on the UI thread, to release what it
holds on the GPU; then every window device is rebuilt on the software renderer,
which draws for the rest of the session.

## Contexts

A `measure_context` measures text, gives a face's line height and its baseline
(`text_baseline`, the ascent and any line gap above it) and makes text layouts;
its `scale_factor` and metrics (padding, icon and handle sizes, scroll bar
width) follow the window's scale. A `draw_context` adds what is drawn: fills,
rounded rectangles, polygons and shadows, lines, ellipses, text (plain, with
highlights, or a layout), borders, textures (to a rectangle, a quad, through a
`texture_transform`), panoramas where the backend projects them, vertices, and a
clip (`scoped_clip`). `draw_surface(dc, surface, dst)` uploads a CPU surface for
one draw. Every backend clears to `scene_clear_shade` before a scene, so what a
scene leaves unpainted is the same grey everywhere.

`render::create_surface_context(surface)` draws with the software renderer into
a `core::surface`: one that already has the drawing's size, packed and top-down,
is drawn on in place, so what was there survives; only a fresh allocation starts
blank. The pixels are there once `render()` returns. `create_memory_present_target`
is the rest of the seam a port needs before it has a window: the software
context draws into it and shows no one.

## Text and fonts

Text is named by `font_face`: the built-in faces (`dialog`, `code`, `title`,
`mega`, `icons`, `small_icons`), sized from the base font size, or one made from
a `font_desc` with `render::make_font(desc)`, a process-wide id every backend
resolves. `font_desc::size` is in the context's pixels, like a GDI `lfHeight` -
the character height, or the cell height with `cell_size` - and render never
scales it, so an application scales it by the frame's `scale_factor()`.

`render::metrics(mc, face)` builds on the context: `ascent` is the backend's own
baseline (DirectWrite and Pango measure it exactly), `descent` the rest of the
line, `avg_char_width` the width of `0`. DirectWrite's measured width leaves out
trailing whitespace and Pango's includes it ([todo](todo.md#render)).

The icon font is the application's: `set_icon_font(bytes)` registers a bundled
one, whose bytes must outlive the process, and `set_icon_font_family(name)`
names an installed one - "Segoe Fluent Icons", which Windows 10 has as Segoe
MDL2 Assets. The later call wins. `create_icon_surface(ch)` rasterises a glyph
for the places that need an image. `render_styled_text` sets a block - a
subtitle - of runs in their own weight, slant and colour, wrapped, aligned and
outlined, into a straight-alpha surface, on any thread.

## Textures, video and vertices

A `texture` is updated from a surface, from raw pixels or from a
`video_frame`, and drawn point-sampled, bilinear or bicubic. A video frame is a
picture its decode thread prepared, or a hardware picture with the texture it is
in and the lock its decode device takes around it; render never sees a
decoder's types. On Windows the Direct3D device is made with video support where
the driver allows and published with its adapter (`render/win32.h`), so the
application can decode on it: a picture decoded on the render device is copied
once into the texture the scene draws, and one from another device crosses
through a keyed-mutex texture the two share. A YUV picture drawn opaque is drawn
with blending off. A released plain texture is kept for the next of its size and
format, once nothing will draw it again.

Geometry that moves every frame while the rest of the scene stands still - a
visualizer's bars, a meter - goes in `vertices`: the scene draws it by
reference, so the application updates it and calls `redraw()`. `update_bars`
draws raised bars over a drop shadow, up to 1,024 on Direct3D; `update_rects`
draws flat rectangles, up to 16,384.

### Hooks the application sets

- `decode_embedded_image`: the application's PNG decoder for the drop shadow
  render embeds. Without it nothing decodes and no shadow is drawn.
- `make_surface_scaler`: the scaler a texture converts video frames and planar
  surfaces with (FFmpeg's, in Diffractor); without one a renderer converts what
  it can and refuses the rest.
- `yuv_textures_allowed` and `on_yuv_fault`: the application's latch on planar
  textures, which a driver fault clears for it to keep.

render defines these hooks and the counters itself (`src/render/device.cpp`), so an application
that links render without ui needs nothing more.

## Backends

On Windows (`src/render/win/`): Direct3D 11 (`d3d11.cpp`) chooses the device -
the Microsoft Basic Render Driver counts as software - and holds the shaders,
the texture pool and the video paths; the software renderer presents through GDI
(`software_present.cpp`), as a layered window for a bubble; DirectWrite sets the
text (`font.cpp`), and `text_surfaces.cpp` rasterises icon glyphs and styled
text.

On Linux (`src/render/linux/`) the software renderer presents the tiles that
changed to the SDL window's surface (`window_device.cpp`), which a window asks
for while it is still hidden: SDL may move a window to the GPU on the first
request, which on X11 recreates it, and a menu takes its popup being hidden as
being dismissed. Pango lays text out (`text.cpp`) - shaping with HarfBuzz,
fallback through fontconfig, glyphs from FreeType - by the DirectWrite engine's
rules: six text styles, text centred vertically in its box, and characters that
do not fit trimmed rather than ellipsised. The application's icon font is
registered with fontconfig. Animations are off, since every window is drawn on
the CPU.

## Tests

`libdesktop_render_tests`, in `tests/render/`, under the CTest label
`libdesktop-render`, links render alone. It covers colour adjustment, vertices
presented again after the scene, the software buffer as the client grows,
styled text, text measured and drawn by one set of rules, primitives drawn into
fresh and existing CPU surfaces, uploaded surfaces, custom font descriptors and
the baseline. `tests/render/win/` adds the Windows backends': GPU text snapping,
device-loss classification, the Basic Render Driver, planar textures, an
integrated GPU's memory, texture recycling, glyph fallback, font and glyph
caches by face and size, and highlights through UTF-16 clusters.

## Known gaps

- DirectWrite and Pango disagree on the width of trailing whitespace.
- `metrics()` gives the line height as `x_height`: no backend measures ink.
- Linux draws in software only, without animations.
- The headers rely on the umbrella's includes and order.
