// TPT's tools on the GPU (sim/Editing works out where they reach): Simulation::CreatePartFlags, applied to each
// pixel of a box as many times as TPT's tool code applies it there. Repeats matter only for deleting (the first
// takes an energy particle, the second what is under it); creating again where something now is does nothing.
// TPT applies them one pixel after another; here each pixel is a thread, which only changes which ids the new
// particles get.
#include "common.slang"
#include "elements.slang"
#include "tool_common.slang"

// The edits (GpuSim::recordEdits), each a header and then two bits a pixel, row by row, 16 a word:
// x0, y0, w, h, the type (TPT's PMAP(v, t): the element and a value for create_part above PMAPBITS), flags,
// replaceModeSelected (or the type to remove), a salt for the random numbers
[[vk::binding(33, 0)]] RWStructuredBuffer<uint> edit;

static const uint REPLACE_MODE = 1, SPECIFIC_DELETE = 2;
static const int PMAPBITS = 9;
static const uint FLOOD_KILL = 0x100; // FloodParts erasing: remove the particle of the selected type
static const uint SPARK = 0x200;      // create_part(-1, x, y, PT_SPRK) (a button sign's): the conductor there

// Simulation::delete_part: an energy particle there goes first
void deletePart(int x, int y, uint c)
{
    int ph = liveId(photons[c]);
    int id = ph >= 0 ? ph : liveId(pmap[c]);
    if (id >= 0)
        killPart(uint(id), x, y);
}

// What CreatePartFlags looks for in replace mode and specific delete: anything (selected 0), a particle of the
// selected type with no energy particle over it, or an energy particle of that type
bool selectedThere(uint c, uint selected)
{
    int ph = liveId(photons[c]), pm = liveId(pmap[c]);
    if (selected == 0)
        return ph >= 0 || pm >= 0;
    return (ph < 0 && pm >= 0 && ptype[pm] == selected) || (ph >= 0 && ptype[ph] == selected);
}

// Elements with a CtypeDraw function: drawing onto them sets what they work on instead of creating anything
bool hasCtypeDraw(uint t)
{
    return t == PT_CLNE || t == PT_BCLN || t == PT_PCLN || t == PT_PBCN || t == PT_DTEC || t == PT_LDTC ||
           t == PT_CONV || t == PT_DRAY || t == PT_CRAY || t == PT_PSTN || t == PT_STOR || t == PT_VOID ||
           t == PT_PVOD || t == PT_RPEL;
}

// Element::basicCtypeDraw
bool basicCtypeDraw(uint i, uint t)
{
    if (ptype[i] == t || (elems[t].properties & PROP_NOCTYPEDRAW) != 0)
        return false;
    parts[i].ctype = int(t);
    return true;
}

// The elements' CtypeDraw functions, with ctypeDrawVInTmp and ctypeDrawVInCtype
void ctypeDraw(uint i, uint t, int v)
{
    uint ut = ptype[i];
    if ((ut == PT_PCLN || ut == PT_PBCN) && (t == PT_PSCN || t == PT_NSCN || t == PT_SPRK))
        return;
    switch (ut)
    {
    case PT_PCLN:
    case PT_PBCN:
    case PT_CLNE:
    case PT_BCLN:
    case PT_DTEC:
        if (basicCtypeDraw(i, t) && t == PT_LIFE)
            parts[i].tmp = v;
        return;
    case PT_CONV:
    case PT_LDTC:
    case PT_DRAY:
    case PT_CRAY:
        if (!basicCtypeDraw(i, t))
            return;
        if (t == PT_LIFE)
            parts[i].ctype |= v << PMAPBITS;
        if (ut == PT_CRAY)
        {
            if (t == PT_LIGH)
                parts[i].ctype |= 30 << PMAPBITS;
            parts[i].temp = elemDefaults[t].temp;
        }
        return;
    case PT_PSTN:
        if (t != PT_FRME)
            basicCtypeDraw(i, t);
        return;
    case PT_STOR:
        if ((elems[t].properties & TYPE_SOLID) == 0)
            basicCtypeDraw(i, t);
        return;
    default: // VOID, PVOD, RPEL
        basicCtypeDraw(i, t);
        return;
    }
}

// create_part from where its new particle has an id: the element's DefaultProperties, then its Create function with
// TPT's value v
void initPart(uint id, int x, int y, uint t, int v, uint salt)
{
    rngSeed(id ^ 0xB5297A4Du ^ salt);
    Particle p = elemDefaults[t];
    p.x = float(x);
    p.y = float(y);
    p.vx = 0.0;
    p.vy = 0.0;
    p.pass = 0xFFFFFFFFu;
    p.pad1 = 0;
    p.pad2 = 0;
    if (t == PT_LIFE)
        lifeCreate(p, v);
    else
        runElementCreate(t, p, x, y);
    // The Create functions that read v (runElementCreate is create_part's v = -1)
    if (t == PT_LIGH)
    {
        p.life = min(v, 55);
        p.temp = float(p.life) * 150.0;
    }
    else if (t == PT_TESC)
        p.tmp = min(v, 300);
    else if (t == PT_FILT)
        p.tmp = v;
    parts[id] = p;
    ptype[id] = t;
}

// create_part(-2, x, y, t, v): from the brush
void createFromBrush(int x, int y, uint c, uint t, int v, uint salt)
{
    if (!gpuHas(t))
        return;
    int under = liveId(pmap[c]);
    bool drawOn = under >= 0 && hasCtypeDraw(ptype[under]);
    if (t == PT_SPRK && !drawOn)
    {
        // sparks the conductor there; INST sparks its whole network (FloodINST, deferred.slang)
        rngSeed(c ^ 0x5BD1E995u ^ salt);
        sparkAt(x, y, true);
        return;
    }
    if (under >= 0)
    {
        if (drawOn)
            ctypeDraw(uint(under), t, v);
        return;
    }
    if (isWallBlocking(x, y, t))
        return;
    bool energy = isEnergy(t);
    if (energy && liveId(photons[c]) >= 0)
        return;
    if (!createAllowed(t))
        return;
    uint slot;
    InterlockedAdd(counters[CTR_FREE_TAKEN], 1u, slot);
    if (slot >= counters[CTR_FREE_COUNT])
        return;
    uint id = freelist[slot];
    uint e = pmap[c], orig;
    if (energy)
        InterlockedExchange(photons[c], pmapEnc(id), orig);
    else if (!isStickman(t)) // (a stick man goes into pmap next tick, as in TPT)
    {
        InterlockedCompareExchange(pmap[c], e, pmapEnc(id), orig);
        if (orig != e)
            return; // the id stays free and is found again next time
    }
    initPart(id, x, y, t, v, salt);
    if (isStickman(t))
        registerStickman(id, x, y, t, true);
}

// create_part(i, x, y, t, v) for the particle i already there: replace mode turns it into t, keeping its id
void replacePart(uint i, int x, int y, uint c, uint t, int v, uint salt)
{
    if (!gpuHas(t))
        return;
    if (t == PT_SPRK)
    {
        sparkAt(x, y, false);
        return;
    }
    if (!createAllowed(t))
        return;
    Particle old = parts[i];
    int ox = int(old.x + 0.5), oy = int(old.y + 0.5);
    uint orig;
    if (inSim(ox, oy))
    {
        InterlockedCompareExchange(pmap[cellIndex(ox, oy)], pmapEnc(i), 0u, orig);
        InterlockedCompareExchange(photons[cellIndex(ox, oy)], pmapEnc(i), 0u, orig);
    }
    if (isEnergy(t))
        InterlockedExchange(photons[c], pmapEnc(i), orig);
    else if (!isStickman(t))
        InterlockedExchange(pmap[c], pmapEnc(i), orig);
    initPart(i, x, y, t, v, salt);
}

// Simulation::CreatePartFlags
void createPartFlags(int x, int y, uint t, int v, uint flags, uint selected, uint salt)
{
    uint c = cellIndex(x, y);
    if ((flags & SPARK) != 0)
    {
        sparkAt(x, y, false);
        return;
    }
    if ((flags & FLOOD_KILL) != 0)
    {
        // FloodParts with nothing to draw: the energy particle, or the one in pmap, as the type to remove is
        int id = isEnergy(selected) ? liveId(photons[c]) : liveId(pmap[c]);
        if (id >= 0)
            killPart(uint(id), x, y);
        return;
    }
    if ((flags & REPLACE_MODE) != 0)
    {
        if (selectedThere(c, selected))
        {
            if (t != PT_NONE)
            {
                int ph = liveId(photons[c]);
                replacePart(uint(ph >= 0 ? ph : liveId(pmap[c])), x, y, c, t, v, salt);
            }
            else
                deletePart(x, y, c);
        }
        return;
    }
    if (t == PT_NONE)
        deletePart(x, y, c);
    else if ((flags & SPECIFIC_DELETE) != 0)
    {
        if (selectedThere(c, selected))
            deletePart(x, y, c);
    }
    else
        createFromBrush(x, y, c, t, v, salt);
}

[shader("compute")]
[numthreads(16, 16, 1)]
void main(uint3 tid: SV_DispatchThreadID)
{
    uint at = pc.colourIdx; // this edit's header in edit
    uint w = edit[at + 2], h = edit[at + 3];
    if (tid.x >= w || tid.y >= h)
        return;
    uint i = tid.y * w + tid.x;
    uint n = (edit[at + 8 + (i >> 4)] >> ((i & 15) * 2)) & 3;
    int x = int(edit[at]) + int(tid.x), y = int(edit[at + 1]) + int(tid.y);
    if (n == 0 || !inSim(x, y))
        return;
    uint type = edit[at + 4];
    uint t = type & 0x1FF;
    int v = int(type) >> PMAPBITS;
    uint flags = edit[at + 5], selected = edit[at + 6], salt = edit[at + 7];
    for (uint k = 0; k < n; k++)
        createPartFlags(x, y, t, v, flags, selected, salt + k);
}
