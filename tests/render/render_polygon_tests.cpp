// libdesktop-render's polygon tests: fills, gradients, borders and edges on the CPU renderer, retained
// polygons, and re-presenting only part of a scene.

#include "libdesktop/render.h"
#include "libdesktop/render/software.h"
#include "test.h"

using namespace std::literals;

namespace
{
	const core::color black(0.0f, 0.0f, 0.0f, 1.0f);
	const core::color white(1.0f, 1.0f, 1.0f, 1.0f);
	const core::color red(1.0f, 0.0f, 0.0f, 1.0f);
	const core::color blue(0.0f, 0.0f, 1.0f, 1.0f);

	int red_of(const uint32_t p)
	{
		return static_cast<int>(p >> 16 & 0xff);
	}
	int green_of(const uint32_t p)
	{
		return static_cast<int>(p >> 8 & 0xff);
	}
	int blue_of(const uint32_t p)
	{
		return static_cast<int>(p & 0xff);
	}

	// What the software renderer draws over black.
	core::surface_ptr draw_on_surface(const core::sizei extent, const std::function<void(render::draw_context&)>& draw)
	{
		auto surface = std::make_shared<core::surface>();
		const auto context = render::create_surface_context(surface);
		if (!context) return {};

		context->begin_draw(extent, render::default_font_size, {});
		context->clear(black);
		draw(*context);
		context->render();
		return surface;
	}

	void should_fill_a_concave_polygon()
	{
		// A U, whose notch is outside it.
		const std::array<core::pointd, 8> u{
			{{4.3, 4.3}, {12.3, 4.3}, {12.3, 20.3}, {28.3, 20.3}, {28.3, 4.3}, {36.3, 4.3}, {36.3, 28.3}, {4.3, 28.3}}};

		for (const auto smooth : {false, true})
		{
			const auto surface = draw_on_surface({40, 32}, [&](render::draw_context& dc)
			{ dc.draw_polygon(u, {.fill = white, .smooth = smooth}); });
			assert_equal(true, surface != nullptr, "surface drawn");
			if (!surface) return;

			assert_equal(0xffffffffu, surface->get_pixel(8, 12), "the left arm is filled");
			assert_equal(0xffffffffu, surface->get_pixel(32, 12), "the right arm is filled");
			assert_equal(0xffffffffu, surface->get_pixel(20, 24), "the base is filled");
			assert_equal(0xff000000u, surface->get_pixel(20, 12), "the notch is not");
			assert_equal(0xff000000u, surface->get_pixel(2, 2), "nor is anything outside");
		}
	}

	// A mesh of crisp polygons covers each pixel once: a square cut along its diagonal and drawn half
	// transparent blends every pixel inside it exactly once.
	void should_meet_crisp_neighbours_without_gap_or_overlap()
	{
		const std::array<core::pointd, 3> upper{{{4.25, 4.25}, {27.75, 4.25}, {27.75, 27.75}}};
		const std::array<core::pointd, 3> lower{{{4.25, 4.25}, {27.75, 27.75}, {4.25, 27.75}}};
		const render::polygon_style half{.fill = core::color(1.0f, 1.0f, 1.0f, 0.5f), .smooth = false};

		const auto surface = draw_on_surface({32, 32}, [&](render::draw_context& dc)
		{
			dc.draw_polygon(upper, half);
			dc.draw_polygon(lower, half);
		});
		assert_equal(true, surface != nullptr, "surface drawn");
		if (!surface) return;

		auto once = 0;
		auto wrong = 0;

		for (auto y = 0; y < 32; ++y)
		{
			for (auto x = 0; x < 32; ++x)
			{
				const auto inside = x >= 4 && x <= 27 && y >= 4 && y <= 27;
				const auto value = red_of(surface->get_pixel(x, y));
				if (inside && std::abs(value - 128) <= 1) ++once;
				else if (inside || value != 0) ++wrong;
			}
		}

		assert_equal(24 * 24, once, "every pixel of the square is blended once");
		assert_equal(0, wrong, "none twice, none missed and none outside");
	}

	// Smooth edges cover a pixel by how much of it is inside, so the coverage adds up to the area; crisp ones
	// cover whole pixels or none.
	void should_cover_smooth_edges_by_area()
	{
		const std::array<core::pointd, 4> tilted{{{6.3, 3.1}, {30.7, 9.4}, {24.2, 29.6}, {3.8, 22.9}}};
		auto area = 0.0;
		for (size_t i = 0; i < tilted.size(); ++i)
		{
			const auto a = tilted[i];
			const auto b = tilted[(i + 1) % tilted.size()];
			area += (a.X * b.Y - b.X * a.Y) / 2.0;
		}

		for (const auto smooth : {false, true})
		{
			const auto surface = draw_on_surface({36, 34}, [&](render::draw_context& dc)
			{ dc.draw_polygon(tilted, {.fill = white, .smooth = smooth}); });
			assert_equal(true, surface != nullptr, "surface drawn");
			if (!surface) return;

			auto partial = 0;
			auto covered = 0.0;
			for (auto y = 0; y < 34; ++y)
			{
				for (auto x = 0; x < 36; ++x)
				{
					const auto value = red_of(surface->get_pixel(x, y));
					if (value > 0 && value < 255) ++partial;
					covered += value / 255.0;
				}
			}

			if (smooth)
			{
				assert_equal(true, partial > 40, "smooth edges are shaded");
				assert_near(std::abs(area), covered, std::abs(area) * 0.01, "smooth coverage adds up to the area");
			}
			else
			{
				assert_equal(0, partial, "crisp edges are not");
				assert_near(std::abs(area), covered, std::abs(area) * 0.06, "crisp pixels approximate the area");
			}
		}
	}

	void should_hold_a_linear_gradient_at_its_ends()
	{
		const std::array<core::pointd, 4> band{{{0.0, 0.0}, {64.0, 0.0}, {64.0, 8.0}, {0.0, 8.0}}};
		const auto surface = draw_on_surface({64, 8}, [&](render::draw_context& dc) {
			dc.draw_polygon(band,
			                {.fill = render::paint::linear({16.0, 0.0}, red, {48.0, 0.0}, blue), .smooth = false});
		});
		assert_equal(true, surface != nullptr, "surface drawn");
		if (!surface) return;

		assert_equal(0xffff0000u, surface->get_pixel(4, 4), "before its start it is the start colour");
		assert_equal(0xff0000ffu, surface->get_pixel(60, 4), "past its end the end colour");

		// Halfway along, at x = 31.5: (31.5 - 16) / 32 of the way.
		const auto mid = surface->get_pixel(31, 4);
		const auto t = (31.5 - 16.0) / 32.0;
		assert_near(255.0 * (1.0 - t), red_of(mid), 1.5, "and between them it mixes");
		assert_near(255.0 * t, blue_of(mid), 1.5, "both ways");
	}

	// Colours mix premultiplied: yellow fading to transparent yellow stays yellow as it fades.
	void should_fade_a_radial_gradient_in_its_own_hue()
	{
		const std::array<core::pointd, 4> square{{{0.0, 0.0}, {64.0, 0.0}, {64.0, 64.0}, {0.0, 64.0}}};
		const core::color yellow(1.0f, 1.0f, 0.0f, 1.0f);
		const core::color clear_yellow(1.0f, 1.0f, 0.0f, 0.0f);

		const auto surface = draw_on_surface({64, 64}, [&](render::draw_context& dc)
		{
			dc.draw_polygon(square,
			                {.fill = render::paint::radial({32.0, 32.0}, 24.0, yellow, clear_yellow), .smooth = false});
		});
		assert_equal(true, surface != nullptr, "surface drawn");
		if (!surface) return;

		const auto halfway = surface->get_pixel(43, 31);
		const auto alpha = 1.0 - std::hypot(11.5, 0.5) / 24.0;
		assert_near(255.0 * alpha, red_of(halfway), 2.0, "halfway out it is half faded");
		assert_equal(red_of(halfway), green_of(halfway), "and still yellow");
		assert_equal(0, blue_of(halfway), "with no blue in it");
		assert_equal(0xff000000u, surface->get_pixel(62, 31), "beyond its radius it is gone");
		assert_equal(true, red_of(surface->get_pixel(31, 31)) > 240, "and at its centre it is whole");
	}

	// The border straddles the outline, mitred at its corners and drawn over the fill.
	void should_centre_a_border_on_the_outline()
	{
		const std::array<core::pointd, 4> square{{{20.0, 20.0}, {60.0, 20.0}, {60.0, 60.0}, {20.0, 60.0}}};
		const auto surface = draw_on_surface({80, 80}, [&](render::draw_context& dc)
		{ dc.draw_polygon(square, {.fill = blue, .border = white, .border_width = 4.0, .smooth = false}); });
		assert_equal(true, surface != nullptr, "surface drawn");
		if (!surface) return;

		assert_equal(0xff000000u, surface->get_pixel(17, 40), "outside the border is untouched");
		assert_equal(0xffffffffu, surface->get_pixel(18, 40), "the border reaches half its width outside");
		assert_equal(0xffffffffu, surface->get_pixel(21, 40), "and half inside");
		assert_equal(0xff0000ffu, surface->get_pixel(22, 40), "where the fill shows");
		assert_equal(0xffffffffu, surface->get_pixel(18, 18), "the corner is mitred square");
		assert_equal(0xffffffffu, surface->get_pixel(61, 61), "on every corner");
	}

	// A fill reads its colour and coverage from its own geometry, so a tile draws exactly what the whole
	// canvas does.
	void should_fill_contours_the_same_in_tiles_as_whole()
	{
		constexpr core::sizei extent{97, 71};
		constexpr int tile_edge = 16;
		const std::array<core::pointd, 7> outline{
			{{3.4, 61.2}, {20.9, 2.7}, {47.3, 30.1}, {66.8, 4.4}, {93.6, 58.7}, {52.2, 44.9}, {30.5, 68.3}}};
		const uint32_t ends[] = {static_cast<uint32_t>(outline.size())};
		const auto gradient = render::paint::radial({48.0, 35.0}, 40.0, core::color(0.9f, 0.3f, 0.1f, 1.0f),
		                                            core::color(0.1f, 0.4f, 0.9f, 0.3f));

		const auto draw = [&](const render::software_canvas& canvas)
		{
			canvas.fill_contours(outline, ends, gradient, true);
			canvas.fill_contours(outline, ends, core::color(0.2f, 0.9f, 0.4f, 0.5f), false);
		};

		std::vector<uint8_t> whole(static_cast<size_t>(extent.cx) * extent.cy * 4, 0x40);
		std::vector<uint8_t> tiled(whole.size(), 0x40);

		render::software_canvas canvas;
		canvas._bits = whole.data();
		canvas._stride = extent.cx * 4;
		canvas._buffer_extent = extent;
		canvas._clip = core::recti(0, 0, extent.cx, extent.cy);
		draw(canvas);

		std::vector<uint8_t> tile(static_cast<size_t>(tile_edge) * tile_edge * 4);
		for (auto ty = 0; ty < extent.cy; ty += tile_edge)
		{
			for (auto tx = 0; tx < extent.cx; tx += tile_edge)
			{
				const auto bounds = core::recti(tx, ty, tx + tile_edge, ty + tile_edge)
				                        .intersection({0, 0, extent.cx, extent.cy});
				std::ranges::fill(tile, uint8_t{0x40});

				render::software_canvas part;
				part._bits = tile.data();
				part._stride = tile_edge * 4;
				part._buffer_extent = {tile_edge, tile_edge};
				part._origin = {tx, ty};
				part._clip = bounds;
				draw(part);

				for (auto y = bounds.top; y < bounds.bottom; ++y)
				{
					memcpy(tiled.data() + (static_cast<size_t>(y) * extent.cx + bounds.left) * 4,
					       tile.data() + static_cast<size_t>(y - ty) * tile_edge * 4 +
					           static_cast<size_t>(bounds.left - tx) * 4,
					       static_cast<size_t>(bounds.width()) * 4);
				}
			}
		}

		auto painted = 0;
		for (size_t i = 0; i < whole.size(); i += 4)
			if (whole[i] != 0x40 || whole[i + 1] != 0x40) ++painted;

		assert_equal(true, painted > 1000, "the polygon is drawn");
		assert_equal(true, whole == tiled, "tiles draw exactly what the whole canvas does");
	}

	// A scene draws its vertices by reference, so polygons updated after it was recorded are what the next
	// present shows, with nothing recorded again.
	void should_present_vertex_polygons_updated_after_the_scene()
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

		const std::array<core::pointd, 3> triangle{{{4.0, 4.0}, {28.0, 4.0}, {4.0, 28.0}}};
		const auto verts = renderer->create_vertices();
		const render::vertex_polygon first[] = {{triangle, {.fill = red, .smooth = false}}};
		const render::vertex_polygon second[] = {{triangle, {.fill = blue, .smooth = false}}};

		renderer->begin_draw(extent, render::default_font_size, {});
		renderer->clear(black);
		verts->update_polygons(first);
		renderer->draw_vertices(verts);
		renderer->render();

		assert_equal(core::rgb(255, 0, 0), shade(8, 8), "the polygon is drawn");
		assert_equal(core::rgb(0, 0, 0), shade(40, 20), "and nothing beside it");

		verts->update_polygons(second);
		renderer->reset_damage();
		renderer->render();
		assert_equal(core::rgb(0, 0, 255), shade(8, 8), "a re-present shows the polygon updated since");

		verts->update_polygons({});
		renderer->reset_damage();
		renderer->render();
		assert_equal(core::rgb(0, 0, 0), shade(8, 8), "and an empty update draws nothing");
	}

	// A re-present limited to what changed repaints that and nothing else.
	void should_redraw_only_the_region_that_changed()
	{
		constexpr core::sizei extent{64, 32};
		const auto target = render::create_memory_present_target();
		const auto renderer = render::create_software_draw_context(nullptr, target, render::default_font_size);

		const auto shade = [&](const int x, const int y)
		{
			const auto buffer = target->acquire_buffer(extent);
			const auto* const p = buffer.bits + y * buffer.stride + x * 4;
			return core::rgb(p[2], p[1], p[0]);
		};

		const std::array<core::pointd, 4> left{{{4.0, 4.0}, {28.0, 4.0}, {28.0, 28.0}, {4.0, 28.0}}};
		const std::array<core::pointd, 4> right{{{36.0, 4.0}, {60.0, 4.0}, {60.0, 28.0}, {36.0, 28.0}}};
		const auto polygons = [&](const core::color c)
		{
			return std::array<render::vertex_polygon, 2>{{{left, {.fill = c}}, {right, {.fill = c}}}};
		};

		const auto verts = renderer->create_vertices();
		renderer->begin_draw(extent, render::default_font_size, {});
		renderer->clear(black);
		verts->update_polygons(polygons(red));
		renderer->draw_vertices(verts);
		renderer->render();
		assert_equal(core::rgb(255, 0, 0), shade(48, 16), "both are drawn");

		verts->update_polygons(polygons(blue));
		renderer->reset_damage({0, 0, 32, 32});
		renderer->render();
		assert_equal(core::rgb(0, 0, 255), shade(16, 16), "the region asked for is repainted");
		assert_equal(core::rgb(255, 0, 0), shade(48, 16), "and nothing past it");

		renderer->reset_damage();
		renderer->render();
		assert_equal(core::rgb(0, 0, 255), shade(48, 16), "until the whole is asked for");
	}

	void should_draw_nothing_for_an_outline_that_encloses_nothing()
	{
		const std::array<core::pointd, 3> line{{{4.0, 4.0}, {12.0, 12.0}, {20.0, 20.0}}};
		const std::array<core::pointd, 2> pair{{{4.0, 4.0}, {20.0, 4.0}}};

		const auto surface = draw_on_surface({24, 24}, [&](render::draw_context& dc)
		{
			dc.draw_polygon(line, {.fill = white, .border = white, .border_width = 2.0});
			dc.draw_polygon(pair, {.fill = white});
			dc.draw_polygon({}, {.fill = white});
		});
		assert_equal(true, surface != nullptr, "surface drawn");
		if (!surface) return;

		auto lit = 0;
		for (auto y = 0; y < 24; ++y)
			for (auto x = 0; x < 24; ++x)
				if (surface->get_pixel(x, y) != 0xff000000u) ++lit;

		assert_equal(0, lit, "nothing is drawn");
	}
}

void register_render_polygon_tests(test_registry& tests)
{
	tests.add("Should fill a concave polygon"s, should_fill_a_concave_polygon);
	tests.add("Should meet crisp neighbours without gap or overlap"s,
	          should_meet_crisp_neighbours_without_gap_or_overlap);
	tests.add("Should cover smooth edges by area"s, should_cover_smooth_edges_by_area);
	tests.add("Should hold a linear gradient at its ends"s, should_hold_a_linear_gradient_at_its_ends);
	tests.add("Should fade a radial gradient in its own hue"s, should_fade_a_radial_gradient_in_its_own_hue);
	tests.add("Should centre a border on the outline"s, should_centre_a_border_on_the_outline);
	tests.add("Should fill contours the same in tiles as whole"s, should_fill_contours_the_same_in_tiles_as_whole);
	tests.add("Should present vertex polygons updated after the scene"s,
	          should_present_vertex_polygons_updated_after_the_scene);
	tests.add("Should redraw only the region that changed"s, should_redraw_only_the_region_that_changed);
	tests.add("Should draw nothing for an outline that encloses nothing"s,
	          should_draw_nothing_for_an_outline_that_encloses_nothing);
}
