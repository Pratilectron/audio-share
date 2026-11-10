#include "pc_remote.hpp"
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <objbase.h>
#include <cstdio>
#include <vector>
#include <string>

int main()
{
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    std::vector<uint8_t> jpeg;
    std::string error;
    bool ok = pc_remote::capture_jpeg(jpeg, error);
    if (ok) {
        bool jpeg_header = jpeg.size() > 3 && jpeg[0] == 0xff && jpeg[1] == 0xd8;
        std::printf("%s camera jpeg=%zu\n", jpeg_header ? "PASS" : "FAIL", jpeg.size());
        CoUninitialize();
        return jpeg_header ? 0 : 1;
    }
    // Accept only explicit "no camera" / MF-unavailable outcomes as the no-device path.
    if (error == "no camera" || error == "Media Foundation is unavailable" || error == "unsupported") {
        std::printf("PASS no-camera path: %s\n", error.c_str());
        CoUninitialize();
        return 0;
    }
    std::printf("FAIL unexpected: %s\n", error.c_str());
    CoUninitialize();
    return 1;
}
