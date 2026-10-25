// markdown.h — a small Markdown model, parser and HTML-to-Markdown reducer.
//
// A document owns exactly one UTF-8 buffer; every line and every span is a
// string_view into it, so rendering never copies text. Pure: no I/O, no UI, no
// globals, which is why the whole file is directly testable.
//
// The model carries no notion of trust. An application that needs to know where a
// document came from, or which of its links it has verified, keeps that beside the
// document rather than inside it — a shared type with a field meaning "this content
// is trusted" invites another application to set it and believe it.

#pragma once

#include <cctype>
#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace pf::ui::md
{
	enum class block : uint8_t
	{
		blank,
		paragraph,
		heading1,
		heading2,
		heading3,
		quote,
		bullet,
		numbered,
		code,
		rule,
	};

	struct span
	{
		std::string_view text;
		std::string_view link; // non-empty when the span is a hyperlink
		bool bold = false;
		bool italic = false;
		bool code = false;
	};

	struct line
	{
		block kind = block::paragraph;
		std::vector<span> spans;
	};

	// One line of source, analysed without discarding any of it.
	//
	// parse() builds a document by throwing the markers away, which is what a
	// renderer that lays the model out afresh wants. A view that draws the source
	// text itself — so a reader can select and copy exactly what was written — needs
	// to know where the marker ends rather than never to see it. Every span points
	// into the line it was handed, so a span's byte offset in that line is
	// span.text.data() - line.data().
	struct source_line
	{
		block kind = block::paragraph;
		bool fence = false;    // the ``` line that opens or closes a code block
		int content_start = 0; // byte offset of the text the marker introduces
		std::vector<span> spans;
	};

	// The buffer is shared rather than held by value: a std::string member
	// would relocate its small-string contents on a move and dangle every view.
	struct document
	{
		std::shared_ptr<const std::string> text;
		std::vector<line> lines;
		std::vector<std::shared_ptr<const std::string>> fragments;

		[[nodiscard]] bool empty() const { return lines.empty(); }
	};

	namespace detail
	{
		constexpr bool is_space(const char c)
		{
			return c == ' ' || c == '\t' || c == '\r' || c == '\n';
		}

		constexpr bool starts_with(const std::string_view s, const std::string_view p)
		{
			return s.size() >= p.size() && s.compare(0, p.size(), p) == 0;
		}

		inline bool iequals(const std::string_view s, const char* b)
		{
			size_t i = 0;
			for (; i < s.size() && b[i]; ++i)
			{
				if (tolower(static_cast<unsigned char>(s[i])) != tolower(static_cast<unsigned char>(b[i])))
					return false;
			}
			return i == s.size() && b[i] == 0;
		}

		constexpr bool is_rule(const std::string_view s)
		{
			if (s.size() < 3) return false;
			const char c = s.front();
			if (c != '-' && c != '*' && c != '_') return false;
			for (const char ch : s) if (ch != c) return false;
			return true;
		}

		// Length of an ordered-list marker — "1. " or "2) " — and 0 when there is
		// none. A digit run on its own is a number, not a list.
		constexpr size_t ordered_marker(const std::string_view s)
		{
			size_t i = 0;
			while (i < s.size() && s[i] >= '0' && s[i] <= '9') ++i;
			if (i == 0 || i + 1 >= s.size()) return 0;
			if (s[i] != '.' && s[i] != ')') return 0;
			return s[i + 1] == ' ' ? i + 2 : 0;
		}

		constexpr int hex_digit(const char c)
		{
			if (c >= '0' && c <= '9') return c - '0';
			if (c >= 'a' && c <= 'f') return c - 'a' + 10;
			if (c >= 'A' && c <= 'F') return c - 'A' + 10;
			return -1;
		}

		inline void append_utf8(std::string& out, uint32_t cp)
		{
			if (cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) cp = 0xFFFD;

			if (cp < 0x80)
			{
				out += static_cast<char>(cp);
			}
			else if (cp < 0x800)
			{
				out += static_cast<char>(0xC0 | cp >> 6);
				out += static_cast<char>(0x80 | (cp & 0x3F));
			}
			else if (cp < 0x10000)
			{
				out += static_cast<char>(0xE0 | cp >> 12);
				out += static_cast<char>(0x80 | (cp >> 6 & 0x3F));
				out += static_cast<char>(0x80 | (cp & 0x3F));
			}
			else
			{
				out += static_cast<char>(0xF0 | cp >> 18);
				out += static_cast<char>(0x80 | (cp >> 12 & 0x3F));
				out += static_cast<char>(0x80 | (cp >> 6 & 0x3F));
				out += static_cast<char>(0x80 | (cp & 0x3F));
			}
		}

		inline bool decode_entity(const std::string_view name, std::string& out)
		{
			if (name.empty()) return false;

			if (name.front() == '#')
			{
				size_t i = 1;
				uint32_t base = 10;
				if (i < name.size() && (name[i] == 'x' || name[i] == 'X'))
				{
					base = 16;
					++i;
				}
				if (i >= name.size()) return false;

				uint32_t cp = 0;
				for (; i < name.size(); ++i)
				{
					const int d = hex_digit(name[i]);
					if (d < 0 || static_cast<uint32_t>(d) >= base) return false;
					cp = cp * base + static_cast<uint32_t>(d);
					if (cp > 0x10FFFF) return false;
				}
				append_utf8(out, cp);
				return true;
			}

			static const struct
			{
				const char* name;
				const char* value;
			} named[] = {
				{"amp", "&"}, {"lt", "<"}, {"gt", ">"}, {"quot", "\""}, {"apos", "'"},
				{"nbsp", " "}, {"ndash", "\xe2\x80\x93"}, {"mdash", "\xe2\x80\x94"},
				{"hellip", "\xe2\x80\xa6"}, {"lsquo", "\xe2\x80\x98"}, {"rsquo", "\xe2\x80\x99"},
				{"ldquo", "\xe2\x80\x9c"}, {"rdquo", "\xe2\x80\x9d"}, {"bull", "\xe2\x80\xa2"},
				{"middot", "\xc2\xb7"}, {"copy", "\xc2\xa9"}, {"reg", "\xc2\xae"}, {"trade", "\xe2\x84\xa2"},
			};

			for (const auto& e : named)
			{
				if (name == e.name)
				{
					out += e.value;
					return true;
				}
			}
			return false;
		}

		// A bare URL in body text should still be clickable. Trailing sentence
		// punctuation is not part of the address.
		constexpr size_t url_end(const std::string_view s, const size_t from)
		{
			size_t e = from;
			while (e < s.size() && !is_space(s[e]) && s[e] != '<' && s[e] != '>' && s[e] != '"' && s[e] != ')') ++e;
			while (e > from)
			{
				const char c = s[e - 1];
				if (c != '.' && c != ',' && c != ';' && c != ':' && c != '!' && c != '?') break;
				--e;
			}
			return e;
		}

		// value of `name="..."` (or an unquoted token) inside a tag's attribute text
		inline std::string attribute(const std::string_view attrs, const char* name)
		{
			const size_t nlen = strlen(name);

			for (size_t i = 0; i + nlen <= attrs.size(); ++i)
			{
				if (_strnicmp(attrs.data() + i, name, nlen) != 0) continue;
				if (i > 0 && !is_space(attrs[i - 1])) continue;

				size_t j = i + nlen;
				while (j < attrs.size() && is_space(attrs[j])) ++j;
				if (j >= attrs.size() || attrs[j] != '=') continue;
				++j;
				while (j < attrs.size() && is_space(attrs[j])) ++j;
				if (j >= attrs.size()) return {};

				std::string_view raw;
				if (attrs[j] == '"' || attrs[j] == '\'')
				{
					const auto end = attrs.find(attrs[j], j + 1);
					if (end == std::string_view::npos) return {};
					raw = attrs.substr(j + 1, end - j - 1);
				}
				else
				{
					size_t end = j;
					while (end < attrs.size() && !is_space(attrs[end]) && attrs[end] != '>') ++end;
					raw = attrs.substr(j, end - j);
				}

				// Attribute values carry entities too — &amp; is common in query strings.
				std::string value;
				value.reserve(raw.size());
				for (size_t k = 0; k < raw.size();)
				{
					if (raw[k] == '&')
					{
						const auto semi = raw.find(';', k + 1);
						std::string decoded;
						if (semi != std::string_view::npos && semi - k <= 10 &&
							decode_entity(raw.substr(k + 1, semi - k - 1), decoded))
						{
							value += decoded;
							k = semi + 1;
							continue;
						}
					}
					value += raw[k++];
				}
				return value;
			}
			return {};
		}

		// Mailers fold long href values across lines; no URL wants the whitespace.
		inline std::string clean_url(const std::string& s)
		{
			std::string out;
			out.reserve(s.size());
			for (const char c : s) if (static_cast<unsigned char>(c) > ' ') out += c;
			return out;
		}

		// A link we would refuse to open must not be rendered as one either; the
		// gate that actually protects ShellExecute is parse::is_http_url.
		inline bool is_http_url(const std::string& s)
		{
			return _strnicmp(s.c_str(), "http://", 7) == 0 || _strnicmp(s.c_str(), "https://", 8) == 0;
		}

		constexpr bool is_escapable(const char c)
		{
			return c == '*' || c == '`' || c == '[' || c == '\\' ||
			       c == '#' || c == '>' || c == '-' || c == '+' || c == '_';
		}

		constexpr size_t nowhere = static_cast<size_t>(-1);

		// Where an open element started, plus enough state to undo it entirely if
		// it turns out to hold no text at all.
		struct mark_state
		{
			size_t pos = nowhere;
			size_t before = 0;
			int newlines = 0;
			bool space = false;
		};
	}

	// Makes arbitrary text safe to compose into a Markdown document. Callers
	// hold subjects and sender names that came off the wire, and those must
	// not be able to invent markup or break out onto a line of their own.
	inline std::string escape(const std::string_view text)
	{
		std::string out;
		out.reserve(text.size() + 8);

		for (const char c : text)
		{
			if (static_cast<unsigned char>(c) < ' ')
			{
				if (!out.empty() && out.back() != ' ') out += ' ';
				continue;
			}
			if (c == '*' || c == '`' || c == '[' || c == '\\') out += '\\';
			out += c;
		}

		while (!out.empty() && out.back() == ' ') out.pop_back();
		return out;
	}

	// Splits one already-stripped block of text into styled spans. Every span
	// points into `s`, so `s` must outlive them.
	inline void parse_inline(const std::string_view s, std::vector<span>& out)
	{
		bool bold = false;
		bool italic = false;
		size_t start = 0;

		const auto flush = [&](const size_t end)
		{
			if (end > start) out.push_back(span{s.substr(start, end - start), {}, bold, italic, false});
		};

		size_t i = 0;
		while (i < s.size())
		{
			// The backslash is dropped by ending the span before it and starting
			// the next one at the character it protects.
			if (s[i] == '\\' && i + 1 < s.size() && detail::is_escapable(s[i + 1]))
			{
				flush(i);
				start = i + 1;
				i += 2;
				continue;
			}

			if (s[i] == '*' && i + 1 < s.size() && s[i + 1] == '*')
			{
				flush(i);
				bold = !bold;
				i += 2;
				start = i;
				continue;
			}

			if (s[i] == '*')
			{
				flush(i);
				italic = !italic;
				start = ++i;
				continue;
			}

			if (s[i] == '`')
			{
				if (const auto close = s.find('`', i + 1); close != std::string_view::npos)
				{
					flush(i);
					out.push_back(span{s.substr(i + 1, close - i - 1), {}, bold, italic, true});
					i = close + 1;
					start = i;
					continue;
				}
			}

			if (s[i] == '[')
			{
				const auto close = s.find(']', i + 1);
				if (close != std::string_view::npos && close + 1 < s.size() && s[close + 1] == '(')
				{
					if (const auto paren = s.find(')', close + 2); paren != std::string_view::npos)
					{
						flush(i);
						out.push_back(span{
							s.substr(i + 1, close - i - 1),
							s.substr(close + 2, paren - close - 2),
							bold, italic, false
						});
						i = paren + 1;
						start = i;
						continue;
					}
				}
			}

			const bool at_boundary = i == 0 || detail::is_space(s[i - 1]) || s[i - 1] == '(' || s[i - 1] == '<';
			if (s[i] == 'h' && at_boundary &&
				(detail::starts_with(s.substr(i), "http://") || detail::starts_with(s.substr(i), "https://")))
			{
				const auto end = detail::url_end(s, i);
				flush(i);
				const auto url = s.substr(i, end - i);
				out.push_back(span{url, url, bold, italic, false});
				i = end;
				start = i;
				continue;
			}

			++i;
		}

		flush(s.size());
	}

	// Analyses one line of source in place. `in_fence` carries the fenced-code
	// state from the previous line, which is the only thing a line's meaning
	// depends on beyond its own text.
	//
	// Nothing is copied and nothing is dropped: `text` must outlive the spans.
	inline void parse_source_line(std::string_view text, bool& in_fence, source_line& out)
	{
		out.kind = block::paragraph;
		out.fence = false;
		out.content_start = 0;
		out.spans.clear();

		if (!text.empty() && text.back() == '\r') text.remove_suffix(1);

		const auto* const base = text.data();
		const auto offset_of = [base](const std::string_view s) { return static_cast<int>(s.data() - base); };

		auto body = text;
		while (!body.empty() && (body.front() == ' ' || body.front() == '\t')) body.remove_prefix(1);
		while (!body.empty() && (body.back() == ' ' || body.back() == '\t')) body.remove_suffix(1);

		if (detail::starts_with(body, "```"))
		{
			in_fence = !in_fence;
			out.fence = true;
			out.kind = block::code;
			out.content_start = offset_of(body);
			return;
		}

		if (in_fence)
		{
			out.kind = block::code;
			if (!text.empty()) out.spans.push_back(span{text, {}, false, false, true});
			return;
		}

		if (body.empty())
		{
			out.kind = block::blank;
			out.content_start = static_cast<int>(text.size());
			return;
		}

		if (detail::is_rule(body))
		{
			out.kind = block::rule;
			out.content_start = offset_of(body);
			return;
		}

		if (detail::starts_with(body, "### "))
		{
			out.kind = block::heading3;
			body.remove_prefix(4);
		}
		else if (detail::starts_with(body, "## "))
		{
			out.kind = block::heading2;
			body.remove_prefix(3);
		}
		else if (detail::starts_with(body, "# "))
		{
			out.kind = block::heading1;
			body.remove_prefix(2);
		}
		else if (detail::starts_with(body, "> "))
		{
			out.kind = block::quote;
			body.remove_prefix(2);
		}
		else if (body == ">")
		{
			out.kind = block::quote;
			body.remove_prefix(1);
		}
		else if (detail::starts_with(body, "- ") || detail::starts_with(body, "* ") ||
			detail::starts_with(body, "+ "))
		{
			out.kind = block::bullet;
			body.remove_prefix(2);
		}
		else if (const auto marker = detail::ordered_marker(body); marker > 0)
		{
			out.kind = block::numbered;
			body.remove_prefix(marker);
		}

		out.content_start = offset_of(body);
		parse_inline(body, out.spans);
	}

	// Takes ownership of the source text; the returned document's views point
	// into it.
	inline document parse(std::string source)
	{
		document doc;
		doc.text = std::make_shared<const std::string>(std::move(source));

		const std::string_view all(*doc.text);
		bool in_fence = false;
		bool prev_blank = true; // drops leading blank lines
		source_line parsed;

		for (size_t pos = 0; pos <= all.size();)
		{
			const auto eol = all.find('\n', pos);
			const auto raw = all.substr(pos, eol == std::string_view::npos ? std::string_view::npos : eol - pos);
			pos = (eol == std::string_view::npos) ? all.size() + 1 : eol + 1;

			parse_source_line(raw, in_fence, parsed);

			if (parsed.fence) continue; // the fence itself is punctuation, not content
			if (parsed.kind == block::blank && prev_blank) continue; // one blank separates; more add nothing

			line l;
			l.kind = parsed.kind;
			l.spans = std::move(parsed.spans);

			prev_blank = l.kind == block::blank;
			doc.lines.push_back(std::move(l));
		}

		while (!doc.lines.empty() && doc.lines.back().kind == block::blank) doc.lines.pop_back();
		return doc;
	}

	// Reduces HTML mail to the Markdown subset above. This is a text reducer,
	// not a parser: markup is discarded rather than interpreted, so nothing a
	// hostile message contains can become anything but text and a link URL.
	inline std::string from_html(const std::string_view html)
	{
		std::string out;
		out.reserve(html.size() / 2 + 64);

		detail::mark_state bold, italic, link;
		std::string href;
		int quote_depth = 0;
		bool pre = false;
		bool space_pending = false;
		int newlines = 2; // suppresses leading blank lines

		const auto ends_open = [&] { return out.empty() || out.back() == ' ' || out.back() == '\n'; };

		// Everything visible goes through here, so a pending space and the
		// quote prefix are each applied exactly once per line.
		const auto open_line = [&]
		{
			if (newlines > 0)
			{
				space_pending = false;
				if (quote_depth > 0) out += "> ";
			}
			if (space_pending)
			{
				if (!ends_open()) out += ' ';
				space_pending = false;
			}
			newlines = 0;
		};

		const auto put = [&](const char c)
		{
			open_line();
			out += c;
		};

		// Message text must never be able to invent markup of its own.
		const auto put_text = [&](const char c)
		{
			open_line();
			if (!pre && (c == '*' || c == '`' || c == '[' || c == '\\')) out += '\\';
			out += c;
		};

		const auto marker = [&](const char* text)
		{
			open_line();
			out += text;
		};

		const auto newline = [&](const int count)
		{
			space_pending = false;
			while (newlines < count)
			{
				out += '\n';
				++newlines;
			}
		};

		const auto emit_char = [&](const char c)
		{
			if (detail::is_space(c) && !pre)
			{
				space_pending = true;
				return;
			}
			if (pre && c == '\r') return;
			if (pre && c == '\n')
			{
				space_pending = false;
				out += '\n';
				++newlines;
				return;
			}
			put_text(c);
		};

		const auto open_mark = [&](detail::mark_state& m, const char* text)
		{
			if (m.pos != detail::nowhere) return; // the same element nested in itself adds nothing
			m.before = out.size();
			m.newlines = newlines;
			m.space = space_pending;
			open_line();
			m.pos = out.size();
			out += text;
		};

		// Returns the element's text with the whitespace markdown would not
		// tolerate inside markers moved back out, or nothing if it held none.
		const auto take_body = [&](const detail::mark_state& m, const size_t marker_len, std::string& body)
		{
			body = out.substr(m.pos + marker_len);
			size_t b = 0, e = body.size();
			while (b < e && detail::is_space(body[b])) ++b;
			while (e > b && detail::is_space(body[e - 1])) --e;
			const bool trailing = e < body.size();
			body = body.substr(b, e - b);
			return std::pair{b > 0, trailing};
		};

		const auto close_mark = [&](detail::mark_state& m, const char* text)
		{
			if (m.pos == detail::nowhere) return;

			std::string body;
			const auto [leading, trailing] = take_body(m, strlen(text), body);

			if (body.empty())
			{
				out.resize(m.before);
				newlines = m.newlines;
				space_pending = m.space;
			}
			else
			{
				out.resize(m.pos);
				if (leading && !ends_open()) out += ' ';
				out += text;
				out += body;
				out += text;
				newlines = 0;
				if (trailing) space_pending = true;
			}
			m = {};
		};

		const auto close_link = [&]
		{
			if (link.pos == detail::nowhere) return;

			std::string body;
			const auto [leading, trailing] = take_body(link, 1, body);

			out.resize(link.pos);
			if (leading && !ends_open()) out += ' ';
			out += '[';
			out += body.empty() ? href : body; // an image-only anchor still needs a label
			out += "](";
			out += href;
			out += ')';
			newlines = 0;
			if (trailing) space_pending = true;
			link = {};
			href.clear();
		};

		for (size_t i = 0; i < html.size();)
		{
			const char c = html[i];

			if (c == '<')
			{
				if (detail::starts_with(html.substr(i), "<!--"))
				{
					const auto end = html.find("-->", i + 4);
					if (end == std::string_view::npos) break;
					i = end + 3;
					continue;
				}

				// A '>' inside a quoted attribute value does not end the tag.
				size_t scan = i + 1;
				char quoted = 0;
				while (scan < html.size())
				{
					const char t = html[scan];
					if (quoted) { if (t == quoted) quoted = 0; }
					else if (t == '"' || t == '\'') quoted = t;
					else if (t == '>') break;
					++scan;
				}
				if (scan >= html.size()) break;

				auto tag = html.substr(i + 1, scan - i - 1);
				i = scan + 1;

				if (!tag.empty() && (tag.front() == '!' || tag.front() == '?')) continue;

				const bool closing = !tag.empty() && tag.front() == '/';
				if (closing) tag.remove_prefix(1);

				size_t n = 0;
				while (n < tag.size() && !detail::is_space(tag[n]) && tag[n] != '/') ++n;
				const auto name = tag.substr(0, n);
				const auto attrs = tag.substr(n);

				const auto is = [&name](const char* t) { return detail::iequals(name, t); };

				// Script and style bodies are not text; skip to the matching close.
				if (!closing && (is("script") || is("style") || is("head")))
				{
					const std::string end_tag = std::string("</") + std::string(name);
					size_t at = i;
					while (at + end_tag.size() <= html.size() &&
						_strnicmp(html.data() + at, end_tag.c_str(), end_tag.size()) != 0) ++at;
					if (at + end_tag.size() > html.size()) break;
					const auto end = html.find('>', at);
					i = end == std::string_view::npos ? html.size() : end + 1;
					continue;
				}

				if (is("br"))
				{
					if (pre)
					{
						out += '\n';
						++newlines;
						space_pending = false;
					}
					else newline(1);
				}
				else if (is("hr"))
				{
					newline(1);
					marker("---");
					newline(1);
				}
				else if (is("img"))
				{
					if (!closing) for (const char ch : detail::attribute(attrs, "alt")) emit_char(ch);
				}
				else if (is("li"))
				{
					newline(1);
					if (!closing) marker("- ");
				}
				else if (is("h1") || is("h2") || is("h3") || is("h4") || is("h5") || is("h6"))
				{
					newline(2);
					if (!closing) marker(name[1] == '1' ? "# " : name[1] == '2' ? "## " : "### ");
				}
				else if (is("pre"))
				{
					newline(closing ? 1 : 2);
					pre = !closing;
					marker("```");
					newline(closing ? 2 : 1);
				}
				else if (is("blockquote"))
				{
					newline(2);
					if (closing) { if (quote_depth > 0) --quote_depth; }
					else ++quote_depth;
				}
				else if (is("p") || is("div") || is("ul") || is("ol") || is("table") ||
					is("section") || is("article") || is("body") || is("form"))
				{
					newline(2);
				}
				else if (is("tr") || is("td") || is("th"))
				{
					newline(1);
				}
				else if (is("b") || is("strong"))
				{
					if (closing) close_mark(bold, "**");
					else open_mark(bold, "**");
				}
				else if (is("i") || is("em"))
				{
					if (closing) close_mark(italic, "*");
					else open_mark(italic, "*");
				}
				else if (is("a"))
				{
					if (closing) close_link();
					else if (link.pos == detail::nowhere)
					{
						href = detail::clean_url(detail::attribute(attrs, "href"));
						// A ')' would end the markdown link early; a scheme we
						// will not open must not look clickable at all.
						if (detail::is_http_url(href) && href.find(')') == std::string::npos)
							open_mark(link, "[");
						else href.clear();
					}
				}
				continue;
			}

			if (c == '&')
			{
				const auto semi = html.find(';', i + 1);
				std::string decoded;
				if (semi != std::string_view::npos && semi - i <= 10 &&
					detail::decode_entity(html.substr(i + 1, semi - i - 1), decoded))
				{
					for (const char ch : decoded) emit_char(ch);
					i = semi + 1;
					continue;
				}
			}

			emit_char(c);
			++i;
		}

		close_mark(italic, "*");
		close_mark(bold, "**");
		close_link();
		return out;
	}
}
