// TPT's wall tools on the GPU (sim/Editing): wall cells take their new types (TPT's bmap; the electricity in emap
// stays, and walls_pre.slang works out what blocks air next tick) and fans their velocities (fvx, fvy).
#include "common.slang"

// GpuSim::recordWallEdits: how many, then four words each: the cell and its type (above bit 24), what to set
// (1 the type, 2 the fan velocity), the velocity's x and y
[[vk::binding(33, 0)]] RWStructuredBuffer<uint> edit;

[shader("compute")]
[numthreads(64, 1, 1)]
void main(uint3 tid: SV_DispatchThreadID)
{
    uint at = pc.colourIdx;
    if (tid.x >= edit[at])
        return;
    uint e = at + 1 + tid.x * 4;
    uint ci = edit[e] & 0xFFFFFF, type = edit[e] >> 24, what = edit[e + 1];
    if (ci >= pc.CW * pc.CH)
        return;
    if ((what & 1) != 0)
        walls[ci] = (walls[ci] & ~0xFFu) | type;
    if ((what & 2) != 0)
    {
        airSet(AIR_FVX, ci, asfloat(edit[e + 2]));
        airSet(AIR_FVY, ci, asfloat(edit[e + 3]));
    }
}
