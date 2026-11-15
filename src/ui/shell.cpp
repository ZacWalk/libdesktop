// This file is part of libdesktop, a C++20 platform layer for desktop applications
// Copyright 2026  Zac Walker
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

// Purpose: The simple application, its realtime loop and keys, and the canvas and surface elements.

#include "libdesktop/ui.h"
#include "libdesktop/ui/shell.h"
#include "libdesktop/render/software.h"

#include <thread>

using namespace std::literals;

void ui::key_input::down(const char32_t key)
{
	if (key >= key_count) return;
	if (!_held[key]) _pressed[key] = true;
	_held[key] = true;
}

void ui::key_input::up(const char32_t key)
{
	if (key >= key_count) return;
	if (_held[key]) _released[key] = true;
	_held[key] = false;
}

void ui::key_input::release_all()
{
	for (size_t key = 0; key < key_count; ++key)
		if (_held[key]) up(static_cast<char32_t>(key));
}

void ui::key_input::next_step()
{
	_pressed.fill(false);
	_released.fill(false);
}

bool ui::key_input::held(const char32_t key) const
{
	return key < key_count && _held[key];
}

bool ui::key_input::pressed(const char32_t key) const
{
	return key < key_count && _pressed[key];
}

bool ui::key_input::released(const char32_t key) const
{
	return key < key_count && _released[key];
}

ui::step_clock::step_clock(const double steps_per_second, const int max_steps) : _max_steps(std::max(1, max_steps))
{
	rate(steps_per_second);
}

void ui::step_clock::rate(const double steps_per_second)
{
	// A clock with no rate steps once a second rather than dividing by nothing; the shell runs none.
	_step = steps_per_second > 0.0 ? 1.0 / steps_per_second : 1.0;
}

int ui::step_clock::advance(const double elapsed)
{
	_pending += std::max(0.0, elapsed);

	// A step is due a hair before rounding says it is, so a clock that ticks exactly a step at a time never
	// waits a whole extra step for the last bit.
	auto steps = static_cast<int>((_pending + 1e-9) / _step);

	if (steps > _max_steps)
	{
		steps = _max_steps;
		_pending = 0.0;
	}
	else
	{
		_pending = std::max(0.0, _pending - steps * _step);
	}

	return steps;
}

ui::shell::shell(shell_options options) : _options(std::move(options)), _clock(_options.steps_per_second)
{
}

void ui::shell::step_rate(const double steps_per_second)
{
	if (steps_per_second <= 0.0 || _options.steps_per_second <= 0.0) return;
	_options.steps_per_second = steps_per_second;
	_clock.rate(steps_per_second);
}

void ui::shell::quit(const int code)
{
	_exit_code = code;
	_quitting = true;
	platform::quit(code);
}

void ui::shell::system_event(const platform::os_event_type event)
{
	if (event == platform::os_event_type::graphics_device_lost)
	{
		// What the content holds belongs to the device that went; it makes it again as it next paints.
		free_graphics();
		frame()->invalidate();
	}
}

void ui::shell::free_graphics()
{
	if (_root)
		_root->dispatch_event(
			{element_event_type::free_graphics_resources, std::static_pointer_cast<element_host>(shared_from_this())});
}

bool ui::shell::init(const std::string_view command_line)
{
	_root = std::make_shared<elements>();
	_root->padding = {0, 0};
	_root->margin = {0, 0};
	_root->flex_container.direction = flex_direction::column;
	_root->flex_container.wrap = flex_wrap::no_wrap;
	_root->flex_container.align_items = flex_align::stretch;

	const auto scale = std::max(96, platform::primary_screen_dpi()) / 96.0;
	const auto scaled = [scale](const core::sizei s)
	{
		return core::sizei{core::round(s.cx * scale), core::round(s.cy * scale)};
	};

	frame_style style;
	style.drawn_caption = false;
	style.can_focus = true;
	style.can_drop = _options.accepts_drops;
	style.offscreen = _options.offscreen;
	style.resizable = _options.resizable;
	style.hardware_accelerated = _options.gpu;
	style.client_size = scaled(_options.client_size);
	style.minimum_client = scaled(_options.minimum_client);
	style.colors = _options.colors.value_or(render::color_style{style::colors.view_background, style::colors.view_text,
	                                                            style::colors.view_selected_background});

	const auto store = _options.remember_placement && !_options.offscreen ? platform::settings()
	                                                                      : platform::setting_file_ptr{};
	const auto restored = store && platform::read_placement(store, platform::placement_setting_name()).has_value();

	const auto self = shared_from_this();
	_frame = create_app_frame(std::static_pointer_cast<app>(self), store,
	                          std::static_pointer_cast<frame_host>(std::static_pointer_cast<element_host>(self)),
	                          style);

	if (!_frame)
	{
		core::log("shell", "the application's window could not be made");
		quit(1);
		return false;
	}

	_frame->window_text(_options.title.empty() ? std::string(core::app_name()) : _options.title);

	// Made once the window is, which no one sees before init returns, so they can read its scale.
	_menus = make_menus();
	_content = make_content();
	if (!_menus.empty()) _root->add(make_menu_bar(_menus));

	if (_content)
	{
		// The content takes what the menu bar leaves.
		_content->flex = _content->flex | flex_item::grow | flex_item::stretch;
		_root->add(_content);
	}

	if (!restored && !_options.offscreen)
	{
		const auto window = _frame->window_bounds();
		const auto screen = platform::desktop_bounds(true);

		if (!window.is_empty() && !screen.is_empty())
		{
			_frame->window_bounds(core::center_rect(window.extent(), screen), false);
		}
	}

	_frame->layout();
	return start(command_line);
}

int ui::shell::main_loop()
{
	auto previous = platform::monotonic_seconds();

	while (platform::pump_events())
	{
		const auto now = platform::monotonic_seconds();
		const auto elapsed = now - previous;
		previous = now;

		// Nothing is stepped while nothing can be seen, unless the application runs on; the time is not caught up
		// after.
		if (_minimized && !_options.run_minimized)
		{
			std::this_thread::sleep_for(50ms);
			continue;
		}

		const auto steps = _clock.advance(elapsed);

		for (auto i = 0; i < steps && !_quitting; ++i)
		{
			step(_clock.step_seconds());
			_keys.next_step();
		}

		if (steps > 0 && !_minimized) present();
		else std::this_thread::sleep_for(std::chrono::duration<double>(std::min(_clock.until_next(), 0.002)));
	}

	return _exit_code;
}

void ui::shell::present()
{
	if (_content && _frame) _frame->invalidate(_content->bounds);
}

void ui::shell::save_placement()
{
	if (_frame && _options.remember_placement && !_options.offscreen)
	{
		auto store = platform::settings();
		if (store) _frame->save_window_position(store);
	}
}

bool ui::shell::can_exit()
{
	// The window still stands while the application is asked; once it is gone there is no placement to save.
	save_placement();
	return true;
}

void ui::shell::exit()
{
	// A quit that closed no window leaves it standing.
	save_placement();
	stop();
}

void ui::shell::final_exit()
{
	free_graphics();

	// A frame must not outlive the renderer, which shuts down after this.
	_frame.reset();
	_root.reset();
	_content.reset();
	_menus.clear();
}

bool ui::shell::key_down(const char32_t c, const platform::key_state keys)
{
	const auto code = static_cast<size_t>(c);
	const auto accelerated = code < _accelerated.size() && _accelerated[code];

	// A game's held key does not repeat its menu command: a toggle would flicker.
	if (keys.repeat && accelerated && owns_loop()) return true;

	if (dispatch_accelerator(_menus, c, keys))
	{
		if (code < _accelerated.size()) _accelerated[code] = true;
		return true;
	}

	// Alt and F10 belong to the menu bar, which takes their release, so they are never held.
	if (!keys.alt && c != platform::keys::ALT && c != platform::keys::F10) _keys.down(c);
	return key(c, keys);
}

bool ui::shell::key_up(const char32_t c, const platform::key_state /*keys*/)
{
	const auto code = static_cast<size_t>(c);
	if (code < _accelerated.size()) _accelerated[code] = false;
	_keys.up(c);
	return false;
}

void ui::shell::track_menu(const frame_ptr& parent, const core::recti bounds, const std::vector<command_ptr>& commands)
{
	if (parent) parent->track_menu(bounds, commands);
}

void ui::shell::track_menu(const core::recti bounds, const std::vector<command_ptr>& commands)
{
	frame()->track_menu(bounds, commands);
}

void ui::shell::controller_changed()
{
	frame()->invalidate();
}

ui::controller_ptr ui::shell::controller_from_location(const core::pointi loc, hit_test_context& ctx)
{
	if (!_root) return nullptr;
	return _root->controller_from_location(std::static_pointer_cast<element_host>(shared_from_this()), loc, {}, ctx);
}

void ui::shell::invalidate_element(const element_ptr& e)
{
	frame()->invalidate(e ? e->invalidate_bounds() : core::recti{});
}

void ui::shell::invalidate(const host_invalid what)
{
	if (what && host_invalid::layout) frame()->layout();
	if (what && host_invalid::redraw) frame()->invalidate();
}

void ui::shell::on_window_layout(render::measure_context& mc, const core::sizei extent, const bool is_minimized)
{
	const auto was_minimized = _minimized;
	_minimized = is_minimized || extent.is_empty();
	if (_minimized && !was_minimized && owns_loop() && !_options.run_minimized) minimized();
	if (_minimized) return;
	_extent = extent;
	if (_root) _root->layout(mc, core::recti(extent));
}

void ui::shell::on_mouse_move(const core::pointi loc, const bool is_tracking)
{
	// Over content with nothing under the pointer to answer it, a move changes nothing on screen: the element host
	// would test again and paint the whole window for each one.
	if (!is_tracking && !_tracking && !_active_controller && !_controller_invalid && _tooltip_bounds.is_empty())
	{
		hit_test_context ctx{loc, core::recti(_extent)};

		if (!controller_from_location(loc, ctx))
		{
			update_tracking(loc, false);
			update_cursor();
			return;
		}
	}

	element_host::on_mouse_move(loc, is_tracking);
}

void ui::shell::on_mouse_leave(const core::pointi loc)
{
	// With nothing under the pointer as it went, nothing on screen changes for its going.
	if (!_tracking && !_active_controller && _tooltip_bounds.is_empty())
	{
		_hover = false;
		return;
	}

	element_host::on_mouse_leave(loc);
}

void ui::shell::on_window_paint(render::draw_context& dc)
{
	dc.clear(core::color(dc.colors.background, dc.colors.alpha));
	if (_root) _root->render(dc, {});
}

void ui::shell::on_mouse_wheel(const core::pointi loc, const int delta, const platform::key_state, bool& was_handled)
{
	if (_root && _root->wheel(loc, {}, delta))
	{
		was_handled = true;
		frame()->invalidate();
	}
}

void ui::shell::visit_elements(const std::function<void(const element_ptr&, core::pointi)>& visit)
{
	if (_root) visit(_root, {});
}

void ui::shell::activate(const bool is_active)
{
	if (!is_active) _keys.release_all();
}

void ui::shell::focus_changed(const bool has_focus, const control_ptr& /*child*/)
{
	if (!has_focus) _keys.release_all();
}

ui::canvas_element::canvas_element(draw_function draw) :
	element(element_options(flex_item::grow | flex_item::stretch)), _draw(std::move(draw))
{
	padding = {0, 0};
	margin = {0, 0};
}

void ui::canvas_element::render(render::draw_context& dc, const core::pointi element_offset) const
{
	const auto area = bounds.offset(element_offset);
	if (area.is_empty() || !_draw) return;

	render::scoped_clip clip(dc, area);
	_draw(dc, area);
}

core::recti ui::fit_picture(const core::sizei extent, const core::recti bounds, const bool whole_multiples,
                            const double pixel_aspect)
{
	if (extent.cx < 1 || extent.cy < 1 || bounds.is_empty()) return {};

	const auto aspect = pixel_aspect > 0.0 ? pixel_aspect : 1.0;
	const auto fit = std::min(bounds.width() / static_cast<double>(extent.cx), bounds.height() / (extent.cy * aspect));
	auto scale_x = fit;
	auto scale_y = fit * aspect;

	if (whole_multiples && fit >= 1.0)
	{
		scale_x = std::floor(fit);
		scale_y = std::max(1.0, std::round(scale_x * aspect));
		// Rounding the height up can overflow the bounds by a multiple; one less then fits.
		if (extent.cy * scale_y > bounds.height()) scale_y = std::max(1.0, scale_y - 1.0);
	}

	const auto width = std::max(1, core::round(extent.cx * scale_x));
	const auto height = std::max(1, core::round(extent.cy * scale_y));
	const auto left = bounds.left + (bounds.width() - width) / 2;
	const auto top = bounds.top + (bounds.height() - height) / 2;
	return {left, top, left + width, top + height};
}

ui::surface_element::surface_element(style s) :
	element(element_options(flex_item::grow | flex_item::stretch)), _style(s)
{
	padding = {0, 0};
	margin = {0, 0};
}

void ui::surface_element::present(const core::const_surface_ptr& picture, const frame_ptr& frame)
{
	_picture = picture;
	++_generation;
	if (!frame) return;

	// A picture the size of the last one updates the texture in place, and the scene recorded with it is
	// shown again over the picture alone.
	if (_texture && picture && !picture->empty() && picture->dimensions() == _texture_extent && !_drawn.is_empty())
	{
		if (_texture->update(picture) == render::texture_update_result::tex_updated)
		{
			_uploaded = _generation;
			frame->redraw(_drawn);
			return;
		}

		_texture.reset();
		_texture_extent = {};
	}

	frame->invalidate(bounds);
}

core::recti ui::surface_element::picture_bounds() const
{
	return _drawn;
}

std::optional<core::pointi> ui::surface_element::picture_point(const core::pointi client) const
{
	if (!_picture || _picture->empty() || _drawn.is_empty() || client.x < _drawn.left || client.y < _drawn.top ||
	    client.x >= _drawn.right || client.y >= _drawn.bottom)
	{
		return std::nullopt;
	}

	const auto extent = _picture->dimensions();
	return core::pointi{static_cast<int>(static_cast<int64_t>(client.x - _drawn.left) * extent.cx / _drawn.width()),
	                    static_cast<int>(static_cast<int64_t>(client.y - _drawn.top) * extent.cy / _drawn.height())};
}

void ui::surface_element::render(render::draw_context& dc, const core::pointi element_offset) const
{
	const auto area = bounds.offset(element_offset);
	if (_style.background.a > 0.0f) dc.draw_rect(area, _style.background);

	_drawn = {};
	if (!_picture || _picture->empty() || area.is_empty()) return;

	if (!_texture || _texture_extent != _picture->dimensions())
	{
		_texture = dc.create_texture();
		_texture_extent = {};
		_uploaded = 0;
	}

	if (!_texture) return;

	if (_uploaded != _generation)
	{
		if (_texture->update(_picture) == render::texture_update_result::failed)
		{
			_texture.reset();
			return;
		}

		_texture_extent = _picture->dimensions();
		_uploaded = _generation;
	}

	_drawn = fit_picture(_picture->dimensions(), area, _style.whole_multiples, _style.pixel_aspect);
	dc.draw_texture(_texture, _drawn, 1.0f, _style.sampler);
}

void ui::surface_element::dispatch_event(const element_event& event)
{
	if (event.type == element_event_type::free_graphics_resources)
	{
		_texture.reset();
		_texture_extent = {};
		_uploaded = 0;
	}
}

core::surface_ptr ui::render_to_surface(const element_ptr& root, const core::sizei extent, const core::color background,
                                        const double scale)
{
	if (!root || extent.cx < 1 || extent.cy < 1) return {};

	auto surface = std::make_shared<core::surface>();
	const auto context = render::create_surface_context(surface);
	if (!context) return {};

	context->begin_draw(extent, render::default_font_size, {});
	context->scale_metrics(scale);
	root->layout(*context, core::recti(extent));
	context->clear(background);
	root->render(*context, {});
	if (context->render().failed) return {};
	return surface;
}
