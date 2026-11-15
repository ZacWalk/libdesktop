// This file is part of libdesktop, a C++20 platform layer for desktop applications
// Copyright 2026  Zac Walker
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

// Purpose: The simple application: one window, a menu bar over its content, and - for a game or an
// emulator - a loop that steps it at a fixed rate, with the keys it holds and the pictures it presents.

#pragma once

#include "libdesktop/ui/elements.h"
#include "libdesktop/ui/menu_bar.h"

namespace ui
{
	// The keys a realtime application reads each step: those held, and those pressed or released since the last
	// step. A tap shorter than a step is both pressed and released in one, so none is lost. Key codes are
	// platform::keys values, below 256.
	class key_input
	{
	public:
		// A key going down; a repeat while held changes nothing.
		void down(char32_t key);
		void up(char32_t key);
		// The keyboard went elsewhere: everything held is let go.
		void release_all();
		// The step that read them is done: pressed and released start again.
		void next_step();

		bool held(char32_t key) const;
		bool pressed(char32_t key) const;
		bool released(char32_t key) const;

	private:
		static constexpr size_t key_count = 256;
		std::array<bool, key_count> _held{};
		std::array<bool, key_count> _pressed{};
		std::array<bool, key_count> _released{};
	};

	// Turns time into whole steps of a fixed length, as many as are due, but no more than max_steps at once: a
	// stall longer than that is dropped rather than caught up.
	class step_clock
	{
	public:
		explicit step_clock(double steps_per_second, int max_steps = 5);

		// The steps due once elapsed more seconds have passed; what is short of a step waits.
		int advance(double elapsed);
		double step_seconds() const { return _step; }
		// A new rate from the next step on; the time already waiting counts towards it.
		void rate(double steps_per_second);
		// How long until the next step is due.
		double until_next() const { return std::max(0.0, _step - _pending); }

	private:
		double _step = 1.0;
		double _pending = 0.0;
		int _max_steps;
	};

	struct shell_options
	{
		// The window's title; the application's name when empty.
		std::string title;
		// The client area at first and the least it may be, in pixels at 100% scale.
		core::sizei client_size{800, 600};
		core::sizei minimum_client{320, 240};
		bool resizable = true;
		// Direct3D where it works, or the software renderer.
		bool gpu = false;
		// The window's background, text and selection colours; the view's palette when unset.
		std::optional<render::color_style> colors;
		// Files may be dropped on the window.
		bool accepts_drops = false;
		// Steps a second for a realtime application, which then owns the loop: step() at that rate, and the content
		// presented after them. 0 for an application that waits for events.
		double steps_per_second = 0.0;
		// A realtime application steps on while its window is minimized, as an emulator playing a tune does;
		// otherwise it stands still, minimized() says so, and the time is not caught up after.
		bool run_minimized = false;
		// Where the window was is remembered in the application's settings, and the window opened there again.
		bool remember_placement = true;
		// A real window that paints without appearing or taking the keyboard: a headless run.
		bool offscreen = false;
	};

	// An application in one window: its menus' bar over its content, opened where it was last time, or centred,
	// with keys offered to the menus' accelerators before the application. Everything a ui::app and an
	// element_host must answer is answered here; an application overrides what it uses.
	class shell : public app, public element_host, public std::enable_shared_from_this<shell>
	{
	public:
		explicit shell(shell_options options);

		// What the window shows below the menu bar, made once just after the window is - before anyone sees it -
		// so it can read frame()'s scale.
		virtual element_ptr make_content() = 0;
		// The menu bar's menus, made once just before the content; none for no menu bar.
		virtual std::vector<command_ptr> make_menus() { return {}; }
		// Once the window is made and laid out, before it shows: false ends the application. A window that cannot
		// be made ends it with exit code 1 before this.
		virtual bool start(std::string_view /*command_line*/) { return true; }
		// Advances a realtime application by one step; keys() says what is held and was pressed during it.
		virtual void step(double /*seconds*/) {}
		// After the steps of one pass of the loop: by default the content is painted again.
		virtual void present();
		// A key the accelerators did not take: true when the application used it. Alt combinations reach
		// here before the menu bar takes them, so an application answers false for those it does not use.
		// A realtime application's held key repeats here, never a menu command it invoked.
		virtual bool key(char32_t /*key*/, platform::key_state /*keys*/) { return false; }
		// A realtime application's window was minimized and its steps stand still: what would carry on without
		// them - a looping sound - stops here.
		virtual void minimized() {}
		// The window is closing: what the application keeps is saved here.
		virtual void stop() {}

		const shell_options& options() const { return _options; }
		key_input& keys() { return _keys; }
		// A realtime application's steps a second from the next step on, as a game's speed setting changes.
		void step_rate(double steps_per_second);
		// Ends the application once the current event or step is handled; the process exits with code.
		void quit(int code = 0);
		const element_ptr& content() const { return _content; }
		const std::vector<command_ptr>& menus() const { return _menus; }
		// Where the content sits in the window's client area.
		core::recti content_bounds() const { return _content ? _content->bounds : core::recti{}; }

		// app
		bool init(std::string_view command_line) override;
		bool owns_loop() const override { return _options.steps_per_second > 0; }
		int main_loop() override;
		// Saves where the window is; an application that may refuse to close answers first and calls this when
		// it agrees.
		bool can_exit() override;
		void exit() override;
		void final_exit() override;
		// A lost graphics device frees the content's textures, which it makes again as it next paints. An application
		// that answers system events itself calls this too.
		void system_event(platform::os_event_type event) override;
		bool wants_gpu() const override { return _options.gpu; }
		bool key_down(char32_t c, platform::key_state keys) override;
		bool key_up(char32_t c, platform::key_state keys) override;
		bool text_input(std::string_view /*text*/) override { return false; }
		ui::focus_mode focus_mode() const override { return ui::focus_mode::none; }
		void track_menu(const frame_ptr& parent, core::recti bounds, const std::vector<command_ptr>& commands) override;

		// element_host
		void invoke(commands /*cmd*/) override {}
		bool is_command_checked(commands /*cmd*/) override { return false; }
		void track_menu(core::recti bounds, const std::vector<command_ptr>& commands) override;
		void controller_changed() override;
		const frame_ptr frame() const override { return _frame ? _frame : no_frame(); }
		const frame_ptr owner() override { return frame(); }
		controller_ptr controller_from_location(core::pointi loc, hit_test_context& ctx) override;
		void invalidate_element(const element_ptr& e) override;
		void invalidate(host_invalid what) override;

		// frame_host
		void on_window_layout(render::measure_context& mc, core::sizei extent, bool is_minimized) override;
		void on_window_paint(render::draw_context& dc) override;
		// The pointer moving over content with nothing to answer it, or leaving it, paints nothing again.
		void on_mouse_move(core::pointi loc, bool is_tracking) override;
		void on_mouse_leave(core::pointi loc) override;
		// The wheel goes to the element under the pointer that scrolls.
		void on_mouse_wheel(core::pointi loc, int delta, platform::key_state keys, bool& was_handled) override;
		void visit_elements(const std::function<void(const element_ptr&, core::pointi)>& visit) override;
		void activate(bool is_active) override;
		void focus_changed(bool has_focus, const control_ptr& child) override;

	protected:
		bool is_minimized() const { return _minimized; }
		// What init makes the window with, for an application to settle from its command line or settings
		// before then - an offscreen run, a size it remembers.
		shell_options& initial_options() { return _options; }

	private:
		void save_placement();
		void free_graphics();

		shell_options _options;
		key_input _keys;
		step_clock _clock;
		frame_ptr _frame;
		elements_ptr _root;
		element_ptr _content;
		std::vector<command_ptr> _menus;
		// Keys whose press a menu command took, so their repeats do not invoke it again.
		std::array<bool, 256> _accelerated{};
		bool _minimized = false;
		bool _quitting = false;
		int _exit_code = 0;
	};

	// Draws what its callback draws, clipped to its bounds, as it grows to fill what its container leaves: a
	// game's scene, a chart. The callback is handed the context and where to draw.
	class canvas_element final : public element
	{
	public:
		using draw_function = std::function<void(render::draw_context& dc, core::recti bounds)>;

		explicit canvas_element(draw_function draw);
		void render(render::draw_context& dc, core::pointi element_offset) const override;

	private:
		draw_function _draw;
	};

	// Shows a picture the application makes - an emulator's screen, a software renderer's frame - scaled to fit
	// its bounds and centred on its background. A new picture of the same size updates the texture in place and
	// re-presents the window's scene over the picture alone, without painting anything again.
	class surface_element final : public element
	{
	public:
		struct style
		{
			core::color background{0.0f, 0.0f, 0.0f, 1.0f};
			// Scales by whole multiples where the bounds allow one, so every source pixel is as big as the next.
			bool whole_multiples = false;
			// A source pixel's height over its width, for a machine whose pixels are not square.
			double pixel_aspect = 1.0;
			render::texture_sampler sampler = render::texture_sampler::point;
		};

		explicit surface_element(style s);
		explicit surface_element() : surface_element(style{}) {}

		// The next picture, in frame's window.
		void present(const core::const_surface_ptr& picture, const frame_ptr& frame);
		const core::const_surface_ptr& picture() const { return _picture; }
		// Where the picture is drawn in the window's client area, and where a client point falls on it, in its
		// own pixels; nullopt beside it.
		core::recti picture_bounds() const;
		std::optional<core::pointi> picture_point(core::pointi client) const;

		void render(render::draw_context& dc, core::pointi element_offset) const override;
		void dispatch_event(const element_event& event) override;

	private:
		style _style;
		core::const_surface_ptr _picture;
		// Each picture presented, and the last of them the texture holds: an application may draw every
		// picture into the same surface.
		uint64_t _generation = 0;
		mutable uint64_t _uploaded = 0;
		mutable render::texture_ptr _texture;
		mutable core::sizei _texture_extent;
		mutable core::recti _drawn;
	};

	// Where a picture of extent goes in bounds: scaled to fit, by whole multiples where they fit, and centred.
	core::recti fit_picture(core::sizei extent, core::recti bounds, bool whole_multiples, double pixel_aspect);

	// Lays root out at extent and draws it with the software renderer into a new surface, with no window: a
	// headless capture. On the UI thread, as any drawing is: inside platform::run, or after
	// platform::set_ui_thread() in a test's own main.
	core::surface_ptr render_to_surface(const element_ptr& root, core::sizei extent, core::color background,
	                                    double scale = 1.0);
}
