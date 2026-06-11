// Standalone smoke test for X11Cursor (VIV-66).  Needs an X session; run on
// the box with DISPLAY set.  Not wired into ctest.
#include "host/capture/x11_cursor.h"
#include <cstdio>
#include <unistd.h>
using namespace vivora::host;
int main() {
    X11Cursor c;
    if (!c.init()) { std::printf("init FAILED (no X / libs)\n"); return 1; }
    for (int i = 0; i < 5; ++i) {
        float x = 0, y = 0; bool vis = false, ch = false; X11Cursor::Shape sh;
        if (c.poll(x, y, vis, ch, sh)) {
            std::printf("poll %d: x_norm=%.3f y_norm=%.3f vis=%d shape_changed=%d "
                        "(%ux%u hot=%u,%u bgra=%zuB)\n",
                        i, x, y, vis, ch, sh.width, sh.height,
                        sh.hotspot_x, sh.hotspot_y, sh.bgra.size());
        } else {
            std::printf("poll %d: unavailable\n", i);
        }
        usleep(250000);
    }
    return 0;
}
