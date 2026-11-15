// This file is part of libdesktop, a C++20 platform layer for desktop applications
// Copyright 2026  Zac Walker
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

// Purpose: Lightweight Flexbox-inspired layout for elements. Resolves wrapping, grow/shrink sizing,
// minimums, alignment, gaps, and explicit line breaks.

#include "libdesktop/ui.h"
#include "libdesktop/ui/elements.h"
#include <ranges>

namespace ui
{
	namespace
	{
		struct flex_item_state
		{
			size_t element_index = 0;
			ui::element* element = nullptr;
			core::sizei measured;
			int main_size = 0;
			int cross_size = 0;
			int main_porch = 0;
			int cross_porch = 0;
			int min_main = 0;
			int max_main = INT_MAX;
			int min_cross = 0;
			int max_cross = INT_MAX;
			// Width the cached measurement was taken at, so a re-measure is skipped when nothing moved.
			int measure_width = -1;
			float grow = 0.0f;
			float shrink = 0.0f;
			flex_align align_self = flex_align::automatic;
			bool main_start_auto : 1 = false;
			bool break_before : 1 = false;
			bool break_after : 1 = false;
			bool keep_with_next : 1 = false;
			bool center_line : 1 = false;
			bool fixed_height : 1 = false;
		};

		struct flex_line
		{
			size_t first = 0;
			size_t count = 0;
			int cross_size = 0;
			int used_main = 0;

			auto indices() const { return std::views::iota(first, first + count); }
		};

		int scaled_length(const int value, const double scale_factor)
		{
			if (value == INT_MAX) return INT_MAX;
			return core::round(value * scale_factor);
		}

		int main_value(const core::sizei value, const flex_direction direction)
		{
			return direction == flex_direction::row ? value.cx : value.cy;
		}

		int cross_value(const core::sizei value, const flex_direction direction)
		{
			return direction == flex_direction::row ? value.cy : value.cx;
		}

		void distribute_grow(std::vector<flex_item_state>& items, const flex_line& line, int free_space)
		{
			while (free_space > 0)
			{
				float total_weight = 0.0f;
				for (const auto item_index : line.indices())
				{
					const auto& item = items[item_index];
					if (item.grow > 0.0f && item.main_size < item.max_main) total_weight += item.grow;
				}

				if (total_weight <= 0.0f) break;
				auto consumed = 0;
				auto remaining_weight = total_weight;
				auto remaining_space = free_space;

				for (const auto item_index : line.indices())
				{
					auto& item = items[item_index];
					if (item.grow <= 0.0f || item.main_size >= item.max_main) continue;
					const auto share = remaining_weight == item.grow
					                       ? remaining_space
					                       : core::round(remaining_space * item.grow / remaining_weight);
					const auto applied = std::min(share, item.max_main - item.main_size);
					item.main_size += applied;
					consumed += applied;
					remaining_space -= share;
					remaining_weight -= item.grow;
				}

				if (consumed <= 0) break;
				free_space -= consumed;
			}
		}

		void distribute_shrink(std::vector<flex_item_state>& items, const flex_line& line, int deficit)
		{
			while (deficit > 0)
			{
				double total_weight = 0.0;
				for (const auto item_index : line.indices())
				{
					const auto& item = items[item_index];
					if (item.shrink > 0.0f && item.main_size > item.min_main)
					{
						total_weight += item.shrink * std::max(1, item.main_size);
					}
				}

				if (total_weight <= 0.0) break;
				auto consumed = 0;
				auto remaining_weight = total_weight;
				auto remaining_deficit = deficit;

				for (const auto item_index : line.indices())
				{
					auto& item = items[item_index];
					if (item.shrink <= 0.0f || item.main_size <= item.min_main) continue;
					const auto weight = item.shrink * std::max(1, item.main_size);
					const auto share = remaining_weight == weight
					                       ? remaining_deficit
					                       : core::round(remaining_deficit * weight / remaining_weight);
					const auto applied = std::min(share, item.main_size - item.min_main);
					item.main_size -= applied;
					consumed += applied;
					remaining_deficit -= share;
					remaining_weight -= weight;
				}

				if (consumed <= 0) break;
				deficit -= consumed;
			}
		}

		int outer_main_size(const flex_item_state& item)
		{
			return item.main_size + item.main_porch * 2;
		}

		int outer_cross_size(const flex_item_state& item)
		{
			return item.cross_size + item.cross_porch * 2;
		}
	}

	flex_layout_result calc_flex_layout(const std::vector<element_ptr>& elements, render::measure_context& mc,
	                                    const core::sizei available, const flex_container_layout& container)
	{
		flex_layout_result result;
		result.layout_bounds.resize(elements.size());

		const auto direction = container.direction;
		const auto scaled_padding = container.padding * mc.scale_factor;
		const auto inner_available = core::sizei{available.cx < 0 ? -1
		                                                          : std::max(0, available.cx - scaled_padding.cx * 2),
		                                         available.cy < 0 ? -1
		                                                          : std::max(0, available.cy - scaled_padding.cy * 2)};
		const auto main_padding = main_value(scaled_padding, direction);
		const auto cross_padding = cross_value(scaled_padding, direction);
		const auto available_main_value = main_value(inner_available, direction);
		const auto main_is_constrained = available_main_value >= 0;
		const auto available_main = std::max(0, available_main_value);
		const auto available_cross = cross_value(inner_available, direction);
		const auto scaled_gap = container.gap * mc.scale_factor;
		const auto main_gap = main_value(scaled_gap, direction);
		const auto cross_gap = cross_value(scaled_gap, direction);
		std::vector<flex_item_state> items;
		items.reserve(elements.size());

		for (auto element_index = 0u; element_index < elements.size(); ++element_index)
		{
			auto& element = *elements[element_index];
			if (!element.is_visible()) continue;

			const auto& style = element.flex;
			const auto porch = element.porch() * mc.scale_factor;
			const auto main_porch = main_value(porch, direction);
			const auto cross_porch = cross_value(porch, direction);
			const auto basis = scaled_length(style.basis, mc.scale_factor);
			const auto min_main = scaled_length(main_value(style.min_size, direction), mc.scale_factor);
			const auto max_main = scaled_length(main_value(style.max_size, direction), mc.scale_factor);
			const auto min_cross = scaled_length(cross_value(style.min_size, direction), mc.scale_factor);
			const auto max_cross = scaled_length(cross_value(style.max_size, direction), mc.scale_factor);
			// Measure inside the maximum the item can occupy, so a capped width wraps text rather than
			// reporting a height for a width the item is never given.
			const auto width_limit = direction == flex_direction::row
			                             ? std::max(0, std::min(basis >= 0
			                                                        ? std::min(basis, available_main - main_porch * 2)
			                                                        : available_main - main_porch * 2,
			                                                    max_main))
			                             : std::max(0, std::min(available_cross - cross_porch * 2, max_cross));
			const auto measured = element.measure(mc, width_limit);
			const auto measured_main = basis >= 0 ? basis : main_value(measured, direction);

			items.push_back({.element_index = element_index,
			                 .element = &element,
			                 .measured = measured,
			                 .main_size = std::clamp(measured_main, min_main, max_main),
			                 .cross_size = std::clamp(cross_value(measured, direction), min_cross, max_cross),
			                 .main_porch = main_porch,
			                 .cross_porch = cross_porch,
			                 .min_main = min_main,
			                 .max_main = max_main,
			                 .min_cross = min_cross,
			                 .max_cross = max_cross,
			                 .measure_width = width_limit,
			                 .grow = style.grow,
			                 .shrink = style.shrink,
			                 .align_self = style.align_self,
			                 .main_start_auto = style.main_start_auto,
			                 .break_before = style.break_before,
			                 .break_after = style.break_after,
			                 .keep_with_next = style.keep_with_next,
			                 .center_line = style.center_line,
			                 .fixed_height = element.is_style_bit_set(element_style::fixed_measure_height)});
		}

		std::vector<flex_line> lines;
		flex_line current_line;
		for (auto item_index = 0u; item_index < items.size();)
		{
			auto group_end = item_index;
			while (group_end + 1 < items.size() && items[group_end].keep_with_next) ++group_end;

			auto group_main = 0;
			for (auto group_index = item_index; group_index <= group_end; ++group_index)
			{
				if (group_index != item_index) group_main += main_gap;
				group_main += outer_main_size(items[group_index]);
			}

			const auto line_main = current_line.count == 0 ? 0 : current_line.used_main + main_gap;
			const auto should_wrap = current_line.count != 0 &&
			                         (items[item_index].break_before ||
			                          (container.wrap == flex_wrap::wrap && main_is_constrained &&
			                           line_main + group_main > available_main));
			if (should_wrap)
			{
				lines.push_back(current_line);
				current_line = {};
			}

			for (auto group_index = item_index; group_index <= group_end; ++group_index)
			{
				if (current_line.count != 0) current_line.used_main += main_gap;
				else current_line.first = group_index;
				++current_line.count;
				current_line.used_main += outer_main_size(items[group_index]);

				if (items[group_index].break_after && group_index == group_end)
				{
					lines.push_back(current_line);
					current_line = {};
				}
			}

			item_index = group_end + 1;
		}
		if (current_line.count != 0) lines.emplace_back(current_line);

		for (auto& line : lines)
		{
			auto used_main = main_gap * std::max(0, static_cast<int>(line.count) - 1);
			for (const auto item_index : line.indices()) used_main += outer_main_size(items[item_index]);

			if (main_is_constrained && used_main < available_main)
			{
				distribute_grow(items, line, available_main - used_main);
			}
			else if (main_is_constrained && used_main > available_main)
			{
				distribute_shrink(items, line, used_main - available_main);
			}

			line.used_main = main_gap * std::max(0, static_cast<int>(line.count) - 1);
			line.cross_size = 0;
			for (const auto item_index : line.indices())
			{
				auto& item = items[item_index];
				if (direction == flex_direction::row)
				{
					const auto content_width = std::max(0, item.main_size);

					if (content_width != item.measure_width && !item.fixed_height)
					{
						item.measured = item.element->measure(mc, content_width);
						item.measure_width = content_width;
					}

					item.cross_size = std::clamp(item.measured.cy, item.min_cross, item.max_cross);
				}
				line.used_main += outer_main_size(item);
				line.cross_size = std::max(line.cross_size, outer_cross_size(item));
			}
			if (container.wrap == flex_wrap::no_wrap && lines.size() == 1 && available_cross >= 0)
			{
				line.cross_size = std::max(line.cross_size, available_cross);
			}
		}

		auto cross_cursor = cross_padding;
		auto max_used_main = 0;
		for (auto& line : lines)
		{
			auto free_space = main_is_constrained ? std::max(0, available_main - line.used_main) : 0;
			auto main_cursor = main_padding;
			auto justify_gap = main_gap;
			auto auto_margin_index = line.count;
			for (auto line_index = 0u; line_index < line.count; ++line_index)
			{
				if (items[line.first + line_index].main_start_auto)
				{
					auto_margin_index = line_index;
					break;
				}
			}

			const auto all_centered = line.count != 0 &&
			                          std::ranges::all_of(line.indices(), [&items](const size_t item_index)
			{ return items[item_index].center_line; });
			auto justify = all_centered ? flex_justify::center : container.justify;
			if (auto_margin_index != line.count)
			{
				justify = flex_justify::start;
			}
			else if (justify == flex_justify::center)
			{
				main_cursor += free_space / 2;
			}
			else if (justify == flex_justify::end)
			{
				main_cursor += free_space;
			}
			else if (justify == flex_justify::space_between && line.count > 1)
			{
				justify_gap += free_space / (static_cast<int>(line.count) - 1);
			}

			// A line that spends free space to position its items occupies the whole main axis. Reporting
			// only the content would let an ancestor centre the same content a second time.
			const auto line_fills_main = main_is_constrained && auto_margin_index == line.count &&
			                             (container.justify == flex_justify::center ||
			                              container.justify == flex_justify::end ||
			                              (container.justify == flex_justify::space_between && line.count > 1));

			for (auto line_index = 0u; line_index < line.count; ++line_index)
			{
				auto& item = items[line.first + line_index];
				if (line_index == auto_margin_index) main_cursor += free_space;

				const auto align = item.align_self == flex_align::automatic ? container.align_items : item.align_self;
				auto cross_position = cross_cursor + item.cross_porch;
				auto item_cross_size = item.cross_size;
				if (align == flex_align::center)
				{
					cross_position = cross_cursor + (line.cross_size - item_cross_size) / 2;
				}
				else if (align == flex_align::end)
				{
					cross_position = cross_cursor + line.cross_size - item.cross_porch - item_cross_size;
				}
				else if (align == flex_align::stretch)
				{
					item_cross_size = std::clamp(std::max(0, line.cross_size - item.cross_porch * 2), item.min_cross,
					                             item.max_cross);
				}

				const auto item_main_position = main_cursor + item.main_porch;
				if (direction == flex_direction::row)
				{
					result.layout_bounds[item.element_index] = core::recti(item_main_position, cross_position,
					                                                       item_main_position + item.main_size,
					                                                       cross_position + item_cross_size);
				}
				else
				{
					result.layout_bounds[item.element_index] = core::recti(cross_position, item_main_position,
					                                                       cross_position + item_cross_size,
					                                                       item_main_position + item.main_size);
				}

				main_cursor += outer_main_size(item);
				if (line_index + 1 < line.count) main_cursor += justify_gap;
			}

			max_used_main = std::max(max_used_main, line_fills_main ? main_padding + available_main : main_cursor);
			cross_cursor += line.cross_size + cross_gap;
		}
		if (lines.empty())
		{
			// Nothing visible occupies no space, so an empty container never leaves a padded gap or paints
			// a background band over it.
			return result;
		}
		cross_cursor -= cross_gap;
		cross_cursor += cross_padding;
		max_used_main += main_padding;

		result.extent = direction == flex_direction::row ? core::sizei(max_used_main, cross_cursor)
		                                                 : core::sizei(cross_cursor, max_used_main);
		return result;
	}

	core::sizei layout_flex_elements(const std::vector<element_ptr>& elements, render::measure_context& mc,
	                                 const core::recti bounds, const flex_container_layout& container)
	{
#ifndef NDEBUG
		core::assert_true(elements.size() == std::unordered_set<element_ptr>(elements.begin(), elements.end()).size());
#endif
		const auto calculated = calc_flex_layout(elements, mc, bounds.extent(), container);
		for (auto i = 0u; i < elements.size(); ++i)
		{
			elements[i]->layout(mc, calculated.layout_bounds[i].offset(bounds.top_left()));
		}
		return calculated.extent;
	}
}
