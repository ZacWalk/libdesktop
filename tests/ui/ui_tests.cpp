// libdesktop-ui's one test runner.

#include "libdesktop/ui.h"
#include "test.h"

#include <clocale>

void register_ui_render_tests(test_registry& tests);
void register_ui_render_primitives_tests(test_registry& tests);
void register_render_controls_tests(test_registry& tests);
void register_render_elements_tests(test_registry& tests);
void register_ui_menu_bar_tests(test_registry& tests);
void register_ui_text_tests(test_registry& tests);
void register_ui_view_tests(test_registry& tests);
void register_ui_list_view_tests(test_registry& tests);
void register_ui_doc_view_tests(test_registry& tests);
void register_ui_markdown_view_tests(test_registry& tests);
#ifdef _WIN32
void register_ui_render_win_tests(test_registry& tests);
#endif
void register_text_tests(test_registry& tests);

static void register_tests(test_registry& tests)
{
#ifdef _WIN32
	register_ui_render_win_tests(tests);
#endif
	register_ui_render_tests(tests);
	register_ui_render_primitives_tests(tests);
	register_render_controls_tests(tests);
	register_render_elements_tests(tests);
	register_ui_menu_bar_tests(tests);
	register_ui_text_tests(tests);
	register_ui_view_tests(tests);
	register_ui_list_view_tests(tests);
	register_ui_doc_view_tests(tests);
	register_ui_markdown_view_tests(tests);
	register_text_tests(tests);
}

int main(const int argc, char* argv[])
{
	core::set_app_name("LibDesktop-UI-Tests");
#ifdef _WIN32
	std::setlocale(LC_ALL, "en_US.UTF-8");
#else
	std::setlocale(LC_CTYPE, "C.UTF-8");
#endif
	const platform::thread_init thread;
	ui::set_chrome_icons({});
	platform::set_ui_thread();
	platform::start_log();

	struct registry final : test_registry
	{
		std::vector<std::pair<std::string, std::function<void()>>> tests;
		void add(std::string name, std::function<void()> f) override
		{
			tests.emplace_back(std::move(name), std::move(f));
		}
	} all;

	register_tests(all);
	return run_registered_tests(all, argc, argv, [] { return std::vector<std::string>{}; });
}
