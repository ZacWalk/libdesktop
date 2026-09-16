#include "platform.h"

#ifdef _WINDOWS_
#error "platform.h must not include Windows headers"
#endif

#include <type_traits>

static_assert(std::is_same_v<decltype(pf::web_request{}.timeout_ms), uint32_t>);
static_assert(std::is_same_v<decltype(pf::web_request{}.max_response_bytes), size_t>);
static_assert(std::is_same_v<decltype(pf::web_response{}.error), pf::web_response_error>);
static_assert(std::is_same_v<decltype(pf::connect_to_host("localhost")), pf::web_host_ptr>);

app_init_result app_init(const pf::window_frame_ptr&, std::span<const std::string_view>) { return {false, 0}; }
void app_idle() {}
void app_destroy() {}

int main()
{
	const pf::web_request request;
	const pf::web_response legacy_response{"", "", "", 200};
	const auto invalid = pf::send_request({}, request);
	return !request.follow_redirects || !request.use_cookies || request.max_response_bytes ||
		request.max_header_bytes || request.timeout_ms || legacy_response.error != pf::web_response_error::none ||
		invalid.error != pf::web_response_error::invalid_request;
}
