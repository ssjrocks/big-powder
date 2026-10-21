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

    if (pc.brushType == int(PT_SPRK))
    {
        // create_part(-2, x, y, PT_SPRK): sparks the conductor under the brush; INST sparks its whole network.
        rngSeed(c ^ 0x5BD1E995u);
        sparkAt(x, y, true);
        return;
    }

    // Drawing onto an element with a CtypeDraw function sets what it works on (DTEC, LDTC; cloners later)
    // instead of creating anything.
    int under = pmapId(e);
    if (under >= 0 && ptype[under] != PT_NONE)
    {
        uint ut = ptype[under];
        uint bt = uint(pc.brushType);
        if ((ut == PT_DTEC || ut == PT_LDTC) && ut != bt && (elems[bt].properties & PROP_NOCTYPEDRAW) == 0)
            parts[under].ctype = int(bt);
        return;
    }
    if ((e & PMAP_LOCK) != 0)
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
    runElementCreate(t, p, x, y);
    parts[id] = p;
    ptype[id] = t;
}
