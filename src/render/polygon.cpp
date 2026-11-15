// This file is part of libdesktop, a C++20 platform layer for desktop applications
// Copyright 2026  Zac Walker
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

// Purpose: Polygon geometry shared by the renderers, and the CPU rasterizer's scanline fill.

#include "libdesktop/render.h"
#include "libdesktop/render/software.h"
#include "polygon.h"

#include <cstring>

namespace
{
	core::pointd unit(const core::pointd v)
	{
		const auto length = std::sqrt(v.dot(v));
		return length > 1e-12 ? v / length : core::pointd{};
	}

	double cross(const core::pointd a, const core::pointd b, const core::pointd c)
	{
		return (b.X - a.X) * (c.Y - b.Y) - (b.Y - a.Y) * (c.X - b.X);
	}

	double edge_side(const core::pointd a, const core::pointd b, const core::pointd p)
	{
		return (b.X - a.X) * (p.Y - a.Y) - (b.Y - a.Y) * (p.X - a.X);
	}

	// Inside or on the edge of a triangle turned positively; the corners themselves are not inside.
	bool in_triangle(const core::pointd a, const core::pointd b, const core::pointd c, const core::pointd p)
	{
		if (p == a || p == b || p == c) return false;
		return edge_side(a, b, p) >= 0.0 && edge_side(b, c, p) >= 0.0 && edge_side(c, a, p) >= 0.0;
	}
}

double render::polygon::twice_signed_area(const std::span<const core::pointd> outline)
{
	auto area = 0.0;

	for (size_t i = 0; i < outline.size(); ++i)
	{
		const auto a = outline[i];
		const auto b = outline[(i + 1) % outline.size()];
		area += a.X * b.Y - b.X * a.Y;
	}

	return area;
}

bool render::polygon::clean_outline(const std::span<const core::pointd> outline, std::vector<core::pointd>& result)
{
	result.clear();

	for (const auto p : outline)
	{
		if (!std::isfinite(p.X) || !std::isfinite(p.Y)) continue;
		if (!result.empty() && result.back() == p) continue;
		result.push_back(p);
	}

	while (result.size() > 1 && result.back() == result.front()) result.pop_back();
	return result.size() >= 3 && std::abs(twice_signed_area(result)) > 1e-9;
}

std::vector<render::polygon::corner> render::polygon::corners(const std::span<const core::pointd> outline)
{
	const auto count = outline.size();
	std::vector<corner> result(count);

	for (size_t i = 0; i < count; ++i)
	{
		const auto p = outline[i];
		const auto in = unit(p - outline[(i + count - 1) % count]);
		const auto out = unit(outline[(i + 1) % count] - p);
		auto& c = result[i];

		c.point = p;
		c.normal_in = {-in.Y, in.X};
		c.normal_out = {-out.Y, out.X};

		// The offsets of both edges meet where the move's projection on each normal is one.
		const auto cos_turn = c.normal_in.dot(c.normal_out);

		if (cos_turn < -0.9999)
		{
			// The outline doubles back on itself: no mitre on either side.
			c.miter = c.normal_out;
			c.bevel_side = 2;
			continue;
		}

		auto miter = (c.normal_in + c.normal_out) / (1.0 + cos_turn);
		const auto length = std::sqrt(miter.dot(miter));

		if (length > miter_limit)
		{
			miter *= miter_limit / length;
			// The offsets part on the side away from the turn.
			c.bevel_side = in.X * out.Y - in.Y * out.X > 0.0 ? -1 : 1;
		}

		c.miter = miter;
	}

	return result;
}

render::polygon::offset_outline render::polygon::offset(const std::vector<corner>& corners, const double distance,
                                                        const bool bevels)
{
	offset_outline result;
	result.points.reserve(corners.size() + 8);
	result.first.reserve(corners.size() + 1);
	const auto side = distance > 0.0 ? 1 : distance < 0.0 ? -1 : 0;

	for (const auto& c : corners)
	{
		result.first.push_back(static_cast<uint32_t>(result.points.size()));

		if (bevels && side != 0 && (c.bevel_side == side || c.bevel_side == 2))
		{
			result.points.push_back(c.point + c.normal_in * distance);
			result.points.push_back(c.point + c.normal_out * distance);
		}
		else
		{
			result.points.push_back(c.point + c.miter * distance);
		}
	}

	result.first.push_back(static_cast<uint32_t>(result.points.size()));
	return result;
}

void render::polygon::border_pieces(const std::vector<corner>& corners, const double width,
                                    std::vector<core::pointd>& points)
{
	const auto half = width / 2.0;

	band(offset(corners, half, true), offset(corners, -half, true),
	     [&points](const core::pointd a, const core::pointd b, const core::pointd c, const core::pointd d)
	{
		const std::array piece{a, b, c, d};
		const auto area = twice_signed_area(piece);
		if (std::abs(area) < 1e-9) return;

		if (area > 0.0) points.insert(points.end(), piece.begin(), piece.end());
		else points.insert(points.end(), piece.rbegin(), piece.rend());
	});
}

bool render::polygon::triangulate(const std::span<const core::pointd> outline, std::vector<uint32_t>& triangles)
{
	const auto count = static_cast<uint32_t>(outline.size());
	if (count < 3) return false;

	// Every test below is made as if the outline turned positively.
	const auto turn = twice_signed_area(outline) >= 0.0 ? 1.0 : -1.0;
	const auto convex_at = [&](const uint32_t a, const uint32_t b, const uint32_t c)
	{
		return cross(outline[a], outline[b], outline[c]) * turn;
	};
	const auto emit = [&](const uint32_t a, const uint32_t b, const uint32_t c)
	{
		triangles.push_back(a);
		triangles.push_back(b);
		triangles.push_back(c);
	};

	auto convex = true;
	for (uint32_t i = 0; i < count && convex; ++i)
		convex = convex_at((i + count - 1) % count, i, (i + 1) % count) >= 0.0;

	if (convex)
	{
		for (uint32_t i = 1; i + 1 < count; ++i) emit(0, i, i + 1);
		return true;
	}

	std::vector<uint32_t> prev(count), next(count);
	std::vector<uint8_t> removed(count);
	// Only a corner that is not convex can lie inside an ear; one that becomes convex never turns back.
	std::vector<uint32_t> reflex;

	for (uint32_t i = 0; i < count; ++i)
	{
		prev[i] = (i + count - 1) % count;
		next[i] = (i + 1) % count;
		if (convex_at(prev[i], i, next[i]) <= 0.0) reflex.push_back(i);
	}

	const auto is_ear = [&](const uint32_t a, const uint32_t b, const uint32_t c)
	{
		if (convex_at(a, b, c) <= 0.0) return false;

		const auto pa = turn > 0.0 ? outline[a] : outline[c];
		const auto pc = turn > 0.0 ? outline[c] : outline[a];

		for (const auto r : reflex)
		{
			if (removed[r] || r == a || r == b || r == c) continue;
			if (in_triangle(pa, outline[b], pc, outline[r])) return false;
		}

		return true;
	};

	auto clean = true;
	auto remaining = count;
	auto i = 0u;
	auto stalled = 0u;

	while (remaining > 3)
	{
		const auto a = prev[i];
		const auto c = next[i];
		const auto turn_here = convex_at(a, i, c);
		const auto ear = turn_here == 0.0 || is_ear(a, i, c);

		if (!ear && stalled <= remaining)
		{
			i = c;
			++stalled;
			continue;
		}

		// With no clean ear left the outline crosses itself; cutting anyway still ends.
		if (!ear) clean = false;

		// A corner on a straight line encloses nothing, so it goes without a triangle.
		if (turn_here != 0.0) emit(a, i, c);
		removed[i] = 1;
		next[a] = c;
		prev[c] = a;
		--remaining;
		stalled = 0;
		i = a;
	}

	emit(prev[i], i, next[i]);
	return clean;
}

void render::polygon::build_mesh(const std::span<const core::pointd> outline, const polygon_style& style, mesh& result)
{
	result.clear();

	thread_local std::vector<core::pointd> points;
	thread_local std::vector<uint32_t> triangles;
	if (!clean_outline(outline, points)) return;

	const auto all_corners = corners(points);
	// A left normal points inward when the outline turns positively.
	const auto inward = twice_signed_area(points) > 0.0 ? 1.0 : -1.0;

	const auto add_band = [&result](const offset_outline& from, const float from_coverage, const offset_outline& to,
	                                const float to_coverage)
	{
		band(from, to, [&](const core::pointd a, const core::pointd b, const core::pointd c, const core::pointd d)
		{
			const auto first = static_cast<uint32_t>(result.vertices.size());
			result.vertices.push_back({a, from_coverage});
			result.vertices.push_back({b, from_coverage});
			result.vertices.push_back({c, to_coverage});
			result.vertices.push_back({d, to_coverage});

			for (const auto index : {0u, 1u, 2u, 0u, 2u, 3u}) result.indices.push_back(first + index);
		});
	};

	if (style.fill.is_visible())
	{
		triangles.clear();
		triangulate(points, triangles);

		if (style.smooth)
		{
			// The fill stops half a pixel inside its edge, and a fringe fades from there to half a pixel outside.
			const auto inside = offset(all_corners, 0.5 * inward, false);
			const auto outside = offset(all_corners, -0.5 * inward, false);

			for (const auto& p : inside.points) result.vertices.push_back({p, 1.0f});
			result.indices.insert(result.indices.end(), triangles.begin(), triangles.end());
			add_band(inside, 1.0f, outside, 0.0f);
		}
		else
		{
			for (const auto& p : points) result.vertices.push_back({p, 1.0f});
			result.indices.insert(result.indices.end(), triangles.begin(), triangles.end());
		}
	}

	result.border_vertex = result.vertices.size();
	result.border_index = result.indices.size();

	if (!style.has_border()) return;

	const auto half = style.border_width / 2.0;

	if (!style.smooth)
	{
		add_band(offset(all_corners, half, true), 1.0f, offset(all_corners, -half, true), 1.0f);
		return;
	}

	const auto left_fringe = offset(all_corners, half + 0.5, true);
	const auto right_fringe = offset(all_corners, -(half + 0.5), true);

	if (style.border_width >= 1.0)
	{
		const auto left = offset(all_corners, half - 0.5, true);
		const auto right = offset(all_corners, -(half - 0.5), true);

		if (half > 0.5) add_band(left, 1.0f, right, 1.0f);
		add_band(left, 1.0f, left_fringe, 0.0f);
		add_band(right, 1.0f, right_fringe, 0.0f);
	}
	else
	{
		// Thinner than a pixel: it covers that share of the pixels it crosses.
		const auto centre = offset(all_corners, 0.0, true);
		const auto coverage = static_cast<float>(style.border_width);
		add_band(centre, coverage, left_fringe, 0.0f);
		add_band(centre, coverage, right_fringe, 0.0f);
	}
}

core::recti render::polygon::bounds(const std::span<const core::pointd> outline, const polygon_style& style)
{
	auto min_x = std::numeric_limits<double>::max();
	auto min_y = std::numeric_limits<double>::max();
	auto max_x = std::numeric_limits<double>::lowest();
	auto max_y = std::numeric_limits<double>::lowest();

	for (const auto p : outline)
	{
		if (!std::isfinite(p.X) || !std::isfinite(p.Y)) continue;
		min_x = std::min(min_x, p.X);
		min_y = std::min(min_y, p.Y);
		max_x = std::max(max_x, p.X);
		max_y = std::max(max_y, p.Y);
	}

	if (min_x > max_x) return {};

	// A mitre reaches out to its limit, and an anti-aliased edge a pixel past that.
	const auto reach = (style.has_border() ? style.border_width / 2.0 * miter_limit : 0.0) + 2.0;
	const auto clamp = [](const double v)
	{
		return static_cast<int>(std::clamp(v, -1e9, 1e9));
	};
	return {clamp(std::floor(min_x - reach)), clamp(std::floor(min_y - reach)), clamp(std::ceil(max_x + reach)),
	        clamp(std::ceil(max_y + reach))};
}

void render::software_canvas::fill_contours(const std::span<const core::pointd> points,
                                            const std::span<const uint32_t> ends, const paint& fill,
                                            const bool smooth) const
{
	if (!fill.is_visible() || points.size() < 3) return;

	struct edge
	{
		double x0, y0, y1, dxdy;
		int direction;
	};

	thread_local std::vector<edge> edges;
	thread_local std::vector<size_t> active;
	thread_local std::vector<std::pair<double, int>> crossings;
	thread_local std::vector<float> coverage;
	// How many sample lines cross each pixel whole, as the differences between neighbours: a span adds one
	// where its whole pixels begin and takes it away where they end, so a long span costs two writes.
	thread_local std::vector<int> whole_runs;
	edges.clear();

	auto min_x = std::numeric_limits<double>::max();
	auto min_y = std::numeric_limits<double>::max();
	auto max_x = std::numeric_limits<double>::lowest();
	auto max_y = std::numeric_limits<double>::lowest();
	uint32_t begin = 0;

	for (const auto end : ends)
	{
		if (end > points.size() || end < begin + 3)
		{
			begin = end;
			continue;
		}

		for (auto i = begin; i < end; ++i)
		{
			auto a = points[i];
			auto b = points[i + 1 < end ? i + 1 : begin];
			if (!std::isfinite(a.X) || !std::isfinite(a.Y) || !std::isfinite(b.X) || !std::isfinite(b.Y)) continue;

			min_x = std::min(min_x, a.X);
			min_y = std::min(min_y, a.Y);
			max_x = std::max(max_x, a.X);
			max_y = std::max(max_y, a.Y);
			if (a.Y == b.Y) continue;

			auto direction = 1;
			if (a.Y > b.Y)
			{
				std::swap(a, b);
				direction = -1;
			}

			edges.push_back({a.X, a.Y, b.Y, (b.X - a.X) / (b.Y - a.Y), direction});
		}

		begin = end;
	}

	if (edges.empty()) return;

	const auto limit = [](const double v)
	{
		return static_cast<int>(std::clamp(v, -1e9, 1e9));
	};
	const auto area = clamp_to_clip(core::recti(limit(std::floor(min_x)), limit(std::floor(min_y)),
	                                            limit(std::ceil(max_x)) + 1, limit(std::ceil(max_y)) + 1));
	if (area.is_empty()) return;

	std::ranges::sort(edges, {}, &edge::y0);
	active.clear();
	coverage.assign(static_cast<size_t>(area.width()), 0.0f);
	whole_runs.assign(static_cast<size_t>(area.width()) + 1, 0);
	size_t next_edge = 0;

	// Smooth samples sixteen lines a row and takes each one's coverage exactly across; crisp samples the centre.
	const auto samples = smooth ? 16 : 1;
	const auto weight = 1.0f / static_cast<float>(samples);
	const auto solid = fill.kind == paint_kind::solid ? fill.color : core::color{};
	// An opaque colour over a whole pixel replaces it, as blend_over would.
	const auto opaque_solid = fill.kind == paint_kind::solid && solid.a >= 1.0f;
	const uint8_t solid_pixel[4] = {to_byte(solid.b), to_byte(solid.g), to_byte(solid.r), 255};

	for (auto y = area.top; y < area.bottom; ++y)
	{
		auto touched_left = area.right;
		auto touched_right = area.left;

		for (auto s = 0; s < samples; ++s)
		{
			const auto sample_y = y + (s + 0.5) / samples;

			while (next_edge < edges.size() && edges[next_edge].y0 <= sample_y) active.push_back(next_edge++);
			std::erase_if(active, [&](const size_t e) { return edges[e].y1 <= sample_y; });

			crossings.clear();
			for (const auto e : active)
			{
				const auto& ed = edges[e];
				crossings.emplace_back(ed.x0 + (sample_y - ed.y0) * ed.dxdy, ed.direction);
			}

			if (crossings.size() < 2) continue;
			std::ranges::sort(crossings, {}, &std::pair<double, int>::first);

			auto winding = 0;
			auto span_start = 0.0;

			for (const auto& [x, direction] : crossings)
			{
				const auto before = winding;
				winding += direction;

				if (before == 0 && winding != 0)
				{
					span_start = x;
					continue;
				}

				if (before == 0 || winding != 0) continue;

				if (!smooth)
				{
					// A centre on the span's left edge is inside, one on its right edge is not.
					const auto first = std::max(area.left, limit(std::ceil(span_start - 0.5)));
					const auto last = std::min(area.right, limit(std::ceil(x - 0.5)));
					for (auto px = first; px < last; ++px) coverage[px - area.left] = 1.0f;
					if (first < last)
					{
						touched_left = std::min(touched_left, first);
						touched_right = std::max(touched_right, last);
					}
					continue;
				}

				const auto x0 = std::max(span_start, static_cast<double>(area.left));
				const auto x1 = std::min(x, static_cast<double>(area.right));
				if (x1 <= x0) continue;

				const auto first = static_cast<int>(std::floor(x0));
				const auto last = static_cast<int>(std::floor(x1));
				touched_left = std::min(touched_left, first);
				touched_right = std::max(touched_right, std::min(area.right, last + 1));

				if (first == last)
				{
					coverage[first - area.left] += static_cast<float>(x1 - x0) * weight;
					continue;
				}

				// The line crosses the pixels between its ends whole, and the first too when it enters at that
				// pixel's left edge - as it does where a tile clips it - so a tile counts what the whole canvas does.
				auto whole = first;
				if (x0 > first)
				{
					coverage[first - area.left] += static_cast<float>(first + 1 - x0) * weight;
					++whole;
				}
				++whole_runs[whole - area.left];
				--whole_runs[last - area.left];
				if (last < area.right) coverage[last - area.left] += static_cast<float>(x1 - last) * weight;
			}
		}

		if (touched_left >= touched_right) continue;

		auto* p = pixel(touched_left, y);
		auto crossed_whole = 0;

		for (auto x = touched_left; x < touched_right; ++x, p += 4)
		{
			const auto i = x - area.left;
			crossed_whole += whole_runs[i];
			whole_runs[i] = 0;
			auto& value = coverage[i];
			const auto covered = std::min(value + static_cast<float>(crossed_whole) * weight, 1.0f);
			value = 0.0f;
			if (covered < 1.0f / 512.0f) continue;

			if (opaque_solid && covered >= 1.0f)
			{
				std::memcpy(p, solid_pixel, sizeof(solid_pixel));
				continue;
			}

			const auto c = fill.kind == paint_kind::solid ? solid : fill.at(fill.position({x + 0.5, y + 0.5}));
			blend_over(p, c, covered, _opaque);
		}

		whole_runs[touched_right - area.left] = 0;
	}
}
