// TPT's rasterizers (common/RasterGeometry.h), for the interface's drawing and the brush cursor.
#pragma once
#include <cstdlib>

namespace bp {

// RasterizeLine<false>: every pixel from (x0, y0) to (x1, y1), along the longer axis
template <class F> void rasterizeLine(int x0, int y0, int x1, int y1, F f) {
    auto zw = [&](int sx, int sy, int dz, int dw, bool xMajor) {
        const int incW = dw >= 0 ? 1 : -1;
        int w = 0, err = 0;
        for (int z = 0; z <= dz; z++) {
            if (xMajor)
                f(sx + z, sy + w);
            else
                f(sx + w, sy + z);
            err += 2 * dw * incW;
            if (err >= dz) {
                w += incW;
                err -= 2 * dz;
            }
        }
    };
    if (std::abs(x0 - x1) >= std::abs(y0 - y1)) {
        bool first = x0 < x1;
        int sx = first ? x0 : x1, sy = first ? y0 : y1;
        zw(sx, sy, first ? x1 - x0 : x0 - x1, first ? y1 - y0 : y0 - y1, true);
    } else {
        bool first = y0 < y1;
        int sx = first ? x0 : x1, sy = first ? y0 : y1;
        zw(sx, sy, first ? y1 - y0 : y0 - y1, first ? x1 - x0 : x0 - x1, false);
    }
}

// RasterizeRect: the outline, each pixel once
template <class F> void rasterizeRect(int x, int y, int w, int h, F f) {
    for (int i = x; i < x + w; i++)
        f(i, y);
    if (h > 1)
        for (int i = x; i < x + w; i++)
            f(i, y + h - 1);
    for (int j = y + 1; j < y + h - 1; j++)
        f(x, j);
    if (w > 1)
        for (int j = y + 1; j < y + h - 1; j++)
            f(x + w - 1, j);
}

} // namespace bp
