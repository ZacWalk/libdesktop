// syntax.cpp — syntax highlighters for the text views.
//
// Each highlighter turns one line into styled runs and returns the cookie the next
// line starts from, so a construct spanning lines — a block comment, a fenced code
// block — continues correctly. They name a text_style rather than a colour, so a
// theme can repalette without touching a scanner.
//
// Choosing a highlighter for a document is the application's business, not this
// layer's: language_from_extension is offered as a convenience, but nothing here
// looks at a path.

#include "platform.h"
#include "ui/syntax.h"

#include <cctype>
#include <span>
#include <set>
#include <unordered_set>

namespace pf::ui::syntax
{
	// The scanners were written against this spelling; text_style is the type.
	using style = pf::ui::text_style;

static bool is_alnum32(const char c) { return isalnum(static_cast<unsigned char>(c)) != 0; }
static bool is_space32(const char c) { return isspace(static_cast<unsigned char>(c)) != 0; }
static bool is_digit32(const char c) { return isdigit(static_cast<unsigned char>(c)) != 0; }

enum class syntax_lang { cpp, rust, python, ps1 };

static bool is_keyword(const syntax_lang lang, const std::string_view text)
{
	if (text.size() > 30) return false;
	if (lang == syntax_lang::rust)
	{
		static const std::unordered_set<std::string_view> keywords = {
			"as", "break", "const", "continue", "crate", "else", "enum", "extern", "false", "fn",
			"for", "if", "impl", "in", "let", "loop", "match", "mod", "move", "mut", "pub",
			"ref", "return", "self", "Self", "static", "struct", "super", "trait", "true", "type",
			"unsafe", "use", "where", "while", "async", "await", "dyn", "abstract", "become", "box",
			"do", "final", "macro", "override", "priv", "typeof", "unsized", "virtual", "yield",
			"try"
		};
		return keywords.contains(text);
	}
	if (lang == syntax_lang::python)
	{
		static const std::unordered_set<std::string_view> keywords = {
			"False", "None", "True", "and", "as", "assert", "async", "await", "break", "class",
			"continue", "def", "del", "elif", "else", "except", "finally", "for", "from", "global",
			"if", "import", "in", "is", "lambda", "nonlocal", "not", "or", "pass", "raise",
			"return", "try", "while", "with", "yield"
		};
		return keywords.contains(text);
	}
	if (lang == syntax_lang::ps1)
	{
		static const std::unordered_set<std::string_view, pf::ihash, pf::ieq> keywords = {
			"begin", "break", "catch", "class", "continue", "data", "define", "do", "dynamicparam",
			"else", "elseif", "end", "enum", "exit", "filter", "finally", "for", "foreach", "from",
			"function", "hidden", "if", "in", "inlinescript", "parallel", "param", "process",
			"return", "sequence", "switch", "throw", "trap", "try", "until", "using", "var",
			"while", "workflow"
		};
		return keywords.contains(text);
	}
	static const std::unordered_set<std::string_view> keywords =
	{
		"__asm",
		"__based",
		"__cdecl",
		"__declspec",
		"__except",
		"__fastcall",
		"__finally",
		"__inline",
		"__int16",
		"__int32",
		"__int64",
		"__int8",
		"__leave",
		"__multiple_inheritance",
		"__single_inheritance",
		"__stdcall",
		"__try",
		"__uuidof",
		"__virtual_inheritance",
		"alignas",
		"alignof",
		"and",
		"and_eq",
		"asm",
		"auto",
		"bitand",
		"bitor",
		"bool",
		"break",
		"case",
		"catch",
		"char",
		"char16_t",
		"char32_t",
		"class",
		"compl",
		"const",
		"const_cast",
		"constexpr",
		"continue",
		"decltype",
		"default",
		"delete",
		"dllexport",
		"dllimport",
		"do",
		"double",
		"dynamic_cast",
		"else",
		"enum",
		"explicit",
		"export",
		"extern",
		"false",
		"float",
		"for",
		"friend",
		"goto",
		"if",
		"inline",
		"int",
		"interface",
		"long",
		"mutable",
		"naked",
		"namespace",
		"new",
		"noexcept",
		"not",
		"not_eq",
		"nullptr",
		"operator",
		"or",
		"or_eq",
		"private",
		"protected",
		"public",
		"register",
		"reinterpret_cast",
		"return",
		"short",
		"signed",
		"sizeof",
		"static",
		"static_assert",
		"static_cast",
		"struct",
		"switch",
		"template",
		"this",
		"thread_local",
		"throw",
		"true",
		"try",
		"typedef",
		"typeid",
		"typename",
		"union",
		"uint32_t",
		"using",
		"uuid",
		"virtual",
		"void",
		"volatile",
		"wchar_t",
		"while",
		"xor",
		"xor_eq"
	};

	return keywords.contains(text);
}

static bool is_number(const std::string_view text)
{
	const auto len = static_cast<int>(text.size());
	if (len == 0) return false;

	if (len > 2 && text[0] == '0' && text[1] == 'x')
	{
		for (auto i = 2; i < len; i++)
		{
			if (is_digit32(text[i]) || (text[i] >= 'A' && text[i] <= 'F') ||
				(text[i] >= 'a' && text[i] <= 'f'))
				continue;
			return false;
		}
		return true;
	}
	if (!is_digit32(text[0]))
		return false;

	int pos = 1;
	while (pos < len && is_digit32(text[pos])) pos++;

	if (pos < len && text[pos] == '.')
	{
		pos++;
		if (pos >= len || !is_digit32(text[pos])) return false;
		while (pos < len && is_digit32(text[pos])) pos++;
	}

	if (pos < len && (text[pos] == 'e' || text[pos] == 'E'))
	{
		pos++;
		if (pos < len && (text[pos] == '+' || text[pos] == '-')) pos++;
		if (pos >= len || !is_digit32(text[pos])) return false;
		while (pos < len && is_digit32(text[pos])) pos++;
	}

	return pos == len;
}

static constexpr int COOKIE_COMMENT = 0x0001;
static constexpr int COOKIE_PREPROCESSOR = 0x0002;
static constexpr int COOKIE_EXT_COMMENT = 0x0004;
static constexpr int COOKIE_STRING = 0x0008;
static constexpr int COOKIE_CHAR = 0x0010;


// Highlighter contract: every highlight_* function resets nActualItems to 0 and then fills buf.
static void add_block(const std::span<text_block> buf, int& nActualItems, const int pos, const style colorindex)
{
	// The bound is the caller's buffer, not a constant this file happens to pick.
	// An empty span asks for the cookie only, which is what the parse-ahead pass
	// over off-screen lines wants.
	if (nActualItems >= static_cast<int>(buf.size()))
		return;

	if (nActualItems == 0 || buf[nActualItems - 1]._char_pos <= pos)
	{
		buf[nActualItems]._char_pos = pos;
		buf[nActualItems]._color = colorindex;
		nActualItems++;
	}
}

// Everything that distinguishes one C-like language from another in the scanner below
struct syntax_rules
{
	syntax_lang lang;
	std::string_view line_comment;
	std::string_view block_open; // empty when the language has no block comment
	std::string_view block_close;
	char escape;
	bool hash_preprocessor; // '#' at the first non-space starts a directive
	bool dash_in_identifiers;
	bool line_continuation; // a trailing backslash carries the state to the next line
};

static constexpr syntax_rules cpp_rules{syntax_lang::cpp, "//", "/*", "*/", '\\', true, false, true};
static constexpr syntax_rules rust_rules{syntax_lang::rust, "//", "/*", "*/", '\\', false, false, false};
static constexpr syntax_rules python_rules{syntax_lang::python, "#", {}, {}, '\\', false, false, false};
static constexpr syntax_rules ps1_rules{syntax_lang::ps1, "#", "<#", "#>", '`', false, true, false};

// A two-character token is matched on its second character, so the block starts one back
static bool token_at(const std::string_view line, const int i, const std::string_view token, int& start)
{
	if (token.empty())
		return false;

	if (token.size() == 1)
	{
		if (line[i] != token[0]) return false;
		start = i;
		return true;
	}

	if (i == 0 || line[i] != token[1] || line[i - 1] != token[0]) return false;
	start = i - 1;
	return true;
}

static uint32_t highlight_code(const syntax_rules& rules, uint32_t dwCookie,
                               const std::string_view line_view, const std::span<text_block> buf, int& nActualItems)
{
	nActualItems = 0;

	if (line_view.empty())
		return dwCookie & COOKIE_EXT_COMMENT;

	const auto len = static_cast<int>(line_view.size());

	// Only a carried block comment can precede the '#' of a directive
	auto first_char = (dwCookie & ~COOKIE_EXT_COMMENT) == 0;
	auto redefine_block = true;
	auto dec_index = false;
	auto block_start = -1;
	auto i = 0;

	const auto is_identifier_char = [&rules](const char c)
	{
		return is_alnum32(c) || c == '_' || c == '.' || (rules.dash_in_identifiers && c == '-');
	};

	const auto flush_identifier = [&](const int end)
	{
		const auto word = line_view.substr(block_start, end - block_start);

		if (is_keyword(rules.lang, word))
			add_block(buf, nActualItems, block_start, style::code_keyword);
		else if (is_number(word))
			add_block(buf, nActualItems, block_start, style::code_number);
	};

	// An odd run of escape characters before a quote means the quote is escaped
	const auto quote_is_escaped = [&](const int pos)
	{
		auto count = 0;
		for (auto j = pos - 1; j >= 0 && line_view[j] == rules.escape; j--) count++;
		return count % 2 != 0;
	};

	for (i = 0;; i++)
	{
		if (redefine_block)
		{
			const auto pos = dec_index ? i - 1 : i;

			if (dwCookie & (COOKIE_COMMENT | COOKIE_EXT_COMMENT))
				add_block(buf, nActualItems, pos, style::code_comment);
			else if (dwCookie & (COOKIE_CHAR | COOKIE_STRING))
				add_block(buf, nActualItems, pos, style::code_string);
			else if (dwCookie & COOKIE_PREPROCESSOR)
				add_block(buf, nActualItems, pos, style::code_preprocessor);
			else
				add_block(buf, nActualItems, pos, style::normal_text);

			redefine_block = false;
			dec_index = false;
		}

		if (i == len)
			break;

		if (dwCookie & COOKIE_COMMENT)
		{
			add_block(buf, nActualItems, i, style::code_comment);
			break;
		}

		const auto c = line_view[i];

		if (dwCookie & COOKIE_STRING)
		{
			if (c == '"' && !quote_is_escaped(i))
			{
				dwCookie &= ~COOKIE_STRING;
				redefine_block = true;
			}
			continue;
		}

		if (dwCookie & COOKIE_CHAR)
		{
			if (c == '\'' && !quote_is_escaped(i))
			{
				dwCookie &= ~COOKIE_CHAR;
				redefine_block = true;
			}
			continue;
		}

		auto token_start = 0;

		if (dwCookie & COOKIE_EXT_COMMENT)
		{
			if (token_at(line_view, i, rules.block_close, token_start))
			{
				dwCookie &= ~COOKIE_EXT_COMMENT;
				redefine_block = true;
			}
			continue;
		}

		// Block comment before line comment, or PowerShell's '<#' reads as a '#' comment
		if (token_at(line_view, i, rules.block_open, token_start))
		{
			add_block(buf, nActualItems, token_start, style::code_comment);
			dwCookie |= COOKIE_EXT_COMMENT;
			continue;
		}

		if (token_at(line_view, i, rules.line_comment, token_start))
		{
			add_block(buf, nActualItems, token_start, style::code_comment);
			dwCookie |= COOKIE_COMMENT;
			break;
		}

		// The remainder of a directive keeps the directive colour
		if (dwCookie & COOKIE_PREPROCESSOR)
			continue;

		if (c == '"')
		{
			add_block(buf, nActualItems, i, style::code_string);
			dwCookie |= COOKIE_STRING;
			continue;
		}

		if (c == '\'')
		{
			add_block(buf, nActualItems, i, style::code_string);
			dwCookie |= COOKIE_CHAR;
			continue;
		}

		if (rules.hash_preprocessor && first_char)
		{
			if (c == '#')
			{
				add_block(buf, nActualItems, i, style::code_preprocessor);
				dwCookie |= COOKIE_PREPROCESSOR;
				continue;
			}
			if (!is_space32(c))
				first_char = false;
		}

		// Keywords and numbers only matter when blocks are being collected
		if (buf.empty())
			continue;

		if (is_identifier_char(c))
		{
			if (block_start == -1)
				block_start = i;
		}
		else if (block_start >= 0)
		{
			flush_identifier(i);
			redefine_block = true;
			dec_index = true;
			block_start = -1;
		}
	}

	if (block_start >= 0)
		flush_identifier(i);

	if (!rules.line_continuation || line_view[len - 1] != '\\')
		dwCookie &= COOKIE_EXT_COMMENT;

	return dwCookie;
}

uint32_t highlight_text(uint32_t dwCookie, const std::string_view line_view, const std::span<text_block> buf,
                        int& nActualItems)
{
	nActualItems = 0;

	if (!buf.empty())
	{
		const auto len = static_cast<int>(line_view.size());
		auto block_start = -1;

		for (auto i = 0; i <= len; i++)
		{
			if (i < len && is_alnum32(line_view[i]))
			{
				if (block_start == -1)
					block_start = i;
			}
			else
			{
				if (block_start >= 0)
				{
					const auto block_len = i - block_start;

					if (is_number(line_view.substr(block_start, block_len)))
					{
						add_block(buf, nActualItems, block_start, style::code_number);
					}
					else
					{
						add_block(buf, nActualItems, block_start, style::normal_text);
					}
				}

				block_start = -1;
			}
		}
	}

	return 0;
}

static int find_md_marker(const std::string_view text, const int start, const char ch, const int count)
{
	const auto len = static_cast<int>(text.size());
	for (int i = start; i <= len - count; i++)
	{
		bool match = true;
		for (int j = 0; j < count; j++)
		{
			if (text[i + j] != ch)
			{
				match = false;
				break;
			}
		}
		if (match)
			return i;
	}
	return -1;
}

static int find_md_char(const std::string_view text, const int start, const char ch)
{
	const auto len = static_cast<int>(text.size());
	for (int i = start; i < len; i++)
	{
		if (text[i] == ch)
			return i;
	}
	return -1;
}

static void parse_md_inline(const std::string_view text, const int start,
                            const std::span<text_block> buf, int& nActualItems)
{
	const auto len = static_cast<int>(text.size());
	int pos = start;

	while (pos < len)
	{
		// Bold: **text** or __text__
		if (pos + 1 < len && ((text[pos] == L'*' && text[pos + 1] == L'*') ||
			(text[pos] == L'_' && text[pos + 1] == L'_')))
		{
			const auto end = find_md_marker(text, pos + 2, text[pos], 2);
			if (end >= 0)
			{
				add_block(buf, nActualItems, pos, style::md_marker);
				add_block(buf, nActualItems, pos + 2, style::md_bold);
				add_block(buf, nActualItems, end, style::md_marker);
				if (end + 2 < len)
					add_block(buf, nActualItems, end + 2, style::normal_text);
				pos = end + 2;
				continue;
			}
		}

		// Italic: *text* or _text_
		if (text[pos] == L'*' || text[pos] == L'_')
		{
			const auto end = find_md_marker(text, pos + 1, text[pos], 1);
			if (end >= 0)
			{
				add_block(buf, nActualItems, pos, style::md_marker);
				add_block(buf, nActualItems, pos + 1, style::md_italic);
				add_block(buf, nActualItems, end, style::md_marker);
				if (end + 1 < len)
					add_block(buf, nActualItems, end + 1, style::normal_text);
				pos = end + 1;
				continue;
			}
		}

		// Link: [text](url)
		if (text[pos] == L'[')
		{
			const auto close_bracket = find_md_char(text, pos + 1, L']');
			if (close_bracket >= 0 && close_bracket + 1 < len && text[close_bracket + 1] == L'(')
			{
				const auto close_paren = find_md_char(text, close_bracket + 2, L')');
				if (close_paren >= 0)
				{
					add_block(buf, nActualItems, pos, style::md_marker);
					add_block(buf, nActualItems, pos + 1, style::md_link_text);
					add_block(buf, nActualItems, close_bracket, style::md_marker);
					add_block(buf, nActualItems, close_bracket + 2, style::md_link_url);
					add_block(buf, nActualItems, close_paren, style::md_marker);
					if (close_paren + 1 < len)
						add_block(buf, nActualItems, close_paren + 1, style::normal_text);
					pos = close_paren + 1;
					continue;
				}
			}
		}

		pos++;
	}
}

uint32_t highlight_markdown(uint32_t dwCookie, const std::string_view line_view, const std::span<text_block> buf,
                            int& nActualItems)
{
	nActualItems = 0;
	if (line_view.empty()) return 0;

	const auto len = static_cast<int>(line_view.size());
	int pos = 0;

	// Heading: # ## ###
	if (line_view[0] == u8'#')
	{
		int level = 0;
		while (pos < len && line_view[pos] == u8'#' && level < 3)
		{
			level++;
			pos++;
		}
		if (pos < len && line_view[pos] == u8' ')
		{
			add_block(buf, nActualItems, 0, style::md_marker);
			const auto heading_style = static_cast<style>(
				static_cast<int>(style::md_heading1) + level - 1);
			add_block(buf, nActualItems, pos + 1, heading_style);
			return 0;
		}
		pos = 0;
	}

	// Unordered list: - or *
	if (len >= 2 && (line_view[0] == u8'-' || line_view[0] == u8'*') && line_view[1] == u8' ')
	{
		add_block(buf, nActualItems, 0, style::md_bullet);
		add_block(buf, nActualItems, 2, style::normal_text);
		parse_md_inline(line_view, 2, buf, nActualItems);
		return 0;
	}

	// Ordered list: digits followed by ". "
	if (len >= 3 && line_view[0] >= u8'0' && line_view[0] <= u8'9')
	{
		int p = 0;
		while (p < len && line_view[p] >= u8'0' && line_view[p] <= u8'9') p++;
		if (p < len - 1 && line_view[p] == u8'.' && line_view[p + 1] == u8' ')
		{
			add_block(buf, nActualItems, 0, style::md_bullet);
			add_block(buf, nActualItems, p + 2, style::normal_text);
			parse_md_inline(line_view, p + 2, buf, nActualItems);
			return 0;
		}
	}

	// Normal line with inline formatting
	parse_md_inline(line_view, 0, buf, nActualItems);
	return 0;
}

static bool is_ps1_extension(const std::string_view ext)
{
	static const std::set<std::string_view, pf::iless> extensions = {
		"ps1", "psm1", "psd1"
	};
	return extensions.contains(ext);
}

static bool is_python_extension(const std::string_view ext)
{
	static const std::set<std::string_view, pf::iless> extensions = {
		"py", "pyw"
	};
	return extensions.contains(ext);
}

static bool is_rust_extension(const std::string_view ext)
{
	static const std::set<std::string_view, pf::iless> extensions = {
		"rs"
	};
	return extensions.contains(ext);
}

static bool is_cpp_extension(const std::string_view ext)
{
	static const std::set<std::string_view, pf::iless> extensions = {
		"c", "cpp", "cxx", "cc", "h", "hh", "hpp", "hxx", "inl", "ixx", "in"
	};

	return extensions.contains(ext);
}

	language language_from_extension(std::string_view ext)
	{
		if (!ext.empty() && ext.starts_with('.')) ext = ext.substr(1);
		if (is_cpp_extension(ext)) return language::cpp;
		if (is_rust_extension(ext)) return language::rust;
		if (is_python_extension(ext)) return language::python;
		if (is_ps1_extension(ext)) return language::powershell;
		return language::plain;
	}

	highlight_fn for_language(const language lang)
	{
		// The rules are constexpr statics, so capturing a reference is safe.
		const auto code = [](const syntax_rules& rules)
		{
			return [&rules](const uint32_t cookie, const std::string_view line, const std::span<text_block> buf, int& count)
			{
				return highlight_code(rules, cookie, line, buf, count);
			};
		};

		switch (lang)
		{
		case language::cpp: return code(cpp_rules);
		case language::rust: return code(rust_rules);
		case language::python: return code(python_rules);
		case language::powershell: return code(ps1_rules);
		case language::markdown: return highlight_markdown;
		case language::plain: break;
		}
		return highlight_text;
	}
}
