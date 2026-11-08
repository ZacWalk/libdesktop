// This file is part of libdesktop, a C++20 platform layer for desktop applications
// Copyright 2026  Zac Walker
// 
// This program is free software; you can redistribute it and / or modify it
// under the terms of the LGPL License either version 2.1 or later.
// License details are available at https://www.gnu.org/licenses/lgpl-2.1.html
// This program is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY

// Purpose: The drawn controls and menu, driven without a window: a host stands in for the window layer,
// text is 8 pixels a byte and 20 high so positions are arithmetic, and the models the controls edit
// with - moved here with their tests from Diffractor - are checked on their own.

#include "libdesktop/ui.h"
#include "libdesktop/ui/controls.h"
#include "libdesktop/render/software.h"
#include "libdesktop/ui/elements.h"
#include "libdesktop/ui/test_support.h"
#include "test.h"

using namespace std::literals;

#include <set>

namespace
{
	class fixed_layout final : public render::text_layout
	{
	public:
		std::string text;
		int* updates = nullptr;

		explicit fixed_layout(int& update_count) : updates(&update_count)
		{
		}

		void update(const std::string_view text_in, render::text_style) override
		{
			text = text_in;
			++*updates;
		}

		core::sizei measure_text(const int cx, int) override
		{
			return {std::min(cx, static_cast<int>(text.size()) * 8), 20};
		}

		std::vector<int> offset_xs(const std::vector<size_t>& utf8_offsets, int, int) override
		{
			std::vector<int> result;
			result.reserve(utf8_offsets.size());
			for (const auto offset : utf8_offsets) result.emplace_back(static_cast<int>(offset) * 8);
			return result;
		}
	};

	// Every byte 8 pixels wide and every line 20 high.
	class fixed_measure final : public render::measure_context
	{
	public:
		int layout_updates = 0;
		int prefix_measures = 0;

		core::sizei measure_text(const std::string_view text, render::font_face, const render::text_style style,
		                   const int cx, int) override
		{
			++prefix_measures;
			const auto natural = static_cast<int>(text.size()) * 8;
			const auto wraps = style == render::text_style::multiline ||
				style == render::text_style::multiline_center;
			const auto lines = wraps && cx > 0 ? std::max(1, (natural + cx - 1) / cx) : 1;
			return {std::min(std::max(cx, 1), natural), 20 * lines};
		}

		int text_line_height(render::font_face) override { return 20; }

		render::text_layout_ptr create_text_layout(render::font_face) override
		{
			return std::make_shared<fixed_layout>(layout_updates);
		}
	};

	// A popup the menu draws in, recording what the window layer would be asked.
	class recording_frame final : public ui::null_frame
	{
	public:
		core::recti bounds;
		bool visible = false;
		bool destroyed = false;
		int invalidations = 0;

		void destroy() override
		{
			destroyed = true;
			visible = false;
		}

		bool is_visible() const override { return visible; }
		core::recti window_bounds() const override { return bounds; }

		void window_bounds(const core::recti b, const bool v) override
		{
			bounds = b;
			visible = v;
		}

		void show(const bool s) override { visible = s; }
		void invalidate(core::recti, bool) override { ++invalidations; }
		bool is_enabled() const override { return true; }
		bool is_occluded() const override { return false; }
		core::pointi cursor_location() override { return {}; }
	};

	// The window's host: its tree, each root at where its content sits, and what it is told.
	class recording_listener final : public ui::frame_host
	{
	public:
		std::vector<std::pair<ui::element_ptr, core::pointi>> roots;
		std::vector<std::pair<bool, ui::control_ptr>> focus;
		std::vector<ui::command_ptr> hovered;

		void visit_elements(const std::function<void(const ui::element_ptr&, core::pointi)>& visit) override
		{
			const auto copy = roots;
			for (const auto& [e, offset] : copy) visit(e, offset);
		}

		void on_window_layout(render::measure_context&, core::sizei, bool) override {}
		void on_window_paint(render::draw_context&) override {}

		void focus_changed(const bool has_focus, const ui::control_ptr& child) override
		{
			focus.emplace_back(has_focus, child);
		}

		void command_hover(const ui::command_ptr& c, core::recti) override
		{
			hovered.push_back(c);
		}
	};

	// The window layer, as the controls see it: client pixels map to the screen 1000 across and 500 down.
	class test_host final : public ui::control_host
	{
	public:
		fixed_measure fixed;
		render::measure_context* metrics = &fixed;
		ui::input_manager_ptr input;
		std::shared_ptr<recording_listener> events = std::make_shared<recording_listener>();
		std::vector<core::recti> invalidated;
		std::vector<std::vector<ui::command_ptr>> menus;
		std::vector<core::recti> menu_bounds;
		std::vector<std::shared_ptr<recording_frame>> popups;
		bool keyboard = false;
		bool captured = false;
		bool dialog = false;
		bool closed = false;
		bool cancelled = false;
		bool typing = false;
		bool caption = false;
		core::recti caret;

		test_host() : input(std::make_shared<ui::input_manager>(this))
		{
		}

		~test_host() override
		{
			input->detach();
		}

		// A control at the root of the window's tree, found by its input manager as a layout finds it.
		template <class T>
		std::shared_ptr<T> add(std::shared_ptr<T> c)
		{
			events->roots.emplace_back(c, core::pointi{});
			input->layout();
			return c;
		}

		ui::edit_ptr create_edit(const ui::edit_styles& styles, const std::string_view text,
		                         std::function<void(const std::string&)> changed)
		{
			return add(ui::make_edit(styles, text, std::move(changed)));
		}

		ui::trackbar_ptr create_slider(const int min, const int max, std::function<void(int, bool)> changed)
		{
			return add(ui::make_slider(min, max, std::move(changed)));
		}

		ui::toolbar_ptr create_toolbar(const ui::toolbar_styles& styles, const std::vector<ui::command_ptr>& buttons)
		{
			return add(ui::make_toolbar(styles, buttons));
		}

		ui::button_ptr create_button(const icon_index icon, const std::string_view title, const std::string_view details,
		                             std::function<void()> invoke, const bool default_button)
		{
			return add(ui::make_button(icon, title, details, std::move(invoke), default_button));
		}

		ui::button_ptr create_check_button(const bool val, const std::string_view text, const bool is_radio,
		                                   std::function<void(bool)> changed, const int radio_group)
		{
			return add(ui::make_check_button(val, text, is_radio, std::move(changed), radio_group));
		}

		ui::date_time_control_ptr create_date_time_control(const core::date_t val, std::function<void(core::date_t)> changed,
		                                                   const bool include_time)
		{
			return add(ui::make_date_time_control(val, std::move(changed), include_time));
		}

		// Where a layout puts a control, and whether it is shown; then found there.
		void place(const ui::control_ptr& c, const core::recti bounds, const bool visible)
		{
			c->is_visible(visible);
			c->layout(fixed, bounds);
			input->layout();
		}

		// The tree, drawn as the window's host draws it.
		void paint(render::draw_context& dc) const
		{
			for (const auto& [e, offset] : events->roots)
			{
				if (e->is_visible()) e->render(dc, offset);
			}
		}

		// Moves the content of the window's tree, as a scrolled region does; then found there.
		void scroll(const int dx, const int dy)
		{
			for (auto& root : events->roots) root.second = root.second + core::pointi{dx, dy};
			input->layout();
		}

		static constexpr core::pointi screen_origin{1000, 500};

		render::measure_context* measure() override { return metrics; }
		void invalidate(const core::recti bounds) override { invalidated.push_back(bounds); }
		bool has_keyboard_focus() const override { return keyboard; }

		void take_keyboard_focus() override
		{
			if (keyboard) return;
			keyboard = true;
			input->keyboard_focus(true);
		}

		void lose_keyboard_focus()
		{
			keyboard = false;
			input->keyboard_focus(false);
		}

		void capture_pointer(const bool capture) override { captured = capture; }
		void set_cursor(platform::cursor) override {}
		core::pointi client_to_screen(const core::pointi loc) const override { return loc + screen_origin; }

		void track_menu(const core::recti screen_bounds, const std::vector<ui::command_ptr>& commands) override
		{
			menus.push_back(commands);
			menu_bounds.push_back(screen_bounds);
		}

		ui::frame_ptr create_popup(const ui::frame_host_weak_ptr&, const render::color_style&) override
		{
			auto popup = std::make_shared<recording_frame>();
			popups.push_back(popup);
			return popup;
		}

		std::shared_ptr<ui::frame_host> listener() const override { return events; }

		// The window itself took the keyboard back from a control.
		int keyboard_to_frame_count = 0;
		void keyboard_to_frame() override { ++keyboard_to_frame_count; }

		void text_input(const bool active, const core::recti c) override
		{
			typing = active;
			caret = c;
		}

		void caret_timer(bool, uint32_t) override {}
		bool is_dialog() const override { return dialog; }

		void close(const bool cancel) override
		{
			closed = true;
			cancelled = cancel;
		}

		bool presents_caption_button() const override { return caption; }

		void type(const std::string_view text) const
		{
			input->text_input(text);
		}

		bool key(const char32_t k, const platform::key_state keys = {}) const
		{
			return input->key_down(static_cast<int>(k), keys);
		}

		void click(const core::pointi loc, const platform::key_state keys = {}) const
		{
			input->mouse_down(loc, keys);
			input->mouse_up(loc, keys);
		}
	};

	platform::key_state shift_key()
	{
		platform::key_state keys;
		keys.shift = true;
		return keys;
	}

	platform::key_state control_key()
	{
		platform::key_state keys;
		keys.control = true;
		return keys;
	}

	std::string command_texts(const std::vector<ui::command_ptr>& commands)
	{
		std::string result;

		for (const auto& c : commands)
		{
			if (!result.empty()) result += "|";
			result += c ? c->text : "-";
		}

		return result;
	}

	// Controls or a window painted into the software canvas, read back a pixel at a time as 0xRRGGBB.
	class painted_canvas
	{
		render::software_present_target_ptr _target = render::create_memory_present_target();
		render::draw_context_device_ptr _ctx;
		core::sizei _extent;
		render::software_buffer _buffer;

	public:
		explicit painted_canvas(const core::sizei extent) : _extent(extent)
		{
			ui::style::install({});
			_ctx = render::create_software_draw_context(render::create_text_engine(render::default_font_size), _target,
			                                        render::default_font_size);
		}

		// A set's controls, over a host's colours.
		painted_canvas(test_host& host, const core::sizei extent, const render::color_style& colors) : painted_canvas(extent)
		{
			_ctx->colors = colors;
			host.metrics = _ctx.get();
		}

		void paint(const test_host& host)
		{
			_ctx->begin_draw(_extent, render::default_font_size, {});
			_ctx->clear(core::color(_ctx->colors.background, 1.0f));
			host.paint(*_ctx);
			_ctx->render();
			_buffer = _target->acquire_buffer(_extent);
		}

		// A window, which paints the whole of itself.
		void paint(ui::frame_host& window)
		{
			_ctx->begin_draw(_extent, render::default_font_size, {});
			window.on_window_paint(*_ctx);
			_ctx->render();
			_buffer = _target->acquire_buffer(_extent);
		}

		uint32_t pixel(const int x, const int y) const
		{
			const auto* const p = _buffer.bits + y * _buffer.stride + x * 4;
			return static_cast<uint32_t>(p[0]) | static_cast<uint32_t>(p[1]) << 8 | static_cast<uint32_t>(p[2]) << 16;
		}

		static uint32_t rgb(const core::color32 c)
		{
			return core::get_r(c) << 16 | core::get_g(c) << 8 | core::get_b(c);
		}

		int count(const core::recti r, const uint32_t rgb) const
		{
			auto result = 0;
			for (auto y = r.top; y < r.bottom; ++y)
			{
				for (auto x = r.left; x < r.right; ++x) if (pixel(x, y) == rgb) ++result;
			}
			return result;
		}
	};

	// The largest difference in one channel: how far apart two colours read.
	int channel_distance(const uint32_t a, const uint32_t b)
	{
		auto result = 0;
		for (const auto shift : {0, 8, 16})
		{
			result = std::max(result, std::abs(static_cast<int>(a >> shift & 0xff) - static_cast<int>(b >> shift & 0xff)));
		}
		return result;
	}
}

///////////////////////////////////////////////////////////////////////////////////////////////////
// Models
///////////////////////////////////////////////////////////////////////////////////////////////////

static void should_edit_single_line_text()
{
	ui::text_edit_model edit;
	edit.text("Hello world");
	edit.select(6, 11);
	edit.insert("Diffractor");
	assert_equal_strict("Hello Diffractor", edit.text(), "replace selection");

	edit.move_left(true);
	assert_equal(true, edit.has_selection(), "shift-left selects");
	edit.move_left();
	assert_equal(false, edit.has_selection(), "left collapses selection");
	assert_equal(15_z, edit.caret(), "left collapses to selection start");

	edit.text("A\u00e9B");
	edit.move_left();
	edit.backspace();
	assert_equal_strict("AB", edit.text(), "backspace removes one UTF-8 code point");
	assert_equal(1_z, edit.caret(), "caret remains on UTF-8 boundary");

	edit.select_all();
	edit.insert("one\r\ntwo\nthree");
	assert_equal_strict("one two three", edit.text(), "paste is normalized to one line");

	edit.text("keep selection");
	edit.select(5, 14);
	assert_equal(false, edit.paste_from_clipboard([]() -> std::optional<std::string> { return std::nullopt; }),
	             "unavailable clipboard text is not pasted");
	assert_equal_strict("keep selection", edit.text(), "unavailable paste preserves text");
	assert_equal(true, edit.has_selection(), "unavailable paste preserves selection");
	assert_equal(true, edit.paste_from_clipboard([] { return std::optional<std::string>{""}; }),
	             "valid empty clipboard text is pasteable");
	assert_equal_strict("keep ", edit.text(), "valid empty paste erases selection");
	edit.undo();
	assert_equal_strict("keep selection", edit.text(), "empty paste remains undoable");

	edit.select(5, 14);
	assert_equal(false, edit.cut_to_clipboard([](std::string_view) { return false; }),
	             "failed cut publication is reported");
	assert_equal_strict("keep selection", edit.text(), "failed cut preserves text");
	assert_equal(true, edit.has_selection(), "failed cut preserves selection");
	std::string cut_text;
	assert_equal(true, edit.cut_to_clipboard([&cut_text](const std::string_view value)
	{
		cut_text = value;
		return true;
	}), "successful cut is reported");
	assert_equal_strict("selection", cut_text, "cut publishes selection before erasing");
	assert_equal_strict("keep ", edit.text(), "successful cut erases selection");

	edit.text("one two three");
	edit.move_word_left();
	assert_equal(8_z, edit.caret(), "control-left moves to previous word");
	edit.backspace_word();
	assert_equal_strict("one three", edit.text(), "control-backspace removes previous word");
	edit.undo();
	assert_equal_strict("one two three", edit.text(), "undo restores word deletion");
	edit.redo();
	assert_equal_strict("one three", edit.text(), "redo reapplies word deletion");

	edit.text("alpha beta");
	edit.select_word(7);
	assert_equal_strict("beta", edit.selected_text(), "double-click model selects a word");
	edit.erase_selection();
	assert_equal_strict("alpha ", edit.text(), "cut removes selected word");
	edit.undo();
	assert_equal_strict("alpha beta", edit.text(), "undo restores cut text");

	edit.begin_edit();
	edit.select_all();
	edit.insert("changed");
	edit.cancel_edit();
	assert_equal_strict("alpha beta", edit.text(), "cancel restores edit-start value");
}

// A multi-line model keeps the breaks it is given, as one kind, and knows where each paragraph runs.
static void should_keep_line_breaks_in_multi_line_text()
{
	ui::text_edit_model edit(true);
	edit.text("one\r\ntwo\rthree");
	assert_equal_strict("one\ntwo\nthree", edit.text(), "every break is kept as a line feed");

	edit.move_to(5);
	assert_equal(4_z, edit.paragraph_start(edit.caret()), "the paragraph starts after the break before it");
	assert_equal(7_z, edit.paragraph_end(edit.caret()), "and ends at the break after it");

	edit.insert("\r\n");
	assert_equal_strict("one\nt\nwo\nthree", edit.text(), "typed breaks are line feeds too");
}

static void should_edit_a_segmented_date()
{
	using seg = ui::date_segment;

	// Order and separators follow the display locale, so the field is built from it rather than
	// from one hard-coded arrangement.
	{
		const ui::date_edit_model dmy(ui::date_field_order::dmy, false);
		assert_equal(true, dmy.segments() == std::vector{seg::day, seg::month, seg::year}, "day first");

		const ui::date_edit_model mdy(ui::date_field_order::mdy, false);
		assert_equal(true, mdy.segments() == std::vector{seg::month, seg::day, seg::year}, "month first");

		const ui::date_edit_model ymd(ui::date_field_order::ymd, true);
		assert_equal(true, ymd.segments() == std::vector{
			             seg::year, seg::month, seg::day, seg::hour, seg::minute, seg::second
		             }, "year first, and a clock is always most significant first");
	}

	ui::date_edit_model m(ui::date_field_order::dmy, true);
	m.value(core::date_t(2025, 8, 16, 18, 11, 56));

	// Left and Right walk the segments and stop at the ends, so the arrows stay inside the field and
	// Tab has a false answer to act on.
	assert_equal(true, m.active_segment() == seg::day, "the field opens on its first segment");
	assert_equal(false, m.move(-1), "and there is nothing to the left of it");
	assert_equal(true, m.move(1), "Right moves on");
	assert_equal(true, m.active_segment() == seg::month, "to the next segment in locale order");
	m.active(5);
	assert_equal(false, m.move(1), "and stops at the last segment rather than wrapping");

	// Every segment is padded to its width, so the field does not change shape as it is edited.
	assert_equal_strict("16", m.text(0), "day");
	assert_equal_strict("08", m.text(1), "month is padded");
	assert_equal_strict("2025", m.text(2), "year is four digits");
	assert_equal_strict("18", m.text(3), "hour");
	assert_equal_strict("11", m.text(4), "minute");
	assert_equal_strict("56", m.text(5), "second");

	// One step is one unit of the active segment applied to the whole date, so a bound rolls into
	// the neighbour rather than wrapping the segment on its own. That is what a clock does.
	m.value(core::date_t(2025, 12, 31, 23, 59, 59));
	m.active(5);
	m.step(1);
	assert_equal(core::date_t(2026, 1, 1, 0, 0, 0), m.value(), "a second rolls the whole date over");

	m.value(core::date_t(2025, 12, 31, 0, 0, 0));
	m.active(1);
	m.step(1);
	assert_equal(core::date_t(2026, 1, 31, 0, 0, 0), m.value(), "December rolls into January of the next year");

	m.value(core::date_t(2026, 1, 1, 0, 0, 0));
	m.active(1);
	m.step(-1);
	assert_equal(core::date_t(2025, 12, 1, 0, 0, 0), m.value(), "and back the other way");

	// A month step can land on a day the new month does not have. 31 April is not a date, so the
	// day is clamped rather than the whole value being refused or silently rolled into May.
	m.value(core::date_t(2025, 1, 31, 0, 0, 0));
	m.active(1);
	m.step(1);
	assert_equal(core::date_t(2025, 2, 28, 0, 0, 0), m.value(), "31 January steps to the end of February");

	m.value(core::date_t(2024, 1, 31, 0, 0, 0));
	m.active(1);
	m.step(1);
	assert_equal(core::date_t(2024, 2, 29, 0, 0, 0), m.value(), "and a leap year has the extra day");

	// Stepping the year off a leap day does the same.
	m.value(core::date_t(2024, 2, 29, 0, 0, 0));
	m.active(2);
	m.step(1);
	assert_equal(core::date_t(2025, 2, 28, 0, 0, 0), m.value(), "29 February steps to the 28th");

	// Digits type left to right and the segment advances when it can hold no more, so a date is
	// typed straight through without reaching for a separator key.
	m.value(core::date_t(2025, 8, 16, 0, 0, 0));
	m.active(0);
	m.type_digit('0');
	m.type_digit('4');
	assert_equal(4, m.field(seg::day), "two digits fill the day");
	assert_equal(true, m.active_segment() == seg::month, "and the field moves on by itself");

	m.type_digit('7');
	assert_equal(7, m.field(seg::month), "a digit no second digit could follow finishes the segment");
	assert_equal(true, m.active_segment() == seg::year, "so that one advances too");

	m.type_digit('1');
	m.type_digit('9');
	m.type_digit('9');
	m.type_digit('9');
	assert_equal(1999, m.field(seg::year), "a year takes four");
	assert_equal(core::date_t(1999, 7, 4, 0, 0, 0), m.value(), "and the typed date is the value");

	// A value the segment cannot hold starts again from that digit rather than being refused, which
	// is what a user retyping a wrong month means by it.
	m.value(core::date_t(2025, 8, 16, 0, 0, 0));
	m.active(1);
	m.type_digit('1');
	assert_equal(1, m.field(seg::month), "1 could still become 10, 11 or 12");
	m.type_digit('9');
	assert_equal(9, m.field(seg::month), "19 is not a month, so 9 starts the segment again");

	// Only the month has a name, so it is the only segment worth a drop menu.
	m.active(1);
	assert_equal(true, m.active_has_menu(), "the month offers its names");
	m.active(0);
	assert_equal(false, m.active_has_menu(), "a day is a number faster to type than to pick");
	m.active(2);
	assert_equal(false, m.active_has_menu(), "and so is a year");
}

static void should_replace_the_full_trailing_spelling_word()
{
	const auto invalid = [](const std::string_view word)
	{
		return word != "teh" && word != "x";
	};

	const auto replace_first = [](std::string text, const std::vector<ui::spelling_range>& ranges)
	{
		assert_equal(1_z, ranges.size(), "one misspelling is found");
		text.replace(ranges.front().start, ranges.front().end - ranges.front().start, "the");
		return text;
	};

	assert_equal_strict("the", replace_first("teh", ui::spelling_ranges("teh", invalid)),
	                    "a final word includes its last character");
	assert_equal_strict("prefix the", replace_first("prefix teh", ui::spelling_ranges("prefix teh", invalid)),
	                    "surrounding prefix text is untouched");
	assert_equal_strict("the ", replace_first("teh ", ui::spelling_ranges("teh ", invalid)),
	                    "a word followed by whitespace keeps the separator");

	const auto one = ui::spelling_ranges("x", invalid);
	assert_equal(1_z, one.size(), "a one-character final word is found");
	assert_equal(0_z, one.front().start, "one-character start");
	assert_equal(1_z, one.front().end, "one-character end remains exclusive");

	const auto accented = ui::spelling_ranges("caf\xc3\xa9 teh", invalid);
	assert_equal(1_z, accented.size(), "a word in another script is one word");
	assert_equal(6_z, accented.front().start, "offsets are UTF-8 bytes");
}

namespace
{
	class listed_dictionary final : public ui::spelling_dictionary
	{
	public:
		mutable std::vector<std::string> asked;
		std::vector<std::string> added;

		void queue_load() override {}

		bool is_word_valid(const std::string_view word) const override
		{
			asked.emplace_back(word);
			return word != "teh" || std::ranges::find(added, word) != added.end();
		}

		std::vector<std::string> suggest(const std::string_view word) const override
		{
			return word == "teh" ? std::vector<std::string>{"the", "ten"} : std::vector<std::string>{};
		}

		void add_word(const std::string_view word, std::function<void()> done) override
		{
			added.emplace_back(word);
			if (done) done();
		}
	};
}

// The control checks words against the dictionary it is given and knows of no other: the application
// supplies spell checking, and without it nothing is underlined.
static void should_check_edit_words_against_the_supplied_dictionary()
{
	listed_dictionary dictionary;
	const auto unknown = ui::unknown_words("the teh cat", &dictionary);

	assert_equal(1_z, unknown.size(), "the word the dictionary rejects is underlined");
	assert_equal_strict("teh", unknown.front().word, "which word");
	assert_equal(4_z, unknown.front().start, "where it starts");
	assert_equal(7_z, unknown.front().end, "where it ends");
	assert_equal(3_z, dictionary.asked.size(), "every word is asked of the dictionary");
	assert_equal(true, ui::unknown_words("the teh cat", nullptr).empty(), "without a dictionary nothing is underlined");
}

///////////////////////////////////////////////////////////////////////////////////////////////////
// The edit
///////////////////////////////////////////////////////////////////////////////////////////////////

static void should_type_into_the_focused_edit_and_report_each_change()
{
	test_host host;
	std::vector<std::string> changes;
	const auto edit = host.create_edit({}, {}, [&changes](const std::string& text) { changes.push_back(text); });
	host.place(edit, {0, 0, 200, 28}, true);

	host.type("lost");
	assert_equal(true, changes.empty(), "typing reaches no edit until one has the focus");

	edit->focus();
	assert_equal(true, host.keyboard, "focusing an edit takes the window's keyboard");
	assert_equal(true, edit->has_focus(), "and the edit has the focus");
	assert_equal(true, host.typing, "so the window expects typing");

	host.type("ab");
	host.type("\x01");
	assert_equal_strict("ab", edit->window_text(), "what is typed is the text, without control characters");
	assert_equal(true, host.key(platform::keys::BACK), "Backspace is the edit's");
	assert_equal_strict("a", edit->window_text(), "and takes the last character");
	assert_equal(2_z, changes.size(), "each change is reported once");
	assert_equal_strict("a", changes.back(), "with the text as it now is");

	assert_equal(false, host.key(platform::keys::F5), "a key the edit has no use for is not taken");

	edit->window_text("set");
	assert_equal_strict("set", edit->window_text(), "text set from code replaces the text");
	assert_equal(2_z, changes.size(), "and is the owner's own value, so it is not reported back");
	host.type("s");
	assert_equal_strict("sets", changes.back(), "typing after it is, from the end it leaves the caret at");

	assert_equal(true, host.key(U'A', control_key()), "Ctrl+A is the edit's");
	host.type("all");
	assert_equal_strict("all", edit->window_text(), "and selects the whole text");

	host.lose_keyboard_focus();
	assert_equal(false, edit->has_focus(), "an edit has the focus only while its window has the keyboard");
	assert_equal(false, host.typing, "and nothing is typed into it");
}

// The native edit placed the caret where it was pressed, extended a selection by dragging and with
// Shift, and selected a word on a double press.
static void should_place_the_caret_where_it_is_pressed()
{
	test_host host;
	const auto edit = host.create_edit({}, "hello world", {});
	host.place(edit, {0, 0, 300, 28}, true);

	// The text starts 4 pixels in, and every byte is 8 wide.
	const auto at = [](const int offset) { return core::pointi{4 + offset * 8, 14}; };

	host.input->mouse_down(at(6), {});
	host.input->mouse_move(at(11));
	host.input->mouse_up(at(11), {});
	host.type("there");
	assert_equal_strict("hello there", edit->window_text(), "a drag selects what it passes over");

	host.click(at(5));
	host.click(at(0), shift_key());
	host.type("Oh");
	assert_equal_strict("Oh there", edit->window_text(), "Shift and a press extend the selection");

	host.input->mouse_double_click(at(4), {});
	host.input->mouse_up(at(4), {});
	host.type("hi");
	assert_equal_strict("Oh hi", edit->window_text(), "a double press selects a word");

	// A press within a character of more than one byte lands on the nearer side of it, never inside it.
	edit->window_text("A\xc3\xa9" "B");
	host.click({4 + 15, 14});
	host.type("x");
	assert_equal_strict("Ax\xc3\xa9" "B", edit->window_text(), "nearer its start, before it");
	edit->window_text("A\xc3\xa9" "B");
	host.click({4 + 17, 14});
	host.type("x");
	assert_equal_strict("A\xc3\xa9" "xB", edit->window_text(), "nearer its end, after it");
}

// The address and filter boxes select everything when they take the focus, and the press that gave
// it them keeps that selection; the next press places the caret.
static void should_keep_the_selection_a_focusing_press_made()
{
	test_host host;
	ui::edit_styles styles;
	styles.select_all_on_focus = true;
	const auto edit = host.create_edit(styles, "search", {});
	host.place(edit, {0, 0, 300, 28}, true);

	host.click({4 + 3 * 8, 14});
	host.type("x");
	assert_equal_strict("x", edit->window_text(), "the press that focused the edit left everything selected");

	edit->window_text("search");
	host.click({4 + 3 * 8, 14});
	host.type("x");
	assert_equal_strict("seaxrch", edit->window_text(), "a press in a focused edit places the caret");
}

// An owner's key handler sees each key before the edit edits with it, and a key it takes goes no
// further: the address box runs a search on Return.
static void should_offer_keys_to_the_owner_first()
{
	test_host host;
	std::vector<int> offered;
	ui::edit_styles styles;
	styles.capture_key_down = [&offered](const int key, platform::key_state)
	{
		offered.push_back(key);
		return key == static_cast<int>(platform::keys::RETURN);
	};

	const auto edit = host.create_edit(styles, "abc", {});
	host.place(edit, {0, 0, 300, 28}, true);
	edit->focus();

	assert_equal(true, host.key(platform::keys::RETURN), "a key the owner takes is taken");
	assert_equal(true, host.key(platform::keys::LEFT), "and the edit still edits with the rest");
	assert_equal(2_z, offered.size(), "the owner saw both");
	assert_equal(true, offered.front() == static_cast<int>(platform::keys::RETURN), "in the order they came");
}

// Selection offsets count the bytes of the UTF-8 text, and a negative offset is its end. One that falls
// inside a character moves to its start.
static void should_select_by_byte_offsets()
{
	test_host host;
	const auto edit = host.create_edit({}, "\xc3\xa9\xf0\x9f\x98\x80x", {});
	host.place(edit, {0, 0, 300, 28}, true);
	edit->focus();

	edit->select(6, -1);
	host.type("y");
	assert_equal_strict("\xc3\xa9\xf0\x9f\x98\x80y", edit->window_text(),
	                    "an accented letter is two bytes and an emoji four, and -1 is the end");

	edit->select(0, 2);
	host.type("e");
	assert_equal_strict("e\xf0\x9f\x98\x80y", edit->window_text(), "a selection from the start");

	edit->select(3, 3);
	host.type("z");
	assert_equal_strict("ez\xf0\x9f\x98\x80y", edit->window_text(), "an offset inside a character moves to its start");
}

// Text is the same on every platform: lines end in '\n' however they were given.
static void should_report_line_breaks_as_new_lines()
{
	test_host host;
	ui::edit_styles styles;
	styles.multi_line = true;
	styles.want_return = true;
	std::string last;
	const auto edit = host.create_edit(styles, {}, [&last](const std::string& text) { last = text; });
	host.place(edit, {0, 0, 300, 100}, true);
	edit->focus();

	host.type("a");
	assert_equal(true, host.key(platform::keys::RETURN), "Return is a new line in an edit that takes it");
	host.type("b");
	assert_equal_strict("a\nb", edit->window_text(), "lines end LF");
	assert_equal_strict(edit->window_text(), last, "and the change reports the same");

	edit->window_text("one\r\ntwo");
	assert_equal_strict("one\ntwo", edit->window_text(), "a CR LF given from code reads back as LF");
	edit->select(4, -1);
	host.type("z");
	assert_equal_strict("one\nz", edit->window_text(), "and offsets count the line break as one byte");
}

// A long paragraph wraps after the last space that fits, and Up and Down move between the lines it
// wraps to, keeping the caret's column.
static void should_wrap_lines_and_move_between_them()
{
	test_host host;
	ui::edit_styles styles;
	styles.multi_line = true;
	const auto edit = host.create_edit(styles, "aaa bbb ccc", {});

	// 60 pixels of text across: "aaa " is 32 and "aaa bbb" 56, "aaa bbb " 64.
	host.place(edit, {0, 0, 68, 100}, true);
	edit->focus();

	const auto first = ui::probe_edit_range_bounds(edit, 0, 3);
	const auto second = ui::probe_edit_range_bounds(edit, 8, 11);
	assert_equal(true, !first.is_empty() && !second.is_empty(), "both lines are laid out");
	assert_equal(true, second.top > first.top, "the last word wraps to a line of its own");

	edit->select(1, 1);
	host.key(platform::keys::DOWN);
	host.type("X");
	assert_equal_strict("aaa bbb cXcc", edit->window_text(), "Down keeps the column on the next line");

	host.key(platform::keys::HOME);
	host.type("Y");
	assert_equal_strict("aaa bbb YcXcc", edit->window_text(), "Home goes to the start of the line, not the text");

	host.key(platform::keys::UP);
	host.key(platform::keys::END);
	host.type("Z");
	assert_equal_strict("aaa bbbZ YcXcc", edit->window_text(),
	                    "End stops before the space a line wrapped at");
}

static void should_hit_test_the_last_trailing_spelling_character()
{
	test_host host;
	listed_dictionary dictionary;
	ui::edit_styles styles;
	styles.spelling = &dictionary;
	const auto edit = host.create_edit(styles, "prefix teh", {});
	host.place(edit, {0, 0, 220, 30}, true);

	const auto bounds = ui::probe_edit_range_bounds(edit, 7, 10);
	assert_equal(true, !bounds.is_empty(), "the spelling bounds include a trailing word whose end is exclusive");
	assert_equal(true, bounds.contains({4 + 9 * 8 + 1, 15}), "the spelling hit-test reaches the final character");
	assert_equal(false, bounds.contains({4 + 10 * 8 + 1, 15}), "and stops after it");
}

// The context menu offers the dictionary's suggestions for a word it does not know, under the pointer,
// and the ordinary editing commands everywhere.
static void should_offer_spelling_suggestions_over_a_misspelled_word()
{
	test_host host;
	listed_dictionary dictionary;
	ui::edit_styles styles;
	styles.spelling = &dictionary;
	std::string last;
	const auto edit = host.create_edit(styles, "prefix teh", [&last](const std::string& text) { last = text; });
	host.place(edit, {0, 0, 220, 30}, true);

	const core::pointi on_word{4 + 8 * 8, 15};
	const core::pointi elsewhere{4 + 2 * 8, 15};

	const auto over = ui::probe_edit_context_commands(edit, on_word);
	assert_equal_strict("the|ten|-|Add 'teh'|-|Cut|Copy|Paste|Delete|-|Select All", command_texts(over),
	                    "suggestions, then adding the word, then editing");

	const auto plain = ui::probe_edit_context_commands(edit, elsewhere);
	assert_equal_strict("Cut|Copy|Paste|Delete|-|Select All", command_texts(plain),
	                    "a correctly spelled word offers editing alone");
	assert_equal(false, plain.front()->enable, "and Cut needs a selection");

	over.front()->invoke();
	assert_equal_strict("prefix the", edit->window_text(), "a suggestion replaces the whole word");
	assert_equal_strict("prefix the", last, "and is reported as a change");

	edit->window_text("teh");
	const auto add = ui::probe_edit_context_commands(edit, {4 + 8, 15});
	const auto add_word = std::ranges::find_if(add, [](const ui::command_ptr& c) { return c && c->text == "Add 'teh'"; });
	assert_equal(true, add_word != add.end(), "a word can be added to the dictionary");
	(*add_word)->invoke();
	assert_equal(1_z, dictionary.added.size(), "and the dictionary is given it");
	host.input->tick();
	assert_equal(true, ui::probe_edit_context_commands(edit, {4 + 8, 15}).front()->text == "Cut",
	             "after which it is no longer underlined");

	assert_equal(true, host.input->context_menu(on_word), "a right press shows the menu");
	assert_equal(1_z, host.menus.size(), "through the window");
	assert_equal(true, host.menu_bounds.front().top_left() == on_word + test_host::screen_origin,
	             "at the press, in screen pixels");
}

static void should_shape_edit_text_once_per_change()
{
	test_host host;
	const auto edit = host.create_edit({}, {}, {});
	host.place(edit, {0, 0, 120, 28}, true);
	auto& mc = host.fixed;

	auto text = std::string(2048, 'a');
	edit->window_text(text);
	const auto after_first = mc.layout_updates;
	assert_equal(true, after_first >= 1, "the text is shaped");
	assert_equal(0, mc.prefix_measures, "prefix measuring is not used when shaping succeeds");

	host.click({119, 14});
	host.type("b");
	assert_equal(after_first + 1, mc.layout_updates, "one change is shaped once");
	assert_equal(2049_z, edit->window_text().size(), "the caret reached the end of long text");

	host.place(edit, {0, 0, 120, 28}, false);
	host.place(edit, {0, 0, 120, 28}, true);
	host.input->blink();
	const auto settled = mc.layout_updates;
	edit->window_text(edit->window_text());
	assert_equal(settled, mc.layout_updates, "an unchanged text is not shaped again");
}

///////////////////////////////////////////////////////////////////////////////////////////////////
// The set
///////////////////////////////////////////////////////////////////////////////////////////////////

static void should_move_focus_with_tab_in_creation_order()
{
	test_host host;
	const auto first = host.create_edit({}, "one", {});
	const auto disabled = host.create_button(icon_index{}, "Off", {}, {}, false);
	const auto second = host.create_edit({}, "two", {});
	const auto hidden = host.create_button(icon_index{}, "Hidden", {}, {}, false);
	const auto check = host.create_check_button(false, "Check", false, {}, ui::radio_group_default);

	host.place(first, {0, 0, 100, 20}, true);
	host.place(disabled, {0, 20, 100, 40}, true);
	host.place(second, {0, 40, 100, 60}, true);
	host.place(hidden, {0, 60, 100, 80}, false);
	host.place(check, {0, 80, 100, 100}, true);
	disabled->enable(false);

	assert_equal(true, host.input->focus_first(), "the first edit takes the focus");
	assert_equal(true, first->has_focus(), "the first in creation order");

	host.key(platform::keys::TAB);
	assert_equal(true, second->has_focus(), "Tab passes over a disabled control");
	host.type("x");
	assert_equal_strict("x", second->window_text(), "and an edit Tab reaches has all its text selected");

	host.key(platform::keys::TAB);
	assert_equal(true, check->has_focus(), "Tab passes over a hidden control");
	host.key(platform::keys::TAB);
	assert_equal(true, first->has_focus(), "and wraps to the start");
	host.key(platform::keys::TAB, shift_key());
	assert_equal(true, check->has_focus(), "Shift+Tab goes back");

	const auto& focus = host.events->focus;
	assert_equal(true, !focus.empty() && focus.back().first && focus.back().second == ui::control_ptr(check),
	             "the owner is told which control has the focus");
}

// A window that loses the keyboard to another application has its control back on return, as a
// dialog box's did; one that loses it to another of the application's windows does not, as a native
// control lost the focus then.
static void should_give_the_focus_back_only_after_the_application_had_it()
{
	test_host host;
	const auto edit = host.create_edit({}, "search", {});
	host.place(edit, {0, 0, 100, 20}, true);
	edit->focus();
	auto& focus = host.events->focus;
	const auto told = [&focus](const bool has, const ui::control_ptr& c)
	{
		return !focus.empty() && focus.back().first == has && focus.back().second == c;
	};

	host.lose_keyboard_focus();
	assert_equal(true, told(false, edit), "losing the keyboard takes it from the control");
	host.take_keyboard_focus();
	assert_equal(true, told(true, edit), "and having it back gives it back");
	assert_equal(true, edit->has_focus(), "so typing goes on where it was");

	host.lose_keyboard_focus();
	const auto heard = focus.size();
	host.input->clear_focus();
	assert_equal(heard, focus.size(), "a control that already lost the keyboard is not told again");
	host.take_keyboard_focus();
	assert_equal(heard, focus.size(), "and the window takes the keyboard back for itself");
	assert_equal(true, host.input->focused() == nullptr, "with no control focused");
	assert_equal(false, host.input->is_typing(), "so keys are the window's");
}

// A region of a window that scrolls moves the controls drawn in it: what is hit, and the caret an input
// method follows, are where they are drawn.
static void should_move_the_controls_with_a_scrolled_window()
{
	test_host host;
	auto pressed = 0;
	const auto edit = host.create_edit({}, "text", {});
	const auto button = host.create_button(icon_index{}, "Go", {}, [&pressed] { ++pressed; }, false);
	host.place(edit, {0, 0, 100, 20}, true);
	host.place(button, {0, 40, 100, 60}, true);
	edit->focus();
	const auto caret = host.caret;

	host.scroll(0, -30);
	assert_equal(true, host.caret == caret.offset(0, -30), "the input method follows the caret");
	assert_equal(true, button->window_bounds() == core::recti(0, 10, 100, 30).offset(test_host::screen_origin),
	             "the button is on screen where it is drawn");

	host.click({50, 50});
	assert_equal(0, pressed, "where the button was is empty");
	host.click({50, 20});
	assert_equal(1, pressed, "and it is pressed where it is drawn");
}

static void should_press_the_default_button_and_close_on_escape_in_a_dialog()
{
	test_host host;
	auto pressed = 0;
	auto other = 0;
	const auto edit = host.create_edit({}, "name", {});
	const auto ok = host.create_button(icon_index{}, "OK", {}, [&pressed] { ++pressed; }, true);
	const auto cancel = host.create_button(icon_index{}, "Cancel", {}, [&other] { ++other; }, false);
	host.place(edit, {0, 0, 100, 20}, true);
	host.place(ok, {0, 30, 100, 50}, true);
	host.place(cancel, {0, 60, 100, 80}, true);
	edit->focus();

	assert_equal(false, host.key(platform::keys::RETURN), "outside a dialog Return presses nothing");
	assert_equal(false, host.key(platform::keys::ESCAPE), "and Escape closes nothing");

	host.dialog = true;
	assert_equal(true, host.key(platform::keys::RETURN), "in a dialog Return presses the default button");
	assert_equal(1, pressed, "once");

	cancel->focus();
	host.key(platform::keys::RETURN);
	assert_equal(1, other, "Return presses the button that has the focus");
	host.key(platform::keys::SPACE);
	assert_equal(2, other, "and so does Space");

	assert_equal(true, host.key(platform::keys::ESCAPE), "Escape is taken");
	assert_equal(true, host.closed && host.cancelled, "and cancels the dialog");
}

// Dialog buttons are labelled as native ones were: '&' marks the key that presses them and is not
// drawn, and the dialog manager's keyboard reaches them by it.
static void should_press_a_dialog_button_with_the_key_its_label_marks()
{
	assert_equal_strict("OK", ui::parse_mnemonic("&OK").text, "the marker is not shown");
	assert_equal(static_cast<int>('O'), static_cast<int>(ui::parse_mnemonic("&OK").key), "it marks the key");
	assert_equal_strict("R&B", ui::parse_mnemonic("R&&B").text, "a doubled marker is an ampersand");
	assert_equal(0, static_cast<int>(ui::parse_mnemonic("R&&B").key), "and marks nothing");
	assert_equal_strict("R&B", ui::parse_mnemonic("R&B").text, "an ampersand inside a word is text");
	assert_equal_strict("Fitness & Workout", ui::parse_mnemonic("Fitness & Workout").text,
	                    "and so is one between words");
	const auto cjk = ui::parse_mnemonic("\xe5\x88\x86\xe6\x9e\x90(&A)");
	assert_equal_strict("\xe5\x88\x86\xe6\x9e\x90(A)", cjk.text, "a translation's bracketed marker is one");
	assert_equal(static_cast<int>('A'), static_cast<int>(cjk.key), "with the letter it brackets");
	assert_equal(static_cast<int>('C'), static_cast<int>(ui::parse_mnemonic("x (&c)").key),
	             "a marked letter is its key in either case");
	assert_equal(0, static_cast<int>(ui::parse_mnemonic("&\xd0\x9e" "K").key),
	             "a character no key code names marks nothing");
	assert_equal_strict("\xd0\x9e" "K", ui::parse_mnemonic("&\xd0\x9e" "K").text, "but is still shown whole");
	assert_equal(0_z, ui::parse_mnemonic("&OK").offset, "the underline goes under the marked letter");
	assert_equal(3_z, ui::parse_mnemonic("As &JPEG").offset, "wherever it is in the label");
	assert_equal(7_z, cjk.offset, "counted in the shown text's bytes");
	assert_equal(true, ui::parse_mnemonic("R&&B").offset == std::string::npos, "and nothing is underlined unmarked");

	test_host host;
	host.dialog = true;
	auto pressed = 0;
	auto checked = false;
	const auto edit = host.create_edit({}, {}, {});
	const auto check = host.create_check_button(false, "&Remember", false,
	                                                 [&checked](const bool v) { checked = v; },
	                                                 ui::radio_group_default);
	const auto ok = host.create_button(icon_index{}, "&OK", {}, [&pressed] { ++pressed; }, true);
	host.place(edit, {0, 0, 100, 20}, true);
	host.place(check, {0, 30, 100, 50}, true);
	host.place(ok, {0, 60, 100, 80}, true);
	assert_equal_strict("&OK", ok->window_text(), "the label keeps its marker");

	edit->focus();
	host.key('O');
	assert_equal(0, pressed, "a letter typed into an edit is the edit's");
	platform::key_state alt;
	alt.alt = true;
	assert_equal(true, host.key('O', alt), "Alt and the marked letter are taken");
	assert_equal(1, pressed, "and press the button");

	check->focus();
	host.key('R');
	assert_equal(true, checked, "with no typing to take it, the letter alone toggles a check box");
	assert_equal(true, check->has_focus(), "which takes the focus");
	host.key('O');
	assert_equal(2, pressed, "and presses a button");
	assert_equal(false, host.key('Z'), "a letter nothing is marked with is left alone");
}

// Each group of radio buttons keeps one checked, and every member hears its state, as the native
// auto-radio buttons did. Another group is left alone.
static void should_check_one_radio_button_in_its_group()
{
	test_host host;
	std::vector<std::pair<char, bool>> heard;
	const auto make = [&](const char name, const bool checked, const int group)
	{
		return host.create_check_button(checked, std::string(1, name), true,
		                                     [&heard, name](const bool v) { heard.emplace_back(name, v); }, group);
	};

	constexpr auto scope_group = 1;
	constexpr auto format_group = 2;
	const auto a = make('a', true, scope_group);
	const auto b = make('b', false, scope_group);
	const auto c = make('c', true, format_group);
	host.place(a, {0, 0, 100, 20}, true);
	host.place(b, {0, 20, 100, 40}, true);
	host.place(c, {0, 40, 100, 60}, true);

	host.click({50, 30});
	assert_equal(2_z, heard.size(), "both members of the group hear");
	assert_equal(true, heard[0] == std::pair{'a', false} && heard[1] == std::pair{'b', true}, "the old one off, the new on");

	heard.clear();
	host.key(platform::keys::UP);
	assert_equal(true, a->has_focus(), "an arrow moves along the group");
	assert_equal(true, heard.size() == 2 && heard[0] == std::pair{'a', true}, "and checks the button it lands on");

	heard.clear();
	host.click({50, 50});
	assert_equal(1_z, heard.size(), "the other group is not told");
}

static void should_report_slider_changes_while_dragging_and_once_released()
{
	test_host host;
	host.fixed.padding2 = 8;
	std::vector<std::pair<int, bool>> changes;
	const auto slider = host.create_slider(0, 100, [&changes](const int v, const bool tracking)
	{
		changes.emplace_back(v, tracking);
	});
	host.place(slider, {0, 0, 100, 20}, true);
	slider->set_pos(100);
	assert_equal(true, changes.empty(), "setting the position reports nothing");

	// The thumb is 12 across, so the track runs from 6 to 94.
	host.input->mouse_down({94, 10}, {});
	assert_equal(true, changes.empty(), "pressing the maximum thumb where it is drawn keeps the maximum");
	host.input->mouse_move({50, 10});
	assert_equal(50, slider->get_pos(), "the track's midpoint is the range's");
	assert_equal(true, changes.back() == std::pair{50, true}, "reported while dragging");
	host.input->mouse_move({6, 10});
	host.input->mouse_up({6, 10}, {});
	assert_equal(0, slider->get_pos(), "the track's start is the minimum");
	assert_equal(true, changes.back() == std::pair{0, false}, "and the release settles it");

	// The thumb is where it is drawn wherever the slider is placed, and at any scale.
	host.place(slider, {20, 0, 120, 20}, true);
	slider->set_pos(100);
	changes.clear();
	host.input->mouse_down({114, 10}, {});
	assert_equal(100, slider->get_pos(), "an offset slider keeps its maximum under its drawn thumb");
	assert_equal(true, changes.empty(), "and reports nothing");
	host.input->mouse_up({114, 10}, {});

	host.fixed.scale_factor = 2.0;
	host.input->scale_changed();
	host.place(slider, {0, 0, 200, 40}, true);
	slider->set_pos(0);
	host.input->mouse_down({188, 20}, {});
	assert_equal(100, slider->get_pos(), "at twice the scale the track ends where the larger thumb is drawn");
	host.input->mouse_up({188, 20}, {});

	host.fixed.scale_factor = 1.0;
	host.input->scale_changed();
	host.place(slider, {0, 0, 100, 20}, true);
	slider->set_pos(0);

	slider->focus();
	changes.clear();
	host.key(platform::keys::RIGHT);
	assert_equal(1, slider->get_pos(), "Right steps up");
	host.key(platform::keys::END);
	assert_equal(100, slider->get_pos(), "End goes to the maximum");
	assert_equal(true, std::ranges::all_of(changes, [](const auto& c) { return !c.second; }),
	             "keys report settled values");

	// Dense ranges put several values on one pixel; dragging to either end must still reach it.
	for (const auto& [track_width, max_value] : {std::pair{40, 100}, std::pair{25, 100}, std::pair{100, 1000}})
	{
		const core::recti dense_track{10, 0, 10 + track_width, 4};
		assert_equal(max_value, ui::slider_track_x_to_value(dense_track, dense_track.right, 0, max_value, 0),
		             "dragging from the minimum to the right end reaches the maximum");
		assert_equal(0, ui::slider_track_x_to_value(dense_track, dense_track.left, 0, max_value, max_value),
		             "dragging from the maximum to the left end reaches the minimum");

		const auto mid_x = dense_track.left + track_width / 2;
		const auto proportional = core::round(static_cast<double>(track_width / 2) * max_value / track_width);
		const auto mid_value = ui::slider_track_x_to_value(dense_track, mid_x, 0, max_value, 0);
		assert_equal(ui::slider_value_to_track_x(dense_track, mid_value, 0, max_value), mid_x,
		             "a mid-track drag lands on a value painted under the pointer");
		assert_equal(proportional, mid_value, "a mid-track drag stays proportional");
	}

	const core::recti track_bounds{6, 8, 94, 12};
	for (auto expected = 0; expected <= 100; ++expected)
	{
		const auto thumb_x = ui::slider_value_to_track_x(track_bounds, expected, 0, 100);
		assert_equal(expected, ui::slider_track_x_to_value(track_bounds, thumb_x, 0, 100, expected),
		             "pressing each painted thumb centre preserves its value");
	}
}

static void should_invoke_a_toolbar_command_on_a_release_over_its_button()
{
	test_host host;
	auto invoked = 0;
	const auto run = std::make_shared<ui::command>();
	run->icon = static_cast<icon_index>(0xE000);
	run->invoke = [&invoked] { ++invoked; };

	const auto more = std::make_shared<ui::command>();
	more->icon = static_cast<icon_index>(0xE001);
	more->menu = [] { return std::vector<ui::command_ptr>{std::make_shared<ui::command>("Item", std::any{}, [] {}, false)}; };

	ui::toolbar_styles styles;
	styles.button_extent = {30, 40};
	const auto toolbar = host.create_toolbar(styles, {run, nullptr, more});
	const auto extent = toolbar->measure_toolbar(host.fixed, 1000);
	assert_equal(true, extent == core::sizei(30 + 8 + 30, 40), "fixed buttons, and a separator between");
	host.place(toolbar, {10, 10, 10 + extent.cx, 10 + extent.cy}, true);

	host.input->mouse_move({20, 20});
	assert_equal(true, !host.events->hovered.empty() && host.events->hovered.back() == run,
	             "the owner hears which command is under the pointer");

	host.input->mouse_down({20, 20}, {});
	host.input->mouse_move({300, 20});
	host.input->mouse_up({300, 20}, {});
	assert_equal(0, invoked, "a press dragged off its button invokes nothing");

	host.click({20, 20});
	assert_equal(1, invoked, "a press and release over it invokes the command once");
	assert_equal(false, toolbar->has_focus(), "a toolbar does not take the focus from a press");

	host.input->mouse_down({10 + 38 + 15, 20}, {});
	assert_equal(1_z, host.menus.size(), "a button with a menu drops it on the press");
	assert_equal_strict("Item", command_texts(host.menus.front()), "with the command's menu");
	assert_equal(true, host.menu_bounds.front() == toolbar->button_bounds(more),
	             "beside the button, in screen pixels");
	assert_equal(false, host.captured, "having let go of the pointer for it");

	more->visible = false;
	toolbar->update_button_state(true, false);
	assert_equal(true, toolbar->measure_toolbar(host.fixed, 1000).cx == 30 + 8, "a hidden command takes no room");
}

// A bar measures its toolbars for the slots it gives them before a frame has found them, as a view's bar
// is when the view first opens. Measured then, a button's text must count, or it is clipped once drawn.
static void should_measure_a_toolbar_s_text_before_a_frame_finds_it()
{
	fixed_measure mc;
	const auto save = std::make_shared<ui::command>();
	save->icon = static_cast<icon_index>(0xE000);
	save->toolbar_text = "Save";

	ui::toolbar_styles styles;
	styles.button_extent = {0, 0};
	const auto toolbar = ui::make_toolbar(styles, {save});
	const auto measured = toolbar->measure_toolbar(mc, 1000);
	assert_equal(true, measured.cx > mc.measure_text("Save", {}, {}, 1000, 0).cx,
	             "the text is measured, with the icon beside it");

	toolbar->layout(mc, core::recti({0, 0}, measured));

	test_host host;
	host.add(toolbar);
	assert_equal(true, toolbar->measure_toolbar(host.fixed, 1000) == measured,
	             "found by a frame, it is the size it was measured");
}

static void should_invoke_the_maximize_caption_button_only_on_a_release_over_its_press()
{
	ui::caption_button_tracker t;

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

// Windows 11 offers snap layouts over a window's maximize button, so a frame that has one reports the
// toolbar button for maximizing as that caption button, and Windows sends the button its own
// messages: a release over the button the press began on invokes it, and nothing else does.
static void should_answer_as_the_caption_button_only_for_maximize()
{
	test_host host;
	auto maximized = 0;
	const auto other = std::make_shared<ui::command>();
	other->icon = static_cast<icon_index>(0xE000);
	const auto maximize = std::make_shared<ui::command>();
	maximize->icon = static_cast<icon_index>(0xE001);
	maximize->caption_maximize = true;
	maximize->invoke = [&maximized] { ++maximized; };

	ui::toolbar_styles styles;
	styles.button_extent = {40, 40};
	const auto toolbar = host.create_toolbar(styles, {other, maximize});
	host.place(toolbar, {0, 0, 80, 40}, true);

	assert_equal(0, host.input->caption_button_at({60, 20}), "a frame that offers no snap layouts has no caption button");

	host.caption = true;
	const auto id = host.input->caption_button_at({60, 20});
	assert_equal(true, id != 0, "the maximize button answers as the caption button");
	assert_equal(0, host.input->caption_button_at({20, 20}), "an ordinary button stays a toolbar button");

	host.input->caption_hover(id);
	assert_equal(id, host.input->caption().hover_id, "resting on it lights it");
	host.input->caption_press(id);
	host.input->caption_release(id);
	assert_equal(1, maximized, "a press and release over it invokes the command once");
	assert_equal(0, host.input->caption().hover_id, "and forgets the hover, as the frame moves under the pointer");

	host.input->caption_press(id);
	host.input->caption_leave();
	host.input->caption_release(id);
	assert_equal(1, maximized, "a press abandoned by leaving invokes nothing");
}

static void should_step_and_type_a_date_field()
{
	test_host host;
	std::vector<core::date_t> changes;
	const auto date = host.create_date_time_control(core::date_t(2025, 8, 16, 18, 11, 56),
	                                                     [&changes](const core::date_t d) { changes.push_back(d); },
	                                                     false);
	host.place(date, {0, 0, 160, 28}, true);
	const auto next = host.create_edit({}, {}, {});
	host.place(next, {0, 40, 160, 68}, true);
	date->focus();
	assert_equal(true, host.typing, "a date field takes typing");

	const auto before = changes.size();
	host.key(platform::keys::UP);
	assert_equal(before + 1, changes.size(), "Up steps the segment and reports the date");

	const auto texts_before = date->window_text();
	host.type("0");
	host.type("1");
	assert_equal(true, date->window_text() != texts_before, "digits type into the field");
	assert_equal(true, changes.size() >= before + 3, "and each is reported");

	// The locale decides which segment comes first, so the walk starts from the first, whichever that is.
	host.key(platform::keys::HOME);
	assert_equal(true, host.key(platform::keys::TAB), "Tab moves to the next segment while there is one");
	assert_equal(true, date->has_focus(), "and stays in the field");
	host.key(platform::keys::TAB);
	assert_equal(true, date->has_focus(), "through every segment");
	host.key(platform::keys::TAB);
	assert_equal(true, next->has_focus(), "from the last segment it leaves the field");
}

///////////////////////////////////////////////////////////////////////////////////////////////////
// The menu
///////////////////////////////////////////////////////////////////////////////////////////////////

namespace
{
	class test_menu_host final : public ui::menu_host
	{
	public:
		fixed_measure fixed;
		core::recti work{0, 0, 1920, 1040};
		std::vector<std::pair<std::shared_ptr<recording_frame>, ui::frame_host_weak_ptr>> popups;
		std::function<void(ui::menu_session&)> drive;

		render::measure_context* menu_measure() override { return &fixed; }

		ui::frame_ptr create_menu_popup(const ui::frame_host_weak_ptr& content, const ui::frame_ptr&) override
		{
			auto frame = std::make_shared<recording_frame>();
			popups.emplace_back(frame, content);
			return frame;
		}

		core::recti menu_work_area(core::recti) override { return work; }

		void run_menu_loop(ui::menu_session& session) override
		{
			if (drive) drive(session);
		}
	};

	ui::command_ptr menu_command(const std::string_view text, int& invoked)
	{
		return std::make_shared<ui::command>(text, std::any{}, [&invoked] { ++invoked; }, false);
	}

	// Content of a fixed size, which remembers where it was laid out.
	class fixed_content final : public ui::element
	{
	public:
		core::sizei extent{100, 40};
		core::recti laid_out;

		core::sizei measure(render::measure_context&, int) const override { return extent; }
		void layout(render::measure_context&, const core::recti bounds) override { laid_out = bounds; }
		void render(render::draw_context&, core::pointi) const override {}
	};
}

static void should_choose_a_menu_command_with_the_keyboard()
{
	auto first = 0;
	auto second = 0;
	auto nested = 0;
	const auto hidden = std::make_shared<ui::command>("Hidden", std::any{}, [] {}, false);
	hidden->visible = false;
	const auto submenu = std::make_shared<ui::command>("More", std::any{}, nullptr, false);
	submenu->menu = [&nested] { return std::vector<ui::command_ptr>{menu_command("Deep", nested)}; };

	const std::vector<ui::command_ptr> commands = {
		menu_command("First", first), nullptr, hidden, nullptr, menu_command("Second", second), submenu
	};

	test_menu_host host;
	host.drive = [&host](ui::menu_session& session)
	{
		assert_equal(1_z, session.popups().size(), "the menu opens one popup");
		session.key_down(static_cast<int>(platform::keys::DOWN), {});
		session.key_down(static_cast<int>(platform::keys::DOWN), {});
		session.key_down(static_cast<int>(platform::keys::RETURN), {});
	};

	ui::track_drawn_menu(host, {100, 100, 140, 120}, {0, 0, 800, 600}, commands);
	assert_equal(0, first, "the first item is passed by");
	assert_equal(1, second, "Down passes over the separator, doubled where a command is hidden, to the second");
	assert_equal(true, host.popups.front().first->destroyed, "and the menu is gone before the command runs");

	host.popups.clear();
	host.drive = [](ui::menu_session& session)
	{
		session.key_down(static_cast<int>(platform::keys::END), {});
		session.key_down(static_cast<int>(platform::keys::RIGHT), {});
		assert_equal(2_z, session.popups().size(), "Right opens the submenu");
		session.key_down(static_cast<int>(platform::keys::RETURN), {});
	};
	ui::track_drawn_menu(host, {100, 100, 140, 120}, {0, 0, 800, 600}, commands);
	assert_equal(1, nested, "and Return chooses from it");

	host.drive = [](ui::menu_session& session)
	{
		session.key_down(static_cast<int>(platform::keys::DOWN), {});
		session.key_down(static_cast<int>(platform::keys::ESCAPE), {});
		assert_equal(true, session.done(), "Escape closes the menu");
	};
	ui::track_drawn_menu(host, {100, 100, 140, 120}, {0, 0, 800, 600}, commands);
	assert_equal(0, first, "and chooses nothing");
}

static void should_open_a_menu_beside_its_button_and_inside_the_display()
{
	auto invoked = 0;
	const std::vector<ui::command_ptr> commands = {menu_command("One", invoked), menu_command("Two", invoked)};
	test_menu_host host;
	core::recti shown;
	host.drive = [&host, &shown](ui::menu_session&) { shown = host.popups.back().first->bounds; };

	ui::track_drawn_menu(host, {100, 100, 140, 120}, {0, 0, 800, 600}, commands);
	assert_equal(true, shown.top == 120 && shown.left == 100, "below a button in the top half of its window");

	ui::track_drawn_menu(host, {100, 500, 140, 520}, {0, 0, 800, 600}, commands);
	assert_equal(true, shown.bottom == 500, "above one in the bottom half");

	ui::track_drawn_menu(host, {700, 100, 740, 120}, {0, 0, 800, 600}, commands);
	assert_equal(true, shown.right == 740, "right-aligned to one in the right half");

	ui::track_drawn_menu(host, core::recti(core::pointi{1910, 1030}, core::sizei{}), {0, 0, 800, 600}, commands);
	assert_equal(true, host.work.contains(shown.top_left()) && shown.right <= host.work.right &&
	             shown.bottom <= host.work.bottom, "a menu at a point near the corner stays on the display");
	assert_equal(true, shown.right <= 1910 && shown.bottom <= 1030, "opening up and to the left of it");
}

// Resting on another item that has a submenu replaces the open submenu with that item's. The selection
// follows the pointer at once, so it cannot be what says whose submenu is open: taken for that, the first
// submenu stayed open over every other item the pointer rested on.
static void should_open_the_submenu_of_the_next_item_the_pointer_rests_on()
{
	auto invoked = 0;
	const auto submenu = [&invoked](const std::string_view text)
	{
		auto c = std::make_shared<ui::command>(text, std::any{}, nullptr, false);
		c->menu = [&invoked] { return std::vector<ui::command_ptr>{menu_command("Item", invoked)}; };
		return c;
	};
	const std::vector<ui::command_ptr> commands = {submenu("Colors"), submenu("Sizes"), menu_command("Plain", invoked)};

	// Rows are 28 high under a 3 pixel border with this host's text.
	const auto row = [](const int i) { return core::pointi{20, 3 + 28 * i + 14}; };

	test_menu_host host;
	host.drive = [&host, &row](ui::menu_session& session)
	{
		const auto root = host.popups.front().second.lock();
		root->on_mouse_move(row(0), false);
		root->on_mouse_left_button_down(row(0), {});
		assert_equal(2_z, session.popups().size(), "a press opens the first item's submenu");
		const auto first = host.popups.back().first;

		root->on_mouse_move(row(1), false);
		std::this_thread::sleep_for(std::chrono::milliseconds(300));
		root->tick();
		assert_equal(2_z, session.popups().size(), "resting on the next item still shows one submenu");
		assert_equal(true, first->destroyed, "the first item's has gone");
		assert_equal(28, host.popups.back().first->bounds.top - first->bounds.top, "and the next item's is beside it");

		// The pointer crosses the plain item on its way into the open submenu, which leaves that item lit
		// rather than the one whose submenu it is.
		root->on_mouse_move(row(2), false);
		root->on_mouse_leave(row(2));
		session.key_down(static_cast<int>(platform::keys::LEFT), {});
		session.key_down(static_cast<int>(platform::keys::RETURN), {});
		assert_equal(2_z, session.popups().size(), "so Return there opens that submenu again");
		session.cancel();
	};

	ui::track_drawn_menu(host, {100, 100, 140, 120}, {0, 0, 800, 600}, commands);
	assert_equal(0, invoked, "and nothing was chosen");
}

// The caption buttons' glyphs are drawn a size down, as the native controls drew them, so they sit level
// with the icons beside them.
static void should_draw_the_caption_glyphs_a_size_down()
{
	ui::chrome_icon_set chrome;
	chrome.minimize = static_cast<icon_index>(0xE001);
	chrome.maximize = static_cast<icon_index>(0xE002);
	chrome.restore = static_cast<icon_index>(0xE003);
	chrome.close = static_cast<icon_index>(0xE004);
	chrome.check = static_cast<icon_index>(0xE005);

	for (const auto icon : {chrome.minimize, chrome.maximize, chrome.restore, chrome.close})
	{
		assert_equal(true, ui::icon_face(icon, chrome) == render::font_face::small_icons, "a caption glyph");
	}

	assert_equal(true, ui::icon_face(chrome.check, chrome) == render::font_face::icons,
	             "any other icon is drawn full size");
	assert_equal(true, ui::icon_face(chrome.close, {}) == render::font_face::icons,
	             "as is every icon when no caption glyphs are named");
}

// A row of buttons stands in a menu as cut, copy and paste do along the top of a context menu: the
// keyboard reaches it as one row and moves along it with Left and Right, and a press chooses the button
// under the pointer.
static void should_choose_from_a_row_of_buttons_in_a_menu()
{
	auto cut = 0;
	auto copy = 0;
	auto paste = 0;
	auto rename = 0;
	const auto row = std::make_shared<ui::command>();
	row->buttons = {menu_command("Cut", cut), menu_command("Copy", copy), menu_command("Paste", paste)};
	const std::vector<ui::command_ptr> commands = {row, nullptr, menu_command("Rename", rename)};

	test_menu_host host;
	host.drive = [](ui::menu_session& session)
	{
		session.key_down(static_cast<int>(platform::keys::DOWN), {});
		session.key_down(static_cast<int>(platform::keys::RIGHT), {});
		session.key_down(static_cast<int>(platform::keys::RETURN), {});
	};
	ui::track_drawn_menu(host, {100, 100, 140, 120}, {0, 0, 800, 600}, commands);
	assert_equal(1, copy, "Down reaches the row, Right moves to its second button and Return chooses it");
	assert_equal(0, cut, "and not the first");

	// With this host's text the widest name, Paste, asks for 60 pixels, so the three buttons share the menu
	// 60 each inside its 3 pixel border and 4 pixel inset: the third spans 127 to 187.
	host.drive = [&host](ui::menu_session&)
	{
		const auto popup = host.popups.back().second.lock();
		popup->on_mouse_move({150, 20}, false);
		popup->on_mouse_left_button_up({150, 20}, {});
	};
	ui::track_drawn_menu(host, {100, 100, 140, 120}, {0, 0, 800, 600}, commands);
	assert_equal(1, paste, "a click chooses the button under the pointer");

	host.drive = [](ui::menu_session& session)
	{
		session.key_down(static_cast<int>(platform::keys::DOWN), {});
		session.key_down(static_cast<int>(platform::keys::DOWN), {});
		session.key_down(static_cast<int>(platform::keys::RETURN), {});
	};
	ui::track_drawn_menu(host, {100, 100, 140, 120}, {0, 0, 800, 600}, commands);
	assert_equal(1, rename, "the items after the row are a step down from it");
}

// A row of buttons is drawn as a command bar draws one: an equal share of the menu each, each name under
// its icon, and a line between neighbours as tall as their icons and names - but not beside the selected
// button, whose share is lit.
static void should_draw_a_row_of_buttons_as_a_command_bar_does()
{
	auto invoked = 0;
	const auto row = std::make_shared<ui::command>();
	row->buttons = {menu_command("Cut", invoked), menu_command("Copy", invoked), menu_command("Paste", invoked)};

	test_menu_host host;
	host.drive = [&host](ui::menu_session& session)
	{
		const auto popup = host.popups.back().second.lock();
		painted_canvas canvas(host.popups.back().first->bounds.extent());
		const auto background = painted_canvas::rgb(ui::style::colors.menu_background);
		const auto rule = painted_canvas::rgb(core::average(ui::style::colors.menu_background,
		                                                  ui::style::colors.view_background));
		const auto lit = painted_canvas::rgb(core::lighten(ui::style::colors.menu_background, 0.22f));
		const auto ink = [&canvas, background](const core::recti r) { return r.width() * r.height() - canvas.count(r, background); };

		// The buttons span 7 to 67, 67 to 127 and 127 to 187, from 3 to 65: a 10 pixel margin, an 18 pixel
		// icon, a 4 pixel gap, a 20 pixel name and a 10 pixel margin.
		canvas.paint(*popup);
		assert_equal(true, canvas.pixel(67, 34) == rule && canvas.pixel(127, 34) == rule, "a line between each two buttons");
		assert_equal(true, canvas.pixel(67, 12) == background && canvas.pixel(67, 55) == background,
		             "as tall as their icons and names");
		assert_equal(true, ink({77, 35, 117, 55}) > 0, "the second button's name is under its icon");
		assert_equal(0, ink({68, 3, 127, 35}), "and nothing is drawn above it but the icon it does not have");

		session.key_down(static_cast<int>(platform::keys::DOWN), {});
		canvas.paint(*popup);
		assert_equal(true, canvas.pixel(10, 5) == lit && canvas.pixel(66, 64) == lit,
		             "the selected button's whole share is lit");
		assert_equal(background, canvas.pixel(67, 34), "with no line beside it");
		assert_equal(rule, canvas.pixel(127, 34), "while the others keep theirs");
		session.cancel();
	};

	ui::track_drawn_menu(host, {100, 100, 140, 120}, {0, 0, 800, 600}, {row});
	assert_equal(0, invoked, "and nothing was chosen");
}

// A label's '&' marks the letter that chooses it, as a native menu's does: underlined, not shown as an
// ampersand; the one item marking a letter is chosen at once, and several take turns being selected. A
// checked item still shows its check when the application gave the chrome no check glyph.
static void should_choose_a_menu_item_by_its_marked_letter()
{
	auto open = 0;
	auto print = 0;
	auto paste = 0;
	auto wrap = 0;
	const auto wrap_command = menu_command("&Wrap lines", wrap);
	wrap_command->checked = true;
	const std::vector<ui::command_ptr> commands = {
		menu_command("&Open...", open), menu_command("&Print", print), menu_command("&Paste", paste), wrap_command
	};

	const auto saved_icons = ui::chrome_icons();
	const core::scope_exit restore_icons([saved_icons] { ui::set_chrome_icons(saved_icons); });
	ui::set_chrome_icons({});

	test_menu_host host;
	host.drive = [&host](ui::menu_session& session)
	{
		const auto popup = host.popups.back().second.lock();
		ui::test::fake_draw_context draw({0, 0, 800, 600});
		if (popup) popup->on_window_paint(draw);
		assert_equal(true, draw.drawn_text().find('&') == std::string::npos, "the marker is not shown");
		assert_equal(true, draw.drawn_text().find("Open...") != std::string::npos, "the label is");
		assert_equal(true, !draw.lines.empty(), "the marked letters are underlined");
		assert_equal(true, !draw.polylines.empty(), "the check is stroked");
		session.key_down('O', {});
	};
	ui::track_drawn_menu(host, {100, 100, 140, 120}, {0, 0, 800, 600}, commands);
	assert_equal(1, open, "the one item marking a letter is chosen at once");

	host.drive = [](ui::menu_session& session)
	{
		session.key_down('P', {});
		assert_equal(false, session.done(), "a letter two items mark selects rather than chooses");
		session.key_down('P', {});
		session.key_down(static_cast<int>(platform::keys::RETURN), {});
	};
	ui::track_drawn_menu(host, {100, 100, 140, 120}, {0, 0, 800, 600}, commands);
	assert_equal(0, print, "the first is passed over");
	assert_equal(1, paste, "for the second");
}

// A dimmed item says nothing of why - that is the toolbar's bubble - so a reason neither widens nor
// heightens it, and it is still not chosen.
static void should_not_say_why_a_menu_item_is_dimmed()
{
	auto invoked = 0;
	const auto convert = menu_command("Convert", invoked);
	convert->enable = false;

	test_menu_host host;
	core::recti shown;
	host.drive = [&host, &shown](ui::menu_session& session)
	{
		shown = host.popups.back().first->bounds;
		session.key_down(static_cast<int>(platform::keys::DOWN), {});
		session.key_down(static_cast<int>(platform::keys::RETURN), {});
	};

	ui::track_drawn_menu(host, {100, 100, 140, 120}, {0, 0, 800, 600}, {convert});
	const auto plain = shown;

	convert->disabled_reason = "Only photos can be converted, and nothing chosen is a photo";
	ui::track_drawn_menu(host, {100, 100, 140, 120}, {0, 0, 800, 600}, {convert});
	assert_equal(plain.width(), shown.width(), "a reason does not widen the menu");
	assert_equal(plain.height(), shown.height(), "or make the dimmed item taller");
	assert_equal(0, invoked, "and the item is still not chosen");
}

// Content its owner draws takes a row of the menu, laid out across it, and is passed over by the keyboard
// and the pointer: it is shown, not chosen.
static void should_show_content_in_a_menu_without_choosing_it()
{
	auto invoked = 0;
	const auto content = std::make_shared<fixed_content>();
	const auto heading = std::make_shared<ui::command>();
	heading->content = content;
	const std::vector<ui::command_ptr> commands = {heading, menu_command("First", invoked)};

	test_menu_host host;
	core::recti shown;
	host.drive = [&host, &shown](ui::menu_session& session)
	{
		shown = host.popups.back().first->bounds;
		const auto popup = host.popups.back().second.lock();
		popup->on_mouse_left_button_up({20, 20}, {});
		assert_equal(false, session.done(), "a click on the content chooses nothing");
		session.key_down(static_cast<int>(platform::keys::DOWN), {});
		session.key_down(static_cast<int>(platform::keys::RETURN), {});
	};

	ui::track_drawn_menu(host, {100, 100, 140, 120}, {0, 0, 800, 600}, commands);
	assert_equal(1, invoked, "Down passes over the content to the first command");
	assert_equal(true, content->laid_out == core::recti(7, 7, shown.width() - 7, 47),
	             "the content is laid out across the top of the menu, inside its border and inset");
}

///////////////////////////////////////////////////////////////////////////////////////////////////
// Controls in the element tree
///////////////////////////////////////////////////////////////////////////////////////////////////

// A control is an element of the tree: as large as it asks with room added, at least its share of the width
// it is measured in, and placed where flex lays it out - two buttons in a centred row sit the gap apart, as
// tall as the row - with no element standing in for it. Its window finds it in the tree, and a hidden row
// hides it.
static void should_lay_out_controls_as_elements()
{
	test_host host;
	auto pressed = 0;
	const auto ok = ui::make_button(icon_index{}, "OK", {}, [&pressed] { ++pressed; }, true);
	const auto cancel = ui::make_button(icon_index{}, "Cancel", {}, {}, false);
	const auto ok_extent = ok->measure(host.fixed, 400);
	const auto cancel_extent = cancel->measure(host.fixed, 400);

	for (const auto& b : {ok, cancel}) b->room = {8, 8};
	ok->min_share = 5;

	const auto row = std::make_shared<ui::elements>();
	row->flex_container.wrap = ui::flex_wrap::no_wrap;
	row->flex_container.justify = ui::flex_justify::center;
	row->flex_container.align_items = ui::flex_align::stretch;
	row->flex_container.gap = {8, 0};
	row->add(ok);
	row->add(cancel);

	const auto extent = row->measure(host.fixed, 400);
	assert_equal(400, extent.cx, "the row is as wide as it is given");
	assert_equal(std::max(ok_extent.cy, cancel_extent.cy) + 8, extent.cy,
	             "and as tall as its tallest control with room added");

	row->layout(host.fixed, core::recti(10, 20, 410, 20 + extent.cy));
	host.add(row);

	const auto cancel_width = cancel_extent.cx + 8;
	const auto left = 10 + (400 - (80 + 8 + cancel_width)) / 2;
	assert_equal(true, ok->bounds == core::recti(left, 20, left + 80, 20 + extent.cy),
	             "the first a fifth of the width, centred with the second");
	assert_equal(true, cancel->bounds == core::recti(left + 88, 20, left + 88 + cancel_width, 20 + extent.cy),
	             "the second its own width with room, the gap after the first");

	host.click(ok->bounds.center());
	assert_equal(1, pressed, "and the window finds it there in the tree");

	row->is_visible(false);
	host.input->layout();
	host.click(ok->bounds.center());
	assert_equal(1, pressed, "a hidden row hides the controls it holds, though they are not hidden themselves");
	row->is_visible(true);

	auto visited = 0;
	row->visit_children([&visited](const ui::element_ptr&) { ++visited; });
	assert_equal(2, visited, "both are the row's children");
}

// A control is made apart from any window: it holds what it is given and measures as any other, and asked
// for the focus it takes it once a window finds it in its tree.
static void should_make_controls_apart_from_any_window()
{
	const auto button = ui::make_button("Save", {}, true);
	const auto edit = ui::make_edit({}, "draft", {});

	test_host host;
	const auto placed = host.create_button(icon_index{}, "Save", {}, {}, true);
	assert_equal(true, button->measure(host.fixed, 400) == placed->measure(host.fixed, 400),
	             "a button measures as one in a window does");

	edit->window_text("kept");
	assert_equal("kept"s, edit->window_text(), "an edit holds its text");

	edit->focus();
	assert_equal(false, edit->has_focus(), "no window, no focus");

	host.add(edit);
	host.place(edit, {0, 0, 100, 20}, true);
	assert_equal(true, edit->has_focus(), "found, it takes the focus it asked for");
}

// A disabled element disables the controls it holds, which take no input and dim, until it is enabled.
static void should_disable_the_controls_a_disabled_element_holds()
{
	test_host host;
	auto pressed = 0;
	const auto button = ui::make_button(icon_index{}, "Go", {}, [&pressed] { ++pressed; }, false);
	const auto panel = std::make_shared<ui::elements>();
	panel->add(button);
	host.add(panel);
	host.place(button, {0, 0, 100, 20}, true);

	panel->enable(false);
	host.input->layout();
	assert_equal(false, button->enabled, "under a disabled panel a button is disabled");
	assert_equal(true, button->is_enabled(), "though it is not disabled itself");
	host.click({50, 10});
	assert_equal(0, pressed, "and a click does nothing");

	panel->enable(true);
	host.input->layout();
	host.click({50, 10});
	assert_equal(1, pressed, "enabled again, it is pressed");
}

// Tab follows the tree, not the order the controls were made in.
static void should_tab_in_the_order_of_the_tree()
{
	test_host host;
	const auto second = ui::make_edit({}, "second", {});
	const auto first = ui::make_edit({}, "first", {});
	const auto column = std::make_shared<ui::elements>();
	column->add(first);
	column->add(second);
	host.add(column);
	host.place(first, {0, 0, 100, 20}, true);
	host.place(second, {0, 30, 100, 50}, true);

	first->focus();
	host.key(platform::keys::TAB);
	assert_equal(true, second->has_focus(), "Tab goes to the next in the tree");
	host.key(platform::keys::TAB);
	assert_equal(true, first->has_focus(), "and round to the first");
}

// A panel rebuilt or dropped takes its controls with it. The focus one of them held comes back to the window
// itself, which says so, so the keys go where its host sends them rather than to a control that is gone.
static void should_give_the_keyboard_back_to_the_frame_when_its_control_goes()
{
	test_host host;
	auto edit = ui::make_edit({}, "template", {});
	host.add(edit);
	host.place(edit, {0, 0, 100, 20}, true);
	host.take_keyboard_focus();
	edit->focus();
	assert_equal(true, edit->has_focus(), "the panel's edit holds the keyboard");

	host.events->roots.clear();
	host.events->focus.clear();
	edit.reset();
	host.input->layout();
	assert_equal(true, host.input->focused() == nullptr, "no control holds it once its edit has gone");
	assert_equal(1, host.keyboard_to_frame_count, "the window says it has the keyboard itself");
	assert_equal(false, host.typing, "and nothing is typed into");

	host.input->layout();
	assert_equal(1, host.keyboard_to_frame_count, "said once");
}

// A control disabled while it holds the focus - a panel a task's run disables - lets it go to the window.
static void should_give_the_keyboard_back_to_the_frame_when_its_control_is_disabled()
{
	test_host host;
	const auto edit = ui::make_edit({}, "name", {});
	const auto panel = std::make_shared<ui::elements>();
	panel->add(edit);
	host.add(panel);
	host.place(edit, {0, 0, 100, 20}, true);
	host.take_keyboard_focus();
	edit->focus();

	panel->enable(false);
	host.input->refresh();
	assert_equal(false, edit->has_focus(), "a disabled edit gives up the focus");
	assert_equal(true, host.input->focused() == nullptr, "which no control holds then");
	assert_equal(1, host.keyboard_to_frame_count, "the window has it itself");
	assert_equal(true, !host.events->focus.empty() && !host.events->focus.back().first &&
	                   host.events->focus.back().second == edit, "and its host heard the edit lose it");
}

// The frame focusing itself - a press on its ground, or its owner giving it the keyboard - takes the focus
// from the control that held it, though the window had the keyboard all along.
static void should_give_the_frame_the_keyboard_from_its_control()
{
	test_host host;
	const auto edit = ui::make_edit({}, "name", {});
	host.add(edit);
	host.place(edit, {0, 0, 100, 20}, true);
	host.take_keyboard_focus();
	edit->focus();

	host.input->clear_focus();
	assert_equal(false, edit->has_focus(), "the edit lets it go");
	assert_equal(1, host.keyboard_to_frame_count, "to the window itself");

	host.input->clear_focus();
	assert_equal(1, host.keyboard_to_frame_count, "which had it already the second time");
}

///////////////////////////////////////////////////////////////////////////////////////////////////
// The bubble
///////////////////////////////////////////////////////////////////////////////////////////////////

namespace
{
	class test_bubble_host final : public ui::bubble_host
	{
	public:
		fixed_measure fixed;
		core::recti work{0, 0, 1000, 800};
		std::shared_ptr<recording_frame> popup;
		int popups_made = 0;

		ui::frame_ptr create_bubble_popup(const ui::frame_host_weak_ptr&) override
		{
			++popups_made;
			popup = std::make_shared<recording_frame>();
			return popup;
		}

		render::measure_context* bubble_measure(const ui::frame_ptr&) override { return &fixed; }
		core::recti bubble_work_area(core::recti) override { return work; }
	};
}

// A bubble opens above what it points at, or to its left, and on the other side where the work area has
// no room; its arrow points at the anchor, and only its shadow may hang off the work area.
static void should_place_a_bubble_beside_what_it_points_at()
{
	const core::recti work{0, 0, 1000, 800};
	const core::recti anchor{400, 400, 440, 420};

	const auto above = ui::place_bubble({100, 40}, anchor, -1, false, work);
	assert_equal(152, above.bounds.width(), "the content's width with the shadow and padding either side");
	assert_equal(84, above.bounds.height(), "and its height");
	assert_equal(anchor.top, above.bounds.bottom, "above the anchor");
	assert_equal(anchor.center().x, above.bounds.center().x, "centred on it");
	assert_equal(anchor.top + 5, above.focus.y, "its arrow pointing down at it");

	const auto below = ui::place_bubble({100, 40}, {400, 20, 440, 40}, -1, false, work);
	assert_equal(40, below.bounds.top, "below an anchor with no room above it");
	assert_equal(35, below.focus.y, "pointing up at it");

	const auto left = ui::place_bubble({100, 40}, anchor, -1, true, work);
	assert_equal(anchor.left, left.bounds.right, "to the left of the anchor when horizontal");
	const auto right = ui::place_bubble({100, 40}, {20, 400, 40, 420}, -1, true, work);
	assert_equal(40, right.bounds.left, "to its right where there is no room on the left");

	const auto edge = ui::place_bubble({100, 40}, {980, 400, 1000, 420}, -1, false, work);
	assert_equal(work.right + 16, edge.bounds.right, "pulled back inside the work area, but for its shadow");
}

// The bubble draws in a layered popup the window layer makes once, shown where place_bubble puts it with
// its content laid out in the middle, and hidden again.
static void should_show_a_bubble_in_its_popup_and_hide_it()
{
	const auto host = std::make_shared<test_bubble_host>();
	const auto bubble = ui::create_drawn_bubble(host);
	const auto content = std::make_shared<fixed_content>();
	const core::recti anchor{400, 400, 440, 420};

	assert_equal(false, bubble->is_visible(), "a bubble starts hidden");
	bubble->show(content, anchor, -1, 300, false);
	assert_equal(true, host->popup && host->popup->visible, "showing it shows its popup");
	assert_equal(true, host->popup->bounds == ui::place_bubble(content->extent, anchor, -1, false, host->work).bounds,
	             "where place_bubble puts it");
	assert_equal(true, content->laid_out == core::center_rect(content->extent, host->popup->bounds.extent()),
	             "with its content laid out in the middle");
	assert_equal(true, bubble->is_visible(), "so the bubble is visible");

	bubble->show(content, {100, 400, 140, 420}, -1, 300, false);
	assert_equal(1, host->popups_made, "showing it again moves the same popup");

	bubble->hide();
	assert_equal(false, bubble->is_visible(), "hiding it hides the popup");
}

///////////////////////////////////////////////////////////////////////////////////////////////////
// Painting
///////////////////////////////////////////////////////////////////////////////////////////////////

// Every control draws into the software canvas, more than a flat fill, and only inside its bounds.
static void should_draw_every_control_into_the_software_canvas()
{
	constexpr core::sizei extent{400, 320};
	ui::style::install({});
	const auto target = render::create_memory_present_target();
	const auto ctx = render::create_software_draw_context(render::create_text_engine(render::default_font_size), target,
	                                                  render::default_font_size);
	ctx->colors = {0x00202020, 0x00f0f0f0, 0x00c08040};

	test_host host;
	host.metrics = ctx.get();

	const auto button = host.create_button(icon_index{}, "Press", {}, {}, true);
	const auto check = host.create_check_button(true, "Check", false, {}, ui::radio_group_default);
	const auto slider = host.create_slider(0, 10, {});
	const auto edit = host.create_edit({}, "Typed text", {});
	const auto date = host.create_date_time_control(core::date_t(2025, 8, 16, 0, 0, 0), {}, false);
	const auto run = std::make_shared<ui::command>();
	run->toolbar_text = "Run";
	run->checked = true;
	const auto toolbar = host.create_toolbar({}, {run});

	const std::vector<std::pair<ui::control_ptr, core::recti>> placed = {
		{button, {10, 10, 190, 50}},
		{check, {10, 60, 190, 90}},
		{slider, {10, 100, 190, 130}},
		{edit, {10, 140, 190, 170}},
		{date, {10, 180, 190, 210}},
		{toolbar, {10, 220, 190, 260}},
	};

	for (const auto& [c, r] : placed) host.place(c, r, true);
	edit->focus();
	slider->set_pos(5);

	ctx->begin_draw(extent, render::default_font_size, {});
	ctx->clear(core::color(0x00202020, 1.0f));
	host.paint(*ctx);
	ctx->render();

	const auto buffer = target->acquire_buffer(extent);
	const auto pixel = [&buffer](const int x, const int y)
	{
		const auto* const p = buffer.bits + y * buffer.stride + x * 4;
		return static_cast<uint32_t>(p[0]) | static_cast<uint32_t>(p[1]) << 8 | static_cast<uint32_t>(p[2]) << 16;
	};

	const auto background = pixel(300, 300);
	const char* const names[] = {"button", "check", "slider", "edit", "date", "toolbar"};
	auto index = 0;

	for (const auto& [c, r] : placed)
	{
		std::set<uint32_t> colours;
		auto painted = 0;

		for (auto y = r.top; y < r.bottom; ++y)
		{
			for (auto x = r.left; x < r.right; ++x)
			{
				const auto p = pixel(x, y);
				colours.insert(p);
				if (p != background) ++painted;
			}
		}

		const std::string name = names[index++];
		assert_equal(true, painted > 0, name + " draws");
		assert_equal(true, colours.size() > 1, name + " draws more than a flat fill");
	}

	auto outside = 0;
	for (auto y = 0; y < extent.cy; ++y)
	{
		for (auto x = 200; x < extent.cx; ++x) if (pixel(x, y) != background) ++outside;
	}
	assert_equal(0, outside, "and nothing draws outside its bounds");
}

// A slider's handle is seen whatever panel the slider is on. The idle handle was a fixed shade that
// matched the dialog background, so the edit view's sliders showed no handle until one had the focus.
static void should_draw_the_slider_handle_apart_from_the_panel_it_is_on()
{
	ui::style::install({});
	// The panels the application puts sliders on: dialogs and the edit view, the bars, groups and views.
	const render::color_style panels[] = {
		{ui::style::colors.dialog_background, ui::style::colors.dialog_text, ui::style::colors.dialog_selected_background},
		{ui::style::colors.toolbar_background, ui::style::colors.view_text, ui::style::colors.view_selected_background},
		{ui::style::colors.group_background, ui::style::colors.view_text, ui::style::colors.view_selected_background},
		{ui::style::colors.view_background, ui::style::colors.view_text, ui::style::colors.view_selected_background},
	};

	for (const auto& colors : panels)
	{
		test_host host;
		painted_canvas canvas(host, {200, 40}, colors);
		const auto slider = host.create_slider(0, 10, {});
		host.place(slider, {0, 0, 200, 40}, true);
		slider->set_pos(5);
		canvas.paint(host);

		const auto background = painted_canvas::rgb(colors.background);
		assert_equal(true, channel_distance(canvas.pixel(100, 20), background) >= 0x30,
		             "the idle handle stands out from the panel");
		assert_equal(true, channel_distance(canvas.pixel(100, 20), canvas.pixel(30, 20)) >= 0x30,
		             "and from the track it slides along");
	}
}

// An edit shows its selection only while it has the focus, and is drawn again when it loses it. Tab
// selects all of an edit's text, and the edit Tab left kept showing it in the focus colour.
static void should_hide_the_selection_of_an_edit_that_loses_the_focus()
{
	test_host host;
	painted_canvas canvas(host, {200, 80}, {0x00202020, 0x00f0f0f0, 0x00c08040});
	const auto first = host.create_edit({}, "Typed text", {});
	const auto second = host.create_edit({}, "Other", {});
	host.place(first, {0, 0, 200, 30}, true);
	host.place(second, {0, 40, 200, 70}, true);
	const auto selection = painted_canvas::rgb(ui::style::colors.dialog_selected_background);

	host.take_keyboard_focus();
	host.input->focus_first();
	canvas.paint(host);
	assert_equal(true, canvas.count({0, 0, 200, 30}, selection) > 50, "the focused edit shows its text selected");

	host.invalidated.clear();
	host.key(platform::keys::TAB);
	const auto drawn_again = [&host](const core::recti bounds)
	{
		return std::ranges::any_of(host.invalidated, [bounds](const core::recti& r) { return r == bounds; });
	};
	assert_equal(true, drawn_again({0, 0, 200, 30}), "the edit Tab left is drawn again");
	canvas.paint(host);
	assert_equal(0, canvas.count({0, 0, 200, 30}, selection), "the edit Tab left shows nothing in the focus colour");
	assert_equal(true, canvas.count({0, 40, 200, 70}, selection) > 50, "and the one it reached does");

	host.invalidated.clear();
	host.lose_keyboard_focus();
	assert_equal(true, drawn_again({0, 40, 200, 70}), "as is one whose window lost the keyboard");
	canvas.paint(host);
	assert_equal(0, canvas.count({0, 40, 200, 70}, selection), "nor does one whose window lost the keyboard");
}

void register_render_controls_tests(test_registry& tests)
{
	tests.add("Should edit single-line text"s, should_edit_single_line_text);
	tests.add("Should keep line breaks in multi-line text"s, should_keep_line_breaks_in_multi_line_text);
	tests.add("Should edit a segmented date"s, should_edit_a_segmented_date);
	tests.add("Should replace the full trailing spelling word"s, should_replace_the_full_trailing_spelling_word);
	tests.add("Should check edit words against the supplied dictionary"s,
	          should_check_edit_words_against_the_supplied_dictionary);
	tests.add("Should type into the focused edit and report each change"s,
	          should_type_into_the_focused_edit_and_report_each_change);
	tests.add("Should place the caret where it is pressed"s, should_place_the_caret_where_it_is_pressed);
	tests.add("Should keep the selection a focusing press made"s, should_keep_the_selection_a_focusing_press_made);
	tests.add("Should offer keys to the owner first"s, should_offer_keys_to_the_owner_first);
	tests.add("Should select by byte offsets"s, should_select_by_byte_offsets);
	tests.add("Should report line breaks as new lines"s, should_report_line_breaks_as_new_lines);
	tests.add("Should wrap lines and move between them"s, should_wrap_lines_and_move_between_them);
	tests.add("Should hit-test the last trailing spelling character"s,
	          should_hit_test_the_last_trailing_spelling_character);
	tests.add("Should offer spelling suggestions over a misspelled word"s,
	          should_offer_spelling_suggestions_over_a_misspelled_word);
	tests.add("Should shape edit text once per change"s, should_shape_edit_text_once_per_change);
	tests.add("Should move focus with Tab in creation order"s, should_move_focus_with_tab_in_creation_order);
	tests.add("Should give the focus back only after the application had it"s,
	          should_give_the_focus_back_only_after_the_application_had_it);
	tests.add("Should move the controls with a scrolled window"s, should_move_the_controls_with_a_scrolled_window);
	tests.add("Should press the default button and close on Escape in a dialog"s,
	          should_press_the_default_button_and_close_on_escape_in_a_dialog);
	tests.add("Should press a dialog button with the key its label marks"s,
	          should_press_a_dialog_button_with_the_key_its_label_marks);
	tests.add("Should check one radio button in its group"s, should_check_one_radio_button_in_its_group);
	tests.add("Should report slider changes while dragging and once released"s,
	          should_report_slider_changes_while_dragging_and_once_released);
	tests.add("Should invoke a toolbar command on a release over its button"s,
	          should_invoke_a_toolbar_command_on_a_release_over_its_button);
	tests.add("Should measure a toolbar's text before a frame finds it"s,
	          should_measure_a_toolbar_s_text_before_a_frame_finds_it);
	tests.add("Should invoke the maximize caption button only on a release over its press"s,
	          should_invoke_the_maximize_caption_button_only_on_a_release_over_its_press);
	tests.add("Should answer as the caption button only for maximize"s,
	          should_answer_as_the_caption_button_only_for_maximize);
	tests.add("Should step and type a date field"s, should_step_and_type_a_date_field);
	tests.add("Should choose a menu command with the keyboard"s, should_choose_a_menu_command_with_the_keyboard);
	tests.add("Should open a menu beside its button and inside the display"s,
	          should_open_a_menu_beside_its_button_and_inside_the_display);
	tests.add("Should open the submenu of the next item the pointer rests on"s,
	          should_open_the_submenu_of_the_next_item_the_pointer_rests_on);
	tests.add("Should draw the caption glyphs a size down"s, should_draw_the_caption_glyphs_a_size_down);
	tests.add("Should choose from a row of buttons in a menu"s, should_choose_from_a_row_of_buttons_in_a_menu);
	tests.add("Should draw a row of buttons as a command bar does"s, should_draw_a_row_of_buttons_as_a_command_bar_does);
	tests.add("Should not say why a menu item is dimmed"s, should_not_say_why_a_menu_item_is_dimmed);
	tests.add("Should choose a menu item by its marked letter"s, should_choose_a_menu_item_by_its_marked_letter);
	tests.add("Should show content in a menu without choosing it"s, should_show_content_in_a_menu_without_choosing_it);
	tests.add("Should lay out controls as elements"s, should_lay_out_controls_as_elements);
	tests.add("Should make controls apart from any window"s, should_make_controls_apart_from_any_window);
	tests.add("Should disable the controls a disabled element holds"s, should_disable_the_controls_a_disabled_element_holds);
	tests.add("Should tab in the order of the tree"s, should_tab_in_the_order_of_the_tree);
	tests.add("Should give the keyboard back to the frame when its control goes"s,
	          should_give_the_keyboard_back_to_the_frame_when_its_control_goes);
	tests.add("Should give the keyboard back to the frame when its control is disabled"s,
	          should_give_the_keyboard_back_to_the_frame_when_its_control_is_disabled);
	tests.add("Should give the frame the keyboard from its control"s,
	          should_give_the_frame_the_keyboard_from_its_control);
	tests.add("Should place a bubble beside what it points at"s, should_place_a_bubble_beside_what_it_points_at);
	tests.add("Should show a bubble in its popup and hide it"s, should_show_a_bubble_in_its_popup_and_hide_it);
	tests.add("Should draw every control into the software canvas"s,
	          should_draw_every_control_into_the_software_canvas);
	tests.add("Should draw the slider handle apart from the panel it is on"s,
	          should_draw_the_slider_handle_apart_from_the_panel_it_is_on);
	tests.add("Should hide the selection of an edit that loses the focus"s,
	          should_hide_the_selection_of_an_edit_that_loses_the_focus);
}
