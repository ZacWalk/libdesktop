// This file is part of libdesktop, a C++20 platform layer for desktop applications
// Copyright 2026  Zac Walker
// 
// This program is free software; you can redistribute it and / or modify it
// under the terms of the LGPL License either version 2.1 or later.
// License details are available at https://www.gnu.org/licenses/lgpl-2.1.html
// This program is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY

// Purpose: The element tree's out-of-line parts: hit testing through containers, the background an
// element's state paints and fades to, links, and the scroller's track and anchors.

#include "libdesktop/render.h"
#include "libdesktop/render_controls.h"

namespace ui
{
	void element::render_children(draw_context& dc, const pointi element_offset) const
	{
		const auto offset = element_offset + children_offset();

		visit_children([&dc, offset](const element_ptr& c)
		{
			if (c && c->is_visible()) c->render(dc, offset);
		});
	}

	controller_ptr element::controller_from_location(const element_host_ptr& host, const pointi loc,
	                                                 const pointi element_offset,
	                                                 hit_test_context& ctx)
	{
		controller_ptr result;
		if (!bounds.contains(loc - element_offset)) return result;

		const auto offset = element_offset + children_offset();

		visit_children([&](const element_ptr& c)
		{
			if (!result && c && c->is_visible()) result = c->controller_from_location(host, loc, offset, ctx);
		});

		return result;
	}


	static ui::color calc_clr(const element_style& style)
	{
		auto result_clr = ui::color();

		const auto tracking = style && element_style::tracking;
		const auto hover = style && element_style::hover;
		const auto selected = style && element_style::selected;
		const auto highlight = style && element_style::highlight;

		if (tracking || hover || selected || highlight)
		{
			result_clr = handle_color(selected || highlight, hover, tracking, true, true);
			result_clr.a = 1.0f;
		}
		else if (style && element_style::checked)
		{
			result_clr.a = 0.22f;
		}
		else if (style && element_style::important)
		{
			result_clr = ui::color(ui::style::colors.important_background);
			result_clr.a = 1.0f;
		}
		else if (style && element_style::info)
		{
			result_clr = ui::color(ui::style::colors.info_background);
			result_clr.a = 1.0f;
		}
		else if (style && element_style::dark_background)
		{
			result_clr.a = 0.777f;
		}
		else if (style && element_style::shaded_background)
		{
			result_clr.a = 0.44f;
		}
		else if (style && element_style::background)
		{
			result_clr.a = 0.222f;
		}

		return result_clr;
	}

	void element::update_background_color()
	{
		_bg_target = _bg_color = calc_clr(style);
	}

	void element::set_style_bit(const element_style mask, const bool state)
	{
		auto new_style = style;

		if (state)
		{
			new_style |= mask;
		}
		else
		{
			new_style &= ~mask;
		}

		if (new_style != style)
		{
			style = new_style;
			update_background_color();
		}
	}


	void element::set_style_bit(const element_style mask, const bool state, const element_host_base_ptr& view,
	                            const element_ptr& e)
	{
		auto new_style = style;

		if (state)
		{
			new_style |= mask;
		}
		else
		{
			new_style &= ~mask;
		}

		if (new_style != style)
		{
			style = new_style;
			const auto bg = calc_clr(style);

			if (view)
			{
				if (_bg_target != bg && e)
				{
					if (ui::is_alpha_zero(_bg_color.a))
					{
						_bg_color.r = bg.r;
						_bg_color.g = bg.g;
						_bg_color.b = bg.b;
						_bg_target = bg;
					}
					else if (ui::is_alpha_zero(bg.a))
					{
						_bg_target.a = 0.0f;
					}
					else
					{
						_bg_target = bg;
					}

					ui::animations[this] = [view, e]
					{
						const auto dd = e->_bg_target - e->_bg_color;
						bool invalidate = false;

						if (dd.abs_sum() > ui::color::color_epsilon)
						{
							// Gated and timed as animate_alpha is: under the CPU renderer the colour is
							// taken at once, and elsewhere the fade lasts as long at any refresh rate.
							e->_bg_color += ui::animations_enabled ? dd * ui::animation_step_factor : dd;
							view->invalidate_element(e);
							invalidate = true;
						}

						return invalidate;
					};

					view->invalidate(ui::host_invalid::animations);
				}
			}
			else
			{
				_bg_target = _bg_color = calc_clr(style);
			}
		}
	}

	void link_element::update_check(const element_event& event)
	{
		const bool is_checked = _cmd != commands{} && event.host->is_command_checked(_cmd);

		if (is_checked != is_style_bit_set(element_style::checked))
		{
			set_style_bit(element_style::checked, is_checked);
			event.host->invalidate(ui::host_invalid::redraw);
		}
	}

	void link_element::dispatch_event(const element_event& event)
	{
		if (event.type == element_event_type::update_command_state)
		{
			update_check(event);
		}
		else if (event.type == element_event_type::invoke)
		{
			if (_cmd != commands{})
			{
				event.host->invoke(_cmd);
				update_check(event);
			}
			else if (_invoke)
			{
				_invoke();
			}
		}
		else
		{
			text_element_base::dispatch_event(event);
		}
	}

	void link_element::tooltip(hover_bubble& hover, const pointi loc, const pointi element_offset) const
	{
		if (_tooltip)
		{
			_tooltip(hover);
		}
		else if (_cmd != commands{})
		{
			hover.id = _cmd;
		}

		hover.active_bounds = hover.window_bounds = bounds.offset(element_offset);
	}

	controller_ptr link_element::controller_from_location(const element_host_ptr& host, const pointi loc,
	                                                      const pointi element_offset,
	                                                      hit_test_context& ctx)
	{
		return default_controller_from_location(*this, host, loc, element_offset, ctx);
	}

	void scroller::draw_scroll(ui::draw_context& dc) const
	{
		if (can_scroll())
		{
			const auto text_line_height = dc.text_line_height(ui::style::font_face::dialog);
			const auto sb = track_bounds();
			const auto clr = handle_color(false, _active, _tracking, dc.frame_has_focus, false,
			                              ui::color(dc.colors.background));
			const auto bg_clr = ui::color(0x000000, dc.colors.alpha * dc.colors.bg_alpha);
			const auto handle_margin = track_inset();
			const auto left = sb.left + handle_margin;
			const auto right = sb.right - handle_margin;
			// The bands are a fixed map of the list, so they read the same whether or not the pointer
			// is over the column. Only the thumb answers the pointer.
			const auto label_color = ui::color(dc.colors.foreground, dc.colors.alpha);

			// The thumb is the backdrop of the column, not an overlay on it. It grows with the share of
			// the list on screen, so a short list puts it over most of the sections; drawn last it wiped
			// them out, worst of all under the pointer where it is at its brightest.
			dc.draw_rounded_rect(thumb_bounds(), clr, dc.padding1);

			for_each_band([&](const int band_top, const int band_bottom, const scroller_section* so)
			{
				const recti rr(left, sb.top + band_top, right, sb.top + band_bottom);

				dc.draw_rounded_rect(rr, bg_clr, dc.padding1);

				if (!so) return;

				// An icon needs less room than a text line, so gate each on what it actually costs.
				if (so->icon != icon_index{})
				{
					if (band_bottom - band_top > dc.icon_cxy) draw_icon(dc, so->icon, rr, label_color, {});
				}
				else if (!so->text.empty() && band_bottom - band_top > text_line_height)
				{
					dc.draw_text(so->text, rr, ui::style::font_face::dialog,
					             ui::style::text_style::single_line_center, label_color, {});
				}
			});
		}
	}

	scroll_anchor scroller::capture_anchor(const element_ptr& element) const
	{
		scroll_anchor result;
		result.valid = !_client_bounds.is_empty();
		result.was_at_start = _offset.y == 0;

		const auto max_scroll = std::max(0, _scroll_extent.cy - _client_bounds.height());
		result.scroll_ratio = max_scroll > 0 ? static_cast<double>(_offset.y) / max_scroll : 0.0;

		if (element)
		{
			const auto logical_bounds = _client_bounds.offset(0, _offset.y);
			if (element->bounds.intersects(logical_bounds))
			{
				result.element = element;
				result.device_top = element->bounds.top - _offset.y;
			}
		}

		return result;
	}

	int scroller::anchor_offset(const scroll_anchor& anchor, const bool element_is_current) const
	{
		if (!anchor.valid || anchor.was_at_start) return 0;
		if (anchor.element && element_is_current) return anchor.element->bounds.top - anchor.device_top;

		const auto max_scroll = std::max(0, _scroll_extent.cy - _client_bounds.height());
		return df::round(anchor.scroll_ratio * max_scroll);
	}

	void scroller::restore_anchor(const element_host_ptr& host, const scroll_anchor& anchor,
	                              const bool element_is_current)
	{
		if (!anchor.valid) return;
		scroll_offset(host, _offset.x, anchor_offset(anchor, element_is_current));
	}

	namespace
	{
		// The geometry a vertices object is handed moves with the context that made it.
		class offset_vertices final : public vertices
		{
		public:
			vertices_ptr _inner;
			std::shared_ptr<pointi> _origin;

			offset_vertices(vertices_ptr inner, std::shared_ptr<pointi> origin) : _inner(std::move(inner)),
				_origin(std::move(origin))
			{
			}

			void update_bars(const std::span<const vertex_rect> bars) override
			{
				std::vector<vertex_rect> moved(bars.begin(), bars.end());
				for (auto& r : moved) r.bounds = r.bounds.offset(*_origin);
				_inner->update_bars(moved);
			}

			void update_rects(const std::span<const vertex_rect> rects) override
			{
				std::vector<vertex_rect> moved(rects.begin(), rects.end());
				for (auto& r : moved) r.bounds = r.bounds.offset(*_origin);
				_inner->update_rects(moved);
			}
		};
	}

	offset_draw_context::offset_draw_context(draw_context& dc, std::shared_ptr<pointi> origin) : _dc(dc),
		_origin(std::move(origin))
	{
		scale_factor = dc.scale_factor;
		icon_cxy = dc.icon_cxy;
		padding2 = dc.padding2;
		padding1 = dc.padding1;
		handle_cxy = dc.handle_cxy;
		scroll_width = dc.scroll_width;
		col_widths = dc.col_widths;
		time_now = dc.time_now;
		colors = dc.colors;
		frame_has_focus = dc.frame_has_focus;
	}

	void offset_draw_context::clear(const color c)
	{
		_dc.draw_rect(_dc.clip_bounds(), c);
	}

	void offset_draw_context::draw_rounded_rect(const recti bounds, const color c, const int radius)
	{
		_dc.draw_rounded_rect(to_dc(bounds), c, radius);
	}

	void offset_draw_context::draw_rect(const recti bounds, const color c)
	{
		_dc.draw_rect(to_dc(bounds), c);
	}

	void offset_draw_context::draw_text(const std::string_view text, const recti bounds, const style::font_face font,
	                                    const style::text_style style, const color c, const color bg)
	{
		_dc.draw_text(text, to_dc(bounds), font, style, c, bg);
	}

	void offset_draw_context::draw_text(const std::string_view text, const std::vector<text_highlight_t>& highlights,
	                                    const recti bounds, const style::font_face font, const style::text_style style,
	                                    const color clr, const color bg)
	{
		_dc.draw_text(text, highlights, to_dc(bounds), font, style, clr, bg);
	}

	void offset_draw_context::draw_text(const text_layout_ptr& tl, const recti bounds, const color clr,
	                                    const color bg)
	{
		_dc.draw_text(tl, to_dc(bounds), clr, bg);
	}

	void offset_draw_context::draw_border(const recti inside, const recti outside, const color c_inside,
	                                      const color c_outside)
	{
		_dc.draw_border(to_dc(inside), to_dc(outside), c_inside, c_outside);
	}

	void offset_draw_context::draw_texture(const texture_ptr& t, const recti dst, const float alpha,
	                                       const texture_sampler sampler)
	{
		_dc.draw_texture(t, to_dc(dst), alpha, sampler);
	}

	void offset_draw_context::draw_texture(const texture_ptr& t, const recti dst, const recti src, const float alpha,
	                                       const texture_sampler sampler, const float radius)
	{
		_dc.draw_texture(t, to_dc(dst), src, alpha, sampler, radius);
	}

	void offset_draw_context::draw_texture(const texture_ptr& t, const quadd& dst, const recti src, const float alpha,
	                                       const texture_sampler sampler)
	{
		_dc.draw_texture(t, dst.offset(_origin->x, _origin->y), src, alpha, sampler);
	}

	void offset_draw_context::draw_texture(const texture_ptr& t, const quadd& dst, const recti src, const float alpha,
	                                       const texture_sampler sampler, const texture_transform& transform)
	{
		_dc.draw_texture(t, dst.offset(_origin->x, _origin->y), src, alpha, sampler, transform);
	}

	bool offset_draw_context::draw_panorama(const texture_ptr& t, const recti dst, const panorama_params& params,
	                                        const float alpha)
	{
		return _dc.draw_panorama(t, to_dc(dst), params, alpha);
	}

	void offset_draw_context::draw_vertices(const vertices_ptr& v)
	{
		const auto moved = std::dynamic_pointer_cast<offset_vertices>(v);
		_dc.draw_vertices(moved ? moved->_inner : v);
	}

	sizei offset_draw_context::measure_text(const std::string_view text, const style::font_face font,
	                                        const style::text_style style, const int width, const int height)
	{
		return _dc.measure_text(text, font, style, width, height);
	}

	int offset_draw_context::text_line_height(const style::font_face font)
	{
		return _dc.text_line_height(font);
	}

	texture_ptr offset_draw_context::create_texture()
	{
		return _dc.create_texture();
	}

	vertices_ptr offset_draw_context::create_vertices()
	{
		return std::make_shared<offset_vertices>(_dc.create_vertices(), _origin);
	}

	text_layout_ptr offset_draw_context::create_text_layout(const style::font_face font)
	{
		return _dc.create_text_layout(font);
	}

	recti offset_draw_context::clip_bounds() const
	{
		return _dc.clip_bounds().offset(-_origin->x, -_origin->y);
	}

	void offset_draw_context::clip_bounds(const recti r)
	{
		_dc.clip_bounds(to_dc(r));
	}

	void offset_draw_context::restore_clip()
	{
		_dc.restore_clip();
	}
}
