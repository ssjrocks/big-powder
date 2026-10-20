// Collects unused particle ids so the brush can create particles. Also clears pmap entries that still
// point at dead particles, so an id is never reused while a stale entry refers to it.
// capacity == W*H, so thread i handles both particle slot i and cell i.
#include "common.slang"

groupshared uint sCount;
groupshared uint sBase;

[shader("compute")]
[numthreads(256, 1, 1)]
void main(uint3 tid: SV_DispatchThreadID, uint gi: SV_GroupIndex)
{
    if (gi == 0)
        sCount = 0;
    GroupMemoryBarrierWithGroupSync();

    uint i = tid.x;
    if (i < pc.W * pc.H)
    {
        uint e = pmap[i];
        int id = pmapId(e);
        if (id >= 0 && ptype[id] == PT_NONE)
        {
            uint orig;
            InterlockedCompareExchange(pmap[i], e, 0u, orig);
        }
    }

    bool isFree = i < pc.capacity && ptype[i] == PT_NONE;
    uint slot = 0;
    if (isFree)
        InterlockedAdd(sCount, 1u, slot);
    GroupMemoryBarrierWithGroupSync();

    if (gi == 0 && sCount > 0)
    {
        uint base;
        InterlockedAdd(counters[CTR_FREE_COUNT], sCount, base);
        sBase = base;
    }
    GroupMemoryBarrierWithGroupSync();

    if (isFree)
        freelist[sBase + slot] = i;
}
