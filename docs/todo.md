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

## Second adopters

Each module was extracted from one application. A second adopter is where its
design is tested; read the adopter's tests before replacing its code.

- [ ] **A second libdesktop-render consumer.** Its window model still serves
      Diffractor alone. zpilot uses core windowing and audio while retaining
      its specialized HDR/bloom/CRT renderer; do not replace that renderer
      merely to adopt another target.
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
