#include "ui/BrushCursor.h"
#include "sim/GpuTypes.h"
#include "ui/Raster.h"

#include <cstdlib>

namespace bp {

void lineSnap(int x1, int y1, int &x2, int &y2) {
    int dx = x2 - x1, dy = y2 - y1;
    if (std::abs(dx / 2) > std::abs(dy)) { // vertical
        x2 = x1 + dx, y2 = y1;
    } else if (std::abs(dx) < std::abs(dy / 2)) { // horizontal
        x2 = x1, y2 = y1 + dy;
    } else if (dx * dy > 0) { // NW-SE
        x2 = x1 + (dx + dy) / 2, y2 = y1 + (dx + dy) / 2;
    } else { // SW-NE
        x2 = x1 + (dx - dy) / 2, y2 = y1 + (dy - dx) / 2;
    }
}

void rectSnap(int x1, int y1, int &x2, int &y2) {
    int dx = x2 - x1, dy = y2 - y1;
    if (dx * dy > 0) { // NW-SE
        x2 = x1 + (dx + dy) / 2, y2 = y1 + (dx + dy) / 2;
    } else { // SW-NE
        x2 = x1 + (dx - dy) / 2, y2 = y1 + (dy - dx) / 2;
    }
}

void brushCursor(const CursorState &s, const TptBrush &brush, std::vector<int> &out) {
    out.clear();
    auto xorLine = [&](int ax, int ay, int bx, int by) {
        rasterizeLine(ax, ay, bx, by, [&](int x, int y) {
            out.push_back(x);
            out.push_back(y);
        });
    };
    int cx = s.x, cy = s.y, ix = s.x1, iy = s.y1;
    if (s.wallBrush) { // NormaliseBlockCoord
        cx = cx / CELL * CELL, cy = cy / CELL * CELL;
        ix = ix / CELL * CELL, iy = iy / CELL * CELL;
    }
    using M = CursorState::Mode;
    if (s.mode == M::Rect && s.mouseDown) {
        if (s.snap)
            rectSnap(ix, iy, cx, cy);
        if (s.wallBrush) {
            if (cx > ix)
                cx += CELL - 1;
            else
                ix += CELL - 1;
            if (cy > iy)
                cy += CELL - 1;
            else
                iy += CELL - 1;
        }
        // Brush::RenderRect
        int width = cx - ix, height = cy - iy;
        if (height < 0) {
            iy += height;
            height = -height;
        }
        if (width < 0) {
            ix += width;
            width = -width;
        }
        xorLine(ix, iy, ix + width, iy);
        if (height > 0) {
            xorLine(ix, iy + height, ix + width, iy + height);
            if (height > 1) {
                xorLine(ix + width, iy + 1, ix + width, iy + height - 1);
                if (width > 0)
                    xorLine(ix, iy + 1, ix, iy + height - 1);
            }
        }
    } else if (s.mode == M::Line && s.mouseDown) {
        if (s.snap)
            lineSnap(ix, iy, cx, cy);
        xorLine(ix, iy, cx, cy); // Brush::RenderLine
    } else if (s.mode == M::Fill) {
        if (!s.decoBrush) { // Brush::RenderFill
            xorLine(cx - 5, cy, cx - 1, cy);
            xorLine(cx + 5, cy, cx + 1, cy);
            xorLine(cx, cy - 5, cx, cy - 1);
            xorLine(cx, cy + 5, cx, cy + 1);
        }
    }
    if (s.mode == M::Points || s.mode == M::Line || (s.mode == M::Rect && !s.mouseDown)) {
        if (s.wallBrush) {
            int rx = brush.rx() / CELL * CELL, ry = brush.ry() / CELL * CELL;
            int lx = cx - rx, ty = cy - ry, rxe = cx + rx + CELL - 1, by = cy + ry + CELL - 1;
            xorLine(lx, ty, rxe, ty);
            xorLine(lx, by, rxe, by);
            xorLine(lx, ty + 1, lx, by - 1); // (offset by one so the corners aren't inverted twice)
            xorLine(rxe, ty + 1, rxe, by - 1);
        } else {
            // Brush::RenderPoint: the outline
            const int rx = brush.rx(), ry = brush.ry(), w = 2 * rx + 1, h = 2 * ry + 1;
            const std::vector<uint8_t> &o = brush.outline();
            for (int j = 0; j < h; j++)
                for (int i = 0; i < w; i++)
                    if (o[size_t(j) * w + size_t(i)]) {
                        out.push_back(cx - rx + i);
                        out.push_back(cy - ry + j);
                    }
        }
    }
}

void xorCursor(std::vector<uint32_t> &pixels, int w, int h, const std::vector<int> &cursor) {
    for (size_t k = 0; k + 1 < cursor.size(); k += 2) {
        int x = cursor[k], y = cursor[k + 1];
        if (x < 0 || y < 0 || x >= w || y >= h)
            continue;
        uint32_t &p = pixels[size_t(y) * w + size_t(x)];
        int r = (p >> 16) & 0xFF, g = (p >> 8) & 0xFF, b = p & 0xFF;
        p = 2 * r + 3 * g + b < 512 ? 0xC0C0C0 : 0x404040;
    }
}

} // namespace bp
