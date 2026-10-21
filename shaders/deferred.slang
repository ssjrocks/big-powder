// Work queued during the update passes and done by one thread afterwards, in this order:
// - FloodINST: spark a connected INST network, with TPT's rules for 1 px INST wires crossing, visiting cells in
//   TPT's order (a stack of scanline starts);
// - BANG's flood_prop: set tmp = 2 on a connected BANG blob;
// - BOMB explosions (they reach further than the colour passes keep particles apart).
#include "common.slang"
#include "elements.slang"

// The stack lives in the colour lists buffer, free once the update passes have run (and between ticks).
static uint g_sp;

bool isSparkableInst(int x, int y)
{
    int id = pmapId(pmap[cellIndex(x, y)]);
    return id >= 0 && ptype[id] == PT_INST && parts[id].life == 0;
}

bool isInst(int x, int y)
{
    int id = pmapId(pmap[cellIndex(x, y)]);
    if (id < 0)
        return false;
    uint t = ptype[id];
    return t == PT_INST || (t == PT_SPRK && parts[id].ctype == int(PT_INST));
}

void push(int x, int y)
{
    if (g_sp < pc.listCap * pc.colourNX * pc.colourNY)
        lists[g_sp++] = uint(x) | (uint(y) << 16);
}

void floodInst(int sx, int sy)
{
    const int W = int(pc.W), H = int(pc.H);
    if (!isSparkableInst(sx, sy))
        return;
    g_sp = 0;
    push(sx, sy);
    while (g_sp > 0)
    {
        uint v = lists[--g_sp];
        int x = int(v & 0xFFFF), y = int(v >> 16);
        int x1 = x, x2 = x;
        // go left and right as far as possible
        while (x1 >= CELL && isSparkableInst(x1 - 1, y))
            x1--;
        while (x2 < W - CELL && isSparkableInst(x2 + 1, y))
            x2++;
        // fill the span
        for (int xx = x1; xx <= x2; xx++)
            createPart(xx, y, PT_SPRK);

        // add vertically adjacent pixels (wire crossing for INST)
        if (y >= CELL + 1 && x1 == x2 && isInst(x1 - 1, y - 1) && isInst(x1, y - 1) && isInst(x1 + 1, y - 1) &&
            !isInst(x1 - 1, y - 2) && isInst(x1, y - 2) && !isInst(x1 + 1, y - 2))
        {
            // travelling vertically up, skipping a horizontal line
            if (isSparkableInst(x1, y - 2))
                push(x1, y - 2);
        }
        else if (y >= CELL + 1)
        {
            for (int xx = x1; xx <= x2; xx++)
            {
                // at the end of a horizontal section, or a T junction, or not a 1 px wire crossing
                if (isSparkableInst(xx, y - 1) &&
                    (xx == x1 || xx == x2 || y >= H - CELL - 1 || !isInst(xx, y + 1) || isInst(xx + 1, y + 1) ||
                     isInst(xx - 1, y + 1)))
                    push(xx, y - 1);
            }
        }

        if (y < H - CELL - 1 && x1 == x2 && isInst(x1 - 1, y + 1) && isInst(x1, y + 1) && isInst(x1 + 1, y + 1) &&
            !isInst(x1 - 1, y + 2) && isInst(x1, y + 2) && !isInst(x1 + 1, y + 2))
        {
            // travelling vertically down, skipping a horizontal line
            if (isSparkableInst(x1, y + 2))
                push(x1, y + 2);
        }
        else if (y < H - CELL - 1)
        {
            for (int xx = x1; xx <= x2; xx++)
            {
                if (isSparkableInst(xx, y + 1) &&
                    (xx == x1 || xx == x2 || y < 0 || !isInst(xx, y - 1) || isInst(xx + 1, y - 1) ||
                     isInst(xx - 1, y - 1)))
                    push(xx, y + 1);
            }
        }
    }
}

// flood_prop(x, y, tmp = 2) for BANG: scanline fill over connected BANG. TPT marks visited pixels in a bitmap;
// here a BANG that already has tmp 2 counts as visited.
bool bangToMark(int x, int y)
{
    int id = pmapId(pmap[cellIndex(x, y)]);
    return id >= 0 && ptype[id] == PT_BANG && parts[id].tmp != 2;
}

void floodBang(int sx, int sy)
{
    const int W = int(pc.W), H = int(pc.H);
    if (!bangToMark(sx, sy))
        return;
    g_sp = 0;
    push(sx, sy);
    while (g_sp > 0)
    {
        uint v = lists[--g_sp];
        int x = int(v & 0xFFFF), y = int(v >> 16);
        if (!bangToMark(x, y))
            continue;
        int x1 = x, x2 = x;
        while (x1 >= CELL && bangToMark(x1 - 1, y))
            x1--;
        while (x2 < W - CELL && bangToMark(x2 + 1, y))
            x2++;
        for (int xx = x1; xx <= x2; xx++)
            parts[pmapId(pmap[cellIndex(xx, y)])].tmp = 2;
        if (y >= CELL + 1)
            for (int xx = x1; xx <= x2; xx++)
                if (bangToMark(xx, y - 1))
                    push(xx, y - 1);
        if (y < H - CELL - 1)
            for (int xx = x1; xx <= x2; xx++)
                if (bangToMark(xx, y + 1))
                    push(xx, y + 1);
    }
}

// BOMB.cpp's explosion: everything within 8 px (except DMND, cloners and VIBR) becomes hot embers and the
// pressure rises; a ring of embers flies out from just beyond.
void explodeBomb(uint id)
{
    if (ptype[id] != PT_BOMB)
        return; // destroyed by an earlier explosion
    int x = int(parts[id].x + 0.5), y = int(parts[id].y + 0.5);
    killPart(id, x, y);
    const int rad = 8;
    for (int nj = -rad; nj <= rad; nj++)
    {
        for (int ni = -rad; ni <= rad; ni++)
        {
            if (ni * ni + nj * nj > rad * rad || !inSim(x + ni, y + nj))
                continue;
            int xx = x + ni, yy = y + nj;
            int rid = pmapId(pmap[cellIndex(xx, yy)]);
            uint nt = rid < 0 ? PT_NONE : ptype[rid];
            if (nt == PT_DMND || nt == PT_CLNE || nt == PT_PCLN || nt == PT_BCLN || nt == PT_VIBR)
                continue;
            if (nt != PT_NONE)
                killPart(uint(rid), xx, yy);
            uint ci = airIndex(xx, yy);
            setAirPV(ci, airPV(ci) + 0.1);
            // BOMB -> EMBR
            int nb = createPartStacked(xx, yy, PT_EMBR);
            if (nb >= 0)
            {
                parts[nb].tmp = 2;
                parts[nb].life = 2;
                parts[nb].temp = MAX_TEMP;
            }
        }
    }
    for (int nj = -(rad + 1); nj <= rad + 1; nj++)
    {
        for (int ni = -(rad + 1); ni <= rad + 1; ni++)
        {
            if (ni * ni + nj * nj > (rad + 1) * (rad + 1) || !inSim(x + ni, y + nj))
                continue;
            if (typeAt(x + ni, y + nj) != PT_NONE)
                continue;
            int nb = createPartStacked(x + ni, y + nj, PT_EMBR);
            if (nb >= 0)
            {
                parts[nb].tmp = 0;
                parts[nb].life = 50;
                parts[nb].temp = MAX_TEMP;
                parts[nb].vx = float(rngBetween(-20, 20));
                parts[nb].vy = float(rngBetween(-20, 20));
            }
        }
    }
}

[shader("compute")]
[numthreads(1, 1, 1)]
void main(uint3 tid: SV_DispatchThreadID)
{
    uint cells = pc.CW * pc.CH;
    uint n = min(counters[CTR_INST_REQ], cells);
    for (uint k = 0; k < n; k++)
    {
        uint v = emapReqs[cells + k];
        floodInst(int(v & 0xFFFF), int(v >> 16));
    }
    n = min(counters[CTR_BANG_REQ], cells);
    for (uint k = 0; k < n; k++)
    {
        uint v = emapReqs[2 * cells + k];
        floodBang(int(v & 0xFFFF), int(v >> 16));
    }
    n = min(counters[CTR_BOMB_REQ], cells);
    for (uint k = 0; k < n; k++)
    {
        rngSeed(0xB0B0u + k);
        explodeBomb(emapReqs[3 * cells + k]);
    }
}
