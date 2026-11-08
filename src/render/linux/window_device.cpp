// This file is part of libdesktop, a C++20 platform layer for desktop applications
// Copyright 2026  Zac Walker
//
// This program is free software; you can redistribute it and / or modify it
// under the terms of the LGPL License either version 2.1 or later.
// License details are available at https://www.gnu.org/licenses/lgpl-2.1.html
// This program is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY

// Purpose: The SDL3 software renderer bound to one platform::window.

#include "libdesktop/render.h"
#include "libdesktop/render/software.h"

#include <SDL3/SDL.h>

namespace render::detail
{
	bool backend_startup(bool use_gpu);
	void backend_shutdown();
	bool backend_downgrade();
	bool backend_gpu_active();

	void register_window_device(void* key, std::function<void()> prepare_loss,
	                            std::function<void()> rebuild_loss, std::function<void()> update_font,
	                            std::function<void()> invalidate);
	void unregister_window_device(void* key);
}

namespace
{
	constexpr auto ui_component_snap = 8;
	constexpr auto ui_baseline_snap = 4;
	constexpr auto ui_cx_resize_handle = 12;
	constexpr auto ui_scroll_width = 20;
	constexpr auto ui_base_icon_cxy = 18;

	constexpr uint32_t hidden_standby_timer_id = 0xD1FF;
	constexpr uint32_t hidden_standby_poll_ms = 250;

	class sdl_present_target final : public render::software_present_target
	{
		SDL_Window* _window = nullptr;
		bool _layered = false;
		std::vector<uint8_t> _pixels;
		core::sizei _extent;
		SDL_Surface* _surface = nullptr;
		std::vector<SDL_Rect> _rects;

	public:
		sdl_present_target(SDL_Window* window, const bool layered) : _window(window), _layered(layered)
		{
		}

		bool is_layered() const override
		{
			return _layered;
		}

		render::software_buffer acquire_buffer(const core::sizei extent) override
		{
			if (extent.cx < 1 || extent.cy < 1)
			{
				release_buffer();
				return {};
			}

			if (_extent != extent)
			{
				_pixels.assign(static_cast<size_t>(extent.cx) * extent.cy * 4, 0);
				_extent = extent;
			}

			return {_pixels.data(), _extent, _extent.cx * 4};
		}

		void release_buffer() override
		{
			_pixels.clear();
			_pixels.shrink_to_fit();
			_extent = {};
		}

		bool begin_present() override
		{
			_rects.clear();
			_surface = _window ? SDL_GetWindowSurface(_window) : nullptr;
			if (!_surface || _pixels.empty()) return false;
			if (SDL_MUSTLOCK(_surface)) SDL_LockSurface(_surface);
			return true;
		}

		void present_tile(const core::recti tile, const core::pointi buffer_origin) override
		{
			if (!_surface) return;

			const auto clipped = tile.intersection(core::recti(0, 0, _surface->w, _surface->h));
			if (clipped.is_empty()) return;

			const auto stride = _extent.cx * 4;
			const auto native = _surface->format == SDL_PIXELFORMAT_ARGB8888 ||
				_surface->format == SDL_PIXELFORMAT_XRGB8888;

			for (auto y = clipped.top; y < clipped.bottom; ++y)
			{
				const auto* const src = _pixels.data() + static_cast<ptrdiff_t>(y - buffer_origin.y) * stride +
					static_cast<ptrdiff_t>(clipped.left - buffer_origin.x) * 4;
				auto* const dst = static_cast<uint8_t*>(_surface->pixels) +
					static_cast<ptrdiff_t>(y) * _surface->pitch +
					static_cast<ptrdiff_t>(clipped.left) * SDL_BYTESPERPIXEL(_surface->format);

				if (_layered)
				{
					for (auto x = 0; x < clipped.width(); ++x)
					{
						const auto* const source = src + x * 4;
						auto* const destination = dst + x * 4;
						const auto alpha = source[3];
						destination[0] = static_cast<uint8_t>(source[0] * alpha / 255);
						destination[1] = static_cast<uint8_t>(source[1] * alpha / 255);
						destination[2] = static_cast<uint8_t>(source[2] * alpha / 255);
						destination[3] = alpha;
					}
				}
				else if (native)
				{
					memcpy(dst, src, static_cast<size_t>(clipped.width()) * 4);
				}
				else
				{
					SDL_ConvertPixels(clipped.width(), 1, SDL_PIXELFORMAT_ARGB8888, src, stride,
					                  _surface->format, dst, _surface->pitch);
				}
			}

			_rects.push_back({clipped.left, clipped.top, clipped.width(), clipped.height()});
		}

		void end_present(int) override
		{
			if (!_surface) return;
			if (SDL_MUSTLOCK(_surface)) SDL_UnlockSurface(_surface);
			if (!_rects.empty()) SDL_UpdateWindowSurfaceRects(_window, _rects.data(),
			                                                    static_cast<int>(_rects.size()));
			_surface = nullptr;
		}
	};
}

namespace render
{
	class window_device_linux final : public window_device
	{
		platform::window& _window;
		double _scale = 1.0;
		device_options _options;
		SDL_Window* _sdl = nullptr;
		software_present_target_ptr _target;
		draw_context_device_ptr _context;
		core::sizei _extent;
		bool _hidden_standby = false;

		int font_size_for_window() const
		{
			return core::round(_scale * static_cast<double>(render::base_font_size()));
		}

		void update_metrics()
		{
			if (!_context) return;

			const auto ui_scale = _scale * render::base_font_size() /
				static_cast<double>(render::default_font_size);
			_context->scale_factor = ui_scale;
			_context->icon_cxy = core::round(ui_base_icon_cxy * ui_scale);
			_context->padding2 = core::round(ui_component_snap * ui_scale);
			_context->padding1 = core::round(ui_baseline_snap * ui_scale);
			_context->handle_cxy = core::round(ui_cx_resize_handle * ui_scale);
			_context->scroll_width = core::round(ui_scroll_width * ui_scale);
		}

		void update_font_and_metrics()
		{
			if (_context) _context->update_font_size(font_size_for_window());
			update_metrics();
		}

		void create_context()
		{
			_target = std::make_shared<sdl_present_target>(_sdl, _options.layered);
			_context = render::create_software_draw_context(render::create_text_engine(font_size_for_window()),
			                                             _target, font_size_for_window());
			update_metrics();
		}

		bool can_be_seen()
		{
			const auto root = _window.root();
			return !(root && root->is_minimized()) && !_window.is_minimized() && !_window.is_occluded();
		}

		bool skip_hidden_frame()
		{
			if (can_be_seen())
			{
				end_hidden_standby();
				return false;
			}

			if (!_hidden_standby)
			{
				_window.start_timer(hidden_standby_timer_id, hidden_standby_poll_ms);
				_hidden_standby = true;
			}

			if (_hidden_standby) core::bump(render::ui_perf.hidden_skips);
			return _hidden_standby;
		}

		void end_hidden_standby()
		{
			if (!_hidden_standby) return;
			_hidden_standby = false;
			_window.stop_timer(hidden_standby_timer_id);
			_window.invalidate();
		}

		void prepare_device_loss()
		{
			if (_context)
			{
				_context->destroy();
				_context.reset();
			}
			_target.reset();
		}

		void rebuild_device()
		{
			create_context();
			if (_context && !_extent.is_empty()) _context->resize(_extent);
			update_font_and_metrics();
		}

	public:
		window_device_linux(platform::window& window, const device_options& options)
			: _window(window), _scale(window.scale()), _options(options), _sdl(std::any_cast<SDL_Window*>(window.native_handle())),
			  _extent(window.client_extent())
		{
			detail::register_window_device(
				this,
				[this] { prepare_device_loss(); },
				[this] { rebuild_device(); },
				[this] { update_font_and_metrics(); },
				[this] { _window.invalidate(); });
			create_context();
			if (_context && !_extent.is_empty()) _context->resize(_extent);
		}

		~window_device_linux() override
		{
			if (_hidden_standby) _window.stop_timer(hidden_standby_timer_id);
			detail::unregister_window_device(this);
			prepare_device_loss();
		}

		void paint(const core::recti damage, const std::function<void(draw_context_device&)>& draw) override
		{
			if (!_context)
			{
				_window.invalidate();
				return;
			}

			const auto was_hidden = _hidden_standby;
			if (skip_hidden_frame()) return;
			const auto actual_damage = was_hidden ? core::recti{} : damage;

			core::bump(render::ui_perf.paints);
			core::perf_timer timer(render::ui_perf.paint_us, &render::ui_perf.paint_max_us,
			                     &render::ui_perf.paint_latency);
			auto context = _context;
			context->begin_draw(_extent, font_size_for_window(), actual_damage);
			context->time_now = core::now();

			{
				core::perf_timer build(render::ui_perf.scene_build_us, &render::ui_perf.scene_build_max_us,
				                     &render::ui_perf.scene_build_latency);
				if (draw) draw(*context);
			}

			{
				core::perf_timer backend(render::ui_perf.backend_render_us,
				                     &render::ui_perf.backend_render_max_us,
				                     &render::ui_perf.backend_render_latency);
				context->render();
			}
		}

		void redraw() override
		{
			core::bump(render::ui_perf.redraws);
			if (!_context || !_context->is_valid())
			{
				_window.invalidate();
				return;
			}

			if (skip_hidden_frame()) return;
			_context->reset_damage();
			_context->render();
		}

		void resize(const core::sizei client, const bool minimized) override
		{
			if (_extent == client) return;
			_extent = client;
			if (_context && !minimized && !_extent.is_empty()) _context->resize(_extent);
			_window.invalidate();
		}

		void scale_changed(const double scale) override
		{
			_scale = scale;
			update_font_and_metrics();
			_window.invalidate();
		}

		bool timer(const uint32_t id) override
		{
			if (id != hidden_standby_timer_id) return false;
			if (can_be_seen()) end_hidden_standby();
			return true;
		}

		void set_layer_alpha(const int) override
		{
			// SDL's software surface preserves the bubble's per-pixel alpha; Linux's old layer did not
			// apply a separate fade alpha because animations are disabled on this backend.
		}

		draw_context_device& context() override
		{
			core::assert_true(_context != nullptr);
			return *_context;
		}
	};
}

namespace render::detail
{
	bool backend_startup(const bool)
	{
		// Every SDL window is presented by the CPU renderer, so per-frame fades are disabled.
		render::animations_enabled = false;
		return true;
	}

	bool backend_gpu_active()
	{
		return false;
	}

	void backend_shutdown()
	{
	}

	bool backend_downgrade()
	{
		return false;
	}
}

render::window_device_ptr render::create_window_device(platform::window& window,
                                                        const render::device_options& options)
{
	return std::make_shared<render::window_device_linux>(window, options);
}
