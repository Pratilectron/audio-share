#include "pc_remote.hpp"
#include <cstdio>
#include <string>
#include <vector>

int main()
{
    std::string error;
    std::vector<pc_remote::dir_entry> entries;
    if (!pc_remote::list_dir("", entries, error)) {
        std::printf("FAIL list home: %s\n", error.c_str());
        return 1;
    }

    const std::string relative = "audio-share-remote-test.txt";
    const std::string body = "step3-file-roundtrip";
    if (!pc_remote::write_file(relative, reinterpret_cast<const uint8_t*>(body.data()), body.size(), error)) {
        std::printf("FAIL write: %s\n", error.c_str());
        return 1;
    }
    std::vector<uint8_t> bytes;
    if (!pc_remote::read_file(relative, bytes, error)) {
        std::printf("FAIL read: %s\n", error.c_str());
        return 1;
    }
    std::string got(bytes.begin(), bytes.end());
    if (got != body) {
        std::printf("FAIL mismatch got='%s'\n", got.c_str());
        return 1;
    }

    std::string outside_error;
    std::vector<uint8_t> outside;
    if (pc_remote::read_file("..\\..\\Windows\\System32\\drivers\\etc\\hosts", outside, outside_error)) {
        std::printf("FAIL escaped profile\n");
        return 1;
    }

    std::printf("PASS files listed=%zu bytes=%zu blocked_escape=1\n", entries.size(), bytes.size());
    return 0;
}
