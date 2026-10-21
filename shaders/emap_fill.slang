// TPT's Simulation::set_emap: electrify a connected network of conductive walls, with TPT's rules for wires
// crossing. TPT recurses; this is the same algorithm on an explicit stack, in the same visiting order, run by
// one thread at the end of the tick for every request queued by requestEmap(). In TPT the walls are
// electrified mid-tick, but emap only matters from the next tick on (walls react when it counts down to 12,
// and decay is applied in BeforeSim), so doing it at the end of the tick gives the same result.
#include "common.slang"

// The stack lives in the colour lists buffer, which is free once the update passes have run.
static const uint FRAME = 5; // y, x1, x2, stage, xi

uint wallFlat(int x, int y)
{
    // TPT indexes bmap[y][x] with x possibly -1 or XCELLS, which reads the neighbouring row; same here.
    int idx = y * int(pc.CW) + x;
    if (idx < 0 || idx >= int(pc.CW * pc.CH))
        return 0;
    return walls[uint(idx)];
}

bool wire(int x, int y)
{
    uint b = wallFlat(x, y) & 0xFF;
    return b == WL_DETECT || b == WL_EWALL || b == WL_ALLOWLIQUID || b == WL_WALLELEC || b == WL_ALLOWALLELEC ||
           b == WL_EHOLE || b == WL_STASIS;
}

bool wireOff(int x, int y) { return wire(x, y) && ((wallFlat(x, y) >> 8) & 0xFF) < 8; }

void setEmap16(int x, int y)
{
    uint ci = uint(y) * pc.CW + uint(x);
    walls[ci] = (walls[ci] & ~0xFF00u) | (16u << 8);
}

static uint g_sp;

// Starts a set_emap(x, y) call: returns false if it returns immediately.
bool enter(int x, int y)
{
    if (!wireOff(x, y))
        return false;
    int x1 = x, x2 = x;
    while (x1 > 0)
    {
        if (!wireOff(x1 - 1, y))
            break;
        x1--;
    }
    while (x2 < int(pc.CW) - 1)
    {
        if (!wireOff(x2 + 1, y))
            break;
        x2++;
    }
    for (int xx = x1; xx <= x2; xx++)
        setEmap16(xx, y);
    uint b = g_sp * FRAME;
    lists[b + 0] = uint(y);
    lists[b + 1] = uint(x1);
    lists[b + 2] = uint(x2);
    lists[b + 3] = 0; // stage
    lists[b + 4] = uint(x1);
    g_sp++;
    return true;
}

void fill(int sx, int sy)
{
    int YC = int(pc.CH);
    g_sp = 0;
    enter(sx, sy);
    uint maxFrames = (pc.listCap * pc.colourNX * pc.colourNY) / FRAME;
    while (g_sp > 0)
    {
        if (g_sp >= maxFrames)
            return; // out of stack space (would need a network bigger than the world)
        uint b = (g_sp - 1) * FRAME;
        int y = int(lists[b + 0]), x1 = int(lists[b + 1]), x2 = int(lists[b + 2]);
        uint stage = lists[b + 3];
        int xi = int(lists[b + 4]);
        if (stage == 0)
        {
            // Children above: a single-cell span with a wire crossing jumps over it.
            if (y > 1 && x1 == x2 && wire(x1 - 1, y - 1) && wire(x1, y - 1) && wire(x1 + 1, y - 1) &&
                !wire(x1 - 1, y - 2) && wire(x1, y - 2) && !wire(x1 + 1, y - 2))
            {
                lists[b + 3] = 2;
                enter(x1, y - 2);
            }
            else if (y > 0)
            {
                lists[b + 3] = 1;
                lists[b + 4] = uint(x1);
            }
            else
                lists[b + 3] = 2;
        }
        else if (stage == 1)
        {
            if (xi > x2)
            {
                lists[b + 3] = 2;
                continue;
            }
            lists[b + 4] = uint(xi + 1);
            if (wireOff(xi, y - 1) &&
                (xi == x1 || xi == x2 || y >= YC - 1 || wire(xi - 1, y - 1) || wire(xi + 1, y - 1) ||
                 wire(xi - 1, y + 1) || !wire(xi, y + 1) || wire(xi + 1, y + 1)))
                enter(xi, y - 1);
        }
        else if (stage == 2)
        {
            // Children below.
            if (y < YC - 2 && x1 == x2 && wire(x1 - 1, y + 1) && wire(x1, y + 1) && wire(x1 + 1, y + 1) &&
                !wire(x1 - 1, y + 2) && wire(x1, y + 2) && !wire(x1 + 1, y + 2))
            {
                lists[b + 3] = 4;
                enter(x1, y + 2);
            }
            else if (y < YC - 1)
            {
                lists[b + 3] = 3;
                lists[b + 4] = uint(x1);
            }
            else
                lists[b + 3] = 4;
        }
        else if (stage == 3)
        {
            if (xi > x2)
            {
                lists[b + 3] = 4;
                continue;
            }
            lists[b + 4] = uint(xi + 1);
            if (wireOff(xi, y + 1) &&
                (xi == x1 || xi == x2 || y < 0 || wire(xi - 1, y + 1) || wire(xi + 1, y + 1) ||
                 wire(xi - 1, y - 1) || !wire(xi, y - 1) || wire(xi + 1, y - 1)))
                enter(xi, y + 1);
        }
        else
        {
            g_sp--;
        }
    }
}

[shader("compute")]
[numthreads(1, 1, 1)]
void main(uint3 tid: SV_DispatchThreadID)
{
    uint n = min(counters[CTR_EMAP_REQ], pc.CW * pc.CH);
    for (uint r = 0; r < n; r++)
    {
        uint ci = emapReqs[r];
        fill(int(ci % pc.CW), int(ci / pc.CW));
    }
}
