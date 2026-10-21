// One colour pass of the particle update: TPT's UpdateParticles loop body for every particle in one
// colour list. Particles in the same list are a multiple of colourNX pixels apart horizontally and
// colourNY vertically, so with 5x5 their reactions (which reach at most 2 px) never touch the same particle.
#include "common.slang"
#include "move.slang"
#include "elements.slang"

Neighbourhood getNeighbourhood(uint t, int x, int y)
{
    Neighbourhood n;
    n.surroundSpace = 0;
    n.nt = 0;
    int j = 0;
    for (int nx = -1; nx < 2; nx++)
    {
        for (int ny = -1; ny < 2; ny++)
        {
            if (nx != 0 || ny != 0)
            {
                uint r = pmap[cellIndex(x + nx, y + ny)];
                uint rt = pmapType(r);
                n.surround[j] = rt == PT_NONE ? 0 : r;
                j++;
                n.surroundSpace += (rt == PT_NONE) ? 1 : 0;
                n.nt += (rt != t) ? 1 : 0;
            }
        }
    }
    n.pGravX = 0.0;
    n.pGravY = 0.0;
    ElementProps e = elems[t];
    if ((e.properties & TYPE_SOLID) == 0 && e.gravity != 0.0)
        gravityField(x, y, e.gravity, 0.0, n.pGravX, n.pGravY); // Newtonian gravity not ported yet
    return n;
}

// The pressure CRMC's melting point rises with (TPT samples 5 cells around it).
float crmcPressure(int x, int y)
{
    float sum = airPV(airIndex(x, y)) + airPV(airIndex(x, y - 2)) + airPV(airIndex(x, y + 2)) +
                airPV(airIndex(x - 2, y)) + airPV(airIndex(x + 2, y));
    return max(sum * 2.0, 0.0);
}

// Returns true if the particle died. `t` is updated if the type changed.
bool transitionPhase(uint i, inout Particle p, inout uint t, int x, int y, Neighbourhood nb,
                     out bool transitionOccurred)
{
    transitionOccurred = false;
    ElementProps e = elems[t];
    uint ci = airIndex(x, y);

    if (params[0].legacyHeat == 0 && (e.properties & TYPE_LIQUID) != 0)
    {
        // Some heat convection for liquids: swap heat with the same liquid "above" (against gravity).
        // (GEL's chance check is not ported yet.)
        float cgx, cgy;
        gravityField(x, y, -2.0, -2.0, cgx, cgy);
        int ox = clamp(int(roundAway(cgx + float(x))), x - 1, x + 1);
        int oy = clamp(int(roundAway(cgy + float(y))), y - 1, y + 1);
        if (ox != x || oy != y)
        {
            uint r = pmap[cellIndex(ox, oy)];
            int rid = pmapId(r);
            if (rid >= 0 && ptype[rid] == t)
            {
                float other = parts[rid].temp;
                if (p.temp > other)
                {
                    parts[rid].temp = p.temp;
                    p.temp = other;
                }
            }
        }
    }

    if (params[0].legacyHeat != 0)
    {
        // Heat simulation off: TPT's legacy mode (legacy element behaviour is not ported yet).
    }
    else if (!isHeatInsulator(t) && rngChance(e.heatConduct, 250))
    {
        // Heat transfer with air (ambient heat).
        if (params[0].ambientHeat != 0 && (e.properties & PROP_NOAMBHEAT) == 0)
        {
            float hv = airGet(AIR_HV, ci);
            float dtemp = hv - p.temp;
            float hc0 = e.heatCapacity;
            float alpha = min(0.04, 0.4 * hc0); // alpha / heat_capacity must be < 1
            // TPT ignores that there are CELL^2 "air pixels" in a cell, and the heat capacity of air.
            p.temp = restrictFlt(p.temp + alpha * dtemp / hc0, MIN_TEMP, MAX_TEMP);
            airSet(AIR_HV, ci, restrictFlt(hv - alpha * dtemp, MIN_TEMP, MAX_TEMP));
        }
        // Heat transfer with other elements.
        float hcTotal = 0.0;
        float cHeat = 0.0;
        int conduct[8];
        for (int j = 0; j < 8; j++)
        {
            conduct[j] = -1;
            int rid = pmapId(nb.surround[j]);
            if (rid < 0)
                continue;
            uint rt = ptype[rid];
            if (rt == PT_NONE || isHeatInsulator(rt))
                continue;
            conduct[j] = rid;
            float hc = elems[rt].heatCapacity;
            cHeat += parts[rid].temp * hc;
            hcTotal += hc;
        }
        cHeat += p.temp * e.heatCapacity;
        hcTotal += e.heatCapacity;
        float pt = restrictFlt(cHeat / hcTotal, MIN_TEMP, MAX_TEMP);
        p.temp = pt;
        for (int j = 0; j < 8; j++)
        {
            if (conduct[j] >= 0)
                parts[conduct[j]].temp = pt;
        }

        float ctemph = pt;
        float ctempl = pt;
        // Boiling point changes with pressure.
        if (((e.properties & TYPE_LIQUID) != 0 && e.highTemperatureT >= 0 &&
             (elems[e.highTemperatureT].properties & TYPE_GAS) != 0) ||
            t == PT_LNTG || t == PT_SLTW)
            ctemph -= 2.0 * airPV(ci);
        else if (((e.properties & TYPE_GAS) != 0 && e.lowTemperatureT >= 0 &&
                  (elems[e.lowTemperatureT].properties & TYPE_LIQUID) != 0) ||
                 t == PT_WTRV)
            ctempl -= 2.0 * airPV(ci);
        bool s = true;
        uint meltedFrom = t; // the ctype ICEI, SNOW and LAVA get

        // A fix for ice with ctype = 0.
        if ((t == PT_ICEI || t == PT_SNOW) &&
            (p.ctype <= 0 || p.ctype >= MAX_ELEMENTS || elems[p.ctype].enabled == 0 || p.ctype == int(PT_ICEI) ||
             p.ctype == int(PT_SNOW)))
            p.ctype = int(PT_WATR);

        int newT = int(t);
        if (e.highTemperatureT != NT && ctemph >= e.highTemperature)
        {
            // Particle type change due to high temperature.
            if (e.highTemperatureT != ST)
            {
                if (t == PT_FOG)
                    p.ctype = 0; // clear unnecessary ctype
                newT = e.highTemperatureT;
            }
            else if (t == PT_ICEI || t == PT_SNOW)
            {
                if (p.ctype > 0 && p.ctype < MAX_ELEMENTS && p.ctype != int(t))
                {
                    ElementProps ce = elems[p.ctype];
                    if (ce.lowTemperatureT == int(PT_ICEI) || ce.lowTemperatureT == int(PT_SNOW))
                    {
                        if (pt < ce.lowTemperature)
                            s = false;
                    }
                    else if (pt < 273.15)
                        s = false;
                    if (s && !gpuHas(uint(p.ctype)))
                        s = false;
                    if (s)
                    {
                        newT = p.ctype;
                        p.ctype = int(PT_NONE);
                        p.life = 0;
                    }
                }
                else
                    s = false;
            }
            else if (t == PT_SLTW)
            {
                // SLTW -> SALT/WTRV
                newT = rngChance(1, 4) ? int(PT_SALT) : int(PT_WTRV);
            }
            else if (t == PT_RIME)
            {
                if (p.tmp > 5 && gpuHas(PT_ACID))
                {
                    // RIME -> ACID
                    newT = int(PT_ACID);
                    p.life = 25 + 5 * p.tmp;
                    p.tmp = 0;
                }
                else
                {
                    // RIME -> WATR/DSTW. (Acidic RIME melts to WATR until ACID is ported.)
                    newT = p.ctype == int(PT_DSTW) ? int(PT_DSTW) : int(PT_WATR);
                    p.ctype = 0;
                }
            }
            else if (t == PT_BRMT)
            {
                if (p.ctype == int(PT_TUNG))
                {
                    // BRMT(TUNG) -> LAVA(TUNG)
                    if (ctemph < elems[PT_TUNG].highTemperature)
                        s = false;
                    else
                    {
                        newT = int(PT_LAVA);
                        meltedFrom = PT_TUNG;
                    }
                }
                else if (ctemph >= e.highTemperature)
                    newT = int(PT_LAVA); // BRMT -> LAVA(BMTL)
                else
                    s = false;
            }
            else if (t == PT_CRMC)
            {
                // CRMC -> LAVA(CRMC): ceramic gets stronger under pressure
                if (ctemph < crmcPressure(x, y) + elems[PT_CRMC].highTemperature)
                    s = false;
                else
                    newT = int(PT_LAVA);
            }
            else
                s = false;
        }
        else if (e.lowTemperatureT != NT && ctempl < e.lowTemperature)
        {
            // Particle type change due to low temperature.
            if (e.lowTemperatureT != ST)
                newT = e.lowTemperatureT;
            else if (t == PT_WTRV)
            {
                // WTRV -> RIME/DSTW
                newT = (pt < 273.0) ? int(PT_RIME) : int(PT_DSTW);
            }
            else if (t == PT_LAVA)
            {
                int ct = p.ctype;
                if (ct > 0 && ct < MAX_ELEMENTS && ct != int(PT_LAVA) && elems[ct].existsInTpt != 0)
                {
                    ElementProps ce = elems[ct];
                    if (ct == int(PT_THRM) && pt >= elems[PT_BMTL].highTemperature)
                        s = false;
                    else if ((ct == int(PT_VIBR) || ct == int(PT_BVBR)) && pt >= 273.15)
                        s = false;
                    else if (ct == int(PT_TUNG))
                    {
                        // TUNG melts in its own update; the threshold is still its HighTemperature
                        if (pt >= ce.highTemperature)
                            s = false;
                    }
                    else if (ct == int(PT_CRMC))
                    {
                        if (ctemph >= crmcPressure(x, y) + elems[PT_CRMC].highTemperature)
                            s = false;
                    }
                    else if (ce.highTemperatureT == int(PT_LAVA) || ct == int(PT_HEAC))
                    {
                        if (pt >= ce.highTemperature)
                            s = false;
                    }
                    else if (pt >= 973.0)
                        s = false; // freezing point for lava with any other ctype
                    // LAVA(THRM) -> BMTL, LAVA(PLUT) -> LAVA
                    int solid = ct == int(PT_THRM) ? int(PT_BMTL) : ct == int(PT_PLUT) ? int(PT_LAVA) : ct;
                    if (s && !gpuHas(uint(solid)))
                        s = false; // stays molten until the GPU engine has that element
                    if (s)
                    {
                        newT = solid;
                        p.ctype = int(PT_NONE);
                        if (ct == int(PT_THRM) || ct == int(PT_PLUT))
                            p.tmp = 0;
                    }
                }
                else if (pt < 973.0)
                    newT = int(PT_STNE); // LAVA -> STNE
                else
                    s = false;
            }
            else
                s = false;
        }
        else
            s = false;

        if (s && newT != int(PT_NONE) && !gpuHas(uint(newT)))
            s = false; // into an element the GPU engine doesn't have yet

        if (s)
        {
            if (newT == int(PT_ICEI) || newT == int(PT_LAVA) || newT == int(PT_SNOW))
                p.ctype = int(meltedFrom);
            if (newT == int(PT_RIME))
                p.ctype = int(PT_DSTW);
            if (!(newT == int(PT_ICEI) && p.ctype == int(PT_FRZW)) && newT != int(PT_ACID))
                p.life = 0;
            if (newT == int(PT_FIRE))
            {
                // If tmp isn't 0 the FIRE might turn into DSTW later.
                p.tmp = 0;
                if (t == PT_SEED)
                {
                    p.ctype = 0;
                    p.tmp2 = 0;
                    p.tmp3 = 0;
                    p.tmp4 = 0;
                }
            }
            if ((elems[newT].properties & TYPE_GAS) != 0 && (e.properties & TYPE_GAS) == 0)
                setAirPV(ci, airPV(ci) + 0.5);
            if (newT == int(PT_NONE))
            {
                killPart(i, x, y);
                return true;
            }
            t = uint(newT);
            partChangeType(i, t);
            if (t == PT_FIRE || t == PT_PLSM || t == PT_CFLM)
                p.life = rngBetween(120, 169);
            if (t == PT_LAVA)
            {
                if (p.ctype == int(PT_BRMT))
                    p.ctype = int(PT_BMTL);
                else if (p.ctype == int(PT_SAND))
                    p.ctype = int(PT_GLAS);
                else if (p.ctype == int(PT_BGLA))
                    p.ctype = int(PT_GLAS);
                else if (p.ctype == int(PT_PQRT))
                    p.ctype = int(PT_QRTZ);
                else if (p.ctype == int(PT_LITH) && p.tmp2 > 3)
                    p.ctype = int(PT_GLAS);
                p.life = rngBetween(240, 359);
            }
            transitionOccurred = true;
            e = elems[t];
        }
        p.temp = restrictFlt(p.temp, MIN_TEMP, MAX_TEMP);
        if (t == PT_LAVA)
        {
            p.life = int(restrictFlt((p.temp - 700.0) / 7.0, 0.0, 400.0));
            if (p.ctype == int(PT_THRM) && p.tmp > 0)
            {
                p.tmp--;
                p.temp = 3500.0;
            }
            if (p.ctype == int(PT_PLUT) && p.tmp > 0)
            {
                p.tmp--;
                p.temp = MAX_TEMP;
            }
        }
    }
    else
    {
        bumpBlockAirH(x, y);
        p.temp = restrictFlt(p.temp, MIN_TEMP, MAX_TEMP);
    }

    // The basic explosion, from the Explosive property.
    if ((e.explosive & 2) != 0 && airPV(ci) > 2.5)
    {
        p.life = rngBetween(180, 259);
        p.temp = restrictFlt(elems[PT_FIRE].defaultTemp + float(e.flammable) / 2.0, MIN_TEMP, MAX_TEMP);
        t = PT_FIRE;
        partChangeType(i, t);
        setAirPV(ci, airPV(ci) + 0.25 * CFDS);
        e = elems[t];
    }

    // Pressure transitions (Newtonian gravity is off, so gravtot = 0).
    {
        float pv = airPV(ci);
        int newT = int(t);
        bool s = false;
        if (e.highPressureT != NT && pv > e.highPressure)
        {
            if (e.highPressureT != ST)
            {
                newT = e.highPressureT;
                s = true;
            }
            else if (t == PT_BMTL)
            {
                // BMTL -> BRMT
                if (pv > 2.5 || (pv > 1.0 && p.tmp == 1))
                {
                    newT = int(PT_BRMT);
                    s = true;
                }
            }
        }
        else if (e.lowPressureT != NT && pv < e.lowPressure && 0.0 <= e.lowPressure / 4.0)
        {
            if (e.lowPressureT != ST)
            {
                newT = e.lowPressureT;
                s = true;
            }
        }
        if (s && newT != int(PT_NONE) && !gpuHas(uint(newT)))
            s = false; // into an element the GPU engine doesn't have yet
        if (s)
        {
            if (newT == int(PT_NONE))
            {
                killPart(i, x, y);
                return true;
            }
            p.life = 0;
            if (newT == int(PT_BRMT))
            {
                // So that e.g. PIPE -> BRMT doesn't set BRMT's ctype
                p.ctype = 0;
                p.tmp = 0;
                p.tmp2 = 0;
                p.tmp3 = 0;
                p.tmp4 = 0;
            }
            t = uint(newT);
            partChangeType(i, t);
            if (t == PT_FIRE)
                p.life = rngBetween(120, 169);
            transitionOccurred = true;
        }
    }
    return false;
}

// Stores the particle's updated state. x and y are left alone: doMove (and in-place create_part) already
// wrote them, and writing them here could undo a swap made by another thread that displaced this particle.
void writeBack(uint i, Particle p)
{
    parts[i].life = p.life;
    parts[i].ctype = p.ctype;
    parts[i].vx = p.vx;
    parts[i].vy = p.vy;
    parts[i].temp = p.temp;
    parts[i].tmp = p.tmp;
    parts[i].tmp2 = p.tmp2;
    parts[i].tmp3 = p.tmp3;
    parts[i].tmp4 = p.tmp4;
    parts[i].flags = p.flags;
    parts[i].dcolour = p.dcolour;
}

void updateParticle(uint i)
{
    uint t = ptype[i];
    if (t == PT_NONE)
        return;
    rngSeed(i);
    Particle p = parts[i];
    int x = int(p.x + 0.5);
    int y = int(p.y + 0.5);

    // Kill a particle off screen.
    if (!inPlayArea(x, y))
    {
        killPart(i, x, y);
        return;
    }

    ElementProps e = elems[t];
    uint ci = airIndex(x, y);

    // Kill a particle in a wall where it isn't supposed to go (stickmen excepted, not ported yet).
    uint wall = bmapAt(x, y);
    uint emap = emapAt(x, y);
    if (wall != 0 &&
        (wall == WL_WALL || wall == WL_WALLELEC || wall == WL_ALLOWAIR || wall == WL_DESTROYALL ||
         (wall == WL_ALLOWLIQUID && (e.properties & TYPE_LIQUID) == 0) ||
         (wall == WL_ALLOWPOWDER && (e.properties & TYPE_PART) == 0) ||
         (wall == WL_ALLOWGAS && (e.properties & TYPE_GAS) == 0) ||
         (wall == WL_ALLOWENERGY && (e.properties & TYPE_ENERGY) == 0) ||
         (wall == WL_EWALL && emap == 0)))
    {
        killPart(i, x, y);
        return;
    }
    // Stasis walls stop particles from ticking.
    if (wall == WL_STASIS && emap < 8)
        return;
    if (wall == WL_DETECT && emap < 8)
        requestEmap(x / CELL, y / CELL);

    // Add to the air velocity from the particle's velocity.
    setAirVX(ci, airVX(ci) * e.airLoss + e.airDrag * p.vx);
    setAirVY(ci, airVY(ci) * e.airLoss + e.airDrag * p.vy);
    if (e.hotAir != 0.0)
        setAirPV(ci, airPV(ci) + 4.0 * e.hotAir);

    Neighbourhood nb = getNeighbourhood(t, x, y);

    // Velocity updates.
    if (t != PT_SPNG || (p.flags & FLAG_MOVABLE) == 0)
    {
        p.vx *= e.loss;
        p.vy *= e.loss;
    }
    p.vx += e.advection * airVX(ci) + nb.pGravX;
    p.vy += e.advection * airVY(ci) + nb.pGravY;
    if (e.diffusion != 0.0)
    {
        p.vx += e.diffusion * (2.0 * rngUniform01() - 1.0);
        p.vy += e.diffusion * (2.0 * rngUniform01() - 1.0);
    }

    bool transitionOccurred;
    if (transitionPhase(i, p, t, x, y, nb, transitionOccurred))
        return;

    if (runElementUpdate(i, p, t, x, y, nb.surroundSpace, nb.nt) != 0)
    {
        // TPT skips the movement; the particle may still be alive with a changed state.
        if (ptype[i] != PT_NONE)
            writeBack(i, p);
        return;
    }
    if (ptype[i] == PT_NONE)
        return;
    t = ptype[i];

    if (!transitionOccurred && (p.vx != 0.0 || p.vy != 0.0))
        movementPhase(i, p, t, nb);

    if (ptype[i] == PT_NONE)
        return;
    writeBack(i, p);
}

static const uint SERIAL_CHUNK = 2048;

[shader("compute")]
[numthreads(64, 1, 1)]
void main(uint3 tid: SV_DispatchThreadID)
{
    if ((pc.mode & 2) != 0)
    {
        // Serial reference mode (testing only): one thread updates particle ids [colourIdx, colourIdx +
        // SERIAL_CHUNK) in order, exactly like TPT's UpdateParticles loop.
        if (tid.x != 0)
            return;
        uint end = min(pc.colourIdx + SERIAL_CHUNK, pc.capacity);
        for (uint i = pc.colourIdx; i < end; i++)
            updateParticle(i);
        return;
    }
    uint count = min(counters[CTR_COLOUR_COUNT + pc.colourIdx], pc.listCap);
    if (tid.x >= count)
        return;
    updateParticle(lists[pc.colourIdx * pc.listCap + tid.x]);
}
