// This file is part of libdesktop, a C++20 platform layer for desktop applications
// Copyright 2026  Zac Walker
//
// This program is free software; you can redistribute it and / or modify it
// under the terms of the LGPL License either version 2.1 or later.
// License details are available at https://www.gnu.org/licenses/lgpl-2.1.html
// This program is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY

// Purpose: The portable frame layer. Native window events are translated into the same host, control,
// menu and bubble behaviour for every platform; a child without a native surface is painted and hit-tested
// as a region of its parent's surface.

#include "libdesktop/ui.h"
#include "libdesktop/ui/controls.h"
#include "libdesktop/ui/elements.h"
#include "libdesktop/platform/window.h"
#include "libdesktop/render/device.h"

namespace
{
	class frame_impl;
	std::weak_ptr<frame_impl> keyboard_frame;
	ui::menu_session* open_menu = nullptr;
	// Timer ids are local to each native window. Keep them fixed so a frame timer or fade can
	// never collide with the renderer's hidden-standby timer (0xD1FF).
	constexpr uint32_t frame_timer_id = 0x100u;
	constexpr uint32_t fade_timer_id = 0x101u;
	constexpr uint32_t caret_timer_id = 0x43415254u;
	constexpr uint32_t hidden_standby_timer_id = 0xD1FFu;
	std::atomic_uint32_t next_pane_timer_id = 0x1000u;

	uint32_t allocate_pane_timer_id()
	{
		for (;;)
		{
			const auto result = next_pane_timer_id.fetch_add(1, std::memory_order_relaxed);
			if (result != 0 && result != frame_timer_id && result != fade_timer_id &&
				result != caret_timer_id && result != hidden_standby_timer_id) return result;
		}
	}

	class frame_impl final : public ui::frame,
	                         public platform::window_events,
	                         public ui::control_host,
	                         public ui::menu_host,
	                         public ui::bubble_host,
	                         public std::enable_shared_from_this<frame_impl>
	{
	public:
		enum class kind { app, dialog, pane };

	private:
		const kind _kind;
		ui::frame_style _style;
		ui::frame_host_weak_ptr _host;
		ui::frame_host_weak_ptr _parent_host;
		ui::weak_app_ptr _app;
		std::weak_ptr<frame_impl> _parent;
		std::vector<std::weak_ptr<frame_impl>> _children;

		std::shared_ptr<platform::window> _window;
		render::window_device_ptr _device;
		core::sizei _extent;
		core::recti _bounds;
		bool _visible = false;
		int _disable_count = 0;
		std::string _text;
		platform::cursor _cursor = platform::cursor::normal;
		bool _destroyed = false;
		bool _minimized = false;

		ui::input_manager_ptr _controls;
		bool _over_controls = false;
		bool _updating_control_cursor = false;
		bool _tracking = false;
		bool _native_focus = false;
		bool _has_focus = false;
		std::weak_ptr<frame_impl> _focus_frame;
		std::weak_ptr<frame_impl> _capture_frame;
		std::weak_ptr<frame_impl> _tracking_frame;
		std::weak_ptr<frame_impl> _hover_frame;
		std::weak_ptr<frame_impl> _pan_frame;
		std::weak_ptr<frame_impl> _caption_input;

		bool _control_text_input = false;
		core::recti _control_caret;
		std::weak_ptr<frame_impl> _caret_owner;
		std::unordered_map<uint32_t, std::weak_ptr<frame_impl>> _timers;
		uint32_t _timer_id = 0;
		uint32_t _fade_timer_id = 0;
		int _alpha = 255;
		int _alpha_target = 255;
		bool _close_signalled = false;
		ui::close_result _modal_result = ui::close_result::ok;

		std::shared_ptr<frame_impl> self();
		std::shared_ptr<frame_impl> native_owner_shared();
		frame_impl* native_owner();
		const frame_impl* native_owner() const;
		core::pointi origin_in_native() const;
		core::pointi local_from_native(core::pointi loc) const;
		core::sizei extent() const;
		bool is_shown() const;
		bool is_enabled_here() const;
		render::measure_context* measure_context();
		frame_impl* focus_target();
		const frame_impl* focus_target() const;
		bool owns_focus() const;
		bool focus_first_child_window();
		void update_text_input();
		void sync_popup_scale();
		void update_resize_scale();
		void report_focus(bool has_focus);
		void focus_gained();
		void focus_lost();
		void change_focus(const std::shared_ptr<frame_impl>& next);
		frame_impl* frame_at(core::pointi loc);
		std::pair<frame_impl*, core::pointi> hit_target(core::pointi loc);
		std::shared_ptr<frame_impl> event_target(core::pointi loc);
		core::pointi target_loc(const std::shared_ptr<frame_impl>& target, core::pointi loc) const;
		std::shared_ptr<frame_impl> menu_owner();
		bool is_in_dialog() const;
		void set_cursor_direct(platform::cursor cursor);
		void remove_child_reference(const frame_impl* child);
		void apply_frame_placement(const ui::input_manager::frame_placement& placement);
		void layout_host(bool minimized);
		void paint_region(render::draw_context& dc, core::pointi origin, core::recti visible);
		void paint_native(render::draw_context_device& dc);
		void stop_timer(uint32_t id);
		void start_timer(uint32_t& slot, uint32_t id, uint32_t milliseconds);
		void fire_timer();
		void fade_to(bool visible);
		void step_fade();
		void show_app_menu(core::recti bounds, const std::vector<ui::command_ptr>& commands);
		void leave_frame(core::pointi native_loc);
		bool wheel_frame(core::pointi local, int delta, platform::wheel_axis axis, platform::key_state keys);
		void cancel_pointer_capture();
		void initialise_region(const std::shared_ptr<frame_impl>& parent);
		void add_child(const std::shared_ptr<frame_impl>& child);
		void destroy_children();

	public:
		frame_impl(const ui::frame_host_weak_ptr& host, const ui::weak_app_ptr& app, kind k,
		           const ui::frame_style& style, const ui::frame_host_weak_ptr& parent_host = {});
		~frame_impl() override;
		platform::window* native_window() const;
		ui::input_manager_ptr keyboard_controls() const;
		bool initialise_native(const platform::window_options& options);

		std::any handle() const override;
		void destroy() override;
		void enable(bool enable) override;
		std::string window_text() const override;
		void window_text(std::string_view value) override;
		void focus() override;
		bool is_visible() const override;
		bool has_focus() const override;
		core::recti window_bounds() const override;
		void window_bounds(core::recti bounds, bool visible) override;
		void show(bool show) override;
		void options_changed() override;
		void invalidate(core::recti bounds = {}, bool erase = false) override;
		void layout() override;
		void redraw() override;
		void redraw_now() override;
		void scroll(int dx, int dy, core::recti bounds) override;
		void track_menu(core::recti bounds, const std::vector<ui::command_ptr>& buttons) override;
		void close(bool cancel = false) override;
		bool is_enabled() const override;
		bool is_maximized() const override;
		bool is_occluded() const override;
		void set_cursor(platform::cursor cursor) override;
		void reset_graphics() override;
		core::pointi cursor_location() override;
		double scale_factor() const override;
		void sys_command(platform::sys_command_type command) override;
		void full_screen(bool full) override;
		void focus_first() override;
		ui::control_ptr focused_control() const override;
		ui::frame_ptr create_dlg(ui::frame_host_weak_ptr host) override;
		ui::frame_ptr create_frame(ui::frame_host_weak_ptr host, const ui::frame_style& style, bool menu);
		ui::frame_ptr create_frame(ui::frame_host_weak_ptr host, const ui::frame_style& style) override;
		ui::bubble_window_ptr create_bubble() override;
		ui::close_result wait_for_close(uint32_t timeout_ms) override;
		bool is_canceled() const override;
		void position(core::recti bounds) override;
		void save_window_position(platform::setting_file_ptr& store) override;

		void created(platform::window&) override;
		void destroyed() override;
		void size_changed(core::sizei client, bool minimized) override;
		void paint(core::recti damage) override;
		void scale_changed(double scale) override;
		void pointer_move(core::pointi loc, platform::key_state keys) override;
		void pointer_leave() override;
		void pointer_down(platform::pointer_button button, core::pointi loc, platform::key_state keys) override;
		void pointer_up(platform::pointer_button button, core::pointi loc, platform::key_state keys) override;
		void double_click(platform::pointer_button button, core::pointi loc, platform::key_state keys) override;
		bool wheel(core::pointi loc, int delta, platform::wheel_axis axis, platform::key_state keys) override;
		bool pinch(core::pointi loc, int delta, bool begins) override;
		void pan_start(core::pointi loc) override;
		void pan(core::pointi start, core::pointi current) override;
		void pan_end(core::pointi start, core::pointi end) override;
		bool touch_double_tap(core::pointi loc) override;
		bool key_down(int key, platform::key_state keys, bool system) override;
		bool key_up(int key, platform::key_state keys, bool system) override;
		void text(std::string_view utf8) override;
		void focus_changed(bool has_focus) override;
		void activated(bool active) override;
		void close_requested() override;
		void context_menu(core::pointi screen, bool from_keyboard) override;
		void cancel_mode() override;
		void timer(uint32_t id) override;
		platform::hit_region hit_test(core::pointi loc) override;
		void caption_button(platform::caption_button_action action, core::pointi loc) override;
		platform::drop_effect drag_over(const platform::clipboard_data& data, platform::key_state keys, core::pointi loc) override;
		platform::drop_effect drag_drop(platform::clipboard_data& data, platform::key_state keys, core::pointi loc) override;
		void drag_leave() override;
		void system_event(platform::os_event_type event) override;

		render::measure_context* measure() override;
		void invalidate(core::recti bounds) override;
		bool has_keyboard_focus() const override;
		void take_keyboard_focus() override;
		void capture_pointer(bool capture) override;
		core::pointi client_to_screen(core::pointi loc) const override;
		ui::frame_ptr create_popup(const ui::frame_host_weak_ptr& content, const render::color_style& colors) override;
		void menu_title_hovered() override;
		std::shared_ptr<ui::frame_host> listener() const override;
		void keyboard_to_frame() override;
		void text_input(bool active, core::recti caret) override;
		void caret_timer(bool on, uint32_t interval_ms) override;
		bool is_dialog() const override;
		bool presents_caption_button() const override;

		render::measure_context* menu_measure() override;
		ui::frame_ptr create_menu_popup(const ui::frame_host_weak_ptr& content, const ui::frame_ptr& parent) override;
		core::recti menu_work_area(core::recti bounds) override;
		void run_menu_loop(ui::menu_session& session) override;
		ui::frame_ptr create_bubble_popup(const ui::frame_host_weak_ptr& content) override;
		render::measure_context* bubble_measure(const ui::frame_ptr& popup) override;
		core::recti bubble_work_area(core::recti anchor) override;
	};
	std::shared_ptr<frame_impl> frame_impl::self()
	{
		return shared_from_this();
	}

	std::shared_ptr<frame_impl> frame_impl::native_owner_shared()
	{
		if (_window) return self();
		const auto parent = _parent.lock();
		return parent ? parent->native_owner_shared() : nullptr;
	}

	frame_impl* frame_impl::native_owner()
	{
		if (_window) return this;
		const auto parent = _parent.lock();
		return parent ? parent->native_owner() : nullptr;
	}

	const frame_impl* frame_impl::native_owner() const
	{
		if (_window) return this;
		const auto parent = _parent.lock();
		return parent ? parent->native_owner() : nullptr;
	}

	core::pointi frame_impl::origin_in_native() const
	{
		core::pointi result;
		for (auto current = this; current && !current->_window;)
		{
			result = result + current->_bounds.top_left();
			const auto parent = current->_parent.lock();
			current = parent.get();
		}
		return result;
	}

	core::pointi frame_impl::local_from_native(const core::pointi loc) const
	{
		return loc - origin_in_native();
	}

	core::sizei frame_impl::extent() const
	{
		return _window ? _extent : _bounds.extent();
	}

	bool frame_impl::is_shown() const
	{
		if (_window)
		{
			if (!_window->is_visible()) return false;
		}
		else if (!_visible)
		{
			return false;
		}
		const auto parent = _parent.lock();
		return !parent || parent->is_shown();
	}

	bool frame_impl::is_enabled_here() const
	{
		if (_disable_count != 0) return false;
		const auto parent = _parent.lock();
		return !parent || parent->is_enabled_here();
	}

	render::measure_context* frame_impl::measure_context()
	{
		if (_device) return &_device->context();
		if (const auto owner = native_owner(); owner && owner != this && owner->_device)
			return &owner->_device->context();

		for (const auto& weak : _children)
		{
			if (const auto child = weak.lock())
				if (auto* const mc = child->measure_context()) return mc;
		}
		return nullptr;
	}

	frame_impl* frame_impl::focus_target()
	{
		const auto owner = native_owner();
		if (!owner) return nullptr;
		const auto target = owner->_focus_frame.lock();
		return target ? target.get() : owner;
	}

	const frame_impl* frame_impl::focus_target() const
	{
		const auto owner = native_owner();
		if (!owner) return nullptr;
		const auto target = owner->_focus_frame.lock();
		return target ? target.get() : owner;
	}

	bool frame_impl::owns_focus() const
	{
		const auto owner = native_owner();
		return owner && owner->_native_focus && owner->focus_target() == this;
	}

	void frame_impl::update_text_input()
	{
		const auto owner = native_owner();
		if (!owner || !owner->_window) return;
		const auto target = owner->focus_target();
		const auto controls_typing = target && target->_controls->is_typing();
		const auto app = owner->_app.lock();
		const auto app_typing = owner->_native_focus && app && app->focus_mode() == ui::focus_mode::text_edit;
		const auto caret = controls_typing ? target->_control_caret.offset(target->origin_in_native()) : core::recti{};
		owner->_window->set_text_input(controls_typing || app_typing, caret);
	}

	void frame_impl::sync_popup_scale()
	{
		if (!_device || !_window || _window->kind() != platform::window_kind::popup) return;
		const auto parent = _parent.lock();
		if (!parent) return;
		auto* const source = parent->measure_context();
		if (!source) return;

		// The native popup can receive a monitor-DPI value of its own. The old window layer
		// deliberately shared the owner's draw context, so copy the portable metrics after the
		// device has handled the notification.
		auto& target = _device->context();
		target.update_font_size(core::round(parent->scale_factor() * render::base_font_size()));
		target.scale_factor = source->scale_factor;
		target.icon_cxy = source->icon_cxy;
		target.padding2 = source->padding2;
		target.padding1 = source->padding1;
		target.handle_cxy = source->handle_cxy;
		target.scroll_width = source->scroll_width;
	}

	void frame_impl::update_resize_scale()
	{
		auto top = this;
		while (const auto parent = top->_parent.lock()) top = parent.get();
		if (!top->_window || top->_window->kind() != platform::window_kind::app) return;
		top->_window->set_resize_scale(static_cast<double>(render::base_font_size()) /
		                               static_cast<double>(render::default_font_size));
	}

	void frame_impl::report_focus(const bool has_focus)
	{
		const auto pane = _kind == kind::pane;
		const auto frame = std::static_pointer_cast<ui::frame>(self());
		if (const auto host = _host.lock())
		{
			if (pane) host->pane_focus_changed(has_focus, frame);
			else host->focus_changed(has_focus, nullptr);
		}
		if (const auto parent = _parent_host.lock())
		{
			if (pane) parent->pane_focus_changed(has_focus, frame);
			else parent->focus_changed(has_focus, nullptr);
		}
	}

	void frame_impl::focus_gained()
	{
		if (_has_focus) return;
		_has_focus = true;
		if (!_controls->keyboard_focus(true)) report_focus(true);
		keyboard_frame = weak_from_this();
		update_text_input();
		invalidate();
	}

	void frame_impl::focus_lost()
	{
		if (!_has_focus) return;
		_has_focus = false;
		if (!_controls->keyboard_focus(false)) report_focus(false);
		if (keyboard_frame.lock().get() == this) keyboard_frame.reset();
		update_text_input();
	}

	void frame_impl::change_focus(const std::shared_ptr<frame_impl>& next)
	{
		const auto owner = native_owner();
		if (!owner) return;
		const auto previous = owner->_focus_frame.lock();
		if (previous == next) return;
		if (owner->_native_focus && previous) previous->focus_lost();
		owner->_focus_frame = next;
		if (owner->_native_focus && next) next->focus_gained();
	}

	frame_impl* frame_impl::frame_at(const core::pointi loc)
	{
		for (auto i = _children.rbegin(); i != _children.rend(); ++i)
		{
			const auto child = i->lock();
			if (!child || child->_window || !child->is_shown() || !child->_bounds.contains(loc)) continue;
			if (auto* const deeper = child->frame_at(loc - child->_bounds.top_left())) return deeper;
			return child.get();
		}
		return this;
	}

	// Hit testing a top-level window is different from routing pointer messages: Windows asks the
	// top-level frame even when a native child is under the point, and SDL asks the root for the
	// same answer. Walk both kinds of child using the point in each child's client coordinates.
	std::pair<frame_impl*, core::pointi> frame_impl::hit_target(const core::pointi loc)
	{
		for (auto i = _children.rbegin(); i != _children.rend(); ++i)
		{
			const auto child = i->lock();
			if (!child || !child->is_shown()) continue;

			if (child->_window)
			{
				if (child->_window->kind() != platform::window_kind::child) continue;
				const auto bounds = child->_window->bounds();
				if (!bounds.contains(loc)) continue;
				return child->hit_target(loc - bounds.top_left());
			}

			if (!child->_bounds.contains(loc)) continue;
			return child->hit_target(loc - child->_bounds.top_left());
		}
		return {this, loc};
	}

	std::shared_ptr<frame_impl> frame_impl::event_target(const core::pointi loc)
	{
		const auto owner = native_owner_shared();
		if (!owner) return nullptr;
		if (owner.get() != this) return self();
		if (const auto captured = owner->_capture_frame.lock()) return captured;
		if (const auto tracking = owner->_tracking_frame.lock()) return tracking;
		return owner->frame_at(loc)->self();
	}

	core::pointi frame_impl::target_loc(const std::shared_ptr<frame_impl>& target, const core::pointi loc) const
	{
		return target ? target->local_from_native(loc) : loc;
	}

	std::shared_ptr<frame_impl> frame_impl::menu_owner()
	{
		auto result = native_owner_shared();
		while (result && result->_kind == kind::pane)
		{
			const auto parent = result->_parent.lock();
			if (!parent) break;
			result = parent;
		}
		return result;
	}

	bool frame_impl::is_in_dialog() const
	{
		for (auto current = this; current; )
		{
			if (current->_kind == kind::dialog) return true;
			const auto parent = current->_parent.lock();
			current = parent.get();
		}
		return false;
	}

	void frame_impl::set_cursor_direct(const platform::cursor cursor)
	{
		_cursor = cursor;
		if (const auto owner = native_owner(); owner && owner->_window) owner->_window->set_cursor(cursor);
	}

	void frame_impl::remove_child_reference(const frame_impl* child)
	{
		std::erase_if(_children, [child](const auto& weak)
		{
			const auto value = weak.lock();
			return !value || value.get() == child;
		});
	}

	void frame_impl::apply_frame_placement(const ui::input_manager::frame_placement& placement)
	{
		auto child = std::dynamic_pointer_cast<frame_impl>(placement.frame);
		if (!child) return;

		const auto old_bounds = child->_bounds;
		const auto old_visible = child->_visible;
		const auto old_extent = child->extent();
		const auto changed = old_bounds != placement.bounds || old_visible != placement.visible;
		if (!changed) return;

		child->_bounds = placement.bounds;
		child->_visible = placement.visible;
		if (child->_window)
		{
			child->_window->set_bounds(placement.bounds, placement.visible);
			return;
		}

		// A region has no native expose event. Repaint both its old and new rectangle, so the
		// pixels it vacated are restored and a newly appearing region is drawn once.
		if (const auto parent = child->_parent.lock(); parent && (old_visible || placement.visible))
			parent->invalidate(old_bounds.make_union(placement.bounds), false);
		if (old_extent != child->extent()) child->layout();
	}

	void frame_impl::layout_host(const bool minimized)
	{
		const auto host = _host.lock();
		auto* const mc = measure_context();
		if (!host || !mc) return;
		host->on_window_layout(*mc, extent(), minimized);
		if (minimized) return;

		std::vector<platform::window_move> native_moves;
		for (const auto& placement : _controls->layout())
		{
			auto child = std::dynamic_pointer_cast<frame_impl>(placement.frame);
			if (!child || !child->_window || child->_window->kind() != platform::window_kind::child)
			{
				apply_frame_placement(placement);
				continue;
			}

			const auto changed = child->_bounds != placement.bounds || child->_visible != placement.visible;
			if (!changed) continue;
			child->_bounds = placement.bounds;
			child->_visible = placement.visible;
			native_moves.push_back({child->_window.get(), placement.bounds, placement.visible});
		}

		if (!native_moves.empty()) platform::defer_window_moves(native_moves);
	}

	void frame_impl::paint_region(render::draw_context& dc, const core::pointi origin, const core::recti visible)
	{
		if (!_visible) return;
		const auto area = core::recti(origin, _bounds.extent()).intersection(visible);
		if (area.is_empty()) return;
		_controls->refresh();
		render::scoped_clip clip(dc, area);
		ui::offset_draw_context child_dc(dc, origin);
		child_dc.frame_has_focus = has_focus();
		child_dc.colors = _style.colors;
		if (!_style.container) child_dc.clear(core::color(_style.colors.background, 1.0f));
		if (const auto host = _host.lock(); host && !_style.container) host->on_window_paint(child_dc);
		const auto children = _children;
		for (const auto& weak : children)
			if (const auto child = weak.lock(); child && !child->_window)
				child->paint_region(dc, origin + child->_bounds.top_left(), area);
	}

	void frame_impl::paint_native(render::draw_context_device& dc)
	{
		_controls->refresh();
		dc.time_now = core::now();
		dc.frame_has_focus = has_focus();
		dc.colors = _style.colors;
		if (_style.layered)
		{
			// Layered surfaces retain transparent pixels around bubbles; clearing them to an opaque
			// background turns the shadow and rounded corners into a rectangle.
			if (_device) _device->set_layer_alpha(_alpha);
			dc.set_layer_alpha(_alpha);
		}
		else
		{
			dc.clear(core::color(_style.colors.background, 1.0f));
		}
		if (const auto host = _host.lock(); host && !_style.container) host->on_window_paint(dc);
		const auto area = core::recti(0, 0, _extent.cx, _extent.cy);
		const auto children = _children;
		for (const auto& weak : children)
			if (const auto child = weak.lock(); child && !child->_window)
				child->paint_region(dc, child->_bounds.top_left(), area);
	}
	void frame_impl::stop_timer(const uint32_t id)
	{
		const auto owner = native_owner();
		if (!owner || !owner->_window || id == 0) return;
		owner->_window->stop_timer(id);
		owner->_timers.erase(id);
	}

	void frame_impl::start_timer(uint32_t& slot, const uint32_t id, const uint32_t milliseconds)
	{
		if (slot) stop_timer(slot);
		slot = 0;
		if (milliseconds == 0 || id == 0) return;
		const auto owner = native_owner();
		if (!owner || !owner->_window) return;
		// Region/pane timers share their native owner, so each needs its own id. The app/dialog
		// frame tick and fade ids remain fixed per native window, as in the old layer.
		const auto timer_id = id == frame_timer_id && _kind == kind::pane ? allocate_pane_timer_id() : id;
		slot = timer_id;
		owner->_timers[slot] = weak_from_this();
		owner->_window->start_timer(slot, milliseconds);
	}

	void frame_impl::fire_timer()
	{
		if (_timer_id == 0) return;
		_controls->tick();
		if (const auto host = _host.lock()) host->tick();
	}

	void frame_impl::fade_to(const bool visible)
	{
		_alpha_target = visible ? 255 : 0;
		if (visible && _window && !_window->is_visible()) _window->show(true);
		if (!render::animations_enabled)
		{
			if (_fade_timer_id) stop_timer(_fade_timer_id);
			_fade_timer_id = 0;
			_alpha = _alpha_target;
			if (_device) _device->set_layer_alpha(_alpha);
			if (_alpha > 0) invalidate();
			if (_alpha == 0 && _window) _window->show(false);
		}
		else if (_alpha != _alpha_target && !_fade_timer_id)
		{
			start_timer(_fade_timer_id, fade_timer_id, 1000 / 30);
		}
	}

	void frame_impl::step_fade()
	{
		if (_alpha != _alpha_target)
		{
			_alpha = ui::gated_fade_alpha_step(_alpha, _alpha_target);
			if (_device) _device->set_layer_alpha(_alpha);
			if (_alpha != 0) invalidate();
		}
		if (_alpha == _alpha_target)
		{
			if (_fade_timer_id) stop_timer(_fade_timer_id);
			_fade_timer_id = 0;
			if (_alpha == 0 && _window) _window->show(false);
		}
	}

	void frame_impl::show_app_menu(const core::recti bounds, const std::vector<ui::command_ptr>& commands)
	{
		if (const auto app = _app.lock()) app->track_menu(std::static_pointer_cast<ui::frame>(self()), bounds, commands);
		else track_menu(bounds, commands);
	}

	void frame_impl::leave_frame(const core::pointi native_loc)
	{
		const auto local = local_from_native(native_loc);
		_controls->mouse_leave();
		const auto was_over = _over_controls;
		_over_controls = false;
		if (!was_over)
			if (const auto host = _host.lock()) host->on_mouse_leave(local);
		set_cursor_direct(_cursor);
	}

	bool frame_impl::wheel_frame(const core::pointi local, const int delta, const platform::wheel_axis axis,
	                             const platform::key_state keys)
	{
		if (axis == platform::wheel_axis::vertical && _controls->wheel(local, delta, keys)) return true;
		bool handled = false;
		if (const auto host = _host.lock())
		{
			if (axis == platform::wheel_axis::vertical) host->on_mouse_wheel(local, delta, keys, handled);
			else host->on_mouse_hwheel(local, delta, keys, handled);
		}
		return handled;
	}

	void frame_impl::cancel_pointer_capture()
	{
		const auto owner = native_owner();
		if (!owner) return;
		owner->_tracking_frame.reset();
		owner->_capture_frame.reset();
		owner->_tracking = false;
		if (owner->_window) owner->_window->capture(false);
	}

	bool frame_impl::initialise_native(const platform::window_options& options)
	{
		_window = platform::create_window(options, std::weak_ptr<platform::window_events>(weak_from_this()));
		if (!_window) return false;
		_extent = _window->client_extent();
		_visible = _window->is_visible();
		_focus_frame = weak_from_this();
		if (_window->kind() == platform::window_kind::app) update_resize_scale();
		if (!_style.container || !platform::child_windows())
		{
			render::device_options device_options;
			device_options.gpu = _style.hardware_accelerated && render::gpu_active() && !_style.layered;
			device_options.layered = _style.layered;
			_device = render::create_window_device(*_window, device_options);
			if (_device)
			{
				if ((_window->kind() == platform::window_kind::child ||
				     _window->kind() == platform::window_kind::popup) && options.parent)
				{
					_device->scale_changed(options.parent->root()->scale());
				}
				if (_style.layered) _device->set_layer_alpha(_alpha);
				_device->resize(_extent, _window->is_minimized());
			}
		}
		if (!_text.empty()) _window->set_title(_text);
		const auto interval = _kind == kind::pane ? _style.timer_milliseconds : 1000 / render::default_ticks_per_second;
		start_timer(_timer_id, frame_timer_id, interval);
		// The application's window is laid out as it is made, so the first paint after run shows it is of a
		// laid out tree. A dialog or a pane is laid out once it is placed or asked to be: its host does not
		// hold it until the call making it returns.
		if (_kind == kind::app) layout_host(_window->is_minimized());
		return true;
	}

	void frame_impl::initialise_region(const std::shared_ptr<frame_impl>& parent)
	{
		_parent = parent;
		_bounds = {};
		_visible = false;
		_focus_frame = weak_from_this();
	}

	void frame_impl::add_child(const std::shared_ptr<frame_impl>& child)
	{
		std::erase_if(_children, [](const std::weak_ptr<frame_impl>& weak) { return weak.expired(); });
		_children.emplace_back(child);
	}

	void frame_impl::destroy_children()
	{
		const auto children = _children;
		for (const auto& weak : children)
			if (const auto child = weak.lock()) child->destroy();
		_children.clear();
	}

	frame_impl::frame_impl(const ui::frame_host_weak_ptr& host, const ui::weak_app_ptr& app, const kind k,
	                       const ui::frame_style& style, const ui::frame_host_weak_ptr& parent_host) :
		_kind(k), _style(style), _host(host), _parent_host(parent_host), _app(app),
		_controls(std::make_shared<ui::input_manager>(static_cast<ui::control_host*>(this)))
	{
		_alpha = style.layered ? 0 : 255;
		_alpha_target = _alpha;
	}

	frame_impl::~frame_impl()
	{
		destroy_children();
		_controls->detach();
		if (_window && !_destroyed) _window->destroy();
	}

	platform::window* frame_impl::native_window() const
	{
		return _window.get();
	}

	ui::input_manager_ptr frame_impl::keyboard_controls() const
	{
		return _controls;
	}

	std::any frame_impl::handle() const
	{
		return _window ? _window->native_handle() : std::any{};
	}

	void frame_impl::destroy()
	{
		if (_destroyed) return;
		_destroyed = true;
		destroy_children();
		if (_timer_id) { stop_timer(_timer_id); _timer_id = 0; }
		if (_fade_timer_id) { stop_timer(_fade_timer_id); _fade_timer_id = 0; }
		if (const auto owner = native_owner(); owner && owner->_caret_owner.lock().get() == this)
			owner->_caret_owner.reset();
		_controls->detach();
		if (!_window)
		{
			if (const auto parent = _parent.lock())
			{
				parent->remove_child_reference(this);
				if (_visible) parent->invalidate(_bounds, false);
			}
			_visible = false;
			return;
		}
		_window->destroy();
	}

	void frame_impl::enable(const bool enable_value)
	{
		if (enable_value)
		{
			if (_disable_count > 0) --_disable_count;
		}
		else ++_disable_count;
		if (_window) _window->enable(enable_value);
	}

	std::string frame_impl::window_text() const
	{
		return _window ? _window->title() : _text;
	}

	void frame_impl::window_text(const std::string_view value)
	{
		_text = value;
		if (_window) _window->set_title(value);
	}

	// The frame itself takes the keyboard, from any of its controls that held it: a window that has it
	// already hears of no change from the platform, so the control lets it go here.
	void frame_impl::focus()
	{
		const auto owner = native_owner_shared();
		if (!owner || !owner->_window) return;
		_controls->clear_focus();
		take_keyboard_focus();
	}

	bool frame_impl::is_visible() const
	{
		return is_shown();
	}

	bool frame_impl::has_focus() const
	{
		return owns_focus();
	}

	core::recti frame_impl::window_bounds() const
	{
		if (_window) return _window->bounds();
		const auto owner = native_owner();
		if (!owner || !owner->_window) return {};
		const auto origin = owner->_window->client_to_screen(origin_in_native());
		return {origin, extent()};
	}

	void frame_impl::window_bounds(const core::recti bounds, const bool visible)
	{
		if (_window)
		{
			if (_style.layered)
			{
				const auto was_visible = _window->is_visible();
				// Keep a shown layered window shown while it moves. Fading is the only operation
				// that should hide or show it.
				_window->set_bounds(bounds, was_visible);
				_visible = visible;
				if (visible != was_visible || _alpha_target != (visible ? 255 : 0)) fade_to(visible);
			}
			else
			{
				_window->set_bounds(bounds, visible);
				_visible = visible;
			}
			return;
		}
		const auto old = _bounds;
		const auto old_visible = _visible;
		_bounds = bounds;
		_visible = visible;
		if (const auto parent = _parent.lock())
			if (old != bounds || old_visible != visible) parent->invalidate(old.make_union(bounds), false);
		if (old.extent() != bounds.extent()) layout();
	}
	void frame_impl::show(const bool show_value)
	{
		if (_window && _style.layered) fade_to(show_value);
		else if (_window) _window->show(show_value);
		else
		{
			_visible = show_value;
			if (show_value) invalidate();
		}
	}

	void frame_impl::options_changed()
	{
		update_resize_scale();
		_controls->scale_changed();
		layout();
		invalidate();
	}

	void frame_impl::invalidate(const core::recti bounds, const bool)
	{
		if (_window)
		{
			_window->invalidate(bounds);
			return;
		}
		const auto owner = native_owner();
		if (!owner || !owner->_window) return;
		const auto area = bounds.is_empty() ? core::recti(origin_in_native(), extent()) : bounds.offset(origin_in_native());
		owner->_window->invalidate(area);
	}

	void frame_impl::layout()
	{
		layout_host(_minimized || (_window && _window->is_minimized()));
	}

	void frame_impl::redraw()
	{
		if (const auto owner = native_owner(); owner && owner->_device) owner->_device->redraw();
	}

	void frame_impl::redraw_now()
	{
		invalidate();
		if (const auto owner = native_owner(); owner && owner->_window) owner->_window->update_now();
	}

	void frame_impl::scroll(const int dx, const int dy, const core::recti bounds)
	{
		if (_window) _window->scroll(dx, dy, bounds);
		else if (const auto owner = native_owner(); owner && owner->_window)
			owner->_window->scroll(dx, dy, bounds.offset(origin_in_native()));
	}

	void frame_impl::track_menu(const core::recti bounds, const std::vector<ui::command_ptr>& buttons)
	{
		if (_kind == kind::pane)
			if (const auto parent = _parent.lock())
			{
				parent->track_menu(bounds, buttons);
				return;
			}
		ui::track_drawn_menu(*this, bounds, window_bounds(), buttons);
	}

	void frame_impl::menu_title_hovered()
	{
		if (open_menu) open_menu->cancel();
	}

	void frame_impl::close(const bool cancel)
	{
		if (_kind == kind::app)
		{
			if (const auto app = _app.lock(); app && app->can_exit())
			{
				// Signal shutdown before destroying the native window, so its destruction callbacks
				// observe the same closing state as the old platform frame.
				platform::quit();
				if (_window) _window->destroy();
			}
		}
		else if (_kind == kind::pane) destroy();
		else
		{
			_modal_result = cancel ? ui::close_result::cancel : ui::close_result::ok;
			_close_signalled = true;
		}
	}

	bool frame_impl::is_enabled() const
	{
		return is_enabled_here() && (!_window || _window->is_enabled());
	}

	bool frame_impl::is_maximized() const
	{
		return _window && _window->is_maximized();
	}

	bool frame_impl::is_occluded() const
	{
		const auto owner = native_owner();
		return !owner || !owner->_window || !is_shown() || owner->_window->is_occluded();
	}

	void frame_impl::set_cursor(const platform::cursor cursor)
	{
		if (_updating_control_cursor)
		{
			if (const auto owner = native_owner(); owner && owner->_window) owner->_window->set_cursor(cursor);
			return;
		}
		set_cursor_direct(cursor);
	}

	void frame_impl::reset_graphics()
	{
		_controls->scale_changed();
		const auto children = _children;
		for (const auto& weak : children)
			if (const auto child = weak.lock()) child->reset_graphics();
		layout();
		invalidate();
	}

	core::pointi frame_impl::cursor_location()
	{
		const auto owner = native_owner();
		return owner && owner->_window ? local_from_native(owner->_window->cursor_location()) : core::pointi{-1, -1};
	}

	double frame_impl::scale_factor() const
	{
		if (_window && _window->kind() == platform::window_kind::popup)
			if (const auto parent = _parent.lock()) return parent->scale_factor();
		const auto owner = native_owner();
		if (!owner || !owner->_window) return 1.0;
		if (owner->_window->kind() == platform::window_kind::child)
			if (const auto root = owner->_window->root()) return root->scale();
		return owner->_window->scale();
	}

	void frame_impl::sys_command(const platform::sys_command_type command)
	{
		if (const auto owner = native_owner(); owner && owner->_window) owner->_window->sys_command(command);
	}

	void frame_impl::full_screen(const bool full)
	{
		if (const auto owner = native_owner(); owner && owner->_window) owner->_window->full_screen(full);
	}

	bool frame_impl::focus_first_child_window()
	{
		for (const auto& weak : _children)
		{
			const auto child = weak.lock();
			if (!child || !child->is_enabled_here()) continue;
			if (child->_window && child->_window->kind() == platform::window_kind::child)
			{
				child->focus();
				return true;
			}
			if (!child->_window && child->focus_first_child_window()) return true;
		}
		return false;
	}

	void frame_impl::focus_first()
	{
		if (_controls->focus_first()) return;
		focus_first_child_window();
	}

	ui::control_ptr frame_impl::focused_control() const
	{
		return _controls->has_keyboard_focus() ? _controls->focused() : nullptr;
	}

	ui::close_result frame_impl::wait_for_close(const uint32_t timeout_ms)
	{
		_close_signalled = false;
		const auto timeout = timeout_ms == 0 ? UINT32_MAX : timeout_ms;
		const auto reached = platform::run_modal_loop([this] { return _close_signalled; }, nullptr, timeout);
		return reached ? _modal_result : ui::close_result::cancel;
	}

	bool frame_impl::is_canceled() const
	{
		return _modal_result == ui::close_result::cancel;
	}

	void frame_impl::position(const core::recti bounds)
	{
		window_bounds(bounds, true);
	}

	void frame_impl::save_window_position(platform::setting_file_ptr& store)
	{
		if (_window && store) _window->save_placement(store, platform::placement_setting_name());
	}

	void frame_impl::created(platform::window&)
	{
	}

	void frame_impl::destroyed()
	{
		_destroyed = true;
		if (_timer_id) { stop_timer(_timer_id); _timer_id = 0; }
		if (_fade_timer_id) { stop_timer(_fade_timer_id); _fade_timer_id = 0; }
		_controls->detach();
		if (const auto host = _host.lock()) host->on_window_destroy();
		_visible = false;
		_window.reset();
		_device.reset();
	}

	void frame_impl::size_changed(const core::sizei client, const bool minimized)
	{
		_extent = client;
		_minimized = minimized;
		if (_device) _device->resize(client, minimized);
		layout_host(minimized);
	}

	void frame_impl::paint(const core::recti damage)
	{
		if (!_device || _destroyed) return;
		_device->paint(damage, [this](render::draw_context_device& dc) { paint_native(dc); });
	}

	void frame_impl::scale_changed(const double scale)
	{
		// Popups share the scale of their owner, even when the native popup receives its own
		// monitor-DPI notification. This keeps menus and bubbles measured as their owner draws them.
		update_resize_scale();
		auto effective_scale = scale;
		if (_window && _window->kind() == platform::window_kind::popup)
			if (const auto parent = _parent.lock()) effective_scale = parent->scale_factor();
		if (_device) _device->scale_changed(effective_scale);
		sync_popup_scale();
		_controls->scale_changed();
		if (const auto host = _host.lock()) host->dpi_changed();
		const auto children = _children;
		for (const auto& weak : children)
			if (const auto child = weak.lock())
			{
				// A dialog's window gets its own DPI notification for the monitor it is on.
				if (child->_kind == kind::dialog) continue;
				child->scale_changed(effective_scale);
			}
		layout();
	}

	void frame_impl::pointer_move(const core::pointi loc, const platform::key_state)
	{
		const auto owner = native_owner_shared();
		const auto target = event_target(loc);
		if (!owner || !target) return;
		const auto previous = owner->_hover_frame.lock();
		if (previous && previous != target) previous->leave_frame(loc);
		owner->_hover_frame = target;
		const auto local = target_loc(target, loc);

		// A host drag owns the pointer until release. Native Windows withheld control moves while
		// dragging; doing the same here prevents a control crossed by the drag from taking hover.
		bool over_control = false;
		if (!owner->_tracking)
		{
			target->_updating_control_cursor = true;
			over_control = target->_controls->mouse_move(local);
			target->_updating_control_cursor = false;
		}

		if (over_control)
		{
			if (!target->_over_controls)
			{
				target->_over_controls = true;
				if (const auto host = target->_host.lock()) host->on_mouse_leave(local);
			}
		}
		else
		{
			if (target->_over_controls)
			{
				target->_over_controls = false;
			}
			target->set_cursor_direct(target->_cursor);
			if (const auto host = target->_host.lock()) host->on_mouse_move(local, owner->_tracking_frame.lock().get() == target.get());
		}
	}

	void frame_impl::pointer_leave()
	{
		const auto owner = native_owner_shared();
		if (!owner || owner->_capture_frame.lock() || owner->_tracking_frame.lock()) return;
		if (const auto target = owner->_hover_frame.lock())
			target->leave_frame(target->local_from_native(owner->_window->cursor_location()));
		owner->_hover_frame.reset();
	}
	void frame_impl::pointer_down(const platform::pointer_button button, const core::pointi loc,
	                              const platform::key_state keys)
	{
		const auto owner = native_owner();
		const auto target = event_target(loc);
		if (!owner || !target) return;
		const auto local = target_loc(target, loc);
		for (auto current = target; current; current = current->_parent.lock())
			if (current->_controls) current->_controls->cancel_system_mode();
		if (button == platform::pointer_button::left)
		{
			if (target->_controls->mouse_down(local, keys)) return;
			target->_tracking = true;
			owner->_tracking = true;
			owner->_tracking_frame = target;
			if (owner->_window) owner->_window->capture(true);
			const auto host = target->_host.lock();
			if (target->_kind == kind::pane && target->_style.can_focus && !target->_style.no_focus &&
			    (!host || host->takes_keyboard_at(local))) target->focus();
			if (host) host->on_mouse_left_button_down(local, keys);
		}
		else if (button == platform::pointer_button::middle)
		{
			if (const auto host = target->_host.lock()) host->on_mouse_middle_button_down(local, keys);
		}
		else if (button == platform::pointer_button::right)
		{
			if (const auto host = target->_host.lock()) host->on_mouse_right_button_down(local, keys);
		}
	}

	void frame_impl::pointer_up(const platform::pointer_button button, const core::pointi loc,
	                            const platform::key_state keys)
	{
		const auto owner = native_owner();
		const auto target = event_target(loc);
		if (!owner || !target) return;
		const auto local = target_loc(target, loc);
		if (button == platform::pointer_button::left)
		{
			if (!owner->_tracking && target->_controls->mouse_up(local, keys)) return;
			if (owner->_tracking)
			{
				owner->_tracking = false;
				owner->_tracking_frame.reset();
				target->_tracking = false;
				if (owner->_window) owner->_window->capture(false);
			}
			// A frame receives the release even when no drag was started; this is the release
			// paired with a double-click press on platforms that report it separately.
			if (const auto host = target->_host.lock()) host->on_mouse_left_button_up(local, keys);
		}
		else if (button == platform::pointer_button::middle)
		{
			if (const auto host = target->_host.lock()) host->on_mouse_middle_button_up(local, keys);
		}
		else if (button == platform::pointer_button::right)
		{
			if (const auto host = target->_host.lock()) host->on_mouse_right_button_up(local, keys);
		}
		else if (button == platform::pointer_button::x1 || button == platform::pointer_button::x2)
		{
			if (const auto host = target->_host.lock())
				host->on_mouse_other_button_up(button == platform::pointer_button::x1
						? platform::other_mouse_button::xb1
						: platform::other_mouse_button::xb2,
					local, keys);
		}
	}

	void frame_impl::double_click(const platform::pointer_button button, const core::pointi loc,
	                              const platform::key_state keys)
	{
		if (button != platform::pointer_button::left) return;
		const auto target = event_target(loc);
		if (!target) return;
		const auto local = target_loc(target, loc);
		if (target->_controls->mouse_double_click(local, keys)) return;
		if (const auto host = target->_host.lock()) host->on_mouse_left_button_double_click(local, keys);
	}

	bool frame_impl::wheel(const core::pointi loc, const int delta, const platform::wheel_axis axis,
	                       const platform::key_state keys)
	{
		const auto target = event_target(loc);
		if (!target) return false;
		if (target->wheel_frame(target_loc(target, loc), delta, axis, keys)) return true;

		// Windows sends an unhandled vertical wheel message to the native parent itself.
		// Region-only trees (SDL) have no such message propagation, so walk those frames here
		// with each parent's own coordinates and only there.
		if (!platform::child_windows())
			for (auto parent = target->_parent.lock(); parent; parent = parent->_parent.lock())
				if (parent->wheel_frame(parent->local_from_native(loc), delta, axis, keys)) return true;
		return false;
	}

	bool frame_impl::pinch(const core::pointi loc, const int delta, const bool begins)
	{
		const auto target = event_target(loc);
		if (!target) return false;
		bool handled = false;
		if (const auto host = target->_host.lock()) host->on_pinch(target_loc(target, loc), delta, begins, handled);
		return handled;
	}

	void frame_impl::pan_start(const core::pointi loc)
	{
		const auto owner = native_owner();
		const auto target = event_target(loc);
		if (!owner || !target) return;
		owner->_pan_frame = target;
		if (const auto host = target->_host.lock()) host->pan_start(target_loc(target, loc));
	}

	void frame_impl::pan(const core::pointi start, const core::pointi current)
	{
		const auto owner = native_owner();
		const auto target = owner ? owner->_pan_frame.lock() : nullptr;
		if (target)
			if (const auto host = target->_host.lock()) host->pan(target_loc(target, start), target_loc(target, current));
	}

	void frame_impl::pan_end(const core::pointi start, const core::pointi end)
	{
		const auto owner = native_owner();
		const auto target = owner ? owner->_pan_frame.lock() : nullptr;
		if (target)
			if (const auto host = target->_host.lock()) host->pan_end(target_loc(target, start), target_loc(target, end));
		if (owner) owner->_pan_frame.reset();
	}

	bool frame_impl::touch_double_tap(const core::pointi loc)
	{
		const auto target = event_target(loc);
		if (!target) return false;
		if (const auto host = target->_host.lock()) return host->touch_double_tap(target_loc(target, loc));
		return false;
	}

	bool frame_impl::key_down(const int key, const platform::key_state keys, const bool system)
	{
		const auto target = focus_target();
		if (!target) return false;
		const auto app = _app.lock();
		const auto controls = target->_controls;
		const auto c = static_cast<char32_t>(key);
		const auto in_dialog = target->is_in_dialog();
		const auto system_input = system || c == platform::keys::F10 || c == platform::keys::ALT || keys.alt;
		const auto typing = controls && controls->is_typing() && !keys.alt;

		// A menu bar in its menu mode takes every key first. It may live in the application frame
		// while focus belongs to a pane, so the focused frame and then each of its parents is asked.
		for (auto current = target->shared_from_this(); current; current = current->_parent.lock())
		{
			const auto manager = current->_controls;
			if (manager && manager->in_system_mode() && manager->system_key(key, keys, true)) return true;
		}

		// Otherwise an application's accelerators come before a menu bar's own use of F10 and Alt, as a
		// native accelerator table is translated before the menu activates. A typing control keeps the
		// keys it had, so only a frame whose keyboard is not in a text field asks the application here.
		auto app_offered = false;
		if (system_input && !typing && !in_dialog && app)
		{
			app_offered = true;
			if (app->key_down(c, keys)) return true;
		}
		if (system_input)
			for (auto current = target->shared_from_this(); current; current = current->_parent.lock())
			{
				const auto manager = current->_controls;
				if (manager && manager->system_key(key, keys, true)) return true;
			}

		if (typing)
		{
			const auto leaves = c == platform::keys::ESCAPE || c == platform::keys::UP || c == platform::keys::DOWN ||
				c == platform::keys::TAB || (c == platform::keys::RETURN && !(controls->wants_return() && !keys.control));
			const auto offered_to_app = !in_dialog && leaves && app;
			if (offered_to_app && app->key_down(c, keys)) return true;
			if (controls->key_down(key, keys)) return true;

			if (!in_dialog && !offered_to_app)
				if (const auto focused = controls->focused(); focused && focused->traits.forwards_unhandled_keys && app &&
					app->key_down(c, keys)) return true;
			return false;
		}
		// A modal dialog owns its keys. In particular, do not let an application shortcut consume
		// Escape, Return or navigation before the dialog's input manager sees them.
		if (!app_offered && !in_dialog && app && app->key_down(c, keys)) return true;
		if (controls && controls->key_down(key, keys)) return true;
		if (const auto host = target->_host.lock(); host && host->key_down(key, keys)) return true;
		if (in_dialog && c == platform::keys::ESCAPE && !keys.alt)
		{
			for (auto current = target->shared_from_this(); current; )
			{
				if (current->_kind == kind::dialog)
				{
					current->close(true);
					return true;
				}
				current = current->_parent.lock();
			}
		}
		return false;
	}

	bool frame_impl::key_up(const int key, const platform::key_state keys, const bool system)
	{
		const auto target = focus_target();
		if (!target) return false;
		const auto c = static_cast<char32_t>(key);
		const auto system_input = system || c == platform::keys::F10 || c == platform::keys::ALT || keys.alt;
		for (auto current = target->shared_from_this(); current; current = current->_parent.lock())
		{
			const auto manager = current->_controls;
			if (manager && (system_input || manager->in_system_mode()) && manager->system_key(key, keys, false)) return true;
		}
		return false;
	}

	void frame_impl::text(const std::string_view utf8)
	{
		const auto target = focus_target();
		if (!target) return;
		if (target->_controls->is_typing())
		{
			target->_controls->text_input(utf8);
			return;
		}
		if (!target->is_in_dialog())
			if (const auto app = _app.lock(); app && app->focus_mode() == ui::focus_mode::text_edit) app->text_input(utf8);
	}

	void frame_impl::focus_changed(const bool has_focus_value)
	{
		const auto owner = native_owner();
		if (!owner) return;
		owner->_native_focus = has_focus_value;
		const auto target = owner->focus_target();
		if (!has_focus_value)
		{
			for (auto current = target ? target->shared_from_this() : owner->self(); current; current = current->_parent.lock())
				if (current->_controls) current->_controls->cancel_system_mode();
		}
		if (!target) return;
		if (has_focus_value) target->focus_gained();
		else target->focus_lost();
		owner->update_text_input();
	}

	void frame_impl::activated(const bool active)
	{
		if (const auto host = _host.lock()) host->activate(active);
		if (!active)
		{
			for (auto current = self(); current; current = current->_parent.lock())
				if (current->_controls) current->_controls->cancel_system_mode();
			if (open_menu) open_menu->cancel();
		}
	}

	void frame_impl::close_requested()
	{
		close(true);
	}

	void frame_impl::context_menu(const core::pointi screen, const bool from_keyboard)
	{
		const auto owner = native_owner_shared();
		if (!owner || !owner->_window) return;
		std::shared_ptr<frame_impl> target;
		if (from_keyboard)
		{
			if (const auto focused = owner->focus_target()) target = focused->self();
			if (target && target->_controls->focused()) return;
		}
		else
		{
			const auto client = owner->_window->screen_to_client(screen);
			const auto hit = owner->hit_target(client);
			target = hit.first->self();
			if (target->_controls->context_menu(hit.second)) return;
		}

		// A region/native pane has no menu of its own: as with the old child-window message
		// chain, ask the root/parent host and pass its original screen coordinates through.
		const auto menu = owner->menu_owner();
		if (menu)
			if (const auto host = menu->_host.lock())
			{
				const auto commands = host->menu(screen);
				if (!commands.empty()) menu->show_app_menu(core::recti(screen, core::sizei{}), commands);
			}
	}

	void frame_impl::cancel_mode()
	{
		cancel_pointer_capture();
		if (open_menu) open_menu->cancel();
	}

	void frame_impl::timer(const uint32_t id)
	{
		if (_device && _device->timer(id)) return;
		const auto owner = native_owner();
		if (!owner) return;
		if (id == caret_timer_id)
		{
			if (const auto target = owner->_caret_owner.lock()) target->_controls->blink();
			return;
		}
		const auto found = owner->_timers.find(id);
		if (found == owner->_timers.end()) return;
		if (const auto target = found->second.lock())
		{
			if (target->_fade_timer_id == id) target->step_fade();
			else target->fire_timer();
		}
	}
	platform::hit_region frame_impl::hit_test(const core::pointi loc)
	{
		// This is always asked on the top-level window. Include native child windows as well as
		// region frames, because a child must pass caption/resize pixels through to its root.
		const auto hit = hit_target(loc);
		const auto target = hit.first;
		if (!target) return platform::hit_region::client;
		const auto local = hit.second;

		const frame_impl* caption_owner = target;
		while (caption_owner && caption_owner->_kind == kind::pane)
		{
			const auto parent = caption_owner->_parent.lock();
			if (!parent) break;
			caption_owner = parent.get();
		}
		const auto draws_caption = !caption_owner || caption_owner->_kind != kind::app ||
		                           caption_owner->_style.drawn_caption;

		// A toolbar maximize button is the one drawn control that is deliberately reported as a
		// native caption button. It may live in any frame, including a region or native child.
		if (draws_caption && target->_controls->caption_button_at(local) != 0)
			return platform::hit_region::maximize_button;
		if (target->_controls->hit(local)) return platform::hit_region::control;
		if (!draws_caption) return platform::hit_region::client;
		if (const auto host = target->_host.lock(); host && host->is_caption_area(local))
			return platform::hit_region::caption;

		// Only the dialog window's own ground moves the dialog. Child frames and popups have
		// their own pointer surfaces and must never test their points against the dialog host.
		if (target->_kind == kind::dialog)
			if (const auto host = target->_host.lock(); host && !host->is_control_area(local))
				return platform::hit_region::caption;
		return platform::hit_region::client;
	}

	void frame_impl::caption_button(const platform::caption_button_action action, const core::pointi loc)
	{
		const auto owner = native_owner();
		if (!owner) return;
		if (action == platform::caption_button_action::leave)
		{
			if (const auto tracked = owner->_caption_input.lock()) tracked->_controls->caption_leave();
			owner->_caption_input.reset();
			return;
		}
		const auto hit = owner->hit_target(loc);
		const auto target = hit.first ? hit.first->self() : nullptr;
		if (!target) return;
		const auto id = target->_controls->caption_button_at(hit.second);
		if (action == platform::caption_button_action::hover)
		{
			if (const auto tracked = owner->_caption_input.lock(); tracked && tracked != target) tracked->_controls->caption_leave();
			if (id) target->_controls->caption_hover(id);
			owner->_caption_input = id ? target : std::weak_ptr<frame_impl>{};
		}
		else if (action == platform::caption_button_action::press)
		{
			if (id) target->_controls->caption_press(id);
			owner->_caption_input = target;
		}
		else if (action == platform::caption_button_action::release)
		{
			if (const auto tracked = owner->_caption_input.lock()) tracked->_controls->caption_release(tracked == target ? id : 0);
			owner->_caption_input.reset();
		}
	}

	platform::drop_effect frame_impl::drag_over(const platform::clipboard_data& data, const platform::key_state keys,
	                                            const core::pointi loc)
	{
		const auto target = event_target(loc);
		if (target)
			if (const auto host = target->_host.lock()) return host->drag_over(data, keys, target_loc(target, loc));
		return platform::drop_effect::none;
	}

	platform::drop_effect frame_impl::drag_drop(platform::clipboard_data& data, const platform::key_state keys,
	                                            const core::pointi loc)
	{
		const auto target = event_target(loc);
		if (target)
			if (const auto host = target->_host.lock()) return host->drag_drop(data, keys, target_loc(target, loc));
		return platform::drop_effect::none;
	}

	void frame_impl::drag_leave()
	{
		const auto target = _hover_frame.lock();
		if (target)
		{
			if (const auto host = target->_host.lock()) host->drag_leave();
		}
		else if (const auto host = _host.lock()) host->drag_leave();
	}

	void frame_impl::system_event(const platform::os_event_type event)
	{
		if (_kind == kind::app)
			if (const auto app = _app.lock()) app->system_event(event);
	}

	render::measure_context* frame_impl::measure()
	{
		return measure_context();
	}

	void frame_impl::invalidate(const core::recti bounds)
	{
		invalidate(bounds, false);
	}

	bool frame_impl::has_keyboard_focus() const
	{
		return owns_focus();
	}

	// For a control that took the focus while its window did not have the keyboard: the control keeps it.
	void frame_impl::take_keyboard_focus()
	{
		const auto owner = native_owner_shared();
		if (!owner || !owner->_window) return;
		owner->change_focus(self());
		owner->_window->focus();
		if (owner->_native_focus) focus_gained();
	}

	void frame_impl::capture_pointer(const bool capture)
	{
		const auto owner = native_owner();
		if (!owner) return;
		if (capture)
		{
			owner->_capture_frame = weak_from_this();
			if (owner->_window) owner->_window->capture(true);
		}
		else if (owner->_capture_frame.lock().get() == this)
		{
			owner->_capture_frame.reset();
			if (!owner->_tracking && owner->_window) owner->_window->capture(false);
		}
	}

	core::pointi frame_impl::client_to_screen(const core::pointi loc) const
	{
		const auto owner = native_owner();
		return owner && owner->_window ? owner->_window->client_to_screen(loc + origin_in_native()) : loc;
	}

	ui::frame_ptr frame_impl::create_popup(const ui::frame_host_weak_ptr& content, const render::color_style& colors)
	{
		ui::frame_style style;
		style.child = false;
		style.no_focus = true;
		style.colors = colors;
		return create_frame(content, style);
	}

	std::shared_ptr<ui::frame_host> frame_impl::listener() const
	{
		return _host.lock();
	}

	void frame_impl::keyboard_to_frame()
	{
		if (owns_focus()) report_focus(true);
		update_text_input();
	}

	void frame_impl::text_input(const bool active, const core::recti caret)
	{
		_control_text_input = active;
		_control_caret = caret;
		update_text_input();
	}

	void frame_impl::caret_timer(const bool on, const uint32_t interval_ms)
	{
		const auto owner = native_owner();
		if (!owner || !owner->_window) return;
		if (owner->_caret_owner.lock().get() == this)
		{
			owner->_window->stop_timer(caret_timer_id);
			owner->_timers.erase(caret_timer_id);
		}
		owner->_caret_owner.reset();
		if (!on) return;
		owner->_caret_owner = weak_from_this();
		owner->_timers[caret_timer_id] = weak_from_this();
		owner->_window->start_timer(caret_timer_id, std::max(1u, interval_ms));
	}

	bool frame_impl::is_dialog() const
	{
		return _kind == kind::dialog;
	}

	bool frame_impl::presents_caption_button() const
	{
		// A native child has itself as native_owner(), but its toolbar still belongs to the
		// application's top-level caption. Walk out of child panes before asking the platform.
		const frame_impl* owner = this;
		while (owner && owner->_kind == kind::pane)
		{
			const auto parent = owner->_parent.lock();
			if (!parent) break;
			owner = parent.get();
		}
		return owner && owner->_kind == kind::app && owner->_style.drawn_caption && owner->_window &&
		       owner->_window->presents_caption_button();
	}

	render::measure_context* frame_impl::menu_measure()
	{
		return measure_context();
	}

	core::recti frame_impl::menu_work_area(const core::recti bounds)
	{
		return platform::monitor_work_area(bounds);
	}

	core::recti frame_impl::bubble_work_area(const core::recti anchor)
	{
		return platform::monitor_work_area(anchor);
	}

	class menu_filter final : public platform::modal_filter
	{
		frame_impl& _owner;
		ui::menu_session& _session;

		static bool is_popup(ui::menu_session& session, platform::window& target)
		{
			for (const auto& popup : session.popups())
			{
				if (const auto frame = dynamic_cast<frame_impl*>(popup.get()); frame && frame->native_window() == &target)
					return true;
			}
			return false;
		}

	public:
		explicit menu_filter(frame_impl& owner, ui::menu_session& session) : _owner(owner), _session(session) {}
		bool key_down(const int key, const platform::key_state keys) override
		{
			const auto horizontal = key == static_cast<int>(platform::keys::LEFT) ||
				key == static_cast<int>(platform::keys::RIGHT);
			if (horizontal)
			{
				if (_session.takes_horizontal_key(key)) return _session.key_down(key, keys);
				if (const auto controls = _owner.keyboard_controls(); controls && controls->system_key(key, keys, true))
				{
					_session.cancel();
					return true;
				}
			}
			return _session.key_down(key, keys);
		}
		bool pointer_down(platform::window& target, core::pointi) override
		{
			if (is_popup(_session, target)) return false;
			_session.cancel();
			return true;
		}
		void activation_lost() override { _session.cancel(); }
	};

	void frame_impl::run_menu_loop(ui::menu_session& session)
	{
		const auto previous = open_menu;
		open_menu = &session;
		menu_filter filter(*this, session);
		platform::run_modal_loop([&session] { return session.done(); }, &filter);
		open_menu = previous;
	}
	ui::frame_ptr frame_impl::create_dlg(const ui::frame_host_weak_ptr host)
	{
		const auto owner = native_owner_shared();
		if (!owner || !owner->_window) return nullptr;
		ui::frame_style style;
		style.child = false;
		style.colors = {ui::style::colors.dialog_background, ui::style::colors.dialog_text,
		                ui::style::colors.dialog_selected_background};
		auto result = std::make_shared<frame_impl>(host, _app, kind::dialog, style);
		platform::window_options options;
		options.kind = platform::window_kind::dialog;
		options.parent = owner->_window.get();
		options.can_focus = true;
		options.resizable = false;
		options.background = style.colors.background;
		if (!result->initialise_native(options)) return nullptr;
		result->_parent = self();
		add_child(result);
		return result;
	}

	ui::frame_ptr frame_impl::create_frame(const ui::frame_host_weak_ptr host, const ui::frame_style& style)
	{
		return create_frame(host, style, false);
	}

	ui::frame_ptr frame_impl::create_frame(const ui::frame_host_weak_ptr host, const ui::frame_style& style, const bool menu)
	{
		const auto owner = native_owner_shared();
		if (!owner || !owner->_window) return nullptr;
		if (style.child && !style.layered && !platform::child_windows())
		{
			auto result = std::make_shared<frame_impl>(host, _app, kind::pane, style, _host);
			result->initialise_region(self());
			add_child(result);
			layout();
			return result;
		}

		auto result = std::make_shared<frame_impl>(host, _app, kind::pane, style, _host);
		platform::window_options options;
		options.kind = style.child && !style.layered ? platform::window_kind::child : platform::window_kind::popup;
		options.parent = owner->_window.get();
		options.can_focus = !style.no_focus;
		options.menu = menu;
		options.resizable = false;
		options.layered = style.layered;
		options.accepts_drops = style.can_drop;
		options.container = style.container && platform::child_windows();
		options.background = style.colors.background;
		if (!result->initialise_native(options)) return nullptr;
		result->_parent = self();
		if (result->_device && (result->_window->kind() == platform::window_kind::child ||
		                        result->_window->kind() == platform::window_kind::popup))
			result->_device->scale_changed(result->scale_factor());
		result->sync_popup_scale();
		add_child(result);
		return result;
	}

	ui::bubble_window_ptr frame_impl::create_bubble()
	{
		return ui::create_drawn_bubble(std::shared_ptr<ui::bubble_host>(self(), static_cast<ui::bubble_host*>(this)));
	}

	ui::frame_ptr frame_impl::create_menu_popup(const ui::frame_host_weak_ptr& content, const ui::frame_ptr& parent)
	{
		ui::frame_style style;
		style.child = false;
		// The platform chooses the native menu focus behaviour; the menu flag is portable.
		style.timer_milliseconds = 50;
		style.colors = {ui::style::colors.menu_background, ui::style::colors.menu_text,
		                ui::style::colors.dialog_selected_background};
		if (const auto popup = std::dynamic_pointer_cast<frame_impl>(parent)) return popup->create_frame(content, style, true);
		return create_frame(content, style, true);
	}

	ui::frame_ptr frame_impl::create_bubble_popup(const ui::frame_host_weak_ptr& content)
	{
		ui::frame_style style;
		style.child = false;
		style.no_focus = true;
		style.layered = true;
		style.colors = {ui::style::colors.bubble_background, ui::style::colors.view_text,
		                ui::style::colors.view_selected_background};
		return create_frame(content, style);
	}

	render::measure_context* frame_impl::bubble_measure(const ui::frame_ptr& popup)
	{
		if (const auto frame = std::dynamic_pointer_cast<frame_impl>(popup)) return frame->measure_context();
		return nullptr;
	}

}

namespace
{
	ui::frame_ptr make_app_frame(const std::shared_ptr<ui::app>& app, const platform::setting_file_ptr& store,
	                             const ui::frame_host_weak_ptr& host, const ui::frame_style& style)
	{
		if (!app) return nullptr;
		const auto use_gpu = app->wants_gpu() &&
			!platform::crash_guard_suppressed(platform::crash_guard::gpu_render);
		if (!render::startup(use_gpu))
		{
			platform::show_startup_failure(platform::text().error_atl_direct3d);
			return nullptr;
		}
		auto app_style = style;
		app_style.child = false;
		auto result = std::make_shared<frame_impl>(host, app, frame_impl::kind::app, app_style);
		platform::window_options options;
		options.kind = platform::window_kind::app;
		options.visible = false;
		options.can_focus = !app_style.no_focus;
		options.accepts_drops = app_style.can_drop;
		options.drawn_caption = app_style.drawn_caption;
		options.client_size = app_style.client_size;
		options.minimum_client = app_style.minimum_client;
		options.offscreen = app_style.offscreen;
		options.resizable = app_style.resizable;
		options.resize_scale = static_cast<double>(render::base_font_size()) /
			static_cast<double>(render::default_font_size);
		options.container = app_style.container && platform::child_windows();
		options.background = app_style.colors.background;
		options.title = core::app_name();
		if (const auto placement = platform::read_placement(store, platform::placement_setting_name()))
		{
			options.bounds = placement->bounds;
			options.maximized = placement->maximized;
		}
		if (!result->initialise_native(options)) return nullptr;
		result->window_text(core::app_name());
		return result;
	}
}

ui::frame_ptr ui::create_app_frame(const std::shared_ptr<ui::app>& app, const platform::setting_file_ptr& store,
                                   const ui::frame_host_weak_ptr& host, const ui::frame_style& style)
{
	return make_app_frame(app, store, host, style);
}

ui::focus_snapshot ui::save_focus()
{
	ui::focus_snapshot result;
	if (const auto frame = keyboard_frame.lock())
	{
		result.window = frame;
		if (const auto control = frame->focused_control()) result.focused = control;
	}
	return result;
}

void ui::restore_focus(const ui::focus_snapshot& snapshot)
{
	if (const auto control = snapshot.focused.lock()) control->focus();
	else if (const auto frame = snapshot.window.lock()) frame->focus();
}
