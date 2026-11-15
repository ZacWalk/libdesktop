// This file is part of libdesktop, a C++20 platform layer for desktop applications
// Copyright 2026  Zac Walker
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

// Purpose: The geometry both renderers draw a polygon with: its outline cleaned of repeated points, its
// corners and the outlines offset from them - the border's band, mitred or bevelled, and the fringe an
// anti-aliased edge fades across - and, for a renderer that draws triangles, the outline cut into them.

#pragma once

namespace render::polygon
{
	// A corner whose mitre would reach past this many half widths is bevelled instead.
	constexpr double miter_limit = 4.0;

	// Twice the area the outline encloses, positive when it turns the way a left normal points inward.
	double twice_signed_area(std::span<const core::pointd> outline);

	// The outline without repeated, closing or non-finite points; false when it encloses nothing.
	bool clean_outline(std::span<const core::pointd> outline, std::vector<core::pointd>& result);

	struct corner
	{
		core::pointd point;
		// Unit normals, to the left of travel, of the edges arriving at and leaving the corner.
		core::pointd normal_in;
		core::pointd normal_out;
		// The move, per unit offset, that keeps both edges' offsets meeting, held to miter_limit.
		core::pointd miter;
		// The side, +1 or -1, a corner too sharp for a mitre is bevelled on; 0 for neither, 2 for both.
		int bevel_side = 0;
	};

	std::vector<corner> corners(std::span<const core::pointd> outline);

	// An outline moved a distance to the left of travel, or to the right when negative: a point for each
	// corner, or two where a corner is bevelled on that side and bevels are wanted. Corner i's points start at
	// first[i], and first.back() is the count.
	struct offset_outline
	{
		std::vector<core::pointd> points;
		std::vector<uint32_t> first;
	};

	offset_outline offset(const std::vector<corner>& corners, double distance, bool bevels);

	// The band between two offsets of one outline, as quadrilaterals: one at each corner where either side is
	// bevelled, and one along each edge. Each is handed as a and b on the first outline, then c and d on the
	// second, d opposite a.
	template <typename Emit> void band(const offset_outline& from, const offset_outline& to, Emit&& emit)
	{
		const auto count = from.first.size() - 1;

		for (size_t i = 0; i < count; ++i)
		{
			const auto next = (i + 1) % count;
			const auto from_first = from.first[i];
			const auto from_last = from.first[i + 1] - 1;
			const auto to_first = to.first[i];
			const auto to_last = to.first[i + 1] - 1;

			if (from_first != from_last || to_first != to_last)
			{
				emit(from.points[from_first], from.points[from_last], to.points[to_last], to.points[to_first]);
			}

			emit(from.points[from_last], from.points[from.first[next]], to.points[to.first[next]], to.points[to_last]);
		}
	}

	// The border's band, as quadrilaterals all turned the same way, so that where they overlap the non-zero
	// rule takes them once: four points each.
	void border_pieces(const std::vector<corner>& corners, double width, std::vector<core::pointd>& points);

	// Cuts a clean outline into triangles, three indices into it each, turned as the outline turns. False
	// when it had to cut where no clean cut was left - an outline that crosses itself - though every corner
	// is still cut away.
	bool triangulate(std::span<const core::pointd> outline, std::vector<uint32_t>& triangles);

	// The triangles a renderer that draws them fills and borders a polygon with: each vertex's position and
	// how much of the pixel an anti-aliased edge leaves covered there. The fill's come first.
	struct mesh
	{
		struct vertex
		{
			core::pointd position;
			float coverage = 1.0f;
		};

		std::vector<vertex> vertices;
		std::vector<uint32_t> indices;
		// The border's first vertex and first index.
		size_t border_vertex = 0;
		size_t border_index = 0;

		void clear()
		{
			vertices.clear();
			indices.clear();
			border_vertex = 0;
			border_index = 0;
		}
	};

	// Empty when the polygon draws nothing.
	void build_mesh(std::span<const core::pointd> outline, const polygon_style& style, mesh& result);

	// The pixels a polygon can touch: its outline's bounds with the border and an anti-aliased edge's fringe.
	core::recti bounds(std::span<const core::pointd> outline, const polygon_style& style);
}
