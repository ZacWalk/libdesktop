# To do

Open development tasks and their acceptance criteria. Keep completed work out
of this list. Current limitations are listed under *Known gaps* in
[core.md](core.md#known-gaps), [platform.md](platform.md#known-gaps),
[render.md](render.md#known-gaps) and [ui.md](ui.md#known-gaps).

## core

- [ ] **Retire the `_snscanf_s` shim on Linux.** `core/dialect.h` gives GCC and
      Clang a bounded `sscanf` under MSVC's name, because Diffractor still reads
      XMP rationals, property ratios and search terms with `_snscanf_s`, and the
      two can read damaged text differently. Done when those reads use portable
      parsers and the shim is deleted.

## platform

- [ ] **Linux file dialogs, clipboard and drag.** The file pickers,
      `show_modal_dialog`, the file and image clipboard and drag and drop, either
      way, are stand-ins on Linux (`src/platform/linux/desktop.cpp` and
      `shell.cpp`); only clipboard text works. SDL3, already linked, has file
      dialogs through the desktop portal, drop events and clipboard data by MIME
      type; dragging files out needs more. Done when each works on Wayland and
      X11 desktops and the capability queries answer true there.
- [ ] **Name the image-only save picker for what it does.**
      `prompt_for_save_path(path)` and `prompt_for_save_path(owner, path)` offer
      JPEG, PNG, WebP and JPEG XL alone, while the overloads beside them take the
      caller's filters, and dde picked the image one by mistake. Consider
      `prompt_for_image_save_path`. Done when the name says what it offers and
      every consumer has moved in the same step.

## render

- [ ] **Measure trailing whitespace alike.** *Owner decision.* DirectWrite's
      measured width leaves trailing whitespace out and Pango's counts it, so
      text that ends in a space measures differently on each platform. Decide
      which answer both give - Diffractor, which ships on Windows, is laid out
      with DirectWrite's - and make the other backend give it. Done when the
      render suite measures text with trailing spaces and passes on both.

## ui

- [ ] **Accessibility of the drawn controls.** The controls, menus and views
      draw themselves, so nothing reaches a screen reader: there is no UI
      Automation provider on Windows and no AT-SPI on Linux. Done when a screen
      reader can name, read and operate the controls and menus of a dialog,
      Diffractor's first.

## Second adopters

A shared component is tested by its second consumer; read the adopter's tests
before replacing its code.

- [ ] **One list.** Diffractor's task rows and `ui::list_view` become one
      virtualised list, sys-mate's next. Done when Diffractor's own list is gone.
- [ ] **One document view.** Diffractor's selection panel shows text, bytes,
      Markdown and CSV through ui's read-only, hex, Markdown and CSV views in
      place of its `document_control`. Done when `document_control` is gone and
      its tests run against the views.

## Workspace

- [ ] **Propagate dd's `gui` template with libdesktop's next release.** `dd init --type
      gui` in dd's source tree generates a Windows/MSVC application with
      `libdesktop_add_app`, `ui::app`/`ui::frame_host` and `platform::run`. Vendor
      that template in every repository and verify that a fresh `gui` project builds
      and passes its smoke test.
