/*
   Copyright 2022-2024 mkckr0 <https://github.com/mkckr0>

   Licensed under the Apache License, Version 2.0 (the "License");
   you may not use this file except in compliance with the License.
   You may obtain a copy of the License at

       http://www.apache.org/licenses/LICENSE-2.0
*/

#include "pc_remote.hpp"

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shlobj.h>
#include <tlhelp32.h>
#include <psapi.h>
#endif

bool capture_camera_jpeg(std::vector<uint8_t>& jpeg, std::string& error);

namespace pc_remote {

namespace {

#ifdef _WIN32

bool send_mouse(DWORD flags, LONG dx, LONG dy, DWORD data)
{
    INPUT input {};
    input.type = INPUT_MOUSE;
    input.mi.dx = dx;
    input.mi.dy = dy;
    input.mi.mouseData = data;
    input.mi.dwFlags = flags;
    return SendInput(1, &input, sizeof(INPUT)) == 1;
}

std::wstring utf8_to_wide(const std::string& text)
{
    if (text.empty()) {
        return {};
    }
    int size = MultiByteToWideChar(CP_UTF8, 0, text.data(), (int)text.size(), nullptr, 0);
    if (size <= 0) {
        return {};
    }
    std::wstring wide(size, 0);
    MultiByteToWideChar(CP_UTF8, 0, text.data(), (int)text.size(), wide.data(), size);
    return wide;
}

std::string wide_to_utf8(const std::wstring& text)
{
    if (text.empty()) {
        return {};
    }
    int size = WideCharToMultiByte(CP_UTF8, 0, text.data(), (int)text.size(), nullptr, 0, nullptr, nullptr);
    if (size <= 0) {
        return {};
    }
    std::string out(size, 0);
    WideCharToMultiByte(CP_UTF8, 0, text.data(), (int)text.size(), out.data(), size, nullptr, nullptr);
    return out;
}

std::filesystem::path profile_root()
{
    wchar_t buffer[MAX_PATH] = {};
    if (FAILED(SHGetFolderPathW(nullptr, CSIDL_PROFILE, nullptr, 0, buffer))) {
        return {};
    }
    std::error_code ec;
    return std::filesystem::weakly_canonical(buffer, ec);
}

bool inside_profile(const std::filesystem::path& path, const std::filesystem::path& root)
{
    auto root_text = root.wstring();
    auto path_text = path.wstring();
    if (path_text.size() < root_text.size()) {
        return false;
    }
    if (_wcsnicmp(path_text.c_str(), root_text.c_str(), root_text.size()) != 0) {
        return false;
    }
    return path_text.size() == root_text.size() || path_text[root_text.size()] == L'\\' || path_text[root_text.size()] == L'/';
}

std::filesystem::path resolve_inside(const std::string& relative, std::string& error)
{
    auto root = profile_root();
    if (root.empty()) {
        error = "no user profile";
        return {};
    }
    std::filesystem::path joined = root / utf8_to_wide(relative);
    std::error_code ec;
    auto full = std::filesystem::weakly_canonical(joined, ec);
    if (ec) {
        full = std::filesystem::absolute(joined, ec);
    }
    if (full.empty() || !inside_profile(full, root)) {
        error = "that folder is outside the user profile";
        return {};
    }
    return full;
}

bool protected_process(const std::wstring& name)
{
    std::wstring lower = name;
    for (auto& c : lower) {
        c = (wchar_t)towlower(c);
    }
    static const wchar_t* blocked[] = {
        L"system", L"registry", L"smss.exe", L"csrss.exe", L"wininit.exe",
        L"services.exe", L"lsass.exe", L"winlogon.exe", L"fontdrvhost.exe",
        L"audioshareserver.exe",
    };
    for (auto* item : blocked) {
        if (lower == item) {
            return true;
        }
    }
    return false;
}

#endif

int16_t read_i16(const std::string& payload, size_t offset)
{
    if (offset + 2 > payload.size()) {
        return 0;
    }
    uint16_t value = (uint8_t)payload[offset] | ((uint16_t)(uint8_t)payload[offset + 1] << 8);
    return (int16_t)value;
}

} // namespace

bool mouse_move(int dx, int dy)
{
#ifdef _WIN32
    return send_mouse(MOUSEEVENTF_MOVE, dx, dy, 0);
#else
    (void)dx;
    (void)dy;
    return false;
#endif
}

bool mouse_button(int button, bool down)
{
#ifdef _WIN32
    DWORD flag = 0;
    if (button == 1) {
        flag = down ? MOUSEEVENTF_RIGHTDOWN : MOUSEEVENTF_RIGHTUP;
    } else if (button == 2) {
        flag = down ? MOUSEEVENTF_MIDDLEDOWN : MOUSEEVENTF_MIDDLEUP;
    } else {
        flag = down ? MOUSEEVENTF_LEFTDOWN : MOUSEEVENTF_LEFTUP;
    }
    return send_mouse(flag, 0, 0, 0);
#else
    (void)button;
    (void)down;
    return false;
#endif
}

bool mouse_scroll(int notches)
{
#ifdef _WIN32
    return send_mouse(MOUSEEVENTF_WHEEL, 0, 0, (DWORD)(notches * WHEEL_DELTA));
#else
    (void)notches;
    return false;
#endif
}

bool type_text(const std::string& utf8)
{
#ifdef _WIN32
    auto wide = utf8_to_wide(utf8);
    if (wide.empty()) {
        return false;
    }
    std::vector<INPUT> inputs(wide.size() * 2);
    for (size_t i = 0; i < wide.size(); ++i) {
        inputs[i * 2].type = INPUT_KEYBOARD;
        inputs[i * 2].ki.wScan = wide[i];
        inputs[i * 2].ki.dwFlags = KEYEVENTF_UNICODE;
        inputs[i * 2 + 1].type = INPUT_KEYBOARD;
        inputs[i * 2 + 1].ki.wScan = wide[i];
        inputs[i * 2 + 1].ki.dwFlags = KEYEVENTF_UNICODE | KEYEVENTF_KEYUP;
    }
    return SendInput((UINT)inputs.size(), inputs.data(), sizeof(INPUT)) == inputs.size();
#else
    (void)utf8;
    return false;
#endif
}

bool key_vk(int virtual_key, bool down)
{
#ifdef _WIN32
    if (virtual_key <= 0 || virtual_key > 255) {
        return false;
    }
    INPUT input {};
    input.type = INPUT_KEYBOARD;
    input.ki.wVk = (WORD)virtual_key;
    input.ki.dwFlags = down ? 0 : KEYEVENTF_KEYUP;
    return SendInput(1, &input, sizeof(INPUT)) == 1;
#else
    (void)virtual_key;
    (void)down;
    return false;
#endif
}

std::string home_dir()
{
#ifdef _WIN32
    return wide_to_utf8(profile_root().wstring());
#else
    return {};
#endif
}

bool list_dir(const std::string& relative, std::vector<dir_entry>& entries, std::string& error)
{
#ifdef _WIN32
    auto full = resolve_inside(relative, error);
    if (full.empty()) {
        return false;
    }
    std::error_code ec;
    if (!std::filesystem::is_directory(full, ec)) {
        error = "not a folder";
        return false;
    }
    for (const auto& item : std::filesystem::directory_iterator(full, ec)) {
        if (entries.size() >= 200) {
            break;
        }
        dir_entry entry;
        entry.name = wide_to_utf8(item.path().filename().wstring());
        entry.directory = item.is_directory(ec);
        if (!entry.directory) {
            entry.bytes = item.file_size(ec);
        }
        entries.push_back(std::move(entry));
    }
    std::sort(entries.begin(), entries.end(), [](const dir_entry& a, const dir_entry& b) {
        if (a.directory != b.directory) {
            return a.directory;
        }
        return a.name < b.name;
    });
    return true;
#else
    (void)relative;
    (void)entries;
    error = "unsupported";
    return false;
#endif
}

bool read_file(const std::string& relative, std::vector<uint8_t>& bytes, std::string& error)
{
#ifdef _WIN32
    auto full = resolve_inside(relative, error);
    if (full.empty()) {
        return false;
    }
    std::error_code ec;
    if (!std::filesystem::is_regular_file(full, ec)) {
        error = "not a file";
        return false;
    }
    auto size = std::filesystem::file_size(full, ec);
    if (ec || size > 8 * 1024 * 1024) {
        error = "file is too large";
        return false;
    }
    std::ifstream file(full, std::ios::binary);
    if (!file) {
        error = "cannot read the file";
        return false;
    }
    bytes.resize((size_t)size);
    file.read(reinterpret_cast<char*>(bytes.data()), (std::streamsize)bytes.size());
    return file.good() || file.eof();
#else
    (void)relative;
    (void)bytes;
    error = "unsupported";
    return false;
#endif
}

bool write_file(const std::string& relative, const uint8_t* data, size_t size, std::string& error)
{
#ifdef _WIN32
    if (size > 8 * 1024 * 1024) {
        error = "file is too large";
        return false;
    }
    auto full = resolve_inside(relative, error);
    if (full.empty()) {
        return false;
    }
    std::error_code ec;
    std::filesystem::create_directories(full.parent_path(), ec);
    std::ofstream file(full, std::ios::binary | std::ios::trunc);
    if (!file) {
        error = "cannot write the file";
        return false;
    }
    file.write(reinterpret_cast<const char*>(data), (std::streamsize)size);
    return file.good();
#else
    (void)relative;
    (void)data;
    (void)size;
    error = "unsupported";
    return false;
#endif
}

machine_stats sample_stats()
{
    machine_stats stats;
#ifdef _WIN32
    MEMORYSTATUSEX memory {};
    memory.dwLength = sizeof(memory);
    if (GlobalMemoryStatusEx(&memory)) {
        stats.memory_total_mb = (uint32_t)(memory.ullTotalPhys / (1024 * 1024));
        stats.memory_used_mb = (uint32_t)((memory.ullTotalPhys - memory.ullAvailPhys) / (1024 * 1024));
    }
    static FILETIME prev_idle {}, prev_kernel {}, prev_user {};
    FILETIME idle {}, kernel {}, user {};
    if (GetSystemTimes(&idle, &kernel, &user) && prev_kernel.dwLowDateTime != 0) {
        auto ticks = [](const FILETIME& a, const FILETIME& b) {
            ULARGE_INTEGER ua { a.dwLowDateTime, a.dwHighDateTime };
            ULARGE_INTEGER ub { b.dwLowDateTime, b.dwHighDateTime };
            return ua.QuadPart - ub.QuadPart;
        };
        auto idle_d = ticks(idle, prev_idle);
        auto total = ticks(kernel, prev_kernel) + ticks(user, prev_user);
        if (total > 0) {
            stats.cpu_percent = (uint32_t)((total - idle_d) * 100 / total);
        }
    }
    prev_idle = idle;
    prev_kernel = kernel;
    prev_user = user;

    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot != INVALID_HANDLE_VALUE) {
        PROCESSENTRY32W entry {};
        entry.dwSize = sizeof(entry);
        if (Process32FirstW(snapshot, &entry)) {
            do {
                process_row row;
                row.pid = entry.th32ProcessID;
                row.name = wide_to_utf8(entry.szExeFile);
                HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, entry.th32ProcessID);
                if (process) {
                    PROCESS_MEMORY_COUNTERS counters {};
                    if (GetProcessMemoryInfo(process, &counters, sizeof(counters))) {
                        row.memory_mb = (uint32_t)(counters.WorkingSetSize / (1024 * 1024));
                    }
                    CloseHandle(process);
                }
                stats.processes.push_back(std::move(row));
            } while (Process32NextW(snapshot, &entry));
        }
        CloseHandle(snapshot);
    }
    std::sort(stats.processes.begin(), stats.processes.end(), [](const process_row& a, const process_row& b) {
        return a.memory_mb > b.memory_mb;
    });
    if (stats.processes.size() > 40) {
        stats.processes.resize(40);
    }
#endif
    return stats;
}

bool end_process(uint32_t pid, std::string& error)
{
#ifdef _WIN32
    if (pid <= 4 || pid == GetCurrentProcessId()) {
        error = "that process is protected";
        return false;
    }
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | PROCESS_TERMINATE, FALSE, pid);
    if (!process) {
        error = "cannot open that process";
        return false;
    }
    wchar_t name[MAX_PATH] = {};
    DWORD size = MAX_PATH;
    QueryFullProcessImageNameW(process, 0, name, &size);
    std::wstring file = std::filesystem::path(name).filename().wstring();
    if (protected_process(file)) {
        CloseHandle(process);
        error = "that process is protected";
        return false;
    }
    BOOL ok = TerminateProcess(process, 1);
    CloseHandle(process);
    if (!ok) {
        error = "could not close it";
        return false;
    }
    return true;
#else
    (void)pid;
    error = "unsupported";
    return false;
#endif
}

bool capture_jpeg(std::vector<uint8_t>& jpeg, std::string& error)
{
    return capture_camera_jpeg(jpeg, error);
}

bool dispatch(uint32_t action, const std::string& payload)
{
    switch (action) {
    case act_mouse_move:
        return mouse_move(read_i16(payload, 0), read_i16(payload, 2));
    case act_mouse_button:
        if (payload.size() < 2) {
            return false;
        }
        return mouse_button((uint8_t)payload[0], payload[1] != 0);
    case act_mouse_scroll:
        return mouse_scroll(read_i16(payload, 0));
    case act_key_text:
        return type_text(payload);
    case act_key_vk:
        if (payload.size() < 3) {
            return false;
        }
        return key_vk((uint8_t)payload[0] | ((uint8_t)payload[1] << 8), payload[2] != 0);
    default:
        return false;
    }
}

} // namespace pc_remote
