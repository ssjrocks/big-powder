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
    // TPT's PMAP(v, t): the element, and above PMAPBITS a value for create_part (a Game of Life ruleset)
    int brushT = pc.brushType & 0x1FF, brushV = pc.brushType >> 9;

    if (brushT == 0)
    {
        // delete_part: an energy particle there goes first
        int pid = pmapId(photons[c]);
        int id = (pid >= 0 && ptype[pid] != PT_NONE) ? pid : pmapId(e);
        if (id >= 0)
            killPart(uint(id), x, y);
        return;
    }

    if (brushT == int(PT_SPRK))
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
        uint bt = uint(brushT);
        bool ctypeDraw = ut == PT_CLNE || ut == PT_BCLN || ut == PT_DTEC || ut == PT_LDTC || ut == PT_CONV ||
                         ut == PT_PVOD || (ut == PT_STOR && (elems[bt].properties & TYPE_SOLID) == 0) ||
                         ((ut == PT_PCLN || ut == PT_PBCN) && bt != PT_PSCN && bt != PT_NSCN && bt != PT_SPRK);
        ctypeDraw = ctypeDraw || ut == PT_CRAY || ut == PT_DRAY || (ut == PT_PSTN && bt != PT_FRME);
        if (ctypeDraw && ut != bt && (elems[bt].properties & PROP_NOCTYPEDRAW) == 0)
        {
            parts[under].ctype = int(bt);
            if (ut == PT_CRAY)
            {
                if (bt == PT_LIGH)
                    parts[under].ctype |= 30 << 9; // LIGH's length
                parts[under].temp = elemDefaults[bt].temp;
            }
        }
        return;
    }
    uint t = uint(brushT);
    if (t == PT_LIGH && (dx != 0 || dy != 0))
        return; // LIGH: one bolt at the brush centre, as strong as the brush is big
    if (isStickman(t) && (dx != 0 || dy != 0))
        return; // stick men are placed one at a time, where clicked (TPT's PlopTool)
    if ((e & PMAP_LOCK) != 0 || isWallBlocking(x, y, t))
        return;
    bool energy = isEnergy(t);
    if (energy && photonTypeAt(x, y) != PT_NONE)
        return; // an energy particle is already there
    if ((isStickman(t) || t == PT_SPAWN || t == PT_SPAWN2) && !stickmanCreateAllowed(t))
        return; // one per player, at most 100 fighters
    uint slot;
    InterlockedAdd(counters[CTR_FREE_TAKEN], 1u, slot);
    if (slot >= counters[CTR_FREE_COUNT])
        return;
    uint id = freelist[slot];
    uint orig;
    if (energy)
        InterlockedExchange(photons[c], pmapEnc(id), orig);
    else if (!isStickman(t)) // (a stick man goes into pmap next tick, as in TPT)
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
    if (t == PT_LIFE)
        lifeCreate(p, brushV);
    else
        runElementCreate(t, p, x, y);
    // Simulation::CreateParts: the brush size sets LIGH's power and TESC's
    if (t == PT_LIGH)
    {
        p.life = min(2 * r, 55);
        p.temp = float(p.life) * 150.0;
    }
    else if (t == PT_TESC)
        p.tmp = min(8 * r + 7, 300);
    parts[id] = p;
    ptype[id] = t;
}
