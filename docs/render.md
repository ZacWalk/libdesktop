# libdesktop::render

`libdesktop::render` draws. It binds a device to a `platform::window` and hands
whoever paints the window a draw context - Direct3D 11 or the software renderer
on Windows, the software renderer on Linux - and it also draws into a CPU
surface with no window at all. It neither lays out an interface nor takes
input: that is `ui`, built on it. An application that draws for itself, a game
say, links render and implements `platform::window_events` itself.

## Using it

Link `libdesktop::render` and include `libdesktop/render.h`: platform and every
render header but `render/software.h` and `render/win32.h`, which are included
where used. On Windows the build needs `fxc` from the Windows SDK, which
compiles the shaders, and configuration fails without it; the compiled shaders
are embedded in the library. On Linux it needs Pango
(`pangoft2`), fontconfig and FreeType, through pkg-config, and SDL3.

## Headers

Each header's comments are its reference; this is where to look.

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
application `ui::create_app_frame` calls it, asking for the GPU when
`ui::app::wants_gpu()` does and the `gpu_render` crash guard has not turned it
off. `create_window_device(window, options)` binds a device to a window, whose
`paint(damage)` calls `device->paint(damage, draw)` and whose size, scale and
timer events go to the device too. Damage is a hint a backend may exceed, so a
scene draws the same pixels whatever the damage. `redraw(changed)` presents the
last scene again without recording it, when only its textures or vertices have
changed: the software renderer replays just the tiles over `changed`, passing
over every command whose bounds miss the tile, and Direct3D, which redraws a
whole scene cheaply, draws it all.

`set_base_font_size(px)` sets the text size everything is scaled from, in
pixels at 96 DPI; it is `render::default_font_size`, 16, until set.

When the Direct3D device is lost, recovery waits until the call that found it
has unwound. The application's `system_event` hears `graphics_device_lost`
first, synchronously on the UI thread, to release what it holds on the GPU;
then every window device is rebuilt on the software renderer, which draws for
the rest of the session.

## Contexts

A `measure_context` measures text, gives a face's line height and baseline and
makes text layouts; its `scale_factor` and metrics - padding, icon and handle
sizes, scroll bar width - follow the window's scale, and `scale_metrics(scale)`
sets them all for one. A `draw_context` adds what
is drawn: fills, rounded rectangles, polygons, lines, ellipses,
text (plain, with highlights, or a layout), borders, textures (to a rectangle,
a quad, through a `texture_transform`), panoramas where the backend projects
them, vertices, and a clip (`scoped_clip`). `draw_surface(dc, surface, dst)`
uploads a CPU surface for one draw. A picture with a look of its own - a
bubble's shadow, body and arrow, an application's chart - is rasterised by a
`software_canvas` over a surface and drawn that way, so it is the same on every
backend; `fill_shadow` is the canvas's soft shadow.

## Polygons

`draw_polygon(outline, style)` draws a polygon - convex or concave, but not
crossing itself, and closed from its last point to its first - as its
`polygon_style` says. Its `paint` is one colour, or a gradient between two:
linear, along the line from `start` to `end`, or radial, out from `start` to
`radius`. A gradient holds its end colours beyond its ends, one with no length
is its end colour throughout, and colours mix premultiplied, so one fading to
transparent keeps its hue. The border, `border_width` wide, is centred on the
outline and drawn over the fill, mitred at its corners and bevelled where a
mitre would reach past four half widths. Edges are smooth, anti-aliased, or
crisp: a crisp polygon fills exactly the pixels whose centres are inside it, so
polygons that share an edge meet with neither gap nor overlap, which is what a
mesh of them - a 3D scene drawn flat - needs. An outline that encloses nothing
draws nothing.

Direct3D cuts a polygon into triangles - ear clipping, around every corner that
turns in - adds a pixel-wide fringe at a smooth edge, and draws a gradient with
`polygon_ps.hlsl`, which mixes as `paint::at` does; solid polygons batch with
the rest of the scene's solid fills. The software renderer scans the outline
(`software_canvas::fill_contours`), taking sixteen lines a row for a smooth
edge, each exactly across, and the centre line for a crisp one. Both draw the
border from the same mitred band (`src/render/polygon.h`), and a crisp polygon
is the same pixels on both.

`render::create_surface_context(surface)` draws with the software renderer into
a `core::surface`: one that already has the drawing's size, packed and top-down,
is drawn on in place, so what was there survives; only a fresh allocation starts
blank. The pixels are there once `render()` returns. Its text engine is its own,
so whichever thread made the context may draw on it, text included - a worker
rendering a video frame by frame as well as the UI thread - while textures and
text layouts stay the UI thread's.
`create_memory_present_target` is the rest of the seam a port needs before it
has a window.

## Text and fonts

Text is named by `font_face`: the built-in faces (`dialog`, `code`, `title`,
`mega`, `icons`, `small_icons`), sized from the base font size, or one made from
a `font_desc` with `render::make_font(desc)`. `font_desc::size` is in the
context's pixels, like a GDI `lfHeight` - the character height, or the cell
height with `cell_size` - and render never scales it, so an application scales
it by the frame's `scale_factor()`.

`render::metrics(mc, face)` builds on the context: `ascent` is the backend's own
baseline (DirectWrite and Pango measure it exactly), `descent` the rest of the
line, `avg_char_width` the width of `0`.

The icon font is the application's: `set_icon_font(bytes)` registers a bundled
one and `set_icon_font_family(name)` names an installed one; the later call
wins. `render_styled_text` sets a block - a subtitle - of runs in their own
weight, slant and colour into a straight-alpha surface, on any thread.

## Textures, video and vertices

A `texture` is updated from a surface, from raw pixels or from a `video_frame`,
and drawn point-sampled, bilinear or bicubic. A video frame is a picture its
decode thread prepared, or a hardware picture with the texture it is in and the
lock its decode device takes around it; render never sees a decoder's types. On
Windows the Direct3D device is made with video support where the driver allows
and published with its adapter (`render/win32.h`), so the application can
decode on it: a picture decoded on the render device is copied once into the
texture the scene draws, and one from another device crosses through a
keyed-mutex texture the two share. A released plain texture is kept for the
next of its size and format, once nothing will draw it again.

A texture drawn at less than half its size is averaged over each pixel's
footprint - a grid of bilinear taps over the texels the pixel covers - rather
than read at its centre, which skips texels and shimmers as the picture moves;
a bicubic draw is averaged likewise past two texels a side, where Catmull-Rom's
taps no longer reach. A planar picture says where its chroma lies
(`chroma_siting`, on the surface and on the `video_picture`): with the left luma
column, as MPEG-2, H.264 and HEVC site 4:2:0, centred as JPEG does, or as the
stream otherwise states. Direct3D reads the chroma there, and a renderer that
converts with the application's scaler hands the scaler the siting. Drawn into
an eight-bit target, a colour that falls between its levels - blended between
texels, graded, converted from YUV or read at sixteen bits - is dithered by less
than a level so its gradients do not band, and one the target holds exactly, an
eight-bit texel at one to one, is drawn as it is. The software renderer does not
dither.

A texture drawn through a `texture_transform` is colour adjusted by one
function, `grade_color`, which the Direct3D shader mirrors line for line and the
software renderer and `color_adjust::apply` call, at a byte or sixteen bits a
channel. White balance comes first, as gains on red and blue that keep a grey's
luma; then the tone curve, on BT.709 luma, with the colour scaled alongside so a
tone change keeps its saturation; then saturation, and vibrance, which lifts the
least saturated colours most and spares skin; and a colour pushed past the cube
is desaturated towards its luma rather than clipped. The curve is monotone and
stays inside its range whatever the controls: darks, midtones and lights move a
Fritsch-Carlson spline through five points, contrast is an S-curve about mid
grey whose slope there is 2 to the power of the contrast, and brightness a bias
that moves the middle and keeps black and white.

Geometry that moves every frame while the rest of the scene stands still - a
visualizer's bars, a meter, a shape made of polygons - goes in `vertices`: the
scene draws it by reference, so the application updates it (`update_bars`,
`update_rects` or `update_polygons`) and calls `redraw(changed)`. Its points
are the client pixels of the update, so an application updates it again after
the window is resized. Bars are set off the wall by their shadows. The software
renderer, and so Linux, draws the
classic bars, raised over a soft shadow and brightening toward their centres.
Direct3D draws each as a rounded block of coloured glass (`audio_vis_ps.hlsl`), a
little narrower than its rect so neighbours stand apart: its face flat and glossy,
with a sheen down its left side and deepening toward its ends, its edges bevelled
round to catch the light from the upper left and turn from it on the lower right,
and casting a soft shadow down and to the right that is tinted with its colour.
One shader draws every bar and its shadow in a single draw, the shadows listed
first so each bar stands over its neighbours'. The glass is the one look the
backends deliberately do not share.

## Hooks the application sets

render defines these in `src/render/device.cpp`, so an application that links
render without ui needs nothing more; `render/context.h` has their contracts.

- `make_surface_scaler`: the scaler a texture converts video frames and planar
  surfaces with (FFmpeg's, in Diffractor); without one a renderer converts what
  it can and refuses the rest.
- `yuv_textures_allowed` and `on_yuv_fault`: the application's latch on planar
  textures, which a driver fault clears for it to keep.

## Backends

On Windows (`src/render/win/`): Direct3D 11 (`d3d11.cpp`) chooses the device -
the Microsoft Basic Render Driver counts as software - and holds the shaders,
the texture pool and the video paths; the software renderer presents through GDI
(`software_present.cpp`), as a layered window for a bubble; DirectWrite sets the
text (`font.cpp`), and `text_surfaces.cpp` rasterises icon glyphs and styled
text.

On Linux (`src/render/linux/`) the software renderer presents the tiles that
changed to the SDL window's surface (`window_device.cpp`). Pango lays text out
(`text.cpp`) - shaping with HarfBuzz, fallback through fontconfig, glyphs from
FreeType - by the DirectWrite engine's rules: six text styles, text centred
vertically in its box, and characters that do not fit trimmed rather than
ellipsised. The application's icon font is registered with fontconfig.

## Tests

`libdesktop_render_tests`, in `tests/render/`, under the CTest label
`libdesktop-render`, links render alone. It covers the software renderer, its
shadows, text measured and drawn by one set of rules, colour adjustment,
polygons - fills, gradients, borders, smooth and crisp edges, tiling - vertices,
re-presenting part of a scene, and fonts;
`tests/render/win/` adds the Windows backends': GPU text, device-loss
classification, the Basic Render Driver, planar textures, texture recycling,
glyph fallback, the font caches, and Direct3D's polygons held against the
software renderer's (`render::test_draw_d3d11` draws on a WARP device and reads
the pixels back).

## Known gaps

- DirectWrite's measured width leaves out trailing whitespace and Pango's
  includes it, so text that ends in a space measures differently on each
  platform.
- `metrics()` gives the line height as `x_height`: no backend measures ink.
- Linux draws in software only, without animations.
