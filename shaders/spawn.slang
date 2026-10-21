// Circle brush: creates particles of brushType in empty cells, or erases with brushType 0.
#include "common.slang"
#include "elements.slang"

[shader("compute")]
[numthreads(64, 1, 1)]
void main(uint3 tid: SV_DispatchThreadID)
{
    int r = pc.brushR;
    int side = 2 * r + 1;
    if (tid.x >= uint(side * side))
        return;
    int dx = int(tid.x) % side - r;
    int dy = int(tid.x) / side - r;
    if (dx * dx + dy * dy > r * r)
        return;
    int x = pc.brushX + dx;
    int y = pc.brushY + dy;
    if (x < CELL || y < CELL || x >= int(pc.W) - CELL || y >= int(pc.H) - CELL)
        return;
    uint c = cellIndex(x, y);
    uint e = pmap[c];

    if (pc.brushType == 0)
    {
        int id = pmapId(e);
        if (id >= 0)
            killPart(uint(id), x, y);
        return;
    }

    if ((e & PMAP_LOCK) != 0 || pmapType(e) != PT_NONE)
        return;
    uint slot;
    InterlockedAdd(counters[CTR_FREE_TAKEN], 1u, slot);
    if (slot >= counters[CTR_FREE_COUNT])
        return;
    uint id = freelist[slot];
    uint orig;
    InterlockedCompareExchange(pmap[c], e, pmapEnc(id), orig);
    if (orig != e)
        return; // the id stays free and is found again next time

    rngSeed(id ^ 0xB5297A4Du);
    uint t = uint(pc.brushType);
    // create_part: the element's DefaultProperties, then its Create function.
    Particle p = elemDefaults[t];
    p.x = float(x);
    p.y = float(y);
    p.vx = 0.0;
    p.vy = 0.0;
    p.pass = 0xFFFFFFFFu;
    p.pad1 = 0;
    p.pad2 = 0;
    runElementCreate(t, p);
    parts[id] = p;
    ptype[id] = t;
}
