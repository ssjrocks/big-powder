// Per-tick wall bookkeeping from TPT's Simulation::BeforeSim (after RecalcFreeParticles): electrified walls
// count down, and walls decide whether they block air and ambient heat this tick.
#include "common.slang"

[shader("compute")]
[numthreads(16, 16, 1)]
void main(uint3 tid: SV_DispatchThreadID)
{
    if (tid.x >= pc.CW || tid.y >= pc.CH)
        return;
    uint ci = tid.y * pc.CW + tid.x;
    uint w = walls[ci];
    uint b = w & 0xFF;
    uint e = (w >> 8) & 0xFF;
    if (e > 0)
        e--;
    bool blockair = b == WL_WALL || b == WL_WALLELEC || b == WL_BLOCKAIR || (b == WL_EWALL && e == 0);
    uint blockairh = (blockair || b == WL_GRAV) ? 0x8u : 0u;
    walls[ci] = b | (e << 8) | (blockairh << 16) | (blockair ? WALL_BLOCKAIR_BIT : 0u);
}
