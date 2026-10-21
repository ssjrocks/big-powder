// Debug statistics, per particle: counts by type, and particles that pmap doesn't point back to.
#include "common.slang"

[shader("compute")]
[numthreads(256, 1, 1)]
void main(uint3 tid: SV_DispatchThreadID)
{
    uint i = tid.x;
    if (i >= pc.capacity)
        return;
    uint t = ptype[i];
    if (t == PT_NONE)
        return;
    InterlockedAdd(stats[t], 1u);
    Particle p = parts[i];
    int x = int(p.x + 0.5);
    int y = int(p.y + 0.5);
    // (Energy particles stack freely in photons, so only the top one of a stack is listed there.)
    if (!inSim(x, y))
        InterlockedAdd(stats[STAT_PART_NOT_IN_PMAP], 1u);
    else if ((elems[t].properties & TYPE_ENERGY) == 0 && (pmap[cellIndex(x, y)] & PMAP_ID_MASK) != pmapEnc(i))
        InterlockedAdd(stats[STAT_PART_NOT_IN_PMAP], 1u);
}
