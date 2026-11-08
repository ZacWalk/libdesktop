// This file is part of libdesktop, a C++20 platform layer for desktop applications
// Copyright 2026  Zac Walker
//
// This program is free software; you can redistribute it and / or modify it
// under the terms of the LGPL License either version 2.1 or later.
// License details are available at https://www.gnu.org/licenses/lgpl-2.1.html
// This program is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY

// Purpose: Drawing on a window's surface. A window device presents what is drawn to a
// platform::window - a swap chain on Direct3D, a present target on the software renderer - follows
// its size and scale, waits out the time nothing drawn could be seen, and survives a lost graphics
// device by drawing with the software renderer from then on.

#pragma once

namespace render
{
	// Chooses the renderer for the process, once, before the first device: Direct3D 11 where gpu is
	// asked for and works, otherwise the software renderer. Later calls change nothing.
	bool startup(bool gpu);
	void shutdown();
	// False after startup(false), when Direct3D failed, and for the rest of the session after a loss.
	bool gpu_active();

	// The text size everything is scaled from, in pixels at 96 DPI; default_font_size until set.
	// Every device follows a change, and its windows are repainted.
	void set_base_font_size(int size);
	int base_font_size();

	struct device_options
	{
		// Direct3D where the renderer has it; dialogs and popups draw with the software renderer.
		bool gpu = false;
		// Per-pixel alpha, presented as a layered window: a bubble.
		bool layered = false;
	};

	class window_device
	{
	public:
		virtual ~window_device() = default;

		// Draws a frame with draw, between begin_draw and the present, unless nothing drawn now could
		// be seen: then the device skips it and repaints the whole window once it can. Damage is in
		// client pixels; empty is the whole window, and pixels outside it must not depend on it.
		virtual void paint(core::recti damage, const std::function<void(draw_context_device&)>& draw) = 0;
		// Presents the last scene again without drawing it: its vertices or textures changed.
		virtual void redraw() = 0;
		virtual void resize(core::sizei client, bool minimized) = 0;
		virtual void scale_changed(double scale) = 0;
		// A timer the device started on its window - its wait for the window to be seen again. True
		// when the id was the device's.
		virtual bool timer(uint32_t id) = 0;
		// Sets the process-wide alpha of a layered presentation. Non-layered devices ignore it.
		virtual void set_layer_alpha(int alpha) = 0;

		// The context layout measures with, and painting draws with; its metrics follow the scale.
		virtual draw_context_device& context() = 0;
	};

	using window_device_ptr = std::shared_ptr<window_device>;

	window_device_ptr create_window_device(platform::window& window, const device_options& options);
}
