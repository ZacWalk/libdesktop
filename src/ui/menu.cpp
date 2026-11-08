// This file is part of libdesktop, a C++20 platform layer for desktop applications
// Copyright 2026  Zac Walker
// 
// This program is free software; you can redistribute it and / or modify it
// under the terms of the LGPL License either version 2.1 or later.
// License details are available at https://www.gnu.org/licenses/lgpl-2.1.html
// This program is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY

// Purpose: The drawn menu: a column of rows - commands with their checks, accelerator text and submenus,
// separators, rows of buttons laid out by flex, and content its owner draws - each measured, laid out,
// drawn and hit by itself, in popups opened beside a button or at a point, driven by the pointer and the
// keyboard. A dimmed command says nothing of why: that is the toolbar's bubble. The popups and the
// message loop are the window layer's, through menu_host; everything else is the same on every platform.

#include "libdesktop/ui.h"
#include "libdesktop/ui/elements.h"
#include "controls_internal.h"

namespace ui
{
	namespace
	{
		constexpr size_t max_menu_depth = 4;
		constexpr int menu_border = 3;
		constexpr uint32_t submenu_delay_ms = 250;
		// Content is laid out no wider than this, unless the menu's commands make the menu wider.
		constexpr int content_width = 360;

		// A button in a row of them, in device-independent pixels: its name under its icon, as a command
		// bar shows it. A long name wraps rather than widening every button of the row.
		constexpr int button_padding_x = 10;
		constexpr int button_padding_top = 10;
		constexpr int button_icon_gap = 4;
		constexpr int button_padding_bottom = 10;
		constexpr int button_max_width = 112;

		class drawn_menu;

		// One button of a row of them. The row is an element container that flex lays out, giving every
		// button an equal share of the menu's width; the menu draws the selection behind the buttons and
		// the lines between them.
		class menu_button final : public element
		{
		public:
			command_ptr command;

			explicit menu_button(command_ptr c) : command(std::move(c))
			{
				padding(0);
				flex.basis = 0;
				flex.grow = 1.0f;
			}

			// As wide as its name or icon asks, within the cap; as tall as its name wrapped to its share.
			core::sizei measure(render::measure_context& mc, const int width_limit) const override
			{
				const auto s = mc.scale_factor;
				const auto pad = drawn::scaled(button_padding_x, s);
				const auto shown = parse_mnemonic(command->text).text;
				const auto has_name = !shown.empty();
				const auto name = has_name
					                  ? mc.measure_text(shown, render::font_face::dialog,
					                                    render::text_style::single_line, 10000).cx
					                  : 0;
				const auto width = std::min(std::max(name, drawn::icon_cxy(s)) + pad * 2,
				                            drawn::scaled(button_max_width, s));
				const auto wrap = std::max(1, (width_limit > 0 ? width_limit : width) - pad * 2);
				const auto name_height = has_name
					                         ? mc.measure_text(shown, render::font_face::dialog,
					                                           render::text_style::multiline_center, wrap).cy
					                         : 0;
				return {width, drawn::scaled(button_padding_top + button_icon_gap + button_padding_bottom, s) +
				        drawn::icon_cxy(s) + name_height};
			}

			void render(render::draw_context& dc, const core::pointi element_offset) const override
			{
				const auto s = dc.scale_factor;
				const auto cell = bounds.offset(element_offset);
				const auto base = command->clr ? core::average(command->clr, style::colors.menu_text) : style::colors.menu_text;
				const auto clr = drawn::solid(command->enable ? base : core::average(style::colors.menu_background, base));
				const auto icon_top = cell.top + drawn::scaled(button_padding_top, s);
				const core::recti icon(cell.left, icon_top, cell.right, icon_top + drawn::icon_cxy(s));
				const auto pad = drawn::scaled(button_padding_x, s);
				const core::recti name(cell.left + pad, icon.bottom + drawn::scaled(button_icon_gap, s), cell.right - pad,
				                 cell.bottom - drawn::scaled(button_padding_bottom, s));

				drawn::draw_icon(dc, command->icon, icon, clr);
				dc.draw_text(parse_mnemonic(command->text).text, name, render::font_face::dialog,
				             render::text_style::multiline_center, clr, {});
			}
		};

		// One row of a menu: a command, a separator, a row of buttons or content its owner draws. Each knows
		// its own size, and is drawn and hit where the menu stacked it.
		struct menu_row
		{
			enum class kind
			{
				item,
				separator,
				buttons,
				content
			};

			kind type = kind::item;
			command_ptr command;
			// A row of buttons: the container that lays them out, and each shown button's element.
			std::shared_ptr<elements> row;
			std::vector<std::shared_ptr<menu_button>> cells;
			core::recti bounds;
			// An item's height: its line of text with room around it.
			int line_height = 0;

			bool selectable() const
			{
				return type == kind::item || type == kind::buttons;
			}

			bool has_submenu() const
			{
				return type == kind::item && command && static_cast<bool>(command->menu);
			}
		};

		// One level of the menu, drawn in its own popup.
		class menu_popup final : public frame_host, public std::enable_shared_from_this<menu_popup>
		{
		public:
			using kind = menu_row::kind;

			drawn_menu* owner_menu = nullptr;
			size_t level = 0;
			std::vector<menu_row> rows;
			int selected = -1;
			// The button selected in a selected row of them.
			int selected_cell = 0;
			frame_ptr frame;
			core::recti screen;
			core::sizei extent;
			double scale = 1.0;

			// A row the pointer rests on whose submenu opens, or a level to close, once it has rested.
			int pending = -2;
			uint32_t pending_since = 0;
			// The row of the level before whose submenu this is. The hover selection moves on at once, so it
			// cannot say which submenu is open.
			int opened_from = -1;

			explicit menu_popup(const std::vector<command_ptr>& commands)
			{
				for (const auto& c : commands)
				{
					menu_row r;
					r.command = c;

					if (!c) r.type = kind::separator;
					else if (c->content) r.type = kind::content;
					else if (!c->buttons.empty())
					{
						r.type = kind::buttons;
						r.row = std::make_shared<elements>();
						r.row->flex_container.wrap = flex_wrap::no_wrap;
						r.row->flex_container.align_items = flex_align::stretch;

						for (const auto& b : c->buttons)
						{
							if (!b || !b->visible) continue;
							r.cells.push_back(std::make_shared<menu_button>(b));
							r.row->add(r.cells.back());
						}
					}

					rows.push_back(std::move(r));
				}
			}

			bool valid(const int i) const
			{
				return i >= 0 && i < static_cast<int>(rows.size());
			}

			bool has_submenu(const int i) const
			{
				return valid(i) && rows[static_cast<size_t>(i)].has_submenu();
			}

			// What choosing the selection chooses: an item, or the selected button of a row of them.
			command_ptr selected_command() const
			{
				if (!valid(selected)) return nullptr;
				const auto& r = rows[static_cast<size_t>(selected)];
				if (r.type != kind::buttons) return r.command;
				return selected_cell >= 0 && selected_cell < static_cast<int>(r.cells.size())
					       ? r.cells[static_cast<size_t>(selected_cell)]->command
					       : nullptr;
			}

			core::sizei measure(render::measure_context& mc)
			{
				scale = mc.scale_factor;
				const auto border = drawn::scaled(menu_border, scale);
				const auto inset = drawn::scaled(4, scale);
				auto width = 0;

				// The width each row asks for...
				for (auto& r : rows)
				{
					if (r.type == kind::item)
					{
						const auto text = mc.measure_text(parse_mnemonic(r.command->text).text, render::font_face::dialog,
						                                  render::text_style::single_line, 10000);
						const auto accelerator = r.command->keyboard_accelerator_text.empty()
							                         ? core::sizei{}
							                         : mc.measure_text(r.command->keyboard_accelerator_text,
							                                           render::font_face::dialog,
							                                           render::text_style::single_line, 10000);
						width = std::max(width, text.cx + accelerator.cx + drawn::scaled(64, scale));
						r.line_height = std::max(text.cy + drawn::scaled(8, scale), drawn::scaled(20, scale));
					}
					else if (r.type == kind::buttons)
					{
						// The buttons share the row equally, so the widest one sets every share.
						auto cell = 0;
						for (const auto& b : r.cells) cell = std::max(cell, b->measure(mc, 0).cx);
						width = std::max(width, inset * 2 + cell * static_cast<int>(r.cells.size()));
					}
					else if (r.type == kind::content)
					{
						width = std::max(width, r.command->content->measure(mc, drawn::scaled(content_width, scale)).cx +
						                 inset * 2);
					}
				}

				extent = {width + border * 2, 0};
				auto y = border;

				// ...then each row's height at the width they make together.
				for (auto& r : rows)
				{
					auto h = 0;

					switch (r.type)
					{
					case kind::separator:
						h = drawn::scaled(8, scale);
						break;

					case kind::item:
						h = r.line_height;
						break;

					case kind::buttons:
						{
							const auto left = border + inset;
							const auto right = extent.cx - border - inset;
							h = r.row->measure(mc, right - left).cy;
							r.row->layout(mc, core::recti(left, y, right, y + h));
						}
						break;

					case kind::content:
						h = r.command->content->measure(mc, std::max(1, width - inset * 2)).cy + inset * 2;
						break;
					}

					r.bounds = {border, y, extent.cx - border, y + h};
					y += h;
				}

				extent.cy = y + border;
				layout_content(mc);
				return extent;
			}

			// Content is measured and laid out again with each context it is drawn with, so the text it
			// keeps is the popup's own.
			void layout_content(render::measure_context& mc) const
			{
				const auto inset = drawn::scaled(4, scale);

				for (const auto& r : rows)
				{
					if (r.type != kind::content) continue;
					const auto inner = r.bounds.inflate(-inset);
					r.command->content->measure(mc, inner.width());
					r.command->content->layout(mc, inner);
				}
			}

			// The row and the button under loc; -1 where nothing there is chosen.
			std::pair<int, int> row_at(const core::pointi loc) const
			{
				for (size_t i = 0; i < rows.size(); ++i)
				{
					const auto& r = rows[i];
					if (!r.bounds.contains(loc)) continue;
					if (r.type == kind::item) return {static_cast<int>(i), 0};

					if (r.type == kind::buttons)
					{
						for (size_t c = 0; c < r.cells.size(); ++c)
						{
							if (r.cells[c]->bounds.contains(loc)) return {static_cast<int>(i), static_cast<int>(c)};
						}
					}

					return {-1, 0};
				}

				return {-1, 0};
			}

			core::recti row_screen_bounds(const int i) const
			{
				if (!valid(i)) return screen;
				return rows[static_cast<size_t>(i)].bounds.offset(screen.top_left());
			}

			void select(const int i, const int cell = 0)
			{
				if (selected == i && selected_cell == cell) return;
				selected = i;
				selected_cell = std::max(0, cell);
				if (frame) frame->invalidate();
			}

			// Moves the selection to the next row that can be chosen, wrapping at the ends.
			void step(const int direction)
			{
				const auto count = static_cast<int>(rows.size());
				if (count == 0) return;
				auto i = selected;

				for (auto n = 0; n < count; ++n)
				{
					i = i < 0 ? (direction > 0 ? 0 : count - 1) : (i + direction + count) % count;

					if (rows[static_cast<size_t>(i)].selectable())
					{
						select(i);
						return;
					}
				}
			}

			void select_edge(const bool last)
			{
				selected = -1;
				step(last ? -1 : 1);
			}

			// Left and Right move along a row of buttons; false at either end, or anywhere else.
			bool can_move_cell(const int direction) const
			{
				if (!valid(selected)) return false;
				const auto& r = rows[static_cast<size_t>(selected)];
				const auto next = selected_cell + direction;
				return r.type == kind::buttons && next >= 0 && next < static_cast<int>(r.cells.size());
			}

			bool move_cell(const int direction)
			{
				if (!can_move_cell(direction)) return false;
				select(selected, selected_cell + direction);
				return true;
			}

			// The next item or button whose name starts with the letter typed.
			void select_by_letter(const char32_t letter)
			{
				const auto starts_with = [letter](const command_ptr& c)
				{
					if (!c) return false;
					const auto shown = parse_mnemonic(c->text).text;
					if (shown.empty()) return false;
					auto t = std::string_view(shown).begin();
					return core::to_upper(core::pop_utf8_char(t, std::string_view(shown).end())) == core::to_upper(letter);
				};

				const auto count = static_cast<int>(rows.size());

				for (auto n = 1; n <= count; ++n)
				{
					const auto i = ((selected < 0 ? -1 : selected) + n + count) % count;
					const auto& r = rows[static_cast<size_t>(i)];

					if (r.type == kind::item && starts_with(r.command))
					{
						select(i);
						return;
					}

					if (r.type == kind::buttons)
					{
						for (size_t c = 0; c < r.cells.size(); ++c)
						{
							if (starts_with(r.cells[c]->command))
							{
								select(i, static_cast<int>(c));
								return;
							}
						}
					}
				}
			}

			// The item rows whose label marks this key with '&', top to bottom.
			std::vector<int> rows_marked(const char32_t key) const
			{
				std::vector<int> result;
				for (size_t i = 0; i < rows.size(); ++i)
				{
					const auto& r = rows[i];
					if (r.type == kind::item && r.command && parse_mnemonic(r.command->text).key == key)
						result.push_back(static_cast<int>(i));
				}
				return result;
			}

			void on_window_layout(render::measure_context& mc, const core::sizei e, const bool is_minimized) override
			{
				if (mc.scale_factor != scale || e != extent) measure(mc);
				else layout_content(mc);
			}

			void on_window_paint(render::draw_context& dc) override
			{
				const auto s = dc.scale_factor;
				const auto border = drawn::scaled(menu_border, s);
				const auto background = style::colors.menu_background;
				const auto lit = core::lighten(background, 0.22f);
				const auto rule = drawn::solid(core::average(background, style::colors.view_background));
				const auto thickness = std::max(1, drawn::scaled(1, s));
				const auto padding = drawn::scaled(2, s);
				const auto icon_column = drawn::icon_cxy(s) + padding * 2;
				const auto inset = drawn::scaled(4, s);

				dc.clear(drawn::solid(lit));
				dc.draw_rect(core::recti(0, 0, extent.cx, extent.cy).inflate(-border), drawn::solid(background));

				for (size_t i = 0; i < rows.size(); ++i)
				{
					const auto& row = rows[i];
					const auto r = row.bounds;
					const auto is_selected = static_cast<int>(i) == selected;

					if (row.type == kind::separator)
					{
						const auto y = r.center().y - thickness / 2;
						dc.draw_rect({r.left + inset * 2, y, r.right - inset * 2, y + thickness}, rule);
						continue;
					}

					if (row.type == kind::content)
					{
						row.command->content->render(dc, {});
						continue;
					}

					if (row.type == kind::buttons)
					{
						const auto chosen = is_selected ? selected_cell : -1;
						if (chosen >= 0 && chosen < static_cast<int>(row.cells.size()))
						{
							dc.draw_rect(row.cells[static_cast<size_t>(chosen)]->bounds, drawn::solid(lit));
						}

						// A line between neighbouring buttons as tall as their icons and names, but not beside
						// the selected one.
						for (auto c = 1; c < static_cast<int>(row.cells.size()); ++c)
						{
							if (c == chosen || c - 1 == chosen) continue;
							const auto cell = row.cells[static_cast<size_t>(c)]->bounds;
							dc.draw_rect({cell.left, cell.top + drawn::scaled(button_padding_top, s), cell.left + thickness,
							              cell.bottom - drawn::scaled(button_padding_bottom, s)}, rule);
						}

						row.row->render(dc, {});
						continue;
					}

					const auto& c = row.command;
					const auto text_base = c->clr ? core::average(c->clr, style::colors.menu_text) : style::colors.menu_text;
					const auto text_clr = c->enable ? text_base : core::average(background, text_base);

					if (is_selected) dc.draw_rect(r, drawn::solid(lit));

					const auto icon = c->checked && c->icon == icon_index{} ? chrome_icons().check : c->icon;
					const core::recti icon_bounds(r.left, r.top, r.left + icon_column, r.bottom);

					if (icon != icon_index{})
					{
						if (c->checked) dc.draw_rect(icon_bounds, drawn::solid(core::average(background, text_base)));
						drawn::draw_icon(dc, icon, icon_bounds, drawn::solid(text_clr));
					}
					else if (c->checked)
					{
						// The application gave the chrome no check glyph, so the check is stroked rather than lost.
						dc.draw_rect(icon_bounds, drawn::solid(core::average(background, text_base)));
						drawn::draw_check_mark(dc, icon_bounds.inflate(-padding), drawn::solid(text_clr));
					}

					auto text_bounds = r;
					text_bounds.left += icon_column + padding;
					text_bounds.right -= icon_column;

					const auto label = parse_mnemonic(c->text);
					dc.draw_text(label.text, text_bounds, render::font_face::dialog, render::text_style::single_line,
					             drawn::solid(text_clr), {});
					drawn::draw_mnemonic_underline(dc, label, text_bounds.left,
					                               r.center().y - dc.text_line_height(render::font_face::dialog) / 2,
					                               drawn::solid(text_clr));

					if (!c->keyboard_accelerator_text.empty())
					{
						dc.draw_text(c->keyboard_accelerator_text, text_bounds, render::font_face::dialog,
						             render::text_style::single_line_far, drawn::solid(style::colors.menu_shortcut_text), {});
					}

					if (c->menu)
					{
						const core::recti arrow(r.right - icon_column, r.top, r.right, r.bottom);
						dc.draw_text("\xE2\x80\xBA", arrow, render::font_face::dialog, render::text_style::single_line_center,
						             drawn::solid(text_clr), {});
					}
				}
			}

			void on_mouse_move(core::pointi loc, bool is_tracking) override;
			void on_mouse_left_button_down(core::pointi loc, platform::key_state keys) override;
			void on_mouse_left_button_up(core::pointi loc, platform::key_state keys) override;
			void on_mouse_leave(core::pointi loc) override;
			std::vector<command_ptr> menu(core::pointi loc) override;
			void tick() override;
		};

		class drawn_menu final : public menu_session
		{
		public:
			menu_host& _host;
			std::vector<std::shared_ptr<menu_popup>> _levels;
			command_ptr _chosen;
			bool _done = false;

			explicit drawn_menu(menu_host& host) : _host(host)
			{
			}

			~drawn_menu() override
			{
				close_from(0);
			}

			drawn_menu(const drawn_menu&) = delete;
			drawn_menu& operator=(const drawn_menu&) = delete;

			bool done() const override
			{
				return _done;
			}

			void cancel() override
			{
				_done = true;
			}

			std::vector<frame_ptr> popups() const override
			{
				std::vector<frame_ptr> result;
				for (const auto& l : _levels) if (l->frame) result.push_back(l->frame);
				return result;
			}

			static bool is_shown(const command_ptr& c)
			{
				if (!c->visible) return false;
				if (c->buttons.empty()) return true;
				return std::ranges::any_of(c->buttons, [](const command_ptr& b) { return b && b->visible; });
			}

			// What a menu shows of commands: the visible ones, with no separator leading, trailing or
			// doubled where the commands between were hidden.
			static std::vector<command_ptr> shown(const std::vector<command_ptr>& commands)
			{
				std::vector<command_ptr> result;

				for (const auto& c : commands)
				{
					if (!c)
					{
						if (!result.empty() && result.back()) result.emplace_back(nullptr);
					}
					else if (is_shown(c))
					{
						result.push_back(c);
					}
				}

				while (!result.empty() && !result.back()) result.pop_back();
				return result;
			}

			void close_from(const size_t level)
			{
				while (_levels.size() > level)
				{
					const auto popup = _levels.back();
					_levels.pop_back();
					if (popup->frame) popup->frame->destroy();
					popup->frame.reset();
					popup->owner_menu = nullptr;
				}
			}

			static core::recti fit(core::recti r, const core::recti work)
			{
				if (work.is_empty()) return r;
				if (r.right > work.right) r = r.offset(work.right - r.right, 0);
				if (r.left < work.left) r = r.offset(work.left - r.left, 0);
				if (r.bottom > work.bottom) r = r.offset(0, work.bottom - r.bottom);
				if (r.top < work.top) r = r.offset(0, work.top - r.top);
				return r;
			}

			std::shared_ptr<menu_popup> make_popup(const size_t level, const std::vector<command_ptr>& commands)
			{
				auto* const mc = _host.menu_measure();
				if (!mc || commands.empty() || level >= max_menu_depth) return nullptr;

				auto popup = std::make_shared<menu_popup>(commands);
				popup->owner_menu = this;
				popup->level = level;
				popup->measure(*mc);
				return popup;
			}

			bool show(const std::shared_ptr<menu_popup>& popup, const core::recti r)
			{
				const auto parent = popup->level > 0 && popup->level - 1 < _levels.size()
					                    ? _levels[popup->level - 1]->frame
					                    : frame_ptr{};
				popup->frame = _host.create_menu_popup(popup, parent);
				if (!popup->frame) return false;

				popup->screen = r;
				popup->frame->window_bounds(r, true);
				popup->frame->invalidate();
				_levels.push_back(popup);
				return true;
			}

			// Below or above a button, as the window it belongs to leaves room, and at a point's top left.
			bool open_root(const std::vector<command_ptr>& commands, const core::recti button, const core::recti owner)
			{
				const auto popup = make_popup(0, shown(commands));
				if (!popup) return false;

				const auto size = popup->extent;
				const auto work = _host.menu_work_area(button);
				auto x = button.left;
				auto y = button.top;
				auto below = true;

				if (button.width() > 1)
				{
					const auto center = owner.center();
					if (center.x < button.left) x = button.right - size.cx;
					if (center.y > button.top) y = button.bottom;
					else
					{
						y = button.top - size.cy;
						below = false;
					}
				}

				core::recti r(x, y, x + size.cx, y + size.cy);

				if (!work.is_empty())
				{
					// Out of room on one side of what it opens from, a menu opens on the other.
					if (r.bottom > work.bottom && below && button.top - size.cy >= work.top)
					{
						r = core::recti(r.left, button.top - size.cy, r.right, button.top);
					}

					if (r.right > work.right && button.width() <= 1 && button.left - size.cx >= work.left)
					{
						r = core::recti(button.left - size.cx, r.top, button.left, r.bottom);
					}
				}

				return show(popup, fit(r, work));
			}

			void open_submenu(const size_t level, const int row, const bool select_first)
			{
				if (level >= _levels.size()) return;

				const auto parent = _levels[level];
				if (!parent->has_submenu(row)) return;
				const auto& command = parent->rows[static_cast<size_t>(row)].command;
				if (!command->enable) return;

				// Already open for this row.
				if (_levels.size() > level + 1 && _levels[level + 1]->opened_from == row)
				{
					parent->select(row);
					if (select_first && _levels[level + 1]->selected < 0) _levels[level + 1]->select_edge(false);
					return;
				}

				close_from(level + 1);
				parent->select(row);

				const auto popup = make_popup(level + 1, shown(command->menu()));
				if (!popup) return;
				popup->opened_from = row;

				const auto anchor = parent->row_screen_bounds(row);
				const auto border = drawn::scaled(menu_border, popup->scale);
				const auto size = popup->extent;
				const auto work = _host.menu_work_area(anchor);
				core::recti r(parent->screen.right - border, anchor.top - border, parent->screen.right - border + size.cx,
				        anchor.top - border + size.cy);

				if (!work.is_empty() && r.right > work.right && parent->screen.left + border - size.cx >= work.left)
				{
					r = core::recti(parent->screen.left + border - size.cx, r.top, parent->screen.left + border, r.bottom);
				}

				if (show(popup, fit(r, work)) && select_first) popup->select_edge(false);
			}

			void choose(const command_ptr& command)
			{
				if (!command || !command->enable) return;
				_chosen = command;
				_done = true;
			}

			// Return, Space or a click on a row: a submenu opens, anything else is chosen - for a row of
			// buttons, the selected one.
			void activate(const size_t level, const int row, const bool by_keyboard)
			{
				if (level >= _levels.size()) return;
				const auto popup = _levels[level];
				if (!popup->valid(row)) return;

				if (popup->has_submenu(row))
				{
					open_submenu(level, row, by_keyboard);
				}
				else if (popup->rows[static_cast<size_t>(row)].selectable() && popup->selected == row)
				{
					choose(popup->selected_command());
				}
			}

			bool takes_horizontal_key(const int key) const override
			{
				if (_levels.empty()) return false;
				const auto popup = _levels.back();
				const auto level = _levels.size() - 1;
				const auto k = static_cast<char32_t>(key);

				if (k == platform::keys::LEFT) return popup->can_move_cell(-1) || level > 0;
				if (k == platform::keys::RIGHT)
					return popup->can_move_cell(1) || popup->has_submenu(popup->selected);
				return false;
			}

			bool key_down(const int key, const platform::key_state keys) override
			{
				if (_levels.empty())
				{
					_done = true;
					return true;
				}

				const auto level = _levels.size() - 1;
				const auto popup = _levels.back();
				const auto k = static_cast<char32_t>(key);

				if (k == platform::keys::ESCAPE)
				{
					if (level > 0) close_from(level);
					else cancel();
				}
				else if (k == platform::keys::LEFT)
				{
					if (!popup->move_cell(-1) && level > 0) close_from(level);
				}
				else if (k == platform::keys::F10 || (keys.alt && k != platform::keys::DOWN && k != platform::keys::UP))
				{
					cancel();
				}
				else if (k == platform::keys::DOWN || k == platform::keys::UP)
				{
					popup->step(k == platform::keys::DOWN ? 1 : -1);
				}
				else if (k == platform::keys::HOME || k == platform::keys::END)
				{
					popup->select_edge(k == platform::keys::END);
				}
				else if (k == platform::keys::RIGHT)
				{
					if (!popup->move_cell(1) && popup->has_submenu(popup->selected))
					{
						open_submenu(level, popup->selected, true);
					}
				}
				else if (k == platform::keys::RETURN || k == platform::keys::SPACE)
				{
					activate(level, popup->selected, true);
				}
				else if ((k >= U'A' && k <= U'Z') || (k >= U'0' && k <= U'9'))
				{
					// A marked letter works as in a native menu: the one item marking it is chosen at once, and
					// several take turns being selected. A menu marking none selects by first letter instead.
					const auto marked = popup->rows_marked(k);
					if (marked.size() == 1)
					{
						popup->select(marked.front());
						activate(level, marked.front(), true);
					}
					else if (!marked.empty())
					{
						auto next = marked.front();
						for (const auto row : marked)
						{
							if (row > popup->selected)
							{
								next = row;
								break;
							}
						}
						popup->select(next);
					}
					else
					{
						popup->select_by_letter(k);
					}
				}

				// A menu takes every key while it is open.
				return true;
			}
		};

		void menu_popup::on_mouse_move(const core::pointi loc, const bool is_tracking)
		{
			if (!owner_menu) return;
			const auto [row, cell] = row_at(loc);
			if (row < 0 || (row == selected && cell == selected_cell)) return;

			const auto row_changed = row != selected;
			select(row, cell);

			// A submenu follows the pointer once it rests, so passing over items on the way to one leaves it open.
			if (row_changed)
			{
				pending = has_submenu(row) ? row : -1;
				pending_since = core::tick_count();
			}
		}

		void menu_popup::on_mouse_left_button_down(const core::pointi loc, const platform::key_state keys)
		{
			if (!owner_menu) return;
			const auto [row, cell] = row_at(loc);
			if (has_submenu(row)) owner_menu->open_submenu(level, row, false);
		}

		void menu_popup::on_mouse_left_button_up(const core::pointi loc, const platform::key_state keys)
		{
			if (!owner_menu) return;
			const auto [row, cell] = row_at(loc);
			if (row < 0) return;
			select(row, cell);
			owner_menu->activate(level, row, false);
		}

		void menu_popup::on_mouse_leave(const core::pointi loc)
		{
			if (!owner_menu) return;
			pending = -2;
			// The row whose submenu is open is lit again, as the way back to it, whatever the pointer passed
			// over on its way out.
			const auto& levels = owner_menu->_levels;
			select(levels.size() > level + 1 ? levels[level + 1]->opened_from : -1);
		}

		std::vector<command_ptr> menu_popup::menu(const core::pointi loc)
		{
			// A right press chooses too, as with a native menu tracked with the right button.
			if (owner_menu)
			{
				const auto [row, cell] = row_at(loc);

				if (row >= 0)
				{
					select(row, cell);
					owner_menu->activate(level, row, false);
				}
			}

			return {};
		}

		void menu_popup::tick()
		{
			if (!owner_menu || pending == -2) return;
			if (core::tick_count() - pending_since < submenu_delay_ms) return;

			const auto row = pending;
			pending = -2;

			if (row >= 0) owner_menu->open_submenu(level, row, false);
			else owner_menu->close_from(level + 1);
		}
	}
}

void ui::track_drawn_menu(menu_host& host, const core::recti button_bounds, const core::recti owner_bounds,
                          const std::vector<command_ptr>& commands)
{
	command_ptr chosen;

	{
		core::scope_locked_inc active(ui::command_active);
		drawn_menu menu(host);
		if (!menu.open_root(commands, button_bounds, owner_bounds)) return;

		host.run_menu_loop(menu);
		chosen = menu._chosen;
	}

	// Invoked once the menu has gone, so a command that opens a dialog or another menu finds none in the way.
	if (chosen && chosen->invoke)
	{
		core::scope_locked_inc active(ui::command_active);
		const auto invoke = chosen->invoke;
		invoke();
	}
}