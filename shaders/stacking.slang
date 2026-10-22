// TPT's CheckStacking (BeforeSim, on 1 tick in 10 and after loading a save): a cell holding more than 5
// particles may collapse. Its lowest-id particle becomes NBHL, as strong as the pile was big, and the rest are
// killed. prepare counts particles (TPT's pmap_count) only in cells where particles stack, and lists them.
//  - colourIdx 0, one group: decides for each listed cell (when pc.mode is 1, a check this tick), clears the
//    others, and sizes the next stages;
//  - colourIdx 1 and 2, over particles (only when a cell was marked): find each marked cell's lowest id, then
//    turn it into NBHL and kill the others;
//  - colourIdx 3, one group: clears the marked cells; colourIdx 4, over all cells (only when more cells
//    stacked than the list holds): clears everything.
#include "common.slang"
#include "elements.slang"

groupshared uint sFound;

[shader("compute")]
[numthreads(256, 1, 1)]
void main(uint3 tid: SV_DispatchThreadID, uint gi: SV_GroupIndex)
{
    uint base = stackBase();
    uint listed = stacking[base];
    uint n = min(listed, STACK_CAP);
    uint cells = base + 1, counts = base + 1 + STACK_CAP, lowest = base + 1 + 2 * STACK_CAP;
    if (pc.colourIdx == 0)
    {
        if (gi == 0)
            sFound = 0;
        GroupMemoryBarrierWithGroupSync();
        for (uint k = gi; k < n; k += 256)
        {
            uint c = stacking[cells + k];
            uint cnt = stacking[c] & STACK_COUNT;
            bool mark = false;
            if (pc.mode == 1 && cnt > 5)
            {
                // (some stacking is normal, e.g. BIZR in FILT; E-holes allow more)
                rngSeed(c ^ 0x6A09E667u);
                int x = int(c % pc.W), y = int(c / pc.W);
                if (bmapAt(x, y) == WL_EHOLE)
                    mark = cnt > 1500;
                else
                    mark = cnt > 1500 || uint(rngBetween(0, 1599)) <= cnt + 100;
            }
            if (mark)
            {
                stacking[c] = STACK_MARK | k;
                stacking[counts + k] = cnt;
                stacking[lowest + k] = 0xFFFFFFFFu;
                sFound = 1;
            }
            else
                stacking[c] = 0;
        }
        GroupMemoryBarrierWithGroupSync();
        if (gi == 0)
        {
            uint groups = sFound != 0 ? (pc.capacity + 255) / 256 : 0;
            counters[CTR_STACK_ARGS] = groups;
            counters[CTR_STACK_ARGS + 1] = 1;
            counters[CTR_STACK_ARGS + 2] = 1;
            counters[CTR_STACK_CLEAR_ARGS] = listed > STACK_CAP ? (base + 255) / 256 : 0;
            counters[CTR_STACK_CLEAR_ARGS + 1] = 1;
            counters[CTR_STACK_CLEAR_ARGS + 2] = 1;
        }
        return;
    }
    if (pc.colourIdx == 3)
    {
        for (uint k = gi; k < n; k += 256)
        {
            uint c = stacking[cells + k];
            if ((stacking[c] & STACK_MARK) != 0)
                stacking[c] = 0;
        }
        return;
    }
    if (pc.colourIdx == 4)
    {
        if (tid.x < base)
            stacking[tid.x] = 0;
        return;
    }

    uint i = tid.x;
    if (i >= pc.capacity)
        return;
    uint t = ptype[i];
    if (t == PT_NONE || (elems[t].properties & TYPE_ENERGY) != 0)
        return;
    int x = int(parts[i].x + 0.5), y = int(parts[i].y + 0.5);
    if (!inSim(x, y))
        return;
    uint v = stacking[cellIndex(x, y)];
    if ((v & STACK_MARK) == 0)
        return;
    uint k = v & (STACK_CAP - 1);
    if (pc.colourIdx == 1)
    {
        uint orig;
        InterlockedMin(stacking[lowest + k], i, orig);
        return;
    }
    if (stacking[lowest + k] == i)
    {
        // stacking -> NBHL, its gravity as strong as the pile was big
        createPartAt(i, x, y, PT_NBHL);
        parts[i].temp = MAX_TEMP;
        parts[i].tmp = int(min(stacking[counts + k], 51200u));
    }
    else
        killPart(i, x, y);
}
