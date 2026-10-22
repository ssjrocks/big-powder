// Inserts particles from the inbox (e.g. a TPT save placed as a stamp), as TPT's Simulation::Load pastes a save.
// pc.brushType = number of inbox entries, pc.brushX/brushY = offset added to their positions.
//  - pc.mode 0: what was already where the save puts a particle is removed (top particle and energy particle);
//  - pc.mode 1: every particle is placed, stacked ones too, with ids from the free list. The id each one got is
//    written back to its inbox entry's pad2 (0xFFFFFFFF if it wasn't placed), for fixing SOAP's links.
#include "common.slang"

[shader("compute")]
[numthreads(64, 1, 1)]
void main(uint3 tid: SV_DispatchThreadID)
{
    if (tid.x >= uint(pc.brushType))
        return;
    Particle p = inbox[tid.x];
    uint t = p.pad1;
    bool ok = t != PT_NONE && t < uint(MAX_ELEMENTS) && elems[t].enabled != 0;
    p.x += float(pc.brushX);
    p.y += float(pc.brushY);
    int x = int(floor(p.x + 0.5)), y = int(floor(p.y + 0.5));
    ok = ok && x >= CELL && y >= CELL && x < int(pc.W) - CELL && y < int(pc.H) - CELL;
    uint c = ok ? cellIndex(x, y) : 0u;
    if (pc.mode == 0)
    {
        if (!ok)
            return;
        int id = pmapId(pmap[c]);
        if (id >= 0 && ptype[id] != PT_NONE)
            killPart(uint(id), x, y);
        id = pmapId(photons[c]);
        if (id >= 0 && ptype[id] != PT_NONE)
            killPart(uint(id), x, y);
        return;
    }
    inbox[tid.x].pad2 = 0xFFFFFFFFu;
    if (!ok)
        return;
    uint slot;
    InterlockedAdd(counters[CTR_FREE_TAKEN], 1u, slot);
    if (slot >= counters[CTR_FREE_COUNT])
        return;
    uint id = freelist[slot];
    p.pass = 0xFFFFFFFFu;
    p.pad1 = 0;
    p.pad2 = 0;
    parts[id] = p;
    ptype[id] = t;
    // On top where it has the highest id, as RecalcFreeParticles sorts it out after loading
    uint orig;
    if ((elems[t].properties & TYPE_ENERGY) != 0)
        InterlockedMax(photons[c], pmapEnc(id), orig);
    else
        InterlockedMax(pmap[c], pmapEnc(id), orig);
    inbox[tid.x].pad2 = id;
}
