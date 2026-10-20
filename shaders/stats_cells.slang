// Debug statistics, per cell: pmap entries that are stale, misplaced or still locked.
#include "common.slang"

[shader("compute")]
[numthreads(256, 1, 1)]
void main(uint3 tid: SV_DispatchThreadID)
{
    uint c = tid.x;
    if (c >= pc.W * pc.H)
        return;
    uint e = pmap[c];
    if ((e & PMAP_LOCK) != 0)
        InterlockedAdd(stats[STAT_PMAP_LOCKED], 1u);
    int id = pmapId(e);
    if (id < 0)
        return;
    if (ptype[id] == PT_NONE)
    {
        InterlockedAdd(stats[STAT_PMAP_DANGLING], 1u);
        return;
    }
    Particle p = parts[id];
    int x = int(p.x + 0.5);
    int y = int(p.y + 0.5);
    if (!inSim(x, y) || cellIndex(x, y) != c)
        InterlockedAdd(stats[STAT_PMAP_WRONG_POS], 1u);
}
