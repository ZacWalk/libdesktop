// Test-only Win32 fixture for an unrelated inheritable handle in the parent.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <cstdint>

uintptr_t process_test_inheritable_handle()
{
	SECURITY_ATTRIBUTES attributes{sizeof(attributes), nullptr, TRUE};
	return reinterpret_cast<uintptr_t>(CreateEventW(&attributes, TRUE, FALSE, nullptr));
}

void process_test_close_handle(uintptr_t handle)
{
	CloseHandle(reinterpret_cast<HANDLE>(handle));
}
