// This file is part of libdesktop, a C++20 platform layer for desktop applications
// Copyright 2026  Zac Walker
// 
// This program is free software; you can redistribute it and / or modify it
// under the terms of the LGPL License either version 2.1 or later.
// License details are available at https://www.gnu.org/licenses/lgpl-2.1.html
// This program is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY

// Purpose: libdesktop-render's Windows backend tests: windows and their placement, the Direct3D 11
// and software renderers, textures and video frames, fonts and text, the native controls, the
// clipboard's formats and the WASAPI mix format. They began in Diffractor's test_platform_win.cpp,
// which keeps its own copies, and use its assertions (system_test.h), so a test can move unchanged.

#include "libdesktop/render.h"
#include "libdesktop/platform_win.h"
#include "libdesktop/platform_win_render.h"
#include "libdesktop/platform_win_visual.h"
#include "libdesktop/platform_win_render_res.h"
#include "system_test.h"

#include <mmreg.h>

// Undocking or unplugging a second display leaves a saved rect that intersects no monitor at all.
static void should_restore_a_window_onto_a_display()
{
	constexpr recti work(0, 0, 1920, 1040);

	const auto from_a_missing_display = platform::fit_window_to_work_area({2200, 300, 3000, 900}, work);
	assert_equal(800, from_a_missing_display.width(), "width is preserved");
	assert_equal(600, from_a_missing_display.height(), "height is preserved");
	assert_equal(true, work.intersects(from_a_missing_display), "and the window lands on the display");
	assert_equal(1120, from_a_missing_display.left, "pushed just inside the right edge");
	assert_equal(300, from_a_missing_display.top, "the axis that already fitted is left alone");

	const auto from_the_left = platform::fit_window_to_work_area({-3000, -400, -2600, -100}, work);
	assert_equal(0, from_the_left.left, "a negative origin comes back to the work area");
	assert_equal(0, from_the_left.top, "on both axes");

	const auto too_large = platform::fit_window_to_work_area({4000, 0, 8000, 3000}, work);
	assert_equal(1920, too_large.width(), "a window larger than the display is clamped to it");
	assert_equal(1040, too_large.height(), "on both axes");
	assert_equal(0, too_large.left, "and positioned at the origin rather than hanging off");

	// Which saved rects are corrected at all. Reaching a display is not on its own enough to be usable.
	assert_equal(true, platform::window_needs_refit({2200, 300, 3000, 900}, work, false),
	             "a rect saved on a display that is gone");
	assert_equal(false, platform::window_needs_refit({100, 100, 900, 700}, work, true),
	             "an ordinary rect is restored exactly as it was saved");
	assert_equal(false, platform::window_needs_refit({1500, 100, 2400, 700}, work, true),
	             "and so is one deliberately straddling two displays");
	assert_equal(true, platform::window_needs_refit({0, 0, 3840, 2160}, work, true),
	             "a window saved on a larger display is clamped to this one");
	assert_equal(true, platform::window_needs_refit({1900, 100, 2700, 700}, work, true),
	             "a sliver at the edge leaves nothing to grab");
	assert_equal(true, platform::window_needs_refit({100, 1030, 900, 1630}, work, true),
	             "and neither has one pushed off the bottom");
}

static void should_classify_dxgi_device_loss()
{
	assert_equal(true, is_device_loss_error(DXGI_ERROR_DEVICE_REMOVED), "device removed");
	assert_equal(true, is_device_loss_error(DXGI_ERROR_DEVICE_RESET), "device reset");
	assert_equal(true, is_device_loss_error(DXGI_ERROR_DEVICE_HUNG), "device hung");
	assert_equal(true, is_device_loss_error(DXGI_ERROR_DRIVER_INTERNAL_ERROR), "driver internal error");
	assert_equal(false, is_device_loss_error(DXGI_STATUS_OCCLUDED), "occlusion is not device loss");
	assert_equal(false, is_device_loss_error(E_FAIL), "generic failure is not device loss");
	assert_equal(false, is_device_loss_error(S_OK), "success is not device loss");
}

static void should_use_screen_coordinates_for_saved_window_placement()
{
	const recti placement(100, 100, 900, 700);
	const auto top_taskbar = platform::placement_to_screen(placement, {0, 0, 1920, 1080}, {0, 40, 1920, 1080});
	assert_equal(140, top_taskbar.top, "workspace origin is below a top taskbar");
	assert_equal(800, top_taskbar.width(), "conversion does not resize the window");
	const auto left_taskbar = platform::placement_to_screen(placement, {0, 0, 1920, 1080}, {48, 0, 1920, 1080});
	assert_equal(148, left_taskbar.left, "workspace origin is beside a left taskbar");
	const auto secondary = platform::placement_to_screen({-1800, 100, -1000, 700},
	                                                    {-1920, 0, 0, 1080}, {-1920, 40, 0, 1080});
	assert_equal(-1800, secondary.left, "secondary-monitor screen origin is retained");
	assert_equal(140, secondary.top, "secondary-monitor work offset is applied once");

	// Full screen goes the other way: monitor bounds in screen coordinates become a placement.
	const auto full_screen = platform::screen_to_placement({0, 0, 1920, 1080}, {0, 0, 1920, 1080}, {48, 0, 1920, 1080});
	assert_equal(-48, full_screen.left, "monitor bounds start left of a left taskbar's workspace origin");
	assert_equal(1920, full_screen.width(), "conversion does not resize a full-screen window");
	const auto round_trip = platform::screen_to_placement(secondary, {-1920, 0, 0, 1080}, {-1920, 40, 0, 1080});
	assert_equal(100, round_trip.top, "screen to placement inverts placement to screen");
	assert_equal(-1800, round_trip.left, "on both axes");
}

static void should_not_treat_pointer_wheel_ids_as_modifiers()
{
	for (const auto message : {0x024eu, 0x024fu})
	{
		const auto none = platform::wheel_key_state(message, MAKEWPARAM(12, WHEEL_DELTA), {});
		assert_equal(false, none.control || none.shift, "pointer ID bits are not modifier flags");
		const auto held = platform::wheel_key_state(message, MAKEWPARAM(1, WHEEL_DELTA), {true, true, false});
		assert_equal(true, held.control && held.shift, "pointer wheels use keyboard state");
	}
	const auto mouse = platform::wheel_key_state(WM_MOUSEWHEEL, MAKEWPARAM(MK_CONTROL, WHEEL_DELTA), {});
	assert_equal(true, mouse.control, "mouse wheel flags retain their existing meaning");
	assert_equal(false, mouse.shift, "unpressed mouse modifiers stay clear");
}

// A machine with no working GPU driver is handed WARP posing as a hardware adapter. Drawn through, it
// redrew the whole window on the CPU every frame, so it is recognised and the CPU backend used instead.
static void should_recognise_the_basic_render_driver()
{
	assert_equal(true, is_software_adapter(0x1414, 0x8c, 0), "the Microsoft Basic Render Driver");
	assert_equal(true, is_software_adapter(0x10de, 0x2684, DXGI_ADAPTER_FLAG_SOFTWARE),
	             "any adapter that says it is software");
	assert_equal(false, is_software_adapter(0x8086, 0x3ea0, 0), "an Intel integrated GPU");
	assert_equal(false, is_software_adapter(0x10de, 0x2684, 0), "an NVIDIA card");
	assert_equal(false, is_software_adapter(0x1414, 0x5353, 0), "the vendor alone does not make an adapter WARP");
}

// NV12 and P010 are drawn by sampling views of their planes. A device that could only hold the format
// accepted the texture, refused its views, and the picture never appeared.
static void should_need_sampling_for_planar_video()
{
	assert_equal(false, can_sample_texture_format(D3D11_FORMAT_SUPPORT_TEXTURE2D), "holding the format is not enough");
	assert_equal(false, can_sample_texture_format(D3D11_FORMAT_SUPPORT_SHADER_SAMPLE),
	             "nor is sampling a format that cannot be a 2D texture");
	assert_equal(false, can_sample_texture_format(0), "nothing supported");
	assert_equal(true, can_sample_texture_format(D3D11_FORMAT_SUPPORT_TEXTURE2D | D3D11_FORMAT_SUPPORT_SHADER_SAMPLE |
		             D3D11_FORMAT_SUPPORT_DECODER_OUTPUT), "a device that samples it");
}

// Intel parts report the 128MB carved out at boot as dedicated memory, and allocate textures from shared
// memory. Read as the card's own, it held every Intel laptop at the 64MB floor, and a photo over 16
// megapixels was shown downscaled even at 100%.
static void should_count_shared_memory_for_an_integrated_gpu()
{
	constexpr uint64_t mb = 1024ull * 1024ull;
	constexpr uint64_t gb = 1024ull * mb;
	constexpr auto laptop = static_cast<int64_t>(16 * gb);
	const auto budget = [](const uint64_t gpu_bytes, const int64_t total_phys)
	{
		return static_cast<uint64_t>(calc_texture_budget(gpu_bytes, total_phys));
	};

	assert_equal(128 * mb + 8 * gb, gpu_texture_memory(128 * mb, 8 * gb, true), "an integrated part uses shared memory");
	assert_equal(8 * gb, gpu_texture_memory(8 * gb, 16 * gb, false), "a discrete card uses its own");
	assert_equal(8 * gb, gpu_texture_memory(0, 8 * gb, false), "an adapter with none of its own still has somewhere");

	assert_equal(128 * mb, budget(gpu_texture_memory(128 * mb, 8 * gb, true), laptop),
	             "an integrated laptop GPU is given the full ceiling");
	assert_equal(64 * mb, budget(128 * mb, laptop), "which the carve-out alone held at the floor");
	assert_equal(64 * mb, budget(512 * mb, laptop), "a small discrete card is still held back");
	assert_equal(128 * mb, budget(8 * gb, laptop), "a large card is held to the ceiling");
	assert_equal(96 * mb, budget(gpu_texture_memory(128 * mb, 768 * mb, true), static_cast<int64_t>(1536 * mb)),
	             "physical memory still bounds a small machine");
	assert_equal(128 * mb, budget(0, 0), "with nothing to ask, the shipped ceiling");
}

// Defined in platform_win_sound.cpp: what the device's format() answers for its mix format.
platform::audio_output_format wasapi_mix_format(const WAVEFORMATEX* format);

// An endpoint's mix format is a WAVEFORMATEX, extended or not, which the device describes as a sample
// format and the channels with the speakers they feed. An extension too short to hold its speaker mask
// names neither its samples nor its speakers.
static void should_describe_a_wasapi_mix_format()
{
	constexpr GUID ieee_float = {0x00000003, 0x0000, 0x0010, {0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71}};
	constexpr GUID pcm = {0x00000001, 0x0000, 0x0010, {0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71}};

	WAVEFORMATEXTENSIBLE mix = {};
	mix.Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
	mix.Format.nChannels = 6;
	mix.Format.nSamplesPerSec = 48000;
	mix.Format.wBitsPerSample = 32;
	mix.Format.nBlockAlign = 24;
	mix.Format.nAvgBytesPerSec = 48000 * 24;
	mix.Format.cbSize = sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX);
	mix.Samples.wValidBitsPerSample = 32;
	mix.dwChannelMask = 0x3F; // 5.1: front left, right and centre, LFE, back left and right
	mix.SubFormat = ieee_float;

	const auto described = wasapi_mix_format(&mix.Format);
	assert_equal(48000u, described.sample_rate, "the endpoint's rate");
	assert_equal(true, described.sample_format == platform::audio_sample_format::float32, "its float samples");
	assert_equal(6u, described.channels, "its channels");
	assert_equal(true, described.channel_mask == 0x3F, "and the speakers they feed");

	mix.SubFormat = pcm;
	assert_equal(true, wasapi_mix_format(&mix.Format).sample_format == platform::audio_sample_format::int32,
	             "32-bit integer samples");

	mix.Format.cbSize = 0;
	const auto short_extension = wasapi_mix_format(&mix.Format);
	assert_equal(true, short_extension.sample_format == platform::audio_sample_format::none,
	             "an extension too short for its mask names no samples");
	assert_equal(true, short_extension.channel_mask == 0, "and no speakers");

	WAVEFORMATEX stereo = {};
	stereo.wFormatTag = WAVE_FORMAT_PCM;
	stereo.nChannels = 2;
	stereo.nSamplesPerSec = 44100;
	stereo.wBitsPerSample = 16;
	stereo.nBlockAlign = 4;
	stereo.nAvgBytesPerSec = 44100 * 4;
	const auto plain = wasapi_mix_format(&stereo);
	assert_equal(true, plain.sample_format == platform::audio_sample_format::int16, "a plain format's 16-bit samples");
	assert_equal(2u, plain.channels, "on its channels");
	assert_equal(true, plain.channel_mask == 0, "names no speakers");

	stereo.wBitsPerSample = 24;
	stereo.nBlockAlign = 6;
	assert_equal(true, wasapi_mix_format(&stereo).sample_format == platform::audio_sample_format::none,
	             "packed 24-bit samples are not spoken");
	assert_equal(0u, wasapi_mix_format(nullptr).channels, "and no format describes no channels");
}

// WARP is in every Windows since 8 and needs no GPU, so the texture tests run the same everywhere.
static ComPtr<ID3D11Device> create_warp_device(ComPtr<ID3D11DeviceContext>& context)
{
	ComPtr<ID3D11Device> device;

	if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr,
	                             0, D3D11_SDK_VERSION, &device, nullptr, &context)))
	{
		return {};
	}

	return device;
}

static ComPtr<ID3D11Texture2D> create_plain_texture(ID3D11Device* device, const sizei dims)
{
	D3D11_TEXTURE2D_DESC desc = {};
	desc.Width = dims.cx;
	desc.Height = dims.cy;
	desc.MipLevels = 1;
	desc.ArraySize = 1;
	desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
	desc.SampleDesc.Count = 1;
	desc.Usage = D3D11_USAGE_DEFAULT;
	desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;

	ComPtr<ID3D11Texture2D> texture;
	device->CreateTexture2D(&desc, nullptr, &texture);
	return texture;
}

// Thumbnails come and go by the hundred as a listing scrolls, and each was a CreateTexture2D on the UI
// thread. A released texture is handed to the next one of its exact size and format - but never while
// a scene a redraw can replay still draws it, or that redraw would show the new picture in its place.
static void should_recycle_a_texture_only_when_nothing_draws_it()
{
	ComPtr<ID3D11DeviceContext> context;
	const auto device = create_warp_device(context);
	assert_equal(true, device != nullptr, "a WARP device");
	if (!device) return;

	constexpr sizei thumb = {64, 48};
	constexpr auto argb = ui::texture_format::ARGB;
	const auto thumb_bytes = d3d11_texture_pool::texture_bytes(thumb, argb);
	d3d11_texture_pool pool(thumb_bytes * 8);

	const auto released = create_plain_texture(device.Get(), thumb);
	pool.give({thumb, argb, released, {}});
	assert_equal(true, pool.take({64, 50}, argb).texture == nullptr, "a different size is not handed out");
	assert_equal(true, pool.take(thumb, ui::texture_format::RGB).texture == nullptr, "nor a different format");
	assert_equal(true, pool.take(thumb, argb).texture == released, "the same size and format is");
	assert_equal(0_z, pool.size(), "and leaves the pool");

	auto drawn = std::make_shared<const texture_views>();
	pool.give({thumb, argb, released, drawn});
	assert_equal(true, pool.take(thumb, argb).texture == nullptr, "not while a scene still draws it");
	drawn.reset();
	assert_equal(true, pool.take(thumb, argb).texture == released, "but once nothing does");

	std::vector<ComPtr<ID3D11Texture2D>> made;

	for (auto i = 0; i < 9; ++i)
	{
		made.emplace_back(create_plain_texture(device.Get(), thumb));
		pool.give({thumb, argb, made.back(), {}});
	}

	assert_equal(8_z, pool.size(), "the budget holds eight");
	assert_equal(true, pool.bytes() <= thumb_bytes * 8, "and never more than it");
	assert_equal(true, pool.take(thumb, argb).texture == made[1], "the oldest went to make room");

	const auto photograph = create_plain_texture(device.Get(), {128, 128});
	const auto before = pool.size();
	pool.give({{128, 128}, argb, photograph, {}});
	assert_equal(before, pool.size(), "a texture over an eighth of the budget is not kept");

	pool.clear();
	assert_equal(0_z, pool.size(), "a cleared pool holds nothing");
	assert_equal(0_z, pool.bytes(), "and costs nothing");
}

static bool texture_holds(ID3D11Device* device, ID3D11DeviceContext* context, ID3D11Texture2D* texture,
                          const uint32_t bgra)
{
	D3D11_TEXTURE2D_DESC desc = {};
	texture->GetDesc(&desc);
	desc.Usage = D3D11_USAGE_STAGING;
	desc.BindFlags = 0;
	desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;

	ComPtr<ID3D11Texture2D> staging;
	if (FAILED(device->CreateTexture2D(&desc, nullptr, &staging))) return false;

	context->CopyResource(staging.Get(), texture);

	D3D11_MAPPED_SUBRESOURCE mapped = {};
	if (FAILED(context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped))) return false;

	auto holds = true;

	for (UINT y = 0; y < desc.Height && holds; ++y)
	{
		const auto* const row = reinterpret_cast<const uint32_t*>(static_cast<const uint8_t*>(mapped.pData) +
			static_cast<size_t>(mapped.RowPitch) * y);
		holds = std::all_of(row, row + desc.Width, [bgra](const uint32_t px) { return px == bgra; });
	}

	context->Unmap(staging.Get(), 0);
	return holds;
}

// A recycled texture still holds the last picture until it is overwritten, so the reuse is only right if
// the new picture lands in it in full. Read back from the texture the second picture was given, which is
// the one the first picture released.
static void should_overwrite_a_recycled_texture_with_the_new_picture()
{
	ComPtr<ID3D11DeviceContext> context;
	const auto device = create_warp_device(context);
	assert_equal(true, device != nullptr, "a WARP device");
	if (!device) return;

	const auto f = std::make_shared<factories>();
	f->d3d_device = device;
	f->d3d_context = context;
	f->texture_pool = std::make_unique<d3d11_texture_pool>(1024 * 1024);

	constexpr sizei dims = {64, 48};
	constexpr auto stride = static_cast<size_t>(dims.cx) * 4;
	constexpr auto bytes = stride * dims.cy;
	constexpr uint32_t red = 0xFFFF0000u;
	constexpr uint32_t green = 0xFF00FF00u;
	const std::vector<uint32_t> first_picture(static_cast<size_t>(dims.cx) * dims.cy, red);
	const std::vector<uint32_t> second_picture(static_cast<size_t>(dims.cx) * dims.cy, green);
	const auto created_before = df::gpu_perf.textures_created.load();
	const auto reused_before = df::gpu_perf.textures_reused.load();

	{
		const auto first = d3d11_create_texture(f);
		assert_equal(true, first->update(dims, ui::texture_format::ARGB, ui::orientation::top_left,
		                                 reinterpret_cast<const uint8_t*>(first_picture.data()), stride, bytes) ==
		             ui::texture_update_result::tex_created, "the first picture makes a texture");
	}

	assert_equal(1_z, f->texture_pool->size(), "which is kept when it is released");
	auto kept = f->texture_pool->take(dims, ui::texture_format::ARGB);
	const auto first_texture = kept.texture;
	f->texture_pool->give(std::move(kept));

	{
		const auto second = d3d11_create_texture(f);
		assert_equal(true, second->update(dims, ui::texture_format::ARGB, ui::orientation::top_left,
		                                  reinterpret_cast<const uint8_t*>(second_picture.data()), stride, bytes) ==
		             ui::texture_update_result::tex_created, "a texture taken from the pool is new to its caller");
		assert_equal(0_z, f->texture_pool->size(), "the second picture took it");
	}

	assert_equal(1ull, df::gpu_perf.textures_created.load() - created_before, "one texture was made");
	assert_equal(1ull, df::gpu_perf.textures_reused.load() - reused_before, "and one reused");

	const auto returned = f->texture_pool->take(dims, ui::texture_format::ARGB);
	assert_equal(true, returned.texture == first_texture, "the second picture was drawn into the first texture");
	assert_equal(true, returned.texture && texture_holds(device.Get(), context.Get(), returned.texture.Get(), green),
	             "and holds the new picture, not the old one");
}

// A decoded frame as the renderers see it, described by the test rather than by a decoder.
class fake_video_frame final : public ui::video_frame
{
public:
	ui::const_surface_ptr prepared;
	ui::video_picture described;
	mutable int cpu_requests = 0;

	ui::const_surface_ptr prepared_surface() const override { return prepared; }
	ui::video_picture picture() const override { return described; }
	void request_cpu_frames() const override { ++cpu_requests; }
};

// Converts any frame into a 32 x 16 picture, and counts the frames it was asked for.
class fake_frame_scaler final : public ui::surface_scaler
{
public:
	std::shared_ptr<int> frames;

	bool scale_surface(const ui::const_surface_ptr&, ui::surface_ptr&, sizei, bool) override { return false; }
	bool convert_yuv_surface(const ui::surface&, const ui::surface_ptr&) override { return false; }

	bool scale_surface(const ui::video_frame&, const ui::surface_ptr& surface_out) override
	{
		++*frames;
		return surface_out->alloc(32, 16, ui::texture_format::RGB) != nullptr;
	}
};

static std::function<std::unique_ptr<ui::surface_scaler>()> make_fake_frame_scaler(std::shared_ptr<int> frames)
{
	return [frames]() -> std::unique_ptr<ui::surface_scaler>
	{
		auto s = std::make_unique<fake_frame_scaler>();
		s->frames = frames;
		return s;
	};
}

static std::shared_ptr<fake_video_frame> make_prepared_video_frame(const sizei dimensions)
{
	auto frame = std::make_shared<fake_video_frame>();
	const auto picture = std::make_shared<ui::surface>();
	picture->alloc(dimensions, ui::texture_format::RGB);
	frame->prepared = picture;
	return frame;
}

// A texture presents a video frame through the frame's interface alone: the picture the decode thread
// prepared when there is one, and otherwise one the application's scaler converts. A hardware picture
// is copied only under its decode device's lock, so one that brings no lock is refused, and with no
// scaler there is nothing to fall back to.
static void should_present_a_video_frame_through_its_interface()
{
	ComPtr<ID3D11DeviceContext> context;
	const auto device = create_warp_device(context);
	assert_equal(true, device != nullptr, "a WARP device");
	if (!device) return;

	const auto f = std::make_shared<factories>();
	f->d3d_device = device;
	f->d3d_context = context;
	f->texture_pool = std::make_unique<d3d11_texture_pool>(1024 * 1024);

	const auto frames_converted = std::make_shared<int>(0);
	const auto restore_scaler = ui::make_surface_scaler;
	const df::scope_exit restore([restore_scaler] { ui::make_surface_scaler = restore_scaler; });
	ui::make_surface_scaler = make_fake_frame_scaler(frames_converted);

	const auto texture = d3d11_create_texture(f);
	assert_equal(true, texture->update(make_prepared_video_frame({64, 48})) == ui::texture_update_result::tex_created,
	             "a prepared picture is uploaded");
	assert_equal(true, texture->dimensions() == sizei{64, 48}, "as it was prepared");
	assert_equal(0, *frames_converted, "with nothing converted");

	const auto unprepared = std::make_shared<fake_video_frame>();
	assert_equal(true, texture->update(unprepared) != ui::texture_update_result::failed,
	             "a picture in system memory is converted by the scaler");
	assert_equal(true, texture->dimensions() == sizei{32, 16}, "and uploaded as converted");
	assert_equal(1, *frames_converted, "once");
	assert_equal(0, unprepared->cpu_requests, "which asks the decoder for nothing");

	const auto unlocked = std::make_shared<fake_video_frame>();
	auto texture_name = 0;
	unlocked->described.texture = &texture_name;
	assert_equal(true, d3d11_create_texture(f)->update(unlocked) == ui::texture_update_result::failed,
	             "a hardware picture without its device's lock is refused");
	assert_equal(1, *frames_converted, "rather than converted");

	ui::make_surface_scaler = nullptr;
	assert_equal(true, d3d11_create_texture(f)->update(unprepared) == ui::texture_update_result::failed,
	             "and with no scaler a picture in system memory cannot be presented");
}

// The software renderer never samples the decoder's surfaces, so a hardware picture is converted by
// the scaler like any other, and the decode thread is asked to deliver the rest of the clip in system
// memory instead.
static void should_present_a_video_frame_in_software_through_its_interface()
{
	const auto frames_converted = std::make_shared<int>(0);
	const auto restore_scaler = ui::make_surface_scaler;
	const df::scope_exit restore([restore_scaler] { ui::make_surface_scaler = restore_scaler; });
	ui::make_surface_scaler = make_fake_frame_scaler(frames_converted);

	const auto renderer = create_software_draw_context(nullptr, nullptr, false, normal_font_size);
	const auto texture = renderer->create_texture();
	assert_equal(true, texture->update(make_prepared_video_frame({64, 48})) == ui::texture_update_result::tex_created,
	             "a prepared picture is copied");
	assert_equal(true, texture->dimensions() == sizei{64, 48}, "as it was prepared");
	assert_equal(0, *frames_converted, "with nothing converted");

	const auto unprepared = std::make_shared<fake_video_frame>();
	assert_equal(true, texture->update(unprepared) == ui::texture_update_result::tex_created,
	             "a picture in system memory is converted by the scaler");
	assert_equal(true, texture->dimensions() == sizei{32, 16}, "and held as converted");
	assert_equal(1, *frames_converted, "once");
	assert_equal(0, unprepared->cpu_requests, "which asks the decoder for nothing");

	const auto hardware = std::make_shared<fake_video_frame>();
	auto texture_name = 0;
	hardware->described.texture = &texture_name;
	assert_equal(true, texture->update(hardware) == ui::texture_update_result::tex_updated,
	             "a hardware picture is converted too");
	assert_equal(2, *frames_converted, "by the scaler");
	assert_equal(1, hardware->cpu_requests, "and the decoder is asked for pictures in system memory");

	ui::make_surface_scaler = nullptr;
	assert_equal(true,
	             renderer->create_texture()->update(unprepared) == ui::texture_update_result::failed,
	             "with no scaler a picture in system memory cannot be presented");
}

static void should_suppress_gpu_for_recovery_session()
{
	platform::suppress_crash_guard(platform::crash_guard::gpu_render, true);
	// Suppression is process-global; a failing assertion below must not leave it on.
	const df::scope_exit restore_guard([]
	{
		platform::suppress_crash_guard(platform::crash_guard::gpu_render, false);
	});
	assert_equal(true, platform::crash_guard_suppressed(platform::crash_guard::gpu_render),
	             "GPU suppressed during recovery");
	assert_equal(false, platform::crash_guard_suppressed(platform::crash_guard::hw_video_decode),
	             "decode suppression remains independent");
	platform::suppress_crash_guard(platform::crash_guard::gpu_render, false);
	assert_equal(false, platform::crash_guard_suppressed(platform::crash_guard::gpu_render),
	             "GPU suppression can be cleared");
}

// Issue #219 - Font glyph fallback for missing (Hangul) glyphs.
// The custom UI renders text with the system font "Calibri" (factories::font_face),
// which has NO Hangul. DirectWrite substitutes a fallback face (e.g. Malgun Gothic)
// for Korean. This test verifies the fallback path resolves Korean glyphs, and
// guards the render_glyph fix: metrics for a glyph must be read from the glyph
// run's OWN face (glyph_run->fontFace), not the primary UI font (_face). Reading
// them from the primary face returns a different glyph's metrics (or fails for an
// out-of-range index, dropping the glyph). render_glyph now queries glyph_face.
static void should_fall_back_for_missing_glyphs()
{
	constexpr char32_t hangul = U'\uAC00'; // 가

	// "Calibri" is the app's dialog/UI font; "Malgun Gothic" is the Windows Korean font.
	const auto probe = platform::probe_glyph_fallback("Calibri", "Malgun Gothic", hangul);

	if (!probe.available)
	{
		// Neither font is guaranteed on every machine. Assert the probe reported that
		// honestly rather than silently passing with nothing checked.
		assert_equal(0, probe.primary_glyph, "an unavailable probe reports no glyph data");
		return;
	}

	// 1. The primary UI font genuinely lacks Hangul -> fallback is mandatory.
	assert_equal(0, probe.primary_glyph, "Calibri has no Hangul glyph (fallback required)");

	// 2. The fallback face maps the same character to a real glyph, and querying
	//    that face for the glyph (what render_glyph SHOULD do) succeeds.
	assert_equal(true, probe.fallback_glyph != 0, "fallback face has a Hangul glyph");
	assert_equal(true, probe.fallback_metrics_ok, "fallback-face metrics query succeeds (correct face)");

	// 3. Querying the PRIMARY face for the same (fallback) glyph index yields the
	//    wrong glyph's metrics - the latent render_glyph bug.
	assert_equal(true, probe.primary_metrics_differ,
	             "primary-face metrics for a fallback glyph are wrong (latent bug)");
}

// Issue #232 - a missing Calibri produced a huge log, because every draw re-ran the family
// lookup and re-logged the failure. Issue #189 - after toggling Large Font some glyphs kept
// drawing at the old size. Both are properties of the same font-face cache: a repeated
// request must be served from the cache, and the requested size must be part of the identity
// so a size change cannot return the previous face.
static void should_cache_font_faces_per_face_and_size()
{
	const auto probe = platform::probe_font_cache(16);

	if (!probe.available)
	{
		// No text engine on this machine. Say so rather than passing with nothing checked.
		assert_equal(0, probe.entries_after_first, "an unavailable probe reports no cache entries");
		return;
	}

	assert_equal(1, probe.entries_after_first, "one request caches exactly one face");

	// #232: the repeat is answered from the cache, so no lookup and no log line repeats.
	assert_equal(true, probe.same_request_is_cached, "an identical request is served from the cache");

	// #189: the size is part of the cache identity.
	assert_equal(true, probe.size_change_is_distinct, "a font-size change yields a distinct face");
	assert_equal(true, probe.face_change_is_distinct, "a face-type change yields a distinct face");

	// A settings change resets the fonts; nothing may survive that.
	assert_equal(true, probe.reset_clears_cache, "resetting fonts empties the cache");
}

// #189 again, one level down: a cached glyph raster belongs to a size as well as to a face.
// IDWriteFontFace carries no size - the size lives on the glyph run - so a cache keyed on the
// face alone serves a raster made at the previous font size to text drawn at the current one,
// which is how a popup ended up mixing glyph sizes within one string.
static void should_key_glyph_cache_by_size()
{
	glyph_face_keys keys;

	assert_equal(true, keys.key(nullptr, 16.0f, 42) == keys.key(nullptr, 16.0f, 42),
	             "the same face, size and glyph give the same key");
	assert_equal(true, keys.key(nullptr, 16.0f, 42) != keys.key(nullptr, 24.0f, 42),
	             "a font-size change yields a distinct glyph key");
	assert_equal(true, keys.key(nullptr, 16.0f, 42) != keys.key(nullptr, 16.0f, 43),
	             "a different glyph yields a distinct key");
}

static void should_preserve_clipboard_format_contracts()
{
	const auto result = platform::probe_clipboard_contracts();
	assert_equal(true, result.clone_keeps_position, "clones resume at the original enumeration cursor");
	assert_equal(true, result.skip_to_end, "an exact skip succeeds and leaves the cursor at the end");
	assert_equal(true, result.skip_past_end, "an oversized skip reports partial completion and advances to the end");
	assert_equal(true, result.preferred_effect, "the advertised Cut effect passes capability negotiation");
	assert_equal(true, result.rejects_wrong_medium, "query and retrieval reject incompatible media consistently");
}

static void should_render_common_controls_into_a_buffer()
{
	const auto probe = platform::probe_buffered_control_paint();

	assert_equal(true, probe.trackbar_painted_pixels > 0, "trackbar draws into a supplied dc");
	assert_equal(true, probe.trackbar_colors > 1, "trackbar draws more than a flat fill");
	assert_equal(true, probe.toolbar_painted_pixels > 0, "toolbar draws into a supplied dc");
	assert_equal(true, probe.toolbar_colors > 1, "toolbar draws more than a flat fill");
	assert_equal(true, probe.button_painted_pixels > 0, "button draws into a supplied dc");
	assert_equal(true, probe.button_colors > 1, "button draws more than a flat fill");
}

static void should_replace_the_full_trailing_spelling_word()
{
	const auto invalid = [](const std::string_view word)
	{
		return word != "teh" && word != "x";
	};

	const auto replace_first = [](std::wstring text, const std::vector<platform::edit_spelling_range>& ranges)
	{
		assert_equal(1_z, ranges.size(), "one misspelling is found");
		text.replace(static_cast<size_t>(ranges.front().pos_start),
		             static_cast<size_t>(ranges.front().pos_end - ranges.front().pos_start), L"the");
		return text;
	};

	assert_equal(L"the"s, replace_first(L"teh", platform::probe_edit_spelling_ranges(L"teh", invalid)),
	             "a final word includes its last character");
	assert_equal(L"prefix the"s, replace_first(L"prefix teh", platform::probe_edit_spelling_ranges(L"prefix teh", invalid)),
	             "surrounding prefix text is untouched");
	assert_equal(L"the "s, replace_first(L"teh ", platform::probe_edit_spelling_ranges(L"teh ", invalid)),
	             "a word followed by whitespace keeps the separator");

	const auto one = platform::probe_edit_spelling_ranges(L"x", invalid);
	assert_equal(1_z, one.size(), "a one-character final word is found");
	assert_equal(0, one.front().pos_start, "one-character start");
	assert_equal(1, one.front().pos_end, "one-character end remains exclusive");
}

// The control checks words against the dictionary it is given and knows of no other: the application
// supplies spell checking, and without it nothing is underlined.
static void should_check_edit_words_against_the_supplied_dictionary()
{
	class listed_dictionary final : public ui::spelling_dictionary
	{
	public:
		mutable std::vector<std::string> asked;

		void queue_load() override {}

		bool is_word_valid(const std::string_view word) const override
		{
			asked.emplace_back(word);
			return word != "teh";
		}

		std::vector<std::string> suggest(std::string_view) const override { return {}; }
		void add_word(std::string_view, std::function<void()> done) override { if (done) done(); }
	};

	listed_dictionary dictionary;
	const auto unknown = platform::probe_edit_unknown_words(L"the teh cat", &dictionary);

	assert_equal(1_z, unknown.size(), "the word the dictionary rejects is underlined");
	assert_equal("teh"s, unknown.front().word, "which word");
	assert_equal(4, unknown.front().pos_start, "where it starts");
	assert_equal(7, unknown.front().pos_end, "where it ends");
	assert_equal(3_z, dictionary.asked.size(), "every word is asked of the dictionary");
	assert_equal(true, platform::probe_edit_unknown_words(L"the teh cat", nullptr).empty(),
	             "without a dictionary nothing is underlined");
}

// The chrome's embedded PNG - the drop shadow - travels in the module's resource script, and decodes
// with whatever decoder the application installs before any backend loads one, so both backends draw
// the same shadow with or without WIC. Diffractor's suite checks that it installs its own.
static void should_decode_embedded_chrome_images()
{
	const auto shadow_png = load_resource(IDB_SHADOW, L"PNG");
	constexpr std::array<uint8_t, 8> png_signature = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
	assert_equal(true, shadow_png.size() > png_signature.size() &&
	             std::equal(png_signature.begin(), png_signature.end(), shadow_png.data()),
	             "the shadow travels as a PNG");

	const auto installed = ui::decode_embedded_image;
	const df::scope_exit restore([installed] { ui::decode_embedded_image = installed; });
	size_t handed = 0;
	ui::decode_embedded_image = [&handed](const df::cspan data)
	{
		handed = data.size;
		auto decoded = std::make_shared<ui::surface>();
		decoded->alloc(4, 4, ui::texture_format::ARGB);
		return decoded;
	};

	const auto shadow = ui::decode_embedded(shadow_png);
	assert_equal(true, shadow != nullptr, "the shadow decodes");
	assert_equal(true, handed == shadow_png.size(), "with the application's decoder, handed all of it");

	ui::decode_embedded_image = nullptr;
	assert_equal(true, ui::decode_embedded(shadow_png) == nullptr, "and with none installed nothing decodes it");
}

static void should_hit_test_the_last_trailing_spelling_character()
{
	const auto point_x = [](const DWORD p) { return static_cast<int>(static_cast<short>(LOWORD(p))); };
	const auto point_y = [](const DWORD p) { return static_cast<int>(static_cast<short>(HIWORD(p))); };

	const auto parent = CreateWindowExW(0, L"STATIC", L"", WS_POPUP, 0, 0, 240, 60, nullptr, nullptr,
	                                   get_resource_instance, nullptr);
	assert_equal(true, parent != nullptr, "parent window created");
	const df::scope_exit destroy_parent([parent] { if (parent) DestroyWindow(parent); });

	const auto edit = CreateWindowExW(0, L"EDIT", L"prefix teh", WS_CHILD | WS_VISIBLE, 0, 0, 220, 30, parent,
	                                 nullptr, get_resource_instance, nullptr);
	assert_equal(true, edit != nullptr, "edit control created");
	const df::scope_exit destroy_edit([edit] { if (edit) DestroyWindow(edit); });

	const auto last_char = static_cast<DWORD>(SendMessageW(edit, EM_POSFROMCHAR, 9, 0));
	const auto end_pos = static_cast<DWORD>(SendMessageW(edit, EM_POSFROMCHAR, 10, 0));
	assert_equal(true, point_x(last_char) >= 0, "last character has a native edit position");
	assert_equal(-1, point_x(end_pos), "exclusive end is past the last native edit character");

	const auto bounds = platform::probe_edit_spelling_bounds(edit, 7, 10);
	assert_equal(true, !bounds.is_empty(), "the spelling bounds include a trailing word whose end is exclusive");
	assert_equal(true, bounds.contains({point_x(last_char) + 1, point_y(last_char) + 1}),
	             "the spelling hit-test reaches the final character");
}

static void should_delegate_ordinary_edit_context_menus()
{
	using route = platform::edit_context_menu_route;

	assert_equal(static_cast<int>(route::native_edit_procedure),
	             static_cast<int>(platform::probe_edit_context_menu_route(false, false)),
	             "spelling-disabled or correctly-spelled text is delegated");
	assert_equal(static_cast<int>(route::native_edit_procedure),
	             static_cast<int>(platform::probe_edit_context_menu_route(true, false)),
	             "a failed custom menu creation is delegated");
	assert_equal(static_cast<int>(route::custom_spelling_menu),
	             static_cast<int>(platform::probe_edit_context_menu_route(true, true)),
	             "a real misspelling shows exactly the custom spelling menu");
}

static void should_offer_the_maximize_caption_button_only_on_a_resizable_frame()
{
	constexpr uint32_t frame = WS_OVERLAPPEDWINDOW;
	constexpr uint32_t maximized = frame | WS_MAXIMIZE;
	constexpr uint32_t full_screen = frame & ~(WS_CAPTION | WS_THICKFRAME);
	constexpr uint32_t dialog = WS_POPUP | WS_CAPTION | WS_SYSMENU | WS_THICKFRAME;

	assert_equal(true, platform::presents_maximize_caption_button(df::os_release::windows_11, frame),
	             "a resizable Windows 11 frame offers snap layouts over its maximize button");
	assert_equal(true, platform::presents_maximize_caption_button(df::os_release::windows_later, frame),
	             "a later release keeps them");
	assert_equal(false, platform::presents_maximize_caption_button(df::os_release::windows_10, frame),
	             "Windows 10 has no snap layouts, so the button stays a toolbar button");
	assert_equal(false, platform::presents_maximize_caption_button(df::os_release::unknown, frame),
	             "a release that could not be read is not assumed to have them");
	assert_equal(false, platform::presents_maximize_caption_button(df::os_release::windows_11, full_screen),
	             "Fullscreen strips the resizing frame");
	assert_equal(false, platform::presents_maximize_caption_button(df::os_release::windows_11, dialog),
	             "a dialog has no maximize box");

	// The flyout would open over the restore button and take the click meant for it.
	assert_equal(false, platform::presents_maximize_caption_button(df::os_release::windows_11, maximized),
	             "a maximized frame's restore button stays a toolbar button");
	assert_equal(true, platform::can_present_maximize_caption_button(df::os_release::windows_11, maximized),
	             "a maximized frame still keeps the top of its frame clear for when it is restored");
	assert_equal(false, platform::can_present_maximize_caption_button(df::os_release::windows_10, frame),
	             "Windows 10 frames keep their frame as it was");
}

static void should_invoke_the_maximize_caption_button_only_on_a_release_over_its_press()
{
	platform::caption_button_tracker t;

	assert_equal(true, t.hover(7), "resting on the button lights it");
	assert_equal(false, t.hover(7), "moving within it changes nothing");
	assert_equal(0, t.release(7), "a release without a press invokes nothing");

	assert_equal(true, t.press(7), "pressing shows it pressed");
	assert_equal(7, t.release(7), "a release over the pressed button invokes it");
	assert_equal(0, t.pressed_id, "the press ends with the release");
	assert_equal(0, t.hover_id, "invoking forgets the hover, because the frame moves under the pointer");

	t.press(7);
	assert_equal(true, t.leave(), "leaving puts the light out");
	assert_equal(0, t.release(7), "a press abandoned by leaving invokes nothing");

	t.press(7);
	assert_equal(true, t.hover(0), "resting off the button redraws it");
	assert_equal(0, t.release(7), "a press abandoned by resting elsewhere invokes nothing");

	t.press(7);
	assert_equal(0, t.release(8), "a release over another button invokes nothing");
}

static void should_answer_hit_tests_on_the_maximize_toolbar_button_as_the_caption_button()
{
	const auto probe = platform::probe_toolbar_caption_button();

	assert_equal(HTCLIENT, probe.other_hit, "an ordinary toolbar button stays a toolbar button");
	assert_equal(HTCLIENT, probe.maximized_hit, "a maximized frame's restore button stays a toolbar button");
	assert_equal(HTCLIENT, probe.frameless_hit, "a frame without its resizing border presents no caption button");

	if (probe.offered)
	{
		assert_equal(HTMAXBUTTON, probe.maximize_hit, "the maximize button answers as the window's caption button");
		assert_equal(probe.maximize_id, probe.hovered_id, "resting on it lights it");
		assert_equal(1, probe.invocations, "a press and release over it invokes the command once");
		assert_equal(0, probe.hover_after_leave, "leaving it puts the light out");
	}
	else
	{
		assert_equal(HTCLIENT, probe.maximize_hit, "without snap layouts it stays a toolbar button");
		assert_equal(0, probe.invocations, "a hit test invokes nothing");
	}
}

static void should_bound_the_software_buffer_as_the_client_grows()
{
	const auto probe = platform::probe_software_tiling();

	// Without this the assertion below would still pass if the buffer went back to tracking the
	// window, which is the thing tiling exists to stop.
	assert_equal(true, probe.grown_buffer_pixels < probe.grown_client_pixels / 4,
	             "the buffer stayed bounded while the client grew");
	assert_equal(probe.grown_client_pixels, probe.grown_writable_pixels,
	             "a grown client is writable to its new edges without reallocating the tile");
}

static void should_translate_highlight_spans_to_utf16_clusters()
{
	const auto highlight = ui::color(1.0f, 0.0f, 0.0f, 1.0f);
	const auto plain = ui::color(0.0f, 0.0f, 1.0f, 1.0f);
	const std::string text = "\xf0\x9f\x98\x80" "caf\xc3\xa9" "X";
	const auto converted = ui::utf8_to_utf16(text, {{4, 5, highlight}});

	assert_equal(7_z, converted.text.size(), "emoji is preserved as a surrogate pair");
	assert_equal(0xd83d, static_cast<int>(converted.text[0]), "lead surrogate");
	assert_equal(0xde00, static_cast<int>(converted.text[1]), "trail surrogate");
	assert_equal(1_z, converted.highlights.size(), "one highlight survives conversion");
	assert_equal(2u, converted.highlights[0].offset, "highlight begins after the surrogate pair");
	assert_equal(4u, converted.highlights[0].length, "highlight length is UTF-16 text, not UTF-8 bytes");

	const uint16_t clusters[] = {0, 0, 1};
	const auto first = ui::text_cluster_span_for_glyph(0, 2, clusters, 3, 10);
	const auto second = ui::text_cluster_span_for_glyph(1, 2, clusters, 3, 10);
	assert_equal(10u, first.offset, "cluster offset includes run text position");
	assert_equal(2u, first.length, "one glyph cluster can span multiple text units");
	assert_equal(12u, second.offset, "the next glyph starts after the multi-unit cluster");
	assert_equal(highlight.r, ui::text_cluster_color(plain, first, {{10, 2, highlight}}).r,
	             "a complete cluster is highlighted");
	assert_equal(highlight.r, ui::text_cluster_color(plain, first, {{10, 1, highlight}}).r,
	             "a base letter highlight colours its combining-mark cluster");
}

static void should_build_text_cluster_spans_once_per_run()
{
	const uint16_t clusters[] = {0, 0, 1, 2, 2};
	const auto spans = ui::text_cluster_spans_for_glyph_run(3, clusters, 5, 4);

	assert_equal(3_z, spans.size(), "one span per glyph");
	assert_equal(4u, spans[0].offset, "first cluster starts at run text position");
	assert_equal(2u, spans[0].length, "first cluster spans two text units");
	assert_equal(6u, spans[1].offset, "second cluster follows");
	assert_equal(1u, spans[1].length, "second cluster is one text unit");
	assert_equal(7u, spans[2].offset, "third cluster follows");
	assert_equal(2u, spans[2].length, "third cluster spans two text units");
}

static void should_apply_positive_ascender_offsets_upward()
{
	assert_equal(80.0f, ui::glyph_top_from_baseline(100.0f, 20, 0.0f), "no ascender offset");
	assert_equal(77.0f, ui::glyph_top_from_baseline(100.0f, 20, 3.0f), "positive ascender moves up");
	assert_equal(83.0f, ui::glyph_top_from_baseline(100.0f, 20, -3.0f), "negative ascender moves down");
}

static void should_clear_text_highlights_between_draws()
{
	std::vector<ui::text_highlight_t> highlights = {{0, 3, ui::color(1.0f, 0.0f, 0.0f, 1.0f)}};
	ui::clear_text_highlights(highlights);
	assert_equal(true, highlights.empty(), "plain/layout draws start without stale highlight intervals");
}

static void should_gate_native_bubble_alpha_steps()
{
	const auto restore_animations = ui::animations_enabled;
	const df::scope_exit restore_scope([restore_animations] { ui::animations_enabled = restore_animations; });

	ui::animations_enabled = true;
	assert_equal(72, ui::fade_alpha_step(0, 255), "enabled fade advances toward visible");
	assert_equal(0, ui::fade_alpha_step(1, 0), "enabled fade snaps the terminal hidden alpha");

	ui::animations_enabled = false;
	auto alpha = 0;
	alpha = ui::gated_fade_alpha_step(alpha, 255);
	assert_equal(255, alpha, "disabled show snaps visible");
	alpha = ui::gated_fade_alpha_step(alpha, 0);
	assert_equal(0, alpha, "disabled hide snaps hidden");
}

static void should_handle_unavailable_font_layout()
{
	text_layout_impl layout(nullptr, ui::style::font_face::dialog);

	layout.update("text", ui::style::text_style::single_line);
	const auto measured = layout.measure_text(100, 100);
	assert_equal(0, measured.cx, "unavailable font has no measured width");
	assert_equal(0, measured.cy, "unavailable font has no measured height");

	const auto offsets = layout.offset_xs({0, 2, 4}, 100, 100);
	assert_equal(3_z, offsets.size(), "unavailable font still answers each requested offset");
	assert_equal(0, offsets[0], "first unavailable offset");
	assert_equal(0, offsets[1], "middle unavailable offset");
	assert_equal(0, offsets[2], "last unavailable offset");
	assert_equal(true, should_retry_font_renderer(nullptr), "GPU renderer retries a transient null font");
	font_renderer::draw(nullptr, nullptr, layout._layout.Get(), {0, 0, 100, 100}, {}, {});
	assert_equal(true, layout._layout == nullptr, "drawing an unavailable layout leaves it unavailable");
}

static void should_snap_gpu_text_in_pixel_coordinates()
{
	assert_equal(true, platform::probe_d3d11_pixel_snapping(),
	             "text callbacks report enabled snapping, identity transform and one pixel per DIP");
}

static void should_refuse_an_impossible_movie()
{
	// The probe is the whole of whether Render is offered, so an unstable answer would make the
	// command appear and disappear between toolbar rebuilds.
	const auto answer = platform::can_write_movies();
	assert_equal(answer, platform::can_write_movies(), "the encoder probe answers the same way twice");

	platform::movie_writer_request request;
	request.path = _temps.next_path(".mp4");
	request.extent = {0, 0};
	request.frame_rate = 30;
	request.video_bitrate = 6000000;

	// A writer that accepted this would leave a zero-byte file where the movie should be, and the
	// render would report success.
	assert_equal(true, platform::create_movie_writer(request) == nullptr, "a zero-sized movie is refused");
	assert_equal(false, platform::exists(request.path), "and no partial file is left behind");

	request.extent = {1920, 1080};
	request.frame_rate = 0;
	assert_equal(true, platform::create_movie_writer(request) == nullptr, "a movie with no frame rate is refused");
	assert_equal(false, platform::exists(request.path), "and still leaves nothing behind");
}

void register_render_win_tests(test_registry& tests)
{
	tests.add("Should use screen coordinates for saved window placement"s, should_use_screen_coordinates_for_saved_window_placement);
	tests.add("Should not treat pointer wheel IDs as modifiers"s, should_not_treat_pointer_wheel_ids_as_modifiers);
	tests.add("Should snap GPU text in pixel coordinates"s, should_snap_gpu_text_in_pixel_coordinates);
	tests.add("Should preserve clipboard format contracts"s, should_preserve_clipboard_format_contracts);
	tests.add("Should restore a window onto a display that still exists"s, should_restore_a_window_onto_a_display);
	tests.add("Should classify DXGI device loss"s, should_classify_dxgi_device_loss);
	tests.add("Should recognise the Microsoft Basic Render Driver as software"s,
	          should_recognise_the_basic_render_driver);
	tests.add("Should need shader sampling for planar video textures"s, should_need_sampling_for_planar_video);
	tests.add("Should count shared memory for an integrated GPU"s, should_count_shared_memory_for_an_integrated_gpu);
	tests.add("Should describe a WASAPI mix format"s, should_describe_a_wasapi_mix_format);
	tests.add("Should recycle a texture only when nothing draws it"s, should_recycle_a_texture_only_when_nothing_draws_it);
	tests.add("Should overwrite a recycled texture with the new picture"s,
	          should_overwrite_a_recycled_texture_with_the_new_picture);
	tests.add("Should present a video frame through its interface"s, should_present_a_video_frame_through_its_interface);
	tests.add("Should present a video frame in software through its interface"s,
	          should_present_a_video_frame_in_software_through_its_interface);
	tests.add("Should suppress GPU for one recovery session"s, should_suppress_gpu_for_recovery_session);
	tests.add("Issue #219: Should fall back for missing glyphs"s, should_fall_back_for_missing_glyphs);
	tests.add("Issue #232/#189: Should cache font faces per face and size"s,
	          should_cache_font_faces_per_face_and_size);
	tests.add("Issue #189: Should key glyph cache by size"s, should_key_glyph_cache_by_size);
	tests.add("Should render common controls into a buffer"s, should_render_common_controls_into_a_buffer);
	tests.add("Should replace the full trailing spelling word"s, should_replace_the_full_trailing_spelling_word);
	tests.add("Should check edit words against the supplied dictionary"s,
	          should_check_edit_words_against_the_supplied_dictionary);
	tests.add("Should decode embedded chrome images"s, should_decode_embedded_chrome_images);
	tests.add("Should hit test the last trailing spelling character"s,
	          should_hit_test_the_last_trailing_spelling_character);
	tests.add("Should delegate ordinary common controls edit context menus"s,
	          should_delegate_ordinary_edit_context_menus);
	tests.add("Should offer the maximize caption button only on a resizable frame"s,
	          should_offer_the_maximize_caption_button_only_on_a_resizable_frame);
	tests.add("Should invoke the maximize caption button only on a release over its press"s,
	          should_invoke_the_maximize_caption_button_only_on_a_release_over_its_press);
	tests.add("Should answer hit tests on the maximize toolbar button as the caption button"s,
	          should_answer_hit_tests_on_the_maximize_toolbar_button_as_the_caption_button);
	tests.add("Should bound the software buffer as the client grows"s,
	          should_bound_the_software_buffer_as_the_client_grows);
	tests.add("Should translate highlight spans to utf16 clusters"s, should_translate_highlight_spans_to_utf16_clusters);
	tests.add("Should build text cluster spans once per run"s, should_build_text_cluster_spans_once_per_run);
	tests.add("Should apply positive ascender offsets upward"s, should_apply_positive_ascender_offsets_upward);
	tests.add("Should clear text highlights between draws"s, should_clear_text_highlights_between_draws);
	tests.add("Should gate native bubble alpha steps"s, should_gate_native_bubble_alpha_steps);
	tests.add("Should handle unavailable font layout"s, should_handle_unavailable_font_layout);
	tests.add("Should refuse an impossible movie"s, should_refuse_an_impossible_movie);
}
