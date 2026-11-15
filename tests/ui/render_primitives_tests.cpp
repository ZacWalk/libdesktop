// libdesktop-ui's bubble drawing test, which paints a headless bubble popup into a CPU surface.

#include "libdesktop/render.h"
#include "libdesktop/render/software.h"
#include "libdesktop/ui.h"
#include "libdesktop/ui/test_support.h"
#include "test.h"

using namespace std::literals;

namespace
{
	class bubble_capture_frame final : public ui::null_frame
	{
	public:
		std::weak_ptr<ui::frame_host> content;
		core::recti bounds;
		bool visible = false;

		core::recti window_bounds() const override { return bounds; }
		void window_bounds(const core::recti value, const bool show) override
		{
			bounds = value;
			visible = show;
		}
		bool is_visible() const override { return visible; }
		void show(const bool value) override { visible = value; }

		void paint(render::draw_context& dc) const
		{
			if (const auto host = content.lock()) host->on_window_paint(dc);
		}
	};

	class bubble_capture_host final : public ui::bubble_host
	{
	public:
		ui::test::fake_measure_context measure;
		core::recti work{0, 0, 1000, 800};
		std::shared_ptr<bubble_capture_frame> popup;

		ui::frame_ptr create_bubble_popup(const ui::frame_host_weak_ptr& content) override
		{
			popup = std::make_shared<bubble_capture_frame>();
			popup->content = content;
			return popup;
		}
		render::measure_context* bubble_measure(const ui::frame_ptr&) override { return &measure; }
		core::recti bubble_work_area(core::recti) override { return work; }
	};

	// A bubble draws its own picture, the same on every backend: its body in the bubble's colour, an arrow out of the
	// side facing what it points at and no other, and a soft shadow round it that is gone by the popup's corners.
	void should_draw_a_bubble_with_its_arrow_and_shadow()
	{
		ui::style::install({});

		enum class edge
		{
			top,
			right,
			bottom,
			left
		};

		struct side
		{
			std::string_view name;
			core::recti anchor;
			bool horizontal;
			edge facing;
		};
		const side sides[] = {
			{"top", {300, 20, 340, 40}, false, edge::top},
			{"bottom", {300, 300, 340, 320}, false, edge::bottom},
			{"left", {20, 300, 60, 320}, true, edge::left},
			{"right", {300, 300, 340, 320}, true, edge::right},
		};

		for (const auto& s : sides)
		{
			const auto placement = ui::place_bubble({}, s.anchor, -1, s.horizontal, {0, 0, 1000, 800});
			const auto host = std::make_shared<bubble_capture_host>();
			const auto bubble = ui::create_drawn_bubble(host);
			bubble->show(std::make_shared<ui::element>(), s.anchor, -1, 300, s.horizontal);
			assert_equal(true, host->popup != nullptr, std::format("{} bubble popup", s.name));
			if (!host->popup) continue;
			assert_equal(true, host->popup->bounds == placement.bounds, std::format("{} bubble placement", s.name));

			const auto surface = std::make_shared<core::surface>();
			const auto context = render::create_surface_context(surface);
			assert_equal(true, context != nullptr, std::format("{} context", s.name));
			if (!context) continue;

			const auto extent = placement.bounds.extent();
			context->begin_draw(extent, render::default_font_size, {});
			context->colors.background = ui::style::colors.bubble_background;
			host->popup->paint(*context);
			assert_equal(false, context->render().failed, std::format("{} bubble render", s.name));
			if (surface->dimensions() != extent) continue;

			const auto body = surface->get_pixel(extent.cx / 2, extent.cy / 2);
			assert_equal(0xffu, body >> 24, std::format("{} body is opaque", s.name));

			// The middle of the margin round the body on each side: the arrow is the only body there.
			const auto arrow_in = [&](const edge e)
			{
				const auto horizontal_edge = e == edge::top || e == edge::bottom;
				const auto across = e == edge::top || e == edge::left ? 8
				                                                      : (horizontal_edge ? extent.cy : extent.cx) - 9;
				auto found = 0;
				for (auto along = 0; along < (horizontal_edge ? extent.cx : extent.cy); ++along)
				{
					const auto x = horizontal_edge ? along : across;
					const auto y = horizontal_edge ? across : along;
					if (surface->get_pixel(x, y) == body) ++found;
				}
				return found;
			};

			for (const auto e : {edge::top, edge::right, edge::bottom, edge::left})
			{
				assert_equal(e == s.facing, arrow_in(e) > 0,
				             std::format("{} bubble's arrow only where it points", s.name));
			}

			// Opposite the arrow there is only the shadow, a little way in from the popup's edge.
			const auto opposite = static_cast<edge>((static_cast<int>(s.facing) + 2) % 4);
			const auto x = opposite == edge::left ? 12 : opposite == edge::right ? extent.cx - 13 : extent.cx / 2;
			const auto y = opposite == edge::top ? 12 : opposite == edge::bottom ? extent.cy - 13 : extent.cy / 2;
			const auto shade = surface->get_pixel(x, y);
			assert_equal(true, shade >> 24 > 0 && shade >> 24 < 0xffu && (shade & 0xffffffu) == 0,
			             std::format("{} bubble's shadow is translucent black", s.name));
			assert_equal(0u, surface->get_pixel(0, 0) >> 24,
			             std::format("{} bubble's shadow is gone by its corners", s.name));
		}
	}
}

void register_ui_render_primitives_tests(test_registry& tests)
{
	tests.add("Should draw a bubble with its arrow and shadow"s, should_draw_a_bubble_with_its_arrow_and_shadow);
}
