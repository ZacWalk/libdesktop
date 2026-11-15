// This file is part of libdesktop, a C++20 platform layer for desktop applications
// Copyright 2026  Zac Walker
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

// Purpose: Runs a shell application over an offscreen window - its menu bar and content, its fixed-step loop,
// the keys its steps read and its menus' accelerators - by sending its window real key messages. No window
// is visible on the desktop.

#include "libdesktop/platform.h"
#include "libdesktop/platform/win32.h"
#include "libdesktop/ui.h"
#include "libdesktop/ui/shell.h"

#include <format>

namespace
{
	constexpr int steps_per_second = 200;
	// What the shell quits with when every check passed: not 0, so run is seen to return the shell's code.
	constexpr int passed_code = 3;

	class shell_check final : public ui::shell
	{
	public:
		shell_check() : shell(make_options()) {}

		static ui::shell_options make_options()
		{
			ui::shell_options options;
			options.offscreen = true;
			options.remember_placement = false;
			options.client_size = {320, 200};
			options.steps_per_second = steps_per_second;
			return options;
		}

		std::vector<ui::command_ptr> make_menus() override
		{
			auto reload = std::make_shared<ui::command>();
			reload->text = "&Reload";
			reload->kba.push_back({platform::keys::F5, 0});
			reload->invoke = [this]
			{
				++_accelerated;
			};

			auto file = std::make_shared<ui::command>();
			file->text = "&File";
			file->menu = [reload]
			{
				return std::vector<ui::command_ptr>{reload};
			};
			return {file};
		}

		ui::element_ptr make_content() override
		{
			// Made once the window is: what it needs of the window - its scale - is there to read.
			_content_saw_window = frame()->handle().has_value() && frame()->scale_factor() > 0.0;

			return std::make_shared<ui::canvas_element>([this](render::draw_context& dc, const core::recti bounds)
			{
				++_paints;
				dc.draw_rect(bounds, core::color(0.2f, 0.4f, 0.6f, 1.0f));
			});
		}

		bool start(std::string_view) override
		{
			const auto content = content_bounds();
			const auto client = frame()->window_bounds();
			check(content.top > 0, "the content sits below the menu bar");
			check(content.left == 0 && content.width() > 0 && content.height() > 0, "and fills the window across");
			check(!client.is_empty(), "the window is made");
			check(_content_saw_window, "before the content, which can read its scale");
			// An application's sounds play wherever there is an endpoint to play them through.
			const auto has_endpoint = !platform::list_audio_playback_devices().empty();
			check(platform::sound_init() == has_endpoint, "sound starts when there is an endpoint");
			platform::sound_shutdown();
			_window = std::any_cast<HWND>(frame()->handle());
			SendMessageW(_window, WM_SETFOCUS, 0, 0);
			_started = platform::monotonic_seconds();
			return true;
		}

		bool key(const char32_t key, platform::key_state) override
		{
			if (key == 'A') ++_keyed;
			return key == 'A';
		}

		void step(const double seconds) override
		{
			if (_quit_at != 0)
			{
				++_late_steps;
				return;
			}

			++_steps;
			check_once(seconds > 0.0049 && seconds < 0.0051, "each step is a fixed two hundredth of a second");

			// Posted, the messages arrive as a person's would: between steps, when the loop pumps.
			const auto post = [this](const UINT message, const char32_t key, const bool repeat = false)
			{
				const LPARAM flags = message == WM_KEYUP ? 0xC0000001 : repeat ? 0x40000001 : 0;
				PostMessageW(_window, message, key, flags);
			};
			// Alt's own messages carry the context bit, as the keyboard's do.
			const auto post_system = [this](const UINT message, const char32_t key)
			{
				const LPARAM flags = message == WM_SYSKEYUP ? 0xC0000001 : 0x20000001;
				PostMessageW(_window, message, key, flags);
			};

			switch (_stage)
			{
			case 0:
				if (_steps == 3)
				{
					post(WM_KEYDOWN, 'A');
					_stage = 1;
				}
				break;
			case 1:
				if (keys().held('A'))
				{
					check(keys().pressed('A'), "a key held is pressed in the first step that sees it");
					check(_keyed == 1, "and offered to the application");
					_stage = 2;
				}
				break;
			case 2:
				check(keys().held('A') && !keys().pressed('A'), "it stays held, pressed only once");
				post(WM_KEYUP, 'A');
				_stage = 3;
				break;
			case 3:
				if (!keys().held('A'))
				{
					check(keys().released('A'), "a key let go is released in the first step after");
					// Both reach the window before the next step: that step still sees the tap.
					post(WM_KEYDOWN, 'B');
					post(WM_KEYUP, 'B');
					_stage = 4;
				}
				break;
			case 4:
				if (keys().pressed('B'))
				{
					check(keys().released('B') && !keys().held('B'), "a tap between steps is kept");
					// Held, the key repeats; the command it invoked does not.
					post(WM_KEYDOWN, platform::keys::F5);
					post(WM_KEYDOWN, platform::keys::F5, true);
					post(WM_KEYDOWN, platform::keys::F5, true);
					post(WM_KEYUP, platform::keys::F5);
					_stage = 5;
				}
				break;
			case 5:
				if (_accelerated > 0)
				{
					check(_accelerated == 1, "a menu's accelerator invokes its command once, held or not");
					check(!keys().pressed(platform::keys::F5) && !keys().held(platform::keys::F5),
					      "and is not a key the steps read");
					post(WM_KEYDOWN, 'C');
					_stage = 6;
				}
				break;
			case 6:
				if (keys().held('C'))
				{
					// Alt alone puts the menu bar in menu mode, which takes the keys - the release of one held from
					// before included - until Escape leaves it.
					post_system(WM_SYSKEYDOWN, platform::keys::ALT);
					post_system(WM_SYSKEYUP, platform::keys::ALT);
					post(WM_KEYUP, 'C');
					post(WM_KEYDOWN, platform::keys::ESCAPE);
					post(WM_KEYUP, platform::keys::ESCAPE);
					_stage = 7;
					_menu_mode_at = _steps;
				}
				break;
			case 7:
				if (!keys().held('C') || _steps > _menu_mode_at + 40)
				{
					check(!keys().held('C'), "a key held as the menu bar takes the keyboard is let go of with it");
					_stage = 8;
					_finished_at = _steps;
				}
				break;
			default: break;
			}

			if ((_stage == 8 && _steps >= _finished_at + 40) || _steps == 1000)
			{
				const auto elapsed = platform::monotonic_seconds() - _started;
				check(_stage == 8, "the keys reach the steps that read them");
				check(elapsed > _steps / static_cast<double>(steps_per_second) * 0.8,
				      "the steps keep their rate rather than racing");
				check(_paints > 1, "the content is painted again after its steps");
				platform::write_stdout(std::format("Shell: {} checks, {} failed\n", _checks, _failures));
				quit(_failures == 0 ? passed_code : 1);
				_quit_at = _steps;
			}
		}

		int failures() const { return _failures; }
		int late_steps() const { return _late_steps; }

	private:
		HWND _window = nullptr;
		double _started = 0.0;
		int _steps = 0;
		int _stage = 0;
		int _finished_at = 0;
		int _menu_mode_at = 0;
		int _quit_at = 0;
		int _late_steps = 0;
		int _keyed = 0;
		int _accelerated = 0;
		int _paints = 0;
		int _checks = 0;
		int _failures = 0;
		bool _fixed_step_failed = false;
		bool _fixed_step_checked = false;
		bool _content_saw_window = false;

		void check(const bool ok, const std::string_view what)
		{
			++_checks;
			platform::write_stdout(std::format("{} {}\n", ok ? "PASS" : "FAIL", what));
			if (!ok) ++_failures;
		}

		// A check made every step, reported once.
		void check_once(const bool ok, const std::string_view what)
		{
			if (!ok && !_fixed_step_failed)
			{
				_fixed_step_failed = true;
				check(false, what);
			}
			else if (!_fixed_step_checked && _steps == 1)
			{
				_fixed_step_checked = true;
				check(ok, what);
			}
		}
	};
}

int main(const int argc, char** argv)
{
	core::set_app_name("LibDesktop-Shell-Smoke");
	const auto app = std::make_shared<shell_check>();
	const auto result = platform::run(app, argc, argv);
	const auto quit_ok = result == passed_code && app->late_steps() == 0;
	platform::write_stdout(std::format("{} quit ends the steps and run returns its code\n", quit_ok ? "PASS" : "FAIL"));
	return quit_ok && app->failures() == 0 ? 0 : 1;
}
