// view_markdown.h — rendered Markdown that is still the text it came from.
//
// Two views were merged into this one. The backing store is the source UTF-8
// text_buffer, so every byte on screen sits at a real position in the document and
// selection, copy and search keep working; the md:: model supplies what the raw
// bytes cannot — what each line means, which lines deserve a bigger font, and which
// run of characters is a link.
//
// It draws the markers rather than hiding them. A view that hid them would have to
// lie about where the text is, and a selection that copies something other than
// what was highlighted is worse than a visible '#'.
//
// Tables go through table_layout, the same code the CSV view draws with.
//
// Columns are counted in code points, and a tab counts as one of them: wrapping,
// hit testing and drawing all have to agree, and agreeing matters more here than
// honouring a tab stop in a document that is mostly prose.

#pragma once

#include "ui/markdown.h"
#include "ui/spell.h"
#include "ui/table_layout.h"
#include "ui/view_doc_readonly.h"

#include <algorithm>
#include <functional>
#include <string>
#include <vector>

namespace pf::ui
{
	class markdown_view : public read_only_doc_view
	{
	public:
		// What a click on a link does. Resolving a target is application policy —
		// one application opens a browser, another answers its own scheme first — so
		// the view reports which link was hit and nothing more.
		using link_fn = std::function<void(std::string_view target)>;

		markdown_view(view_host& host, const theme& th, view_context* context = nullptr)
			: read_only_doc_view(host, th, context)
		{
			_sel_margin = false;
			_word_wrap = true;
		}

		~markdown_view() override = default;

		void set_link_handler(link_fn handler) { _on_link = std::move(handler); }
		[[nodiscard]] bool has_link_handler() const { return static_cast<bool>(_on_link); }

		// Which link was hit, as well as what it points at.
		//
		// An application that decides *per occurrence* whether a link may be
		// followed needs to know which one was clicked, not just its target: the
		// same target can appear twice in one document with different provenance,
		// and only one of them may be navigable. list0 is why this exists — a
		// citation verified in the turn that read it is not the same thing as the
		// same URI repeated in a turn that did not.
		struct link_hit
		{
			std::string target;
			int line = -1; // source line, or -1 when no link was hit
			int span = -1; // index into that line's parsed spans
		};

		[[nodiscard]] link_hit link_hit_at(const pf::ipoint& point) const
		{
			bool on_text = false;
			const auto loc = hit_test(point, &on_text);

			// Hit testing clamps, because a click below the last line still has to
			// select something. Following a link must not: empty space beside a
			// heading is not the heading's link.
			if (!on_text) return {};

			(*_doc)[loc.y].render(_link_buf);
			const std::string_view text(_link_buf);
			auto in_fence = _lines[loc.y].in_fence;
			md::parse_source_line(text, in_fence, _link_line);

			for (size_t i = 0; i < _link_line.spans.size(); ++i)
			{
				const auto& span = _link_line.spans[i];
				if (span.link.empty()) continue;

				// Both halves of "[text](url)" answer, so clicking anywhere in the
				// link follows it.
				if (offset_within(text, span.text, loc.x) || offset_within(text, span.link, loc.x))
					return {std::string(span.link), loc.y, static_cast<int>(i)};
			}

			return {};
		}

		// The link under a client point, or empty when there is none. Returned by
		// value: the caller may keep it, and the line it was read from is scratch.
		[[nodiscard]] std::string link_at(const pf::ipoint& point) const
		{
			return link_hit_at(point).target;
		}

		// This view hit-tests the layout it actually drew, so dragging out a
		// selection lands where the reader pointed.
		[[nodiscard]] bool allows_drag_selection() const override { return true; }

		// The cached tables and heights describe the previous document until the next
		// layout, so none of them may outlive the buffer they were measured from.
		void set_buffer(const text_buffer_ptr& d, highlight_fn highlight) override
		{
			_tables.clear();
			_lines.clear();
			_total_content_height = 0;
			read_only_doc_view::set_buffer(d, std::move(highlight));
		}

		void handle_size(pf::window_frame_ptr& window, const pf::isize extent,
		                 pf::measure_context& measure) override
		{
			// Heading metrics have to be known before the base calls layout()
			for (int level = 1; level <= heading_levels; ++level)
				_heading_extent[level - 1] = measure.measure_char(_theme.heading_font(level));

			doc_view::handle_size(window, extent, measure);
		}

		void layout() override
		{
			_wrap_dirty_all = false;
			_wrap_dirty_first = _wrap_dirty_last = -1;

			_wrap_breaks.clear();
			_wrap_offsets.clear();
			_wrap_line_y.clear();
			_tables.clear();
			_lines.clear();
			_total_visual_rows = 0;
			_total_content_height = 0;

			if (!_doc) return;

			const auto line_count = static_cast<int>(_doc->size());
			_wrap_offsets.assign(line_count + 1, 0);
			_wrap_line_y.assign(line_count + 1, 0);
			_lines.assign(line_count, line_metrics{});

			const auto base_cy = std::max(1, _font_extent.cy);
			// The vertical scrollbar is drawn over the view's right edge, so a row
			// stops short of it rather than running under the thumb. The room is
			// kept whether or not the bar can scroll, so the text does not re-flow
			// the moment it first outgrows the view.
			const auto avail_width = std::max(1, _view_extent.cx - left_pad() - right_pad());
			const auto table_cols = safe_cols(avail_width, _font_extent.cx);
			const auto line_pad = base_cy / 4;

			int y = 0;
			int rows = 0;
			bool in_fence = false;
			int i = 0;

			while (i < line_count)
			{
				(*_doc)[i].render(_line_buf);

				// A table is measured as a block: its rows share their columns
				if (!in_fence && is_table_row(_line_buf))
				{
					const auto table = find_table(i, table_cols);

					if (table.start_line >= 0 && table.start_line <= i)
					{
						const auto index = static_cast<int>(_tables.size());
						_tables.push_back(table);

						for (int tl = i; tl < table.end_line; tl++)
						{
							auto& row = _lines[tl];
							row.y = y;
							row.row_height = base_cy;
							row.char_width = std::max(1, _font_extent.cx);
							row.table = index;
							row.rows = tl == table.separator_line ? 1 : table_row_visual_rows(tl, table);
							row.height = row.rows * base_cy;

							_wrap_offsets[tl] = static_cast<int>(_wrap_breaks.size());
							_wrap_line_y[tl] = rows;

							y += row.height;
							rows += row.rows;
						}

						i = table.end_line;
						continue;
					}
				}

				const auto fence_state = in_fence;
				md::parse_source_line(_line_buf, in_fence, _source_line);

				auto& m = _lines[i];
				m.in_fence = fence_state;
				m.heading = heading_level(_source_line.kind);

				const auto extent = font_extent_for(m.heading);
				m.char_width = std::max(1, extent.cx);
				m.row_height = std::max(1, extent.cy);
				m.indent = continuation_indent(_source_line, _line_buf);

				const auto cols = std::max(1, safe_cols(avail_width, m.char_width) - m.indent);
				wrap_line_into(_line_breaks_buf, _line_buf, cols);

				_wrap_offsets[i] = static_cast<int>(_wrap_breaks.size());
				_wrap_breaks.insert(_wrap_breaks.end(), _line_breaks_buf.begin(), _line_breaks_buf.end());
				_wrap_line_y[i] = rows;

				m.rows = static_cast<int>(_line_breaks_buf.size()) + 1;
				m.lead = m.heading > 0 && i > 0 ? m.row_height / 2 : 0;
				m.trail = m.heading > 0 ? m.row_height / 3 : line_pad;
				m.y = y;
				m.height = m.lead + m.rows * m.row_height + m.trail;

				y += m.height;
				// The inherited scrolling arithmetic counts base rows, so a tall
				// heading is worth as many of those as it covers.
				rows += (m.height + base_cy - 1) / base_cy;
				i++;
			}

			_wrap_offsets[line_count] = static_cast<int>(_wrap_breaks.size());
			_wrap_line_y[line_count] = rows;
			_total_visual_rows = rows;
			_total_content_height = y;
		}

		void recalc_vert_scrollbar() override
		{
			_content_extent.cy = top_content_padding() + _total_content_height + bottom_content_padding();

			const int max_y = std::max(0, _content_extent.cy - (_view_extent.cy - text_top()));

			if (_scroll_offset.y > max_y)
			{
				_scroll_offset.y = max_y;
				_host.invalidate_view();
			}

			const int visible_height = std::max(0, _view_extent.cy - text_top());
			_vscroll.update(_content_extent.cy, visible_height, _scroll_offset.y);
		}

		// --- Where a line is, and what is under a point ---

		[[nodiscard]] int line_offset(const int lineIndex) const override
		{
			const auto count = laid_out_lines();
			if (count <= 0) return top_content_padding();
			return top_content_padding() + _lines[std::clamp(lineIndex, 0, count - 1)].y;
		}

		[[nodiscard]] int line_height(const int lineIndex) const override
		{
			const auto count = laid_out_lines();
			if (count <= 0) return _font_extent.cy;
			return _lines[std::clamp(lineIndex, 0, count - 1)].height;
		}

		int client_to_line(const pf::ipoint& point) const override
		{
			const auto count = laid_out_lines();
			if (count <= 0) return 0;

			// The last line that starts at or above the point is the line it is in —
			// the same search the paint loop starts from.
			const auto it = std::ranges::upper_bound(_lines.begin(), _lines.begin() + count,
			                                         content_y_of(point.y), {}, &line_metrics::y);
			const auto index = static_cast<int>(it - _lines.begin()) - 1;
			return std::clamp(index, 0, count - 1);
		}

		text_location client_to_text(const pf::ipoint& point) const override
		{
			return hit_test(point, nullptr);
		}

		pf::ipoint text_to_client(const text_location& point) const override
		{
			const auto count = laid_out_lines();
			if (count <= 0) return {};

			const auto line_index = std::clamp(point.y, 0, count - 1);
			const auto& m = _lines[line_index];

			(*_doc)[line_index].render(_render_buf);
			const std::string_view line_view(_render_buf);

			const auto breaks = line_breaks(line_index);
			const auto row = std::clamp(char_to_sub_row(line_index, point.x), 0, static_cast<int>(breaks.size()));
			const auto row_start = row == 0 ? 0 : clamp_offset(breaks[row - 1], line_view);
			const auto indent = row > 0 ? m.indent : 0;

			pf::ipoint pt;
			pt.y = top_content_padding() + m.y + m.lead + row * m.row_height - _scroll_offset.y + text_top();
			pt.x = left_pad() + (indent + column_of(line_view, row_start, point.x)) * m.char_width;
			return pt;
		}

		uint32_t handle_mouse(pf::window_frame_ptr window, const pf::mouse_message_type msg,
		                      const pf::mouse_params& params) override
		{
			// Where the press landed is what tells a click from a drag later
			if (msg == pf::mouse_message_type::left_button_down) _press_point = params.point;

			return read_only_doc_view::handle_mouse(window, msg, params);
		}

		void ensure_visible(pf::window_frame_ptr& window, const text_location& pt) override
		{
			const auto count = laid_out_lines();
			if (count <= 0) return;

			const auto index = std::clamp(pt.y, 0, count - 1);
			const auto top = line_offset(index);
			const auto bottom = top + _lines[index].height;
			const auto visible_height = _view_extent.cy - text_top();

			if (bottom > _scroll_offset.y + visible_height)
				set_scroll_pixel(bottom - visible_height);
			else if (top < _scroll_offset.y)
				set_scroll_pixel(top);
		}

	protected:
		// A press that neither moved nor selected anything is a click on whatever is
		// under it.
		void on_left_button_up(const pf::window_frame_ptr& window, const pf::ipoint& point) override
		{
			const auto on_scrollbar = _vscroll._tracking || _hscroll._tracking;
			const auto slop = std::max(2, _font_extent.cx / 2);
			const auto moved = std::abs(point.x - _press_point.x) > slop ||
				std::abs(point.y - _press_point.y) > slop;

			doc_view::on_left_button_up(window, point);

			if (!_on_link || on_scrollbar || moved || !_doc || _doc->has_selection()) return;

			if (const auto target = link_at(point); !target.empty())
				_on_link(target);
		}

		void update_cursor(const pf::window_frame_ptr& window) const override
		{
			if (_on_link && !_drag_selection)
			{
				const auto pt = window->screen_to_client(pf::platform_cursor_pos());
				const auto rc = scrollbar_rect();

				if (!_vscroll.hit_test(pt, rc) && !_hscroll.hit_test(pt, rc) && !link_at(pt).empty())
				{
					window->set_cursor_shape(pf::cursor_shape::hand);
					return;
				}
			}

			doc_view::update_cursor(window);
		}

		void draw_view(pf::window_frame_ptr& window, pf::draw_context& draw) const override
		{
			const auto rc_client = client_rect();

			draw.fill_solid_rect(rc_client, _theme.style_color(text_style::normal_bkgnd));

			if (_doc && laid_out_lines() > 0)
			{
				const auto line_count = laid_out_lines();
				auto line_index = std::clamp(client_to_line({0, text_top()}), 0, line_count - 1);
				auto y = line_offset(line_index) - _scroll_offset.y + text_top();

				while (y < rc_client.bottom && line_index < line_count)
				{
					const auto& m = _lines[line_index];

					if (m.table >= 0 && m.table < static_cast<int>(_tables.size()))
					{
						const auto& table = _tables[m.table];
						const auto end = std::min(table.end_line, line_count);

						for (int tl = line_index; tl < end && y < rc_client.bottom; tl++)
						{
							(*_doc)[tl].render(_line_buf);
							y += draw_table_line(draw, y, rc_client.right, tl, table) * _font_extent.cy;
						}

						line_index = std::max(end, line_index + 1);
						continue;
					}

					draw_markdown_line(draw, y, rc_client.right, line_index, m);
					y += m.height;
					line_index++;
				}
			}

			_vscroll.draw(draw, scrollbar_rect());
			draw_message_bar(draw);
		}

	private:
		static constexpr int heading_levels = 3;

		// What layout decided about one source line. Cached so drawing, hit testing
		// and scrolling all agree about where a line is without parsing it again.
		struct line_metrics
		{
			int y = 0;             // content-space top, before the space above a heading
			int height = 0;        // everything the line occupies, including that space
			int lead = 0;          // blank space above the text
			int trail = 0;         // and below it
			int rows = 0;          // visual rows the text wrapped into
			int row_height = 0;    // pixel height of one of those rows
			int char_width = 0;    // and the width of a character in it
			int indent = 0;        // columns a wrapped continuation is indented by
			int table = -1;        // index into _tables, or -1
			uint8_t heading = 0;   // 1..3, or 0 for body text
			bool in_fence = false; // fenced-code state at the start of this line
		};

		// A run of source bytes that share a meaning, and one that shares a colour
		// once spell checking has had its say.
		struct style_run
		{
			int start = 0;
			int length = 0;
			text_style style = text_style::normal_text;
		};

		struct text_run
		{
			int start = 0;
			int length = 0;
			pf::color_t color;
		};

		link_fn _on_link;
		pf::ipoint _press_point{};
		pf::isize _heading_extent[heading_levels] = {};
		std::vector<line_metrics> _lines;
		std::vector<table_layout::table_block> _tables;
		int _total_content_height = 0;

		// Layout and paint scratch, reused for every line so drawing allocates nothing
		mutable std::string _line_buf;
		mutable std::string _table_buf;
		mutable std::string _link_buf;
		mutable md::source_line _source_line;
		mutable md::source_line _link_line;
		mutable std::vector<uint8_t> _byte_style;
		mutable std::vector<style_run> _style_runs;
		mutable std::vector<text_run> _runs;
		mutable std::vector<int> _cell_breaks;
		mutable std::vector<std::string_view> _cells;

		[[nodiscard]] int left_pad() const { return _font_extent.cx * 2; }
		[[nodiscard]] int right_pad() const { return _vscroll.thumb_thickness() + 2 * _vscroll.edge_margin(); }

		// Lines this view has actually measured. The table lags the buffer between
		// an edit and the next layout, exactly as the inherited wrap arrays do, so
		// every lookup is bounded by both.
		[[nodiscard]] int laid_out_lines() const
		{
			return _doc ? std::min(static_cast<int>(_doc->size()), static_cast<int>(_lines.size())) : 0;
		}

		// A break point from a stale layout may sit past the end of the line it now
		// describes, so every one of them is clamped before it indexes anything.
		static int clamp_offset(const int offset, const std::string_view text)
		{
			return std::clamp(offset, 0, static_cast<int>(text.size()));
		}

		// Where a point lands, and — when asked — whether it landed on drawn text at
		// all. Hit testing has to clamp, because a click below the last line still
		// selects something; a caller that must not act on empty space, such as one
		// following a link, needs the difference.
		text_location hit_test(const pf::ipoint& point, bool* on_text) const
		{
			if (on_text) *on_text = false;

			const auto count = laid_out_lines();
			if (count <= 0) return {};

			text_location pt{0, client_to_line(point)};
			const auto& m = _lines[pt.y];

			(*_doc)[pt.y].render(_render_buf);
			const std::string_view line_view(_render_buf);
			const auto line_size = static_cast<int>(line_view.size());

			const auto breaks = line_breaks(pt.y);
			const auto y_in_line = content_y_of(point.y) - m.y;
			const auto row = std::clamp((y_in_line - m.lead) / std::max(1, m.row_height),
			                            0, static_cast<int>(breaks.size()));
			const auto row_start = row == 0 ? 0 : clamp_offset(breaks[row - 1], line_view);
			const auto row_end = row < static_cast<int>(breaks.size())
				                    ? std::max(row_start, clamp_offset(breaks[row], line_view))
				                    : line_size;

			const auto indent = row > 0 ? m.indent : 0;
			const auto row_left = left_pad() + indent * m.char_width;
			const auto column = std::max(0, (point.x - row_left) / m.char_width);

			// A table row is drawn padded into its columns, so its pixels are not
			// where the source says they are. Snapping to the ends of the line keeps
			// a drag over a table selecting whole source rows rather than nonsense.
			pt.x = m.table >= 0
				       ? (point.x < _view_extent.cx / 2 ? 0 : line_size)
				       : offset_at_column(line_view, row_start, row_end, column);

			if (on_text)
			{
				const auto rows_bottom = m.lead + m.rows * m.row_height;
				const auto on_row = point.y >= text_top() && y_in_line >= m.lead && y_in_line < rows_bottom;
				const auto on_column = point.x >= row_left && pt.x < row_end &&
					column_of(line_view, row_start, pt.x) >= column;

				*on_text = m.table < 0 && on_row && on_column;
			}

			return pt;
		}

		// Screen Y to content Y, which is what the line table is measured in
		[[nodiscard]] int content_y_of(const int screen_y) const
		{
			return screen_y - text_top() + _scroll_offset.y - top_content_padding();
		}

		[[nodiscard]] pf::isize font_extent_for(const uint8_t heading) const
		{
			if (heading >= 1 && heading <= heading_levels)
			{
				const auto& sz = _heading_extent[heading - 1];
				if (sz.cx > 0 && sz.cy > 0) return sz;
			}

			return _font_extent;
		}

		[[nodiscard]] pf::font font_for(const uint8_t heading) const
		{
			return heading >= 1 && heading <= heading_levels ? _theme.heading_font(heading) : body_font();
		}

		static uint8_t heading_level(const md::block kind)
		{
			switch (kind)
			{
			case md::block::heading1: return 1;
			case md::block::heading2: return 2;
			case md::block::heading3: return 3;
			default: return 0;
			}
		}

		// True when a byte offset falls inside a span of the line it was parsed from
		static bool offset_within(const std::string_view line, const std::string_view span, const int offset)
		{
			const auto start = static_cast<int>(span.data() - line.data());
			return offset >= start && offset < start + static_cast<int>(span.size());
		}

		// A wrapped list or quote continues under its own text rather than under its
		// marker, so the marker keeps pointing at one item.
		static int continuation_indent(const md::source_line& parsed, const std::string_view text)
		{
			switch (parsed.kind)
			{
			case md::block::bullet:
			case md::block::numbered:
			case md::block::quote:
				return static_cast<int>(pf::utf8_codepoint_count(
					text.substr(0, std::min(static_cast<size_t>(parsed.content_start), text.size()))));
			default:
				return 0;
			}
		}

		// --- Styling ---

		static text_style style_for_span(const md::span& s, const uint8_t heading,
		                                 const md::block kind)
		{
			if (!s.link.empty()) return text_style::md_link_text;
			if (s.code) return text_style::md_code;
			if (heading == 1) return text_style::md_heading1;
			if (heading == 2) return text_style::md_heading2;
			if (heading == 3) return text_style::md_heading3;
			if (s.bold) return text_style::md_bold;
			if (s.italic) return text_style::md_italic;

			// Quoted text is still text, so this is last: a link or a bold run
			// inside a quote is drawn as what it is.
			if (kind == md::block::quote) return text_style::md_quote;
			return text_style::normal_text;
		}

		static bool spell_check_style(const text_style s)
		{
			switch (s)
			{
			case text_style::md_marker:
			case text_style::md_bullet:
			case text_style::md_link_url:
			case text_style::md_code:
				return false;
			default:
				return true;
			}
		}

		// Every byte of the source gets a style, so nothing on screen is unaccounted
		// for: whatever no span claims is punctuation the markup needed.
		void build_style_runs(const std::string_view text, const md::source_line& parsed,
		                      const uint8_t heading) const
		{
			const auto len = static_cast<int>(text.size());
			_byte_style.assign(len, static_cast<uint8_t>(text_style::md_marker));

			const auto fill = [&](const int start, const int length, const text_style s)
			{
				const auto from = std::clamp(start, 0, len);
				const auto to = std::clamp(start + length, from, len);
				std::fill(_byte_style.begin() + from, _byte_style.begin() + to, static_cast<uint8_t>(s));
			};

			if (parsed.kind == md::block::bullet || parsed.kind == md::block::numbered)
			{
				if (const auto first = text.find_first_not_of(" \t"); first != std::string_view::npos)
					fill(static_cast<int>(first), parsed.content_start - static_cast<int>(first),
					     text_style::md_bullet);
			}

			for (const auto& span : parsed.spans)
			{
				// The URL half of "[text](url)" is source the model hands back only as
				// a link target, so it is styled from the view that points at it.
				if (!span.link.empty())
					fill(static_cast<int>(span.link.data() - text.data()),
					     static_cast<int>(span.link.size()), text_style::md_link_url);

				fill(static_cast<int>(span.text.data() - text.data()),
				     static_cast<int>(span.text.size()), style_for_span(span, heading, parsed.kind));
			}

			_style_runs.clear();

			for (int i = 0; i < len;)
			{
				int end = i + 1;
				while (end < len && _byte_style[end] == _byte_style[i]) end++;
				_style_runs.push_back({i, end - i, static_cast<text_style>(_byte_style[i])});
				i = end;
			}
		}

		// Splits a run again wherever a misspelling starts or ends
		void build_text_runs(const std::string_view text, const text_style style,
		                     const pf::color_t base_color) const
		{
			_runs.clear();

			const auto enabled = _doc && _doc->spell_check() && spell_check_style(style);
			const auto error_color = _theme.style_color(text_style::error_text);

			const auto push_run = [&](const int start, const int length, const pf::color_t color)
			{
				if (length <= 0) return;

				if (!_runs.empty() && _runs.back().start + _runs.back().length == start && _runs.back().color == color)
				{
					_runs.back().length += length;
					return;
				}

				_runs.push_back({start, length, color});
			};

			int pos = 0;
			const auto len = static_cast<int>(text.size());

			while (pos < len)
			{
				int next = pos + 1;
				auto color = base_color;

				if (spell::is_word_byte(text[pos]))
				{
					while (next < len && spell::is_word_byte(text[next])) next++;
					if (enabled && !spell::check_word(text.substr(pos, next - pos))) color = error_color;
				}
				else
				{
					while (next < len && !spell::is_word_byte(text[next])) next++;
				}

				push_run(pos, next - pos, color);
				pos = next;
			}
		}

		// --- Drawing ---

		void draw_run(pf::draw_context& draw, const pf::irect& rc, pf::ipoint& origin,
		              const std::string_view text, const int absolute_start, const pf::font& font,
		              const int font_cx, const pf::color_t color, const pf::color_t bg_color,
		              const int sel_begin, const int sel_end, const pf::color_t sel_text_color,
		              const pf::color_t sel_bg_color) const
		{
			const auto run_len = static_cast<int>(text.size());
			const auto s0 = std::clamp(sel_begin - absolute_start, 0, run_len);
			const auto s1 = std::clamp(sel_end - absolute_start, 0, run_len);
			auto clip = rc;

			const auto put = [&](const std::string_view part, const pf::color_t fg, const pf::color_t bg)
			{
				if (part.empty() || origin.x >= rc.right) return;
				clip.left = origin.x;
				draw.draw_text(origin.x, origin.y, clip, part, font, fg, bg);
				origin.x += font_cx * static_cast<int>(pf::utf8_codepoint_count(part));
			};

			put(text.substr(0, s0), color, bg_color);
			put(text.substr(s0, s1 - s0), sel_text_color, sel_bg_color);
			put(text.substr(s1), color, bg_color);
		}

		void draw_markdown_line(pf::draw_context& draw, const int top, const int right,
		                        const int line_index, const line_metrics& m) const
		{
			(*_doc)[line_index].render(_line_buf);
			const std::string_view text(_line_buf);

			auto in_fence = m.in_fence;
			md::parse_source_line(text, in_fence, _source_line);
			build_style_runs(text, _source_line, m.heading);

			const auto font = font_for(m.heading);
			const auto bg = _theme.style_color(text_style::normal_bkgnd);
			const auto sel_text_color = _theme.style_color(text_style::sel_text);
			const auto sel_bg_color = _theme.style_color(text_style::sel_bkgnd);
			const auto len = static_cast<int>(text.size());

			int sel_begin = 0;
			int sel_end = 0;

			if (const auto sel = _doc->selection().normalize(); !sel.empty())
			{
				const auto edge = [&](const text_location& at)
				{
					if (at.y > line_index) return len;
					if (at.y == line_index) return std::clamp(at.x, 0, len);
					return 0;
				};

				sel_begin = edge(sel._start);
				sel_end = edge(sel._end);
			}

			const auto breaks = line_breaks(line_index);
			auto y = top + m.lead;

			for (int row = 0; row <= static_cast<int>(breaks.size()); row++)
			{
				const auto row_start = row == 0 ? 0 : clamp_offset(breaks[row - 1], text);
				const auto row_end = row < static_cast<int>(breaks.size())
					                     ? std::max(row_start, clamp_offset(breaks[row], text))
					                     : len;
				const auto row_left = left_pad() + (row > 0 ? m.indent * m.char_width : 0);
				const pf::irect rc(row_left, y, right, y + m.row_height);
				pf::ipoint origin(rc.left, rc.top);

				for (const auto& run : _style_runs)
				{
					const auto run_end = run.start + run.length;
					if (run_end <= row_start || run.start >= row_end) continue;

					const auto from = std::max(run.start, row_start);
					const auto part = text.substr(from, std::min(run_end, row_end) - from);

					build_text_runs(part, run.style, _theme.style_color(run.style));

					for (const auto& piece : _runs)
					{
						draw_run(draw, rc, origin, part.substr(piece.start, piece.length),
						         from + piece.start, font, m.char_width, piece.color, bg,
						         sel_begin, sel_end, sel_text_color, sel_bg_color);
					}
				}

				y += m.row_height;
			}
		}

		// --- Tables ---

		static bool is_table_row(const std::string_view line)
		{
			const auto pos = line.find_first_not_of(' ');
			return pos != std::string_view::npos && line[pos] == u8'|';
		}

		static bool is_table_separator(const std::string_view line)
		{
			bool has_dash = false;
			bool has_pipe = false;

			for (const auto ch : line)
			{
				if (ch == u8'-') has_dash = true;
				else if (ch == u8'|') has_pipe = true;
				else if (ch != u8' ' && ch != u8':') return false;
			}

			return has_dash && has_pipe;
		}

		// Fills the caller's buffer, so wrapping a cell costs no allocation. A
		// continuation byte advances nothing, or a break could land inside a
		// character and table_layout would draw half of one.
		static void cell_breaks_fn(std::vector<int>& out, const std::string_view text, const int col_w)
		{
			calc_word_breaks_into(out, text, col_w, [&](const int i, int) -> int
			{
				return pf::is_utf8_continuation(text[i]) ? 0 : 1;
			});
		}

		table_layout::table_block find_table(const int line_hint, const int avail_cols) const
		{
			table_layout::table_block result;
			const auto line_count = static_cast<int>(_doc->size());
			if (line_hint < 0 || line_hint >= line_count) return result;

			auto& tmp = _table_buf;
			(*_doc)[line_hint].render(tmp);
			if (!is_table_row(tmp)) return result;

			int start = line_hint;
			while (start > 0)
			{
				(*_doc)[start - 1].render(tmp);
				if (!is_table_row(tmp)) break;
				start--;
			}

			int end = line_hint + 1;
			while (end < line_count)
			{
				(*_doc)[end].render(tmp);
				if (!is_table_row(tmp)) break;
				end++;
			}

			if (end - start < 2) return result; // a header and its separator, at the least

			(*_doc)[start + 1].render(tmp);
			if (!is_table_separator(tmp)) return result;

			result.start_line = start;
			result.end_line = end;
			result.separator_line = start + 1;

			for (int i = start; i < end; i++)
			{
				if (i == result.separator_line) continue;

				(*_doc)[i].render(tmp);
				table_layout::split_pipe_cells(_cells, tmp);

				while (result.col_widths.size() < _cells.size())
					result.col_widths.push_back(0);

				for (size_t c = 0; c < _cells.size(); c++)
				{
					const auto w = static_cast<int>(pf::utf8_codepoint_count(table_layout::trim_cell(_cells[c])));
					if (w > result.col_widths[c]) result.col_widths[c] = w;
				}
			}

			table_layout::cap_col_widths(result.col_widths, avail_cols);
			return result;
		}

		int table_row_visual_rows(const int line_index, const table_layout::table_block& table) const
		{
			(*_doc)[line_index].render(_table_buf);
			table_layout::split_pipe_cells(_cells, _table_buf);

			int rows = 1;

			for (size_t c = 0; c < table.col_widths.size() && c < _cells.size(); c++)
			{
				const auto vr = table_layout::cell_visual_rows(
					table_layout::trim_cell(_cells[c]), table.col_widths[c], _cell_breaks, cell_breaks_fn);
				if (vr > rows) rows = vr;
			}

			return rows;
		}

		// Returns the height in base rows the drawn row consumed
		int draw_table_line(pf::draw_context& draw, const int y, const int right,
		                    const int line_index, const table_layout::table_block& table) const
		{
			const auto font = body_font();
			const auto selected = line_fully_selected(line_index, static_cast<int>(_line_buf.size()));

			// A table row is selected whole or not at all — its cells are drawn
			// padded into their columns, so there is no honest half of one to shade.
			const auto bg = _theme.style_color(selected ? text_style::sel_bkgnd : text_style::normal_bkgnd);
			const auto marker = _theme.style_color(selected ? text_style::sel_text : text_style::md_marker);

			if (line_index == table.separator_line)
			{
				table_layout::draw_separator_row(draw, y, left_pad(), right, table, font,
				                                 _font_extent.cx, _font_extent.cy, bg, marker);
				return 1;
			}

			const auto is_header = line_index == table.start_line;
			const auto text_color = _theme.style_color(selected
				                                           ? text_style::sel_text
				                                           : is_header
				                                           ? text_style::md_bold
				                                           : text_style::normal_text);

			table_layout::split_pipe_cells(_cells, _line_buf);

			return table_layout::draw_table_row(draw, y, left_pad(), right, _cells, table, font,
			                                    _font_extent.cx, _font_extent.cy, is_header,
			                                    bg, marker, text_color, _cell_breaks, cell_breaks_fn);
		}

		// --- Column arithmetic ---

		static void wrap_line_into(std::vector<int>& breaks, const std::string_view text, const int cols)
		{
			calc_word_breaks_into(breaks, text, cols, [&](const int i, int) -> int
			{
				return pf::is_utf8_continuation(text[i]) ? 0 : 1;
			});
		}

		// The byte offset in [start, end) that sits at display column `column`
		static int offset_at_column(const std::string_view text, const int start, const int end, const int column)
		{
			int col = 0;
			int i = start;

			while (i < end)
			{
				if (!pf::is_utf8_continuation(text[i]))
				{
					col++;
					if (col > column) break;
				}

				i++;
			}

			return std::clamp(i, start, end);
		}

		// And the column a byte offset sits at, counting from `start`
		static int column_of(const std::string_view text, const int start, const int offset)
		{
			const auto to = std::min(offset, static_cast<int>(text.size()));
			int col = 0;

			for (int i = std::clamp(start, 0, to); i < to; i++)
				if (!pf::is_utf8_continuation(text[i])) col++;

			return col;
		}
	};
}
