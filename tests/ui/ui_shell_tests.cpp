// libdesktop-ui's shell tests: the keys a realtime application reads, its step clock, the canvas and surface
// elements, and a capture drawn with no window.

#include "libdesktop/ui.h"
#include "libdesktop/ui/shell.h"
#include "libdesktop/ui/test_support.h"
#include "test.h"

using namespace std::literals;

namespace
{
	// A frame that records what was asked of it, as a window would be asked to paint or re-present.
	class recording_frame final : public ui::null_frame
	{
	public:
		std::vector<core::recti> invalidated;
		std::vector<core::recti> redrawn;

		void invalidate(const core::recti bounds = {}, bool = false) override { invalidated.push_back(bounds); }
		void redraw(const core::recti bounds = {}) override { redrawn.push_back(bounds); }
	};

	core::surface_ptr picture(const core::sizei extent, const uint32_t pixel)
	{
		auto result = std::make_shared<core::surface>();
		if (!result->alloc(extent, core::texture_format::ARGB)) return {};
		for (auto y = 0; y < extent.cy; ++y)
			for (auto x = 0; x < extent.cx; ++x) result->set_pixel(x, y, pixel);
		return result;
	}

	void should_hold_keys_between_their_press_and_release()
	{
		ui::key_input keys;
		keys.down('A');
		assert_equal(true, keys.held('A'), "a key goes down");
		assert_equal(true, keys.pressed('A'), "pressed in the step it went down in");

		keys.next_step();
		keys.down('A');
		assert_equal(true, keys.held('A'), "still held through a repeat");
		assert_equal(false, keys.pressed('A'), "which is no new press");

		keys.up('A');
		assert_equal(false, keys.held('A'), "let go");
		assert_equal(true, keys.released('A'), "released in the step it went up in");

		keys.next_step();
		assert_equal(false, keys.released('A'), "and not after");
	}

	// A tap shorter than a step is both pressed and released in one, so a step still sees it.
	void should_keep_a_tap_shorter_than_a_step()
	{
		ui::key_input keys;
		keys.down(platform::keys::SPACE);
		keys.up(platform::keys::SPACE);
		assert_equal(true, keys.pressed(platform::keys::SPACE), "the tap is a press");
		assert_equal(true, keys.released(platform::keys::SPACE), "and a release");
		assert_equal(false, keys.held(platform::keys::SPACE), "and nothing held");
	}

	void should_let_go_of_every_key_when_the_keyboard_goes()
	{
		ui::key_input keys;
		keys.down('W');
		keys.down('S');
		keys.release_all();
		assert_equal(false, keys.held('W'), "losing the keyboard lets go of every key");
		assert_equal(false, keys.held('S'), "both of them");
		assert_equal(true, keys.released('W'), "as a release");
		assert_equal(false, keys.held(0x1000), "and a code past the table is never held");
	}

	void should_step_at_a_fixed_rate()
	{
		ui::step_clock clock(50);
		assert_near(0.02, clock.step_seconds(), 1e-12, "a step is a fiftieth of a second");
		assert_equal(0, clock.advance(0.015), "short of a step, nothing is due");
		assert_equal(1, clock.advance(0.010), "the rest of it makes one");
		assert_equal(2, clock.advance(0.040), "two steps' time makes two");
		assert_equal(1, clock.advance(0.020), "a step exactly is one, rounding aside");
		assert_equal(5, clock.advance(10.0), "a stall makes no more than five");
		assert_equal(0, clock.advance(0.0), "and what it held back is dropped");

		assert_equal(0, clock.advance(0.015), "three quarters of a step waits");
		clock.rate(100.0);
		assert_near(0.01, clock.step_seconds(), 1e-12, "a new rate makes a hundredth of a second a step");
		assert_equal(1, clock.advance(0.0), "and the time that waited counts towards it");
		assert_equal(1, clock.advance(0.005), "with what is left over");

		const ui::step_clock fractional(12.5);
		assert_near(0.08, fractional.step_seconds(), 1e-12, "a rate need not be whole");
	}

	void should_fit_a_picture_by_whole_multiples()
	{
		const auto fitted = ui::fit_picture({320, 200}, {0, 0, 1000, 700}, true, 1.0);
		assert_equal(960, fitted.width(), "three times across fits");
		assert_equal(600, fitted.height(), "and three times down");
		assert_equal(20, fitted.left, "centred across");
		assert_equal(50, fitted.top, "and down");

		const auto stretched = ui::fit_picture({320, 200}, {0, 0, 1000, 700}, false, 1.0);
		assert_equal(1000, stretched.width(), "without multiples it fills the width");
		assert_equal(625, stretched.height(), "keeping its shape");

		const auto tall = ui::fit_picture({320, 200}, {0, 0, 1000, 800}, true, 1.2);
		assert_equal(960, tall.width(), "tall pixels keep a whole multiple across");
		assert_equal(800, tall.height(), "and are nearest the aspect down");
		assert_equal(true, ui::fit_picture({}, {0, 0, 10, 10}, true, 1.0).is_empty(), "no picture, no place");
	}

	void should_draw_a_canvas_clipped_to_its_bounds()
	{
		core::recti drawn;
		core::recti clip;
		const auto canvas = std::make_shared<ui::canvas_element>([&](render::draw_context& dc, const core::recti bounds)
		{
			drawn = bounds;
			clip = dc.clip_bounds();
			dc.draw_rect(bounds, core::color(1.0f, 0.0f, 0.0f, 1.0f));
		});

		ui::test::fake_draw_context dc;
		canvas->layout(dc, {10, 20, 110, 70});
		canvas->render(dc, {5, 5});
		assert_equal(true, drawn == core::recti(15, 25, 115, 75), "the callback draws where the canvas sits");
		assert_equal(true, clip == core::recti(15, 25, 115, 75), "clipped to it");
		assert_equal(true, dc.clip_bounds() == core::recti(0, 0, 1024, 768), "and the clip is restored after");
	}

	// A picture the size of the last re-presents the window's scene over itself alone; one of a new size
	// asks for the scene to be painted again.
	void should_present_a_new_picture_without_painting_again()
	{
		const auto element = std::make_shared<ui::surface_element>();
		const auto frame = std::make_shared<recording_frame>();
		ui::test::recording_draw_context dc;
		element->layout(dc, {0, 0, 200, 100});

		const auto first = picture({100, 50}, 0xff112233u);
		element->present(first, frame);
		assert_equal(1, static_cast<int>(frame->invalidated.size()), "the first picture is painted");

		element->render(dc, {});
		assert_equal(1, static_cast<int>(dc.textures.size()), "and drawn");
		assert_equal(true, dc.textures.back().bounds == core::recti(0, 0, 200, 100), "fitted to the bounds");
		assert_equal(true, dc.textures.back().texture->surface == first, "from its pixels");

		const auto second = picture({100, 50}, 0xff445566u);
		element->present(second, frame);
		assert_equal(1, static_cast<int>(frame->invalidated.size()), "the next of its size is not painted");
		assert_equal(1, static_cast<int>(frame->redrawn.size()), "but re-presented");
		assert_equal(true, frame->redrawn.back() == core::recti(0, 0, 200, 100), "over the picture alone");
		assert_equal(true, dc.textures.back().texture->surface == second, "with its pixels in the texture");

		const auto mapped = element->picture_point({150, 75});
		assert_equal(true, mapped.has_value(), "a point on the picture maps onto it");
		assert_equal(75, mapped ? mapped->x : -1, "at its own scale across");
		assert_equal(37, mapped ? mapped->y : -1, "and down");
		assert_equal(true, !element->picture_point({300, 75}).has_value(), "and one beside it does not");

		const auto lost = dc.textures.back().texture;
		element->dispatch_event({ui::element_event_type::free_graphics_resources, nullptr});
		element->render(dc, {});
		assert_equal(true, dc.textures.back().texture != lost, "a lost device's texture is made again as it paints");
		assert_equal(true, dc.textures.back().texture->surface == second, "holding the picture shown");

		element->present(picture({64, 64}, 0xff000000u), frame);
		assert_equal(2, static_cast<int>(frame->invalidated.size()), "a new size is painted again");
	}

	void should_capture_an_element_with_no_window()
	{
		const auto canvas = std::make_shared<ui::canvas_element>([](render::draw_context& dc, const core::recti bounds)
		{
			const std::array<core::pointd, 3> triangle{{{bounds.left + 4.0, bounds.top + 4.0},
			                                            {bounds.right - 4.0, bounds.top + 4.0},
			                                            {bounds.left + 4.0, bounds.bottom - 4.0}}};
			dc.draw_polygon(triangle, {.fill = core::color(0.0f, 1.0f, 0.0f, 1.0f), .smooth = false});
		});

		const auto surface = ui::render_to_surface(canvas, {64, 48}, core::color(0.0f, 0.0f, 0.0f, 1.0f));
		assert_equal(true, surface != nullptr, "a capture is drawn");
		if (!surface) return;
		assert_equal(true, surface->dimensions() == core::sizei{64, 48}, "at the size asked for");
		assert_equal(0xff00ff00u, surface->get_pixel(10, 10), "with what the element draws");
		assert_equal(0xff000000u, surface->get_pixel(58, 42), "on its background");
	}

	// A shell given a recording frame in place of a window, which no test can make.
	class pointer_shell final : public ui::shell
	{
	public:
		pointer_shell() : shell(ui::shell_options{}) {}

		ui::element_ptr make_content() override { return {}; }
		const ui::frame_ptr frame() const override { return recorder; }

		std::shared_ptr<recording_frame> recorder = std::make_shared<recording_frame>();
	};

	void should_not_paint_again_for_a_pointer_over_nothing()
	{
		const auto shell = std::make_shared<pointer_shell>();
		ui::test::fake_measure_context mc;
		shell->on_window_layout(mc, {320, 200}, false);

		shell->on_mouse_move({40, 50}, false);
		shell->on_mouse_move({41, 52}, false);
		assert_equal(0, static_cast<int>(shell->recorder->invalidated.size()),
		             "moving over content with nothing to answer the pointer paints nothing again");
		assert_equal(true, shell->_hover, "though the pointer is over the window");

		shell->on_mouse_move({400, 50}, false);
		assert_equal(false, shell->_hover, "and is not once it leaves the laid out window");

		shell->on_mouse_move({40, 50}, false);
		shell->on_mouse_leave({40, 50});
		assert_equal(0, static_cast<int>(shell->recorder->invalidated.size()), "nor does it leaving the window");
		assert_equal(false, shell->_hover, "which it is then not over");
	}
}

void register_ui_shell_tests(test_registry& tests)
{
	tests.add("Should hold keys between their press and release"s, should_hold_keys_between_their_press_and_release);
	tests.add("Should keep a tap shorter than a step"s, should_keep_a_tap_shorter_than_a_step);
	tests.add("Should let go of every key when the keyboard goes"s, should_let_go_of_every_key_when_the_keyboard_goes);
	tests.add("Should step at a fixed rate"s, should_step_at_a_fixed_rate);
	tests.add("Should fit a picture by whole multiples"s, should_fit_a_picture_by_whole_multiples);
	tests.add("Should draw a canvas clipped to its bounds"s, should_draw_a_canvas_clipped_to_its_bounds);
	tests.add("Should present a new picture without painting again"s,
	          should_present_a_new_picture_without_painting_again);
	tests.add("Should capture an element with no window"s, should_capture_an_element_with_no_window);
	tests.add("Should not paint again for a pointer over nothing"s, should_not_paint_again_for_a_pointer_over_nothing);
}
