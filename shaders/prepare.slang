// Start of a tick: TPT's RecalcFreeParticles life handling, then sort every live particle into the
// colour list for its position. Unused ids go into the free list that create_part takes from during the
// update passes (CTR_FREE_COUNT, cleared before this pass).
#include "common.slang"

groupshared uint sHist[256];
groupshared uint sBase[256];
groupshared uint sFree;
groupshared uint sFreeBase;

[shader("compute")]
[numthreads(256, 1, 1)]
void main(uint3 tid: SV_DispatchThreadID, uint gi: SV_GroupIndex)
{
    sHist[gi] = 0; // 256 threads, 256 colours
    if (gi == 0)
        sFree = 0;
    GroupMemoryBarrierWithGroupSync();

    uint i = tid.x;
    int colour = -1;
    uint slot = 0;
    bool isFree = i < pc.capacity;
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
            else if (p.life > 0 && (e.properties & PROP_LIFE_DEC) != 0 &&
                     !(inSim(x, y) && bmapAt(x, y) == WL_STASIS && emapAt(x, y) < 8))
            {
                p.life--;
                parts[i].life = p.life;
                if (p.life <= 0 && (e.properties & (PROP_LIFE_KILL_DEC | PROP_LIFE_KILL)) != 0)
                {
                    killPart(i, x, y);
                    alive = false;
                }
            }
            else if (p.life <= 0 && (e.properties & PROP_LIFE_KILL) != 0 &&
                     !(inSim(x, y) && bmapAt(x, y) == WL_STASIS && emapAt(x, y) < 8))
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
                if ((e.properties & TYPE_ENERGY) != 0)
                    InterlockedMax(photons[cellIndex(x, y)], pmapEnc(i), orig); // energy particles stack freely
                else
                {
                    InterlockedMax(pmap[cellIndex(x, y)], pmapEnc(i), orig);
                    if (orig != 0)
                        InterlockedAdd(counters[CTR_STACKED], 1u);
                }
            }

            if (alive)
            {
                isFree = false;
                // BeforeSim: WIRE's previous state for this tick's WireWorld step
                if (t == PT_WIRE)
                    parts[i].tmp = p.ctype;
                if (t == PT_LOVE || t == PT_LOLZ)
                    counters[CTR_LOVE] = 1; // (love.slang)
                // The ETRD that sparks may arc to this tick (etrdNearest in elements.slang)
                if (t == PT_ETRD)
                {
                    uint k;
                    InterlockedAdd(counters[CTR_ETRD_COUNT], 1u, k);
                    if (k < pc.CW * pc.CH)
                        emapReqs[6 * pc.CW * pc.CH + k] = i;
                }
                uint cx = (uint(max(x, 0)) + pc.colourOX) % pc.colourNX;
                uint cy = (uint(max(y, 0)) + pc.colourOY) % pc.colourNY;
                colour = int(cy * pc.colourNX + cx);
                parts[i].pass = uint(colour);
                InterlockedAdd(sHist[colour], 1u, slot);
            }
        }
    }
    uint freeSlot = 0;
    if (isFree)
        InterlockedAdd(sFree, 1u, freeSlot);
    GroupMemoryBarrierWithGroupSync();

    if (gi < pc.colourNX * pc.colourNY && sHist[gi] > 0)
    {
        uint base;
        InterlockedAdd(counters[CTR_COLOUR_COUNT + gi], sHist[gi], base);
        sBase[gi] = base;
    }
    if (gi == 0 && sFree > 0)
    {
        uint base;
        InterlockedAdd(counters[CTR_FREE_COUNT], sFree, base);
        sFreeBase = base;
    }
    GroupMemoryBarrierWithGroupSync();

    if (isFree)
        freelist[sFreeBase + freeSlot] = i;

    if (colour >= 0)
    {
        uint idx = sBase[colour] + slot;
        if (idx < pc.listCap)
            lists[uint(colour) * pc.listCap + idx] = i;
        else
            InterlockedAdd(counters[CTR_LIST_OVERFLOW], 1u);
    }
}
