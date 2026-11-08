// libdesktop-ui's bubble drawing test, which drives a headless frame through render primitives.

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

	void draw_bubble_reference(render::draw_context& dc, const core::recti bounds, const core::pointi focus,
	                          const core::color32 background)
	{
		constexpr int padding = 16;
		constexpr int radius = 8;

		dc.clear(core::color(0.0f, 0.0f, 0.0f, 0.0f));
		dc.draw_shadow({0, 0, bounds.width(), bounds.height()}, 32, 1.0f);

		const auto l = static_cast<float>(padding);
		const auto t = static_cast<float>(padding);
		const auto r = static_cast<float>(bounds.width() - padding);
		const auto b = static_cast<float>(bounds.height() - padding);
		const auto clr = core::color(background, 1.0f);
		const auto body = core::recti(core::round(l), core::round(t), core::round(r), core::round(b));
		const auto edge = 0.83333f * (radius + 2);
		const auto fill_grow = std::max(0, core::round(edge - radius));
		const auto undo_emphasis = [](const float value) { return (value - 0.05f) / 0.9f; };
		const core::color body_color(undo_emphasis(clr.r), undo_emphasis(clr.g), undo_emphasis(clr.b), clr.a);
		dc.draw_rounded_rect(body.inflate(-fill_grow), body_color, radius);

		const auto p = static_cast<float>(padding);
		std::array<core::pointd, 3> pointer;
		if (focus.y <= bounds.top)
		{
			const auto center = std::clamp(static_cast<float>(focus.x - bounds.left), l + 24.0f, r - 24.0f);
			pointer = {{{center - p, t}, {center, t - p}, {center + p, t}}};
		}
		else if (focus.x >= bounds.right)
		{
			const auto center = std::clamp(static_cast<float>(focus.y - bounds.top), t + 24.0f, b - 24.0f);
			pointer = {{{r, center - p}, {r + p, center}, {r, center + p}}};
		}
		else if (focus.y >= bounds.bottom)
		{
			const auto center = std::clamp(static_cast<float>(focus.x - bounds.left), l + 24.0f, r - 24.0f);
			pointer = {{{center + p, b}, {center, b + p}, {center - p, b}}};
		}
		else
		{
			const auto center = std::clamp(static_cast<float>(focus.y - bounds.top), t + 24.0f, b - 24.0f);
			pointer = {{{l, center + p}, {l - p, center}, {l, center - p}}};
		}
		dc.fill_polygon(std::span<const core::pointd>(pointer), clr);
	}

	core::surface_ptr make_bubble_test_shadow()
	{
		const auto shadow = std::make_shared<core::surface>();
		shadow->alloc(64, 64, core::texture_format::ARGB);
		for (auto y = 0; y < 64; ++y)
		{
			for (auto x = 0; x < 64; ++x)
			{
				const auto edge = std::max(std::abs(x - 31.5f), std::abs(y - 31.5f)) / 31.5f;
				shadow->set_pixel(x, y, core::rgba(0, 0, 0, core::round(std::clamp(edge, 0.0f, 1.0f) * 160.0f)));
			}
		}
		return shadow;
	}

	void should_draw_bubbles_with_generic_primitives()
	{
		ui::style::install({});
		const auto installed_decoder = render::decode_embedded_image;
		const core::scope_exit restore_decoder([installed_decoder] { render::decode_embedded_image = installed_decoder; });
		const auto shadow = make_bubble_test_shadow();
		render::decode_embedded_image = [shadow](const core::cspan) { return shadow; };

		struct side
		{
			std::string_view name;
			core::recti anchor;
			bool horizontal;
		};
		const side sides[] = {
			{"top", {300, 20, 340, 40}, false},
			{"bottom", {300, 300, 340, 320}, false},
			{"left", {20, 300, 60, 320}, true},
			{"right", {300, 300, 340, 320}, true},
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

			const auto ui_surface = std::make_shared<core::surface>();
			const auto ui_context = render::create_surface_context(ui_surface);
			const auto reference_surface = std::make_shared<core::surface>();
			const auto reference_context = render::create_surface_context(reference_surface);
			assert_equal(true, ui_context != nullptr && reference_context != nullptr, std::format("{} contexts", s.name));
			if (!ui_context || !reference_context) continue;

			const auto extent = placement.bounds.extent();
			ui_context->begin_draw(extent, render::default_font_size, {});
			ui_context->colors.background = ui::style::colors.bubble_background;
			host->popup->paint(*ui_context);
			assert_equal(false, ui_context->render().failed, std::format("{} UI bubble render", s.name));

			reference_context->begin_draw(extent, render::default_font_size, {});
			reference_context->colors.background = ui::style::colors.bubble_background;
			draw_bubble_reference(*reference_context, placement.bounds, placement.focus, ui::style::colors.bubble_background);
			assert_equal(false, reference_context->render().failed, std::format("{} reference render", s.name));

			bool identical = ui_surface->dimensions() == reference_surface->dimensions();
			if (identical)
			{
				for (auto y = 0; y < extent.cy && identical; ++y)
					for (auto x = 0; x < extent.cx; ++x)
						if (ui_surface->get_pixel(x, y) != reference_surface->get_pixel(x, y)) identical = false;
			}
			assert_equal(true, identical, std::format("{} bubble pixels", s.name));
		}
	}

}

void register_ui_render_primitives_tests(test_registry& tests)
{
	tests.add("Should draw bubbles with generic primitives"s, should_draw_bubbles_with_generic_primitives);
}
