// This file is part of libdesktop, a C++20 platform layer for desktop applications
// Copyright 2026  Zac Walker
//
// This program is free software; you can redistribute it and / or modify it
// under the terms of the LGPL License either version 2.1 or later.
// License details are available at https://www.gnu.org/licenses/lgpl-2.1.html
// This program is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY

// Purpose: Draw the portable menu bar control and dispatch command accelerators through its menu tree.

#include "libdesktop/ui/menu_bar.h"
#include "controls_internal.h"

namespace
{
	constexpr int menu_title_padding = 8;

	bool accelerator_matches(const platform::keyboard_accelerator_t& accelerator, const char32_t key,
	                         const platform::key_state keys)
	{
		const auto normalized = platform::keys::normalize_numpad(key);
		const auto bound = platform::keys::normalize_numpad(accelerator.key);
		if (bound != normalized) return false;

		const auto state = accelerator.key_state;
		return ((state & platform::keyboard_accelerator_t::control) != 0) == keys.control &&
			((state & platform::keyboard_accelerator_t::shift) != 0) == keys.shift &&
			((state & platform::keyboard_accelerator_t::alt) != 0) == keys.alt;
	}

	bool dispatch_commands(const std::vector<ui::command_ptr>& commands, const char32_t key,
	                       const platform::key_state keys)
	{
		for (const auto& command : commands)
		{
			if (!command || !command->visible || !command->enable) continue;

			for (const auto& accelerator : command->kba)
			{
				if (!accelerator_matches(accelerator, key, keys)) continue;
				if (!command->invoke) break;
				core::scope_locked_inc active(ui::command_active);
				command->invoke();
				return true;
			}

			if (command->menu && dispatch_commands(command->menu(), key, keys)) return true;
		}

		return false;
	}

	class menu_bar_control final : public ui::control
	{
		std::vector<ui::command_ptr> _menus;
		int _selected = -1;
		bool _menu_mode = false;
		bool _menu_open = false;
		bool _alt_pressed = false;
		bool _alt_used = false;

		bool title_visible(const int index) const
		{
			return index >= 0 && index < static_cast<int>(_menus.size()) && _menus[static_cast<size_t>(index)] &&
				_menus[static_cast<size_t>(index)]->visible;
		}

		std::vector<int> visible_titles() const
		{
			std::vector<int> result;
			for (int i = 0; i < static_cast<int>(_menus.size()); ++i)
				if (title_visible(i)) result.push_back(i);
			return result;
		}

		int first_title() const
		{
			for (int i = 0; i < static_cast<int>(_menus.size()); ++i)
				if (title_visible(i)) return i;
			return -1;
		}

		int title_width(const ui::command& command, render::measure_context* mc, const double scale) const
		{
			const auto label = ui::parse_mnemonic(command.text);
			const auto text_width = mc
				                       ? mc->measure_text(label.text, render::font_face::dialog,
				                                         render::text_style::single_line, 100000).cx
				                       : static_cast<int>(label.text.size()) * 8;
			return text_width + ui::drawn::scaled(menu_title_padding * 2, scale);
		}

		core::recti title_bounds(const int index, render::measure_context* mc, const double scale) const
		{
			auto left = bounds.left;
			for (int i = 0; i < static_cast<int>(_menus.size()); ++i)
			{
				if (!title_visible(i)) continue;
				const auto width = title_width(*_menus[static_cast<size_t>(i)], mc, scale);
				if (i == index) return {left, bounds.top, left + width, bounds.bottom};
				left += width;
			}
			return {};
		}

		int title_at(const core::pointi loc) const
		{
			auto* const mc = metrics();
			const auto scale = mc ? mc->scale_factor : 1.0;
			auto left = bounds.left;
			for (int i = 0; i < static_cast<int>(_menus.size()); ++i)
			{
				if (!title_visible(i)) continue;
				const auto right = left + title_width(*_menus[static_cast<size_t>(i)], mc, scale);
				if (loc.x >= left && loc.x < right && loc.y >= bounds.top && loc.y < bounds.bottom) return i;
				left = right;
			}
			return -1;
		}

		int title_for_mnemonic(const char32_t key) const
		{
			for (int i = 0; i < static_cast<int>(_menus.size()); ++i)
			{
				if (!title_visible(i)) continue;
				if (ui::parse_mnemonic(_menus[static_cast<size_t>(i)]->text).key == key) return i;
			}
			return -1;
		}

		void select_title(const int index)
		{
			if (!title_visible(index) || _selected == index) return;
			_selected = index;
			invalidate();
		}

		void enter_menu_mode()
		{
			if (_menu_mode) return;
			const auto first = first_title();
			if (first < 0) return;
			_menu_mode = true;
			_selected = first;
			invalidate();
		}

		void leave_menu_mode()
		{
			if (!_menu_mode && _selected < 0) return;
			_menu_mode = false;
			_selected = -1;
			invalidate();
		}

		void move_title(const int direction)
		{
			const auto titles = visible_titles();
			if (titles.empty()) return;
			const auto current = std::ranges::find(titles, _selected);
			if (current == titles.end())
			{
				_selected = titles.front();
			}
			else
			{
				const auto index = static_cast<int>(current - titles.begin());
				const auto next = (index + (direction > 0 ? 1 : static_cast<int>(titles.size()) - 1)) %
					static_cast<int>(titles.size());
				_selected = titles[static_cast<size_t>(next)];
			}
			invalidate();
		}

		void open_title(const int index)
		{
			if (!title_visible(index)) return;
			int current = index;

			for (;;)
			{
				if (!title_visible(current)) break;
				const auto& command = _menus[static_cast<size_t>(current)];
				if (!command->enable || !command->menu) break;

				const auto items = command->menu();
				if (items.empty()) break;

				_selected = current;
				_menu_mode = true;
				_menu_open = true;
				_alt_used = true;
				auto* const mc = metrics();
				const auto scale = mc ? mc->scale_factor : 1.0;
				const auto screen = title_bounds(current, mc, scale).offset(screen_offset());

				if (const auto in = input()) in->track_menu(screen, items);
				else if (const auto h = host()) h->track_menu(screen, items);

				// The frame's menu filter cancels the old session after a left/right title move.
				// Re-enter the loop for the newly selected title without leaving menu mode.
				if (_menu_mode && _selected >= 0 && _selected != current)
				{
					current = _selected;
					continue;
				}
				break;
			}

			_menu_open = false;
			_alt_pressed = false;
			leave_menu_mode();
		}

		bool menu_key(const char32_t key)
		{
			if (!_menu_mode) return false;
			if (key == platform::keys::LEFT)
			{
				move_title(-1);
				return true;
			}
			if (key == platform::keys::RIGHT)
			{
				move_title(1);
				return true;
			}
			if (key == platform::keys::DOWN || key == platform::keys::RETURN)
			{
				open_title(_selected >= 0 ? _selected : first_title());
				return true;
			}
			if (key == platform::keys::ESCAPE)
			{
				leave_menu_mode();
				return true;
			}
			if (const auto title = title_for_mnemonic(key); title >= 0)
			{
				_selected = title;
				open_title(title);
				return true;
			}
			return false;
		}

	public:
		explicit menu_bar_control(std::vector<ui::command_ptr> menus) : _menus(std::move(menus))
		{
			traits.focus_on_click = false;
			traits.first_focus_candidate = false;
		}

		core::sizei measure_content(render::measure_context& mc, const int) const override
		{
			const auto scale = mc.scale_factor;
			int width = 0;
			for (const auto& command : _menus)
				if (command && command->visible) width += title_width(*command, &mc, scale);
			const auto height = mc.text_line_height(render::font_face::dialog) + ui::drawn::scaled(menu_title_padding, scale);
			return {width, height};
		}

		void paint(render::draw_context& dc, const bool) const override
		{
			const auto scale = dc.scale_factor;
			dc.draw_rect(bounds, ui::drawn::solid(ui::style::colors.menu_background, dc.colors.alpha));
			auto left = bounds.left;
			for (int i = 0; i < static_cast<int>(_menus.size()); ++i)
			{
				const auto& command = _menus[static_cast<size_t>(i)];
				if (!command || !command->visible) continue;
				const auto width = title_width(*command, &dc, scale);
				const core::recti title(left, bounds.top, left + width, bounds.bottom);
				const auto selected = _menu_mode && _selected == i;
				if (selected)
					dc.draw_rect(title, ui::drawn::solid(ui::style::colors.dialog_selected_background, dc.colors.alpha));
				const auto base = command->enable
					                 ? (selected ? ui::style::colors.dialog_selected_text : ui::style::colors.menu_text)
					                 : core::average(ui::style::colors.menu_background, ui::style::colors.menu_text);
				const auto label = ui::parse_mnemonic(command->text);
				dc.draw_text(label.text, title, render::font_face::dialog, render::text_style::single_line_center,
				             ui::drawn::solid(base, dc.colors.alpha), {});

				if (_menu_mode || _alt_pressed)
				{
					const auto text_extent = dc.measure_text(label.text, render::font_face::dialog,
					                                         render::text_style::single_line, 100000);
					ui::drawn::draw_mnemonic_underline(dc, label, title.center().x - text_extent.cx / 2,
					                                   title.center().y - text_extent.cy / 2,
					                                   ui::drawn::solid(base, dc.colors.alpha));
				}
				left += width;
			}
		}

		bool in_system_mode() const override
		{
			return _menu_mode || _menu_open;
		}

		void cancel_system_mode() override
		{
			_alt_pressed = false;
			_alt_used = true;
			_menu_open = false;
			leave_menu_mode();
			invalidate();
		}

		bool on_system_key(const int key, const platform::key_state keys, const bool down) override
		{
			if (first_title() < 0) return false;
			const auto k = static_cast<char32_t>(key);

			if (k == platform::keys::ALT)
			{
				if (down)
				{
					_alt_pressed = true;
					_alt_used = false;
					invalidate();
					return true;
				}
				const auto tap = _alt_pressed && !_alt_used;
				_alt_pressed = false;
				if (tap)
				{
					if (_menu_mode || _menu_open)
					{
						_menu_open = false;
						leave_menu_mode();
					}
					else enter_menu_mode();
				}
				invalidate();
				return true;
			}

			if (down && _alt_pressed) _alt_used = true;

			if (k == platform::keys::F10)
			{
				if (down) enter_menu_mode();
				return true;
			}

			if (!down) return _menu_mode || _menu_open;
			if (keys.alt)
			{
				if (const auto title = title_for_mnemonic(k); title >= 0)
				{
					_alt_used = true;
					enter_menu_mode();
					_selected = title;
					open_title(title);
					return true;
				}
			}

			if (_menu_mode || _menu_open)
			{
				(void)menu_key(k);
				return true;
			}
			return false;
		}

		bool on_key(const int key, const platform::key_state) override
		{
			return menu_key(static_cast<char32_t>(key));
		}

		void on_mouse_down(const core::pointi loc, const platform::key_state) override
		{
			if (_alt_pressed) _alt_used = true;
			const auto title = title_at(loc);
			if (!title_visible(title) || !_menus[static_cast<size_t>(title)]->enable) return;
			enter_menu_mode();
			_selected = title;
			open_title(title);
		}

		void on_mouse_move(const core::pointi loc, const bool) override
		{
			if (!_menu_mode && !_menu_open) return;
			const auto title = title_at(loc);
			if (title < 0) return;
			if (_menu_open && title != _selected)
				if (auto* const h = host()) h->menu_title_hovered();
			select_title(title);
		}
	};
}

ui::control_ptr ui::make_menu_bar(std::vector<ui::command_ptr> menus)
{
	return std::make_shared<menu_bar_control>(std::move(menus));
}

bool ui::dispatch_accelerator(const std::vector<ui::command_ptr>& commands, const char32_t key,
                              const platform::key_state keys)
{
	return dispatch_commands(commands, key, keys);
}
