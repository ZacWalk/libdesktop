// libdesktop-render's portable renderer tests.

#include "libdesktop/render.h"
#include "libdesktop/render/software.h"
#include "test.h"

#include <clocale>
#include <format>
#include <set>

using namespace std::literals;

static void should_leave_colour_unchanged_when_neutral()
{
	const auto src = std::make_shared<core::surface>();
	assert_equal(true, src->alloc(16, 4, core::texture_format::ARGB), "neutral source allocated");

	for (auto y = 0; y < 4; ++y)
	{
		auto* const row = src->pixels_line(y);

		for (auto x = 0; x < 16; ++x)
		{
			row[x * 4 + 0] = static_cast<uint8_t>(x * 17);
			row[x * 4 + 1] = static_cast<uint8_t>(255 - x * 17);
			row[x * 4 + 2] = static_cast<uint8_t>((x * 37 + y * 11) & 0xff);
			row[x * 4 + 3] = 255;
		}
	}

	const auto dst = std::make_shared<core::surface>();
	assert_equal(true, dst->alloc(16, 4, core::texture_format::ARGB), "neutral destination allocated");

	render::color_adjust adjust;
	adjust.color_params(0, 0, 0, 0, 0, 0, 0);
	adjust.apply(src, dst->pixels(), dst->stride(), {});

	auto worst = 0;
	auto bias = 0;
	auto samples = 0;

	for (auto y = 0; y < 4; ++y)
	{
		const auto* const in = src->pixels_line(y);
		const auto* const out = dst->pixels_line(y);

		for (auto i = 0; i < 16 * 4; ++i)
		{
			const auto delta = static_cast<int>(out[i]) - static_cast<int>(in[i]);
			worst = std::max(worst, std::abs(delta));
			bias += delta;
			++samples;
		}
	}

	// One level of slack for floating point; two would hide a real curve error.
	assert_equal(true, worst <= 1, "neutral adjustment leaves the picture alone");

	// The residual must also be unbiased. A worst-case bound alone cannot see truncation, which costs
	// at most one level but costs it in the same direction on every pixel, darkening the whole picture.
	assert_equal(true, std::abs(bias) * 8 <= samples, "the neutral residual is not biased in one direction");
}

// Full desaturation is the one adjustment with an answer that can be stated exactly, so it pins the
// chroma path independently of the tone curve.
static void should_desaturate_to_grey()
{
	const auto src = std::make_shared<core::surface>();
	assert_equal(true, src->alloc(8, 1, core::texture_format::ARGB), "chroma source allocated");

	auto* const row = src->pixels_line(0);

	for (auto x = 0; x < 8; ++x)
	{
		row[x * 4 + 0] = static_cast<uint8_t>(x * 30);
		row[x * 4 + 1] = static_cast<uint8_t>(200 - x * 20);
		row[x * 4 + 2] = static_cast<uint8_t>(40 + x * 25);
		row[x * 4 + 3] = 255;
	}

	const auto dst = std::make_shared<core::surface>();
	assert_equal(true, dst->alloc(8, 1, core::texture_format::ARGB), "chroma destination allocated");

	render::color_adjust adjust;
	adjust.color_params(0, -1.0, 0, 0, 0, 0, 0);
	adjust.apply(src, dst->pixels(), dst->stride(), {});

	const auto* const out = dst->pixels_line(0);

	for (auto x = 0; x < 8; ++x)
	{
		const auto b = static_cast<int>(out[x * 4 + 0]);
		const auto g = static_cast<int>(out[x * 4 + 1]);
		const auto r = static_cast<int>(out[x * 4 + 2]);

		assert_equal(true, std::abs(r - g) <= 1 && std::abs(g - b) <= 1, "saturation of -1 leaves every pixel grey");
		assert_equal(255u, static_cast<uint32_t>(out[x * 4 + 3]), "alpha survives desaturation");
	}
}

// One opaque pixel through apply, answered red, green, blue.
static std::array<int, 3> grade_pixel(const render::color_adjust& adjust, const int r, const int g, const int b)
{
	const auto src = std::make_shared<core::surface>();
	const auto dst = std::make_shared<core::surface>();
	src->alloc(1, 1, core::texture_format::RGB);
	dst->alloc(1, 1, core::texture_format::RGB);

	auto* const in = src->pixels_line(0);
	in[0] = static_cast<uint8_t>(b);
	in[1] = static_cast<uint8_t>(g);
	in[2] = static_cast<uint8_t>(r);
	in[3] = 255;
	adjust.apply(src, dst->pixels(), dst->stride(), {});

	const auto* const out = dst->pixels_line(0);
	return {out[2], out[1], out[0]};
}

static double hue_degrees(const std::array<int, 3>& c)
{
	return std::atan2(0.8660254 * (c[1] - c[2]), c[0] - 0.5 * (c[1] + c[2])) * 180.0 / 3.14159265358979;
}

static double hsv_saturation(const std::array<int, 3>& c)
{
	const auto high = std::max({c[0], c[1], c[2]});
	return high > 0 ? (high - std::min({c[0], c[1], c[2]})) / static_cast<double>(high) : 0.0;
}

static double bt709_luma(const std::array<int, 3>& c)
{
	return 0.2126 * c[0] + 0.7152 * c[1] + 0.0722 * c[2];
}

// No setting can fold the curve back, which inverts contrast somewhere in the picture, or flatten a run of
// tones into black or white. A natural spline through clamped knots did both at the far end of the
// brightness and contrast sliders.
static void should_build_tone_curves_that_neither_fold_back_nor_clip()
{
	constexpr double settings[] = {-1.0, 0.0, 1.0};
	auto folded = 0;
	auto pinned = 0;

	for (const auto darks : settings)
		for (const auto midtones : settings)
			for (const auto lights : settings)
				for (const auto contrast : settings)
					for (const auto brightness : settings)
					{
						render::color_adjust adjust;
						adjust.color_params(0, 0, darks, midtones, lights, contrast, brightness);

						if (std::abs(adjust.tone(0.0)) > 1e-12 || std::abs(adjust.tone(1.0) - 1.0) > 1e-12) ++pinned;

						for (auto i = 1; i <= 1024; ++i)
						{
							const auto x = i / 1024.0;
							if (adjust.tone(x) <= adjust.tone((i - 1) / 1024.0)) ++folded;
							if (i < 1024 && (adjust.tone(x) <= 0.0 || adjust.tone(x) >= 1.0)) ++folded;
						}
					}

	assert_equal(0, pinned, "black and white stay where they are");
	assert_equal(0, folded, "every curve rises strictly, inside black and white");
}

// Vibrance weights the least saturated colours and spares skin, and never turns a hue: a negative setting
// once ran the multiplier below zero and inverted vivid colours to their complements.
static void should_vibrance_muted_colours_and_keep_every_hue()
{
	const std::array<std::array<int, 3>, 7> colours{
		{{255, 0, 0}, {0, 255, 0}, {0, 0, 255}, {0, 255, 255}, {70, 130, 220}, {100, 115, 140}, {230, 180, 150}}};

	for (const auto vibrance : {-1.0, 1.0})
	{
		render::color_adjust adjust;
		adjust.color_params(vibrance, 0, 0, 0, 0, 0, 0);

		for (const auto& c : colours)
		{
			const auto out = grade_pixel(adjust, c[0], c[1], c[2]);
			auto turn = std::abs(hue_degrees(out) - hue_degrees(c));
			turn = std::min(turn, 360.0 - turn);
			assert_equal(true, turn <= 4.0,
			             std::format("vibrance {} keeps the hue of {},{},{}", vibrance, c[0], c[1], c[2]));
		}
	}

	render::color_adjust more;
	more.color_params(1.0, 0, 0, 0, 0, 0, 0);
	const auto gain = [&](const int r, const int g, const int b)
	{
		return hsv_saturation(grade_pixel(more, r, g, b)) / hsv_saturation({r, g, b});
	};

	assert_equal(true, gain(100, 115, 140) > gain(70, 130, 220) + 0.1, "a muted colour gains more than a vivid one");
	assert_equal(true, gain(230, 180, 150) + 0.1 < gain(150, 200, 230), "skin gains less than a colour as saturated");
}

// A change of tone scales every channel by the luma's ratio, so darkening leaves a colour as saturated as it
// was instead of driving it to a clipped primary, and brightening does not wash it out.
static void should_keep_saturation_through_a_change_of_tone()
{
	const std::array foliage{60, 90, 40};

	for (const auto brightness : {-1.0, 1.0})
	{
		render::color_adjust adjust;
		adjust.color_params(0, 0, 0, 0, 0, 0, brightness);
		const auto out = grade_pixel(adjust, foliage[0], foliage[1], foliage[2]);

		assert_equal(true, std::abs(hsv_saturation(out) - hsv_saturation(foliage)) < 0.03,
		             std::format("brightness {} keeps the saturation", brightness));
		assert_equal(true,
		             brightness > 0 ? bt709_luma(out) > bt709_luma(foliage) + 40
		                            : bt709_luma(out) < bt709_luma(foliage) - 20,
		             std::format("brightness {} moves the tone", brightness));
	}
}

// White balance moves the colour, not the exposure: a neutral keeps its luma, and a highlight warmed past
// white is fitted back towards its luma, keeping its hue, rather than clipped in one channel.
static void should_keep_a_neutral_luma_through_white_balance()
{
	for (const auto temperature : {-1.0, 1.0})
	{
		for (const auto tint : {-1.0, 0.0, 1.0})
		{
			render::color_adjust adjust;
			adjust.color_params(0, 0, 0, 0, 0, 0, 0, temperature, tint);
			const auto grey = grade_pixel(adjust, 128, 128, 128);

			assert_equal(true, std::abs(bt709_luma(grey) - 128.0) <= 1.0,
			             std::format("temperature {} tint {} keeps a grey's luma", temperature, tint));
			assert_equal(true, temperature > 0 ? grey[0] > grey[2] : grey[0] < grey[2], "and warms or cools it");
		}
	}

	render::color_adjust warm;
	warm.color_params(0, 0, 0, 0, 0, 0, 0, 1.0, 0);
	const auto highlight = grade_pixel(warm, 250, 250, 250);
	assert_equal(true, std::abs(bt709_luma(highlight) - 250.0) <= 1.5, "a warmed highlight keeps its luma");
	assert_equal(true, highlight[0] >= highlight[1] && highlight[1] >= highlight[2] && highlight[0] > highlight[2],
	             "and its warmth, fitted below white");
}

// A wide picture is graded at its own precision: neutral settings leave every sixteen-bit value where it was,
// and a steep curve over a smooth ramp keeps far more distinct tones than a byte can hold.
static void should_grade_sixteen_bit_pictures_at_their_own_precision()
{
	constexpr auto width = 4096;
	const auto src = std::make_shared<core::surface>();
	const auto dst = std::make_shared<core::surface>();
	assert_equal(true, src->alloc(width, 1, core::texture_format::RGBX16) != nullptr, "wide ramp allocated");
	assert_equal(true, dst->alloc(width, 1, core::texture_format::RGBX16) != nullptr, "wide result allocated");

	auto* const ramp = std::bit_cast<uint16_t*>(src->pixels_line(0));
	for (auto x = 0; x < width; ++x)
	{
		ramp[x * 4 + 0] = ramp[x * 4 + 1] = ramp[x * 4 + 2] = static_cast<uint16_t>(x * 16);
		ramp[x * 4 + 3] = 0xffff;
	}

	render::color_adjust neutral;
	neutral.color_params(0, 0, 0, 0, 0, 0, 0);
	assert_equal(true, neutral.apply(src, dst->pixels(), dst->stride(), {}), "a wide picture is graded");

	const auto* const out = std::bit_cast<const uint16_t*>(dst->pixels_line(0));
	auto worst = 0;
	for (auto i = 0; i < width * 4; ++i) worst = std::max(worst, std::abs(out[i] - ramp[i]));
	assert_equal(0, worst, "neutral settings leave sixteen bits alone");

	render::color_adjust steep;
	steep.color_params(0, 0, 0, 0, 0, 1.0, 0);
	steep.apply(src, dst->pixels(), dst->stride(), {});

	std::set<uint16_t> tones;
	for (auto x = 0; x < width; ++x) tones.insert(out[x * 4]);
	assert_equal(true, tones.size() > 1000, "a steep curve keeps more tones than a byte holds");
}

// The software renderer grades with the template apply saves with, so the two agree within a level - for a
// byte source, and for a wide one, which it samples at its own sixteen bits.
static void should_grade_the_software_preview_as_apply_saves()
{
	render::color_adjust adjust;
	adjust.color_params(0.6, 0.3, -0.4, 0.2, 0.5, 0.7, -0.3, 0.4, -0.2);
	render::texture_transform transform;
	adjust.populate_texture_transform(transform);

	constexpr auto side = 16;
	const auto src = std::make_shared<core::surface>();
	assert_equal(true, src->alloc(side, side, core::texture_format::RGB) != nullptr, "source allocated");

	for (auto y = 0; y < side; ++y)
	{
		auto* const row = src->pixels_line(y);
		for (auto x = 0; x < side; ++x)
		{
			row[x * 4 + 0] = static_cast<uint8_t>(x * 16);
			row[x * 4 + 1] = static_cast<uint8_t>(y * 16);
			row[x * 4 + 2] = static_cast<uint8_t>((x * 37 + y * 53) & 0xff);
			row[x * 4 + 3] = 255;
		}
	}

	for (const auto wide : {false, true})
	{
		const auto source = wide ? core::to_16_bit(src) : core::const_surface_ptr(src);
		const auto saved = std::make_shared<core::surface>();
		assert_equal(true, saved->alloc(side, side, source->format()) != nullptr, "saved result allocated");
		adjust.apply(source, saved->pixels(), saved->stride(), {});
		const auto expected = core::to_8_bit(saved);

		core::surface target;
		assert_equal(true, target.alloc(side, side, core::texture_format::ARGB) != nullptr, "preview allocated");
		target.make_blank();
		const render::software_canvas canvas(target);
		canvas.blit_quad(*source, core::recti(0, 0, side, side), core::quadd(core::sizei{side, side}), 1.0f,
		                 render::texture_sampler::point, false, &transform);

		auto worst = 0;
		for (auto y = 0; y < side; ++y)
		{
			for (auto i = 0; i < side * 4; i += 4)
			{
				for (auto c = 0; c < 3; ++c)
				{
					worst = std::max(worst, std::abs(target.pixels_line(y)[i + c] - expected->pixels_line(y)[i + c]));
				}
			}
		}

		assert_equal(true, worst <= 1, std::format("the {} preview matches the saved grade", wide ? "wide" : "byte"));
	}
}

// A scene draws its vertices by reference, so rectangles updated after it was recorded are what the
// next present shows - each update in place of the last - with nothing recorded again.
static void should_present_vertex_rectangles_updated_after_the_scene()
{
	constexpr core::sizei extent{48, 32};
	const auto target = render::create_memory_present_target();
	const auto renderer = render::create_software_draw_context(nullptr, target, render::default_font_size);

	const auto shade = [&](const int x, const int y)
	{
		const auto buffer = target->acquire_buffer(extent);
		const auto* const p = buffer.bits + y * buffer.stride + x * 4;
		return core::rgb(p[2], p[1], p[0]);
	};

	const auto verts = renderer->create_vertices();
	const render::vertex_rect red[] = {{core::recti(4, 4, 20, 12), core::color(1.0f, 0.0f, 0.0f, 1.0f)}};
	const render::vertex_rect blue[] = {{core::recti(28, 16, 40, 28), core::color(0.0f, 0.0f, 1.0f, 1.0f)}};

	renderer->begin_draw(extent, render::default_font_size, {});
	renderer->clear(core::color(0.0f, 0.0f, 0.0f, 1.0f));
	verts->update_rects(red);
	renderer->draw_vertices(verts);
	renderer->render();

	assert_equal(core::rgb(255, 0, 0), shade(10, 8), "the rectangle is drawn");
	assert_equal(core::rgb(0, 0, 0), shade(30, 20), "and nothing beside it");

	verts->update_rects(blue);
	renderer->reset_damage();
	renderer->render();

	assert_equal(core::rgb(0, 0, 255), shade(30, 20), "a re-present shows the rectangle updated since");
	assert_equal(core::rgb(0, 0, 0), shade(10, 8), "in place of the one before");

	verts->update_rects({});
	renderer->reset_damage();
	renderer->render();

	assert_equal(core::rgb(0, 0, 0), shade(30, 20), "and an empty update draws nothing");
}

static void should_bound_the_software_buffer_as_the_client_grows()
{
	const auto probe = render::probe_software_tiling();

	// Without this the assertion below would still pass if the buffer went back to tracking the
	// window, which is the thing tiling exists to stop.
	assert_equal(true, probe.grown_buffer_pixels < probe.grown_client_pixels / 4,
	             "the buffer stayed bounded while the client grew");
	assert_equal(probe.grown_client_pixels, probe.grown_writable_pixels,
	             "a grown client is writable to its new edges without reallocating the tile");
}

// A subtitle's text: its runs in their own colours over one outline, in straight alpha, so it draws over any
// picture at any scale.
static void should_render_styled_text_with_its_outline()
{
	render::styled_text text;
	text.runs = {{"Red ", false, false, 0xFF0000FFu}, {"white", true, true, 0xFFFFFFFFu}};
	text.font_px = 40.0f;
	text.outline_px = 3.0f;
	text.outline_color = 0xFF000000u;

	const auto surface = render::render_styled_text(text);
	assert_equal(true, surface != nullptr, "the text is drawn");
	if (!surface) return;

	assert_equal(true, surface->width() > 80u && surface->height() >= 40u, "into a block that holds it");

	auto red = 0;
	auto white = 0;
	auto outline = 0;
	auto clear = 0;

	for (auto y = 0; y < static_cast<int>(surface->height()); ++y)
	{
		const auto* const line = surface->pixels_line(y);

		for (auto x = 0; x < static_cast<int>(surface->width()); ++x)
		{
			const auto* const p = line + x * 4;
			const auto b = p[0];
			const auto g = p[1];
			const auto r = p[2];
			const auto a = p[3];

			if (a == 0) ++clear;
			else if (a == 255 && r > 200 && g < 60 && b < 60) ++red;
			else if (a == 255 && r > 200 && g > 200 && b > 200) ++white;
			else if (a == 255 && r < 40 && g < 40 && b < 40) ++outline;
		}
	}

	assert_equal(true, red > 20, "the first run in its colour");
	assert_equal(true, white > 20, "the second in its own");
	assert_equal(true, outline > 20, "around them, the outline");
	assert_equal(true, clear > 0, "and clear around that");

	// Straight alpha: a faint edge keeps its colour rather than darkening towards black.
	auto faint_bright = 0;

	for (auto y = 0; y < static_cast<int>(surface->height()); ++y)
	{
		const auto* const line = surface->pixels_line(y);

		for (auto x = 0; x < static_cast<int>(surface->width()); ++x)
		{
			const auto* const p = line + x * 4;
			if (p[3] > 0 && p[3] < 128 && (p[0] > 128 || p[1] > 128 || p[2] > 128)) ++faint_bright;
		}
	}

	text.outline_px = 0.0f;
	const auto unoutlined = render::render_styled_text(text);
	assert_equal(true, unoutlined != nullptr, "text with no outline");

	if (unoutlined)
	{
		for (auto y = 0; y < static_cast<int>(unoutlined->height()); ++y)
		{
			const auto* const line = unoutlined->pixels_line(y);

			for (auto x = 0; x < static_cast<int>(unoutlined->width()); ++x)
			{
				const auto* const p = line + x * 4;
				if (p[3] > 0 && p[3] < 128 && (p[0] > 128 || p[1] > 128 || p[2] > 128)) ++faint_bright;
			}
		}
	}

	assert_equal(true, faint_bright > 0, "a glyph's soft edge keeps its colour: the alpha is straight");

	text.runs = {{"  \n ", false, false, 0xFFFFFFFFu}};
	assert_equal(true, render::render_styled_text(text) == nullptr, "blank text draws nothing");

	text.runs = {{"Text", false, false, 0xFFFFFFFFu}};
	text.font_px = 0.5f;
	assert_equal(true, render::render_styled_text(text) == nullptr, "nor does text too small to see");
}

// The platform's text engine lays text out by one set of rules on every platform: a line measures one
// line height, longer text measures wider, a line break or wrapping adds lines, and a layout's offsets
// advance along the text to its end.
static void should_measure_text_by_one_set_of_rules()
{
	constexpr auto face = render::font_face::dialog;

	const auto engine = render::create_text_engine(render::default_font_size);
	assert_equal(true, engine != nullptr, "the platform has a text engine");
	if (!engine) return;

	const auto near = [](const int a, const int b)
	{
		return std::abs(a - b) <= 1;
	};
	const auto line = engine->text_line_height(face);
	assert_equal(true, line >= render::default_font_size && line <= render::default_font_size * 2,
	             "a line is a little over the font size");

	const auto one = engine->measure_text("Ag", face, render::text_style::single_line, 1000, 0);
	assert_equal(true, near(line, one.cy), "one line measures one line height");
	assert_equal(true, one.cx > 0, "and has a width");

	const auto longer = engine->measure_text("Agreeable text", face, render::text_style::single_line, 1000, 0);
	assert_equal(true, longer.cx > one.cx, "longer text measures wider");
	assert_equal(true, near(line, longer.cy), "on the same one line");

	const auto two = engine->measure_text("Ag\nAg", face, render::text_style::single_line, 1000, 0);
	assert_equal(true, near(line * 2, two.cy), "a line break adds a line");

	constexpr auto words = "one two three four five six seven eight nine ten";
	const auto wrapped = engine->measure_text(words, face, render::text_style::multiline, longer.cx, 0);
	assert_equal(true, wrapped.cy >= line * 2 - 1, "multiline text wraps to its width");
	assert_equal(true, wrapped.cx <= longer.cx, "and keeps within it");
	const auto unwrapped = engine->measure_text(words, face, render::text_style::single_line, longer.cx, 0);
	assert_equal(true, near(line, unwrapped.cy), "single line text does not wrap");
	assert_equal(true, unwrapped.cx > longer.cx, "and is as wide as it is");

	assert_equal(true, engine->measure_text("", face, render::text_style::single_line, 1000, 0) == core::sizei{},
	             "nothing measures nothing");

	const auto layout = engine->create_text_layout(face);
	assert_equal(true, layout != nullptr, "a layout");
	if (!layout) return;

	layout->update("abc", render::text_style::single_line);
	const auto xs = layout->offset_xs({0, 1, 2, 3}, 1000);
	assert_equal(4u, static_cast<uint32_t>(xs.size()), "an x for every offset");
	if (xs.size() != 4) return;

	assert_equal(0, xs[0], "the first at the start");
	assert_equal(true, xs[1] > xs[0] && xs[2] > xs[1] && xs[3] > xs[2], "each later one further along");
	assert_equal(true, near(xs[3], layout->measure_text(1000, 1000).cx), "and the last at the text's end");
}

// Text drawn through a software context lands inside its box, centred where its style centres it, and a
// highlight colours the characters it covers.
static void should_draw_text_inside_its_box()
{
	constexpr core::sizei extent{240, 80};
	const auto target = render::create_memory_present_target();
	const auto ctx = render::create_software_draw_context(render::create_text_engine(render::default_font_size), target,
	                                                      render::default_font_size);
	const core::recti box(40, 20, 200, 60);

	const auto draw = [&](const std::vector<render::text_highlight_t>& highlights)
	{
		ctx->begin_draw(extent, render::default_font_size, {});
		ctx->clear(core::color(0.0f, 0.0f, 0.0f, 1.0f));
		ctx->draw_text("Hello there", highlights, box, render::font_face::dialog,
		               render::text_style::single_line_center, core::color(1.0f, 1.0f, 1.0f, 1.0f), {});
		ctx->render();
	};

	draw({});

	auto buffer = target->acquire_buffer(extent);
	core::recti ink(INT_MAX, INT_MAX, INT_MIN, INT_MIN);
	auto inside = 0;
	auto outside = 0;

	for (auto y = 0; y < extent.cy; ++y)
	{
		for (auto x = 0; x < extent.cx; ++x)
		{
			const auto* const p = buffer.bits + y * buffer.stride + x * 4;
			if (p[1] < 128) continue;

			if (box.contains({x, y})) ++inside;
			else ++outside;

			ink.left = std::min(ink.left, x);
			ink.top = std::min(ink.top, y);
			ink.right = std::max(ink.right, x + 1);
			ink.bottom = std::max(ink.bottom, y + 1);
		}
	}

	assert_equal(true, inside > 40, "the text is drawn");
	assert_equal(0, outside, "inside its box");
	assert_equal(true, std::abs(ink.center().x - box.center().x) <= 3, "centred across it");
	assert_equal(true, std::abs(ink.center().y - box.center().y) <= 6, "and down it");

	const render::text_highlight_t red{6, 5, core::color(1.0f, 0.0f, 0.0f, 1.0f)};
	draw({red});
	buffer = target->acquire_buffer(extent);

	auto reds = 0;
	auto whites = 0;

	for (auto y = box.top; y < box.bottom; ++y)
	{
		for (auto x = box.left; x < box.right; ++x)
		{
			const auto* const p = buffer.bits + y * buffer.stride + x * 4;
			if (p[2] > 160 && p[1] < 80) ++reds;
			else if (p[2] > 160 && p[1] > 160) ++whites;
		}
	}

	assert_equal(true, reds > 10, "a highlight colours the word it covers");
	assert_equal(true, whites > 10, "and leaves the rest in the text's colour");
}

#ifdef _WIN32
void register_render_win_tests(test_registry& tests);
#endif
void register_render_primitives_tests(test_registry& tests);
void register_render_polygon_tests(test_registry& tests);

static void register_tests(test_registry& tests)
{
#ifdef _WIN32
	register_render_win_tests(tests);
#endif
	register_render_primitives_tests(tests);
	register_render_polygon_tests(tests);
	tests.add("Should leave colour unchanged when neutral"s, should_leave_colour_unchanged_when_neutral);
	tests.add("Should desaturate to grey"s, should_desaturate_to_grey);
	tests.add("Should build tone curves that neither fold back nor clip"s,
	          should_build_tone_curves_that_neither_fold_back_nor_clip);
	tests.add("Should vibrance muted colours and keep every hue"s, should_vibrance_muted_colours_and_keep_every_hue);
	tests.add("Should keep saturation through a change of tone"s, should_keep_saturation_through_a_change_of_tone);
	tests.add("Should keep a neutral luma through white balance"s, should_keep_a_neutral_luma_through_white_balance);
	tests.add("Should grade sixteen bit pictures at their own precision"s,
	          should_grade_sixteen_bit_pictures_at_their_own_precision);
	tests.add("Should grade the software preview as apply saves"s, should_grade_the_software_preview_as_apply_saves);
	tests.add("Should present vertex rectangles updated after the scene"s,
	          should_present_vertex_rectangles_updated_after_the_scene);
	tests.add("Should bound the software buffer as the client grows"s,
	          should_bound_the_software_buffer_as_the_client_grows);
	tests.add("Should render styled text with its outline"s, should_render_styled_text_with_its_outline);
	tests.add("Should measure text by one set of rules"s, should_measure_text_by_one_set_of_rules);
	tests.add("Should draw text inside its box"s, should_draw_text_inside_its_box);
}

int main(const int argc, char* argv[])
{
	core::set_app_name("LibDesktop-Render-Tests");
	std::setlocale(LC_CTYPE, "C.UTF-8");
	const platform::thread_init thread;
	platform::set_ui_thread();
	platform::start_log();

	struct registry final : test_registry
	{
		std::vector<std::pair<std::string, std::function<void()>>> tests;
		void add(std::string name, std::function<void()> f) override
		{
			tests.emplace_back(std::move(name), std::move(f));
		}
	} all;

	register_tests(all);
	return run_registered_tests(all, argc, argv, [] { return std::vector<std::string>{}; });
}
