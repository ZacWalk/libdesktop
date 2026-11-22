// This file is part of libdesktop, a C++20 platform layer for desktop applications
// Copyright 2026  Zac Walker
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

// Purpose: Makes frames over an offscreen application window and checks native and embedded panes,
// their layout, painting, input, focus and timers, the ticks their controls are given, a control's drag
// ending with the pointer taken from it, the keys a typing control has first, and input sent from outside -
// presses, keys, text and a context menu worked inside its loop. No window is visible on the desktop.

#include "libdesktop/platform.h"
#include "libdesktop/platform/win32.h"
#include "libdesktop/ui.h"
#include "libdesktop/ui/controls.h"
#include "libdesktop/ui/menu_bar.h"

#include <dwmapi.h>

#include <cstring>
#include <format>

namespace
{
	// A host that, as an application's does, holds the frame made for it only once the call making it
	// has returned.
	class recording_host final : public ui::frame_host
	{
	public:
		ui::frame_ptr frame;
		int layouts = 0;
		int layouts_before_frame = 0;
		int paints = 0;
		int ticks = 0;
		int downs = 0;
		int ups = 0;
		int wheels = 0;
		int keys = 0;
		core::pointi pointer;
		bool tracking = false;
		bool handles_wheel = false;
		std::vector<ui::command_ptr> menu_commands;

		void on_window_layout(render::measure_context&, core::sizei, bool) override
		{
			++layouts;
			if (!frame) ++layouts_before_frame;
		}

		void on_window_paint(render::draw_context&) override { ++paints; }
		void tick() override { ++ticks; }
		void on_mouse_move(const core::pointi loc, const bool is_tracking) override
		{
			pointer = loc;
			tracking = is_tracking;
		}
		void on_mouse_left_button_down(const core::pointi loc, platform::key_state) override
		{
			pointer = loc;
			++downs;
		}
		void on_mouse_left_button_up(const core::pointi loc, platform::key_state) override
		{
			pointer = loc;
			++ups;
		}
		void on_mouse_wheel(const core::pointi loc, int, platform::key_state, bool& handled) override
		{
			pointer = loc;
			++wheels;
			handled = handles_wheel;
		}
		bool key_down(char32_t, platform::key_state) override
		{
			++keys;
			return true;
		}
		std::vector<ui::command_ptr> menu(core::pointi) override { return menu_commands; }
	};

	using recording_host_ptr = std::shared_ptr<recording_host>;

	// A host whose tree is the elements it is given, each laid out where it was placed.
	class controls_host final : public ui::frame_host
	{
	public:
		ui::frame_ptr frame;
		std::vector<std::pair<ui::element_ptr, core::recti>> placed;
		int ticks = 0;

		void on_window_layout(render::measure_context& mc, core::sizei, bool) override
		{
			for (const auto& [e, r] : placed) e->layout(mc, r);
		}

		void on_window_paint(render::draw_context& dc) override
		{
			for (const auto& [e, r] : placed) e->render(dc, {});
		}

		void tick() override { ++ticks; }

		void visit_elements(const std::function<void(const ui::element_ptr&, core::pointi)>& visit) override
		{
			const auto copy = placed;
			for (const auto& [e, r] : copy) visit(e, {});
		}
	};

	using controls_host_ptr = std::shared_ptr<controls_host>;

	// A control that counts the ticks it is given.
	class tick_probe final : public ui::control_with_features<tick_probe>
	{
	public:
		int ticks = 0;

		core::sizei measure_content(render::measure_context&, int) const override { return {10, 10}; }
		void paint(render::draw_context&, bool) const override {}
		void on_control_tick() { ++ticks; }
	};

	class frame_creation_check final : public ui::app, public std::enable_shared_from_this<frame_creation_check>
	{
	public:
		bool init(std::string_view) override
		{
			ui::frame_style style;
			style.offscreen = true;
			style.drawn_caption = false;
			style.client_size = {320, 200};
			_main->frame = ui::create_app_frame(shared_from_this(), nullptr, _main, style);
			check(_main->frame != nullptr, "an application window is made");
			if (_main->frame)
			{
				check(_main->layouts > 0, "the application's window is laid out as it is made");
				check_key_release();
				check_modal_key_release();
				check_dialog();
				check_pane(_popup, false, "a popup");
				check_pane(_pane, true, "a pane");
				check_embedded_panes();
				check_embedded_input_state();
				check_control_ticks();
				check_capture_loss();
				check_typing_keys();
				check_sent_input();
			}

			for (const auto& host : {_untimed_native, _untimed_embedded, _timed, _capture, _typing, _sent})
			{
				if (host->frame) host->frame->destroy();
				host->frame.reset();
				host->placed.clear();
			}
			for (const auto& host : {_sent_native, _inner, _outer, _nested, _embedded, _pane, _popup, _dialog, _main})
			{
				if (host->frame) host->frame->destroy();
				host->frame.reset();
			}
			platform::write_stdout(std::format("Frame creation: {} checks, {} failed\n", _checks, _failures));
			platform::quit(_failures == 0 ? 0 : 1);
			return false;
		}

		bool wants_gpu() const override { return false; }
		bool key_down(const char32_t key, platform::key_state) override
		{
			_offered.push_back(key);
			return false;
		}
		bool key_up(const char32_t key, platform::key_state) override
		{
			_released = key;
			return false;
		}
		bool text_input(std::string_view) override { return false; }
		ui::focus_mode focus_mode() const override { return ui::focus_mode::none; }
		void track_menu(const ui::frame_ptr& frame, const core::recti bounds,
		                const std::vector<ui::command_ptr>& commands) override
		{
			if (_shows_menus && frame) frame->track_menu(bounds, commands);
		}

	private:
		recording_host_ptr _main = std::make_shared<recording_host>();
		recording_host_ptr _dialog = std::make_shared<recording_host>();
		recording_host_ptr _popup = std::make_shared<recording_host>();
		recording_host_ptr _pane = std::make_shared<recording_host>();
		recording_host_ptr _embedded = std::make_shared<recording_host>();
		recording_host_ptr _nested = std::make_shared<recording_host>();
		recording_host_ptr _outer = std::make_shared<recording_host>();
		recording_host_ptr _inner = std::make_shared<recording_host>();
		recording_host_ptr _sent_native = std::make_shared<recording_host>();
		controls_host_ptr _untimed_native = std::make_shared<controls_host>();
		controls_host_ptr _untimed_embedded = std::make_shared<controls_host>();
		controls_host_ptr _timed = std::make_shared<controls_host>();
		controls_host_ptr _capture = std::make_shared<controls_host>();
		controls_host_ptr _typing = std::make_shared<controls_host>();
		controls_host_ptr _sent = std::make_shared<controls_host>();
		int _failures = 0;
		int _checks = 0;
		char32_t _released = 0;
		// Whether a context menu asked for is shown, as a drawn menu whose loop runs inside the call.
		bool _shows_menus = false;
		// The keys offered to the application, in order.
		std::vector<char32_t> _offered;

		void check(const bool ok, const std::string_view what)
		{
			++_checks;
			platform::write_stdout(std::format("{} {}\n", ok ? "PASS" : "FAIL", what));
			if (!ok) ++_failures;
		}

		// An application that reads held keys hears each one let go, as it heard it go down.
		void check_key_release()
		{
			const auto window = std::any_cast<HWND>(_main->frame->handle());
			SendMessageW(window, WM_SETFOCUS, 0, 0);
			SendMessageW(window, WM_KEYDOWN, 'J', 0);
			SendMessageW(window, WM_KEYUP, 'J', 0xC0000001);
			check(_released == 'J', "the application hears a key let go");
			// The checks after count the main window's keys from none.
			_main->keys = 0;
		}

		// An open menu's loop takes every release, which its filter may pass on to the application.
		void check_modal_key_release()
		{
			struct release_filter final : platform::modal_filter
			{
				char32_t released = 0;
				void key_up(const char32_t key, platform::key_state) override { released = key; }
			} filter;

			const auto window = std::any_cast<HWND>(_main->frame->handle());
			_released = 0;
			PostMessageW(window, WM_KEYUP, 'Q', 0xC0000001);
			platform::run_modal_loop([&filter] { return filter.released != 0; }, &filter, 2000);
			check(filter.released == 'Q', "a modal loop's filter hears a key let go");
			check(_released == 0, "which reaches no window");
		}

		void check_dialog()
		{
			_dialog->frame = _main->frame->create_dlg(_dialog);
			check(_dialog->frame != nullptr, "a dialog is made over the application's window");
			if (!_dialog->frame) return;
			check(_dialog->layouts_before_frame == 0, "a dialog is not laid out before its host holds it");

			const auto before = _dialog->layouts;
			_dialog->frame->window_bounds({0, 0, 240, 120}, false);
			check(_dialog->layouts > before, "placing a dialog lays it out");
			_dialog->frame->layout();
			check(_dialog->layouts > before + 1, "a dialog is laid out when asked");

			_dialog->frame->window_bounds({40, 40, 280, 160}, true);
			check(is_offscreen(_dialog->frame), "a dialog of a window nobody sees is not put on the desktop");
		}

		// How an offscreen window is kept from the desktop: cloaked, by the application.
		static bool is_offscreen(const ui::frame_ptr& f)
		{
			const auto handle = f ? f->handle() : std::any{};
			const auto* const window = std::any_cast<HWND>(&handle);
			DWORD cloaked = 0;
			return window && SUCCEEDED(DwmGetWindowAttribute(*window, DWMWA_CLOAKED, &cloaked, sizeof(cloaked))) &&
			       (cloaked & DWM_CLOAKED_APP) != 0;
		}

		void check_pane(const recording_host_ptr& host, const bool child, const std::string_view name)
		{
			ui::frame_style style;
			style.child = child;
			style.no_focus = !child;
			style.embedded = !child;
			host->frame = _main->frame->create_frame(host, style);
			check(host->frame != nullptr, std::format("{} is made in the application's window", name));
			if (!host->frame) return;
			check(host->frame->handle().has_value(), std::format("{} retains its native window", name));
			check(host->layouts_before_frame == 0, std::format("{} is not laid out before its host holds it", name));

			const auto before = host->layouts;
			const core::recti placement{19, 23, 179, 103};
			host->frame->window_bounds(placement, false);
			check(host->layouts > before, std::format("placing {} lays it out", name));
			const core::pointi point{7, 11};
			const auto origin = host->frame->client_to_screen({});
			check(host->frame->client_to_screen(point) == origin + point,
			      std::format("{} maps physical client pixels to screen pixels", name));
			if (child)
				check(origin == _main->frame->client_to_screen(placement.top_left()),
				      "a child frame includes the application's screen position");
			else
				check(is_offscreen(host->frame),
				      std::format("{} of a window nobody sees is not put on the desktop", name));
		}

		static int child_window_count(const HWND parent)
		{
			int count = 0;
			EnumChildWindows(parent, [](HWND, const LPARAM value) -> BOOL
			{
				++*reinterpret_cast<int*>(value);
				return TRUE;
			}, reinterpret_cast<LPARAM>(&count));
			return count;
		}

		void check_embedded_panes()
		{
			const auto window = std::any_cast<HWND>(_main->frame->handle());
			const auto children = child_window_count(window);
			ui::frame_style style;
			style.embedded = true;
			style.can_focus = true;
			style.timer_milliseconds = 10;
			_embedded->frame = _main->frame->create_frame(_embedded, style);
			check(_embedded->frame != nullptr, "an embedded pane is made");
			if (!_embedded->frame) return;
			_nested->frame = _embedded->frame->create_frame(_nested, style);
			check(_nested->frame != nullptr, "an embedded pane can contain another");
			if (!_nested->frame) return;
			const auto windowless = !_embedded->frame->handle().has_value() && !_nested->frame->handle().has_value();
			check(windowless, "embedded panes have no native handles");
			check(child_window_count(window) == children, "embedded panes create no native child windows");
			if (!windowless) return;
			check(_embedded->layouts_before_frame == 0 && _nested->layouts_before_frame == 0,
			      "embedded panes are not laid out before their hosts hold them");

			const core::recti outer{189, 29, 309, 179};
			const core::recti inner{7, 11, 77, 71};
			_embedded->frame->window_bounds(outer, true);
			_nested->frame->window_bounds(inner, true);
			const auto origin = outer.top_left() + inner.top_left();
			const core::pointi point{13, 17};
			check(_embedded->layouts > 0 && _nested->layouts > 0, "placing embedded panes lays them out");
			check(_nested->frame->client_to_screen(point) == _main->frame->client_to_screen(origin + point),
			      "nested embedded panes include every parent offset exactly once");

			const auto paints = _embedded->paints;
			const auto nested_paints = _nested->paints;
			_main->frame->redraw_now();
			check(_embedded->paints > paints && _nested->paints > nested_paints,
			      "redrawing the native owner paints its embedded descendants");

			// Offscreen windows never take native focus; supply the notification without activating one.
			SendMessageW(window, WM_SETFOCUS, 0, 0);
			_embedded->frame->focus();
			check(_embedded->frame->has_focus() && !_main->frame->has_focus(),
			      "an embedded pane can hold the native owner's keyboard focus");
			_nested->frame->focus();
			check(_nested->frame->has_focus() && !_embedded->frame->has_focus(),
			      "focus moves independently between embedded panes");

			const auto loc = origin + point;
			SendMessageW(window, WM_MOUSEMOVE, 0, MAKELPARAM(loc.x, loc.y));
			check(_nested->pointer == point, "pointer input arrives in embedded client coordinates");
			SendMessageW(window, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(loc.x, loc.y));
			const core::pointi outside{3, 5};
			SendMessageW(window, WM_MOUSEMOVE, MK_LBUTTON, MAKELPARAM(outside.x, outside.y));
			check(_nested->downs == 1 && _nested->tracking && _nested->pointer == outside - origin,
			      "a captured drag stays with its embedded pane outside its bounds");
			SendMessageW(window, WM_LBUTTONUP, 0, MAKELPARAM(outside.x, outside.y));
			check(_nested->ups == 1 && _nested->pointer == outside - origin,
			      "the embedded pane receives the captured release");

			const auto screen = _main->frame->client_to_screen(loc);
			_nested->handles_wheel = true;
			SendMessageW(window, WM_MOUSEWHEEL, MAKEWPARAM(0, WHEEL_DELTA), MAKELPARAM(screen.x, screen.y));
			check(_nested->wheels == 1 && _embedded->wheels == 0 && _main->wheels == 0 && _nested->pointer == point,
			      "a handled wheel stays in the embedded pane");
			_nested->handles_wheel = false;
			_main->handles_wheel = true;
			SendMessageW(window, WM_MOUSEWHEEL, MAKEWPARAM(0, WHEEL_DELTA), MAKELPARAM(screen.x, screen.y));
			check(_nested->wheels == 2 && _embedded->wheels == 1 && _main->wheels == 1 && _main->pointer == loc,
			      "an unhandled wheel reaches each composed parent and the native owner once");

			check(platform::run_modal_loop([this] { return _embedded->ticks > 0 && _nested->ticks > 0; }, nullptr,
			                               2000),
			      "embedded panes receive independent timers through their native owner");
			const auto hidden_paints = _embedded->paints;
			const auto hidden_nested_paints = _nested->paints;
			_embedded->frame->show(false);
			_main->frame->redraw_now();
			check(_embedded->paints == hidden_paints && _nested->paints == hidden_nested_paints,
			      "hiding an embedded pane also hides its descendants without destroying them");
		}

		// Windows gives the keyboard to the parent of a child window it hides or destroys, and passes a
		// disabled child's pointer input to its parent. Embedded panes have no native window to do it.
		void check_embedded_input_state()
		{
			const auto window = std::any_cast<HWND>(_main->frame->handle());
			ui::frame_style style;
			style.embedded = true;
			style.can_focus = true;
			_outer->frame = _main->frame->create_frame(_outer, style);
			if (_outer->frame) _inner->frame = _outer->frame->create_frame(_inner, style);
			check(_outer->frame && _inner->frame, "embedded panes are made to hide, disable and destroy");
			if (!_outer->frame || !_inner->frame) return;

			const core::recti outer{11, 13, 151, 133};
			_outer->frame->window_bounds(outer, true);
			_inner->frame->window_bounds({5, 7, 65, 57}, true);
			const auto key = [window]
			{
				SendMessageW(window, WM_KEYDOWN, 'K', 0);
			};
			const auto loc = outer.top_left() + core::pointi{100, 90};
			const auto down = [window, loc]
			{
				SendMessageW(window, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(loc.x, loc.y));
			};
			const auto up = [window, loc]
			{
				SendMessageW(window, WM_LBUTTONUP, 0, MAKELPARAM(loc.x, loc.y));
			};

			SendMessageW(window, WM_SETFOCUS, 0, 0);
			_inner->frame->focus();
			key();
			check(_inner->frame->has_focus() && _inner->keys == 1, "a focused embedded pane receives keys");

			_outer->frame->show(false);
			key();
			check(!_inner->frame->has_focus() && _main->frame->has_focus() && _inner->keys == 1 && _main->keys == 1,
			      "hiding a pane gives the keyboard from the pane it holds to the nearest one shown");

			_outer->frame->show(true);
			_inner->frame->focus();
			_inner->frame->show(false);
			key();
			check(_outer->frame->has_focus() && _outer->keys == 1 && _inner->keys == 1,
			      "a hidden embedded pane gives the keyboard to its parent");

			_inner->frame->destroy();
			_inner->frame->focus();
			key();
			check(!_inner->frame->has_focus() && _outer->keys == 2 && _inner->keys == 1,
			      "a destroyed embedded pane cannot take the keyboard back");

			_outer->frame->enable(false);
			down();
			up();
			key();
			check(!_outer->frame->is_enabled() && _outer->downs == 0 && _main->downs == 1 && _main->keys == 2,
			      "a disabled embedded pane gives up the keyboard and passes clicks to its parent");

			_outer->frame->enable(true);
			down();
			_outer->frame->enable(false);
			up();
			check(_outer->downs == 1 && _outer->ups == 0 && _main->ups == 2,
			      "disabling an embedded pane ends the drag it holds");

			_outer->frame->enable(true);
			_outer->frame->focus();
			_outer->frame->destroy();
			key();
			check(_main->frame->has_focus() && _main->keys == 3 && _outer->keys == 2,
			      "destroying a focused embedded pane gives the keyboard to its native window");
		}

		// A pane ticks its host only as often as its style asks. The controls in one that asks for none, native or
		// embedded, tick with the window holding it, so an edit waiting on a dictionary or a word it added hears of
		// it; a pane with a timer of its own ticks its controls with its host, once a tick.
		void check_control_ticks()
		{
			const auto native = std::make_shared<tick_probe>();
			const auto embedded = std::make_shared<tick_probe>();
			const auto timed = std::make_shared<tick_probe>();
			_untimed_native->placed = {{native, {0, 0, 10, 10}}};
			_untimed_embedded->placed = {{embedded, {0, 0, 10, 10}}};
			_timed->placed = {{timed, {0, 0, 10, 10}}};

			ui::frame_style style;
			_untimed_native->frame = _main->frame->create_frame(_untimed_native, style);
			style.embedded = true;
			_untimed_embedded->frame = _main->frame->create_frame(_untimed_embedded, style);
			style.timer_milliseconds = 10;
			_timed->frame = _main->frame->create_frame(_timed, style);
			const auto made = _untimed_native->frame && _untimed_embedded->frame && _timed->frame;
			check(made, "panes with and without timers are made");
			if (!made) return;

			_untimed_native->frame->window_bounds({200, 150, 220, 170}, true);
			_untimed_embedded->frame->window_bounds({230, 150, 250, 170}, true);
			_timed->frame->window_bounds({260, 150, 280, 170}, true);

			check(platform::run_modal_loop([&] { return native->ticks > 0 && embedded->ticks > 0 && timed->ticks > 2; },
			                               nullptr, 2000),
			      "the controls of panes with no timer of their own tick with their window");
			check(_untimed_native->ticks == 0 && _untimed_embedded->ticks == 0, "though their hosts do not");
			check(timed->ticks == _timed->ticks,
			      "and a pane's own timer ticks its controls with its host, once a tick");
		}

		// A message box shown mid-drag, or another window taking the pointer, ends the drag a control was making: a
		// slider settles where it was, and the pointer moving on drags it no further.
		void check_capture_loss()
		{
			std::vector<std::pair<int, bool>> reports;
			const auto slider = ui::make_slider(0, 100, [&reports](const int pos, const bool tracking)
			{ reports.emplace_back(pos, tracking); });
			_capture->placed = {{slider, {0, 0, 200, 30}}};
			ui::frame_style style;
			style.embedded = true;
			_capture->frame = _main->frame->create_frame(_capture, style);
			check(_capture->frame != nullptr && _dialog->frame != nullptr, "a pane is made for a slider");
			if (!_capture->frame || !_dialog->frame) return;
			_capture->frame->window_bounds({10, 100, 210, 130}, true);

			const auto window = std::any_cast<HWND>(_main->frame->handle());
			const auto send = [window](const UINT message, const WPARAM buttons, const int x)
			{
				SendMessageW(window, message, buttons, MAKELPARAM(10 + x, 115));
			};

			send(WM_LBUTTONDOWN, MK_LBUTTON, 100);
			check(GetCapture() == window && !reports.empty() && reports.back().second,
			      "a press on a slider drags it, holding the pointer");
			SendMessageW(window, WM_CANCELMODE, 0, 0);
			check(!reports.empty() && !reports.back().second && GetCapture() != window,
			      "a message box's cancel settles the drag and lets the pointer go");
			auto settled = reports.size();
			send(WM_MOUSEMOVE, MK_LBUTTON, 180);
			send(WM_LBUTTONUP, 0, 180);
			check(reports.size() == settled, "after which the pointer drags it no further");

			send(WM_LBUTTONDOWN, MK_LBUTTON, 50);
			SetCapture(std::any_cast<HWND>(_dialog->frame->handle()));
			check(!reports.back().second, "another window taking the pointer settles the drag too");
			settled = reports.size();
			ReleaseCapture();
			send(WM_MOUSEMOVE, MK_LBUTTON, 150);
			send(WM_LBUTTONUP, 0, 150);
			check(reports.size() == settled, "and the drag ends there");
		}

		// A typing control has first the keys it works with. An edit's open list takes Down and Return before the
		// application, which has them first otherwise; and a key AltGr types a character with is text, neither a
		// shortcut nor a menu's marked letter, though Windows reports AltGr as Ctrl+Alt.
		void check_typing_keys()
		{
			auto menus = 0;
			const auto file = std::make_shared<ui::command>();
			file->text = "&File";
			file->menu = [&menus]
			{
				++menus;
				return std::vector<ui::command_ptr>{};
			};
			ui::edit_styles styles;
			styles.auto_complete_list = {"apple", "apricot"};
			const auto edit = ui::make_edit(styles, {}, {});
			_typing->placed = {{ui::make_menu_bar({file}), {0, 0, 300, 30}}, {edit, {0, 40, 300, 70}}};

			ui::frame_style style;
			style.embedded = true;
			_typing->frame = _main->frame->create_frame(_typing, style);
			check(_typing->frame != nullptr, "a pane is made for an edit and a menu bar");
			if (!_typing->frame) return;
			_typing->frame->window_bounds({10, 130, 310, 200}, true);

			const auto events = std::dynamic_pointer_cast<platform::window_events>(_main->frame);
			SendMessageW(std::any_cast<HWND>(_main->frame->handle()), WM_SETFOCUS, 0, 0);
			edit->focus();
			check(events && edit->has_focus(), "the edit has the keyboard");
			if (!events || !edit->has_focus()) return;

			_offered.clear();
			events->key_down(platform::keys::RETURN, {}, false);
			check(_offered == std::vector<char32_t>{platform::keys::RETURN},
			      "with no list open, Return is offered to the application first");

			events->text("ap");
			_offered.clear();
			events->key_down(platform::keys::DOWN, {}, false);
			events->key_down(platform::keys::RETURN, {}, false);
			check(_offered.empty() && edit->window_text() == "apple",
			      "an open list has Down and Return before the application");

			platform::key_state alt_graph;
			alt_graph.control = true;
			alt_graph.alt = true;
			alt_graph.alt_graph = true;
			events->key_down(U'F', alt_graph, false);
			events->text("\xC4\x99");
			check(_offered.empty() && menus == 0 && edit->window_text() == "apple\xC4\x99",
			      "a key AltGr types with is text: no menu opens, and the application is not asked first");

			platform::key_state control_alt;
			control_alt.control = true;
			control_alt.alt = true;
			events->key_down(platform::keys::LEFT, control_alt, false);
			check(_offered == std::vector<char32_t>{platform::keys::LEFT},
			      "while Ctrl+Alt+Left, which types nothing, is still the application's shortcut");
		}

		// Input sent from outside - an agent's, a test's - takes real input's path. A press on a drawn button presses
		// it; one on an edit gives it the keyboard, though nobody sees the window, and the text and keys sent reach
		// it. A right press raises the window's context menu, whose loop has the input first: a row is pressed where
		// its popup shows it, a press outside closes it, keys work it and text goes nowhere. A window a dialog holds
		// takes none of it, the dialog taking the keys.
		void check_sent_input()
		{
			auto pressed = 0;
			const auto button = ui::make_button(icon_index{}, "Go", {}, [&pressed] { ++pressed; }, false);
			const auto edit = ui::make_edit({}, {}, {});
			_sent->placed = {{button, {0, 0, 100, 30}}, {edit, {0, 40, 300, 70}}};
			ui::frame_style style;
			style.embedded = true;
			_sent->frame = _main->frame->create_frame(_sent, style);
			check(_sent->frame != nullptr, "a pane is made for input sent from outside");
			if (!_sent->frame || !_dialog->frame) return;
			_sent->frame->window_bounds({10, 120, 310, 190}, true);

			const auto frame = _sent->frame;
			const auto press = [&frame](const core::pointi loc)
			{
				const auto down = ui::send_pointer(frame, ui::pointer_input::down, platform::pointer_button::left, loc);
				const auto up = ui::send_pointer(frame, ui::pointer_input::up, platform::pointer_button::left, loc);
				return down && up;
			};

			check(press({50, 15}) && pressed == 1, "a press sent to a drawn button presses it");

			const auto window = std::any_cast<HWND>(_main->frame->handle());
			SendMessageW(window, WM_KILLFOCUS, 0, 0);
			check(press({150, 55}) && edit->has_focus(),
			      "one on an edit gives it the keyboard, though nobody sees the window");
			check(ui::send_text(frame, "hello") && ui::send_key(frame, platform::keys::BACK) &&
			          edit->window_text() == "hell",
			      "and the text and keys sent reach it");

			platform::ticks_since_last_user_action = 100;
			ui::send_key(frame, platform::keys::RIGHT);
			const auto stepped_idle = platform::ticks_since_last_user_action >= 100;
			ui::send_pointer(frame, ui::pointer_input::move, platform::pointer_button::left, {150, 55});
			check(stepped_idle && platform::ticks_since_last_user_action < 100,
			      "sent input is the user's activity as real input is, but for Right, which steps along");

			struct menu_state
			{
				std::vector<ui::menu_level_state> levels;
				bool outside_taken = false;
				bool closed_by_outside = false;
				bool typed = false;
			};
			const auto state = std::make_shared<menu_state>();
			auto first = 0;
			auto second = 0;
			_main->menu_commands = {std::make_shared<ui::command>("&First", std::any{}, [&first] { ++first; }, false),
			                        std::make_shared<ui::command>("&Second", std::any{}, [&second] { ++second; },
			                                                      false)};
			_shows_menus = true;

			// Each runs inside the menu's loop, as an agent's call does. A menu it leaves open is closed, so a failure
			// cannot leave the loop waiting.
			const auto in_menu = [](std::function<void(ui::menu_session&)> work)
			{
				platform::run_ui([work = std::move(work)]
				{
					auto* const session = ui::open_menu_session();
					if (!session) return;
					work(*session);
					if (!session->done()) session->cancel();
				});
			};

			in_menu([state](ui::menu_session& session)
			{
				state->levels = session.levels();
				if (state->levels.size() != 1 || state->levels.front().rows.size() != 2) return;
				const auto& level = state->levels.front();
				const auto at = level.rows[1].bounds.center();
				ui::send_pointer(level.popup, ui::pointer_input::move, platform::pointer_button::left, at);
				ui::send_pointer(level.popup, ui::pointer_input::down, platform::pointer_button::left, at);
				ui::send_pointer(level.popup, ui::pointer_input::up, platform::pointer_button::left, at);
			});
			const auto raised = ui::send_context_menu(frame, {50, 15});
			check(raised && state->levels.size() == 1 && state->levels.front().rows.size() == 2 &&
			          state->levels.front().rows[1].text == "Second",
			      "a right press raises the context menu, whose rows are found where its popup shows them");
			check(second == 1 && first == 0 && pressed == 1, "and a press on a row chooses it");

			in_menu([state, frame](ui::menu_session& session)
			{
				state->outside_taken = ui::send_pointer(frame, ui::pointer_input::down, platform::pointer_button::left,
				                                        {50, 15});
				state->closed_by_outside = session.done();
				ui::send_pointer(frame, ui::pointer_input::up, platform::pointer_button::left, {50, 15});
			});
			ui::send_context_menu(frame, {50, 15});
			check(state->outside_taken && state->closed_by_outside && pressed == 1 && first + second == 1,
			      "a press outside the open menu closes it, pressing nothing under it");

			in_menu([state, frame](ui::menu_session&)
			{
				state->typed = ui::send_text(frame, "x");
				ui::send_key(frame, platform::keys::DOWN);
				ui::send_key(frame, platform::keys::RETURN);
			});
			ui::send_context_menu(frame, {50, 15});
			_shows_menus = false;
			_main->menu_commands.clear();
			check(first == 1 && !state->typed && edit->window_text() == "hell",
			      "keys sent work the open menu, and text sent goes nowhere");

			EnableWindow(window, FALSE);
			const auto refused = !press({50, 15}) && !ui::send_text(frame, "z");
			const auto dialog_keys = _dialog->keys;
			const auto to_dialog = ui::send_key(_dialog->frame, U'D');
			EnableWindow(window, TRUE);
			check(refused && pressed == 1 && edit->window_text() == "hell", "a window a dialog holds takes none of it");
			check(to_dialog && _dialog->keys == dialog_keys + 1,
			      "while the dialog takes the keys sent, as the platform gives it the keyboard");

			// Sent to the application's window, a press goes where the platform would send a real one: to the native
			// child window under it, which has the release after it wherever that is.
			_sent_native->frame = _main->frame->create_frame(_sent_native, {});
			if (!_sent_native->frame) return;
			_sent_native->frame->window_bounds({250, 20, 310, 60}, true);
			const auto main_downs = _main->downs;
			const auto sent_down = ui::send_pointer(_main->frame, ui::pointer_input::down,
			                                        platform::pointer_button::left, {270, 40});
			check(sent_down && _sent_native->downs == 1 && _main->downs == main_downs &&
			          _sent_native->pointer == core::pointi(20, 20),
			      "a press sent to the window reaches the native child window under it");
			ui::send_pointer(_main->frame, ui::pointer_input::up, platform::pointer_button::left, {100, 100});
			check(_sent_native->ups == 1 && _sent_native->pointer == core::pointi(-150, 80),
			      "which has the release, though it is let go of outside it");
		}
	};
}

int main(const int argc, char** argv)
{
	if (argc != 2 || std::strcmp(argv[1], "--check-frame-creation") != 0) return 1;
	core::set_app_name("libdesktop-ui-smoke");
	return platform::run(std::make_shared<frame_creation_check>(), argc, argv);
}
