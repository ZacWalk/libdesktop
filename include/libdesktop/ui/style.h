// This file is part of libdesktop, a C++20 platform layer for desktop applications
// Copyright 2026  Zac Walker
// 
// This program is free software; you can redistribute it and / or modify it
// under the terms of the LGPL License either version 2.1 or later.
// License details are available at https://www.gnu.org/licenses/lgpl-2.1.html
// This program is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY

// Purpose: UI palette, themes, and animation styling.

#pragma once

namespace ui
{
	namespace style
	{
		// The colours everything is drawn in, as one palette: the product's own, with the few the running
		// system theme decides.
		struct theme
		{
			core::color32 menu_background = 0;
			core::color32 menu_text = 0;
			core::color32 menu_shortcut_text = 0;

			core::color32 toolbar_background = 0;
			core::color32 sidebar_background = 0;
			core::color32 bubble_background = 0;
			core::color32 group_background = 0;

			core::color32 view_selected_background = 0;
			core::color32 view_background = 0;
			core::color32 view_text = 0;

			core::color32 dialog_background = 0;
			core::color32 dialog_text = 0;
			core::color32 dialog_selected_text = 0;
			core::color32 dialog_selected_background = 0;

			core::color32 button_background = 0;
			core::color32 edit_background = 0;
			core::color32 edit_text = 0;

			core::color32 desktop_background = 0;
			core::color32 important_background = 0;
			core::color32 warning_background = 0;
			core::color32 success_background = 0;
			core::color32 info_background = 0;
		};

		// The colours a platform takes from the running theme. The rest of the palette is the product's own
		// and identical on every platform.
		struct system_colors
		{
			core::color32 edit_background = 0x00ffffff;
			core::color32 edit_text = 0x00000000;
			core::color32 desktop_background = 0x00000000;
		};

		// The installed palette, which everything that draws reads.
		extern const theme& colors;

		// Installs the palette. The platform layer calls this once at startup, before anything draws.
		void install(const system_colors& system);
	}
	// Exponential decay rate for alpha fades, per second. Chosen as -ln(1 - 0.333) * 60 so that a
	// 60Hz frame reproduces exactly the fixed 0.333 per frame this replaced.
	constexpr float alpha_fade_rate = 24.33f;

	// Fraction of the remaining distance a fade closes this frame, recomputed once per frame from
	// real elapsed time. Without it a fade advances per frame, so its duration is set by how fast
	// the machine renders - the same fade runs twice as fast on a 120Hz display as on a 60Hz one.
	// Written by prepare_frame and read by every step() it drives, so it is UI-thread-owned like
	// animations_enabled and the animations map beside it.
	extern float animation_step_factor;

	inline bool step_color_alpha(core::color& value, const core::color target)
	{
		const auto delta = target - value;
		if (delta.abs_sum() <= core::color::color_epsilon) return false;
		value += render::animations_enabled ? delta * animation_step_factor : delta;
		return true;
	}

	inline int fade_alpha_step(const int current, const int target)
	{
		auto result = (current * 5 + target * 2) / 7;
		if (std::abs(result - target) <= 1) result = target;
		return result;
	}

	inline int gated_fade_alpha_step(const int current, const int target)
	{
		return render::animations_enabled ? fade_alpha_step(current, target) : target;
	}

	class animate_alpha
	{
		float _val = 0.0f;
		float _target = 0.0f;

	public:
		animate_alpha()
		{
		}

		animate_alpha(const float v) : _val(v), _target(v)
		{
		}

		float val() const
		{
			return _val;
		};

		float target() const
		{
			return _target;
		}

		void reset(const float v)
		{
			_target = _val = v;
		}

		void reset(const float v, const float t)
		{
			_target = t;
			_val = render::animations_enabled ? v : t;
		}

		void val(const float v)
		{
			_val = v;
		}

		void target(const float v)
		{
			_target = v;
			if (!render::animations_enabled) _val = v;
		}

		bool step()
		{
			const auto dd = _target - _val;

			if (!render::animations_enabled || std::abs(dd) < 0.001f)
			{
				const auto changed = _val != _target;
				_val = _target;
				return changed;
			}

			_val += dd * animation_step_factor;
			return true;
		}
	};

	extern std::unordered_map<void*, std::function<bool()>> animations;

}
