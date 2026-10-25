# To do

Open development tasks and their acceptance criteria. Keep completed work out
of this list. Current limitations are listed under *Known gaps* in the
[README](../README.md#known-gaps), [system.md](system.md#known-gaps) and
[render.md](render.md#known-gaps).

## Decisions needed first

- [ ] **Merge each service with its `pf::` equivalent.** libdesktop-system and
      libdesktop-core both provide files, settings, child processes, web
      requests with async HTTP, sound, the clipboard and known folders. Merge
      them one service at a time, keeping the better implementation. Decide
      first where a merged service lives: in libdesktop-system, which every
      app built on libdesktop-core then links; or with libdesktop-system's
      Windows backend calling libdesktop-core's, its Linux backend staying its
      own. Done when each pair has one implementation and both suites pass.

## libdesktop-system

- [ ] **Audio output in libdesktop-system.** The WASAPI `platform::audio_output`
      moves from libdesktop-render to libdesktop-system, merged with
      libdesktop-core's `pf::audio_stream`.
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

The two window models - libdesktop-core's reactor and libdesktop-render's
`ui::` - stay side by side until one application needs both. ddc stays
independent of libdesktop.
