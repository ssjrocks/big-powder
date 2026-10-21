// Element update functions, ported from TPT's src/simulation/elements/*.cpp.
// Only reactions between elements that exist in Phase 0 are included.
#pragma once
#include "common.slang"

void partChangeType(uint id, uint t)
{
    ptype[id] = t;
}

// WATR.cpp
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
                // WATR + SALT -> SLTW + SALT; on average 3 WATR turn to SLTW before the SALT does.
                partChangeType(i, PT_SLTW);
                if (rngChance(1, 3))
                    partChangeType(uint(rid), PT_SLTW);
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
            else if (rt == PT_SLTW && rngChance(1, 2000))
            {
                // WATR + SLTW -> 2xSLTW
                partChangeType(i, PT_SLTW);
            }
            else if (rt == PT_ROCK && abs(p.vx) + abs(p.vy) >= 0.5 && rngChance(1, 1000))
            {
                // WATR + ROCK -> WATR + SAND/STNE (erosion)
                if (rngChance(1, 3))
                    partChangeType(uint(rid), PT_SAND);
                else
                    partChangeType(uint(rid), PT_STNE);
            }
        }
    }
    return 0;
}

// DSTW.cpp
int updateDSTW(uint i, inout Particle p, int x, int y)
{
    for (int rx = -1; rx <= 1; rx++)
    {
        for (int ry = -1; ry <= 1; ry++)
        {
            if (rx == 0 && ry == 0)
                continue;
            uint r = pmap[cellIndex(x + rx, y + ry)];
            int rid = pmapId(r);
            uint rt = rid < 0 ? PT_NONE : ptype[rid];
            if (rt == PT_SALT)
            {
                if (rngChance(1, 50))
                {
                    // DSTW + SALT -> 2xSLTW
                    partChangeType(i, PT_SLTW);
                    if (rngChance(1, 3))
                        partChangeType(uint(rid), PT_SLTW);
                }
            }
            else if (rt == PT_SLTW || rt == PT_WATR)
            {
                // TPT's switch falls through from SLTW into WATR when the first chance fails.
                bool done = false;
                if (rt == PT_SLTW && rngChance(1, 2000))
                {
                    // DSTW + SLTW -> 2xSLTW
                    partChangeType(i, PT_SLTW);
                    done = true;
                }
                if (!done && rngChance(1, 100))
                {
                    // DSTW + WATR -> 2xWATR
                    partChangeType(i, PT_WATR);
                }
            }
            else if (rt == PT_RBDM || rt == PT_LRBD)
            {
                // TPT compares temp > 12.0 here (Kelvin), unlike WATR's 12 C.
                if ((params[0].legacyHeat != 0 || p.temp > 12.0) && rngChance(1, 100))
                {
                    // DSTW + RBDM/LRBD -> FIRE + RBDM/LRBD
                    partChangeType(i, PT_FIRE);
                    p.life = 4;
                }
            }
            else if (rt == PT_FIRE)
            {
                killPart(uint(rid), x + rx, y + ry);
                if (rngChance(1, 30))
                {
                    killPart(i, x, y);
                    return 1;
                }
            }
            else if (rt == PT_SMKE)
            {
                float rtemp = parts[rid].temp;
                if (rtemp > (40.0 + 273.15) && rtemp < (60.0 + 273.15) && p.temp > (40.0 + 273.15) &&
                    p.temp < (60.0 + 273.15))
                {
                    if (rngChance(1, 100) && gpuHas(PT_BASE))
                    {
                        // DSTW + SMKE -> BASE (once BASE is ported)
                        partChangeType(i, PT_BASE);
                        p.life = 1;
                        killPart(uint(rid), x + rx, y + ry);
                    }
                }
            }
        }
    }
    return 0;
}

// SLTW.cpp
int updateSLTW(uint i, inout Particle p, int x, int y)
{
    for (int rx = -1; rx <= 1; rx++)
    {
        for (int ry = -1; ry <= 1; ry++)
        {
            if (rx == 0 && ry == 0)
                continue;
            uint r = pmap[cellIndex(x + rx, y + ry)];
            int rid = pmapId(r);
            uint rt = rid < 0 ? PT_NONE : ptype[rid];
            if (rt == PT_SALT)
            {
                // SLTW + SALT -> 2xSLTW
                if (rngChance(1, 2000))
                    partChangeType(uint(rid), PT_SLTW);
            }
            else if (rt == PT_PLNT)
            {
                if (rngChance(1, 40))
                    killPart(uint(rid), x + rx, y + ry);
            }
            else if (rt == PT_RBDM || rt == PT_LRBD)
            {
                // SLTW + RBDM/LRBD -> FIRE + RBDM/LRBD
                if ((params[0].legacyHeat != 0 || p.temp > (273.15 + 12.0)) && rngChance(1, 100))
                {
                    partChangeType(i, PT_FIRE);
                    p.life = 4;
                    p.ctype = int(PT_WATR);
                }
            }
            else if (rt == PT_FIRE)
            {
                if (parts[rid].ctype != int(PT_WATR))
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
    }
    return 0;
}

// WTRV.cpp
int updateWTRV(uint i, inout Particle p, int x, int y)
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
            if ((rt == PT_RBDM || rt == PT_LRBD) && params[0].legacyHeat == 0 && p.temp > (273.15 + 12.0) &&
                rngChance(1, 100))
            {
                // WTRV + RBDM/LRBD -> FIRE + RBDM/LRBD
                partChangeType(i, PT_FIRE);
                p.life = 4;
                p.ctype = int(PT_WATR);
            }
        }
    }
    if (p.temp > 1273.0 && p.ctype == int(PT_FIRE))
        p.temp -= p.temp / 1000.0;
    return 0;
}

// ICEI.cpp
int updateICEI(uint i, inout Particle p, int x, int y)
{
    if (p.ctype == int(PT_FRZW)) // get colder if it is from FRZW
        p.temp = restrictFlt(p.temp - 1.0, MIN_TEMP, MAX_TEMP);
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
            if (rt == PT_SALT || rt == PT_SLTW)
            {
                if (p.temp > elems[PT_SLTW].lowTemperature && rngChance(1, 200))
                {
                    // ICEI + SALT/SLTW -> 2xSLTW
                    partChangeType(i, PT_SLTW);
                    partChangeType(uint(rid), PT_SLTW);
                    return 0;
                }
            }
            else if (rt == PT_FRZZ && rngChance(1, 200))
            {
                // ICEI + FRZZ -> ICEI + ICEI(FRZW)
                partChangeType(uint(rid), PT_ICEI);
                parts[rid].ctype = int(PT_FRZW);
            }
        }
    }
    return 0;
}

// SNOW.cpp
int updateSNOW(uint i, inout Particle p, int x, int y)
{
    if (p.ctype == int(PT_FRZW)) // get colder if it is from FRZW
        p.temp = restrictFlt(p.temp - 1.0, MIN_TEMP, MAX_TEMP);
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
            // SNOW + SALT/SLTW -> 2xSLTW
            if ((rt == PT_SALT || rt == PT_SLTW) && rngChance(1, 333))
            {
                partChangeType(i, PT_SLTW);
                partChangeType(uint(rid), PT_SLTW);
            }
        }
    }
    return 0;
}

// RIME.cpp
int updateRIME(uint i, inout Particle p, int x, int y)
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
            if (rt == PT_SPRK)
            {
                // RIME -> FOG
                partChangeType(i, PT_FOG);
                p.life = rngBetween(60, 119);
            }
            else if (rt == PT_GAS && p.tmp < 10)
            {
                // GAS increases acidity
                killPart(uint(rid), x + rx, y + ry);
                if (p.ctype == int(PT_DSTW))
                    p.ctype = 0;
                else
                    p.tmp++;
            }
            else if (rt == PT_FOG && parts[rid].life > 0)
            {
                // RIME + FOG -> 2xFOG
                partChangeType(i, PT_FOG);
                p.life = parts[rid].life;
            }
        }
    }
    return 0;
}

// FOG.cpp
int updateFOG(uint i, inout Particle p, int x, int y)
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
            if ((elems[rt].properties & TYPE_SOLID) != 0 && rngChance(1, 10) && p.life == 0 &&
                !(rt == PT_CLNE || rt == PT_PCLN || rt == PT_BCLN || rt == PT_PBCN))
            {
                // FOG -> RIME
                partChangeType(i, PT_RIME);
            }
            if (rt == PT_SPRK)
                p.life += rngBetween(0, 19);
            // GAS increases acidity
            if (rt == PT_GAS && p.tmp < 10)
            {
                killPart(uint(rid), x + rx, y + ry);
                if (p.ctype == int(PT_DSTW))
                    p.ctype = 0;
                else
                    p.tmp++;
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
    case PT_DSTW:
        return updateDSTW(i, p, x, y);
    case PT_SLTW:
        return updateSLTW(i, p, x, y);
    case PT_WTRV:
        return updateWTRV(i, p, x, y);
    case PT_ICEI:
        return updateICEI(i, p, x, y);
    case PT_SNOW:
        return updateSNOW(i, p, x, y);
    case PT_RIME:
        return updateRIME(i, p, x, y);
    case PT_FOG:
        return updateFOG(i, p, x, y);
    case PT_FIRE:
        return updateFIRE(i, p, t, x, y, surroundSpace);
    default:
        return 0;
    }
}

// Element Create functions (TPT's create_part calls them after setting DefaultProperties).
void runElementCreate(uint t, inout Particle p)
{
    switch (t)
    {
    case PT_FIRE:
        p.life = rngBetween(120, 169);
        break;
    case PT_CFLM:
        p.life = rngBetween(50, 199);
        break;
    case PT_ROCK:
        p.tmp2 = rngBetween(0, 10);
        break;
    default:
        break;
    }
}
