// This file is part of libdesktop, a C++20 platform layer for desktop applications
// Copyright 2026  Zac Walker
// 
// This program is free software; you can redistribute it and / or modify it
// under the terms of the LGPL License either version 2.1 or later.
// License details are available at https://www.gnu.org/licenses/lgpl-2.1.html
// This program is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY

// Purpose: The Windows backend's windows and rendering: Direct3D 11 and software draw contexts, GPU
// adapters and device loss, window placement and the native controls' probes. platform_win.h holds the
// operating-system services they build on.

#pragma once

#include "libdesktop/platform_win.h"

struct factories;
using factories_ptr = std::shared_ptr<factories>;

constexpr int max_vert_count = 4 * 512;
constexpr int max_index_count = 6 * 512;
constexpr int max_text_len = 2000;

// Colour every render target is cleared to before the scene is drawn. Both the Direct3D and the
// software backend must use this, otherwise anything the scene does not explicitly paint shows
// through as black on one backend and as this grey on the other.
constexpr float scene_clear_shade = 0.222f;

bool is_device_loss_error(HRESULT hr);

// The Microsoft Basic Render Driver: WARP presented as a hardware adapter, which is what Windows
// offers when no GPU driver is working. Drawn through, every frame is a full-window CPU redraw.
bool is_software_adapter(uint32_t vendor_id, uint32_t device_id, uint32_t flags);

// Whether a device answering this CheckFormatSupport mask can draw a texture of the format, which for
// NV12 and P010 means sampling views of their planes - holding the format is not enough.
bool can_sample_texture_format(UINT support);

// The memory a GPU allocates textures from. An integrated part shares system memory, and the
// "dedicated" figure it reports is only a carve-out made at boot (128MB on Intel parts).
uint64_t gpu_texture_memory(uint64_t dedicated, uint64_t shared, bool unified_memory);

// The bytes one displayed texture may cost, given that memory (0 when there is no GPU to ask) and
// the machine's physical memory (0 when unknown).
int64_t calc_texture_budget(uint64_t gpu_bytes, int64_t total_phys);

// The adapter the renderer draws with, which a hardware decode device is made on so the two can share
// pictures. Published once the render device exists; a zero LUID while there is none, and on the CPU
// backend.
void publish_render_adapter(LUID luid);
LUID render_adapter();

// The adapter with this LUID as Windows lists them now, or null once it is gone - or for a zero LUID.
ComPtr<IDXGIAdapter1> find_adapter_by_luid(LUID luid);

// Private window message posted by a frame when a Direct3D device-loss result is seen, or when a
// hardware window cannot build a Direct3D context at all. It is posted (never sent) so recovery runs
// after the render or resize call stack has unwound and no draw-context or device object is still
// live on the stack.
constexpr UINT WM_RENDER_DEVICE_LOST = WM_APP + 0x3d1;

// Switches the whole process to CPU software rendering once the Direct3D device is lost or cannot
// present, then asks the app to release GPU resources and rebuild every frame's draw context.
void handle_graphics_device_lost(const factories_ptr& f);

namespace platform
{
	// Brings a saved window rect back onto a display. Size is preserved where it fits and clamped to
	// the work area where it does not; the position is nudged inside.
	recti fit_window_to_work_area(recti saved, recti work_area);
	recti placement_to_screen(recti saved, recti monitor, recti work_area);
	recti screen_to_placement(recti screen, recti monitor, recti work_area);
	ui::key_state wheel_key_state(uint32_t message, WPARAM parameter, ui::key_state keyboard);
	bool probe_d3d11_pixel_snapping();

	// True when a saved rect cannot be restored as it stands: it reaches no display at all, it is
	// larger than the work area it would land in, or so little of it overlaps that there is nothing
	// left to grab. A window deliberately straddling two displays is none of those and is left alone.
	bool window_needs_refit(recti saved, recti work_area, bool reaches_a_display);

	struct edit_spelling_range
	{
		std::string word;
		int pos_start = 0;
		int pos_end = 0;
	};

	std::vector<edit_spelling_range> probe_edit_spelling_ranges(
		const std::wstring& text, const std::function<bool(std::string_view)>& is_word_valid);
	// What an edit control with this dictionary underlines in text.
	std::vector<edit_spelling_range> probe_edit_unknown_words(
		const std::wstring& text, const ui::spelling_dictionary* dictionary);
	recti probe_edit_spelling_bounds(HWND hwnd, int pos_start, int pos_end);

	enum class edit_context_menu_route
	{
		native_edit_procedure,
		custom_spelling_menu,
	};

	edit_context_menu_route probe_edit_context_menu_route(bool spelling_error_under_pointer, bool custom_menu_created);

	// Windows 11 offers snap layouts over a window's maximize button, and only over a button the
	// window reports as one. Only a resizable frame with a maximize box has one to report: Fullscreen
	// strips the resizing frame, and dialogs have no maximize box.
	bool can_present_maximize_caption_button(df::os_release release, uint32_t root_style);

	// Whether a toolbar reports its maximize button as that caption button now. Not while the frame is
	// maximized: Windows then opens the flyout at a standard caption's depth, over the middle of the
	// taller restore button the top bar draws, and the flyout takes the click meant for the button.
	bool presents_maximize_caption_button(df::os_release release, uint32_t root_style);

	// The hover and press of a toolbar button reported as the maximize caption button. Windows sends
	// that button non-client mouse messages, which the toolbar control never tracks. Ids are toolbar
	// command ids; zero is none.
	struct caption_button_tracker
	{
		int hover_id = 0;
		int pressed_id = 0;

		// Each answers whether what the toolbar draws changed.
		bool hover(int id);
		bool press(int id);
		bool leave();

		// The id to invoke: a release over the button its press began on, otherwise zero.
		int release(int id);
	};

	struct caption_button_probe
	{
		bool offered = false;
		int maximize_id = 0;
		int maximize_hit = HTNOWHERE;
		int other_hit = HTNOWHERE;
		int maximized_hit = HTNOWHERE;
		int frameless_hit = HTNOWHERE;
		int hovered_id = 0;
		int hover_after_leave = 0;
		int invocations = 0;
	};

	// Builds a toolbar holding an ordinary button and a maximize button in a hidden frame, and sends
	// it the messages Windows sends while the pointer rests on, presses and leaves the maximize button.
	caption_button_probe probe_toolbar_caption_button();
}

struct char_pos_width
{
	float x = 0.0f;
	float y = 0.0f;
	float cx = 0.0f;
};

struct calc_text_extent_result
{
	std::vector<char_pos_width> pos;
	int cy = 0;
	int cx = 0;
};

struct text_line
{
	int begin = 0;
	int end = 0;
	int pixel_width = 0;
};

// The backend interface is ui's, not Windows'. Imported here so the Windows layer can keep
// spelling it unqualified, which is how it reads in every backend and in the window layer.
using ui::draw_context_device;
using ui::draw_context_device_ptr;

draw_context_device_ptr d3d11_create_context(const factories_ptr& f, const ComPtr<IDXGISwapChain>& swap_chain,
                                             int base_font_size);
// A texture on the factories' Direct3D device, as every hardware draw context makes them.
ui::texture_ptr d3d11_create_texture(const factories_ptr& f);
draw_context_device_ptr create_software_draw_context(const factories_ptr& f, HWND hwnd, bool layered,
                                                     int base_font_size);
