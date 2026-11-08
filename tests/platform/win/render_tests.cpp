// Windows platform cases that were mixed into the renderer test program.

#include "libdesktop/platform.h"
#include "libdesktop/platform/win32.h"
#include "platform/test_support.h"
#include "test.h"

#include <mmreg.h>

platform::audio_output_format wasapi_mix_format(const WAVEFORMATEX* format);

using namespace std::literals;


// Undocking or unplugging a second display leaves a saved rect that intersects no monitor at all.
static void should_restore_a_window_onto_a_display()
{
	constexpr core::recti work(0, 0, 1920, 1040);

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

static void should_use_screen_coordinates_for_saved_window_placement()
{
	const core::recti placement(100, 100, 900, 700);
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

static void should_suppress_gpu_for_recovery_session()
{
	platform::suppress_crash_guard(platform::crash_guard::gpu_render, true);
	// Suppression is process-global; a failing assertion below must not leave it on.
	const core::scope_exit restore_guard([]
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

static void should_preserve_clipboard_format_contracts()
{
	const auto result = platform::probe_clipboard_contracts();
	assert_equal(true, result.clone_keeps_position, "clones resume at the original enumeration cursor");
	assert_equal(true, result.skip_to_end, "an exact skip succeeds and leaves the cursor at the end");
	assert_equal(true, result.skip_past_end, "an oversized skip reports partial completion and advances to the end");
	assert_equal(true, result.preferred_effect, "the advertised Cut effect passes capability negotiation");
	assert_equal(true, result.rejects_wrong_medium, "query and retrieval reject incompatible media consistently");
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

void register_platform_render_win_tests(test_registry& tests)
{
	tests.add("Should restore a window onto a display that still exists"s, should_restore_a_window_onto_a_display);
	tests.add("Should use screen coordinates for saved window placement"s, should_use_screen_coordinates_for_saved_window_placement);
	tests.add("Should not treat pointer wheel IDs as modifiers"s, should_not_treat_pointer_wheel_ids_as_modifiers);
	tests.add("Should describe a WASAPI mix format"s, should_describe_a_wasapi_mix_format);
	tests.add("Should suppress GPU for one recovery session"s, should_suppress_gpu_for_recovery_session);
	tests.add("Should preserve clipboard format contracts"s, should_preserve_clipboard_format_contracts);
	tests.add("Should refuse an impossible movie"s, should_refuse_an_impossible_movie);
}
