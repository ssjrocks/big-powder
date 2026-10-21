// Start of a tick: TPT's RecalcFreeParticles life handling, then sort every live particle into the
// colour list for its position.
#include "common.slang"

groupshared uint sHist[32];
groupshared uint sBase[32];

[shader("compute")]
[numthreads(256, 1, 1)]
void main(uint3 tid: SV_DispatchThreadID, uint gi: SV_GroupIndex)
{
    if (gi < 32)
        sHist[gi] = 0;
    GroupMemoryBarrierWithGroupSync();

    uint i = tid.x;
    int colour = -1;
    uint slot = 0;
    if (i < pc.capacity)
    {
        uint t = ptype[i];
        if (t != PT_NONE)
        {
            Particle p = parts[i];
            ElementProps e = elems[t];
            int x = int(p.x + 0.5);
            int y = int(p.y + 0.5);
            bool alive = true;
            if (e.enabled == 0)
            {
                killPart(i, x, y);
                alive = false;
            }
            else if (p.life > 0 && (e.properties & PROP_LIFE_DEC) != 0)
            {
                p.life--;
                parts[i].life = p.life;
                if (p.life <= 0 && (e.properties & (PROP_LIFE_KILL_DEC | PROP_LIFE_KILL)) != 0)
                {
                    killPart(i, x, y);
                    alive = false;
                }
            }
            else if (p.life <= 0 && (e.properties & PROP_LIFE_KILL) != 0)
            {
                killPart(i, x, y);
                alive = false;
            }

            if (alive && inSim(x, y))
            {
                // Rebuild pmap from the particle list, as TPT's RecalcFreeParticles does every frame (pmap
                // was cleared before this pass). Where particles share a cell the highest id wins, as in
                // TPT where the later index overwrites.
                uint orig;
                InterlockedMax(pmap[cellIndex(x, y)], pmapEnc(i), orig);
                if (orig != 0)
                    InterlockedAdd(counters[CTR_STACKED], 1u);
            }

            if (alive)
            {
                uint n = pc.colourN;
                uint cx = (uint(max(x, 0)) + pc.colourOX) % n;
                uint cy = (uint(max(y, 0)) + pc.colourOY) % n;
                colour = int(cy * n + cx);
                parts[i].pass = uint(colour);
                InterlockedAdd(sHist[colour], 1u, slot);
            }
        }
    }
    GroupMemoryBarrierWithGroupSync();

    if (gi < pc.colourN * pc.colourN && sHist[gi] > 0)
    {
        uint base;
        InterlockedAdd(counters[CTR_COLOUR_COUNT + gi], sHist[gi], base);
        sBase[gi] = base;
    }
    GroupMemoryBarrierWithGroupSync();

    if (colour >= 0)
    {
        uint idx = sBase[colour] + slot;
        if (idx < pc.listCap)
            lists[uint(colour) * pc.listCap + idx] = i;
        else
            InterlockedAdd(counters[CTR_LIST_OVERFLOW], 1u);
    }
}
