# libdesktop::ui

`libdesktop::ui` is the interface an application is built from, the same on
every platform. It is written over `platform::window` and render's devices
alone, so `src/ui` has no operating-system folder; it builds wherever the
layers below do.

## Using it

Link `libdesktop::ui` and include `libdesktop/ui.h`: render and the vocabulary,
`ui/fwd.h`, `ui/style.h`, `ui/command.h`, `ui/frame.h` and `ui/app.h`. The rest
is included where used: `ui/elements.h`, `ui/controls.h`, `ui/menu_bar.h`,
`ui/shell.h`, `ui/syntax.h`, `ui/test_support.h`, and `ui/views.h` for the text
models and views.

## Headers

Each header's comments are its reference; this is where to look.

| Header | What it holds |
|---|---|
| `ui/fwd.h` | The opaque enums `icon_index`, `command_group` and `commands`, forward declarations, pointer aliases |
| `ui/style.h` | The palette (`style::colors`) and the animation helpers |
| `ui/command.h` | `command`, the styles of edits and toolbars, the spelling dictionary an edit is given, the chrome's glyphs |
| `ui/frame.h` | `frame_host`, `frame_style`, `frame`, `create_app_frame`, the bubble, `null_frame`, saving and restoring the focus |
| `ui/app.h` | `ui::app`, the UI's part of the application |
| `ui/elements.h` | Elements, flex layout, hit testing and controllers, `element_host`, the scroller, frame elements, and text, link, icon, divider and padding elements |
| `ui/table_element.h` | Element tables: ordinary tree-owned cells, shared column sizing, optional headings that sort the rows and resize the columns, and optional scrolling |
| `ui/controls.h` | `control` and its kinds - buttons, edits, sliders, toolbars, date fields - the input manager, the drawn menu and bubble, and the window-free models they edit |
| `ui/menu_bar.h` | `make_menu_bar` and `dispatch_accelerator` |
| `ui/shell.h` | `shell`, the simple application, its `shell_options`, `key_input` and `step_clock`; `canvas_element`, `surface_element` and `render_to_surface` |
| `ui/text_types.h`, `ui/syntax.h` | Text locations, selections and styles, and the syntax highlighters: standard C++ alone |
| `ui/text/*.h` | `text_buffer` and `text_line`, `view_host`, the Markdown model, table layout, spelling and the clipboard seam |
| `ui/views/*.h`, `ui/views.h` | The text, document, list, CSV, hex and Markdown views, the flex document panel, the scroll bar, the splitter and `view_theme` |
| `ui/test_support.h` | Headless fakes, in `ui::test`: a fixed-grid measure context, a draw context that records - and one whose textures record what reaches them - a view host, a control host and a clipboard |

## The application

A `ui::app` is a `platform::app` with what a frame asks of it: `wants_gpu()`,
`key_down`, `text_input`, `focus_mode()` and `track_menu`, which shows a menu a
frame raised - usually `parent->track_menu(bounds, commands)`. It is made before
`platform::run`, and its constructor installs the palette, with the colours the
running system theme decides. In `init` the application makes its window with
`ui::create_app_frame(app, store, host, style)`, which starts the renderer,
restores the window's saved place from the store and titles it with the
application's name; the application saves the place itself, with
`frame::save_window_position`. Closing the window asks `can_exit()`, and quits
if it agrees. The [README](../README.md#using-it) has a whole program.

What the application supplies:

- Its values of the opaque enums: `icon_index`, the code points of its icon
  font; `command_group`; and `commands`, its command ids. A value-initialized
  one is none.
- The chrome's glyphs, `ui::set_chrome_icons(...)`: a menu's check, check boxes,
  radio buttons and the caption buttons, from the icon font render was given.
  Without a check glyph a menu draws a stroked check.
- Render's hooks, where it uses what they serve
  ([render.md](render.md#hooks-the-application-sets)).

## The shell

`ui::shell` (`ui/shell.h`) is the simple application: one window, the menu bar
its menus make over the content it makes, opened where it was last time or
centred, with keys offered to the menus' accelerators before the application.
The menus and content are made just after the window, before anyone sees it, so
they can read its scale; a window that cannot be made ends the application with
exit code 1.
It answers everything a `ui::app` and an `element_host` must, so an application
overrides `make_content`, and `make_menus`, `start`, `key` and `stop` where it
uses them; one that needs more - several frames, a drawn caption - is a
`ui::app` with hosts of its own. The wheel goes to the element under the
pointer that scrolls, and the pointer moving over or leaving content with
nothing to answer it paints nothing again. A lost graphics device frees the
content's textures, which it makes again as it next paints. `initial_options()` lets an application settle its
options from its command line or settings before the window is made - an
offscreen test run, say.

A realtime application - a game, an emulator - sets
`shell_options::steps_per_second`. The shell then owns the loop: it calls
`step` at that fixed rate, no more than five steps at once, so a stall is
dropped rather than caught up; and it calls `present` after them, which paints
the content again unless the application presents another way. Minimized, the
application stands still and hears `minimized()`, where a looping sound stops,
unless `run_minimized` keeps it stepping, as an emulator playing a tune does.
`step_rate` changes the rate from the next step on, as a game's speed setting
does, and `quit(code)` ends the application with that exit code once the
current step is done. The steps read `keys()`: what is held, and what was
pressed or released since the step before, so a tap shorter than a step is
kept. A held key repeats to `key`, but never repeats a menu command it invoked.
Losing the keyboard lets go of every key, and a key let go of while the menu
bar or an open menu has the keyboard is let go of too. `ui::app::key_up` is how
the shell hears a release, which a frame offers the application outside a
dialog.

`canvas_element` draws what its callback draws, clipped to its bounds.
`surface_element` shows a picture the application makes - an emulator's
screen, a frame a renderer of its own drew - fitted, by whole multiples where
it can, and centred. A new picture of the same size updates its texture in
place and re-presents the window's scene over the picture alone
(`frame::redraw(bounds)`), so nothing is painted again. `render_to_surface`
draws an element with no window, for a headless capture.

## Frames

A `ui::frame` draws what its `frame_host` draws: the host lays out and paints,
and is told of the pointer, keys, ticks, drops and focus. The application's
window, a dialog, a pane, a popup and a bubble are all frames, the same on every
platform, and any of them makes frames of its own. A pane normally has a native
child window on Windows; `frame_style::embedded` composes it into its parent's
surface instead, sharing the renderer without a native window or device of its
own. On Linux every pane is composed. Popups remain separate windows, and a
dialog or popup of a window made `frame_style::offscreen` is offscreen too.
`ui::no_frame()` stands in for a frame that does not exist.

- The application's window lays its host out as it is made, so the first paint
  after `platform::run` shows a laid-out tree. A dialog, a pane and a popup are
  laid out once placed (`window_bounds`, `position`) or asked (`layout()`),
  never before the call that makes them returns: a host may keep its frame in a
  member and use it while it lays out.
- A window ticks `render::default_ticks_per_second` (five) times a second,
  whatever `frame_style::timer_milliseconds` says, which drives panes alone;
  faster frames come from `platform::set_frame_delay` and `frame_tick`. A
  pane's controls tick with its timer, or, in a pane with none, with the
  window or dialog holding it, so a control waiting on work it polls for hears
  of it wherever it is.
- `redraw(bounds)` presents what a frame last painted again, its host not asked
  to paint, when a texture or vertices it draws changed in place - a video
  frame, a visualisation - repainting only `bounds` where the renderer can; a
  composed pane's is its region of its owner's surface.
- A pane's context menu is asked of the root host, `frame_host::menu(screen)`,
  which decides by the screen point, and shown through the application's
  `track_menu`.
- `client_to_screen` maps a frame's physical client pixels to the desktop,
  through native parents or composed regions. Native child-window bounds are
  parent-relative; they are not a screen origin for tooltips or popups.
- An embedded pane follows a native child window's rules for input. Hidden,
  disabled or destroyed, it gives the keyboard to the nearest pane above it
  that can take it, or to its window. Disabled, it passes the pointer to its
  parent and ends any drag it holds. Once destroyed or disabled, it refuses
  `focus()`.

## The element tree

What a frame shows is built from `ui::element`s, each measured, laid out, drawn
and hit tested. A container, `ui::elements`, lays its children out as a flexbox
does - wrapping, growing, shrinking and aligning them within their minimums and
maximums - in logical units its measure context scales once. Text, links,
icons, dividers, padding and the drawn controls are elements of the library's;
an application adds its own.

Layout is stored as compact values rather than virtual policy objects. Dimensions
and limits remain full-width integers; flags and enum values use compact storage.
The flex solver keeps contiguous line ranges and only the item values it needs.
Single-line leaves declare a fixed measurement height, so resolving their width
does not require another virtual measurement; containers and wrapped text still
reflow. Background targets live with active animations, not in every element; a
fade holds its element and view weakly, so a dialog closed mid-fade goes at once.

`panel_element` gives a group the common background, clipping and inner inset,
without extra per-instance fields or outside margins. Its parent owns the gap
between panels. `document_panel` uses the same chrome for all document views;
its background remains visible independently of media-overlay opacity. Element
padding is an outside porch, while flex-container padding is an inside inset.

A hit test walks the tree and hands the pointer to a `ui::controller`, which
the `element_host` keeps while the pointer stays inside the region the test
says the answer holds for: `hit_test_context` cuts out of it everything tested
first and passed over, so reaching that tests again. A controller's own bounds
are never narrowed by what covers it, and Escape cancels a press a controller
holds, so the release performs nothing. An element asks its host for work
rather than doing it - a redraw, a layout, a fresh hit test, a tooltip, an
animation tick (`host_invalid`) - which the host may merge with its own.

One tree serves everything. A frame finds what it serves - the controls it gives
the pointer and the keyboard, in Tab order, and the frame elements whose frames
it places - by walking the elements its host offers (`visit_elements`) and what
each names in `visit_children`. So an element names everything it holds there,
and one that draws anything of its own draws what it holds too
(`render_children`): a control left out is neither drawn nor reached. A hidden
element hides what it holds, and a disabled one disables it. An element that
owns a frame - a map, a pane - is a `ui::frame_element`, whose frame its own
frame places after each layout.

## Controls

The controls - `make_edit`, `make_button`, `make_check_button`, `make_slider`,
`make_toolbar`, `make_date_time_control`, and the views - are drawn by the
library and belong to no frame: whatever tree holds one is where it is shown. A
control is laid out by flex like the rest of the tree, and measures and
arranges itself with the context its layout is given, never through a frame,
because it is laid out before a frame finds it.

Each frame's `ui::input_manager` knows which control has the keyboard, which
the pointer is over and which holds it, finding them in the tree as it needs
them. There are no child windows for controls: one window holds the keyboard,
and the manager says which control has it, telling the host (`focus_changed`).
A control that takes focus while its window lacks the keyboard keeps that
selection until the window gains it. A control holding the pointer since a
press that loses it before the release - to a message box, another window, the
system - hears `on_capture_lost`, and ends what the press began: a slider
settles where it was dragged, a button presses nothing.

The controls do what Diffractor uses. An edit holds UTF-8 text with `\n` line
breaks, `select` takes byte offsets, and it reports typing, not text the
application sets; it underlines the words the application's
`spelling_dictionary` rejects and offers its suggestions, and drops down
completions or folders, eight rows at a time, scrolled by the keys, the wheel
and a scrollbar. It lists a typed path's folders on a worker, once for each
folder typed in, so a slow drive never holds up typing. A multi-line edit
scrolls with the wheel and with the scrollbar it shows when its text is taller
than it, and stays where it was scrolled until its caret moves; text it is
given while another control has the focus reads from its first line. A
button's `&` marks its mnemonic only at the start of a word, so `R&B` keeps
its ampersand, and `&&` is one. A toolbar's maximize button answers as the
frame's caption button, so Windows 11 offers snap layouts over it. A toolbar
lights the button its keys act on only while it has the keyboard, and lights
the same one again when the keyboard comes back.

## Keys

Key codes are platform's Win32 virtual keys, carried as `char32_t` through
applications, frame hosts, controls, edit callbacks and menus. Typed text is
separate UTF-8 input. A frame offers a key down to:

1. a control in system mode - the menu bar - in the focused frame or a parent;
2. for a system key (Alt, F10) when no typing control has the keyboard and no
   dialog is open, the application's `key_down`, and then the menu bar;
3. a control that takes typing, which leaves Escape, Tab, Return and the arrows
   up and down to the application first, unless it claims them
   (`control::claims_key`), as an edit with its list of suggestions open does;
4. otherwise the application, the controls, then the host; in a dialog the
   application is not asked, and Escape closes it.

Windows reports AltGr as Ctrl+Alt, so a key-down says whether the layout types
a character with it (`key_state::alt_graph`). Such a key is text to a typing
control, neither a system key nor the application's shortcut, and a menu bar
answers no Ctrl+Alt at all, as a native one does not.

The application answers false for every key it does not use, or Alt+F4 and the
menus stop working. Typed text goes to the typing control that has the
keyboard, or to the application's `text_input` when its `focus_mode()` is
`text_edit`.

## Menus and the menu bar

Menus are drawn (`ui::track_drawn_menu`) in popups the frame makes, with the
keyboard, submenus and the work area's limits handled by the menu. A row is a
command - its check, its accelerator's text and its submenu - a separator, a
row of buttons (`command::buttons`, as cut, copy and paste are along the top of
a context menu), or an element shown and not chosen (`command::content`). `&`
marks a row's mnemonic: underlined, and its letter chooses it. A dimmed command
says nothing of why: its `disabled_reason` is for a toolbar's bubble. An open
menu raises `ui::command_active`, as a running command does, and
`ui::menu_active` beside it; the chosen command runs once the menu has gone,
with only the first raised. An application that makes its commands unavailable
while one runs counts the open menus out, or the menu's own rows dim under it.
A press outside the menu closes it and goes no further; the release that follows
reaches the window under it as it is, so a release that comes with no press of its
own is no click. A key let go of while the menu is open reaches no window, and
the application hears it through `ui::app::key_up`, so a key it held is never
left down.

`ui::make_menu_bar(commands)` is a control: put it first in the window's tree,
stretched across it. A tap of Alt or F10 enters menu mode, Alt and a marked
letter opens a menu, and a title click leaves the keyboard where it was.
`ui::dispatch_accelerator(commands, key, keys)`, called from the application's
`key_down`, invokes the first enabled, visible command whose accelerator
matches, the keypad's digits answering for the top row's.

## Working the window from outside

An agent or a test works a window as a user does, from what it shows.
`toolbar::buttons()` gives each button's command, name, state and place in its
frame; the menu open now, `ui::open_menu_session()`, gives its `levels()`: each
row's text, keys and state, a row of buttons cell by cell, and where its popup
shows it. `ui::send_pointer`, `send_wheel`, `send_context_menu`, `send_key` and
`send_text` give a frame input that takes real input's path - hit testing, the
controls' focus and capture, then the host - and a modal loop the input opens
runs inside the call. An open menu's loop has the input first, as it has the
platform's. A press gives its window the keyboard as activating it would, an
offscreen window included, which the platform never gives one; a window a
dialog holds takes nothing. The input is the user's activity, as real input is,
so what fades once the user goes idle stays while an agent works.
`ui::on_control_used` tells the application of each
control used - a button pressed, a toolbar's command run, typing, a slider let
go, a date chosen - so it can count what has been exercised.

## Text and views

The text model is window-free: `text_buffer` holds UTF-8 lines, selections and
grouped undo, and tells its `view_host` what changed. The syntax highlighters
compare extensions and keywords without regard to ASCII case. `ui::md` parses
Markdown and reduces HTML to it, and `table_layout` parses and sizes the tables
the Markdown and CSV views draw. Its column sizing and cancellable resize gesture
are also used by list controls and element tables: earlier columns stay put,
the last makes room, Escape restores the widths and a double-click fits content.
Widths survive relayout and a user-sized pixel column follows changes of DPI.
Element tables keep them through empty row refreshes and can transfer their logical
size snapshot to a replacement table before its first layout. An element table
with headings draws them in a band pinned over its rows: a click on one sorts the
rows by the key each row gives for its column - first the column's own way, then
the other, rows without a key last - and the divider after one resizes its
column, the filled last column giving back what filling gave it. A table without
headings has neither, so its first row never reads as a heading.
`on_user_change` tells an owner that rebuilds its table, so the replacement gets
the same sizes and order.
A view never opens a file, knows a path or
names an application: loading and saving stay the application's.

The views are controls: `view_base` → `text_view` → `doc_view`, then
`edit_doc_view` and `read_only_doc_view`, with `csv_view`, `hex_view` and
`markdown_view` over the read-only one; and `list_view`, a virtualised list
with weighted, resizable columns. `document_panel` gives any of these contents
the same inset, clipped background and flex placement, exposing its contents
to the normal element tree rather than making another window.
Beside them, `split_element` is an element holding two
panes and the bar between them, and `scrollbar` the scroll bar the views draw.
Each view draws over the source text, so selection works on what is on screen.
`text_view` owns selection, scrolling and the clipboard *with its shortcuts*, so
a view with no menu behind it can still be copied from; `read_only_doc_view`
answers `at_bottom` and `scroll_to_end`, so a growing transcript follows its end
only when the reader was there. `view_theme` holds the fonts, spacing and
palette a view draws with: metrics are data, and `style_color` is virtual so an
application can recolour without touching a highlighter.

## Tests

`libdesktop_ui_tests`, in `tests/ui/`, under the CTest label `libdesktop-ui`,
links ui and holds the resource lock `libdesktop-clipboard`. It covers the
controls, menus and the menu bar, bubbles, the element tree, the text model,
Markdown and table layout, the syntax highlighters, every view and animation,
and the shell's keys, step clock and elements;
layout and hit testing need no window, since `measure_context` is an interface
`ui/test_support.h` fakes. `tests/ui/win/` adds what needs a Windows frame: the
caption button, layered bubbles, textures recycled and video frames presented.
On Windows `libdesktop-ui-frame-creation`, under the same label, makes real
frames over an offscreen application window, none visible on the desktop. It
also checks that embedded panes create no native windows, paint through their
owner, translate input, retain capture and focus, give the keyboard up when
hidden, disabled or destroyed, pass clicks through when disabled, and receive
their own timers, and that the application hears a key let go, even in a
menu's modal loop. Input sent from outside takes real input's path there too:
presses, keys and text, and a context menu worked inside its loop.
`libdesktop-ui-shell` runs a shell application over an offscreen window,
posting it the key messages a keyboard would: its menu bar and content, its
fixed steps, the keys they read and its menus' accelerators.

## Known gaps

- A text view gives every code point one cell: a wide character - CJK, most
  emoji - is drawn one cell wide and the next overprints its right half, and
  the caret and hit testing follow the same grid, which is why
  `view_theme::text_font` is a fixed-pitch face.
- A CSV record is one line: `csv_view` splits each physical line, so a quoted
  field with a line break in it draws its second half as a new record.
- The drawn controls, menus and views expose nothing to screen readers: there
  is no UI Automation provider on Windows and no AT-SPI on Linux.
- A mnemonic is pressed by its key code, so only a marked ASCII letter or digit
  answers; a Cyrillic one is shown but has no key.
