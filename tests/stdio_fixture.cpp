#include "platform.h"

app_init_result app_init(const pf::window_frame_ptr&, std::span<const std::string_view> args)
{
	if (!args.empty() && args.back() == "self")
		return {false, pf::stdio_write(pf::executable_path().view()) ? 0 : 1};
	if (!args.empty() && args.back() == "invalid-read")
		return {false, pf::stdio_write(pf::stdio_read(nullptr, 1) == -1 ? "error" : "unexpected") ? 0 : 1};
	char buffer[4096];
	for (;;)
	{
		const auto read = pf::stdio_read(buffer, sizeof(buffer));
		if (read < 0) return {false, 1};
		if (!read) break;
		if (!pf::stdio_write(std::string_view(buffer, static_cast<size_t>(read)))) return {false, 1};
	}
	return {false, 0};
}

void app_idle() {}
void app_destroy() {}
