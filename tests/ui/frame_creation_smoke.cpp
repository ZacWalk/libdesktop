// This file is part of libdesktop, a C++20 platform layer for desktop applications
// Copyright 2026  Zac Walker
//
// This program is free software; you can redistribute it and / or modify it
// under the terms of the LGPL License either version 2.1 or later.
// License details are available at https://www.gnu.org/licenses/lgpl-2.1.html
// This program is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY

// Purpose: Makes real frames over an offscreen application window, as an application makes its dialogs,
// popups and panes, and checks when each host is first laid out. None of them is shown.

#include "libdesktop/platform.h"
#include "libdesktop/ui.h"

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

		void on_window_layout(render::measure_context&, core::sizei, bool) override
		{
			++layouts;
			if (!frame) ++layouts_before_frame;
		}

		void on_window_paint(render::draw_context&) override {}
	};

	using recording_host_ptr = std::shared_ptr<recording_host>;

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
				check_dialog();
				check_pane(_popup, false, "a popup");
				check_pane(_pane, true, "a pane");
			}

			for (const auto& host : {_pane, _popup, _dialog, _main})
			{
				if (host->frame) host->frame->destroy();
				host->frame.reset();
			}
			platform::quit(_failures == 0 ? 0 : 1);
			return false;
		}

		bool wants_gpu() const override { return false; }
		bool key_down(char32_t, platform::key_state) override { return false; }
		bool text_input(std::string_view) override { return false; }
		ui::focus_mode focus_mode() const override { return ui::focus_mode::none; }
		void track_menu(const ui::frame_ptr&, core::recti, const std::vector<ui::command_ptr>&) override {}

	private:
		recording_host_ptr _main = std::make_shared<recording_host>();
		recording_host_ptr _dialog = std::make_shared<recording_host>();
		recording_host_ptr _popup = std::make_shared<recording_host>();
		recording_host_ptr _pane = std::make_shared<recording_host>();
		int _failures = 0;

		void check(const bool ok, const std::string_view what)
		{
			platform::write_stdout(std::format("{} {}\n", ok ? "PASS" : "FAIL", what));
			if (!ok) ++_failures;
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
		}

		void check_pane(const recording_host_ptr& host, const bool child, const std::string_view name)
		{
			ui::frame_style style;
			style.child = child;
			style.no_focus = !child;
			host->frame = _main->frame->create_frame(host, style);
			check(host->frame != nullptr, std::format("{} is made in the application's window", name));
			if (!host->frame) return;
			check(host->layouts_before_frame == 0, std::format("{} is not laid out before its host holds it", name));

			const auto before = host->layouts;
			host->frame->window_bounds({0, 0, 160, 80}, false);
			check(host->layouts > before, std::format("placing {} lays it out", name));
		}
	};
}

int main(const int argc, char** argv)
{
	if (argc != 2 || std::strcmp(argv[1], "--check-frame-creation") != 0) return 1;
	core::set_app_name("libdesktop-ui-smoke");
	return platform::run(std::make_shared<frame_creation_check>(), argc, argv);
}
