// libdesktop-render's CPU primitive and font tests.

#include "libdesktop/render.h"
#include "libdesktop/render/software.h"
#include "test.h"

using namespace std::literals;

namespace
{
	void should_draw_primitives_to_a_cpu_surface()
	{
		const auto surface = std::make_shared<core::surface>();
		const auto context = render::create_surface_context(surface);
		assert_equal(true, context != nullptr, "surface context created");
		if (!context) return;

		constexpr core::sizei extent{64, 48};
		context->begin_draw(extent, render::default_font_size, {});
		context->clear(core::color(0.0f, 0.0f, 0.0f, 1.0f));
		context->draw_line({4.0, 4.0}, {58.0, 12.0}, core::color(1.0f, 0.0f, 0.0f, 1.0f), 2.0);
		const std::array<core::pointd, 3> polyline{{{8.0, 30.0}, {22.0, 18.0}, {38.0, 34.0}}};
		context->draw_polyline(std::span<const core::pointd>(polyline), core::color(0.0f, 1.0f, 0.0f, 1.0f), 1.0);
		context->draw_ellipse({38, 4, 60, 26}, core::color(0.0f, 0.0f, 1.0f, 1.0f), 2.0);
		context->fill_ellipse({12, 28, 28, 44}, core::color(1.0f, 1.0f, 0.0f, 1.0f));

		const auto result = context->render();
		assert_equal(false, result.failed, "surface render succeeds");
		assert_equal(true, surface->dimensions() == extent, "surface has the requested extent");

		auto changed = 0;
		for (auto y = 0; y < extent.cy; ++y)
		{
			const auto* const row = surface->pixels_line(y);
			for (auto x = 0; x < extent.cx; ++x)
			{
				const auto* const pixel = row + x * 4;
				if (pixel[0] != 0 || pixel[1] != 0 || pixel[2] != 0) ++changed;
			}
		}
		assert_equal(true, changed > 0, "primitives reach the surface");
	}

	void should_draw_into_an_existing_cpu_surface()
	{
		const auto surface = std::make_shared<core::surface>();
		assert_equal(true, surface->alloc(16, 16, core::texture_format::ARGB) != nullptr, "surface allocated");
		const auto context = render::create_surface_context(surface);
		assert_equal(true, context != nullptr, "existing surface context created");
		if (!context) return;

		context->draw_rect({0, 0, 16, 16}, core::color(0.2f, 0.4f, 0.6f, 1.0f));
		context->draw_line({0.0, 0.0}, {15.0, 15.0}, core::color(1.0f, 0.0f, 0.0f, 1.0f));
		context->render();
		surface->set_pixel(1, 1, 0xff102030u);
		assert_equal(0xff102030u, surface->get_pixel(1, 1), "direct surface writes remain visible");
	}

	void should_keep_existing_pixels_outside_what_a_surface_context_draws()
	{
		const auto surface = std::make_shared<core::surface>();
		assert_equal(true, surface->alloc(32, 32, core::texture_format::ARGB) != nullptr, "surface allocated");
		for (auto y = 0; y < 32; ++y)
			for (auto x = 0; x < 32; ++x) surface->set_pixel(x, y, 0xff3366ccu);

		const auto context = render::create_surface_context(surface);
		assert_equal(true, context != nullptr, "context over an existing surface");
		if (!context) return;

		context->draw_rect({4, 4, 12, 12}, core::color(1.0f, 0.0f, 0.0f, 1.0f));
		context->render();
		assert_equal(true, surface->dimensions() == core::sizei{32, 32}, "surface keeps its extent");
		assert_equal(0xff3366ccu, surface->get_pixel(24, 24), "pixels outside the drawing survive");
		assert_equal(true, surface->get_pixel(8, 8) != 0xff3366ccu, "the drawing reaches the surface");
	}

	void should_upload_and_draw_a_cpu_surface()
	{
		const auto source = std::make_shared<core::surface>();
		assert_equal(true, source->alloc(2, 2, core::texture_format::ARGB) != nullptr, "source allocated");
		for (auto y = 0; y < 2; ++y)
			for (auto x = 0; x < 2; ++x) source->set_pixel(x, y, 0xff3366ccu);

		const auto target = std::make_shared<core::surface>();
		const auto context = render::create_surface_context(target);
		assert_equal(true, context != nullptr, "draw surface context created");
		if (!context) return;

		context->begin_draw({16, 16}, render::default_font_size, {});
		context->clear(core::color(0.0f, 0.0f, 0.0f, 1.0f));
		render::draw_surface(*context, source, {4, 4, 12, 12}, 1.0f, render::texture_sampler::point);
		context->render();
		assert_equal(0xff3366ccu, target->get_pixel(8, 8), "draw_surface uploads the source pixels");
	}

	void should_cache_custom_font_descriptors()
	{
		const render::font_desc desc{"Consolas", 19, 600, true, true, false, false};
		const auto first = render::make_font(desc);
		const auto second = render::make_font(desc);
		assert_equal(true, first == second, "the same descriptor has one process face");
		assert_equal(true, first != render::font_face::dialog, "custom faces are past the named faces");

		const auto context = render::create_surface_context(std::make_shared<core::surface>());
		assert_equal(true, context != nullptr, "font context created");
		if (!context) return;
		context->begin_draw({32, 32}, render::default_font_size, {});
		const auto measured = render::metrics(*context, first);
		assert_equal(true, measured.height > 0, "custom font has a line height");
		assert_equal(true, measured.avg_char_width > 0, "custom font has an advance");
	}

	// The baseline is the font's own: a layout that sits text on a line, as a web page does, needs the
	// real ascent and descent, not a share of the line height.
	void should_measure_the_font_baseline()
	{
		const auto context = render::create_surface_context(std::make_shared<core::surface>());
		assert_equal(true, context != nullptr, "font context created");
		if (!context) return;
		context->begin_draw({32, 32}, render::default_font_size, {});

		const auto height = context->text_line_height(render::font_face::dialog);
		const auto baseline = context->text_baseline(render::font_face::dialog);
		assert_equal(true, height > 0, "a line has height");
		assert_equal(true, baseline > height / 2 && baseline < height, "the baseline is in the line's lower half");

		const auto measured = render::metrics(*context, render::font_face::dialog);
		assert_equal(baseline, measured.ascent, "the ascent is the distance to the baseline");
		assert_equal(measured.height, measured.ascent + measured.descent, "ascent and descent make the line");
		assert_equal(true, measured.descent > 0, "text descends below its baseline");
	}
}

void register_render_primitives_tests(test_registry& tests)
{
	tests.add("Should draw primitives to a CPU surface"s, should_draw_primitives_to_a_cpu_surface);
	tests.add("Should draw into an existing CPU surface"s, should_draw_into_an_existing_cpu_surface);
	tests.add("Should keep existing pixels outside what a surface context draws"s,
		should_keep_existing_pixels_outside_what_a_surface_context_draws);
	tests.add("Should upload and draw a CPU surface"s, should_upload_and_draw_a_cpu_surface);
	tests.add("Should cache custom font descriptors"s, should_cache_custom_font_descriptors);
	tests.add("Should measure the font baseline"s, should_measure_the_font_baseline);
}
