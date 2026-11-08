// This file is part of libdesktop, a C++20 platform layer for desktop applications
// Copyright 2026  Zac Walker
//
// This program is free software; you can redistribute it and / or modify it
// under the terms of the LGPL License either version 2.1 or later.
// License details are available at https://www.gnu.org/licenses/lgpl-2.1.html
// This program is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY

// Purpose: Headless menu-bar navigation, system-key routing, and recursive accelerator tests.

#include "libdesktop/ui.h"
#include "libdesktop/ui/controls.h"
#include "libdesktop/ui/menu_bar.h"
#include "test.h"

using namespace std::literals;

namespace
{
	class fixed_measure final : public render::measure_context
	{
	public:
		core::sizei measure_text(const std::string_view text, const render::font_face, const render::text_style, const int,
		                         const int = 0) override
		{
			return {static_cast<int>(text.size()) * 8, 20};
		}

		int text_line_height(const render::font_face) override { return 20; }
		render::text_layout_ptr create_text_layout(const render::font_face) override { return {}; }
	};

	class recording_listener final : public ui::frame_host
	{
	public:
		std::vector<std::pair<ui::element_ptr, core::pointi>> roots;

		void visit_elements(const std::function<void(const ui::element_ptr&, core::pointi)>& visit) override
		{
			for (const auto& [root, offset] : roots) visit(root, offset);
		}

		void on_window_layout(render::measure_context&, core::sizei, bool) override {}
		void on_window_paint(render::draw_context&) override {}
	};

	class key_probe final : public ui::control
	{
	public:
		int key_count = 0;

		explicit key_probe(const bool typing)
		{
			traits.typing = typing;
		}

		core::sizei measure_content(render::measure_context&, const int) const override { return {120, 24}; }
		void paint(render::draw_context&, const bool) const override {}
		bool on_key(const int, const platform::key_state) override
		{
			++key_count;
			return false;
		}
	};

	class test_host final : public ui::control_host
	{
	public:
		fixed_measure metrics_impl;
		std::shared_ptr<recording_listener> events = std::make_shared<recording_listener>();
		ui::input_manager_ptr input;
		std::vector<std::vector<ui::command_ptr>> menus;
		bool keyboard = false;

		test_host() : input(std::make_shared<ui::input_manager>(this)) {}
		~test_host() override { input->detach(); }

		render::measure_context* measure() override { return &metrics_impl; }
		void invalidate(core::recti) override {}
		bool has_keyboard_focus() const override { return keyboard; }
		void take_keyboard_focus() override
		{
			keyboard = true;
			input->keyboard_focus(true);
		}
		void capture_pointer(bool) override {}
		void set_cursor(platform::cursor) override {}
		core::pointi client_to_screen(const core::pointi loc) const override { return loc + core::pointi{100, 200}; }
		void track_menu(core::recti, const std::vector<ui::command_ptr>& commands) override { menus.push_back(commands); }
		ui::frame_ptr create_popup(const ui::frame_host_weak_ptr&, const render::color_style&) override { return nullptr; }
		std::shared_ptr<ui::frame_host> listener() const override { return events; }
		void text_input(bool, core::recti) override {}
		void caret_timer(bool, uint32_t) override {}
		bool is_dialog() const override { return false; }
		void close(bool) override {}

		template <class T>
		std::shared_ptr<T> add(const std::shared_ptr<T>& control, const core::recti bounds)
		{
			events->roots.emplace_back(control, core::pointi{});
			control->is_visible(true);
			control->layout(metrics_impl, bounds);
			input->layout();
			return control;
		}
	};

	ui::command_ptr make_leaf(const char32_t key, const uint32_t modifiers, bool& invoked)
	{
		auto result = std::make_shared<ui::command>();
		result->text = "Open";
		result->kba.push_back({key, modifiers});
		result->invoke = [&invoked] { invoked = true; };
		return result;
	}

	ui::command_ptr make_menu(const std::string_view text, std::vector<ui::command_ptr> items)
	{
		auto result = std::make_shared<ui::command>();
		result->text = text;
		result->menu = [items = std::move(items)] { return items; };
		return result;
	}

	platform::key_state alt_key()
	{
		platform::key_state result;
		result.alt = true;
		return result;
	}

	void press_alt(test_host& host)
	{
		assert_equal(true, host.input->system_key(static_cast<int>(platform::keys::ALT), {}, true), "Alt down");
		assert_equal(true, host.input->system_key(static_cast<int>(platform::keys::ALT), {}, false), "Alt up");
	}

	void should_dispatch_nested_accelerators_and_numpad()
	{
		bool invoked = false;
		const auto leaf = make_leaf(U'O', platform::keyboard_accelerator_t::control, invoked);
		auto file = make_menu("&File", {leaf});

		platform::key_state control;
		control.control = true;
		assert_equal(true, ui::dispatch_accelerator({file}, U'O', control), "nested accelerator runs");
		assert_equal(true, invoked, "nested command invoked");

		bool disabled_invoked = false;
		bool invisible_invoked = false;
		bool digit_invoked = false;
		const auto disabled = make_leaf(U'D', 0, disabled_invoked);
		const auto invisible = make_leaf(U'I', 0, invisible_invoked);
		const auto digit = make_leaf(U'1', 0, digit_invoked);
		disabled->enable = false;
		invisible->visible = false;
		const auto state = make_menu("&State", {disabled, invisible, digit});

		assert_equal(false, ui::dispatch_accelerator({state}, U'D', {}), "disabled command is skipped");
		assert_equal(false, ui::dispatch_accelerator({state}, U'I', {}), "invisible command is skipped");
		assert_equal(true, ui::dispatch_accelerator({state}, platform::keys::numpad0 + 1, {}),
		             "numpad digit is normalized");
		assert_equal(true, digit_invoked, "numpad command invoked");
		assert_equal(false, disabled_invoked, "disabled command stayed idle");
		assert_equal(false, invisible_invoked, "invisible command stayed idle");
	}

	void should_not_open_speed_for_alt_f4()
	{
		bool invoked = false;
		test_host host;
		host.add(ui::make_menu_bar({make_menu("&Speed", {make_leaf(U'S', 0, invoked)})}), {0, 0, 320, 32});
		host.input->system_key(static_cast<int>(platform::keys::ALT), {}, true);
		host.input->system_key(static_cast<int>(platform::keys::F4), alt_key(), true);
		host.input->system_key(static_cast<int>(platform::keys::ALT), {}, false);
		assert_equal(0, static_cast<int>(host.menus.size()), "Alt+F4 did not open a mnemonic title");
		assert_equal(false, host.input->in_system_mode(), "Alt+F4 did not enter menu mode");
	}

	void should_not_enter_menu_mode_after_alt_enter()
	{
		test_host host;
		host.add(ui::make_menu_bar({make_menu("&File", {std::make_shared<ui::command>()})}), {0, 0, 320, 32});
		host.input->system_key(static_cast<int>(platform::keys::ALT), {}, true);
		host.input->key_down(static_cast<int>(platform::keys::RETURN), {});
		host.input->system_key(static_cast<int>(platform::keys::ALT), {}, false);
		assert_equal(false, host.input->in_system_mode(), "Alt+Enter did not enter menu mode");
	}

	void should_not_enter_menu_mode_after_alt_click()
	{
		test_host host;
		host.add(ui::make_menu_bar({make_menu("&File", {std::make_shared<ui::command>()})}), {0, 0, 320, 32});
		host.input->system_key(static_cast<int>(platform::keys::ALT), {}, true);
		host.input->mouse_down({300, 300}, {});
		host.input->system_key(static_cast<int>(platform::keys::ALT), {}, false);
		assert_equal(false, host.input->in_system_mode(), "Alt+click did not enter menu mode");
	}

	void should_toggle_menu_mode_with_two_alt_taps()
	{
		test_host host;
		host.add(ui::make_menu_bar({make_menu("&File", {std::make_shared<ui::command>()})}), {0, 0, 320, 32});
		press_alt(host);
		assert_equal(true, host.input->in_system_mode(), "first Alt tap enters menu mode");
		press_alt(host);
		assert_equal(false, host.input->in_system_mode(), "second Alt tap leaves menu mode");
	}

	void should_keep_editor_focus_when_clicking_title()
	{
		test_host host;
		const auto editor = host.add(std::make_shared<key_probe>(true), {0, 40, 320, 80});
		host.add(ui::make_menu_bar({make_menu("&File", {std::make_shared<ui::command>()})}), {0, 0, 320, 32});
		assert_equal(true, host.input->focus(editor), "editor focused before title click");
		host.input->mouse_down({4, 4}, {});
		assert_equal(true, host.input->focused() == editor, "menu title did not steal editor focus");
	}

	void should_swallow_menu_keys_before_typing_control()
	{
		test_host host;
		const auto editor = host.add(std::make_shared<key_probe>(true), {0, 40, 320, 80});
		host.add(ui::make_menu_bar({make_menu("&File", {std::make_shared<ui::command>()})}), {0, 0, 320, 32});
		host.input->focus(editor);
		host.input->system_key(static_cast<int>(platform::keys::F10), {}, true);
		assert_equal(true, host.input->focused() == editor, "F10 menu mode keeps editor focus");
		host.input->system_key(static_cast<int>(platform::keys::ESCAPE), {}, true);
		host.input->system_key(static_cast<int>(platform::keys::F10), {}, true);
		host.input->system_key(static_cast<int>(U'Z'), {}, true);
		host.input->system_key(static_cast<int>(platform::keys::DOWN), {}, true);
		assert_equal(0, editor->key_count, "menu mode swallowed Escape, letters and Down");
	}

	void should_move_titles_and_preserve_popup_horizontal_keys()
	{
		bool first_invoked = false;
		bool second_invoked = false;
		const auto first_item = make_leaf(U'1', 0, first_invoked);
		const auto second_item = make_leaf(U'2', 0, second_invoked);
		test_host host;
		host.add(ui::make_menu_bar({make_menu("&File", {first_item}), make_menu("&View", {second_item})}),
		         {0, 0, 320, 32});
		host.input->system_key(static_cast<int>(platform::keys::F10), {}, true);
		host.input->system_key(static_cast<int>(platform::keys::RIGHT), {}, true);
		host.input->system_key(static_cast<int>(platform::keys::DOWN), {}, true);
		assert_equal(1, static_cast<int>(host.menus.size()), "Right moved to the next title");
		assert_equal(true, host.menus.front().front() == second_item, "next title opened its menu");

		class menu_probe_host final : public ui::menu_host
		{
		public:
			fixed_measure metrics;
			bool submenu_opened = false;
			bool submenu_closed = false;

			render::measure_context* menu_measure() override { return &metrics; }
			ui::frame_ptr create_menu_popup(const ui::frame_host_weak_ptr&, const ui::frame_ptr&) override
			{
				return std::make_shared<ui::null_frame>();
			}
			core::recti menu_work_area(core::recti) override { return {0, 0, 1000, 800}; }
			void run_menu_loop(ui::menu_session& session) override
			{
				session.key_down(static_cast<int>(platform::keys::DOWN), {});
				assert_equal(true, session.takes_horizontal_key(static_cast<int>(platform::keys::RIGHT)),
				             "Right belongs to a submenu row");
				session.key_down(static_cast<int>(platform::keys::RIGHT), {});
				submenu_opened = session.popups().size() == 2;
				assert_equal(true, session.takes_horizontal_key(static_cast<int>(platform::keys::LEFT)),
				             "Left belongs to an open submenu");
				session.key_down(static_cast<int>(platform::keys::LEFT), {});
				submenu_closed = session.popups().size() == 1;
				session.cancel();
			}
		};

		const auto child = make_leaf(U'C', 0, first_invoked);
		const auto submenu = make_menu("Submenu", {child});
		menu_probe_host menu_host;
		ui::track_drawn_menu(menu_host, {100, 100, 140, 132}, {0, 0, 500, 500}, {submenu});
		assert_equal(true, menu_host.submenu_opened, "Right opened the selected submenu");
		assert_equal(true, menu_host.submenu_closed, "Left closed the selected submenu");
	}
}

void register_ui_menu_bar_tests(test_registry& tests)
{
	tests.add("Should dispatch nested accelerators and normalize numpad"s, should_dispatch_nested_accelerators_and_numpad);
	tests.add("Should reject Alt+F4 as a mnemonic"s, should_not_open_speed_for_alt_f4);
	tests.add("Should reject Alt+Enter as a menu tap"s, should_not_enter_menu_mode_after_alt_enter);
	tests.add("Should reject Alt+click as a menu tap"s, should_not_enter_menu_mode_after_alt_click);
	tests.add("Should toggle menu mode with two Alt taps"s, should_toggle_menu_mode_with_two_alt_taps);
	tests.add("Should preserve editor focus on title click"s, should_keep_editor_focus_when_clicking_title);
	tests.add("Should swallow menu keys before typing controls"s, should_swallow_menu_keys_before_typing_control);
	tests.add("Should preserve popup horizontal navigation"s, should_move_titles_and_preserve_popup_horizontal_keys);
}
