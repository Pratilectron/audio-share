/*
   Copyright 2022-2024 mkckr0 <https://github.com/mkckr0>

   Licensed under the Apache License, Version 2.0 (the "License");
   you may not use this file except in compliance with the License.
   You may obtain a copy of the License at

       http://www.apache.org/licenses/LICENSE-2.0
*/

#ifndef REMOTE_CONTROL_HPP
#define REMOTE_CONTROL_HPP

#include <cstdint>
#include <string>
#include <vector>

// Actions the phone (or the local dashboard) can ask this PC to perform.
// The numbers are the on-wire ids. Keep them in lockstep with the phone app.
namespace remote_control {

enum action : uint32_t {
    volume_up = 1,
    volume_down = 2,
    mute_toggle = 3,
    media_play_pause = 4,
    media_next = 5,
    media_prev = 6,
    open_settings = 7,
    open_sound = 8,
    open_display = 9,
    open_wifi = 10,
    open_bluetooth = 11,
    open_browser = 12,
    open_explorer = 13,
    open_task_manager = 14,
    lock_pc = 15,
    launch_shortcut = 16,
    open_night_light = 17,
    open_notepad = 18,
    open_calculator = 19,
};

bool run(uint32_t action, const std::string& payload);
std::vector<std::string> shortcut_labels();
bool add_shortcut(const std::string& label, const std::string& path);

} // namespace remote_control

#endif
