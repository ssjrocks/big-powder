// Circle brush: creates particles of brushType in empty cells, or erases with brushType 0.
#include "common.slang"
#include "elements.slang"

[shader("compute")]
[numthreads(64, 1, 1)]
void main(uint3 tid: SV_DispatchThreadID)
{
    int r = pc.brushR;
    int side = 2 * r + 1;
    if (tid.x >= uint(side * side))
        return;
    int dx = int(tid.x) % side - r;
    int dy = int(tid.x) / side - r;
    if (dx * dx + dy * dy > r * r)
        return;
    int x = pc.brushX + dx;
    int y = pc.brushY + dy;
    if (x < CELL || y < CELL || x >= int(pc.W) - CELL || y >= int(pc.H) - CELL)
        return;
    uint c = cellIndex(x, y);
    uint e = pmap[c];

    if (pc.brushType == 0)
    {
        // delete_part: an energy particle there goes first
        int pid = pmapId(photons[c]);
        int id = (pid >= 0 && ptype[pid] != PT_NONE) ? pid : pmapId(e);
        if (id >= 0)
            killPart(uint(id), x, y);
        return;
    }

    if (pc.brushType == int(PT_SPRK))
    {
        // create_part(-2, x, y, PT_SPRK): sparks the conductor under the brush; INST sparks its whole network.
        rngSeed(c ^ 0x5BD1E995u);
        sparkAt(x, y, true);
        return;
    }

    // Drawing onto an element with a CtypeDraw function sets what it works on (cloners, CONV, PVOD, DTEC, LDTC)
    // instead of creating anything.
    int under = pmapId(e);
    if (under >= 0 && ptype[under] != PT_NONE)
    {
        uint ut = ptype[under];
        uint bt = uint(pc.brushType);
        bool ctypeDraw = ut == PT_CLNE || ut == PT_BCLN || ut == PT_DTEC || ut == PT_LDTC || ut == PT_CONV ||
                         ut == PT_PVOD || (ut == PT_STOR && (elems[bt].properties & TYPE_SOLID) == 0) ||
                         ((ut == PT_PCLN || ut == PT_PBCN) && bt != PT_PSCN && bt != PT_NSCN && bt != PT_SPRK);
        if (ctypeDraw && ut != bt && (elems[bt].properties & PROP_NOCTYPEDRAW) == 0)
            parts[under].ctype = int(bt);
        return;
    }
    uint t = uint(pc.brushType);
    if ((e & PMAP_LOCK) != 0 || isWallBlocking(x, y, t))
        return;
    bool energy = isEnergy(t);
    if (energy && photonTypeAt(x, y) != PT_NONE)
        return; // an energy particle is already there
    uint slot;
    InterlockedAdd(counters[CTR_FREE_TAKEN], 1u, slot);
    if (slot >= counters[CTR_FREE_COUNT])
        return;
    uint id = freelist[slot];
    uint orig;
    if (energy)
        InterlockedExchange(photons[c], pmapEnc(id), orig);
    else
    {
        InterlockedCompareExchange(pmap[c], e, pmapEnc(id), orig);
        if (orig != e)
            return; // the id stays free and is found again next time
    }

    rngSeed(id ^ 0xB5297A4Du);
    // create_part: the element's DefaultProperties, then its Create function.
    Particle p = elemDefaults[t];
    p.x = float(x);
    p.y = float(y);
    p.vx = 0.0;
    p.vy = 0.0;
    p.pass = 0xFFFFFFFFu;
    p.pad1 = 0;
    p.pad2 = 0;
    runElementCreate(t, p, x, y);
    parts[id] = p;
    ptype[id] = t;
}
