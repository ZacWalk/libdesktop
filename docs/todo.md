# To do

Open development tasks and their acceptance criteria. Keep completed work out
of this list. Current limitations are listed under *Known gaps* in the
[README](../README.md#known-gaps), [system.md](system.md#known-gaps) and
[render.md](render.md#known-gaps).

## libdesktop-os

A service libdesktop-core and libdesktop-system both implement moves into
libdesktop-os one at a time, held to both suites' record of its outcomes
([AGENTS.md](../AGENTS.md#libdesktop-os)). The web client, clipboard text, known
folders and audio output have moved. Done when each pair below has one
implementation and both suites pass.

- [ ] **Files and file dialogs.** pf:: wraps Win32 thinly over `pf::file_path`,
      UTF-8 text, and reports failure through `GetLastError` - dde words its
      errors from it - while libdesktop-system works in `df::file_path`, with
      extended-length paths, shell operations and `file_op_result`. Decide first
      how the merged service takes a path - UTF-8, and when it adds the `\\?\`
      prefix, which a relative path cannot take - and how it reports failure;
      then record both stacks' outcomes for copy, move, rename, delete, recycle,
      folder creation, enumeration and the dialogs before moving them.
- [ ] **Settings.** pf:: keeps an INI beside the executable, else in a temporary
      folder; libdesktop-system keeps the registry or its own INI, with escaped
      line breaks. Merge the store, not the locations: every application must
      go on reading the settings its users already have.
- [ ] **pf::'s default user agent.** An empty user agent sends `PotatoApp/1.0`,
      which names an application. Make the default neutral, or require one.

Not duplicates, so not merged: pf::'s child processes, with pipes and line
callbacks, beside `platform::run`, which starts a program and returns;
`try_lock_instance`, one holder per user, beside `claim_startup_scope`; pf::'s
asynchronous WinHTTP client, which libdesktop-system has no counterpart for;
and spelling and images, where the stacks use different engines - the Windows
spell checker and Hunspell, WIC and libfiles.

## libdesktop-system

- [ ] **`df::blob` adopts a `std::vector`.** Diffractor copies each encoded
      image from libfiles' `std::vector` into a `df::blob`, which cannot take
      ownership of one. Done when the blob can adopt the vector and Diffractor's
      encoders drop the copy.
- [ ] **Retire the `_snscanf_s` shim on Linux.** Dates parse by one rule
      everywhere (`date_text`), but Diffractor still reads XMP rationals,
      property ratios and search terms with `_snscanf_s`, which can differ
      from glibc on damaged text. Done when those reads use portable parsers
      and the Linux shim is deleted.

## Second adopters

Each module was extracted from one application. A second adopter is where its
design is tested; read the adopter's tests before replacing its code.

- [ ] **libdesktop-system: ImageWalker 3.0.** Its forked
      `include/core/util.h`, `util_layout.h` and `util_geometry.h` overlap
      `df::` and `str::`, and `util_geometry` is part of libiw's public API.
      Re-measure the divergence when the work starts.
- [ ] **libdesktop-core: ImageWalker 3.0's `src30/Platform*`.** `iw::platform`
      copies `pf::`'s shape; move it onto `pf::`, and its types onto
      libdesktop-system.
- [ ] **libdesktop-render: zpilot.** zpilot draws with its own Direct3D 11
      renderer.
- [ ] **The raw-Win32 apps.** lib8bit's `app/`, sys-mate, ergo-active,
      fast-draw, low-ban and vector-experiments' `platform.hpp` each carry their
      own window, message loop and GDI helpers. Move each only once libdesktop
      has what it needs; their Windows SDK code may stay where it is.
- [ ] **More of `src/ui` in libdesktop-text.** Diffractor's read-only document
      view draws text and dumps itself, coloured by libdesktop-text. Rendered
      Markdown or CSV tables there would want `ui/markdown.h` (standard C++ but
      for one `_strnicmp`) and `table_layout`'s cell parsing moved into it, not
      the views, which are written in `pf::`. Done when a second consumer needs
      either.

The two window models - libdesktop-core's reactor and libdesktop-render's
`ui::` - stay side by side until one application needs both. What depends on
neither is shared instead: libdesktop-text, the text vocabulary and syntax
highlighters that libdesktop-ui and Diffractor's document view both use. ddc
stays independent of libdesktop.
