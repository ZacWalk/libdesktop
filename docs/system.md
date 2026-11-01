# libdesktop-system

libdesktop-system is the foundation and the operating-system services that
began as Diffractor's: the `df::` and `str::` vocabulary - strings, paths,
dates, geometry, containers, hashing, Base64, SIMD, logging and JSON - and the
`platform::` services written in it - files, settings, processes, threads and
events, known folders, the shell, sharing, the web and crash recovery - for
Windows and Linux, built with MSVC and GCC.

It sits beside libdesktop-core (`pf::`) and libdesktop-ui (`pf::ui::`) rather
than on top of them. The two vocabularies are separate on purpose: `pf::` is a
small layer under a reactor window model that four applications share, and
this module is the larger layer under Diffractor's own `ui::` framework. They
merge only when one application needs both.

## Using it

The module is opt-in. A consumer turns it on before it brings libdesktop in,
then links `libdesktop::system`:

```cmake
set(LIBDESKTOP_SYSTEM ON)
FetchContent_MakeAvailable(libdesktop)
target_link_libraries(app PRIVATE libdesktop::system)
```

Built directly, libdesktop builds it by default, and on Linux it is the only
part that builds.

Code includes the umbrella header first. It brings in the compiler dialect
(`platform_compat.h`), the standard library the module is written against,
`using namespace std::literals`, and the module's headers in dependency order.
The other headers are not standalone:

```cpp
#include "libdesktop/system.h"
#include "libdesktop/util_base64.h"   // and util_kdtree.h, util_log.h, util_simd.h where used
```

Every header lives under `include/libdesktop/`, so its name cannot collide with
an application's headers or with libdesktop-core's own `platform.h`.

### What the application supplies

One symbol, defined once by the application:

```cpp
const std::string_view s_app_name = "Diffractor";
```

The module names what it makes from it, and those names are fixed by it:

| What | Name |
|---|---|
| The log | `<stem>.log` and `<stem>.previous.log`, beside the executable when that folder is writable, otherwise in app data |
| Settings | `<stem>.ini`; on Windows the registry key `Software\<name>` when one already exists |
| App data and cache | `%LOCALAPPDATA%\<name>`; `$XDG_DATA_HOME/<stem>` and `$XDG_CACHE_HOME/<stem>` |
| Temporary and staging files | `<stem>_…` |
| The startup scope (Windows) | the mutex `Local\<name>StartupScope` |

`<stem>` is the name in lower case, `df::app_file_stem()`.

Before it uses the module, the application also sets a UTF-8 C locale - Diffractor sets
`en_US.UTF-8` on Windows, and `LC_CTYPE` to `C.UTF-8` on Linux - which the narrow conversions follow
(`platform::utf8_to_a` among them). Windows shell callers still initialize COM on their worker
threads with `platform::thread_init`; libdesktop-core's WIC and compound-preview entry points own
their own balanced COM initialization and are safe from a fresh thread or an existing STA/MTA.

The platform's messages (`platform::messages`) are English and name no
application. An application translates them with the rest of its text, and may
reword them first; it installs its set with `platform::install_text`.

### Options

| Option | Effect |
|---|---|
| `LIBDESKTOP_SYSTEM` | Builds the module. Off for a consumer unless it sets it |
| `LIBDESKTOP_STORE_PACKAGE` | Windows: defines `WINSTORE`, which leaves out the application-owned update path - `download_and_verify` and `install` - because the Store updates a package itself |

### Dependencies

RapidJSON, header-only, pinned with its archive's SHA-256 in
`cmake/dd-dependencies.json` and fetched at configure time. A consumer that
already carries the same revision sets `FETCHCONTENT_SOURCE_DIR_RAPIDJSON` to
it, so a program holds one copy.

On Windows the sources name most of their import libraries with
`#pragma comment(lib)`; the target links the rest. On Linux it needs threads.
The CRT is the consumer's choice: nothing here sets
`CMAKE_MSVC_RUNTIME_LIBRARY`.

On Windows it also links libdesktop-os, privately: UTF-8/extended-path file
queries, enumeration, open/create and positional streams, atomic
no-overwrite moves, permanent/recycle deletion, folder creation and dialogs,
the escaped INI store, the web client (`connect_to_host`, `send_request`,
`is_online`), clipboard text (`set_clipboard(text)`, `clipboard_text`) and the
known folders are libdesktop-os's, which libdesktop-core shares. This module wraps them in what
belongs to the application: its name as the user agent, its log messages, the
clean-up of copied text, and a web response that is either whole or not one at
all - a transfer that fails or is cut short answers status 0 with no body, and
removes what it downloaded.

The public system wrapper keeps its `file_op_result`, shell collision
policy and settings-location policy. File results preserve a backend
`std::error_code` as well as a readable diagnostic. A caller can disable the
path move's automatic copy fallback, handle only `cross_device_link` itself,
and keep that staged copy behind its own review/commit checks. Permission or
sharing failures must not become an attempted copy.
Ordinary `read` retains the system stack's restrictive sharing contract.
`read_shared` explicitly permits concurrent readers, writers and renames for
adapters that previously allowed them; the token remains readable and reports
its current filename after a rename.
`platform::open_file_lock` does not follow reparse points. On Windows its
default `rename_delete` mode excludes writers and other rename/delete handles;
its `read` mode needs no delete access and permits renames while excluding
writers. Acquire a mutation token before checking pathname snapshots.
`file_sha256(token)` hashes the held object without reopening its pathname or
advancing its cursor. Token move/delete overloads refuse to emulate the
identity guarantee on a backend without a native handle operation; Linux read
tokens pin the object but do not emulate Windows sharing exclusions.
The core wrapper likewise exposes `pf::config_set_path` and
`pf::config_set_registry_root` for adopters whose historical settings live
somewhere other than its default; neither call silently migrates or changes
the caller's selected location.

## The window layer's hooks

The services know nothing of windows, but an application's window layer has
things to tell them. On Windows it uses the backend's semi-public headers,
`libdesktop/platform_win.h` and `libdesktop/platform_win_process.h`, which
include `<windows.h>` and are meant for that layer alone:

- `platform::set_ui_thread()` names the UI thread, first thing in the entry
  point. Services that show system UI - the share sheet, a mail draft - assert
  they are called on it.
- `set_app_wnd()` names the main window, which dialogs and the share sheet are
  parented to.
- `set_ui_thread_wait()` installs the wait `platform::wait_for` uses on the UI
  thread, so the window keeps handling its messages while it waits.
- `set_app_window_enabled_sync()` installs what brings the window's enabled
  state back after a modal system dialog.
- `platform::process_events` is what the application implements for the crash
  and recovery callbacks.

## The Linux backend

The Linux backend covers what a portable core needs in order to run headless.
It has no window, no renderer and no desktop integration.

| Area | Implementation |
|---|---|
| Processes, synchronization, locale, known folders | `platform_linux.cpp` |
| Files, mapping, enumeration, paths | `platform_linux_files.cpp` |
| Copy, move, delete and shell stand-ins | `platform_linux_desktop.cpp` |
| INI settings | `platform_linux_settings.cpp` |

Where the two backends meet:

- Path identity follows the filesystem: case-insensitive on Windows and
  case-sensitive on Linux. Case-folded comparisons remain right for media types,
  tags and other vocabulary, but not for path-keyed containers.
- Times are Windows FILETIME units - 100 ns intervals since 1601-01-01 UTC -
  because `df::date_t` stores them that way in portable code.
- A calendar date out of range is no date on either backend: Linux checks the
  fields as Windows' `SystemTimeToFileTime` does - a year from 1601 to 30827, a
  day the month has, a time of day before 24:00:00, a month or day of 0 read
  as 1 - rather than letting `timegm` roll them into another day.
- Creation time is read from birth-time metadata where the filesystem has it;
  an absent birth time stays absent.
- Directory moves need atomic non-replacing rename support, and fail rather
  than emulate it with an existence check followed by a replacing rename.
- A failed file operation always supplies a nonempty diagnostic, including a
  collision reported by an atomic move.
- A line break in a setting survives both INI stores. A line cannot hold one,
  so each is written as DLE and a letter, and DLE itself as two; a backslash
  could not be the escape, as the values are full of Windows paths, and a value
  written before the escape existed reads back unchanged.

Not implemented on Linux, and answered as failures or absences:

- the clipboard and drag and drop;
- Windows Shell features: associations, shell properties, Explorer thumbnails,
  device eject, the share sheet and taskbar progress. Opening a location in the
  desktop's handler maps to an XDG portal call not yet made;
- update and installer workflows.

A stand-in may support headless tests, but it must not report success for an
operation it did not perform, and a capability query answers false, which keeps
the commands that depend on it out of an application's interface. Some are
product decisions rather than missing ports: `can_recycle` answers false, so an
application presents deletion as permanent rather than implying a recovery that
does not exist here.

## Tests

`tests/system_tests.cpp` is a console program, run on both platforms under the
CTest label `libdesktop-system`. Its tests came with the code from Diffractor's
`test_util.cpp` and use the same assertions (`tests/system_test.h`), so a test
moves between the two suites unchanged. `tests/system_win_tests.cpp` adds the
Windows backend's own - time zones, extended paths, the ANSI code page, the
registry and INI stores, the shell's file operations, number formats, file
names, network locations and a pasted bitmap - from Diffractor's
`test_platform_win.cpp`, whose window and rendering tests stay with it. The
program sets up what an application does first (above), and on Windows its
manifest selects UTF-8 and long paths as an application's does. Diffractor
keeps its own copies, which test the module through the application.

## Known gaps

- The headers rely on the umbrella's includes and order.
- The umbrella puts `std::literals` and `M_PI` in the global namespace.
- The Windows backend still carries `#pragma comment(lib)` directives, several
  for libraries it no longer uses.
