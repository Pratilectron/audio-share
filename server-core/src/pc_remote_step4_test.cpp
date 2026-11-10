#include "pc_remote.hpp"
#include <cstdio>
#include <string>
#include <windows.h>

int main()
{
    auto before = pc_remote::sample_stats();
    if (before.memory_total_mb == 0 || before.processes.empty()) {
        std::printf("FAIL stats empty mem=%u procs=%zu\n", before.memory_total_mb, before.processes.size());
        return 1;
    }

    STARTUPINFOW si {};
    PROCESS_INFORMATION pi {};
    si.cb = sizeof(si);
    wchar_t cmd[] = L"notepad.exe";
    if (!CreateProcessW(nullptr, cmd, nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi)) {
        std::printf("FAIL spawn notepad\n");
        return 1;
    }
    CloseHandle(pi.hThread);
    Sleep(500);

    std::string error;
    bool ended = pc_remote::end_process(pi.dwProcessId, error);
    DWORD wait = WaitForSingleObject(pi.hProcess, 4000);
    DWORD code = 0;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hProcess);

    if (!ended) {
        std::printf("FAIL end_process: %s\n", error.c_str());
        return 1;
    }
    if (wait == WAIT_TIMEOUT || code == STILL_ACTIVE) {
        std::printf("FAIL notepad still running\n");
        return 1;
    }

    std::string protected_error;
    if (pc_remote::end_process(4, protected_error)) {
        std::printf("FAIL killed protected pid 4\n");
        return 1;
    }
    if (pc_remote::end_process(GetCurrentProcessId(), protected_error)) {
        std::printf("FAIL killed self\n");
        return 1;
    }

    std::printf("PASS cpu=%u mem=%u/%u procs=%zu ended_notepad=1\n",
                before.cpu_percent, before.memory_used_mb, before.memory_total_mb, before.processes.size());
    return 0;
}
