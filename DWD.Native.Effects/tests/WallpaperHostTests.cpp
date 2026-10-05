#include "WallpaperHost.h"
#include <cstdio>
int main() {
    const RECT dual{0,0,4480,1440},negative{-1920,-240,2560,1440};
    const bool success=WallpaperHost::Covers(dual,dual) && WallpaperHost::Covers(negative,negative) &&
        !WallpaperHost::Covers(RECT{0,0,2560,1440},dual) &&
        !WallpaperHost::Covers(RECT{0,0,4480,1440},negative) &&
        !WallpaperHost::Covers(RECT{0,0,152,0},dual) &&
        !WallpaperHost::Covers(RECT{100,100,4580,1540},dual);
    puts(success?"Host coverage: dual / negative coordinates / hidden-tool geometry OK":"FAIL");
    return success?0:1;
}
