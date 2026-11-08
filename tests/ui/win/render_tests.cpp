// Windows UI frame cases that were mixed into the renderer test program.

#include "libdesktop/platform/win32.h"
#include "libdesktop/render.h"
#include "libdesktop/render/software.h"
#include "libdesktop/render/win32.h"
#include "libdesktop/ui.h"
#include "test.h"

using namespace std::literals;

static void should_offer_the_maximize_caption_button_only_on_a_resizable_frame()
{
	constexpr uint32_t frame = WS_OVERLAPPEDWINDOW;
	constexpr uint32_t maximized = frame | WS_MAXIMIZE;
	constexpr uint32_t full_screen = frame & ~(WS_CAPTION | WS_THICKFRAME);
	constexpr uint32_t dialog = WS_POPUP | WS_CAPTION | WS_SYSMENU | WS_THICKFRAME;

	assert_equal(true, platform::presents_maximize_caption_button(platform::os_release::windows_11, frame),
	             "a resizable Windows 11 frame offers snap layouts over its maximize button");
	assert_equal(true, platform::presents_maximize_caption_button(platform::os_release::windows_later, frame),
	             "a later release keeps them");
	assert_equal(false, platform::presents_maximize_caption_button(platform::os_release::windows_10, frame),
	             "Windows 10 has no snap layouts, so the button stays a toolbar button");
	assert_equal(false, platform::presents_maximize_caption_button(platform::os_release::unknown, frame),
	             "a release that could not be read is not assumed to have them");
	assert_equal(false, platform::presents_maximize_caption_button(platform::os_release::windows_11, full_screen),
	             "Fullscreen strips the resizing frame");
	assert_equal(false, platform::presents_maximize_caption_button(platform::os_release::windows_11, dialog),
	             "a dialog has no maximize box");

	// The flyout would open over the restore button and take the click meant for it.
	assert_equal(false, platform::presents_maximize_caption_button(platform::os_release::windows_11, maximized),
	             "a maximized frame's restore button stays a toolbar button");
	assert_equal(true, platform::can_present_maximize_caption_button(platform::os_release::windows_11, maximized),
	             "a maximized frame still keeps the top of its frame clear for when it is restored");
	assert_equal(false, platform::can_present_maximize_caption_button(platform::os_release::windows_10, frame),
	             "Windows 10 frames keep their frame as it was");
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

static void should_gate_native_bubble_alpha_steps()
{
	const auto restore_animations = render::animations_enabled;
	const core::scope_exit restore_scope([restore_animations] { render::animations_enabled = restore_animations; });

	render::animations_enabled = true;
	assert_equal(72, ui::fade_alpha_step(0, 255), "enabled fade advances toward visible");
	assert_equal(0, ui::fade_alpha_step(1, 0), "enabled fade snaps the terminal hidden alpha");

	render::animations_enabled = false;
	auto alpha = 0;
	alpha = ui::gated_fade_alpha_step(alpha, 255);
	assert_equal(255, alpha, "disabled show snaps visible");
	alpha = ui::gated_fade_alpha_step(alpha, 0);
	assert_equal(0, alpha, "disabled hide snaps hidden");
}

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

static ComPtr<ID3D11Texture2D> create_plain_texture(ID3D11Device* device, const core::sizei dims)
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

	constexpr core::sizei dims = {64, 48};
	constexpr auto stride = static_cast<size_t>(dims.cx) * 4;
	constexpr auto bytes = stride * dims.cy;
	constexpr uint32_t red = 0xFFFF0000u;
	constexpr uint32_t green = 0xFF00FF00u;
	const std::vector<uint32_t> first_picture(static_cast<size_t>(dims.cx) * dims.cy, red);
	const std::vector<uint32_t> second_picture(static_cast<size_t>(dims.cx) * dims.cy, green);
	const auto created_before = render::gpu_perf.textures_created.load();
	const auto reused_before = render::gpu_perf.textures_reused.load();

	{
		const auto first = d3d11_create_texture(f);
		assert_equal(true, first->update(dims, core::texture_format::ARGB, core::orientation::top_left,
		                                 reinterpret_cast<const uint8_t*>(first_picture.data()), stride, bytes) ==
		             render::texture_update_result::tex_created, "the first picture makes a texture");
	}

	assert_equal(1_z, f->texture_pool->size(), "which is kept when it is released");
	auto kept = f->texture_pool->take(dims, core::texture_format::ARGB);
	const auto first_texture = kept.texture;
	f->texture_pool->give(std::move(kept));

	{
		const auto second = d3d11_create_texture(f);
		assert_equal(true, second->update(dims, core::texture_format::ARGB, core::orientation::top_left,
		                                  reinterpret_cast<const uint8_t*>(second_picture.data()), stride, bytes) ==
		             render::texture_update_result::tex_created, "a texture taken from the pool is new to its caller");
		assert_equal(0_z, f->texture_pool->size(), "the second picture took it");
	}

	assert_equal(1ull, render::gpu_perf.textures_created.load() - created_before, "one texture was made");
	assert_equal(1ull, render::gpu_perf.textures_reused.load() - reused_before, "and one reused");

	const auto returned = f->texture_pool->take(dims, core::texture_format::ARGB);
	assert_equal(true, returned.texture == first_texture, "the second picture was drawn into the first texture");
	assert_equal(true, returned.texture && texture_holds(device.Get(), context.Get(), returned.texture.Get(), green),
	             "and holds the new picture, not the old one");
}

// A decoded frame as the renderers see it, described by the test rather than by a decoder.
class fake_video_frame final : public render::video_frame
{
public:
	core::const_surface_ptr prepared;
	render::video_picture described;
	mutable int cpu_requests = 0;

	core::const_surface_ptr prepared_surface() const override { return prepared; }
	render::video_picture picture() const override { return described; }
	void request_cpu_frames() const override { ++cpu_requests; }
};

// Converts any frame into a 32 x 16 picture, and counts the frames it was asked for.
class fake_frame_scaler final : public render::surface_scaler
{
public:
	std::shared_ptr<int> frames;

	bool scale_surface(const core::const_surface_ptr&, core::surface_ptr&, core::sizei, bool) override { return false; }
	bool convert_yuv_surface(const core::surface&, const core::surface_ptr&) override { return false; }

	bool scale_surface(const render::video_frame&, const core::surface_ptr& surface_out) override
	{
		++*frames;
		return surface_out->alloc(32, 16, core::texture_format::RGB) != nullptr;
	}
};

static std::function<std::unique_ptr<render::surface_scaler>()> make_fake_frame_scaler(std::shared_ptr<int> frames)
{
	return [frames]() -> std::unique_ptr<render::surface_scaler>
	{
		auto s = std::make_unique<fake_frame_scaler>();
		s->frames = frames;
		return s;
	};
}

static std::shared_ptr<fake_video_frame> make_prepared_video_frame(const core::sizei dimensions)
{
	auto frame = std::make_shared<fake_video_frame>();
	const auto picture = std::make_shared<core::surface>();
	picture->alloc(dimensions, core::texture_format::RGB);
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
	const auto restore_scaler = render::make_surface_scaler;
	const core::scope_exit restore([restore_scaler] { render::make_surface_scaler = restore_scaler; });
	render::make_surface_scaler = make_fake_frame_scaler(frames_converted);

	const auto texture = d3d11_create_texture(f);
	assert_equal(true, texture->update(make_prepared_video_frame({64, 48})) == render::texture_update_result::tex_created,
	             "a prepared picture is uploaded");
	assert_equal(true, texture->dimensions() == core::sizei{64, 48}, "as it was prepared");
	assert_equal(0, *frames_converted, "with nothing converted");

	const auto unprepared = std::make_shared<fake_video_frame>();
	assert_equal(true, texture->update(unprepared) != render::texture_update_result::failed,
	             "a picture in system memory is converted by the scaler");
	assert_equal(true, texture->dimensions() == core::sizei{32, 16}, "and uploaded as converted");
	assert_equal(1, *frames_converted, "once");
	assert_equal(0, unprepared->cpu_requests, "which asks the decoder for nothing");

	const auto unlocked = std::make_shared<fake_video_frame>();
	auto texture_name = 0;
	unlocked->described.texture = &texture_name;
	assert_equal(true, d3d11_create_texture(f)->update(unlocked) == render::texture_update_result::failed,
	             "a hardware picture without its device's lock is refused");
	assert_equal(1, *frames_converted, "rather than converted");

	render::make_surface_scaler = nullptr;
	assert_equal(true, d3d11_create_texture(f)->update(unprepared) == render::texture_update_result::failed,
	             "and with no scaler a picture in system memory cannot be presented");
}

// A picture decoded on the render device itself is copied once, straight out of the decoder's surface into
// the texture the scene draws, under the decoder's lock: no texture shared between devices, and nothing asked
// of the decoder. WARP decodes nothing and shares no decoder surfaces, so the bridge a decoder on a device of
// its own needs could only have handed this picture to the scaler.
static void should_copy_a_picture_decoded_on_the_render_device_into_its_texture()
{
	ComPtr<ID3D11DeviceContext> context;
	const auto device = create_warp_device(context);
	assert_equal(true, device != nullptr, "a WARP device");
	if (!device) return;

	UINT support = 0;
	const auto samples_nv12 = SUCCEEDED(device->CheckFormatSupport(DXGI_FORMAT_NV12, &support)) &&
		can_sample_texture_format(support);
	assert_equal(true, samples_nv12, "which samples NV12 planes");
	if (!samples_nv12) return;

	const auto restore_yuv = render::yuv_textures_enabled;
	const core::scope_exit restore_gate([restore_yuv] { render::yuv_textures_enabled = restore_yuv; });
	render::yuv_textures_enabled = true;

	const auto frames_converted = std::make_shared<int>(0);
	const auto restore_scaler = render::make_surface_scaler;
	const core::scope_exit restore([restore_scaler] { render::make_surface_scaler = restore_scaler; });
	render::make_surface_scaler = make_fake_frame_scaler(frames_converted);

	// The decoder's pool: NV12 surfaces in one array, the picture in the second.
	constexpr core::sizei extent{64, 48};
	D3D11_TEXTURE2D_DESC desc = {};
	desc.Width = extent.cx;
	desc.Height = extent.cy;
	desc.MipLevels = 1;
	desc.ArraySize = 2;
	desc.Format = DXGI_FORMAT_NV12;
	desc.SampleDesc.Count = 1;
	desc.Usage = D3D11_USAGE_DEFAULT;
	desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;

	ComPtr<ID3D11Texture2D> pool;
	assert_equal(true, SUCCEEDED(device->CreateTexture2D(&desc, nullptr, &pool)), "a pool of NV12 surfaces");
	if (!pool) return;

	auto held = 0;
	const auto frame = std::make_shared<fake_video_frame>();
	frame->described.texture = pool.Get();
	frame->described.index = 1;
	frame->described.width = extent.cx;
	frame->described.height = extent.cy;
	frame->described.lock = [](void* c) { ++*static_cast<int*>(c); };
	frame->described.unlock = [](void* c) { --*static_cast<int*>(c); };
	frame->described.lock_context = &held;

	const auto f = std::make_shared<factories>();
	f->d3d_device = device;
	f->d3d_context = context;
	f->texture_pool = std::make_unique<d3d11_texture_pool>(1024 * 1024);

	const auto texture = d3d11_create_texture(f);

	assert_equal(true, texture->update(frame) == render::texture_update_result::tex_created,
	             "the picture is given a texture of its own");
	assert_equal(true, texture->dimensions() == extent, "at the decoder's size, not converted");
	assert_equal(0, *frames_converted, "with nothing converted");
	assert_equal(0, frame->cpu_requests, "or asked of the decoder");
	assert_equal(0, held, "and the decoder's lock released");
	assert_equal(true, texture->update(frame) == render::texture_update_result::tex_updated,
	             "the next picture is copied into the same texture");
}

// The software renderer never samples the decoder's surfaces, so a hardware picture is converted by
// the scaler like any other, and the decode thread is asked to deliver the rest of the clip in system
// memory instead.
static void should_present_a_video_frame_in_software_through_its_interface()
{
	const auto frames_converted = std::make_shared<int>(0);
	const auto restore_scaler = render::make_surface_scaler;
	const core::scope_exit restore([restore_scaler] { render::make_surface_scaler = restore_scaler; });
	render::make_surface_scaler = make_fake_frame_scaler(frames_converted);

	const auto renderer = create_software_draw_context(nullptr, nullptr, false, render::default_font_size);
	const auto texture = renderer->create_texture();
	assert_equal(true, texture->update(make_prepared_video_frame({64, 48})) == render::texture_update_result::tex_created,
	             "a prepared picture is copied");
	assert_equal(true, texture->dimensions() == core::sizei{64, 48}, "as it was prepared");
	assert_equal(0, *frames_converted, "with nothing converted");

	const auto unprepared = std::make_shared<fake_video_frame>();
	assert_equal(true, texture->update(unprepared) == render::texture_update_result::tex_created,
	             "a picture in system memory is converted by the scaler");
	assert_equal(true, texture->dimensions() == core::sizei{32, 16}, "and held as converted");
	assert_equal(1, *frames_converted, "once");
	assert_equal(0, unprepared->cpu_requests, "which asks the decoder for nothing");

	const auto hardware = std::make_shared<fake_video_frame>();
	auto texture_name = 0;
	hardware->described.texture = &texture_name;
	assert_equal(true, texture->update(hardware) == render::texture_update_result::tex_updated,
	             "a hardware picture is converted too");
	assert_equal(2, *frames_converted, "by the scaler");
	assert_equal(1, hardware->cpu_requests, "and the decoder is asked for pictures in system memory");

	render::make_surface_scaler = nullptr;
	assert_equal(true,
	             renderer->create_texture()->update(unprepared) == render::texture_update_result::failed,
	             "with no scaler a picture in system memory cannot be presented");
}

// The chrome's embedded PNG - the drop shadow - travels in the render library, and decodes with
// whatever decoder the application installs before any backend loads one, so both backends draw the
// same shadow with or without WIC. Diffractor's suite checks that it installs its own.
static void should_decode_embedded_chrome_images()
{
	constexpr std::array<uint8_t, 8> png_signature = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};

	const auto installed = render::decode_embedded_image;
	const core::scope_exit restore([installed] { render::decode_embedded_image = installed; });
	size_t handed = 0;
	bool is_png = false;
	render::decode_embedded_image = [&handed, &is_png, &png_signature](const core::cspan data)
	{
		handed = data.size;
		is_png = data.size > png_signature.size() &&
			std::equal(png_signature.begin(), png_signature.end(), data.data);
		auto decoded = std::make_shared<core::surface>();
		decoded->alloc(4, 4, core::texture_format::ARGB);
		return decoded;
	};

	const auto shadow = render::load_shadow_art();
	assert_equal(true, is_png, "the shadow travels as a PNG");
	assert_equal(true, shadow != nullptr, "the shadow decodes");
	assert_equal(true, handed > png_signature.size(), "with the application's decoder, handed all of it");

	render::decode_embedded_image = nullptr;
	assert_equal(true, render::load_shadow_art() == nullptr, "and with none installed nothing decodes it");
}

void register_ui_render_win_tests(test_registry& tests)
{
	tests.add("Should offer the maximize caption button only on a resizable frame"s,
		should_offer_the_maximize_caption_button_only_on_a_resizable_frame);
	tests.add("Should answer hit tests on the maximize toolbar button as the caption button"s,
		should_answer_hit_tests_on_the_maximize_toolbar_button_as_the_caption_button);
	tests.add("Should gate native bubble alpha steps"s, should_gate_native_bubble_alpha_steps);
	tests.add("Should overwrite a recycled texture with the new picture"s, should_overwrite_a_recycled_texture_with_the_new_picture);
	tests.add("Should present a video frame through its interface"s, should_present_a_video_frame_through_its_interface);
	tests.add("Should copy a picture decoded on the render device into its texture"s,
		should_copy_a_picture_decoded_on_the_render_device_into_its_texture);
	tests.add("Should present a video frame in software through its interface"s,
		should_present_a_video_frame_in_software_through_its_interface);
	tests.add("Should decode embedded chrome images"s, should_decode_embedded_chrome_images);
}
