// Test-only Win32 fixture: observes handles and descendants without exposing OS types in pf::.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <cstdio>
#include <cstdlib>
#include <fcntl.h>
#include <io.h>
#include <string>
#include <vector>

std::string utf8(const wchar_t* text)
{
	const int size = WideCharToMultiByte(CP_UTF8, 0, text, -1, nullptr, 0, nullptr, nullptr);
	std::string result(size, '\0');
	WideCharToMultiByte(CP_UTF8, 0, text, -1, result.data(), size, nullptr, nullptr);
	result.pop_back();
	return result;
}

void record(const std::string& value)
{
	std::printf("%zu:", value.size());
	std::fwrite(value.data(), 1, value.size(), stdout);
	std::putchar('\n');
}

int wmain(int argc, wchar_t** argv)
{
	_setmode(_fileno(stdin), _O_BINARY);
	_setmode(_fileno(stdout), _O_BINARY);
	_setmode(_fileno(stderr), _O_BINARY);
	if (argc < 2) return 2;
	const std::wstring mode = argv[1];
	if (mode == L"args")
	{
		for (int i = 2; i < argc; ++i) record(utf8(argv[i]));
	}
	else if (mode == L"env")
	{
		for (int i = 2; i < argc; ++i)
		{
			wchar_t value[32768]{};
			GetEnvironmentVariableW(argv[i], value, 32768);
			record(utf8(value));
		}
		wchar_t cwd[32768]{};
		GetCurrentDirectoryW(32768, cwd);
		record(utf8(cwd));
	}
	else if (mode == L"echo")
	{
		char data[4096];
		for (;;)
		{
			const auto count = std::fread(data, 1, sizeof(data), stdin);
			if (!count) break;
			std::fwrite(data, 1, count, stdout);
			std::fflush(stdout);
		}
		std::fputs("closed\n", stderr);
	}
	else if (mode == L"streams")
	{
		const std::string out(4096, 'o'), err(4096, 'e');
		for (int i = 0; i != 128; ++i)
		{
			std::fwrite(out.data(), 1, out.size(), stdout);
			std::fwrite(err.data(), 1, err.size(), stderr);
		}
	}
	else if (mode == L"hold")
	{
		std::fputs("ready\n", stdout);
		std::fflush(stdout);
		Sleep(60000);
	}
	else if (mode == L"tree" || mode == L"tree-exit")
	{
		wchar_t exe[32768]{};
		GetModuleFileNameW(nullptr, exe, 32768);
		std::wstring command = L"\"" + std::wstring(exe) + L"\" hold";
		STARTUPINFOW startup{sizeof(startup)};
		startup.dwFlags = STARTF_USESTDHANDLES;
		startup.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
		startup.hStdOutput = GetStdHandle(STD_OUTPUT_HANDLE);
		startup.hStdError = GetStdHandle(STD_ERROR_HANDLE);
		PROCESS_INFORMATION child{};
		if (!CreateProcessW(exe, command.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW | CREATE_SUSPENDED,
			nullptr, nullptr, &startup, &child)) return 3;
		std::printf("%lu\n", child.dwProcessId);
		std::fflush(stdout);
		ResumeThread(child.hThread);
		CloseHandle(child.hThread);
		CloseHandle(child.hProcess);
		if (mode == L"tree") Sleep(60000);
	}
	else if (mode == L"watch" && argc == 3)
	{
		const auto pid = wcstoul(argv[2], nullptr, 10);
		HANDLE child = OpenProcess(SYNCHRONIZE, FALSE, pid);
		const bool dead = !child || WaitForSingleObject(child, 5000) == WAIT_OBJECT_0;
		if (child) CloseHandle(child);
		std::puts(dead ? "dead" : "alive");
		return dead ? 0 : 4;
	}
	else if (mode == L"handle" && argc == 3)
	{
		const auto value = _wcstoui64(argv[2], nullptr, 10);
		const auto handle = reinterpret_cast<HANDLE>(static_cast<uintptr_t>(value));
		std::puts(SetEvent(handle) ? "inherited" : "absent");
	}
	else return 5;
	return 0;
}
