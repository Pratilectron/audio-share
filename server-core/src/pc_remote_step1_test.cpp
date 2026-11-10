#include "pc_remote.hpp"
#include <cstdio>
#include <windows.h>

int main()
{
    POINT before {};
    GetCursorPos(&before);
    int dx = before.x > 200 ? -70 : 70;
    int dy = before.y > 200 ? -40 : 40;
    if (!pc_remote::mouse_move(dx, dy)) {
        std::printf("FAIL mouse_move send\n");
        return 1;
    }
    Sleep(50);
    POINT after {};
    GetCursorPos(&after);
    pc_remote::mouse_move(-dx, -dy);
    bool moved = (after.x - before.x) * dx > 0 && (after.y - before.y) * dy > 0;
    std::printf("%s cursor %ld,%ld -> %ld,%ld\n", moved ? "PASS" : "FAIL", before.x, before.y, after.x, after.y);
    return moved ? 0 : 1;
}
