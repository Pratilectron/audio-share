#include "pc_remote.hpp"
#include <cstdio>
#include <string>
#include <windows.h>

static void pump(DWORD ms)
{
    MSG msg;
    auto end = GetTickCount64() + ms;
    while (GetTickCount64() < end) {
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        Sleep(5);
    }
}

int main()
{
    HWND edit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
                                WS_OVERLAPPEDWINDOW | WS_VISIBLE | ES_MULTILINE | ES_AUTOVSCROLL,
                                200, 200, 480, 240, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    ShowWindow(edit, SW_SHOWNORMAL);
    UpdateWindow(edit);
    SetWindowPos(edit, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE);
    SetForegroundWindow(edit);
    BringWindowToTop(edit);
    SetFocus(edit);
    pump(250);

    RECT rect {};
    GetWindowRect(edit, &rect);
    int cx = (rect.left + rect.right) / 2;
    int cy = (rect.top + rect.bottom) / 2;
    SetCursorPos(cx, cy);
    pc_remote::mouse_button(0, true);
    pc_remote::mouse_button(0, false);
    pump(150);

    const char* text = "AudioShareKeyOK";
    if (!pc_remote::type_text(text)) {
        std::printf("FAIL type_text SendInput\n");
        return 1;
    }
    pump(500);

    wchar_t buffer[128] = {};
    GetWindowTextW(edit, buffer, 128);
    char narrow[128] = {};
    WideCharToMultiByte(CP_UTF8, 0, buffer, -1, narrow, sizeof(narrow), nullptr, nullptr);
    SetWindowPos(edit, HWND_NOTOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE);
    DestroyWindow(edit);

    bool ok = std::string(narrow).find(text) != std::string::npos;
    std::printf("%s typed='%s'\n", ok ? "PASS" : "FAIL", narrow);
    return ok ? 0 : 1;
}
