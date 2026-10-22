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
    {
        // A stick man goes into pmap only the tick after it appears, and its head goes on top of whatever it
        // passes through, as in TPT; when it moves on, the cell is left empty until the next tick.
        uint c = cellIndex(x, y);
        int top = pmapId(pmap[c]);
        bool left = false;
        uint n = min(counters[CTR_STKM_LEFT], STKM_LEFT_CAP);
        for (uint k = 0; k < n && !left; k++)
            left = counters[CTR_STKM_LEFT + 1 + k] == c;
        if (!isStickman(t) && !(top >= 0 && isStickman(ptype[top])) && !left)
            InterlockedAdd(stats[STAT_PART_NOT_IN_PMAP], 1u);
    }
}
