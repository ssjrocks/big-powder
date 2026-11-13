// Shared by resolve.slang and resolve_heavy.slang: TPT's frame at one world pixel. In order: the air display
// (Renderer::draw_air), the walls (DrawWalls), then every particle's drawing there sorted into TPT's order (the top
// particle's and the top energy particle's own pixel from gfx, the rest from the pixel's fragment list and, for
// glow and blur, the particles around), then the fire glow (render_fire, from the fire buffer fire.slang filled
// this frame) and the EMP flash.
#include "render_common.slang"

uint clampFlt(float f, float lo, float hi) // Misc.h's clamp_flt: f mapped onto 0..255
{
    if (f < lo)
        return 0;
    if (f > hi)
        return 255;
    return uint(int(255.0 * (f - lo) / (hi - lo)));
}

int3 airColour(int cx, int cy)
{
    uint ci = uint(cy) * pc.CW + uint(cx);
    float pv = airPV(ci), vx = airVX(ci), vy = airVY(ci);
    uint dm = pc.displayMode;
    if ((dm & DISPLAY_AIRP) != 0)
        return pv > 0.0 ? int3(int(clampFlt(pv, 0.0, 8.0)), 0, 0) : int3(0, 0, int(clampFlt(-pv, 0.0, 8.0)));
    if ((dm & DISPLAY_AIRV) != 0)
        return int3(int(clampFlt(abs(vx), 0.0, 8.0)), int(clampFlt(pv, 0.0, 8.0)), int(clampFlt(abs(vy), 0.0, 8.0)));
    if ((dm & DISPLAY_AIRH) != 0)
    {
        float2 hr = hdispRange();
        int3 h = unpackRGB(tableAt(RT_HEAT, 1024, int((airGet(AIR_HV, ci) - hr.x) / (hr.y - hr.x) * 1024.0)));
        return int3(float3(h) * 0.7); // HeatToColour
    }
    if ((dm & DISPLAY_AIRC) != 0)
    {
        int r = int(clampFlt(abs(vx), 0.0, 24.0) + clampFlt(abs(vy), 0.0, 20.0));
        int g = int(clampFlt(abs(vx), 0.0, 20.0) + clampFlt(abs(vy), 0.0, 24.0));
        int b = int(clampFlt(abs(vx), 0.0, 24.0) + clampFlt(abs(vy), 0.0, 20.0));
        if (pv > 0.0)
            r += int(clampFlt(pv, 0.0, 16.0));
        else
            b += int(clampFlt(-pv, 0.0, 16.0));
        return min(int3(r, g, b), int3(255, 255, 255));
    }
    if ((dm & DISPLAY_AIRW) != 0)
    {
        float w = 0.0;
        if (cx > 1 && cx < int(pc.CW) - 2 && cy > 1 && cy < int(pc.CH) - 2)
        {
            float dvydx = airVY(ci + 1) - airVY(ci - 1);
            float dvxdy = airVX(ci + pc.CW) - airVX(ci - pc.CW);
            w = 4.0 * (dvydx - dvxdy) * 0.5;
        }
        return w > 0.0 ? int3(int(clampFlt(w, 0.0, 8.0)), 0, 0) : int3(0, 0, int(clampFlt(-w, 0.0, 8.0)));
    }
    return int3(0, 0, 0);
}

// A wall's own colour or glow colour (dimmed while an element is being found)
int3 wallColour(uint at)
{
    int3 c = unpackRGB(renderTables[at]);
    return findActive() ? c / 10 : c;
}

// DrawWalls' pattern at pixel (i, j) of a wall cell: true and the colour if the pattern sets it
bool wallPattern(uint wt, uint powered, int x, int y, int i, int j, out int3 c)
{
    c = wallColour(RT_WALL_COLOUR + wt);
    switch (renderTables[RT_WALL_STYLE + wt])
    {
    case 0:
        if (wt == WL_EWALL || wt == WL_STASIS)
        {
            bool on = (powered > 0) != (wt == WL_STASIS);
            return on ? (i & j & 1) != 0 : (i & j & 1) == 0;
        }
        if (wt == WL_WALLELEC)
        {
            if (!((y % 2) == 0 && (x % 2) == 0))
                c = int3(0x80, 0x80, 0x80);
            return true;
        }
        if (wt == WL_EHOLE)
        {
            if (powered != 0)
                c = ((i | j) & 1) == 0 ? int3(0, 0, 0) : int3(0x24, 0x24, 0x24);
            else
                c = int3(0x24, 0x24, 0x24);
            return powered != 0 || ((i | j) & 1) == 0;
        }
        return false; // (streams draw streamlines)
    case 1:
        return (j & 1) == 0 && i >= ((j >> 1) & 1) && ((i - ((j >> 1) & 1)) & 1) == 0;
    case 2:
        return ((i | j) & 1) == 0;
    case 3:
        return true;
    case 4:
        if (i == j)
            return true;
        if (i == j + 1 || (i == 0 && j == CELL - 1))
            c = wallColour(RT_WALL_EGLOW + wt);
        else
            c = int3(0x20, 0x20, 0x20);
        return true;
    }
    return false;
}

// In blob display, DrawWalls' second loop: whether pixel (i, j) of a wall cell draws a blob (and its colour),
// and whether it then sets the pixel itself (style 4's other pixels; the dots of a powered E-hole, after the
// cell's blobs)
void wallBlob(uint wt, uint powered, int x, int y, int i, int j, out bool blob, out int3 bc, out bool set, out bool late,
              out int3 sc)
{
    blob = false;
    set = false;
    late = false;
    bc = wallColour(RT_WALL_COLOUR + wt);
    sc = int3(0, 0, 0);
    switch (renderTables[RT_WALL_STYLE + wt])
    {
    case 0:
        if (wt == WL_EWALL || wt == WL_STASIS)
        {
            bool on = (powered > 0) != (wt == WL_STASIS);
            blob = on ? (i & j & 1) != 0 : (i & j & 1) == 0;
        }
        else if (wt == WL_WALLELEC)
        {
            blob = true;
            if (!((y % 2) == 0 && (x % 2) == 0))
                bc = int3(0x80, 0x80, 0x80);
        }
        else if (wt == WL_EHOLE)
        {
            bc = int3(0x24, 0x24, 0x24);
            if (powered != 0)
            {
                blob = true;
                if (((i | j) & 1) == 0)
                {
                    set = true;
                    late = true;
                }
            }
            else
                blob = ((i | j) & 1) == 0;
        }
        break;
    case 1:
        blob = (j & 1) == 0 && i >= ((j >> 1) & 1) && ((i - ((j >> 1) & 1)) & 1) == 0;
        break;
    case 2:
        blob = ((i | j) & 1) == 0;
        break;
    case 3:
        blob = true;
        break;
    case 4:
        if (i == j)
            blob = true;
        else
        {
            set = true;
            sc = (i == j + 1 || (i == 0 && j == CELL - 1)) ? wallColour(RT_WALL_EGLOW + wt) : int3(0x20, 0x20, 0x20);
        }
        break;
    }
}

uint wallAt(int x, int y, out uint powered)
{
    powered = 0;
    if (!inVideo(x, y))
        return 0;
    uint wt = bmapAt(x, y);
    if (wt == 0 || wt >= 19)
        return 0;
    powered = emapAt(x, y);
    return wt;
}

static const uint MAX_A = 48;

void pushA(inout uint aKey[MAX_A], inout uint aOp[MAX_A], inout uint aCol[MAX_A], inout uint na, uint key, uint op,
           uint col)
{
    if (na < MAX_A)
    {
        aKey[na] = key;
        aOp[na] = op;
        aCol[na] = col;
        na++;
    }
}

// An AddPixel: its term (computed on its own, as each call rounds on its own) goes into the gap after the last
// overwrite or blend with a smaller key.
// (each gap's sum is kept packed, at most 255 a channel: anything more ends as 255 anyway)
void addOp(uint aKey[MAX_A], uint na, inout uint sums[MAX_A + 1], uint key, uint colour)
{
    uint b = 0;
    while (b < na && aKey[b] < key)
        b++;
    int4 c = unpackRGBA(colour);
    sums[b] = packRGB(min(unpackRGB(sums[b]) + (c.w * c.xyz) / 0xFF, int3(255, 255, 255)));
}

// The pixel before any particle: the persistent display's faded last frame (or black), the air display, the walls
static const uint WALL_OPS = 16; // walls' operations on one pixel (a pattern, 8 blobs, sets, streamlines)

// simpleWalls: (resolve.slang) the pixel has no fragments and blob display is off, so its own wall's pattern is
// all the walls draw there; the ordering below then isn't compiled into that kernel at all.
int3 pixelBase(int x, int y, uint c, bool simpleWalls = false)
{
    // Renderer::Clear: the persistent display starts from its faded last frame
    int3 px = (pc.displayMode & DISPLAY_PERS) != 0 ? unpackRGB(persistVideo[c]) : int3(0, 0, 0);
    if ((pc.displayMode & DISPLAY_AIR) != 0 && !(params[0].ambientHeat == 0 && (pc.displayMode & DISPLAY_AIRH) != 0))
    {
        px = airColour(x / CELL, y / CELL);
        if (findActive())
            px /= 10;
    }
    // Renderer::draw_grav: the gravity field display's points, white saturating adds (so their order doesn't matter)
    if ((pc.overlays & 1) != 0)
        px = min(px + int(min(gravDraw[c], 255u)), int3(0xFF, 0xFF, 0xFF));

    // Walls
    {
        uint powered;
        uint wt = wallAt(x, y, powered);
        int i = x % CELL, j = y % CELL;
        int3 wc;
        bool blobMode = (pc.renderMode & PMODE_BLOB) != 0;
        if (simpleWalls || (!blobMode && fragHeads[c] == 0))
        {
            if (wt != 0 && wallPattern(wt, powered, x, y, i, j, wc))
                px = wc;
        }
        else
        {
            // What DrawWalls draws on this pixel, put in its order (cell by cell; in a cell, its pattern, then its
            // blobs and sets pixel by pixel, then a powered E-hole's dots; a stream cell's line, then its icon). The
            // entries are blends and sets (w up to 255) or streamlines' saturating white adds (w = -amount).
            uint keys[WALL_OPS];
            int4 cols[WALL_OPS];
            uint n = 0;
            uint ownCell = uint(y / CELL) * pc.CW + uint(x / CELL);
            if (wt != 0 && wallPattern(wt, powered, x, y, i, j, wc))
            {
                keys[n] = ownCell * 64;
                cols[n++] = int4(wc, 255);
            }
            for (uint f = fragHeads[c]; f != 0 && n < WALL_OPS;)
            {
                uint4 fr = frags[f - 1];
                uint op = fr.z >> 28;
                if (op == OP_WALL_ADD)
                {
                    keys[n] = fr.x;
                    cols[n++] = int4(0, 0, 0, -int(min(fr.y, 255u)));
                }
                else if (op == OP_WALL_BLEND)
                {
                    keys[n] = fr.x;
                    cols[n++] = unpackRGBA(fr.y);
                }
                f = fr.z & FRAG_NEXT;
            }
            for (int dy = -1; dy <= 1 && blobMode; dy++)
                for (int dx = -1; dx <= 1; dx++)
                {
                    int sx = x + dx, sy = y + dy;
                    uint sp;
                    uint swt = wallAt(sx, sy, sp);
                    if (swt == 0)
                        continue;
                    bool blob, set, late;
                    int3 bc, sc;
                    int si = sx % CELL, sj = sy % CELL;
                    wallBlob(swt, sp, sx, sy, si, sj, blob, bc, set, late, sc);
                    uint sCell = uint(sy / CELL) * pc.CW + uint(sx / CELL);
                    if (dx == 0 && dy == 0)
                    {
                        if (set && n < WALL_OPS)
                        {
                            keys[n] = sCell * 64 + (late ? 48u : 16u) + uint(sj * CELL + si);
                            cols[n++] = int4(sc, 255);
                        }
                        continue;
                    }
                    if (blob && n < WALL_OPS)
                    {
                        keys[n] = sCell * 64 + 16 + uint(sj * CELL + si);
                        cols[n++] = int4(bc, (dx == 0 || dy == 0) ? 112 : 64);
                    }
                }
            for (uint a = 1; a < n; a++)
            {
                uint k = keys[a];
                int4 v = cols[a];
                int b = int(a) - 1;
                while (b >= 0 && keys[b] > k)
                {
                    keys[b + 1] = keys[b];
                    cols[b + 1] = cols[b];
                    b--;
                }
                keys[b + 1] = k;
                cols[b + 1] = v;
            }
            for (uint a = 0; a < n; a++)
            {
                if (cols[a].w < 0)
                    px = min(px - cols[a].w, int3(0xFF, 0xFF, 0xFF));
                else
                    px = cols[a].w == 255 ? cols[a].xyz : blendRGB(px, cols[a]);
            }
        }
    }

    // render_parts begins with the grid (G): lines every gridSize cells, or a checkerboard
    int gs4 = int((pc.overlays >> 8) & 0xFF) * 4;
    if (gs4 > 0)
    {
        bool on = (pc.overlays & 0x10000) != 0 ? ((x / gs4 + y / gs4) % 2) != 0 : (y % gs4 == 0 || x % gs4 == 0);
        if (on)
            px = blendRGB(px, int4(0x64, 0x64, 0x64, 80));
    }
    return px;
}

// After the particles: the persistent display's copy, the fire glow, the EMP flash; then the image
void finishPixel(int x, int y, uint c, int3 px)
{
    // DISPLAY_PERS keeps this frame (without the fire glow), each colour component one step fainter (RGB::Decay)
    if ((pc.displayMode & DISPLAY_PERS) != 0)
        persistVideo[c] = packRGB(max(px - int3(1, 1, 1), int3(0, 0, 0)));

    // render_fire: the fire buffer's soft glow (saturating adds, so the cells' order doesn't matter)
    if ((pc.renderMode & FIREMODE) != 0)
    {
        int cx0 = x / CELL, cy0 = y / CELL;
        for (int cy = cy0 - 1; cy <= cy0 + 1; cy++)
            for (int cx = cx0 - 1; cx <= cx0 + 1; cx++)
            {
                if (cx < 0 || cy < 0 || cx >= int(pc.CW) || cy >= int(pc.CH))
                    continue;
                int3 fc = unpackRGB(fireBuf[uint(cy) * pc.CW + uint(cx)]);
                if (fc.x == 0 && fc.y == 0 && fc.z == 0)
                    continue;
                int ox = x - cx * CELL + CELL, oy = y - cy * CELL + CELL;
                if (ox < 0 || oy < 0 || ox >= 3 * CELL || oy >= 3 * CELL)
                    continue;
                int a = int(renderTables[RT_FIRE_ALPHA + uint(oy * 3 * CELL + ox)]);
                if (findActive())
                    a /= 2;
                px = min(int3(0xFF, 0xFF, 0xFF), px + (a * fc) / 0xFF); // RGB::AddFire
            }
    }
    // draw_other: the blue flash of EMP going off
    if ((pc.renderMode & EFFECT) != 0)
    {
        int d = clamp(int(counters[CTR_EMP_DECOR]), 0, 40);
        if (d > 0)
        {
            int r = min(int(float(d) * 2.5), 255), g = min(int(100.0 + float(d) * 1.5), 255);
            int a = min(int((1.0 * float(d) / 110.0) * 255.0), 255);
            px = blendRGB(px, int4(r, g, 255, a));
        }
    }
    // draw_grav_zones: the cells Newtonian gravity reaches (outside GRAV wall boxes), tinted, a yellow diagonal in each
    if ((pc.overlays & 2) != 0 && gravCells[GRAV_MASK * pc.CW * pc.CH + uint(y / CELL) * pc.CW + uint(x / CELL)] != 0)
        px = blendRGB(px, (x % CELL) == (y % CELL) ? int4(0xFF, 0xC8, 0x00, 120) : int4(0x20, 0x20, 0x20, 120));
    video[c] = packRGB(px);
}
