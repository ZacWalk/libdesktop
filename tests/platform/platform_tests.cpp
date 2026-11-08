// libdesktop-platform's one test runner.

#include "libdesktop/platform.h"
#include "platform/test_support.h"
#include "test.h"

#include <clocale>

void register_platform_system_tests(test_registry& tests);
void register_platform_tasks_tests(test_registry& tests);
void register_platform_media_tests(test_registry& tests);
void register_platform_services_tests(test_registry& tests);
void register_platform_shell_tests(test_registry& tests);
#ifdef _WIN32
void register_os_tests(test_registry& tests);
#endif

static void register_tests(test_registry& tests)
{
	register_platform_tasks_tests(tests);
	register_platform_media_tests(tests);
	register_platform_services_tests(tests);
	register_platform_shell_tests(tests);
	register_platform_system_tests(tests);
#ifdef _WIN32
	register_os_tests(tests);
#endif
}

int main(const int argc, char* argv[])
{
	core::set_app_name("LibDesktop-Tests");
#ifdef _WIN32
	std::setlocale(LC_ALL, "en_US.UTF-8");
#else
	std::setlocale(LC_CTYPE, "C.UTF-8");
#endif
	const platform::thread_init thread;
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
	return run_registered_tests(all, argc, argv, [] { return _temps.delete_temps(); });
}
