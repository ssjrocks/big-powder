// TPT's Simulation::FloodINST: spark a connected INST network, with TPT's rules for 1 px INST wires crossing.
// Run by one thread after the update passes for every request queued by requestInstFlood(), visiting cells
// in TPT's order (a stack of scanline starts).
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

[shader("compute")]
[numthreads(1, 1, 1)]
void main(uint3 tid: SV_DispatchThreadID)
{
    uint n = min(counters[CTR_INST_REQ], pc.CW * pc.CH);
    for (uint k = 0; k < n; k++)
    {
        uint v = emapReqs[pc.CW * pc.CH + k];
        floodInst(int(v & 0xFFFF), int(v >> 16));
    }
}
