/*
   Copyright 2022-2024 mkckr0 <https://github.com/mkckr0>

   Licensed under the Apache License, Version 2.0 (the "License");
   you may not use this file except in compliance with the License.
   You may obtain a copy of the License at

       http://www.apache.org/licenses/LICENSE-2.0
*/

#ifndef PC_REMOTE_HPP
#define PC_REMOTE_HPP

#include <cstdint>
#include <string>
#include <vector>

// Desktop control beyond the original audio actions.
// Wire ids stay stable. Paths for files stay inside the user profile.
namespace pc_remote {

enum action : uint32_t {
    act_mouse_move = 20,
    act_mouse_button = 21,
    act_mouse_scroll = 22,
    act_key_text = 23,
    act_key_vk = 24,
};

struct dir_entry {
    std::string name;
    bool directory = false;
    uint64_t bytes = 0;
};

struct process_row {
    uint32_t pid = 0;
    uint32_t memory_mb = 0;
    std::string name;
};

struct machine_stats {
    uint32_t cpu_percent = 0;
    uint32_t memory_used_mb = 0;
    uint32_t memory_total_mb = 0;
    std::vector<process_row> processes;
};

bool mouse_move(int dx, int dy);
bool mouse_button(int button, bool down);
bool mouse_scroll(int notches);
bool type_text(const std::string& utf8);
bool key_vk(int virtual_key, bool down);

std::string home_dir();
bool list_dir(const std::string& relative, std::vector<dir_entry>& entries, std::string& error);
bool read_file(const std::string& relative, std::vector<uint8_t>& bytes, std::string& error);
bool write_file(const std::string& relative, const uint8_t* data, size_t size, std::string& error);

machine_stats sample_stats();
bool end_process(uint32_t pid, std::string& error);

// One JPEG frame from the first camera. False when the PC has no camera.
bool capture_jpeg(std::vector<uint8_t>& jpeg, std::string& error);

bool dispatch(uint32_t action, const std::string& payload);

} // namespace pc_remote

#endif
