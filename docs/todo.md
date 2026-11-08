# To do

Open development tasks and their acceptance criteria. Keep completed work out
of this list. Current limitations are listed under *Known gaps* in the
[README](../README.md#known-gaps), [system.md](system.md#known-gaps) and
[render.md](render.md#known-gaps).

## libdesktop-os

A service libdesktop-core and libdesktop-system both implement moves into
libdesktop-os one at a time, held to both suites' record of its outcomes
([AGENTS.md](../AGENTS.md#libdesktop-os)). The web client, clipboard text, known
folders, audio output, files, dialogs and the escaped-INI store have moved on
Windows. The wrappers retain their own paths, settings locations, registry
policy and result types; Linux keeps its native headless implementations until
an os backend exists.

Not duplicates, so not merged: pf::'s child processes, with pipes and line
callbacks, beside `platform::run`, which starts a program and returns;
`try_lock_instance`, one holder per user, beside `claim_startup_scope`; pf::'s
asynchronous WinHTTP client, which libdesktop-system has no counterpart for;
and spelling and images, where the stacks use different engines - the Windows
spell checker and Hunspell, WIC and libfiles.

## libdesktop-system

- [ ] **Retire the `_snscanf_s` shim on Linux.** Dates parse by one rule
      everywhere (`date_text`), but Diffractor still reads XMP rationals,
      property ratios and search terms with `_snscanf_s`, which can differ
      from glibc on damaged text. Done when those reads use portable parsers
      and the Linux shim is deleted.

## One UI stack

Decided 2026-11-08: libdesktop converges on libdesktop-render. Its element tree
(`ui_elements.h`, moved from Diffractor) is the one retained UI model, and
libdesktop-ui's views are rebuilt on it. Each step ships alone, the code moved
before its behaviour changes. The drawn controls are elements of the tree,
painted, hit and focused through it, and render names no application
([render.md](render.md#the-element-tree)).

- [ ] **A small application on render.** ergo-active moves, as decided on
      2026-11-08: a drawn caption - its title, a menu button for Hide to tray and
      Exit, and window buttons drawn from the system's Segoe icon font when the
      application bundles no icon font; drawn dialogs for its close prompt and
      reminders; and render's slider and edit in its element tree. Three steps,
      each shipping alone:
      1. Its services where a render application reaches them. A render
         application cannot link libdesktop-core beside render: core's `WinMain`
         shares a translation unit with the services, and core's window procedure
         delivers named-instance signals. The tray - icon, urgency, balloons, its
         menu, re-adding after Explorer restarts - and the single-instance lock and
         show-window signal move into libdesktop-os, held to core's tests; core
         keeps its `pf::` wrappers, libdesktop-system gains `platform::` ones, and
         render's application window delivers the signal.
      2. `platform_add_app(... RENDER)` links render and its entry point and writes
         the resource script: `platform_win_render.rc`, the window icon, and the
         application's icon font when it has one.
      3. The dashboard (`ui-frame.h`) on the element tree, its tests moved with it.
      Done when ergo-active ships on render.
- [ ] **libdesktop-ui's views on elements.** One virtualized list serving
      Diffractor's task rows and `pf::ui::list_view`'s panel, then sys-mate; the
      text, read-only, Markdown, CSV and hex views, with Diffractor's selection
      panel in place of `document_control` as their second consumer, then dde and
      ImageWalker 3.0; libdesktop-ui building on Linux. The open decision on
      `pane_host` and `composer` (workspace AGENTS.md) is settled then: a tree in
      one frame does what `pane_host` emulated.

## Second adopters

Each module was extracted from one application. A second adopter is where its
design is tested; read the adopter's tests before replacing its code.

- [ ] **A second libdesktop-render consumer.** Its window model still serves
      Diffractor alone; ergo-active is the first planned (above). zpilot uses core
      windowing and audio while retaining its specialized HDR/bloom/CRT renderer;
      do not replace that renderer merely to adopt another target.
- [ ] **More of `src/ui` in libdesktop-text.** Diffractor's read-only document
      view draws text and dumps itself, coloured by libdesktop-text. Rendered
      Markdown or CSV tables there would want `ui/markdown.h` (standard C++ but
      for one `_strnicmp`) and `table_layout`'s cell parsing moved into it, not
      the views, which are written in `pf::`. Done when a second consumer needs
      either.

The two window models - libdesktop-core's reactor and libdesktop-render's
`ui::` - converge on render ([One UI stack](#one-ui-stack)); until each consumer
has moved, what depends on neither is shared instead: libdesktop-text, the text
vocabulary and syntax highlighters that libdesktop-ui and Diffractor's document
view both use. ddc stays independent of libdesktop.
