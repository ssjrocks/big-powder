// render_fire's smoothing and decay of the fire buffer, after resolve.slang drew its glow. TPT does it in place, cell
// by cell in rows, so each cell sees the cells above it and to its left already smoothed and the rest not yet. One
// workgroup keeps exactly that order: each thread takes a row and walks along it two columns behind the row above
// (waves along x + 2y). The row above's new values come through a small ring in shared memory; old values come from
// the buffer, which no other thread writes before they are read. Only the area with fire (and a cell around it) is
// gone over (and three cells around it): elsewhere everything stays 0.
#include "render_common.slang"

static const uint MAX_ROWS = 2048;
groupshared uint ring[MAX_ROWS * 4]; // each row's last four new values, by column % 4

uint cellAt(int x, int y)
{
    if (x < 0 || y < 0 || x >= int(pc.CW) || y >= int(pc.CH))
        return 0;
    return fireBuf[uint(y) * pc.CW + uint(x)];
}

[shader("compute")]
[numthreads(1024, 1, 1)]
void main(uint3 tid: SV_DispatchThreadID, uint gi: SV_GroupIndex)
{
    int CW = int(pc.CW), CH = int(min(pc.CH, MAX_ROWS));
    uint bx0 = counters[CTR_FIRE_BOX], by0 = counters[CTR_FIRE_BOX + 1];
    uint bx1 = counters[CTR_FIRE_BOX + 2], by1 = counters[CTR_FIRE_BOX + 3];
    if (bx0 > bx1 || by0 > by1)
        return; // no fire anywhere
    // New values spread right and down within one sweep: two cells out they can still come to 1, three cells out
    // the decay always wins
    int x0 = max(int(bx0) - 3, 0), y0 = max(int(by0) - 3, 0);
    int x1 = min(int(bx1) + 3, CW - 1), y1 = min(int(by1) + 3, CH - 1);
    int3 left[2]; // each thread's rows' last new value (a thread has at most two rows)
    left[0] = int3(0, 0, 0);
    left[1] = int3(0, 0, 0);
    for (int w = x0 + 2 * y0; w <= x1 + 2 * y1; w++)
    {
        for (int r = 0; r < 2; r++)
        {
            int y = y0 + int(gi) + r * 1024;
            if (y > y1)
                continue;
            int x = w - 2 * y;
            if (x < x0 || x > x1)
                continue;
            int3 s = unpackRGB(cellAt(x, y)) * 8;
            if (x - 1 >= x0)
                s += left[r]; // (left of the area, everything is 0)
            s += unpackRGB(cellAt(x + 1, y));
            if (y - 1 >= y0)
                for (int dx = -1; dx <= 1; dx++)
                    if (x + dx >= x0 && x + dx <= x1)
                        s += unpackRGB(ring[uint(y - 1) * 4 + uint(x + dx) % 4]);
            for (int dx = -1; dx <= 1; dx++)
                s += unpackRGB(cellAt(x + dx, y + 1));
            s /= 16;
            s = max(s - int3(4, 4, 4), int3(0, 0, 0));
            uint v = packRGB(s);
            fireBuf[uint(y) * pc.CW + uint(x)] = v;
            ring[uint(y) * 4 + uint(x) % 4] = v;
            left[r] = s;
        }
        GroupMemoryBarrierWithGroupSync();
    }
}
