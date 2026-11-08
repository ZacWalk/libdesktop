// libdesktop-ui's animation tests.

#include "libdesktop/render.h"
#include "libdesktop/ui.h"
#include "test.h"

using namespace std::literals;

static void should_animate_alpha_between_values()
{
	// The gate is off when the CPU software backend is active, so force the animated
	// behaviour under test and restore whatever the running renderer selected.
	const auto restore_animations = render::animations_enabled;
	const core::scope_exit restore_scope([restore_animations] { render::animations_enabled = restore_animations; });
	render::animations_enabled = true;

	ui::animate_alpha alpha;
	alpha.reset(0.0f, 1.0f);
	assert_equal(0.0f, alpha.val(), "fade-in starts at zero");
	assert_equal(1.0f, alpha.target(), "fade-in targets one");
	assert_equal(true, alpha.step(), "fade-in advances");
	assert_equal(true, alpha.val() > 0.0f && alpha.val() < 1.0f, "fade-in interpolates");

	alpha.target(0.0f);
	assert_equal(true, alpha.val() > 0.0f, "retarget preserves current value");
	assert_equal(true, alpha.step(), "fade-out advances");

	for (auto i = 0; i < 100; ++i) alpha.step();
	assert_equal(0.0f, alpha.val(), "fade-out reaches target");
	assert_equal(false, alpha.step(), "completed animation stops");

	alpha.reset(0.0005f, 0.0f);
	assert_equal(true, alpha.step(), "final snap requests a frame");
	assert_equal(0.0f, alpha.val(), "final snap reaches target");
	assert_equal(false, alpha.step(), "final snap completes once");
}

// A fade used to advance a fixed fraction per frame, so the same fade ran twice as fast on a 120Hz
// display as on a 60Hz one. It now advances by elapsed time, so equal wall-clock time must leave it
// in the same place whatever the refresh rate that produced it.
static void should_fade_at_the_same_rate_on_any_refresh_rate()
{
	const auto restore_animations = render::animations_enabled;
	const auto restore_factor = ui::animation_step_factor;
	const core::scope_exit restore_scope([restore_animations, restore_factor]
	{
		render::animations_enabled = restore_animations;
		ui::animation_step_factor = restore_factor;
	});
	render::animations_enabled = true;

	const auto fade_for = [](const int frames_per_second, const double seconds)
	{
		const auto dt = 1.0 / frames_per_second;
		ui::animation_step_factor = static_cast<float>(1.0 - std::exp(-ui::alpha_fade_rate * dt));

		ui::animate_alpha alpha;
		alpha.reset(0.0f, 1.0f);

		for (auto i = 0; i < static_cast<int>(seconds * frames_per_second); ++i) alpha.step();

		return alpha.val();
	};

	constexpr auto quarter_second = 0.25;
	const auto at_60 = fade_for(60, quarter_second);
	const auto at_120 = fade_for(120, quarter_second);
	const auto at_30 = fade_for(30, quarter_second);

	assert_equal(true, at_60 > 0.1f && at_60 < 0.999f, "a quarter second is mid-fade");
	assert_equal(true, std::abs(at_120 - at_60) < 0.02f, "120Hz matches 60Hz at the same elapsed time");
	assert_equal(true, std::abs(at_30 - at_60) < 0.02f, "30Hz matches 60Hz at the same elapsed time");

	// The replaced constant: a 60Hz frame must still close a third of the remaining distance, so
	// nothing about the feel of a fade changes on the refresh rate almost every display reports.
	ui::animation_step_factor = static_cast<float>(1.0 - std::exp(-ui::alpha_fade_rate / 60.0));
	assert_equal(true, std::abs(ui::animation_step_factor - 0.333f) < 0.001f, "60Hz reproduces the old step");
}

static void should_skip_alpha_animation_when_disabled()
{
	// CPU software rendering cannot afford per-frame fades: every alpha reaches its target
	// immediately so draws stay on the opaque fast paths and no extra frames are requested.
	const auto restore_animations = render::animations_enabled;
	const core::scope_exit restore_scope([restore_animations] { render::animations_enabled = restore_animations; });
	render::animations_enabled = false;

	ui::animate_alpha alpha;
	alpha.reset(0.0f, 1.0f);
	assert_equal(1.0f, alpha.val(), "fade-in starts complete");
	assert_equal(false, alpha.step(), "fade-in requests no frame");

	alpha.target(0.0f);
	assert_equal(0.0f, alpha.val(), "fade-out completes immediately");
	assert_equal(false, alpha.step(), "fade-out requests no frame");
}

// The neutral parameter set is the one a picture with no edits is rendered through, so any drift in
// the tone curve, the YUV round trip or the rounding shows up here as a picture the app quietly
// altered. Truncating instead of rounding in adjust_color moves every channel a level and fails this.


void register_ui_render_tests(test_registry& tests)
{
	tests.add("Should animate alpha between values"s, should_animate_alpha_between_values);
	tests.add("Should fade at the same rate on any refresh rate"s, should_fade_at_the_same_rate_on_any_refresh_rate);
	tests.add("Should skip alpha animation when disabled"s, should_skip_alpha_animation_when_disabled);
}
