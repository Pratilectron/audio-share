/*
   Copyright 2022-2024 mkckr0 <https://github.com/mkckr0>

   Licensed under the Apache License, Version 2.0 (the "License");
   you may not use this file except in compliance with the License.
   You may obtain a copy of the License at

       http://www.apache.org/licenses/LICENSE-2.0
*/

#include "remote_control.hpp"
#include "pc_remote.hpp"

#include <fstream>
#include <mutex>
#include <utility>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shellapi.h>
#endif

namespace remote_control {

namespace {

std::mutex g_shortcuts_mutex;

#ifdef _WIN32

std::wstring exe_dir()
{
    wchar_t path[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    return std::wstring(path).substr(0, std::wstring(path).find_last_of(L"\\/") + 1);
}

std::wstring shortcuts_file()
{
    return exe_dir() + L"shortcuts.txt";
}

std::wstring widen(const std::string& text)
{
    if (text.empty()) {
        return {};
    }
    int size = MultiByteToWideChar(CP_UTF8, 0, text.data(), (int)text.size(), nullptr, 0);
    std::wstring wide(size, 0);
    MultiByteToWideChar(CP_UTF8, 0, text.data(), (int)text.size(), wide.data(), size);
    return wide;
}

std::string narrow(const std::wstring& text)
{
    if (text.empty()) {
        return {};
    }
    int size = WideCharToMultiByte(CP_UTF8, 0, text.data(), (int)text.size(), nullptr, 0, nullptr, nullptr);
    std::string out(size, 0);
    WideCharToMultiByte(CP_UTF8, 0, text.data(), (int)text.size(), out.data(), size, nullptr, nullptr);
    return out;
}

void tap_key(WORD key)
{
    keybd_event(key, 0, 0, 0);
    keybd_event(key, 0, KEYEVENTF_KEYUP, 0);
}

void open_target(const wchar_t* target)
{
    ShellExecuteW(nullptr, L"open", target, nullptr, nullptr, SW_SHOWNORMAL);
}

std::vector<std::pair<std::string, std::wstring>> read_shortcuts()
{
    std::vector<std::pair<std::string, std::wstring>> rows;
    std::lock_guard<std::mutex> lock(g_shortcuts_mutex);
    std::ifstream file(shortcuts_file());
    std::string line;
    while (std::getline(file, line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        auto bar = line.find('|');
        if (bar == std::string::npos || bar == 0 || bar + 1 >= line.size()) {
            continue;
        }
        rows.emplace_back(line.substr(0, bar), widen(line.substr(bar + 1)));
    }
    return rows;
}

#endif

} // namespace

bool run(uint32_t action, const std::string& payload)
{
#ifdef _WIN32
    switch (action) {
    case volume_up: tap_key(VK_VOLUME_UP); return true;
    case volume_down: tap_key(VK_VOLUME_DOWN); return true;
    case mute_toggle: tap_key(VK_VOLUME_MUTE); return true;
    case media_play_pause: tap_key(VK_MEDIA_PLAY_PAUSE); return true;
    case media_next: tap_key(VK_MEDIA_NEXT_TRACK); return true;
    case media_prev: tap_key(VK_MEDIA_PREV_TRACK); return true;
    case open_settings: open_target(L"ms-settings:"); return true;
    case open_sound: open_target(L"ms-settings:sound"); return true;
    case open_display: open_target(L"ms-settings:display"); return true;
    case open_wifi: open_target(L"ms-settings:network-wifi"); return true;
    case open_bluetooth: open_target(L"ms-settings:bluetooth"); return true;
    case open_night_light: open_target(L"ms-settings:nightlight"); return true;
    case open_browser: {
        std::wstring url = payload.empty() ? L"https://" : widen(payload);
        if (url.find(L"://") == std::wstring::npos) {
            url = L"https://" + url;
        }
        open_target(url.c_str());
        return true;
    }
    case open_explorer: open_target(L"explorer.exe"); return true;
    case open_task_manager: open_target(L"taskmgr.exe"); return true;
    case open_notepad: open_target(L"notepad.exe"); return true;
    case open_calculator: open_target(L"calc.exe"); return true;
    case lock_pc: LockWorkStation(); return true;
    case launch_shortcut: {
        for (const auto& row : read_shortcuts()) {
            if (row.first == payload) {
                open_target(row.second.c_str());
                return true;
            }
        }
        return false;
    }
    default:
        return pc_remote::dispatch(action, payload);
    }
#else
    (void)action;
    (void)payload;
    return false;
#endif
}

std::vector<std::string> shortcut_labels()
{
#ifdef _WIN32
    std::vector<std::string> labels;
    for (const auto& row : read_shortcuts()) {
        labels.push_back(row.first);
    }
    return labels;
#else
    return {};
#endif
}

bool add_shortcut(const std::string& label, const std::string& path)
{
#ifdef _WIN32
    if (label.empty() || path.empty() || label.find('|') != std::string::npos || label.find('\n') != std::string::npos || path.find('\n') != std::string::npos) {
        return false;
    }
    std::lock_guard<std::mutex> lock(g_shortcuts_mutex);
    std::ofstream file(shortcuts_file(), std::ios::app);
    if (!file) {
        return false;
    }
    file << label << '|' << path << '\n';
    return true;
#else
    (void)label;
    (void)path;
    return false;
#endif
}

} // namespace remote_control
