// TPT's simulation tools (simulation/simtools) and its property tool (gui/game/tool/PropertyTool) on the GPU. sim/Editing
// works out on the CPU where a use reaches and how often, as TPT's SimTool and PropertyTool would call Perform and
// SetProperty; each kind of edit then runs here:
//   - CELLS: AIR, VAC, AMBM, AMBP, WIND, PGRV, NGRV act on the air cell of each pixel they reach, the same way every
//     time, so each cell gets its count of them, applied one after another as TPT does (with its clamping).
//   - PIXELS: HEAT and COOL change the temperature of the particle at each pixel, the property tool sets a property
//     of it; a box of counts.
//   - CYCLONE: CYCL turns the air around the brush's centre, at the cells' corner pixels, by an amount that depends
//     on where the corner is from the centre (worked out on the CPU with TPT's arithmetic, a table over the brush), in
//     the order of the brush's stamps along the line; CYCLONE_LIST is a box's, one amount a cell.
//   - MIX: the swaps MIX makes, in TPT's order, by one thread (where they happen is random, as in TPT; sim/Editing
//     draws it).
#include "common.slang"
#include "elements.slang"
#include "tool_common.slang"

// GpuSim::recordToolEdits: each edit's words, from pc.colourIdx (see GpuEditor::simTool)
[[vk::binding(33, 0)]] RWStructuredBuffer<uint> edit;

static const uint OP_CELLS = 1, OP_PIXELS = 2, OP_CYCLONE = 3, OP_CYCLONE_LIST = 4, OP_MIX = 5;
static const uint TOOL_HEAT = 0, TOOL_COOL = 1, TOOL_AIR = 2, TOOL_VAC = 3, TOOL_PGRV = 4, TOOL_NGRV = 5,
                  TOOL_AMBM = 8, TOOL_AMBP = 9, TOOL_WIND = 10, TOOL_PROPERTY = 100;
static const uint STATE_FLAGS = TYPE_PART | TYPE_LIQUID | TYPE_SOLID | TYPE_GAS | TYPE_ENERGY;

float clampPressure(float v) { return v > MAX_PRESSURE ? MAX_PRESSURE : v < MIN_PRESSURE ? MIN_PRESSURE : v; }

// Simulation::part_change_type, as the property tool calls it
void toolChangeType(uint i, uint t)
{
    Particle p = parts[i];
    int x = int(p.x + 0.5), y = int(p.y + 0.5);
    if (!inSim(x, y) || t >= MAX_ELEMENTS || ptype[i] == PT_NONE)
        return;
    if (elems[t].existsInTpt == 0 || t == PT_NONE)
    {
        killPart(i, x, y);
        return;
    }
    if (!gpuHas(t) || !createAllowed(t))
        return;
    // the ChangeType functions: SOAP lets go of its bubble, STKM and STKM2 get their legs and FIGH a slot (a stick
    // man or spawn point turned into something else is let go of in stickmen.slang)
    if (ptype[i] == PT_SOAP && t != PT_SOAP)
        soapDetach(i);
    if (isStickman(t))
        registerStickman(i, x, y, t, false);
    ptype[i] = t;
    uint c = cellIndex(x, y), me = pmapEnc(i), orig;
    if (isEnergy(t))
    {
        InterlockedExchange(photons[c], me, orig);
        InterlockedCompareExchange(pmap[c], me, 0u, orig);
    }
    else
    {
        InterlockedExchange(pmap[c], me, orig);
        InterlockedCompareExchange(photons[c], me, 0u, orig);
    }
}

// AccessProperty::Set
void setProperty(uint i, uint field, uint bits)
{
    switch (field)
    {
    case 0: toolChangeType(i, bits); break;
    case 1: parts[i].life = int(bits); break;
    case 2: parts[i].ctype = int(bits); break;
    case 3: parts[i].x = asfloat(bits); break;
    case 4: parts[i].y = asfloat(bits); break;
    case 5: parts[i].vx = asfloat(bits); break;
    case 6: parts[i].vy = asfloat(bits); break;
    case 7: parts[i].temp = asfloat(bits); break;
    case 8: parts[i].flags = bits; break;
    case 9: parts[i].tmp = int(bits); break;
    case 10: parts[i].tmp2 = int(bits); break;
    case 11: parts[i].tmp3 = int(bits); break;
    case 12: parts[i].tmp4 = int(bits); break;
    case 13: parts[i].dcolour = bits; break;
    default: break;
    }
}

// The particle a tool finds at a pixel: the one in pmap, else an energy particle
int toolTarget(uint c)
{
    int id = liveId(pmap[c]);
    return id >= 0 ? id : liveId(photons[c]);
}

// [op, tool, n, a, b], then n of [cell, count]
void cells(uint at, uint k)
{
    if (k >= edit[at + 2])
        return;
    uint tool = edit[at + 1];
    float a = asfloat(edit[at + 3]), b = asfloat(edit[at + 4]);
    uint ci = edit[at + 5 + 2 * k], n = edit[at + 6 + 2 * k];
    if (ci >= pc.CW * pc.CH)
        return;
    switch (tool)
    {
    case TOOL_AIR:
    case TOOL_VAC:
    {
        float v = airGet(AIR_PV, ci);
        for (uint r = 0; r < n; r++)
            v = clampPressure(v + a);
        airSet(AIR_PV, ci, v);
        break;
    }
    case TOOL_AMBM:
    case TOOL_AMBP:
    {
        if (params[0].ambientHeat == 0)
            return;
        float v = airGet(AIR_HV, ci);
        for (uint r = 0; r < n; r++)
        {
            v += a;
            if (v > MAX_TEMP)
                v = MAX_TEMP;
            if (v < MIN_TEMP)
                v = MIN_TEMP;
        }
        airSet(AIR_HV, ci, v);
        break;
    }
    case TOOL_WIND:
    {
        float vx = airGet(AIR_VX, ci), vy = airGet(AIR_VY, ci);
        for (uint r = 0; r < n; r++)
        {
            vx += a;
            vy += b;
        }
        airSet(AIR_VX, ci, vx);
        airSet(AIR_VY, ci, vy);
        break;
    }
    case TOOL_PGRV:
    case TOOL_NGRV:
        if (params[0].newtonian != 0) // (TPT clears the mass every tick, so with gravity off it never counts)
            gravCells[GRAV_MASS * pc.CW * pc.CH + ci] = asuint(a);
        break;
    }
}

// [op, tool, x0, y0, w, h, bits a count (2 or 16), p0, p1, p2], then the counts row by row
void pixels(uint at, uint tx, uint ty)
{
    uint w = edit[at + 4], h = edit[at + 5];
    if (tx >= w || ty >= h)
        return;
    uint i = ty * w + tx;
    uint n = edit[at + 6] == 16 ? (edit[at + 10 + (i >> 1)] >> ((i & 1) * 16)) & 0xFFFF
                                : (edit[at + 10 + (i >> 4)] >> ((i & 15) * 2)) & 3;
    int x = int(edit[at + 2]) + int(tx), y = int(edit[at + 3]) + int(ty);
    if (n == 0 || !inSim(x, y))
        return;
    uint c = cellIndex(x, y);
    uint tool = edit[at + 1];
    if (tool == TOOL_PROPERTY)
    {
        // PropertyTool::SetProperty, each time it reaches the pixel (more than twice changes nothing)
        for (uint r = 0; r < min(n, 2u); r++)
        {
            int id = toolTarget(c);
            if (id < 0)
                return;
            setProperty(uint(id), edit[at + 7], edit[at + 8]);
        }
        return;
    }
    // HEAT and COOL: PUMP and GPMP change by a twentieth as much
    int id = toolTarget(c);
    if (id < 0)
        return;
    uint t = ptype[id];
    float d = asfloat(edit[at + (t == PT_PUMP || t == PT_GPMP ? 8 : 7)]);
    float temp = parts[id].temp;
    for (uint r = 0; r < n; r++)
    {
        temp += d;
        if (temp > MAX_TEMP)
            temp = MAX_TEMP;
        else if (temp < 0.0)
            temp = 0.0;
    }
    parts[id].temp = temp;
}

// [op, stamps, rx, ry, cx0, cy0, cw, ch], then the stamps' centres (x, y), the brush (a bit a point, row by row) and
// what CYCL adds to the velocity at each point of it (x, y)
void cyclone(uint at, uint tx, uint ty)
{
    uint stamps = edit[at + 1];
    int rx = int(edit[at + 2]), ry = int(edit[at + 3]);
    uint cw = edit[at + 6], ch = edit[at + 7];
    if (tx >= cw || ty >= ch)
        return;
    int cx = int(edit[at + 4] + tx), cy = int(edit[at + 5] + ty);
    if (cx >= int(pc.CW) || cy >= int(pc.CH))
        return;
    int x = cx * CELL, y = cy * CELL;
    uint bw = uint(2 * rx + 1), points = bw * uint(2 * ry + 1);
    uint stampAt = at + 8, bitsAt = stampAt + 2 * stamps, tableAt = bitsAt + (points + 31) / 32;
    uint ci = uint(cy) * pc.CW + uint(cx);
    float vx = airGet(AIR_VX, ci), vy = airGet(AIR_VY, ci);
    bool changed = false;
    for (uint s = 0; s < stamps; s++)
    {
        int ox = x - int(edit[stampAt + 2 * s]), oy = y - int(edit[stampAt + 2 * s + 1]);
        if (ox < -rx || ox > rx || oy < -ry || oy > ry || (ox == 0 && oy == 0))
            continue;
        uint k = uint(oy + ry) * bw + uint(ox + rx);
        if (((edit[bitsAt + (k >> 5)] >> (k & 31)) & 1) == 0)
            continue;
        vx = clampPressure(vx + asfloat(edit[tableAt + 2 * k]));
        vy = clampPressure(vy + asfloat(edit[tableAt + 2 * k + 1]));
        changed = true;
    }
    if (changed)
    {
        airSet(AIR_VX, ci, vx);
        airSet(AIR_VY, ci, vy);
    }
}

// [op, n], then n of [cell, what it adds to vx, to vy]
void cycloneList(uint at, uint k)
{
    if (k >= edit[at + 1])
        return;
    uint e = at + 2 + 3 * k, ci = edit[e];
    if (ci >= pc.CW * pc.CH)
        return;
    airSet(AIR_VX, ci, clampPressure(airGet(AIR_VX, ci) + asfloat(edit[e + 1])));
    airSet(AIR_VY, ci, clampPressure(airGet(AIR_VY, ci) + asfloat(edit[e + 2])));
}

// [op, n], then n of [x, y, the other pixel's x, y]: MIX's swaps of two particles of the same state
void mix(uint at)
{
    uint n = edit[at + 1];
    for (uint k = 0; k < n; k++)
    {
        uint e = at + 2 + 4 * k;
        int x = int(edit[e]), y = int(edit[e + 1]), nx = int(edit[e + 2]), ny = int(edit[e + 3]);
        if (!inSim(x, y) || !inSim(nx, ny))
            continue;
        uint c = cellIndex(x, y), c2 = cellIndex(nx, ny);
        int a = liveId(pmap[c]);
        if (a < 0 || (elems[ptype[a]].properties & (TYPE_PART | TYPE_LIQUID | TYPE_GAS)) == 0)
            continue;
        int b = liveId(pmap[c2]);
        if (b < 0 || (elems[ptype[a]].properties & STATE_FLAGS) != (elems[ptype[b]].properties & STATE_FLAGS))
            continue;
        pmap[c] = pmapEnc(uint(b));
        parts[b].x = float(x);
        parts[b].y = float(y);
        pmap[c2] = pmapEnc(uint(a));
        parts[a].x = float(nx);
        parts[a].y = float(ny);
    }
}

[shader("compute")]
[numthreads(16, 16, 1)]
void main(uint3 tid: SV_DispatchThreadID)
{
    uint at = pc.colourIdx;
    switch (edit[at])
    {
    case OP_CELLS: cells(at, tid.y * uint(pc.brushR) + tid.x); break;
    case OP_PIXELS: pixels(at, tid.x, tid.y); break;
    case OP_CYCLONE: cyclone(at, tid.x, tid.y); break;
    case OP_CYCLONE_LIST: cycloneList(at, tid.y * uint(pc.brushR) + tid.x); break;
    case OP_MIX:
        if (tid.x == 0 && tid.y == 0)
            mix(at);
        break;
    }
}
