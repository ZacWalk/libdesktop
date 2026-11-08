// This file is part of libdesktop, a C++20 platform layer for desktop applications
// Copyright 2026  Zac Walker
//
// This program is free software; you can redistribute it and / or modify it
// under the terms of the LGPL License either version 2.1 or later.
// License details are available at https://www.gnu.org/licenses/lgpl-2.1.html
// This program is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY

// Purpose: Windows process setup and the UI message loop. Window objects report native events;
// this file owns the application's lifetime, waits, timers and OS-wide notifications.

#include "libdesktop/platform.h"

namespace platform::detail
{
	void set_task_shutdown_bounded(bool bounded);
}
#include "libdesktop/platform/win32.h"

#include <Dbt.h>
#include <ShlObj.h>
#include <ShObjIdl.h>
#include <wtsapi32.h>
#include <VersionHelpers.h>

#include <io.h>
#include <winsock2.h>

#include <condition_variable>
#include <deque>

#ifndef GET_X_LPARAM
#define GET_X_LPARAM(lp) static_cast<int>(static_cast<short>(LOWORD(lp)))
#endif
#ifndef GET_Y_LPARAM
#define GET_Y_LPARAM(lp) static_cast<int>(static_cast<short>(HIWORD(lp)))
#endif

std::weak_ptr<platform::app> g_platform_app;

namespace
{
	constexpr UINT wm_platform_run_ui = WM_APP + 0x3d3;
	constexpr int default_frame_delay_ms = 200;
	constexpr wchar_t ui_task_window_class_name[] = L"LIBDESKTOP_UI_TASK";
	constexpr DWORD create_waitable_timer_high_resolution = 0x00000002;
	constexpr size_t max_wait_handles = MAXIMUM_WAIT_OBJECTS - 1;

	struct folder_watch
	{
		HANDLE handle = nullptr;
		core::folder_path path;
	};

	struct queued_ui_task
	{
		std::function<void()> action;
		core::thread_event* completed = nullptr;
	};

	class app_state;
	LRESULT CALLBACK ui_task_window_proc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam);
	HWND create_ui_task_window(app_state* state);
	void run_next_ui_task(app_state& state);

	class app_state
	{
	public:
		std::shared_ptr<platform::app> app;
		std::vector<HANDLE> event_handles;
		std::vector<std::function<void()>> event_actions;
		std::vector<folder_watch> folders;
		std::vector<std::function<void()>> shutdown_hooks;
		HANDLE timer = nullptr;
		HWND activity_window = nullptr;
		HWND task_window = nullptr;
		int frame_delay = 0;
		// Set once the application chooses a delay, so the loop's default never overrides that choice.
		bool frame_delay_chosen = false;
		DWORD ui_thread = 0;
		core::thread_event idle_event{false, false};
		bool screen_saver_enabled = true;
		bool closed = false;
		ComPtr<ITaskbarList3> taskbar;
		bool taskbar_unavailable = false;
		TBPFLAG taskbar_state = TBPF_NOPROGRESS;
		int64_t taskbar_done = -1;
		int64_t taskbar_total = -1;

		explicit app_state(std::shared_ptr<platform::app> value) : app(std::move(value)), ui_thread(GetCurrentThreadId())
		{
			timer = CreateWaitableTimerExW(nullptr, nullptr, create_waitable_timer_high_resolution,
			                               TIMER_MODIFY_STATE | SYNCHRONIZE);
			if (!timer) timer = CreateWaitableTimerW(nullptr, FALSE, nullptr);
			task_window = create_ui_task_window(this);
			MSG message{};
			PeekMessageW(&message, nullptr, WM_USER, WM_USER, PM_NOREMOVE);
		}

		~app_state()
		{
			close();
		}

		void close()
		{
			if (closed) return;
			closed = true;
			if (task_window)
			{
				// A token carries no task; the queued tasks are let go with the queue.
				const auto hwnd = task_window;
				const auto discard_tokens = [hwnd]
				{
					MSG message{};
					while (PeekMessageW(&message, hwnd, wm_platform_run_ui, wm_platform_run_ui, PM_REMOVE)) {}
				};
				discard_tokens();
				DestroyWindow(hwnd);
				discard_tokens();
				task_window = nullptr;
			}
			if (timer)
			{
				CancelWaitableTimer(timer);
				CloseHandle(timer);
				timer = nullptr;
			}
			if (activity_window)
			{
				KillTimer(activity_window, platform_activity_timer_id);
				activity_window = nullptr;
			}
			for (const auto& folder : folders)
			{
				if (folder.handle && folder.handle != INVALID_HANDLE_VALUE) FindCloseChangeNotification(folder.handle);
			}
			folders.clear();
			if (!screen_saver_enabled) SetThreadExecutionState(ES_CONTINUOUS);
		}

		void ensure_timer()
		{
			if (!timer) timer = CreateWaitableTimerW(nullptr, FALSE, nullptr);
		}

		void set_delay(const int milliseconds)
		{
			const auto wanted = std::max(0, milliseconds);
			if (wanted == frame_delay) return;
			frame_delay = wanted;
			ensure_timer();
			if (!timer) return;
			if (frame_delay == 0)
			{
				CancelWaitableTimer(timer);
				return;
			}
			LARGE_INTEGER due{};
			due.QuadPart = 0;
			SetWaitableTimer(timer, &due, frame_delay, nullptr, nullptr, FALSE);
		}

		void dispatch_ui_task(queued_ui_task task)
		{
			if (closed)
			{
				if (task.completed) task.completed->set();
				return;
			}
			try
			{
				if (task.action) task.action();
			}
			catch (...)
			{
				if (task.completed) task.completed->set();
				throw;
			}
			if (task.completed) task.completed->set();
		}

		void frame_tick_if_due()
		{
			if (!timer || frame_delay <= 0 || WaitForSingleObject(timer, 0) != WAIT_OBJECT_0) return;
			if (app && !closed) app->frame_tick();
		}

		void ensure_activity_timer()
		{
			const auto hwnd = app_wnd();
			if (hwnd == activity_window) return;
			if (activity_window) KillTimer(activity_window, platform_activity_timer_id);
			activity_window = nullptr;
			if (hwnd && SetTimer(hwnd, platform_activity_timer_id, 200, nullptr)) activity_window = hwnd;
		}

		void rebuild_events()
		{
			ensure_activity_timer();
			event_handles.clear();
			event_actions.clear();
			if (timer && frame_delay > 0)
			{
				event_handles.push_back(timer);
				event_actions.push_back([this] { if (app) app->frame_tick(); });
			}
			event_handles.push_back(static_cast<HANDLE>(idle_event._h));
			event_actions.push_back([this] { if (app) app->idle(); });
			for (auto& folder : folders)
			{
				if (event_handles.size() >= max_wait_handles) break;
				event_handles.push_back(folder.handle);
				event_actions.push_back([this, handle = folder.handle, path = folder.path]
				{
					if (app) app->folder_changed(path);
					FindNextChangeNotification(handle);
				});
			}
		}

		void monitor_folders(const std::vector<core::folder_path>& values)
		{
			for (const auto& folder : folders)
			{
				if (folder.handle && folder.handle != INVALID_HANDLE_VALUE) FindCloseChangeNotification(folder.handle);
			}
			folders.clear();
			constexpr size_t max_folders = MAXIMUM_WAIT_OBJECTS - 4;
			constexpr DWORD filter = FILE_NOTIFY_CHANGE_ATTRIBUTES | FILE_NOTIFY_CHANGE_LAST_WRITE |
				FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_DIR_NAME;
			for (const auto& path : values)
			{
				if (folders.size() >= max_folders) break;
				const auto wide = platform::to_file_system_path(path);
				const auto handle = FindFirstChangeNotificationW(wide.c_str(), FALSE, filter);
				if (handle != INVALID_HANDLE_VALUE && handle) folders.push_back({handle, path});
			}
			rebuild_events();
		}

		void enable_screen_saver(const bool enable)
		{
			if (screen_saver_enabled == enable) return;
			const auto state = enable ? ES_CONTINUOUS : (ES_CONTINUOUS | ES_SYSTEM_REQUIRED | ES_DISPLAY_REQUIRED);
			if (SetThreadExecutionState(state) != 0) screen_saver_enabled = enable;
		}

		ITaskbarList3* get_taskbar()
		{
			if (taskbar || taskbar_unavailable) return taskbar.Get();
			ComPtr<ITaskbarList3> candidate;
			if (SUCCEEDED(CoCreateInstance(CLSID_TaskbarList, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&candidate))) &&
				SUCCEEDED(candidate->HrInit())) taskbar = candidate;
			else taskbar_unavailable = true;
			return taskbar.Get();
		}

		void show_progress(const int64_t done, const int64_t total)
		{
			const auto hwnd = app_wnd();
			auto* list = get_taskbar();
			if (!list || !hwnd) return;
			const auto state = total > 0 ? TBPF_NORMAL : TBPF_INDETERMINATE;
			if (state != taskbar_state)
			{
				list->SetProgressState(hwnd, state);
				taskbar_state = state;
				taskbar_done = -1;
			}
			if (state == TBPF_NORMAL)
			{
				const auto value = std::clamp<int64_t>(done, 0, total);
				if (value != taskbar_done || total != taskbar_total)
				{
					list->SetProgressValue(hwnd, static_cast<ULONGLONG>(value), static_cast<ULONGLONG>(total));
					taskbar_done = value;
					taskbar_total = total;
				}
			}
		}

		void end_progress()
		{
			if (taskbar_state == TBPF_NOPROGRESS) return;
			taskbar_state = TBPF_NOPROGRESS;
			taskbar_done = -1;
			taskbar_total = -1;
			const auto hwnd = app_wnd();
			if (!hwnd) return;
			if (taskbar) taskbar->SetProgressState(hwnd, TBPF_NOPROGRESS);
			DWORD process = 0;
			GetWindowThreadProcessId(GetForegroundWindow(), &process);
			if (process != GetCurrentProcessId())
			{
				FLASHWINFO flash{sizeof(flash), hwnd, FLASHW_TRAY | FLASHW_TIMERNOFG, 0, 0};
				FlashWindowEx(&flash);
			}
		}
	};

	HWND create_ui_task_window(app_state* state)
	{
		const auto instance = get_resource_instance ? get_resource_instance : GetModuleHandleW(nullptr);
		WNDCLASSEXW klass{sizeof(klass)};
		klass.lpfnWndProc = ui_task_window_proc;
		klass.hInstance = instance;
		klass.lpszClassName = ui_task_window_class_name;
		if (!GetClassInfoExW(instance, ui_task_window_class_name, &klass))
		{
			klass = WNDCLASSEXW{sizeof(klass)};
			klass.lpfnWndProc = ui_task_window_proc;
			klass.hInstance = instance;
			klass.lpszClassName = ui_task_window_class_name;
			if (!RegisterClassExW(&klass) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) return nullptr;
		}
		return CreateWindowExW(0, ui_task_window_class_name, L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr,
		                       instance, state);
	}

	LRESULT CALLBACK ui_task_window_proc(const HWND hwnd, const UINT message, const WPARAM wparam, const LPARAM lparam)
	{
		if (message == WM_NCCREATE)
		{
			const auto* create = reinterpret_cast<const CREATESTRUCTW*>(lparam);
			SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(create->lpCreateParams));
			return TRUE;
		}
		auto* const state = reinterpret_cast<app_state*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
		if (message == wm_platform_run_ui)
		{
			if (state) run_next_ui_task(*state);
			return 0;
		}
		if (message == WM_NCDESTROY) SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
		return DefWindowProcW(hwnd, message, wparam, lparam);
	}

	app_state* current_state = nullptr;
	std::vector<std::function<void()>> pending_shutdown_hooks;
	// Every task run_ui queues, oldest first, on every thread. The UI thread runs one for each token
	// posted to its task window, so the tasks keep the order, and the place among other messages,
	// that posting each one gave them, and a modal loop runs them as it dispatches any message.
	// Before that window exists, and for a headless caller on another thread, pump_ui_tasks runs them.
	std::mutex ui_task_mutex;
	std::condition_variable ui_task_ready;
	std::deque<queued_ui_task> ui_tasks;
	std::atomic_bool ui_accepting = true;
	LPARAM last_mouse_move = 0;
	int quit_code = 0;

	bool pre_translate(MSG& message)
	{
		if (message.message == WM_MOUSEMOVE)
		{
			if (last_mouse_move != 0 && last_mouse_move != message.lParam) platform::ticks_since_last_user_action = 0;
			last_mouse_move = message.lParam;
		}
		else if (message.message >= WM_LBUTTONDOWN && message.message <= WM_XBUTTONDBLCLK)
		{
			platform::ticks_since_last_user_action = 0;
		}
		else if ((message.message == WM_KEYDOWN || message.message == WM_SYSKEYDOWN) && message.wParam != VK_LEFT &&
			message.wParam != VK_RIGHT)
		{
			platform::ticks_since_last_user_action = 0;
		}
		return false;
	}
	bool post_ui_task(std::function<void()> action, core::thread_event* completed = nullptr)
	{
		if (!action) return false;
		{
			std::lock_guard lock(ui_task_mutex);
			if (!ui_accepting || (current_state && (current_state->closed || !current_state->task_window)))
				return false;
			ui_tasks.push_back({std::move(action), completed});
			if (current_state && PostMessageW(current_state->task_window, wm_platform_run_ui, 0, 0) == FALSE)
			{
				ui_tasks.pop_back();
				return false;
			}
		}
		ui_task_ready.notify_one();
		return true;
	}

	// One token, one task: the oldest still queued, which a headless caller may already have run.
	void run_next_ui_task(app_state& state)
	{
		queued_ui_task task;
		{
			std::lock_guard lock(ui_task_mutex);
			if (ui_tasks.empty()) return;
			task = std::move(ui_tasks.front());
			ui_tasks.pop_front();
		}
		state.dispatch_ui_task(std::move(task));
	}

	bool dispatch_platform_message(MSG& message)
	{
		if (message.message != wm_platform_run_ui) return false;
		if (current_state && message.hwnd == current_state->task_window) run_next_ui_task(*current_state);
		return true;
	}

	void dispatch_message(MSG& message)
	{
		const auto platform_window = platform_window_from_native(message.hwnd);
		const auto is_key_down = message.message == WM_KEYDOWN || message.message == WM_SYSKEYDOWN;
		if (!platform_window || !is_key_down) TranslateMessage(&message);
		DispatchMessageW(&message);
	}

	void pump_messages(const std::function<bool(MSG&)>& filter)
	{
		MSG message{};
		while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE))
		{
			if (message.message == WM_QUIT)
			{
				quit_code = static_cast<int>(message.wParam);
				platform::is_closing = true;
				if (current_state) current_state->closed = true;
				continue;
			}
			if (dispatch_platform_message(message)) continue;
			if (filter && filter(message)) continue;
			dispatch_message(message);
		}
	}

	void dispatch_app_event(const size_t index)
	{
		if (!current_state || index >= current_state->event_actions.size()) return;
		const auto action = current_state->event_actions[index];
		action();
	}

	uint32_t wait_for_events(const std::vector<std::reference_wrapper<core::thread_event>>& caller,
	                         const bool wait_all, const uint32_t timeout_ms,
	                         const std::function<bool(MSG&)>& filter)
	{
		constexpr auto wait_timeout = WAIT_TIMEOUT - WAIT_OBJECT_0;
		if (!current_state) return wait_timeout;
		const auto caller_count = caller.size();
		const auto deadline = timeout_ms && timeout_ms != UINT32_MAX ? GetTickCount64() + timeout_ms : UINT64_MAX;
		std::vector<HANDLE> handles;
		while (!platform::is_closing)
		{
			current_state->rebuild_events();
			handles.clear();
			handles.reserve(max_wait_handles);
			for (const auto& event : caller) handles.push_back(static_cast<HANDLE>(event.get()._h));
			const auto room = caller_count < max_wait_handles ? max_wait_handles - caller_count : 0;
			const auto appended = std::min(room, current_state->event_handles.size());
			handles.insert(handles.end(), current_state->event_handles.begin(), current_state->event_handles.begin() + appended);

			DWORD wait_ms = INFINITE;
			if (deadline != UINT64_MAX)
			{
				const auto now = GetTickCount64();
				if (now >= deadline) return wait_timeout;
				wait_ms = static_cast<DWORD>(std::min<uint64_t>(deadline - now, INFINITE - 1ull));
			}

			const auto result = MsgWaitForMultipleObjectsEx(static_cast<DWORD>(handles.size()), handles.data(), wait_ms,
			                                                QS_ALLINPUT, MWMO_ALERTABLE);
			if (result == WAIT_TIMEOUT || result == WAIT_FAILED) return wait_timeout;
			if (result != WAIT_IO_COMPLETION && result >= WAIT_OBJECT_0 &&
				result < WAIT_OBJECT_0 + handles.size())
			{
				const auto index = static_cast<size_t>(result - WAIT_OBJECT_0);
				if (index < caller_count)
				{
					if (!wait_all) return static_cast<uint32_t>(index);
					bool all = true;
					for (const auto& event : caller)
						all = all && WaitForSingleObject(static_cast<HANDLE>(event.get()._h), 0) == WAIT_OBJECT_0;
					if (all) return static_cast<uint32_t>(index);
				}
				else dispatch_app_event(index - caller_count);
			}

			// Input and queued UI work are drained after every wake, not only when the message queue won.
			// A frame timer that is ready continuously must not starve paint, sent messages, or a worker
			// completion that is ending this wait.
			if (!platform::is_closing) pump_messages(filter);
			if (!platform::is_closing && WaitForSingleObject(static_cast<HANDLE>(current_state->idle_event._h), 0) == WAIT_OBJECT_0)
				if (current_state->app) current_state->app->idle();

			if (caller_count != 0 && !platform::is_closing)
			{
				bool signalled = wait_all;
				uint32_t first = wait_timeout;
				for (size_t i = 0; i < caller_count; ++i)
				{
					const auto ready = WaitForSingleObject(static_cast<HANDLE>(caller[i].get()._h), 0) == WAIT_OBJECT_0;
					if (ready && first == wait_timeout) first = static_cast<uint32_t>(i);
					if (wait_all) signalled = signalled && ready;
					else signalled = signalled || ready;
				}
				if (signalled) return first == wait_timeout ? 0u : first;
			}
		}
		return wait_timeout;
	}

	std::optional<uint32_t> ui_thread_wait_callback(const std::vector<std::reference_wrapper<core::thread_event>>& events,
	                                       const bool wait_all, const uint32_t timeout_ms)
	{
		if (!current_state || !platform::is_ui_thread()) return std::nullopt;
		return wait_for_events(events, wait_all, timeout_ms, [](MSG& message) { return pre_translate(message); });
	}
}

namespace platform::detail
{
	bool pump_backend_ui_tasks(const uint32_t timeout_ms)
	{
		// The UI thread runs its tasks as it dispatches their tokens; any other caller, and every caller
		// before the task window exists, runs them here. Asking about the thread first keeps a worker
		// from reading current_state, which the UI thread clears without the queue's lock.
		if (platform::is_ui_thread() && current_state)
		{
			if (timeout_ms != 0)
				MsgWaitForMultipleObjectsEx(0, nullptr, timeout_ms, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
			if (!platform::is_closing) pump_messages([](MSG& message) { return pre_translate(message); });
			return !platform::is_closing && !current_state->closed;
		}

		std::deque<queued_ui_task> tasks;
		{
			std::unique_lock lock(ui_task_mutex);
			if (ui_tasks.empty() && timeout_ms != 0)
				ui_task_ready.wait_for(lock, std::chrono::milliseconds(timeout_ms),
					[&] { return !ui_tasks.empty() || !ui_accepting; });
			tasks.swap(ui_tasks);
		}
		for (auto& task : tasks)
		{
			try { if (task.action) task.action(); }
			catch (const std::exception& error) { core::log("platform UI task", error.what()); }
			catch (...) { core::log("platform UI task", "unknown exception"); }
			if (task.completed) task.completed->set();
		}
		return ui_accepting;
	}
}

void dispatch_frame_tick_if_due()
{
	if (current_state) current_state->frame_tick_if_due();
}

void reset_mouse_move_baseline()
{
	last_mouse_move = 0;
}

namespace platform::keys
{
	char32_t APPS = VK_APPS;
	char32_t BACK = VK_BACK;
	char32_t BROWSER_BACK = VK_BROWSER_BACK;
	char32_t BROWSER_FAVORITES = VK_BROWSER_FAVORITES;
	char32_t BROWSER_FORWARD = VK_BROWSER_FORWARD;
	char32_t BROWSER_HOME = VK_BROWSER_HOME;
	char32_t BROWSER_REFRESH = VK_BROWSER_REFRESH;
	char32_t BROWSER_SEARCH = VK_BROWSER_SEARCH;
	char32_t BROWSER_STOP = VK_BROWSER_STOP;
	char32_t DEL = VK_DELETE;
	char32_t DOWN = VK_DOWN;
	char32_t ESCAPE = VK_ESCAPE;
	char32_t F1 = VK_F1;
	char32_t F10 = VK_F10;
	char32_t F11 = VK_F11;
	char32_t F2 = VK_F2;
	char32_t F3 = VK_F3;
	char32_t F4 = VK_F4;
	char32_t F5 = VK_F5;
	char32_t F6 = VK_F6;
	char32_t F7 = VK_F7;
	char32_t F8 = VK_F8;
	char32_t F9 = VK_F9;
	char32_t F12 = VK_F12;
	char32_t INSERT = VK_INSERT;
	char32_t LEFT = VK_LEFT;
	char32_t MEDIA_NEXT_TRACK = VK_MEDIA_NEXT_TRACK;
	char32_t MEDIA_PLAY_PAUSE = VK_MEDIA_PLAY_PAUSE;
	char32_t MEDIA_PREV_TRACK = VK_MEDIA_PREV_TRACK;
	char32_t MEDIA_STOP = VK_MEDIA_STOP;
	char32_t NEXT = VK_NEXT;
	char32_t OEM_1 = VK_OEM_1;
	char32_t OEM_2 = VK_OEM_2;
	char32_t OEM_3 = VK_OEM_3;
	char32_t OEM_4 = VK_OEM_4;
	char32_t OEM_5 = VK_OEM_5;
	char32_t OEM_6 = VK_OEM_6;
	char32_t OEM_7 = VK_OEM_7;
	char32_t OEM_102 = VK_OEM_102;
	char32_t OEM_COMMA = VK_OEM_COMMA;
	char32_t OEM_PERIOD = VK_OEM_PERIOD;
	char32_t OEM_MINUS = VK_OEM_MINUS;
	char32_t OEM_PLUS = VK_OEM_PLUS;
	char32_t PRIOR = VK_PRIOR;
	char32_t RETURN = VK_RETURN;
	char32_t RIGHT = VK_RIGHT;
	char32_t SPACE = VK_SPACE;
	char32_t TAB = VK_TAB;
	char32_t UP = VK_UP;
	char32_t VOLUME_DOWN = VK_VOLUME_DOWN;
	char32_t VOLUME_MUTE = VK_VOLUME_MUTE;
	char32_t VOLUME_UP = VK_VOLUME_UP;
	char32_t HOME = VK_HOME;
	char32_t END = VK_END;
	char32_t SHIFT = VK_SHIFT;
	char32_t CONTROL = VK_CONTROL;
	char32_t ALT = VK_MENU;
	char32_t LSHIFT = VK_LSHIFT;
	char32_t RSHIFT = VK_RSHIFT;
	char32_t LCONTROL = VK_LCONTROL;
	char32_t RCONTROL = VK_RCONTROL;
	char32_t LALT = VK_LMENU;
	char32_t RALT = VK_RMENU;
	char32_t LWIN = VK_LWIN;
	char32_t RWIN = VK_RWIN;
	char32_t PAUSE = VK_PAUSE;
	char32_t CAPITAL = VK_CAPITAL;
	char32_t NUMLOCK = VK_NUMLOCK;
	char32_t SCROLL = VK_SCROLL;
	char32_t SNAPSHOT = VK_SNAPSHOT;
	char32_t NUMPAD0 = VK_NUMPAD0;
	char32_t NUMPAD1 = VK_NUMPAD1;
	char32_t NUMPAD2 = VK_NUMPAD2;
	char32_t NUMPAD3 = VK_NUMPAD3;
	char32_t NUMPAD4 = VK_NUMPAD4;
	char32_t NUMPAD5 = VK_NUMPAD5;
	char32_t NUMPAD6 = VK_NUMPAD6;
	char32_t NUMPAD7 = VK_NUMPAD7;
	char32_t NUMPAD8 = VK_NUMPAD8;
	char32_t NUMPAD9 = VK_NUMPAD9;
	char32_t MULTIPLY = VK_MULTIPLY;
	char32_t ADD = VK_ADD;
	char32_t SUBTRACT = VK_SUBTRACT;
	char32_t DECIMAL = VK_DECIMAL;
	char32_t DIVIDE = VK_DIVIDE;
	char32_t SEPARATOR = VK_SEPARATOR;
}

namespace platform
{
	int ticks_since_last_user_action = 0;

	key_state current_key_state()
	{
		return {
			GetAsyncKeyState(VK_CONTROL) < 0,
			GetAsyncKeyState(VK_SHIFT) < 0,
			GetAsyncKeyState(VK_MENU) < 0
		};
	}

	bool is_key_down(const char32_t key)
	{
		return (GetKeyState(static_cast<int>(key)) & 0x8000) != 0;
	}

	bool is_key_down_async(const char32_t key)
	{
		return (GetAsyncKeyState(static_cast<int>(key)) & 0x8000) != 0;
	}

	key_state wheel_key_state(const uint32_t message, const WPARAM parameter, const key_state keyboard)
	{
		return message == 0x024e || message == 0x024f
			? keyboard
			: key_state{(parameter & MK_CONTROL) != 0, (parameter & MK_SHIFT) != 0, false};
	}

	core::recti desktop_bounds(const bool work_area)
	{
		RECT window_rect{};
		const auto hwnd = app_wnd();
		if (hwnd) GetWindowRect(hwnd, &window_rect);
		else window_rect = {0, 0, GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN)};
		const auto monitor = MonitorFromRect(&window_rect, MONITOR_DEFAULTTONEAREST);
		MONITORINFO info{sizeof(info)};
		if (monitor && GetMonitorInfoW(monitor, &info)) return {work_area ? info.rcWork.left : info.rcMonitor.left,
			work_area ? info.rcWork.top : info.rcMonitor.top, work_area ? info.rcWork.right : info.rcMonitor.right,
			work_area ? info.rcWork.bottom : info.rcMonitor.bottom};
		if (work_area && SystemParametersInfoW(SPI_GETWORKAREA, 0, &window_rect, 0)) return {
			window_rect.left, window_rect.top, window_rect.right, window_rect.bottom};
		return {0, 0, GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN)};
	}
}

namespace
{

	std::string command_line_without_executable()
	{
		const auto raw = std::wstring(GetCommandLineW());
		size_t pos = 0;
		const auto whitespace = [](const wchar_t value) { return value == L' ' || value == L'\t'; };
		while (pos < raw.size() && whitespace(raw[pos])) ++pos;
		if (pos < raw.size() && raw[pos] == L'\"')
		{
			++pos;
			while (pos < raw.size() && raw[pos] != L'\"') ++pos;
			if (pos < raw.size()) ++pos;
		}
		else
		{
			while (pos < raw.size() && !whitespace(raw[pos])) ++pos;
		}
		while (pos < raw.size() && whitespace(raw[pos])) ++pos;
		return core::utf16_to_utf8(std::wstring_view(raw).substr(pos));
	}

	void set_dpi_awareness()
	{
		using set_process_dpi_awareness = HRESULT(WINAPI*)(int);
		const auto module = LoadLibraryW(L"shcore.dll");
		if (!module) return;
		const auto set = reinterpret_cast<set_process_dpi_awareness>(GetProcAddress(module, "SetProcessDpiAwareness"));
		if (set) set(2); // PROCESS_PER_MONITOR_DPI_AWARE; the manifest may already have selected a newer mode.
		FreeLibrary(module);
	}

	int run_message_loop()
	{
		if (!current_state) return 0;
		// Keep the old five-times-per-second cadence alive before the first frame can choose a
		// more specific delay - unless init already chose one. This also arms the fallback tick
		// used by native modal loops.
		if (!current_state->frame_delay_chosen) current_state->set_delay(default_frame_delay_ms);
		MSG ignored{};
		PeekMessageW(&ignored, nullptr, WM_USER, WM_USER, PM_NOREMOVE);
		while (!platform::is_closing && !current_state->closed)
		{
			current_state->rebuild_events();
			const auto count = static_cast<DWORD>(current_state->event_handles.size());
			const auto result = MsgWaitForMultipleObjectsEx(count, current_state->event_handles.data(), INFINITE,
			                                                QS_ALLINPUT, MWMO_ALERTABLE);
			if (result == WAIT_FAILED) return -1;
			if (result != WAIT_IO_COMPLETION && result >= WAIT_OBJECT_0 && result < WAIT_OBJECT_0 + count)
				dispatch_app_event(static_cast<size_t>(result - WAIT_OBJECT_0));
			// Drain input after every wake. A frame timer that wins repeatedly must not starve messages.
			if (!platform::is_closing) pump_messages([](MSG& message) { return pre_translate(message); });
			if (!platform::is_closing && current_state->app) current_state->app->idle();
		}
		return quit_code;
	}

	bool pump_message_events()
	{
		if (!current_state) return false;
		pump_messages([](MSG& message) { return pre_translate(message); });
		return !platform::is_closing && !current_state->closed;
	}

	bool modal_filter_message(MSG& message, platform::modal_filter* filter)
	{
		if (!filter) return false;
		switch (message.message)
		{
		case WM_KEYDOWN:
		case WM_SYSKEYDOWN:
			return filter->key_down(static_cast<int>(message.wParam), platform::current_key_state());
		case WM_KEYUP:
		case WM_SYSKEYUP:
		case WM_CHAR:
		case WM_SYSCHAR:
		case WM_DEADCHAR:
		case WM_SYSDEADCHAR:
			// A drawn menu owns the keyboard for the duration of its modal loop. Its key-down handler
			// has already acted, so key-up and typed-character messages must not leak to the host.
			return true;
		case WM_LBUTTONDOWN:
		case WM_RBUTTONDOWN:
		case WM_MBUTTONDOWN:
		case WM_XBUTTONDOWN:
		case WM_LBUTTONDBLCLK:
		case WM_RBUTTONDBLCLK:
		case WM_MBUTTONDBLCLK:
		case WM_XBUTTONDBLCLK:
		case WM_NCLBUTTONDOWN:
		case WM_NCRBUTTONDOWN:
		case WM_NCMBUTTONDOWN:
		case WM_NCXBUTTONDOWN:
		case WM_NCLBUTTONDBLCLK:
		case WM_NCRBUTTONDBLCLK:
		case WM_NCMBUTTONDBLCLK:
			{
				const bool nonclient = message.message == WM_NCLBUTTONDOWN || message.message == WM_NCRBUTTONDOWN ||
				                       message.message == WM_NCMBUTTONDOWN || message.message == WM_NCXBUTTONDOWN ||
				                       message.message == WM_NCLBUTTONDBLCLK || message.message == WM_NCRBUTTONDBLCLK ||
				                       message.message == WM_NCMBUTTONDBLCLK;
				POINT point{GET_X_LPARAM(message.lParam), GET_Y_LPARAM(message.lParam)};
				if (auto* target = platform_window_from_native(message.hwnd))
				{
					if (!nonclient) ClientToScreen(message.hwnd, &point);
					return filter->pointer_down(*target, {point.x, point.y});
				}
				// A click on a foreign window is outside every popup owned by this process.
				filter->activation_lost();
				return true;
			}
		case WM_ACTIVATEAPP:
			if (!message.wParam) filter->activation_lost();
			return false;
		case WM_ACTIVATE:
			if (LOWORD(message.wParam) == WA_INACTIVE) filter->activation_lost();
			return false;
		default:
			return false;
		}
	}

	int run_modal(const std::function<bool()>& done, platform::modal_filter* filter, const uint32_t timeout_ms)
	{
		if (!current_state) return done();
		const auto deadline = timeout_ms == UINT32_MAX ? UINT64_MAX : GetTickCount64() + timeout_ms;
		while (!platform::is_closing && !done())
		{
			current_state->rebuild_events();
			DWORD wait_ms = INFINITE;
			if (deadline != UINT64_MAX)
			{
				const auto now = GetTickCount64();
				if (now >= deadline) return false;
				wait_ms = static_cast<DWORD>(std::min<uint64_t>(deadline - now, INFINITE - 1ull));
			}
			const auto count = static_cast<DWORD>(current_state->event_handles.size());
			const auto result = MsgWaitForMultipleObjectsEx(count, current_state->event_handles.data(), wait_ms,
			                                                QS_ALLINPUT, MWMO_ALERTABLE);
			if (result == WAIT_TIMEOUT || result == WAIT_FAILED) return false;
			if (result != WAIT_IO_COMPLETION && result >= WAIT_OBJECT_0 && result < WAIT_OBJECT_0 + count)
				dispatch_app_event(static_cast<size_t>(result - WAIT_OBJECT_0));

			MSG message{};
			while (!platform::is_closing && PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE))
			{
				if (message.message == WM_QUIT)
				{
					quit_code = static_cast<int>(message.wParam);
					platform::is_closing = true;
					if (current_state) current_state->closed = true;
					continue;
				}
				if (dispatch_platform_message(message)) continue;
				if (modal_filter_message(message, filter)) continue;
				// A tracked drawn menu treats a right release inside one of its popups as a left
				// release, matching the native menu convention. An outside right-down already ended
				// the modal session, so this is only reached for a popup target.
				if (filter && message.message == WM_RBUTTONUP && platform_window_from_native(message.hwnd))
					message.message = WM_LBUTTONUP;
				if (pre_translate(message)) continue;
				dispatch_message(message);
			}
			if (!platform::is_closing && current_state->app) current_state->app->idle();
		}
		return !platform::is_closing && done();
	}

}
namespace platform
{
	core::color32 system_color_value(const system_color c)
	{
		switch (c)
		{
		case system_color::window: return static_cast<core::color32>(GetSysColor(COLOR_WINDOW));
		case system_color::window_text: return static_cast<core::color32>(GetSysColor(COLOR_WINDOWTEXT));
		case system_color::desktop: return static_cast<core::color32>(GetSysColor(COLOR_DESKTOP));
		}
		return 0;
	}

	int run(const std::shared_ptr<app> application, const int argc, char** argv)
	{
		(void)argc;
		(void)argv;
		if (!application) return -1;
		platform::detail::set_task_shutdown_bounded(true);
		get_resource_instance = GetModuleHandleW(nullptr);
		set_ui_thread();
		start_log();
		is_closing = false;
		ui_accepting = true;
		quit_code = 0;
		last_mouse_move = 0;
		g_platform_app = application;
		g_process_events = application;
		std::unique_ptr<app_state> state;
		bool finalized = false;
		std::vector<std::function<void()>> hooks;
		const auto finalize = [&]
		{
			if (finalized) return;
			{
				std::lock_guard lock(ui_task_mutex);
				ui_accepting = false;
				for (auto& task : ui_tasks) if (task.completed) task.completed->set();
				ui_tasks.clear();
			}
			ui_task_ready.notify_all();
			if (current_state)
			{
				current_state->close();
				hooks = std::move(current_state->shutdown_hooks);
			}
			unregister_restart();
			current_state = nullptr;
			set_ui_thread_wait(nullptr);
			set_app_window_enabled_sync(nullptr);
			g_process_events.reset();
			g_platform_app.reset();
			application->final_exit();
			for (auto& hook : pending_shutdown_hooks) hooks.emplace_back(std::move(hook));
			pending_shutdown_hooks.clear();
			for (auto& hook : hooks) if (hook) hook();
			finalized = true;
		};
		const core::scope_exit finish([&]
		{
			finalize();
			close_log();
		});

#ifndef WINSTORE
		unhandled_exception_filter exception_filter;
#endif

		try
		{
			if (!application->pre_init()) return 0;
			set_dpi_awareness();
			core::set_thread_description("main");

			WSADATA wsa_data{};
			if (WSAStartup(MAKEWORD(2, 2), &wsa_data) != 0)
			{
				show_fatal_error(text().error_winsock_failed);
				return 0;
			}
			const core::scope_exit cleanup_winsock([] { WSACleanup(); });

			const auto command_line = command_line_without_executable();
			const auto hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
			if (FAILED(hr))
			{
				show_fatal_error(text().error_ole_failed);
				return 0;
			}
			const core::scope_exit cleanup_com([] { CoUninitialize(); });
			const auto ole_hr = OleInitialize(nullptr);
			if (FAILED(ole_hr))
			{
				show_fatal_error(text().error_ole_failed);
				return 0;
			}
			const core::scope_exit cleanup_ole([] { OleUninitialize(); });
			// finalise is declared after the COM/OLE guards, so its guard runs first on every return.
			const core::scope_exit finalize_before_ole([&] { finalize(); });

			state = std::make_unique<app_state>(application);
			{
				// Tasks queued before the task window existed get their tokens now, in their order.
				std::lock_guard lock(ui_task_mutex);
				current_state = state.get();
				if (current_state->task_window)
					for (size_t i = 0; i < ui_tasks.size(); ++i)
						if (PostMessageW(current_state->task_window, wm_platform_run_ui, 0, 0) == FALSE) break;
			}
			current_state->shutdown_hooks = std::move(pending_shutdown_hooks);
			set_ui_thread_wait(ui_thread_wait_callback);
			set_app_window_enabled_sync(sync_app_window_enabled);

			if (application->parse_command_line(command_line))
			{
				platform::detail::set_task_shutdown_bounded(false);
				setup_headless_console();
				return application->run_console_mode();
			}

#if defined(_M_IX86) || defined(_M_X64)
			if (!core::sse2_supported)
			{
				show_fatal_error(text().error_sse2_needed);
				return 0;
			}
#endif
			if (!IsWindows7OrGreater())
			{
				show_fatal_error(text().error_unsupported_os);
				return 0;
			}

			// A false result means settings were unavailable, not that the process must silently exit.
			application->load_settings(settings());
			int exit_code = 0;
			if (application->init(command_line))
			{
				show_app_windows();
				setup_restart(application->restart_command_line());
				exit_code = application->owns_loop() ? application->main_loop() : run_message_loop();
			}
			else
			{
				exit_code = quit_code;
			}
			application->exit();
			return exit_code;
		}
		catch (const std::exception& error)
		{
			core::log(__FUNCTION__, error.what());
			show_fatal_error(error.what());
			return -1;
		}
		catch (...)
		{
			show_fatal_error("Unhandled exception");
			return -1;
		}
	}

	bool pump_events()
	{
		return pump_message_events();
	}

	void quit(const int exit_code)
	{
		quit_code = exit_code;
		is_closing = true;
		event_exit.set();
		PostQuitMessage(exit_code);
	}

	void queue_idle()
	{
		if (current_state) current_state->idle_event.set();
	}

	void set_frame_delay(const int milliseconds)
	{
		if (!current_state) return;
		current_state->frame_delay_chosen = true;
		current_state->set_delay(milliseconds);
	}

	void monitor_folders(const std::vector<core::folder_path>& folders)
	{
		if (current_state) current_state->monitor_folders(folders);
	}

	void enable_screen_saver(const bool enable)
	{
		if (current_state) current_state->enable_screen_saver(enable);
		else SetThreadExecutionState(enable ? ES_CONTINUOUS : (ES_CONTINUOUS | ES_SYSTEM_REQUIRED | ES_DISPLAY_REQUIRED));
	}

	void show_progress(const int64_t done, const int64_t total)
	{
		if (current_state) current_state->show_progress(done, total);
	}

	void end_progress()
	{
		if (current_state) current_state->end_progress();
	}

	void run_ui(std::function<void()> task)
	{
		post_ui_task(std::move(task));
	}

	void send_system_event(const os_event_type event)
	{
		if (!current_state) return;
		if (GetCurrentThreadId() == current_state->ui_thread)
		{
			if (current_state->app) current_state->app->system_event(event);
			return;
		}
		core::thread_event completed(false, false);
		const auto queued = post_ui_task([event]
		{
			if (current_state && current_state->app) current_state->app->system_event(event);
		}, &completed);
		if (queued) WaitForSingleObject(static_cast<HANDLE>(completed._h), INFINITE);
	}

	void add_shutdown_hook(std::function<void()> hook)
	{
		if (!hook) return;
		if (current_state) current_state->shutdown_hooks.emplace_back(std::move(hook));
		else pending_shutdown_hooks.emplace_back(std::move(hook));
	}

	bool run_modal_loop(const std::function<bool()>& done, modal_filter* filter, const uint32_t timeout_ms)
	{
		return run_modal(done, filter, timeout_ms) != 0;
	}
}