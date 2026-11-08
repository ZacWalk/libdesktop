# libdesktop::ui

`libdesktop::ui` is the interface an application is built from, the same on
every platform: the element tree and its flex layout, the frame every window,
dialog, pane, menu and bubble is, `ui::app`, drawn controls with the input
manager that gives them the keyboard and the pointer, the menu bar, commands and
menus, and the text model, syntax highlighters and text views. It is written
over `platform::window` and render's devices alone, so `src/ui` has no
operating-system folder; it builds wherever the layers below do.

## Using it

```cmake
target_link_libraries(app PRIVATE libdesktop::ui)
```

`libdesktop/ui.h` brings in render and the vocabulary - `ui/fwd.h`, `ui/style.h`,
`ui/command.h`, `ui/frame.h` and `ui/app.h`. The rest is included where used:
`ui/elements.h`, `ui/controls.h`, `ui/menu_bar.h`, `ui/syntax.h`,
`ui/test_support.h`, and `ui/views.h` for the text models and views.

## Headers

| Header | What it holds |
|---|---|
| `ui/fwd.h` | The opaque enums `icon_index`, `command_group` and `commands`, forward declarations, pointer aliases |
| `ui/style.h` | The palette (`style::colors`) and the animation helpers |
| `ui/command.h` | `command`, the styles of edits and toolbars, the spelling dictionary an edit is given, the chrome's glyphs |
| `ui/frame.h` | `frame_host`, `frame_style`, `frame`, `create_app_frame`, the bubble, `null_frame`, saving and restoring the focus |
| `ui/app.h` | `ui::app`, the UI's part of the application |
| `ui/elements.h` | Elements, flex layout, hit testing and controllers, `element_host`, the scroller, frame elements, and text, link, icon, divider and padding elements |
| `ui/controls.h` | `control` and its kinds - buttons, edits, sliders, toolbars, date fields - the input manager, the drawn menu and bubble, and the window-free models they edit |
| `ui/menu_bar.h` | `make_menu_bar` and `dispatch_accelerator` |
| `ui/text_types.h`, `ui/syntax.h` | Text locations, selections and styles, and the syntax highlighters: standard C++ alone |
| `ui/text/*.h` | `text_buffer` and `text_line`, `view_host`, the Markdown model, table layout, spelling and the clipboard seam |
| `ui/views/*.h`, `ui/views.h` | The text, document, list, CSV, hex and Markdown views, the scroll bar, the splitter and `view_theme` |
| `ui/test_support.h` | Headless fakes, in `ui::test`: a fixed-grid measure context, a draw context that records, a view host, a control host and a clipboard |

## The application

A `ui::app` is a `platform::app` with what a frame asks of it: `wants_gpu()`,
`key_down`, `text_input`, `focus_mode()` and `track_menu`, which shows a menu a
frame raised - usually `parent->track_menu(bounds, commands)`. It is made before
`platform::run`, and its constructor installs the palette, with the colours the
running system theme decides. In `init` the application makes its window with
`ui::create_app_frame(app, store, host, style)`, which starts the renderer,
restores the window's saved place from the store and titles it with the
application's name; it saves the place itself, with
`frame::save_window_position`. Closing the window asks `can_exit()`, and quits
if it agrees. The [README](../README.md#using-it) has a whole program.

What the application supplies:

- Its values of the opaque enums: `icon_index`, the code points of its icon
  font; `command_group`; and `commands`, its command ids. A value-initialized
  one is none, and libdesktop never names a value.
- The chrome's glyphs, `ui::set_chrome_icons(...)`: a menu's check, check boxes,
  radio buttons and the caption buttons, from the icon font render was given.
  Without a check glyph a menu draws a stroked check.
- Render's hooks, where it uses what they serve ([render.md](render.md#hooks-the-application-sets)).

## Frames

A `ui::frame` draws what its `frame_host` draws: the host lays out and paints,
and is told of the pointer, keys, ticks, drops and focus. The application's
window, a dialog (`create_dlg`), a pane, a popup and a bubble (`create_frame`
with a `frame_style`, `create_bubble`) are all frames, the same on every
platform, and any of them makes frames of its own. A pane is a native child
window on Windows and a region composed into its parent's surface on Linux. A
`container` frame draws nothing of its own: it holds frames, and its background
shows where they leave it uncovered - Diffractor's window holds its title bar,
view and status bar so, each a surface of its own. `ui::null_frame`, which
`ui::no_frame()` returns, stands in for a frame that does not exist, and a
dialog on it reports itself cancelled.

- The application's window lays its host out as it is made, so the first paint
  after `platform::run` shows it is of a laid out tree. A dialog, a pane and a
  popup are laid out once placed (`window_bounds`, `position`) or asked
  (`layout()`), never before the call that makes them returns: a host may keep
  its frame in a member and use it while it lays out.
- `frame_style::client_size` and `minimum_client` are physical pixels, as
  `platform::window_options`' are. `drawn_caption` is true by default: the
  application draws the caption, and false keeps the system's title bar.
- A window ticks `render::default_ticks_per_second` (five) times a second,
  whatever `timer_milliseconds` says - that is the idle count its host reads.
  `timer_milliseconds` drives panes alone; faster frames come from
  `platform::set_frame_delay` and `frame_tick`.
- A pane's context menu is asked of the root host, `frame_host::menu(screen)`,
  which decides by the screen point, and shown through the application's
  `track_menu`.
- `ui::save_focus()` keeps where the keyboard was, and `restore_focus` gives it
  back once a dialog closes.

## The element tree

What a frame shows is built from `ui::element`s, each measured, laid out, drawn
and hit tested. A container, `ui::elements`, lays its children out as a flexbox
does - wrapping, growing, shrinking and aligning them within their minimums and
maximums - in logical units its measure context scales once. Text, links,
icons, dividers, padding and the drawn controls are elements of the library's;
an application adds its own.

A hit test walks the tree and hands the pointer to a `ui::controller`, which
the `element_host` keeps while the pointer stays inside the region the test
says the answer holds for: `hit_test_context` cuts out of it everything tested
first and passed over, so reaching that tests again. A controller's own bounds
are never narrowed by what covers it, and Escape cancels a press a controller
holds, so the release performs nothing. An element asks its host for work rather
than doing it - a redraw, a layout, a fresh hit test, a tooltip, an animation
tick (`host_invalid`) - which the host may merge with its own. `ui::scroller` is
the scroll bar such a host draws, with its section bands and the anchor that
keeps content in place across a layout. A link bound to one of the
application's `commands` asks its host to invoke it and whether it is checked,
so the host decides what invoking means.

One tree serves everything. A frame finds what it serves - the controls it gives
the pointer and the keyboard, in Tab order, and the frame elements whose frames
it places - by walking the elements its host offers (`visit_elements`) and what
each names in `visit_children`. So an element names everything it holds there,
and one that draws anything of its own draws what it holds too
(`render_children`): a control left out is neither drawn nor reached. What an
element holds is offered the wheel, a press, a hover and a tooltip
(`controller_from_location`), each moved by its `children_offset`. A hidden
element hides what it holds, and a disabled one disables it. An element that
owns a frame - a map, a pane - is a `ui::frame_element`, whose frame its own
frame places after each layout.

## Controls

The controls - `make_edit`, `make_button`, `make_check_button`, `make_slider`,
`make_toolbar`, `make_date_time_control`, and the views - are drawn by the
library and belong to no frame: whatever tree holds one is where it is shown. A
control measures as large as it asks, plus any `room`, and no narrower than the
share of the width it is given that `min_share` names (5 is a fifth), so a row
of them is laid out by flex like the rest of the tree; it measures and arranges
itself with the context its layout is given, never through a frame, because it
is laid out before a frame finds it. A control draws and takes the pointer in
the coordinates its bounds are in; one with `traits.local_coordinates`, every
view among them, does so from (0, 0) at its own corner wherever the tree places
it. `frame_bounds()` is where it sits in its frame.

Each frame's `ui::input_manager` knows which control has the keyboard, which
the pointer is over and which holds it, and owns none: it finds them by walking
the tree as it needs them, so Tab follows the tree, a control under a hidden
element is passed over, and one taken out of the tree is no longer reached. A
control takes input only when it and every element above it are enabled, and
gives up the keyboard when it no longer does. There are no child windows for
controls: one window holds the keyboard, and the manager says which control has
it, telling the host (`focus_changed`). `ui::frame::focus()` clears its control's
focus and gives the keyboard to the frame (`keyboard_to_frame`). If a control
takes focus while its window lacks the keyboard, it keeps that selection until
the window gains it.

The controls do what Diffractor uses. An edit holds UTF-8 text with `\n` line
breaks, `select` takes byte offsets, and it reports typing, not text the
application sets; it underlines the words the application's
`spelling_dictionary` rejects and offers its suggestions, and drops down
completions or folders. A button's `&` marks its mnemonic only at the start of
a word, so `R&B` keeps its ampersand, and `&&` is one. Radio buttons check one
per group, sliders report while dragging and once released, and a toolbar's
maximize button answers as the frame's caption button, so Windows 11 offers snap
layouts over it.

## Keys

Key codes are platform's Win32 virtual keys. A frame offers a key down to:

1. a control in system mode - the menu bar - in the focused frame or a parent;
2. for a system key (Alt, F10) when no typing control has the keyboard and no
   dialog is open, the application's `key_down`, and then the menu bar;
3. a control that takes typing, which leaves Escape, Tab, Return and the arrows
   up and down to the application first;
4. otherwise the application, the controls, then the host; in a dialog the
   application is not asked, and Escape closes it.

The application answers false for every key it does not use, or Alt+F4 and the
menus stop working. Typed text goes to the typing control that has the
keyboard, or to the application's `text_input` when its `focus_mode()` is
`text_edit`.

## Menus and the menu bar

Menus are drawn (`ui::track_drawn_menu`) in popups the frame makes, a submenu's
nested in the one it opens from, with the keyboard, submenus and the work area's
limits handled by the menu. A row is a command - its check, its
`keyboard_accelerator_text`, which `platform::format_keyboard_accelerator`
makes, and its submenu - a separator, a row of buttons (`command::buttons`, as
cut, copy and paste are along the top of a context menu), or an element shown
and not chosen (`command::content`). `&` marks a row's mnemonic: underlined,
and its letter chooses it. A dimmed command says nothing of why: its
`disabled_reason` is for a toolbar's bubble.

`ui::make_menu_bar(commands)` is a control: put it first in the window's tree,
stretched across it. A tap of Alt or F10 enters menu mode, Alt and a marked
letter opens a menu, and a title click leaves the keyboard where it was.
`ui::dispatch_accelerator(commands, key, keys)`, called from the application's
`key_down`, invokes the first enabled, visible command whose accelerator
matches, the keypad's digits answering for the top row's.

## Text and views

The text model is window-free: `text_buffer` holds UTF-8 lines, selections and
grouped undo, and tells its `view_host` what changed. The syntax highlighters
(`ui/syntax.h`, over `ui/text_types.h`) include the standard library alone and
compare extensions and keywords without regard to ASCII case; Diffractor's
document view colours with them. `ui::md` parses Markdown and reduces HTML to
it, and `table_layout` parses and sizes the tables the Markdown and CSV views
draw. A view never opens a file, knows a path or names an application: loading
and saving stay the application's.

The views are controls placed in a tree: `view_base` → `text_view` →
`doc_view`, then `edit_doc_view` and `read_only_doc_view`, with `csv_view`,
`hex_view` and `markdown_view` over the read-only one; `list_view`, a
virtualised list with weighted columns; `split_element`, two panes and the bar
between them; and `scrollbar`. Each draws over the source text, so selection
works on what is on screen. `text_view` owns selection, scrolling and the
clipboard *with its shortcuts*, so a view with no menu behind it can still be
copied from; `read_only_doc_view` answers `at_bottom` and `scroll_to_end`, so a
growing transcript follows its end only when the reader was there.
`view_theme` holds the fonts, spacing and palette a view draws with: metrics
are data, and `style_color` is virtual so an application can recolour without
touching a highlighter.

## Tests

`libdesktop_ui_tests`, in `tests/ui/`, under the CTest label `libdesktop-ui`,
links ui, and shares the resource lock `libdesktop-clipboard` with the platform
suite. It covers the controls - editing, focus and Tab order, keys, spelling,
mnemonics, dialogs, radio groups, sliders, toolbars, dates - the menus and the
menu bar's keyboard, bubbles, the element tree's flex layout, scroller and
controllers, the text model, Markdown and table layout, the syntax highlighters,
every view, and animation; layout and hit testing need no window, since
`measure_context` is an interface `ui/test_support.h` fakes.
`tests/ui/win/` adds what needs a Windows frame: the caption button, layered
bubbles, textures recycled, video frames presented and the chrome's embedded
images decoded.

## Known gaps

- A text view gives every code point one cell: a wide character - CJK, most
  emoji - is drawn one cell wide and the next overprints its right half, and
  the caret and hit testing follow the same grid, which is why
  `view_theme::text_font` is a fixed-pitch face.
- A CSV record is one line: `csv_view` splits each physical line, so a quoted
  field with a line break in it draws its second half as a new record.
- The drawn controls expose nothing to screen readers ([todo](todo.md#ui)).
- A mnemonic is pressed by its key code, so only a marked ASCII letter or digit
  answers; a Cyrillic one is shown but has no key.
- Most headers rely on the umbrella's includes and order.
