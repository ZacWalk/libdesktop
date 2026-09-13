#include "platform.h"

app_init_result app_init(const pf::window_frame_ptr&, std::span<const std::string_view> args)
{
	if (!args.empty() && args.back() == "self")
		return {false, pf::write_stdout_raw(pf::executable_path().view()) ? 0 : 1};
	char buffer[4096];
	for (;;)
	{
		const auto read = pf::read_stdin(buffer, sizeof(buffer));
		if (!read) break;
		if (!pf::write_stdout_raw(std::string_view(buffer, read))) return {false, 1};
	}
	return {false, 0};
}

void app_idle() {}
void app_destroy() {}
