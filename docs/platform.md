# libdesktop::platform

`libdesktop::platform` is what an application asks of the operating system. It
never draws: `render` binds a device to a window, and the window's events go to
`ui`'s frames or to an application that draws for itself. Windows has a Win32
backend; Linux has SDL3 windows over POSIX services.

## Using it

`libdesktop_add_app()` links it ([below](#libdesktop_add_app-and-embedded-resources));
a console tool links `libdesktop::platform` itself. `libdesktop/platform.h`
brings in core and every platform header but `platform/win32.h`. On Linux SDL3
is built statically, at the pin in `cmake/dd-dependencies.json`. On Windows
`LIBDESKTOP_STORE_PACKAGE` defines `WINSTORE` for a Store package, which updates
itself: `download_and_verify`, `install` and crash dumps are left out, settings
stay in the INI file and the cache moves to the package's LocalCache.

## Headers

Each header's comments are its reference; this is where to look.

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

1. It starts the log, then asks `pre_init()`. Windows turns on per-monitor DPI
   awareness, Winsock, COM and OLE; Linux sets the C.UTF-8 character locale.
2. `parse_command_line()` answering true asks for a headless mode: run returns
   what `run_console_mode()` returns, once the worker pool has drained.
   Answering false, an application that returns a `core::mcp::server` from
   `agent_server()` is served to an agent on stdin and stdout
   ([core.md](core.md#serving-an-agent)): run claims both before anything can
   print - what is written to stdout goes to stderr from then on - reads a line
   at a time once `init()` returns, runs each tool on the UI thread, and quits
   at the end of input. A process with no stdin or stdout to serve on fails.
   `is_agent_session()` answers whether one is being served.
3. Linux opens the display, and fails without one. Then `load_settings()` with
   `platform::settings()`, and `init()`, which makes the windows; they are shown
   once it returns.
4. The loop: the application's `main_loop()` where `owns_loop()` is true,
   otherwise run's own, which calls `idle()` and `frame_tick()`, reports
   `folder_changed` and `system_event`, and runs what `run_ui` queued. Input
   that keeps arriving - a drag, a held wheel - holds it on messages for a
   frame's worth of time at most before `idle()` runs, so what the application
   queued for the UI thread is not held for the whole gesture. A request
   to end the session, and in a `ui` application closing the main window, asks
   `can_exit()` first.
5. `exit()`, then `final_exit()` and the shutdown hooks, the renderer's among
   them; the log closes last. Windows tears COM down after the hooks; on Linux
   the windows have closed and SDL has quit before `final_exit`.

The frame delay is 200 ms until `set_frame_delay` sets another. On Windows a
crash writes a minidump and calls `crash()` on the first thread to fault; a
thread that faults while that report is written waits, for a minute at most,
rather than end the process under it. `save_recovery_state()` and
`restart_command_line()` serve Windows' recovery.

`monitor_folders` names the folders whose changes reach `folder_changed`. A
folder watched with its subfolders reports a change in one as that subfolder,
so an application showing the whole tree knows which folder to read again. On
Linux, where inotify watches one directory at a time, a tree is watched up to
4096 folders.

The application supplies its words: `platform::messages` is the platform's text
in English, naming no application, and the application installs its
translation with `platform::install_text`. On Windows it also sets the C locale
the narrow conversions (`utf8_to_a`) follow, and a worker that uses the shell
holds a `platform::thread_init` for COM; image decoding balances its own.

## Names made from the application's name

| What | Name |
|---|---|
| The log | `<stem>.log`, and the run before as `<stem>.previous.log`: beside the executable when that folder is writable, otherwise in app data |
| Settings | `<stem>.ini` in app data; on Windows the key `HKEY_CURRENT_USER\Software\<name>` instead when it already exists, so existing users keep their settings (never in a Store package) |
| App data and cache | `%LOCALAPPDATA%\<name>`; `$XDG_DATA_HOME/<stem>` and `$XDG_CACHE_HOME/<stem>` |
| Temporary files (`temp_file`) | `<stem>_…` |
| The startup scope (Windows) | The mutex `Local\<name>StartupScope` |
| An instance lock (Windows) | The mutex `Global\pf.instance.<user SID>.<lock name>`, whose prefix lets a release find an older one running |

`<stem>` is `core::app_file_stem()`, the name in lower case.

## Windows and input

`create_window(options, events)` makes a native window - the application's, a
dialog, a popup or a child - or null when the system refuses one.
`window_options::client_size` and `minimum_client` are physical pixels, and
closing is the handler's decision. Windows gives a child window a surface of its
own; SDL does not, so on Linux `child_windows()` answers false and a caller
composes child regions into the parent's surface.

`window_options::offscreen` keeps a real drawable window from the desktop and
refuses focus. On Windows it is cloaked - the shell never shows it and the
pointer passes it by - while it is drawn, composed and captured
(`capture_window_surface`) as a window on the desktop is, and it has no taskbar
button or Alt+Tab entry; a dialog or popup of it is cloaked too. On Linux it
stays hidden while offscreen paint passes still run.

Native windows must be created on the registered UI thread. `platform::run`
registers it for an application; a standalone host or test runner calls
`platform::set_ui_thread()` at startup. COM initialisation with `thread_init`
alone does not register a UI thread.

Key codes use `char32_t` throughout the window and modal-filter APIs and carry
Win32 virtual keys on both platforms (`platform::keys::`, letters `U'A'` to
`U'Z'`). They are not typed Unicode text. F1 to F12 and the numeric keypad share
values with lower-case ASCII, so a key code is never case-folded.

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
platform, the shaders into render.

`EMBED` compiles each file into a generated translation unit that registers it
before `main`, and the application reads it back by name on every platform:

```cpp
const std::string_view css = platform::embedded_resource_text("master.css");  // null-terminated
const std::span<const uint8_t> logo = platform::embedded_resource_data("logo.png");
```

### Reproducible application icons

`ICON` is the application's Windows icon, not one of the drawn UI glyphs. The
generated resource tracks the ICO as a build dependency; the Windows backend
uses resource 1 for the window class, small caption icon and large taskbar icon.
On Linux, `EMBED` a 32-bit V4 BMP named `app-icon.bmp`: SDL decodes its explicit
alpha masks and sets the X11 window icon, without an image-decoder dependency.
Wayland takes its icon from a matching desktop launcher instead. The default SDL
application ID is `core::app_file_stem()`, which the launcher's filename must match.

`tools/iconlib.py` is a small, standard-library-only Python vector rasteriser and
PNG/BMP/ICO writer. Application repositories keep a byte-identical vendored copy
beside their own `tools/generate_icon.py`, so a fresh clone has no sibling-source,
font, Pillow or network dependency for regeneration. Update the copies together.
Generators draw each ICO frame at its own size (16, 20, 24, 32, 40, 48, 64, 128 and
256); `--output <file.png> --size <pixels>` exports another size, and `--check`
compares generated bytes without writing. Binary Git attributes preserve the
artwork and the vendored helper exactly.

Run `python tools\test_iconlib.py` for the renderer and file-format checks.
On Windows, `python tools\check_ico.py <file.ico> ...` also decodes every frame
with the native icon loader, without opening a window. Linux platform tests
check V4 alpha decoding and malformed icon rejection without opening a display.

## Files and settings

Paths are core's; `to_file_system_path` makes the form an OS call or a
third-party library takes - UTF-16 with the `\\?\` prefix a long path needs on
Windows, the UTF-8 bytes on Linux - and `to_stream_path` the one `std::fstream`
takes. The file services never guess: a failed query answers
`file_presence::unknown`, not absence; a failed `file_op_result` carries a
diagnostic and the backend's `std::error_code`; `move_file` never turns a
permission or sharing failure into a copy; and an operation given an
`open_file_lock` token acts on the object it pins, or fails where the backend
cannot. Take the token before checking anything about the path.

`storage_probe` in `platform/storage.h` checks the volume backing a folder. A positive answer
requires a local device that reports no seek penalty on Windows or non-rotating storage on Linux.
A remote volume or a device that cannot report its medium does not qualify. Reuse a probe on a
worker for one pass so changing a drive between passes does not leave an old answer in use.

`platform::settings()` is the process's one store. A line break survives it as
DLE and a letter (DLE itself as two), since a backslash could not escape values
full of Windows paths. The crash guards record that a graphics subsystem was
running, so the next launch can fall back.

A phone or camera that a cable connects as a device rather than as a drive - on
Windows a portable device, speaking MTP or PTP - is reached by the id
`portable_devices` gives it, never by a path; a mass-storage device, which is a
drive as well, is left to its path. `portable_device_files` walks only the
folders named at the top of each of its storages, so listing a camera's `DCIM`
does not walk the rest of a phone, and `copy_from_portable_device` copies with
the device opened once, to destinations that must not exist yet, stamping each
with the device's times - which a device keeps in its own wall-clock time. A
file the device will not give up, such as one kept only in the cloud, is
reported and passed over, and the copy stops only when cancelled or when what
it writes cannot be. Each call opens the device and lets it go, so any thread
may ask; a phone still locked, or asking how to connect, is listed but opens to
nothing.

## Tasks

`run_async` tasks may overlap, and a task's exception is logged. A serialised
writer or a long-running consumer takes a `create_serial_executor` worker of its
own; a pool job may wait for one, never for more of the pool. Outside
`platform::run`'s loop - on another thread, or before the loop starts -
`pump_ui_tasks()` runs what `run_ui` queued. When a GUI run ends, queued pool
work is dropped and running jobs get five seconds before they are left to
finish alone; queued UI tasks are discarded, and later ones ignored. A headless
run drains the pool.

## The shell, dialogs and the web

- `try_lock_instance(name)` takes a per-user lock across login sessions without
  waiting; the name is 1 to 128 ASCII letters, digits, `.`, `_` or `-`. Windows
  holds a named mutex without owning it, so even a second attempt on the same
  thread is refused; Linux locks a file in `$XDG_RUNTIME_DIR`.
- Instance request receivers own a separate per-user, per-login-session name on
  their creating UI thread. On Windows a hidden window accepts bounded,
  versioned file-path messages and acknowledges acceptance; the sender waits
  only for its configured timeout, including receiver startup, and grants the
  receiver foreground permission. A handler only accepts and queues work:
  messages can arrive inside modal loops. Destroying the receiver releases
  its window and claim. Linux reports this capability unavailable.
- A window's activation system command restores it only if minimized, keeps
  maximized placement, and requests the foreground. Windows restrictions can
  refuse that request; the taskbar then flashes instead.
- `message_box`, `show_choice`, `show_modal_dialog` and the file pickers are the
  system's own; on Windows a null owner is the application's window, which
  keeps them modal to it. `prompt_for_save_path` without filters offers the
  image formats alone: JPEG, PNG, WebP, JPEG XL and AVIF. A save picker opens on
  the type its suggested name has, so a name ending `.png` is not offered as a
  JPEG. In an agent session none is
  shown, since nobody is there to answer it: each answers as cancelled - a
  message box `none`, a choice 0 - and the properties sheet, the disc burner and
  the share sheet are not opened. Nor does an agent session open another
  program, a folder or a web page - `open`, `run`, `show_in_file_browser`, an
  Open with handler - which would open on the user's desktop, beyond the
  session, until the application allows it with `allow_agent_to_open`, as its
  client grants; each skipped is logged.
- What the system puts up itself while a call into it waits - the shell's
  confirmation, or an error such as a file another program holds - cannot be
  skipped, since the call goes on only once it is answered. `system_prompts`
  reads each - its title, text and buttons, as a screen reader reads them -
  `system_prompt_shown` says cheaply whether one shows, and
  `answer_system_prompt` presses a button by its text or cancels it, so an
  agent session can show it to its client like the application's own dialogs.
  On Linux the system puts up none.
- `delete_items` takes a `delete_mode`: `recycle` lets Windows confirm before it
  destroys what it cannot recycle; `recycle_where_possible`, for a delete the
  application has already confirmed as permanent because `can_recycle` said so,
  recycles what it can and destroys the rest without asking a second time; and
  `permanent` destroys without asking.
- A picture put on the clipboard on Windows is offered as a bitmap and a
  device-independent bitmap, made when a target asks, and - when its
  `clipboard_image` offers one - in the registered `PNG` format browsers, Office
  and the Snipping Tool use, which keeps the alpha a bitmap's target may drop.
  Windows defines no clipboard format for more than eight bits a channel or for
  HDR. A paste or drop takes a PNG it is offered before the bitmap, and saves it
  as it came, through its IEND chunk.
- `connect_to_host` and `send_request` are the synchronous client: the
  application's name is the user agent, the system's timeouts apply, and a
  transfer that fails or is cut short is no response at all - status 0, no body,
  and what it downloaded removed.

## Audio, movies and images

`sound_buffer` and `audio_stream` are a mixer that plays through the default
endpoint (`create_audio_output`) on a thread of its own, so a new system needs
the endpoint alone. `create_movie_writer` writes H.264 and AAC in MP4.
`load_image_file` and `load_image_memory` decode through the system, with WIC
on Windows; codec libraries are [libfiles'](https://github.com/ZacWalk/libfiles),
not platform's.

## The Windows backend

`src/platform/win/` is written over a private service layer, `os.h` - UTF-8 and
extended-path files, dialogs, the escaped INI store, WinINet and WinHTTP,
clipboard text, known folders and the WASAPI endpoint - which asks nothing of an
application ([AGENTS.md](../AGENTS.md#where-code-goes)).

## The Linux backend

SDL3 windows over POSIX services, in `src/platform/linux/`: the windows in
`window.cpp`, and the stand-ins for what Linux lacks mostly in `desktop.cpp`.
Creation time comes from birth-time metadata where the file system has it, and
an absent one stays absent; a directory move needs an atomic no-replace rename
rather than check and then rename; a read token pins its file without Windows'
sharing exclusions. Dates are core's FILETIME units here too, and path identity
and dates follow core's rules ([paths](core.md#paths), [dates](core.md#dates)).

Not implemented on Linux, and answered as failures or absences:

- file pickers, `show_modal_dialog`, the file and image clipboard, and drag and
  drop either way - clipboard text works while `run` runs, through SDL;
- the web client and `is_online`; image decoding and compound previews;
- the audio endpoint, sounds and streams, and movie writing;
- the tray, the spell checker and instance signals;
- portable devices, which MTP through libmtp or a GVFS mount would reach;
- the Windows shell: opening a file or location in the desktop's handler
  (`open`) and starting a program by command line (`run`); associations, shell
  properties and tags, Explorer thumbnails, sharing and mail, taskbar progress,
  eject, wallpaper, burning, printing, updates and installers.

The file pickers and `open` correspond to XDG desktop portals, which the backend
does not call; shell properties and Explorer thumbnails have no Linux
counterpart. A stand-in never reports success for what it did not do, and a
capability query answers false, which keeps the commands that need it out of an
application's interface. `can_recycle` answering false is a product decision,
not a missing port: deletion is presented as permanent rather than implying a
recovery.

## Tests

`libdesktop_platform_tests`, in `tests/platform/`, under the CTest label
`libdesktop-platform`, links platform alone and holds the resource lock
`libdesktop-clipboard`. It covers the services - the names made from the
application's and the stand-ins that answer unavailable among them - and, on
Windows, web requests against a scripted server on 127.0.0.1 (`loopback_http`);
`tests/platform/win/` adds the Windows backend's own, under a manifest like an
application's (`tests/tests.manifest`) - among them a prompt of the system's class,
put up cloaked and off the screen, read and answered as an agent session answers
one. On Windows `libdesktop-platform-smoke`,
under the same label, is a program declared with `libdesktop_add_app` that
reads back its `EMBED` file, and `libdesktop-platform-ui-pump` runs it through
`platform::run`, where a console mode's worker queues UI work and runs it with
`pump_ui_tasks` while the UI thread waits for it. `libdesktop-platform-agent`
serves an agent session from it over real pipes: `server/discover`, a tool run on
the UI thread and one answered from a worker, nothing but the protocol on stdout,
no other program opened until the application allows it, and the end of input
ending the process.

## Known gaps

- Much of the desktop integration is missing on Linux: the stand-ins
  [above](#the-linux-backend) answer as failures or absences.
- Menus and completion popups are untried on a desktop Wayland compositor,
  which places popups itself and may give a tooltip popup no input.
- The window classes are named `LIBDESKTOP_*` whichever application registers
  them, and the Windows backend's `#pragma comment(lib)` directives name
  several libraries it does not use.
