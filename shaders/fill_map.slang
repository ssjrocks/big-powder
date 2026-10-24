// What Simulation::FloodParts checks at every pixel (GpuSim::readFillMap), for its span fill on the CPU
// (sim/Editing): FloodFillPmapCheck for the type being filled over (cm: 0 for empty), and with something to draw
// (c), that no wall blocks it there. With nothing to draw, cm is what is at the fill's start: in pmap, else an
// energy particle.
#include "common.slang"
#include "move.slang"

// Word 0: cm (0xFFFFFFFF if c is 0 and nothing is at the start); then a bit a pixel, row by row, 32 a word
[[vk::binding(33, 0)]] RWStructuredBuffer<uint> edit;

uint liveType(uint e)
{
    int id = pmapId(e);
    return id >= 0 ? ptype[id] : PT_NONE;
}

[shader("compute")]
[numthreads(256, 1, 1)]
void main(uint3 tid: SV_DispatchThreadID)
{
    uint words = (pc.W * pc.H + 31) / 32;
    if (tid.x >= words)
        return;
    uint c = uint(pc.brushType);
    uint cm = 0;
    if (c == 0)
    {
        uint start = cellIndex(pc.brushX, pc.brushY);
        cm = liveType(pmap[start]);
        if (cm == PT_NONE)
            cm = liveType(photons[start]);
        if (cm == PT_NONE)
        {
            if (tid.x == 0)
                edit[0] = 0xFFFFFFFFu;
            return;
        }
    }
    if (tid.x == 0)
        edit[0] = cm;
    bool energy = cm != 0 && isEnergy(cm);
    uint bits = 0;
    for (uint b = 0; b < 32; b++)
    {
        uint i = tid.x * 32 + b;
        if (i >= pc.W * pc.H)
            break;
        int x = int(i % pc.W), y = int(i / pc.W);
        bool ok;
        if (cm == 0)
            ok = liveType(pmap[i]) == PT_NONE && liveType(photons[i]) == PT_NONE;
        else if (energy)
            ok = liveType(photons[i]) == cm;
        else
            ok = liveType(pmap[i]) == cm;
        if (ok && c != 0 && isWallBlocking(x, y, c))
            ok = false;
        if (ok)
            bits |= 1u << b;
    }
    edit[1 + tid.x] = bits;
}
