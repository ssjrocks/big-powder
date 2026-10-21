// Element update functions, ported from TPT's src/simulation/elements/*.cpp.
// Only reactions between elements that exist in Phase 0 are included.
#pragma once
#include "common.slang"

void partChangeType(uint id, uint t)
{
    ptype[id] = t;
}

// WATR.cpp: water puts out fire.
int updateWATR(uint i, inout Particle p, int x, int y)
{
    for (int rx = -1; rx <= 1; rx++)
    {
        for (int ry = -1; ry <= 1; ry++)
        {
            if (rx == 0 && ry == 0)
                continue;
            uint r = pmap[cellIndex(x + rx, y + ry)];
            int rid = pmapId(r);
            if (rid < 0)
                continue;
            uint rt = ptype[rid];
            if (rt == PT_SALT && rngChance(1, 50))
            {
                // WATR + SALT -> SLTW + SALT (SLTW not ported yet; SALT is not enabled on the GPU until it is).
            }
            else if ((rt == PT_RBDM || rt == PT_LRBD) && (params[0].legacyHeat != 0 || p.temp > (273.15 + 12.0)) &&
                     rngChance(1, 100))
            {
                // WATR + RBDM/LRBD -> FIRE + RBDM/LRBD
                partChangeType(i, PT_FIRE);
                p.life = 4;
                p.ctype = int(PT_WATR);
            }
            else if (rt == PT_FIRE && parts[rid].ctype != int(PT_WATR))
            {
                killPart(uint(rid), x + rx, y + ry);
                if (rngChance(1, 30))
                {
                    killPart(i, x, y);
                    return 1;
                }
            }
        }
    }
    return 0;
}

// FIRE.cpp (Element_FIRE_update): burning spreads to flammable neighbours within 2 px.
int updateFIRE(uint i, inout Particle p, uint t, int x, int y, int surroundSpace)
{
    if (t == PT_FIRE && p.life <= 1)
    {
        if ((p.tmp & 0x3) == 3)
        {
            // FIRE + O2 + H2 -> WTRV(FIRE): needs O2, H2 and WTRV, not ported yet.
        }
        else if (p.temp < 625.0)
        {
            // FIRE -> SMKE
            partChangeType(i, PT_SMKE);
            p.life = rngBetween(250, 269);
        }
    }
    // The PLSM and LAVA cases of this function need elements that are not ported yet.
    for (int rx = -2; rx <= 2; rx++)
    {
        for (int ry = -2; ry <= 2; ry++)
        {
            if (rx == 0 && ry == 0)
                continue;
            uint r = pmap[cellIndex(x + rx, y + ry)];
            int rid = pmapId(r);
            if (rid < 0)
                continue;
            uint rt = ptype[rid];
            if (rt == PT_NONE)
                continue;
            ElementProps re = elems[rt];
            if ((surroundSpace != 0 || re.explosive != 0) && re.flammable != 0 &&
                rngChance(int(float(re.flammable) + airPV(airIndex(x + rx, y + ry)) * 10.0), 1000) &&
                (rt != PT_SPNG || parts[rid].life == 0))
            {
                partChangeType(uint(rid), PT_FIRE);
                parts[rid].temp = restrictFlt(elems[PT_FIRE].defaultTemp + float(re.flammable) / 2.0, MIN_TEMP, MAX_TEMP);
                parts[rid].life = rngBetween(180, 259);
                parts[rid].tmp = 0;
                parts[rid].ctype = 0;
                if (re.explosive != 0)
                {
                    uint ci = airIndex(x, y);
                    setAirPV(ci, airPV(ci) + 0.25 * CFDS);
                }
            }
        }
    }
    return 0;
}

// Calls the element's update function. Returns nonzero if the particle was killed.
int runElementUpdate(uint i, inout Particle p, uint t, int x, int y, int surroundSpace, int nt)
{
    switch (t)
    {
    case PT_WATR:
        return updateWATR(i, p, x, y);
    case PT_FIRE:
        return updateFIRE(i, p, t, x, y, surroundSpace);
    default:
        return 0;
    }
}
