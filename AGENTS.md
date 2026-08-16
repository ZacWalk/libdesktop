# AGENTS.md

platform-h is the platform layer shared by every application in this workspace
(`list0`, `ovrwin`, `potato`, `rethinkify-app`, `spq`, `stuntcarracer`). It is a
library with no application of its own.

## Non-negotiables

1. **`platform.h` includes no OS headers.** Not `windows.h`, not transitively.
   The Windows SDK appears only in `platform_win.cpp` and
   `platform_win_audio.cpp`.
2. **The API must be expressible on another OS.** Nothing Win32-shaped may leak
   into a signature: no `HWND`, no `HRESULT`, no hundredths-of-a-decibel volume,
   no resource IDs. Prefer neutral units — linear gain, normalised pan, Hz,
   UTF-8 `string_view`. If a concept only makes sense on one platform, it
   belongs in the app.
3. **App-specific knowledge stays out.** No app names, no resource identifiers,
   no game constants. `config_set_app_name` exists precisely so the layer never
   has to know who is calling.
4. **A change here breaks six repos.** Adding to `window_frame` or
   `frame_reactor` is a breaking change for every app and their test stubs.
   Build all of them before you consider a change done — see below.
5. **Add a test with a capability.** `tests/platform_tests.cpp` is a console
   program; anything checkable without a window belongs in it.

## Layout

| File | Contents |
|---|---|
| `platform.h` | The whole `pf::` surface. Declarations, plus inline text/geometry helpers |
| `platform_common.cpp` | Backend-independent implementation (the embedded-resource registry, `line_splitter`) |
| `platform_win.cpp` | Win32 backend: `WinMain`, message loop, GDI drawing, WIC, WinINet/WinHTTP, dialogs, clipboard, spell check, child processes |
| `platform_win_audio.cpp` | XAudio2 backend, isolated from the rest |
| `cmake/platform_app.cmake` | `platform_add_app()` — the function apps use to declare themselves |
| `cmake/embed_resources.cmake` | Turns data files into a generated C++ byte-array TU |
| `cmake/app.manifest.in` | The manifest every app gets |
| `tests/` | The unit suite and its embedded fixture |

## Build and test

```pwsh
.\dd.ps1 test                  # default command: build + run the suite
.\dd.ps1 build -Config Debug
```

To check nothing downstream broke, from the workspace root:

```pwsh
foreach ($r in 'platform-h','list0','ovrwin','potato','rethinkify-app','spq','stuntcarracer') {
    Push-Location $r; cmake --build --preset release; Pop-Location
}
```

Each app finds a sibling `../platform-h` checkout automatically, so a local edit
here is picked up by all of them with no publish step.

## Adding a capability

1. Declare it in `platform.h`, in neutral terms (see rule 2).
2. Implement it in the backend.
3. Add a case to `tests/platform_tests.cpp` if it can run without a window.
4. Rebuild every app. If you added a pure virtual to `window_frame`, expect to
   update `stub_window_frame` in `rethinkify-app/src/tests.cpp` too.

## Conventions

- Modern C++20 in `snake_case`; `pf::` for everything public.
- Comments explain *why*, in one line. Do not narrate the next line.
- Diagnose failures with `pf::debug_trace` and the real error code rather than
  failing silently.
- `dd.ps1` is duplicated in each repo by design — it has to run before CMake, so
  it cannot come from this package.
