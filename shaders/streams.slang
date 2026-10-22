// DrawWalls' streamlines (WL_STREAM). From each stream cell's centre, a line traced along the air's velocity, an
// eighth of it a step for up to 1024 steps, adding white at alpha 64 wherever it is each step; then the stream's
// icon over the cell at alpha 128. In still air, the icon and then a white dot. TPT draws these cell by cell among
// the walls, so each pixel's share is a fragment keyed by the cell (cell * 64, then + 1) that the resolve puts in
// that order among the walls' patterns and blobs (resolve_common.slang).
#include "render_common.slang"

void streamAdd(int x, int y, uint key, uint amount)
{
    if (amount != 0 && inVideo(x, y))
        pushFrag(uint(y) * pc.W + uint(x), key, OP_WALL_ADD, amount);
}

// BlendText({cx * CELL, cy * CELL - 2}, 0xE00D, white at alpha 128): BlendChar's rows start 2 px higher still
void streamIcon(int cx, int cy, uint key)
{
    uint glyph = renderTables[RT_GLYPH_STREAM];
    int w = glyphWidth(glyph);
    int x0 = cx * CELL, y0 = cy * CELL - 4;
    for (int j = 0; j < FONT_H; j++)
        for (int i = 0; i < w; i++)
        {
            int a = glyphPixel(glyph, i, j) * 128 / 3;
            if (a != 0 && inVideo(x0 + i, y0 + j))
                pushFrag(uint(y0 + j) * pc.W + uint(x0 + i), key, OP_WALL_BLEND, packRGBA(255, 255, 255, a));
        }
}

[shader("compute")]
[numthreads(64, 1, 1)]
void main(uint3 tid: SV_DispatchThreadID)
{
    uint k = tid.x;
    if (k >= pc.CW * pc.CH)
        return;
    int cx = int(k % pc.CW), cy = int(k / pc.CW);
    if (bmapCell(cx, cy) != WL_STREAM)
        return;
    uint key = k * 64;
    float xf = float(cx * CELL) + float(CELL) * 0.5;
    float yf = float(cy * CELL) + float(CELL) * 0.5;
    int oldX = int(xf + 0.5), oldY = int(yf + 0.5);
    float xVel = airVX(k) * 0.125, yVel = airVY(k) * 0.125;
    if (xVel == 0.0 && yVel == 0.0)
    {
        streamIcon(cx, cy, key);
        streamAdd(oldX, oldY, key + 1, 255);
        return;
    }
    bool changed = false;
    int runX = oldX, runY = oldY; // the pixel being added to, and how much so far
    uint run = 0;
    for (int t = 0; t < 1024; t++)
    {
        int newX = int(xf + 0.5), newY = int(yf + 0.5);
        if (newX != oldX || newY != oldY)
        {
            changed = true;
            oldX = newX;
            oldY = newY;
        }
        if (changed && (newX < 0 || newX >= int(pc.W) || newY < 0 || newY >= int(pc.H)))
            break;
        if (newX != runX || newY != runY)
        {
            streamAdd(runX, runY, key, run);
            runX = newX;
            runY = newY;
            run = 0;
        }
        run += 64;
        if (changed)
        {
            int wallX = newX / CELL, wallY = newY / CELL;
            uint wc = uint(wallY) * pc.CW + uint(wallX);
            xVel = airVX(wc) * 0.125;
            yVel = airVY(wc) * 0.125;
            // (TPT's check: only a stream cell diagonally away ends the line)
            if (wallX != cx && wallY != cy && bmapCell(wallX, wallY) == WL_STREAM)
                break;
        }
        xf += xVel;
        yf += yVel;
    }
    streamAdd(runX, runY, key, run);
    streamIcon(cx, cy, key + 1);
}
