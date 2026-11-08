// This file is part of libdesktop, a C++20 platform layer for desktop applications
// Copyright 2026  Zac Walker
//
// This program is free software; you can redistribute it and / or modify it
// under the terms of the LGPL License either version 2.1 or later.
// License details are available at https://www.gnu.org/licenses/lgpl-2.1.html
// This program is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY

// Purpose: A Direct3D or software renderer bound to one platform::window.

#include "libdesktop/render.h"
#include "libdesktop/platform/win32.h"
#include "libdesktop/render/win32.h"

bool is_device_loss_error(const HRESULT hr)
{
	return hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET ||
		hr == DXGI_ERROR_DEVICE_HUNG || hr == DXGI_ERROR_DRIVER_INTERNAL_ERROR;
}

namespace render::detail
{
	bool backend_startup(bool use_gpu);
	void backend_shutdown();
	bool backend_downgrade();
	bool backend_gpu_active();
	factories_ptr backend_factories();

	void register_window_device(void* key, std::function<void()> prepare_loss,
	                            std::function<void()> rebuild_loss, std::function<void()> update_font,
	                            std::function<void()> invalidate);
	void unregister_window_device(void* key);
	void device_lost();
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

	core::sizei quantise_back_buffer_extent(const core::sizei extent)
	{
		const auto round_up = [](const int value)
		{
			return std::max(back_buffer_quantum,
			                (value + back_buffer_quantum - 1) / back_buffer_quantum * back_buffer_quantum);
		};

		return {round_up(extent.cx), round_up(extent.cy)};
	}
}

namespace render
{
	class window_device_win final : public window_device
	{
		platform::window& _window;
		double _scale = 1.0;
		device_options _options;
		factories_ptr _factories;
		draw_context_device_ptr _context;
		ComPtr<IDXGISwapChain> _swap_chain;

		core::sizei _extent;
		core::sizei _buffer_extent;
		bool _use_d3d = false;
		bool _device_loss_handled = false;
		bool _present_occluded = false;
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

		// Returns true when the requested D3D path could not make a context and the whole process
		// should switch to software, rather than only this window doing so temporarily.
		bool create_draw_context()
		{
			_context.reset();
			_swap_chain.Reset();
			_buffer_extent = {};

			bool d3d_failed = false;
			const auto hwnd = std::any_cast<HWND>(_window.native_handle());

			if (_use_d3d && _factories && _factories->d3d_device && _factories->dxgi && hwnd)
			{
				HRESULT hr = S_OK;
				ComPtr<IDXGIFactory2> factory2;

				hr = _factories->dxgi.As(&factory2);
				if (SUCCEEDED(hr))
				{
					DXGI_SWAP_CHAIN_DESC1 desc = {};
					desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
					desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT | DXGI_USAGE_BACK_BUFFER;
					desc.Scaling = DXGI_SCALING_NONE;
					desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
					desc.BufferCount = swap_buffer_count;
					desc.SampleDesc.Count = 1;
					desc.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
					_buffer_extent = quantise_back_buffer_extent(_extent);
					desc.Width = _buffer_extent.cx;
					desc.Height = _buffer_extent.cy;

					ComPtr<IDXGISwapChain1> swap_chain1;
					hr = factory2->CreateSwapChainForHwnd(_factories->d3d_device.Get(), hwnd, &desc, nullptr,
					                                             nullptr, &swap_chain1);
					if (SUCCEEDED(hr)) _swap_chain = swap_chain1;
				}

				if (SUCCEEDED(hr) && _swap_chain)
				{
					_factories->dxgi->MakeWindowAssociation(hwnd, DXGI_MWA_NO_WINDOW_CHANGES);
					_context = ::d3d11_create_context(_factories, _swap_chain, font_size_for_window());
				}

				if (!_context)
				{
					core::log(__FUNCTION__, "Direct3D draw context unavailable - falling back to software rendering");
					_swap_chain.Reset();
					d3d_failed = true;

					// A failed context did not fault the process, so keep the crash guard from blaming a
					// later unrelated shutdown for this deliberate software fallback.
					if (!platform::crash_guard_failed(platform::crash_guard::gpu_render))
					{
						platform::set_crash_guard(platform::crash_guard::gpu_render, false);
					}
				}
			}

			if (!_context)
			{
				_context = ::create_software_draw_context(_factories, hwnd, _options.layered,
				                                         font_size_for_window());
			}

			update_metrics();
			return d3d_failed;
		}

		bool can_be_seen()
		{
			const auto root = _window.root();
			if ((root && root->is_minimized()) || _window.is_minimized() || _window.is_occluded()) return false;

			if (_present_occluded)
			{
				if (_swap_chain && _swap_chain->Present(0, DXGI_PRESENT_TEST) == DXGI_STATUS_OCCLUDED) return false;
				_present_occluded = false;
			}

			return true;
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

		void poll_hidden_standby()
		{
			if (can_be_seen()) end_hidden_standby();
		}

		void end_hidden_standby()
		{
			if (!_hidden_standby) return;

			_hidden_standby = false;
			_window.stop_timer(hidden_standby_timer_id);
			_window.invalidate();
		}

		void handle_device_loss(const HRESULT hr, const std::string_view operation)
		{
			if (!::is_device_loss_error(hr) || _device_loss_handled) return;
			_device_loss_handled = true;

			ComPtr<ID3D11Device> device;
			const auto reason = _swap_chain && SUCCEEDED(_swap_chain->GetDevice(IID_PPV_ARGS(&device))) && device
				                    ? device->GetDeviceRemovedReason()
				                    : hr;
			core::log(__FUNCTION__, std::format(
				"{} failed with HRESULT 0x{:08x}; device reason 0x{:08x}. Switching to CPU software rendering.",
				operation, static_cast<uint32_t>(hr), static_cast<uint32_t>(reason)));

			platform::fail_crash_guard(platform::crash_guard::gpu_render);
			detail::device_lost();
		}

		void prepare_device_loss()
		{
			if (_context)
			{
				_context->destroy();
				_context.reset();
			}

			_swap_chain.Reset();
			_buffer_extent = {};
		}

		void rebuild_device()
		{
			_factories = detail::backend_factories();
			create_draw_context();
			if (_context && !_extent.is_empty()) _context->resize(_extent);
			update_font_and_metrics();
			_device_loss_handled = false;
			_present_occluded = false;
		}

		void present()
		{
			if (!_swap_chain) return;

			HRESULT hr;
			{
				core::perf_timer timer(render::gpu_perf.present_us, &render::gpu_perf.present_max_us,
				                     &render::gpu_perf.present_latency);
				hr = _swap_chain->Present(0, 0);
			}

			if (FAILED(hr))
			{
				handle_device_loss(hr, "Present");
				return;
			}

			if (hr == DXGI_STATUS_OCCLUDED) _present_occluded = true;

			static std::atomic_bool gpu_render_confirmed{false};
			if (!gpu_render_confirmed.exchange(true))
			{
				platform::set_crash_guard(platform::crash_guard::gpu_render, false);
			}
		}

	public:
		window_device_win(platform::window& window, const device_options& options)
			: _window(window), _scale(window.scale()), _options(options), _factories(detail::backend_factories()),
			  _extent(window.client_extent()), _use_d3d(options.gpu && !options.layered)
		{
			detail::register_window_device(
				this,
				[this] { prepare_device_loss(); },
				[this] { rebuild_device(); },
				[this] { update_font_and_metrics(); },
				[this] { _window.invalidate(); });

			if (create_draw_context() && render::gpu_active()) detail::device_lost();
			if (_context && !_extent.is_empty()) _context->resize(_extent);
		}

		~window_device_win() override
		{
			if (_hidden_standby) _window.stop_timer(hidden_standby_timer_id);
			detail::unregister_window_device(this);
			prepare_device_loss();
			_factories.reset();
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

			{
				core::perf_timer build(render::ui_perf.scene_build_us, &render::ui_perf.scene_build_max_us,
				                     &render::ui_perf.scene_build_latency);
				if (draw) draw(*context);
			}

			const auto presented = [&context]
			{
				core::perf_timer backend(render::ui_perf.backend_render_us,
				                         &render::ui_perf.backend_render_max_us,
				                         &render::ui_perf.backend_render_latency);
				return context->render();
			}();

			if (presented.failed)
			{
				handle_device_loss(presented.backend_code, "render");
				return;
			}

			present();
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
			const auto presented = _context->render();
			if (presented.failed)
			{
				handle_device_loss(presented.backend_code, "redraw");
				return;
			}

			present();
		}

		void resize(const core::sizei client, const bool minimized) override
		{
			const auto changed = _extent != client;
			if (!changed) return;

			_extent = client;
			if (_extent.is_empty() || minimized) return;

			if (_swap_chain)
			{
				const auto wanted = quantise_back_buffer_extent(_extent);
				const auto must_grow = _extent.cx > _buffer_extent.cx || _extent.cy > _buffer_extent.cy;
				const auto worth_shrinking = wanted.cx * 2 <= _buffer_extent.cx && wanted.cy * 2 <= _buffer_extent.cy;

				if (must_grow || worth_shrinking)
				{
					if (_context) _context->release_back_buffer_references();
					const auto hr = _swap_chain->ResizeBuffers(swap_buffer_count, wanted.cx, wanted.cy,
					                                           back_buffer_format, 0);
					if (FAILED(hr))
					{
						core::log(__FUNCTION__, std::format("ResizeBuffers failed 0x{:08x}",
					                                  static_cast<uint32_t>(hr)));
						handle_device_loss(hr, "ResizeBuffers");
					}
					else
					{
						_buffer_extent = wanted;
					}
				}
			}

			if (_context) _context->resize(_extent);
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
			poll_hidden_standby();
			return true;
		}

		void set_layer_alpha(const int alpha) override
		{
			if (_context) _context->set_layer_alpha(alpha);
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
	static factories_ptr process_factories;

	bool backend_startup(const bool use_gpu)
	{
		process_factories = std::make_shared<factories>();
		if (!process_factories->init(use_gpu))
		{
			process_factories->destroy();
			process_factories.reset();
			return false;
		}

		// Initialisation succeeding is distinct from choosing the GPU. WARP/software mode is a
		// valid renderer and must not make render::startup report failure.
		return true;
	}

	void backend_shutdown()
	{
		if (process_factories)
		{
			process_factories->destroy();
			process_factories.reset();
		}
	}

	bool backend_downgrade()
	{
		if (!process_factories || process_factories->software_mode) return false;
		process_factories->downgrade_to_software();
		return true;
	}

	bool backend_gpu_active()
	{
		return process_factories && !process_factories->software_mode && process_factories->d3d_device != nullptr;
	}

	factories_ptr backend_factories()
	{
		return process_factories;
	}
}

render::window_device_ptr render::create_window_device(platform::window& window,
                                                        const render::device_options& options)
{
	return std::make_shared<render::window_device_win>(window, options);
}
