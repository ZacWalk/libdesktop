// pane_host.h — running libdesktop-ui views inside one window.
//
// Every view here is a pf::frame_reactor: it expects a window of its own, and it
// asks that window for focus, capture, timers and repaints. dde gives each view
// a real child window, so its views fit natively. An application that is a single
// window drawing its panes into rectangles does not, so without this it cannot
// adopt a shared view at all.
//
// pf::window_frame is an interface, not a window, which is the whole seam. The
// headless test fakes have been driving these views with no window for three
// phases; this is the same trick in production. A hosted_window answers a view's
// questions in terms of its pane: "invalidate" means repaint my rectangle of the
// parent, "set focus" means route the keyboard to me, "capture" means the parent
// captures and forwards.
//
// The application keeps its single reactor and forwards each event to the host,
// which routes it. Nothing about the application's window model changes.
//
// The parent is held weakly and each pane holds the host by plain pointer: a
// window owns its reactor, which is what owns the host, so anything stronger
// would be a cycle that never frees the window.

#pragma once

#include "platform.h"

#include <algorithm>
#include <memory>
#include <unordered_map>
#include <vector>

namespace pf::ui
{
	// A draw_context that shifts everything into a pane's rectangle.
	//
	// A view draws at its own coordinates, with 0,0 at its top-left corner. Rather
	// than teach every view where it really is, the surface it is handed does the
	// translating — and clamps it, so a view cannot paint over its neighbours.
	//
	// pf::draw_context clipping replaces rather than nests (SelectClipRgn on Win32),
	// so this replaces too: a pane is clipped for as long as it is painting, and the
	// clip is dropped when it finishes. An application that had a clip of its own set
	// before calling into the host must set it again afterwards.
	class offset_draw_context final : public pf::draw_context
	{
	public:
		offset_draw_context(pf::draw_context& target, const pf::irect& pane)
			: _target(target), _pane(pane)
		{
			_target.set_clip_rect(_pane);
		}

		~offset_draw_context() override
		{
			_target.clear_clip_rect();
		}

		offset_draw_context(const offset_draw_context&) = delete;
		offset_draw_context& operator=(const offset_draw_context&) = delete;

		// The dirty rectangle, in the pane's own coordinates.
		[[nodiscard]] pf::irect clip_rect() const override
		{
			return to_pane(intersect(_target.clip_rect(), _pane));
		}

		void fill_solid_rect(const pf::irect& rc, const pf::color_t color) override
		{
			_target.fill_solid_rect(to_parent(rc), color);
		}

		void fill_solid_rect(const int x, const int y, const int cx, const int cy,
		                     const pf::color_t color) override
		{
			_target.fill_solid_rect(x + _pane.left, y + _pane.top, cx, cy, color);
		}

		void draw_text(const int x, const int y, const pf::irect& clip, const std::string_view text,
		               const pf::font& f, const pf::color_t text_color, const pf::color_t bg_color) override
		{
			_target.draw_text(x + _pane.left, y + _pane.top, to_parent(clip), text, f, text_color, bg_color);
		}

		[[nodiscard]] pf::isize measure_text(const std::string_view text, const pf::font& f) const override
		{
			return _target.measure_text(text, f);
		}

		void draw_text_h(const int x, const int y, const std::string_view text,
		                 const pf::font_handle handle, const pf::color_t text_color) override
		{
			_target.draw_text_h(x + _pane.left, y + _pane.top, text, handle, text_color);
		}

		[[nodiscard]] pf::isize measure_text_h(const std::string_view text,
		                                       const pf::font_handle handle) const override
		{
			return _target.measure_text_h(text, handle);
		}

		void draw_lines(const std::span<const pf::ipoint> points, const pf::color_t color) override
		{
			_points.assign(points.begin(), points.end());
			for (auto& p : _points) p = {p.x + _pane.left, p.y + _pane.top};
			_target.draw_lines(_points, color);
		}

		void draw_solid_line(const pf::ipoint a, const pf::ipoint b, const pf::color_t color,
		                     const int width) override
		{
			_target.draw_solid_line({a.x + _pane.left, a.y + _pane.top},
			                        {b.x + _pane.left, b.y + _pane.top}, color, width);
		}

		void draw_ellipse(const int x, const int y, const int w, const int h,
		                  const pf::color_t color, const int line_width) override
		{
			_target.draw_ellipse(x + _pane.left, y + _pane.top, w, h, color, line_width);
		}

		void fill_ellipse(const int x, const int y, const int w, const int h, const pf::color_t color) override
		{
			_target.fill_ellipse(x + _pane.left, y + _pane.top, w, h, color);
		}

		// A view narrowing its clip narrows it within the pane; a view clearing it
		// gets the pane back, never the whole window. Otherwise a pane that clears
		// its clip — a markdown pane does — would be free to paint over the
		// rest of the application.
		void set_clip_rect(const pf::irect& rc) override
		{
			_target.set_clip_rect(intersect(to_parent(rc), _pane));
		}

		void clear_clip_rect() override
		{
			_target.set_clip_rect(_pane);
		}

		void draw_bitmap(const int x, const int y, const struct pf::bitmap& bmp) override
		{
			_target.draw_bitmap(x + _pane.left, y + _pane.top, bmp);
		}

		void draw_bitmap(const pf::irect& dest, const struct pf::bitmap& bmp) override
		{
			_target.draw_bitmap(to_parent(dest), bmp);
		}

	private:
		pf::draw_context& _target;
		pf::irect _pane;
		std::vector<pf::ipoint> _points; // scratch, so drawing a polyline allocates once

		[[nodiscard]] pf::irect to_parent(const pf::irect& r) const
		{
			return {r.left + _pane.left, r.top + _pane.top, r.right + _pane.left, r.bottom + _pane.top};
		}

		[[nodiscard]] pf::irect to_pane(const pf::irect& r) const
		{
			return {r.left - _pane.left, r.top - _pane.top, r.right - _pane.left, r.bottom - _pane.top};
		}

		// Two rectangles that do not meet intersect to an empty one *inside the
		// pane*, never to an inverted one: a backend that normalises a reversed
		// rectangle would otherwise turn "nothing" into a region over the neighbours.
		static pf::irect intersect(const pf::irect& a, const pf::irect& b)
		{
			const auto left = std::max(a.left, b.left);
			const auto top = std::max(a.top, b.top);
			const auto right = std::min(a.right, b.right);
			const auto bottom = std::min(a.bottom, b.bottom);

			if (right <= left || bottom <= top) return {left, top, left, top};
			return {left, top, right, bottom};
		}
	};

	class pane_host;

	// The window a hosted view thinks it has.
	//
	// Everything that is really the parent's — the clipboard, key state, the DPI —
	// passes straight through. Everything that is about *this pane* — its extent,
	// where a repaint lands, whether it has the focus — is answered by the host.
	//
	// A detached pane answers harmlessly rather than reaching a host that is gone,
	// because a view handed out as a window_frame_ptr can outlive its host.
	class hosted_window final : public pf::window_frame, public std::enable_shared_from_this<hosted_window>
	{
		friend class pane_host;

	public:
		explicit hosted_window(pane_host& host) : _host(&host)
		{
		}

		~hosted_window() override = default;

		[[nodiscard]] const pf::irect& pane_bounds() const { return _bounds; }
		[[nodiscard]] const pf::frame_reactor_ptr& reactor() const { return _reactor; }
		[[nodiscard]] bool attached() const { return _host != nullptr; }

		void set_reactor(pf::frame_reactor_ptr reactor) override { _reactor = std::move(reactor); }

		void notify_size() override;

		// A view works in its own coordinates, so its client rect starts at zero.
		[[nodiscard]] pf::irect get_client_rect() const override
		{
			return {0, 0, _bounds.width(), _bounds.height()};
		}

		void invalidate() override
		{
			if (const auto p = parent()) p->invalidate_rect(_bounds);
		}

		void invalidate_rect(const pf::irect& rect) override
		{
			if (const auto p = parent())
				p->invalidate_rect({
					rect.left + _bounds.left, rect.top + _bounds.top,
					rect.right + _bounds.left, rect.bottom + _bounds.top
				});
		}

		void set_focus() override;
		[[nodiscard]] bool has_focus() const override;
		void set_capture() override;
		void release_capture() override;

		uint32_t set_timer(uint32_t id, uint32_t ms) override;
		void kill_timer(uint32_t id) override;

		[[nodiscard]] pf::ipoint screen_to_client(const pf::ipoint pt) const override
		{
			const auto p = parent();
			if (!p) return pt;

			const auto client = p->screen_to_client(pt);
			return {client.x - _bounds.left, client.y - _bounds.top};
		}

		void set_cursor_shape(const pf::cursor_shape shape) override
		{
			if (const auto p = parent()) p->set_cursor_shape(shape);
		}

		// Moving a pane is the host's business, so this positions it rather than
		// asking the operating system for anything.
		void move_window(const pf::irect& bounds) override;

		void show(bool visible) override;

		[[nodiscard]] bool is_visible() const override { return _visible; }

		// A pane has no title bar to put it in.
		void set_text(std::string_view) override
		{
		}

		std::string text_from_clipboard() override
		{
			const auto p = parent();
			return p ? p->text_from_clipboard() : std::string{};
		}

		bool text_to_clipboard(const std::string_view text) override
		{
			const auto p = parent();
			return p && p->text_to_clipboard(text);
		}

		[[nodiscard]] placement get_placement() const override
		{
			const auto p = parent();
			return p ? p->get_placement() : placement{};
		}

		void set_placement(const placement&) override
		{
		}

		void track_mouse_leave() override
		{
			if (const auto p = parent()) p->track_mouse_leave();
		}

		[[nodiscard]] bool is_key_down(const unsigned int vk) const override
		{
			const auto p = parent();
			return p && p->is_key_down(vk);
		}

		[[nodiscard]] bool is_key_down_async(const unsigned int vk) const override
		{
			const auto p = parent();
			return p && p->is_key_down_async(vk);
		}

		[[nodiscard]] pf::window_frame_ptr create_child(const std::string_view class_name, const uint32_t style,
		                                               const pf::color_t background) const & override
		{
			const auto p = parent();
			return p ? p->create_child(class_name, style, background) : nullptr;
		}

		// A pane cannot close the window it is only borrowing.
		void close() override
		{
		}

		int message_box(const std::string_view text, const std::string_view title, const uint32_t style) override
		{
			const auto p = parent();
			return p ? p->message_box(text, title, style) : 0;
		}

		void present_pixels(const uint32_t*, int, int) override
		{
		}

		void set_menu(std::vector<pf::menu_command>) override
		{
		}

		[[nodiscard]] std::unique_ptr<pf::measure_context> create_measure_context() const override
		{
			const auto p = parent();
			return p ? p->create_measure_context() : nullptr;
		}

		void show_popup_menu(const std::vector<pf::menu_command>& items, const pf::ipoint& screen_pt) override
		{
			if (const auto p = parent()) p->show_popup_menu(items, screen_pt);
		}

		[[nodiscard]] double get_dpi_scale() const override
		{
			const auto p = parent();
			return p ? p->get_dpi_scale() : 1.0;
		}

		void accept_drop_files(const bool accept) override
		{
			if (const auto p = parent()) p->accept_drop_files(accept);
		}

		pf::toolbar_frame_ptr create_address_bar(const pf::address_bar_config& cfg) override
		{
			const auto p = parent();
			return p ? p->create_address_bar(cfg) : nullptr;
		}

	private:
		pane_host* _host;
		pf::frame_reactor_ptr _reactor;
		pf::irect _bounds;
		bool _visible = true;

		// The ids this pane asked for, mapped to the ones the parent really runs.
		// Two panes both blinking a caret would otherwise ask for the same id.
		std::unordered_map<uint32_t, uint32_t> _timers;

		[[nodiscard]] pf::window_frame_ptr parent() const;

		void detach()
		{
			_host = nullptr;
			_timers.clear();
		}
	};

	using hosted_window_ptr = std::shared_ptr<hosted_window>;

	// Routes one window's events to the panes drawn inside it.
	//
	// The application keeps its own reactor and forwards; this decides which pane
	// an event belongs to and translates it on the way.
	class pane_host
	{
	public:
		explicit pane_host(const pf::window_frame_ptr& parent) : _parent(parent)
		{
		}

		~pane_host()
		{
			const auto parent = parent_frame();

			for (const auto& pane : _panes)
			{
				if (parent)
					for (const auto& [pane_id, real_id] : pane->_timers)
						parent->kill_timer(real_id);

				pane->detach();
			}

			_timer_ids.clear();
		}

		pane_host(const pane_host&) = delete;
		pane_host& operator=(const pane_host&) = delete;

		[[nodiscard]] pf::window_frame_ptr parent_frame() const { return _parent.lock(); }

		// Adds a pane and gives it the window it believes in. Panes are hit-tested
		// in reverse order of addition, so a later pane sits above an earlier one.
		hosted_window_ptr add_pane(pf::frame_reactor_ptr reactor)
		{
			auto pane = std::make_shared<hosted_window>(*this);
			pane->set_reactor(std::move(reactor));
			_panes.push_back(pane);
			return pane;
		}

		void remove_pane(const hosted_window_ptr& pane)
		{
			if (!pane) return;

			deactivate(pane.get());
			pane->detach();
			std::erase(_panes, pane);
		}

		// Positions a pane and, when its size changed, tells it — which is what
		// every view uses to lay itself out.
		void set_pane_bounds(const hosted_window_ptr& pane, const pf::irect& bounds,
		                     pf::measure_context& measure)
		{
			if (!pane) return;

			const auto previous = pane->_bounds;
			pane->_bounds = bounds;

			if (const auto parent = parent_frame())
			{
				// The rectangle it left behind still has its old pixels in it
				parent->invalidate_rect(previous);
				parent->invalidate_rect(bounds);
			}

			const auto resized = previous.width() != bounds.width() || previous.height() != bounds.height();
			if (!resized || !pane->_reactor) return;

			pf::window_frame_ptr frame = pane;
			pane->_reactor->handle_size(frame, {bounds.width(), bounds.height()}, measure);
		}

		// --- The application forwards its reactor's events here ---

		void handle_paint(pf::draw_context& draw) const
		{
			const auto clip = draw.clip_rect();

			for (const auto& pane : _panes)
			{
				if (!pane->_visible || !pane->_reactor) continue;
				if (pane->_bounds.width() <= 0 || pane->_bounds.height() <= 0) continue;

				// Skip a pane the repaint does not touch
				if (pane->_bounds.right <= clip.left || pane->_bounds.left >= clip.right ||
					pane->_bounds.bottom <= clip.top || pane->_bounds.top >= clip.bottom)
					continue;

				offset_draw_context pane_draw(draw, pane->_bounds);
				pf::window_frame_ptr frame = pane;
				pane->_reactor->handle_paint(frame, pane_draw);
			}
		}

		// Returns true when a pane consumed the event.
		//
		// Not every mouse message carries a client point. set_cursor and
		// mouse_activate carry none at all — their point is really the hit-test and
		// the message packed into one word — and context_menu carries a *screen*
		// point, or {-1,-1} when it came from the keyboard. Routing those by
		// subtracting the pane origin would pick the wrong pane and corrupt what the
		// view then reads.
		bool handle_mouse(const pf::mouse_message_type msg, const pf::mouse_params& params)
		{
			using mt = pf::mouse_message_type;

			if (msg == mt::set_cursor || msg == mt::mouse_activate)
			{
				auto* const target = _captured ? _captured : visible_pane_at(cursor_in_parent());
				return forward_mouse_unchanged(target, msg, params);
			}

			if (msg == mt::context_menu)
			{
				const auto from_keyboard = params.point.x == -1 && params.point.y == -1;
				auto* const target = from_keyboard
					                     ? _focused
					                     : _captured
					                     ? _captured
					                     : visible_pane_at(to_parent_client(params.point));

				// The point stays a screen point: the view converts it itself, and
				// its window subtracts the pane origin when it does.
				return forward_mouse_unchanged(target, msg, params);
			}

			// A capture outlives the pointer leaving the pane — that is what makes
			// dragging a selection past the edge work.
			auto* const target = _captured ? _captured : visible_pane_at(params.point);

			if (msg == mt::mouse_leave)
			{
				auto* const leaving = std::exchange(_hovered, nullptr);
				if (leaving) send_mouse(leaving, msg, params);
				return leaving != nullptr;
			}

			if (msg == mt::mouse_move && !_captured)
			{
				// Moving between panes leaves the one behind, so its hover state
				// clears even though the parent window never saw a leave.
				if (_hovered && _hovered != target)
				{
					send_mouse(_hovered, mt::mouse_leave, params);
					_hovered = nullptr;
				}

				_hovered = target;
			}

			if (!target) return false;

			send_mouse(target, msg, params);
			return true;
		}

		bool handle_keyboard(const pf::keyboard_message_type msg, const pf::keyboard_params& params) const
		{
			if (!_focused || !_focused->_reactor) return false;

			pf::window_frame_ptr frame = _focused->shared_from_this();
			_focused->_reactor->handle_keyboard(frame, msg, params);
			return true;
		}

		// Handles the messages a pane can own — timers, focus, and the window-wide
		// changes a view has to react to — and reports whether one was consumed.
		bool handle_message(const pf::message_type msg, const pf::message_params& params)
		{
			using mt = pf::message_type;

			if (msg == mt::timer)
			{
				const auto found = _timer_ids.find(params.timer_id);
				if (found == _timer_ids.end()) return false;

				auto* const pane = found->second.pane;
				const auto pane_id = found->second.pane_id;
				if (!pane->_reactor) return false;

				auto forwarded = params;
				forwarded.timer_id = pane_id;

				pf::window_frame_ptr frame = pane->shared_from_this();
				pane->_reactor->handle_message(frame, msg, forwarded);
				return true;
			}

			// The window gaining or losing focus is the focused pane gaining or
			// losing it too, or a caret would blink on in a window that is not active.
			if (msg == mt::set_focus || msg == mt::kill_focus)
			{
				if (!_focused || !_focused->_reactor) return false;
				notify(_focused, msg);
				return true;
			}

			// A palette or scale change belongs to the whole window, so every pane
			// hears it — a hosted view would otherwise keep colours the rest of the
			// application had already repainted.
			if (msg == mt::sys_color_change || msg == mt::dpi_changed)
			{
				auto delivered = false;

				for (const auto& pane : _panes)
				{
					if (!pane->_reactor) continue;
					pf::window_frame_ptr frame = pane;
					pane->_reactor->handle_message(frame, msg, params);
					delivered = true;
				}

				return delivered;
			}

			return false;
		}

		// --- Focus ---

		[[nodiscard]] hosted_window* focused_pane() const { return _focused; }

		// A host arbitrating a drag with its own needs to know whether a pane is
		// mid-drag, and to be able to end that drag on its terms.
		[[nodiscard]] hosted_window* captured_pane() const { return _captured; }

		// Ends a pane drag the way a real window would: the pane is told the button
		// came up where the mouse actually is, so it finalizes its selection and
		// releases its own capture. A pane that does not release it loses it anyway,
		// or the window would stay captured with nothing tracking the mouse.
		void release_all_capture()
		{
			auto* const held = _captured;
			if (!held) return;

			pf::mouse_params params{};
			params.point = cursor_in_parent();
			send_mouse(held, pf::mouse_message_type::left_button_up, params);

			if (_captured == held)
			{
				_captured = nullptr;
				if (const auto parent = parent_frame()) parent->release_capture();
			}
		}

		void focus_pane(const hosted_window_ptr& pane) { focus_pane(pane.get()); }

		void focus_pane(hosted_window* pane)
		{
			// Asking again for focus a pane already holds still activates the window,
			// which may have lost it since — exactly as a real window would. The
			// parent is focused first, so has_focus() is already true when the pane
			// is told.
			if (pane)
				if (const auto parent = parent_frame()) parent->set_focus();

			if (_focused == pane) return;

			auto* const previous = std::exchange(_focused, pane);

			// Both sides are told, so the one losing focus stops its caret and the
			// one gaining it starts.
			notify(previous, pf::message_type::kill_focus);
			notify(_focused, pf::message_type::set_focus);
		}

		[[nodiscard]] hosted_window* pane_at(const pf::ipoint& point) const { return visible_pane_at(point); }

	private:
		friend class hosted_window;

		struct timer_entry
		{
			hosted_window* pane;
			uint32_t pane_id;
		};

		std::weak_ptr<pf::window_frame> _parent;
		std::vector<hosted_window_ptr> _panes;
		hosted_window* _focused = nullptr;
		hosted_window* _captured = nullptr;
		hosted_window* _hovered = nullptr;

		// Ids handed to the parent, and the pane and id they came from. The base is
		// high enough to sit clear of the timers an application runs itself.
		std::unordered_map<uint32_t, timer_entry> _timer_ids;
		uint32_t _next_timer_id = 0x7000;

		[[nodiscard]] hosted_window* visible_pane_at(const pf::ipoint& point) const
		{
			for (auto it = _panes.rbegin(); it != _panes.rend(); ++it)
			{
				const auto& pane = *it;
				if (!pane->_visible) continue;

				const auto& b = pane->_bounds;
				if (point.x >= b.left && point.x < b.right && point.y >= b.top && point.y < b.bottom)
					return pane.get();
			}

			return nullptr;
		}

		bool forward_mouse_unchanged(hosted_window* target, const pf::mouse_message_type msg,
		                             const pf::mouse_params& params) const
		{
			if (!target || !target->_reactor) return false;

			pf::window_frame_ptr frame = target->shared_from_this();
			target->_reactor->handle_mouse(frame, msg, params);
			return true;
		}

		[[nodiscard]] pf::ipoint to_parent_client(const pf::ipoint& screen_pt) const
		{
			const auto parent = parent_frame();
			return parent ? parent->screen_to_client(screen_pt) : screen_pt;
		}

		[[nodiscard]] pf::ipoint cursor_in_parent() const { return to_parent_client(pf::platform_cursor_pos()); }

		void notify(hosted_window* pane, const pf::message_type msg) const
		{
			if (!pane || !pane->_reactor) return;

			pf::window_frame_ptr frame = pane->shared_from_this();
			pane->_reactor->handle_message(frame, msg, {});
		}

		void send_mouse(hosted_window* pane, const pf::mouse_message_type msg,
		                const pf::mouse_params& params) const
		{
			if (!pane->_reactor) return;

			auto local = params;
			local.point = {params.point.x - pane->_bounds.left, params.point.y - pane->_bounds.top};

			pf::window_frame_ptr frame = pane->shared_from_this();
			pane->_reactor->handle_mouse(frame, msg, local);
		}

		// Everything the host is holding on a pane's behalf, given back. Used when a
		// pane is hidden, removed or destroyed: a pane nobody can see must not keep
		// the keyboard, the mouse or a timer.
		void deactivate(hosted_window* pane)
		{
			const auto parent = parent_frame();

			if (_hovered == pane) _hovered = nullptr;

			if (_captured == pane)
			{
				_captured = nullptr;
				if (parent) parent->release_capture();
			}

			if (_focused == pane)
			{
				_focused = nullptr;
				notify(pane, pf::message_type::kill_focus);
			}

			for (const auto& [pane_id, real_id] : pane->_timers)
			{
				if (parent) parent->kill_timer(real_id);
				_timer_ids.erase(real_id);
			}

			pane->_timers.clear();
		}

		uint32_t register_timer(hosted_window* pane, const uint32_t pane_id, const uint32_t ms)
		{
			// Zero is how this interface reports failure, so it cannot also be a
			// usable id.
			if (pane_id == 0) return 0;

			const auto parent = parent_frame();
			if (!parent) return 0;

			// Asking again for an id the pane already runs restarts it, which is
			// what a real window does.
			release_timer(pane, pane_id);

			// The id the parent answers with is the one it will deliver, which need
			// not be the one that was asked for.
			const auto real_id = parent->set_timer(_next_timer_id++, ms);
			if (real_id == 0) return 0;

			_timer_ids[real_id] = {pane, pane_id};
			pane->_timers[pane_id] = real_id;
			return pane_id;
		}

		void release_timer(hosted_window* pane, const uint32_t pane_id)
		{
			const auto found = pane->_timers.find(pane_id);
			if (found == pane->_timers.end()) return;

			if (const auto parent = parent_frame()) parent->kill_timer(found->second);

			_timer_ids.erase(found->second);
			pane->_timers.erase(found);
		}

		void capture(hosted_window* pane)
		{
			_captured = pane;
			if (const auto parent = parent_frame()) parent->set_capture();
		}

		void release_capture_for(const hosted_window* pane)
		{
			if (_captured != pane) return;

			_captured = nullptr;
			if (const auto parent = parent_frame()) parent->release_capture();
		}

		[[nodiscard]] bool pane_has_focus(const hosted_window* pane) const
		{
			const auto parent = parent_frame();
			return _focused == pane && parent && parent->has_focus();
		}

		void reposition(hosted_window* pane, const pf::irect& bounds)
		{
			const auto previous = pane->_bounds;
			pane->_bounds = bounds;

			const auto parent = parent_frame();

			if (parent)
			{
				parent->invalidate_rect(previous);
				parent->invalidate_rect(bounds);
			}

			const auto resized = previous.width() != bounds.width() || previous.height() != bounds.height();
			if (!resized || !pane->_reactor || !parent) return;

			// A view that is resized has to be told, or it lays out to its old extent
			if (const auto measure = parent->create_measure_context())
			{
				pf::window_frame_ptr frame = pane->shared_from_this();
				pane->_reactor->handle_size(frame, {bounds.width(), bounds.height()}, *measure);
			}
		}

		void set_visible(hosted_window* pane, const bool visible)
		{
			if (pane->_visible == visible) return;

			pane->_visible = visible;

						// A pane nobody can see keeps neither the keyboard, the mouse nor a timer
						if (!visible) deactivate(pane);

						if (const auto parent = parent_frame()) parent->invalidate_rect(pane->_bounds);
		}
	};

	inline pf::window_frame_ptr hosted_window::parent() const
	{
		return _host ? _host->parent_frame() : nullptr;
	}

	inline void hosted_window::notify_size()
	{
		if (!_reactor) return;

		const auto p = parent();
		if (!p) return;

		if (const auto measure = p->create_measure_context())
		{
			pf::window_frame_ptr frame = shared_from_this();
			_reactor->handle_size(frame, {_bounds.width(), _bounds.height()}, *measure);
		}
	}

	inline void hosted_window::set_focus()
	{
		if (_host) _host->focus_pane(this);
	}

	inline bool hosted_window::has_focus() const
	{
		return _host && _host->pane_has_focus(this);
	}

	inline void hosted_window::set_capture()
	{
		if (_host) _host->capture(this);
	}

	inline void hosted_window::release_capture()
	{
		if (_host) _host->release_capture_for(this);
	}

	inline uint32_t hosted_window::set_timer(const uint32_t id, const uint32_t ms)
	{
		return _host ? _host->register_timer(this, id, ms) : 0;
	}

	inline void hosted_window::kill_timer(const uint32_t id)
	{
		if (_host) _host->release_timer(this, id);
	}

	inline void hosted_window::move_window(const pf::irect& bounds)
	{
		if (_host) _host->reposition(this, bounds);
		else _bounds = bounds;
	}

	inline void hosted_window::show(const bool visible)
	{
		if (_host) _host->set_visible(this, visible);
		else _visible = visible;
	}
}
