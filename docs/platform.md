# libdesktop::platform

`libdesktop::platform` is what an application asks of the operating system: its
lifecycle and event loop, native windows and their input, files and settings,
the clipboard, dialogs and the shell, child processes, tasks, the web client,
audio and movies, image decoding and the data files compiled into the
executable. It never draws: `render` binds a device to a window, and the
window's events go to `ui`'s frames or to an application that draws for itself.
Windows has a Win32 backend; Linux has SDL3 windows over POSIX services.

## Using it

`libdesktop_add_app()` links it ([below](#libdesktop_add_app-and-embedded-resources));
a console tool links `libdesktop::platform` itself. `libdesktop/platform.h`
brings in core and every platform header but `platform/win32.h`. On Linux SDL3
is built statically, at the pin in `cmake/dd-dependencies.json`. On Windows
`LIBDESKTOP_STORE_PACKAGE` defines `WINSTORE` for a Store package, which updates
itself: `download_and_verify`, `install` and crash dumps are left out, settings
stay in the INI file and the cache moves to the package's LocalCache.

## Headers

| Header | What it holds |
|---|---|
| `platform/app.h` | `platform::app`, `run`, `quit`, `pump_events`, the frame delay, `run_ui`, modal loops, shutdown hooks |
| `platform/tasks.h` | The worker pool (`run_async`), serial executors, pumping UI tasks |
| `platform/process.h` | Child processes, by line or as binary streams; `find_executable`, `quote_command_arg`; the console |
| `platform/resources.h` | The data files `EMBED` compiled into the executable |
| `platform/window.h` | `create_window`, `window_options`, `window_events`, `window`, placement and work areas |
| `platform/input.h` | Key codes (`keys::`), `key_state`, keyboard accelerators and their text, cursors, the wheel |
| `platform/services.h` | Files and file operations, known folders, settings, the clipboard, drag and drop, crash guards, threads and waits, the synchronous web client, local time, the shell, and the messages the platform shows |
| `platform/file.h` | Atomic replacement, canonical and contained paths, `buffered_file`, whole files as blobs or JSON |
| `platform/dialogs.h` | Message boxes, choices and modal field dialogs |
| `platform/shell.h` | The tray, single-instance locks and signals, the spell checker, file drags |
| `platform/media.h` | Movie writing, the audio endpoint, sounds and streams, playback devices |
| `platform/image.h` | Image decoding, trusted or bounded, probes and compound-document previews |
| `platform/web.h`, `platform/text.h` | The asynchronous HTTP session; URLs, code pages and transcoding to UTF-8 |
| `platform/environment.h` | The platform identity `environment()` reports, and its packed form |
| `platform/win32.h` | Semi-public, including `<windows.h>`: the main window's `HWND`, conversions for shell APIs, COM helpers and test seams, for an application's own Windows code |

## The application

The application owns `main` and returns `platform::run(app, argc, argv)`, which
drives a `platform::app` on the UI thread:

1. It names the UI thread and starts the log; `pre_init()` answering false ends
   the process. Windows turns on per-monitor DPI awareness, Winsock, COM and
   OLE; Linux sets the C.UTF-8 character locale.
2. `parse_command_line()` answering true asks for a headless mode: run returns
   what `run_console_mode()` returns, once the worker pool has drained.
3. Linux opens the display, and fails without one. Then `load_settings()` with
   `platform::settings()`, and `init()`, which makes the windows: answering
   false ends the process with the code given to `quit`, if init called it. The
   windows are shown once init returns, maximized if they were.
4. The loop: `main_loop()` for an application whose `owns_loop()` is true, a
   game or an emulator that pumps with `pump_events()`; otherwise run's own,
   which calls `idle()` after each batch of events and `frame_tick()` each frame
   delay, reports `folder_changed` for `monitor_folders` and `system_event` for
   power, session, theme, display and device changes and a lost graphics
   device, and runs what `run_ui` queued. `quit(code)` ends it after the current
   event; a request to end the session, and in a `ui` application closing the
   main window, asks `can_exit()` first.
5. `exit()`, then `final_exit()` and the shutdown hooks, the renderer's among
   them; the log closes last. Windows tears COM down after the hooks; on Linux
   the windows have closed and SDL has quit before `final_exit`.

The frame delay is 200 ms unless init sets one with `set_frame_delay(ms)`; 0
stops the ticks. On Windows a crash writes a minidump and calls `crash()`, and
`save_recovery_state()` and `restart_command_line()` serve Windows' recovery.

Besides `core::set_app_name`, the application supplies its words:
`platform::messages` - the errors the platform shows, the key names accelerator
text is made of, the edit menu, file-operation errors and image-format names -
is English and names no application, and the application installs its
translated copy with `platform::install_text`. On Windows it also sets the C
locale the narrow conversions (`utf8_to_a`) follow - Diffractor sets
`en_US.UTF-8` - and a worker that uses the shell holds a
`platform::thread_init` for COM; image decoding balances its own.

## Names made from the application's name

| What | Name |
|---|---|
| The log | `<stem>.log`, and the run before as `<stem>.previous.log`: beside the executable when that folder is writable, otherwise in app data |
| Settings | `<stem>.ini` in app data; on Windows the key `HKEY_CURRENT_USER\Software\<name>` instead when it already exists, so earlier releases' settings are kept (never in a Store package) |
| App data and cache | `%LOCALAPPDATA%\<name>`; `$XDG_DATA_HOME/<stem>` and `$XDG_CACHE_HOME/<stem>` |
| Temporary and staging files | `<stem>_…` |
| The startup scope (Windows) | The mutex `Local\<name>StartupScope` |
| An instance lock (Windows) | The mutex `Global\pf.instance.<user SID>.<lock name>`, the prefix earlier releases locked, so a new release still finds an old one running |

`<stem>` is `core::app_file_stem()`, the name in lower case.

## Windows and input

`create_window(options, events)` makes a native window - the application's, a
dialog, a popup that never activates, or a child - or null when the system
refuses one. `window_options::client_size` and `minimum_client` are physical
client pixels; `read_placement` gives back the bounds a window was saved at.
`window_events` reports on the UI thread, in client pixels. Where the
application draws the caption, `hit_test` says what a point is, and a point it
calls `maximize_button` is reported to the system as the maximize button, which
is what Windows 11 offers snap layouts over. Closing is the handler's decision:
`close_requested` closes nothing until it calls `window::destroy`. Windows gives
a child window a surface of its own; SDL does not, so on Linux
`child_windows()` answers false and a caller composes child regions into the
parent's surface.

Key codes are Win32 virtual keys on both platforms (`platform::keys::`, letters
`'A'` to `'Z'`). F1 to F12 and the numeric keypad share values with lower-case
ASCII, so a key code is never case-folded. `key_state::repeat` marks a repeated
key, `format_keyboard_accelerator` makes the text a menu shows for an
accelerator, in the installed messages' words, and `keys::normalize_numpad`
lets a digit accelerator answer the keypad.

## libdesktop_add_app and embedded resources

```cmake
libdesktop_add_app(<target> [CONSOLE]
    SOURCES <files> [ICON <file.ico>] [DESCRIPTION "<text>"] [OUTPUT_NAME <name>]
    [VERSION <a.b.c>] [COMPANY "<text>"] [MANIFEST <file> | NONE] [EMBED <files>])
```

It makes a Windows-subsystem executable whose entry point is `main` (`CONSOLE`
keeps the console), generates its version resource, its manifest and its icon
(icon 1, which the shell and the windows show) and links `libdesktop::platform`.
The manifest selects per-monitor-V2 DPI awareness, UTF-8 as the ANSI code page,
long paths, the segment heap and Common Controls 6, which task dialogs need:
without it `show_choice` answers 0. An application needs no resource script for
any of it, and includes none of libdesktop's: the cursors are compiled into
platform, the shaders and the drop shadow into render.

`EMBED` compiles each file into a generated translation unit that registers it
before `main`, and the application reads it back by name on every platform:

```cpp
const std::string_view css = platform::embedded_resource_text("master.css");  // null-terminated
const std::span<const uint8_t> logo = platform::embedded_resource_data("logo.png");
```

## Files and settings

Paths are core's; `to_file_system_path` makes the form an OS call or a
third-party library takes - UTF-16 with the `\\?\` prefix a long path needs on
Windows, the UTF-8 bytes on Linux - and `to_stream_path` the one `std::fstream`
takes.

- A failed query is not proof of absence: `file_attributes` answers
  `file_presence::unknown` for a denied query, an offline volume or a dropped
  share, and `not_found` only when the file is known to be gone.
- A failed `file_op_result` carries a diagnostic, never empty, and the backend's
  `std::error_code`. `move_file(…, allow_copy = false)` leaves a cross-volume
  copy to the caller, to stage behind its own review; a permission or sharing
  failure never becomes a copy.
- `file_open_mode::read` shares restrictively, `read_shared` lets other readers,
  writers and renames proceed, and `create_exclusive` admits one live writer.
- `open_file_lock` pins a file without following reparse points. Take it before
  checking anything about the path: `file_sha256`, `move_file` and
  `delete_file` given the token act on the object it holds, and fail where the
  backend cannot rather than emulate it. `create_atomic_file_writer` stages
  beside the destination and commits in one step.

`platform::settings()` is the process's one store, synchronised. A line break
survives it as DLE and a letter (DLE itself as two), since a backslash could not
escape values full of Windows paths. `create_ini_file_settings(folder)` opens a
store where an application's settings already live, and the crash guards record
that a graphics subsystem was running, so the next launch can fall back.

## Tasks and child processes

`run_async(task)` starts four workers on first use - from init, a headless mode
or a console tool alike - and its tasks may overlap. `run_ui(task)` appends one
FIFO queue. On Windows, once `platform::run` has created its message-only task
window, the UI thread runs one queued task for each token posted to that window;
any other thread, and every caller before `platform::run` creates that window,
runs queued tasks with `pump_ui_tasks()`. A serialised writer or a long-running
consumer takes a `create_serial_executor` worker of its own (see the header for
its post and stop rules); a pool job may wait for one, never for more of the
pool. A task's exception is logged. When a GUI run ends, queued pool work is
dropped and running jobs get five seconds before they are left to finish alone;
queued UI tasks are discarded, and later ones ignored. A headless run drains the
pool.

`spawn_child_process` gives a tool its own pipes, reads its output and errors on
threads of their own and delivers whole lines on the UI thread, none after the
process object is gone; `process_spawn` and its read and write calls are binary
streams the caller drains. `find_executable` searches `PATH` (and `PATHEXT` on
Windows), never the current directory, and `quote_command_arg` quotes by the
Windows rules, so an argument cannot be split or injected.

## The shell, dialogs and the web

- `try_lock_instance(name)` takes a per-user lock across login sessions without
  waiting, held while the returned object lives; the name is 1 to 128 ASCII
  letters, digits, `.`, `_` or `-`. Windows holds a named mutex without owning
  it, so even a second attempt on the same thread is refused; Linux locks a file
  in `$XDG_RUNTIME_DIR`. `try_lock_named_instance` takes an earlier release's
  mutex name as it is, `signal_existing_instance` tells the running instance
  another launch happened, and `claim_startup_scope`, held until a launch
  reaches the user, tells a concurrent launch from one repeated after a failure.
- `message_box`, `show_choice`, `show_modal_dialog` and the file pickers are the
  system's own; a null owner is the application's window, which keeps them
  modal to it. The tray and the spell checker answer null where there is none.
- `connect_to_host` and `send_request` are the synchronous client: the
  application's name is the user agent, the system's timeouts apply, and a
  transfer that fails or is cut short is no response at all - status 0, no body,
  and what it downloaded removed. `create_async_http_session` streams a GET to
  callbacks on a worker thread.

## Audio, movies and images

- `create_audio_output(device)` is an endpoint opened in its own mix format.
  Diffractor's player drives it directly: its clock times the video, `reset`
  empties the ring, `write_silence` keeps a clip's tail from replaying.
  `sound_buffer` and `audio_stream` are a mixer that plays through the default
  endpoint on a thread of its own, so a new system needs the endpoint alone.
- `can_write_movies()` probes the system's encoders, about half a second the
  first time, so ask on a worker; `create_movie_writer` writes H.264 and AAC in
  MP4 and leaves no partial file behind.
- `load_image_file` and `load_image_memory` decode what the application trusts;
  given `image_load_options` they also refuse a source over 100,000 pixels on a
  side, keep to the budgets, cancel, report progress and say why they failed.
  Native codecs are libfiles'.

## The Windows backend

`src/platform/win/` is written over a private service layer, `os.h` - UTF-8 and
extended-path files, dialogs, the escaped INI store, WinINet and WinHTTP,
clipboard text, known folders and the WASAPI endpoint. Platform's Windows SDK
code is there and in `platform/win32.h` alone, and `UNICODE` is the backend's
private definition, so a consumer keeps its own character set.

## The Linux backend

SDL3 windows over POSIX services, in `src/platform/linux/`, with the stand-ins
in `desktop.cpp` and `image.cpp`. Creation time comes from birth-time metadata
where the file system has it, and an absent one stays absent; a directory move
needs an atomic no-replace rename rather than check and then rename; a read
token pins its file without Windows' sharing exclusions. Path identity and dates
follow [core's rules](core.md#paths).

Not implemented on Linux, and answered as failures or absences:

- file pickers, `show_modal_dialog`, the file and image clipboard, and drag and
  drop either way - clipboard text works while `run` runs, through SDL;
- the web client and `is_online`; image decoding and compound previews;
- the audio endpoint, sounds and streams, and movie writing;
- the tray, the spell checker and instance signals;
- the Windows shell: opening a file or location in the desktop's handler
  (`open`, an XDG portal call not yet made) and starting a program by command
  line (`run`); associations, shell properties and tags, Explorer thumbnails,
  sharing and mail, taskbar progress, eject, wallpaper, burning, printing,
  updates and installers.

A stand-in never reports success for what it did not do, and a capability query
answers false, which keeps the commands that need it out of an application's
interface. `can_recycle` answering false is a product decision, not a missing
port: deletion is presented as permanent rather than implying a recovery.

## Tests

`libdesktop_platform_tests`, in `tests/platform/`, under the CTest label
`libdesktop-platform`, links platform alone and shares the resource lock
`libdesktop-clipboard` with the ui suite, since both use the real clipboard. It
covers tasks, processes, keys, files, settings, the names made from the
application's, local dates, text encodings, web requests against a scripted
server on 127.0.0.1 (`loopback_http`), sounds, images, instance locks and the
services that answer unavailable; `tests/platform/win/` adds the Windows
backend's own, under a manifest like an application's (`tests/tests.manifest`).
`libdesktop-platform-smoke`, under the same label, is a program declared with
`libdesktop_add_app` that reads back its `EMBED` file; `libdesktop-platform-ui-pump`
runs it through `platform::run`, where a console mode's worker queues UI work
and runs it with `pump_ui_tasks` while the UI thread waits for it.

## Known gaps

- The Linux stand-ins above ([todo](todo.md#platform)).
- The Linux window layer has run under SDL's offscreen driver; menus and
  completion popups have not been tried on a desktop Wayland compositor, which
  places popups itself and may give a tooltip popup no input.
- The window classes are named `LIBDESKTOP_*` whichever application registers
  them, and the Windows backend's `#pragma comment(lib)` directives name
  several libraries it no longer uses.
- The headers rely on the umbrella's includes and order.
