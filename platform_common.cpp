// platform_common.cpp — backend-independent parts of the platform layer.
// Must NOT include OS headers; this file compiles on every target.

#include "platform.h"

#include <vector>

namespace
{
	std::vector<pf::embedded_resource>& registry()
	{
		static std::vector<pf::embedded_resource> items;
		return items;
	}

	const pf::embedded_resource* find(const std::string_view name)
	{
		for (const auto& item : registry())
			if (item.name == name)
				return &item;

		return nullptr;
	}
}

void pf::register_embedded_resources(const embedded_resource* items, const size_t count)
{
	auto& all = registry();
	all.insert(all.end(), items, items + count);
}

std::span<const uint8_t> pf::embedded_resource_data(const std::string_view name)
{
	if (const auto* found = find(name))
		return {found->data, found->size};

	return {};
}

std::string_view pf::embedded_resource_text(const std::string_view name)
{
	if (const auto* found = find(name))
		return {reinterpret_cast<const char*>(found->data), found->size};

	return {};
}

// ── line_splitter ──────────────────────────────────────────────────────────────

namespace
{
	std::string_view without_trailing_cr(const std::string_view line)
	{
		return !line.empty() && line.back() == '\r' ? line.substr(0, line.size() - 1) : line;
	}
}

void pf::line_splitter::feed(const std::string_view chunk, const std::function<void(std::string_view)>& emit)
{
	size_t pos = 0;

	while (pos < chunk.size())
	{
		const auto newline = chunk.find('\n', pos);

		if (newline == std::string_view::npos)
		{
			const auto tail = chunk.substr(pos);

			if (discarding)
				return;

			if (buffer.size() + tail.size() > max_line_bytes)
			{
				buffer.clear();
				discarding = true;
				++discarded_count;
				return;
			}

			buffer.append(tail);
			return;
		}

		const auto piece = chunk.substr(pos, newline - pos);
		pos = newline + 1;

		if (discarding)
		{
			discarding = false;
			continue;
		}

		if (buffer.empty())
		{
			if (piece.size() > max_line_bytes)
			{
				++discarded_count;
				continue;
			}

			emit(without_trailing_cr(piece));
			continue;
		}

		if (buffer.size() + piece.size() > max_line_bytes)
		{
			buffer.clear();
			++discarded_count;
			continue;
		}

		buffer.append(piece);
		emit(without_trailing_cr(buffer));
		buffer.clear();
	}
}

void pf::line_splitter::flush(const std::function<void(std::string_view)>& emit)
{
	if (!discarding && !buffer.empty())
		emit(without_trailing_cr(buffer));

	buffer.clear();
	discarding = false;
}
