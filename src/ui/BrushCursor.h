// TPT's brush cursor (GameView::OnDraw): the world pixels TPT inverts to show the brush, the line or box being drawn
// and the flood fill's cross, in its order, as x, y pairs. A pixel can be listed more than once, and inverting it
// twice isn't the same as once (RasterDrawMethods' XorPixel).
#pragma once
#include "classic/TptTools.h"

#include <vector>

namespace bp {

struct CursorState {
    enum class Mode { Points, Line, Rect, Fill }; // GameView's drawMode
    Mode mode = Mode::Points;
    bool mouseDown = false;
    bool snap = false;      // drawSnap (alt): lines and boxes snap to 45 degrees
    bool wallBrush = false; // the last tool is a wall: the brush shows as whole cells
    bool decoBrush = false; // (decoration tools show no fill cross)
    int x1 = 0, y1 = 0;     // drawPoint1: where a line or box started
    int x = 0, y = 0;       // the mouse, in world pixels
};

void brushCursor(const CursorState &s, const TptBrush &brush, std::vector<int> &out);

// GameView::lineSnapCoords and rectSnapCoords
void lineSnap(int x1, int y1, int &x2, int &y2);
void rectSnap(int x1, int y1, int &x2, int &y2);

// TPT's XorPixel on a 0xRRGGBB picture, for each listed pixel inside it (Classic mode)
void xorCursor(std::vector<uint32_t> &pixels, int w, int h, const std::vector<int> &cursor);

} // namespace bp
