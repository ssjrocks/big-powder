// Inserts particles from the inbox (e.g. a TPT save placed as a stamp) into empty cells, taking ids from the
// free list. pc.brushType = number of inbox entries, pc.brushX/brushY = offset added to their positions.
// Particles whose cell is already occupied are dropped, like a stamp drawn over existing material in TPT
// only fills empty space for particles.
#include "common.slang"

[shader("compute")]
[numthreads(64, 1, 1)]
void main(uint3 tid: SV_DispatchThreadID)
{
    if (tid.x >= uint(pc.brushType))
        return;
    Particle p = inbox[tid.x];
    uint t = p.pad1;
    if (t == PT_NONE || t >= uint(MAX_ELEMENTS) || elems[t].enabled == 0)
        return;
    p.x += float(pc.brushX);
    p.y += float(pc.brushY);
    int x = int(p.x + 0.5), y = int(p.y + 0.5);
    if (x < CELL || y < CELL || x >= int(pc.W) - CELL || y >= int(pc.H) - CELL)
        return;
    uint c = cellIndex(x, y);
    uint e = pmap[c];
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
        return;
    p.pass = 0xFFFFFFFFu;
    p.pad1 = 0;
    parts[id] = p;
    ptype[id] = t;
}
