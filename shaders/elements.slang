// Element update functions, ported from TPT's src/simulation/elements/*.cpp.
// Reactions that would create an element the GPU engine doesn't have yet are skipped (gpuHas).
#pragma once
#include "common.slang"
#include "move.slang"

void partChangeType(uint id, uint t)
{
    ptype[id] = t;
}

// PLNT.h: directions in counter-clockwise order starting from the top (107 / 2.6 / 345).
static const int2 DIR3X3[8] = { int2(0, -1), int2(-1, -1), int2(-1, 0), int2(-1, 1),
                                int2(0, 1),  int2(1, 1),   int2(1, 0),  int2(1, -1) };
// Positions of info in PLNT's and SEED's ctype field.
static const int PLNT_PHASE = 1;
static const int PLNT_DIR = 3;
static const int PLNT_COLOUR = 6;
static const int PLNT_LIFE = 12;
static const int PLNT_TOTAL_TMP = 15;
// Inverse of DIR3X3, indexed [gx * 3 + gy] with gx, gy = sign + 1. 8 means no direction.
static const int PLNT_INV_DIR[9] = { 1, 2, 3, 0, 8, 4, 7, 6, 5 };

// Element_PLNT_detectDown: the direction gravity pulls in (8 when there is none).
int plntDetectDown(int x, int y)
{
    float gx, gy;
    gravityField(x, y, 1.0, 1.0, gx, gy);
    float len = sqrt(gx * gx + gy * gy);
    if (len > 0.0)
    {
        gx /= len;
        gy /= len;
    }
    int ix = 1, iy = 1;
    if (gx > 0.5)
        ix = 2;
    if (gx < -0.5)
        ix = 0;
    if (gy > 0.5)
        iy = 2;
    if (gy < -0.5)
        iy = 0;
    return PLNT_INV_DIR[ix * 3 + iy];
}

// Element Create functions (TPT's create_part calls them after setting DefaultProperties).
void runElementCreate(uint t, inout Particle p, int x, int y)
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
    case PT_LAVA:
        p.life = rngBetween(240, 359);
        break;
    case PT_GLAS:
        p.tmp3 = int(airPV(airIndex(x, y)) * 64.0);
        break;
    case PT_CLST:
        p.tmp = rngBetween(0, 6);
        break;
    case PT_CRMC:
        p.tmp2 = rngBetween(0, 4);
        break;
    case PT_SEED:
    {
        p.ctype = 0xEC0; // 0b111011'000'00'0: colour genes, no direction, phase 0
        int down = plntDetectDown(x, y);
        if (down < 0 || down > 7)
            down = rngBetween(0, 7); // random if gravity can't decide
        int up = (down + 4) % 8;
        p.ctype |= (up & 7) << PLNT_DIR;
        p.tmp = 0;
        p.tmp2 = 0;
        p.tmp3 = 0;
        p.tmp4 = 0;
        for (int ind = 0; ind < PLNT_TOTAL_TMP; ind++)
        {
            if (rngChance(1, 2))
                p.tmp |= 1 << ind;
            if (rngChance(1, 2))
                p.tmp2 |= 1 << ind;
            if (rngChance(1, 2))
                p.tmp3 |= 1 << ind;
            if (rngChance(1, 2))
                p.tmp4 |= 1 << ind;
        }
        break;
    }
    default:
        break;
    }
}

// ---- create_part ----

// The particle create_part makes: the element's DefaultProperties, then its Create function.
Particle newParticle(uint t, int x, int y, uint pass)
{
    Particle np = elemDefaults[t];
    np.x = float(x);
    np.y = float(y);
    np.pass = pass;
    np.pad1 = 0;
    np.pad2 = 0;
    runElementCreate(t, np, x, y);
    return np;
}

// Parts::Alloc. The colour passes take ids from the free list prepare built. Serial mode follows TPT's
// order instead: ids freed this tick, last first, then the lowest unused id.
int allocPart()
{
    if ((pc.mode & 2) != 0)
    {
        uint n = counters[CTR_FREE_COUNT];
        while (n > 0)
        {
            n--;
            uint id = freelist[n];
            if (ptype[id] == PT_NONE)
            {
                counters[CTR_FREE_COUNT] = n;
                return int(id);
            }
        }
        counters[CTR_FREE_COUNT] = 0;
        uint c = counters[CTR_FREE_TAKEN];
        while (c < pc.capacity && ptype[c] != PT_NONE)
            c++;
        counters[CTR_FREE_TAKEN] = min(c + 1, pc.capacity);
        return c < pc.capacity ? int(c) : -1;
    }
    uint slot;
    InterlockedAdd(counters[CTR_FREE_TAKEN], 1u, slot);
    if (slot >= counters[CTR_FREE_COUNT])
        return -1;
    return int(freelist[slot]);
}

// Puts particle id into pmap at (x, y), taking it out of (ox, oy) if it was there.
void placeInPmap(uint id, int ox, int oy, int x, int y)
{
    uint me = pmapEnc(id);
    uint orig;
    if ((ox != x || oy != y) && inSim(ox, oy))
        InterlockedCompareExchange(pmap[cellIndex(ox, oy)], me, 0u, orig);
    uint c = cellIndex(x, y);
    uint e = pmap[c];
    if ((e & PMAP_ID_MASK) != me && (e & PMAP_LOCK) == 0)
        InterlockedCompareExchange(pmap[c], e, me, orig);
}

// create_part(-1, x, y, t): a new particle, if the cell is empty (or the new particle may share it, which
// puts it on top) and walls allow it. Returns its id, or -1. The colour passes don't update it until the
// next tick; serial mode updates it later this tick when its id is higher, as TPT does.
int createPart(int x, int y, uint t)
{
    if (!inSim(x, y) || t == PT_NONE || !gpuHas(t))
        return -1;
    uint c = cellIndex(x, y);
    uint e = pmap[c];
    if ((e & PMAP_LOCK) != 0)
        return -1; // a mover is claiming the cell right now
    uint rt = pmapType(e);
    if (rt != PT_NONE ? evalMoveType(t, x, y, rt) != 2
                      : (bmapAt(x, y) != 0 && evalMoveType(t, x, y, PT_NONE) == 0))
        return -1;
    int id = allocPart();
    if (id < 0)
        return -1;
    parts[id] = newParticle(t, x, y, 0xFFFFFFFFu);
    ptype[id] = t;
    uint orig;
    InterlockedCompareExchange(pmap[c], e, pmapEnc(uint(id)), orig);
    if (orig != e)
    {
        ptype[id] = PT_NONE; // a mover took the cell; the id is free again next tick
        return -1;
    }
    return id;
}

// create_part(id, x, y, t) on another particle: it becomes a fresh particle of type t in place. It keeps
// its place in this tick's passes, so it is still updated this tick if its pass hasn't run yet.
bool createPartAt(uint id, int x, int y, uint t)
{
    if (!inSim(x, y) || t == PT_NONE || !gpuHas(t))
        return false;
    Particle old = parts[id];
    parts[id] = newParticle(t, x, y, old.pass);
    ptype[id] = t;
    placeInPmap(id, int(old.x + 0.5), int(old.y + 0.5), x, y);
    return true;
}

// create_part(i, x, y, t) on the particle being updated, whose state is in p.
bool createSelf(uint i, inout Particle p, int x, int y, uint t)
{
    if (!inSim(x, y) || t == PT_NONE || !gpuHas(t))
        return false;
    int ox = int(p.x + 0.5), oy = int(p.y + 0.5);
    p = newParticle(t, x, y, p.pass);
    parts[i].x = p.x;
    parts[i].y = p.y;
    ptype[i] = t;
    placeInPmap(i, ox, oy, x, y);
    return true;
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

// Sets LAVA's ctype to c if the GPU engine has that element (otherwise the lava couldn't solidify).
void setLavaCtype(inout Particle p, int c)
{
    if (gpuHas(uint(c)))
        p.ctype = c;
}

// FIRE.cpp (Element_FIRE_update), shared by FIRE and LAVA (PLSM later): burning spreads to flammable
// neighbours within 2 px, and lava reacts with what it touches.
int updateFIRE(uint i, inout Particle p, uint t, int x, int y, int surroundSpace)
{
    if (t == PT_FIRE && p.life <= 1)
    {
        if ((p.tmp & 0x3) == 3)
        {
            // FIRE + O2 + H2 -> WTRV(FIRE)
            partChangeType(i, PT_WTRV);
            p.life = 0;
            p.ctype = int(PT_FIRE);
        }
        else if (p.temp < 625.0)
        {
            // FIRE -> SMKE
            partChangeType(i, PT_SMKE);
            p.life = rngBetween(250, 269);
        }
    }
    else if (t == PT_LAVA)
    {
        float pres = airPV(airIndex(x, y));
        if (p.ctype == int(PT_ROCK))
        {
            if (pres <= -9.0)
            {
                // LAVA(ROCK) -> LAVA(STNE)
                p.ctype = int(PT_STNE);
            }
            else if (pres >= 25.0 && rngChance(1, 12500))
            {
                if (pres <= 50.0)
                {
                    // LAVA(ROCK) -> LAVA(BRMT/CNCT)
                    setLavaCtype(p, rngChance(1, 2) ? int(PT_BRMT) : int(PT_CNCT));
                }
                else if (pres <= 75.0)
                {
                    // LAVA(ROCK) -> LAVA(GOLD/QRTZ)
                    setLavaCtype(p, (pres >= 73.0 || rngChance(1, 8)) ? int(PT_GOLD) : int(PT_QRTZ));
                }
                else if (pres <= 100.0 && p.temp >= 5000.0)
                {
                    // LAVA(ROCK) -> LAVA(TTAN/IRON)
                    setLavaCtype(p, rngChance(1, 5) ? int(PT_TTAN) : int(PT_IRON));
                }
                else if (p.temp >= 5000.0 && rngChance(1, 5))
                {
                    // LAVA(ROCK) -> LAVA(URAN/PLUT/TUNG)
                    if (rngChance(1, 5))
                        setLavaCtype(p, int(PT_URAN));
                    else if (rngChance(1, 5))
                        setLavaCtype(p, int(PT_PLUT));
                    else
                        setLavaCtype(p, int(PT_TUNG));
                }
            }
        }
        else if (p.ctype == int(PT_GOLD) && pres < -200.0 && p.temp > elems[PT_PTNM].highTemperature &&
                 rngChance(1, 20000))
        {
            // LAVA(GOLD) -> LAVA(PTNM)
            if (gpuHas(PT_PTNM))
            {
                p.ctype = int(PT_PTNM);
                uint ci = airIndex(x, y);
                setAirPV(ci, airPV(ci) + 2.0);
            }
        }
        else if ((p.ctype == int(PT_STNE) || p.ctype == 0) && pres >= 30.0 &&
                 (p.temp > elems[PT_ROCK].highTemperature || pres < elems[PT_ROCK].highPressure))
        {
            // LAVA -> LAVA(ROCK): rock forms under pressure, if it will stay molten or not break at once
            p.tmp2 = rngBetween(0, 10); // colour noise
            p.ctype = int(PT_ROCK);
        }
    }

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

            // THRM burning
            if (rt == PT_THRM && (t == PT_FIRE || t == PT_PLSM || t == PT_LAVA))
            {
                // FIRE/PLSM/LAVA + THRM -> FIRE/PLSM/LAVA + LAVA(BMTL/THRM)
                partChangeType(uint(rid), PT_LAVA);
                parts[rid].temp = 3500.0;
                if (rngChance(1, 500))
                {
                    parts[rid].ctype = int(PT_BMTL);
                    uint rci = airIndex(x + rx, y + ry);
                    setAirPV(rci, airPV(rci) + 50.0);
                }
                else
                {
                    parts[rid].life = 400;
                    parts[rid].ctype = int(PT_THRM);
                    parts[rid].tmp = 20;
                }
                continue;
            }

            if (rt == PT_COAL || rt == PT_BCOL)
            {
                if (t == PT_FIRE || t == PT_PLSM)
                {
                    if (parts[rid].life > 100 && rngChance(1, 500))
                        parts[rid].life = 99; // set it smouldering
                }
                else if (t == PT_LAVA)
                {
                    if (p.ctype == int(PT_IRON) && rngChance(1, 500))
                    {
                        // LAVA(IRON) + COAL/BCOL -> LAVA(METL)
                        p.ctype = int(PT_METL);
                        killPart(uint(rid), x + rx, y + ry);
                        continue;
                    }
                    if ((p.ctype == int(PT_STNE) || p.ctype == int(PT_NONE)) && rngChance(1, 60) && gpuHas(PT_SLCN))
                    {
                        // LAVA + COAL/BCOL -> LAVA(SLCN)
                        p.ctype = int(PT_SLCN);
                        killPart(uint(rid), x + rx, y + ry);
                        continue;
                    }
                }
            }

            if (t == PT_LAVA)
            {
                Particle q = parts[rid];
                if (p.ctype == int(PT_QRTZ) && rt == PT_LAVA && q.ctype == int(PT_CLST))
                {
                    // LAVA(QRTZ) + LAVA(CLST) -> 2xLAVA(CRMC)
                    float pres = max(airPV(airIndex(x, y)) * 10.0, 0.0);
                    if (p.temp >= pres + elems[PT_CRMC].highTemperature + 50.0)
                    {
                        p.ctype = int(PT_CRMC);
                        parts[rid].ctype = int(PT_CRMC);
                    }
                }
                else if (rt == PT_O2 && p.ctype == int(PT_SLCN))
                {
                    // LAVA(SLCN) + O2 -> LAVA(SAND/CLST/PQRT/STNE)
                    int c = int(PT_STNE);
                    switch (rngBetween(0, 2))
                    {
                    case 0:
                        c = int(PT_SAND);
                        break;
                    case 1:
                        c = int(PT_CLST);
                        // avoid creating CRMC
                        if (p.temp >= elems[PT_PQRT].highTemperature * 3.0)
                            c = int(PT_PQRT);
                        break;
                    default:
                        break;
                    }
                    if (gpuHas(uint(c)))
                    {
                        p.ctype = c;
                        p.tmp = 0;
                        killPart(uint(rid), x + rx, y + ry);
                        continue;
                    }
                }
                else if (rt == PT_LAVA && (q.ctype == int(PT_METL) || q.ctype == int(PT_BMTL)) &&
                         p.ctype == int(PT_SLCN))
                {
                    // LAVA(SLCN) + LAVA(METL/BMTL) -> LAVA(NSCN) + LAVA(PSCN)
                    if (gpuHas(PT_NSCN) && gpuHas(PT_PSCN))
                    {
                        p.tmp = 0;
                        p.ctype = int(PT_NSCN);
                        parts[rid].ctype = int(PT_PSCN);
                    }
                }
                else if (p.ctype == int(PT_SLCN) && rt == PT_LAVA && q.ctype == int(PT_SALT))
                {
                    // LAVA(SLCN) + LAVA(SALT) -> LAVA(LITH)
                    if (p.temp > elems[PT_LITH].highTemperature && rngChance(1, 1000) && gpuHas(PT_LITH))
                    {
                        p.ctype = int(PT_LITH);
                        p.tmp = 0;
                        p.tmp2 = 0;
                        p.life = 0;
                        killPart(uint(rid), x + rx, y + ry);
                        continue;
                    }
                }
                else if (rt == PT_HEAC && p.ctype == int(PT_HEAC))
                {
                    if (q.temp > elems[PT_HEAC].highTemperature)
                    {
                        partChangeType(uint(rid), PT_LAVA);
                        parts[rid].ctype = int(PT_HEAC);
                    }
                }
                else if (p.ctype == int(PT_ROCK) && rt == PT_LAVA && q.ctype == int(PT_GOLD) && q.tmp == 0 &&
                         airPV(airIndex(x, y)) >= 50.0 && rngChance(1, 10000))
                {
                    // LAVA(ROCK) + LAVA(GOLD) -> 2xLAVA(GOLD): gold veins, trending vertical
                    p.ctype = int(PT_GOLD);
                    if (rx > 1 || rx < -1)
                        p.tmp = 1;
                }
                else if (p.ctype == int(PT_SALT) && rt == PT_GLAS && q.life < 234 * 120)
                {
                    parts[rid].life++;
                }
            }

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

// SEED.cpp
int updateSEED(uint i, inout Particle p, int x, int y)
{
    float temp = p.temp;
    // Turn into liquid wax under high temperature and pressure
    if (temp > 320.0 && airPV(airIndex(x, y)) > 50.0 && rngChance(1, 150))
    {
        // SEED -> MWAX
        createSelf(i, p, x, y, PT_MWAX);
        p.ctype = 0;
        p.life = 0;
        p.tmp = 0;
        p.tmp2 = 0;
        p.tmp3 = 0;
        p.tmp4 = 0;
        return 0;
    }

    // Don't do anything at bad temperatures
    if (temp > 343.15 || temp < 278.15)
    {
        p.ctype &= ~(0xFF << PLNT_LIFE); // set water to zero
        p.life = 0;
        return 0;
    }

    int water = (p.ctype >> PLNT_LIFE) & 0xFF;

    // Check if we can grow
    if (water > 3)
    {
        int down = plntDetectDown(x, y);
        if (down < 0 || down > 7)
            down = 4; // default direction if gravity can't decide
        int up = (down + 4) % 8;

        // SAND/SPNG under the seed and empty space above
        uint downT = typeAt(x + DIR3X3[down].x, y + DIR3X3[down].y);
        if ((downT == PT_SAND || downT == PT_SPNG) && typeAt(x + DIR3X3[up].x, y + DIR3X3[up].y) == PT_NONE)
        {
            if (p.life > 200)
            {
                // Start growth: SEED -> PLNT
                partChangeType(i, PT_PLNT);
                p.ctype &= ~((7 << PLNT_DIR) | (3 << PLNT_PHASE)); // clear direction and phase
                p.ctype |= ((up & 7) << PLNT_DIR) | 1;             // initial growth direction, growth bit
                p.life = 15 * water;                                // make the first branch longer
                return 1;
            }
            else
                p.life++;
        }
        else
            p.life = 0; // lost contact with the ground
    }

    for (int rx = -1; rx <= 1; rx++)
    {
        for (int ry = -1; ry <= 1; ry++)
        {
            if (rx == 0 && ry == 0)
                continue;
            int rid = pmapId(pmap[cellIndex(x + rx, y + ry)]);
            if (rid < 0)
                continue;
            uint rt = ptype[rid];
            if (rt == PT_WATR)
            {
                // Drink water
                if (water < 31)
                {
                    killPart(uint(rid), x + rx, y + ry);
                    water++;
                    p.life = 0;
                }
            }
            else if (rt == PT_DEUT)
            {
                // Heavy water allows extra growth
                if (water < 255)
                {
                    killPart(uint(rid), x + rx, y + ry);
                    water++;
                    p.life = 0;
                }
            }
            else if (rt == PT_CBNW || rt == PT_DSTW)
            {
                // Distilled or carbonated water only in very limited amounts
                if (water < 3)
                {
                    killPart(uint(rid), x + rx, y + ry);
                    water++;
                    p.life = 0;
                }
            }
            else if (rt == PT_SLTW)
            {
                // Salty water is harmful
                if (water > 0)
                {
                    killPart(uint(rid), x + rx, y + ry);
                    water--;
                    p.life = 0;
                }
            }
            else if (rt == PT_SPNG)
            {
                // Absorb some water from SPNG
                if (water < 15 && parts[rid].life > 0)
                {
                    parts[rid].life--;
                    water++;
                    p.life = 0;
                }
            }
            else if (rt == PT_SEED)
            {
                // Breed with another seed
                Particle q = parts[rid];
                if (water > 0 && (p.ctype & 1) == 0 && (q.ctype & 1) == 0 && rngChance(1, 10))
                {
                    int ac = p.ctype;
                    int bc = q.ctype;
                    int oldI[4] = { p.tmp, p.tmp2, p.tmp3, p.tmp4 };
                    int oldR[4] = { q.tmp, q.tmp2, q.tmp3, q.tmp4 };
                    // TPT ORs the inherited bits into the fields' old values.
                    int newI[4] = { p.tmp, p.tmp2, p.tmp3, p.tmp4 };
                    int newR[4] = { q.tmp, q.tmp2, q.tmp3, q.tmp4 };

                    // Breeding bits set, so these seeds no longer breed with others
                    int ci = 1;
                    int cr = 1;

                    // Inherit colour
                    for (int ind = 0; ind < 3; ind++)
                    {
                        int bits[4]; // [kid * 2 + parent]
                        for (int kid = 0; kid < 2; kid++)
                        {
                            if (rngChance(1, 2))
                                bits[kid * 2] = (ac & (1 << (2 * ind + PLNT_COLOUR))) != 0 ? 1 : 0;
                            else
                                bits[kid * 2] = (ac & (1 << (1 + 2 * ind + PLNT_COLOUR))) != 0 ? 1 : 0;
                            if (rngChance(1, 2))
                                bits[kid * 2 + 1] = (bc & (1 << (2 * ind + PLNT_COLOUR))) != 0 ? 1 : 0;
                            else
                                bits[kid * 2 + 1] = (bc & (1 << (1 + 2 * ind + PLNT_COLOUR))) != 0 ? 1 : 0;
                        }
                        ci |= bits[0] << (2 * ind + PLNT_COLOUR);
                        ci |= bits[1] << (1 + 2 * ind + PLNT_COLOUR);
                        cr |= bits[2] << (2 * ind + PLNT_COLOUR);
                        cr |= bits[3] << (1 + 2 * ind + PLNT_COLOUR);
                    }

                    // Inherit the branching part of tmp - tmp4
                    for (int ind = 0; ind < 5; ind++)
                    {
                        for (int field = 0; field < 4; field++)
                        {
                            newI[field] |= rngChance(1, 2) ? (oldI[field] & (1 << ind)) : (oldR[field] & (1 << ind));
                            newR[field] |= rngChance(1, 2) ? (oldI[field] & (1 << ind)) : (oldR[field] & (1 << ind));
                        }
                    }

                    // Inherit the phases part of tmp - tmp4
                    for (int ind = 5; ind < PLNT_TOTAL_TMP; ind += 2)
                    {
                        for (int field = 0; field < 4; field++)
                        {
                            newI[field] |= rngChance(1, 2) ? (oldI[field] & (3 << ind)) : (oldR[field] & (3 << ind));
                            newR[field] |= rngChance(1, 2) ? (oldI[field] & (3 << ind)) : (oldR[field] & (3 << ind));
                        }
                    }

                    p.ctype = ci;
                    p.tmp = newI[0];
                    p.tmp2 = newI[1];
                    p.tmp3 = newI[2];
                    p.tmp4 = newI[3];
                    parts[rid].ctype = cr;
                    parts[rid].tmp = newR[0];
                    parts[rid].tmp2 = newR[1];
                    parts[rid].tmp3 = newR[2];
                    parts[rid].tmp4 = newR[3];
                }
            }
        }
    }

    // Update water counter
    p.ctype &= ~(0xFF << PLNT_LIFE);
    p.ctype |= (water & 0xFF) << PLNT_LIFE;
    return 0;
}

// PLNT.cpp. Tree growth (ctype bit 0) is a small program in ctype and tmp - tmp4: see the field notes there.
int updatePLNT(uint i, inout Particle p, int x, int y, int surroundSpace)
{
    if ((p.ctype & 1) != 0)
    {
        float temp = p.temp;

        // Hot and cold temperature kills tree growth
        if (temp > 373.15 || temp < 223.15)
        {
            p.ctype = 0;
            p.tmp = 0;
            p.tmp2 = 0;
            p.tmp3 = 0;
            p.tmp4 = 0;
            return 0;
        }

        // No growth happens at bad temperatures
        if (temp > 343.15 || temp < 278.15)
            return 0;

        // Slow things down
        if (rngChance(9, 10))
            return 0;

        int phase = (p.ctype >> PLNT_PHASE) & 0x3;
        int dir = (p.ctype >> PLNT_DIR) & 0x7;
        int life = (p.ctype >> PLNT_LIFE) & 0xFF;
        int prog = phase == 0 ? p.tmp : phase == 1 ? p.tmp2 : phase == 2 ? p.tmp3 : p.tmp4;
        int colour = p.ctype & (0x3f << PLNT_COLOUR);

        bool stopped = false;
        int down = plntDetectDown(x, y); // 8 means no direction

        if (life > 0)
        {
            if (p.life == 0)
            {
                // Branching
                int bmask = prog & 0x1F;
                int phases = prog >> 5;
                if (bmask != 0)
                {
                    for (int phi = 0; phi < 5; phi++)
                    {
                        if (((bmask >> phi) & 0x1) != 0)
                        {
                            int ndir = (dir + phi + 6) % 8; // phi = 0 is -90 deg, phi = 1 is -45 deg etc.
                            int nphase = phases & 0x3;
                            int nlife = (2 * life) / 3;

                            // Discourage branches which go down
                            if (down < 8)
                            {
                                if (ndir == down)
                                    nlife = life / 3;
                                if (ndir == ((down + 1) % 8) || ndir == ((down + 7) % 8))
                                    nlife = life / 3;
                                if (life > 4 && (ndir == ((down + 2) % 8) || ndir == ((down + 6) % 8)))
                                    nlife = life / 2;
                            }

                            int si = createPart(x + DIR3X3[ndir].x, y + DIR3X3[ndir].y, PT_PLNT);
                            if (si >= 0)
                            {
                                parts[si].ctype = ((nlife & 0xFF) << PLNT_LIFE) | colour | ((ndir & 7) << PLNT_DIR) |
                                                  ((nphase & 3) << PLNT_PHASE) | 0x1;
                                // Diagonal branches are shorter by 1/sqrt(2) ~= 0.7
                                parts[si].life = (ndir % 2 == 1) ? 7 * nlife : 10 * nlife;
                                parts[si].tmp = p.tmp;
                                parts[si].tmp2 = p.tmp2;
                                parts[si].tmp3 = p.tmp3;
                                parts[si].tmp4 = p.tmp4;
                            }
                        }
                        phases = phases >> 2;
                    }
                }
                else
                    stopped = true;
            }
            else
            {
                // Grow forward
                int si = createPart(x + DIR3X3[dir].x, y + DIR3X3[dir].y, PT_PLNT);
                if (si >= 0)
                {
                    parts[si].ctype = ((life & 0xFF) << PLNT_LIFE) | colour | ((dir & 7) << PLNT_DIR) |
                                      ((phase & 3) << PLNT_PHASE) | 0x1;
                    parts[si].life = p.life;
                    parts[si].tmp = p.tmp;
                    parts[si].tmp2 = p.tmp2;
                    parts[si].tmp3 = p.tmp3;
                    parts[si].tmp4 = p.tmp4;
                }
            }

            if (!stopped)
            {
                // Create a stem: PLNT -> WOOD, or a thick stem PLNT -> GOO with WOOD either side
                bool thick = !(phase != 0 || life < 12);
                createSelf(i, p, x, y, thick ? PT_GOO : PT_WOOD);
                p.ctype = 0;
                p.life = 0;
                p.tmp = 0;
                p.tmp2 = 0;
                p.tmp3 = 0;
                p.tmp4 = 0;
                if (thick)
                {
                    int leftDir = (dir + 6) % 8;
                    int rightDir = (dir + 2) % 8;
                    if (dir % 2 != 0)
                    {
                        // Correction for diagonal directions
                        leftDir = (dir + 5) % 8;
                        rightDir = (dir + 3) % 8;
                    }
                    createPart(x + DIR3X3[leftDir].x, y + DIR3X3[leftDir].y, PT_WOOD);
                    createPart(x + DIR3X3[rightDir].x, y + DIR3X3[rightDir].y, PT_WOOD);
                }
            }
        }
        else
            stopped = true;

        if (stopped)
        {
            // Shoot out a seed: PLNT -> PLNT + SEED
            if (rngChance(1, 10))
            {
                int si = createPart(x + 2 * DIR3X3[dir].x, y + 2 * DIR3X3[dir].y, PT_SEED);
                if (si >= 0)
                {
                    parts[si].vx = float(DIR3X3[dir].x);
                    parts[si].vy = float(DIR3X3[dir].y);
                    // Inherit the genome, keeping only the colour from ctype
                    parts[si].ctype = colour;
                    parts[si].tmp = p.tmp;
                    parts[si].tmp2 = p.tmp2;
                    parts[si].tmp3 = p.tmp3;
                    parts[si].tmp4 = p.tmp4;
                }
            }

            // Turn into a usual plant, keeping the colour (direction 7 tells graphics to colour it)
            p.ctype = colour | (7 << PLNT_DIR);
            p.life = 0;
            p.tmp = 0;
            p.tmp2 = 0;
            p.tmp3 = 0;
            p.tmp4 = 0;
        }
        return 1;
    }

    // Usual PLNT
    for (int rx = -1; rx <= 1; rx++)
    {
        for (int ry = -1; ry <= 1; ry++)
        {
            if (rx == 0 && ry == 0)
                continue;
            int rid = pmapId(pmap[cellIndex(x + rx, y + ry)]);
            if (rid < 0)
                continue;
            uint rt = ptype[rid];
            if (rt == PT_WATR)
            {
                if (rngChance(1, 50))
                {
                    // PLNT + WATR -> 2xPLNT
                    if (createPartAt(uint(rid), x + rx, y + ry, PT_PLNT))
                    {
                        parts[rid].life = 0;
                        parts[rid].ctype = p.ctype; // keep the colour identical
                    }
                }
            }
            else if (rt == PT_LAVA)
            {
                if (rngChance(1, 50))
                {
                    // PLNT + LAVA -> FIRE + LAVA
                    partChangeType(i, PT_FIRE);
                    p.life = 4;
                }
            }
            else if (rt == PT_SMKE || rt == PT_CO2)
            {
                if (rngChance(1, 50))
                {
                    killPart(uint(rid), x + rx, y + ry);
                    p.life = rngBetween(60, 119);
                }
            }
            else if (rt == PT_WOOD)
            {
                // Grow VINE along WOOD (plants that were VINE have tmp 1)
                uint rnd = rngGen();
                if (surroundSpace != 0 && (rnd % 4) == 0 && p.tmp == 1)
                {
                    rnd >>= 3;
                    int nnx = int(rnd % 3) - 1;
                    rnd >>= 2;
                    int nny = int(rnd % 3) - 1;
                    if ((nnx != 0 || nny != 0) && typeAt(x + rx + nnx, y + ry + nny) == PT_NONE)
                    {
                        int np = createPart(x + rx + nnx, y + ry + nny, PT_VINE);
                        if (np >= 0)
                            parts[np].temp = p.temp;
                    }
                }
            }
        }
    }
    if (p.life == 2)
    {
        // PLNT + SMKE/CO2 -> PLNT + O2
        for (int rx = -1; rx <= 1; rx++)
        {
            for (int ry = -1; ry <= 1; ry++)
            {
                if ((rx != 0 || ry != 0) && typeAt(x + rx, y + ry) == PT_NONE)
                    createPart(x + rx, y + ry, PT_O2);
            }
        }
        p.life = 0;
    }
    if (p.temp > 350.0 && p.temp > float(p.tmp2))
        p.tmp2 = int(p.temp);
    return 0;
}

// VINE.cpp
int updateVINE(uint i, inout Particle p, int x, int y)
{
    uint rnd = rngGen();
    int rx = int(rnd % 3) - 1;
    rnd >>= 2;
    int ry = int(rnd % 3) - 1;
    rnd >>= 2;
    if (rx != 0 || ry != 0)
    {
        if ((rnd % 15) == 0)
        {
            // VINE -> PLNT
            partChangeType(i, PT_PLNT);
        }
        else if (typeAt(x + rx, y + ry) == PT_NONE)
        {
            int np = createPart(x + rx, y + ry, PT_VINE);
            if (np < 0)
                return 0;
            parts[np].temp = p.temp;
            partChangeType(i, PT_PLNT);
        }
    }
    if (p.temp > 350.0 && p.temp > float(p.tmp2))
        p.tmp2 = int(p.temp);
    return 0;
}

// WOOD.cpp
int updateWOOD(uint i, inout Particle p, int x, int y)
{
    if (p.temp > 450.0 && p.temp > float(p.tmp))
        p.tmp = int(p.temp);
    if (p.temp > 773.0 && airPV(airIndex(x, y)) <= -10.0)
    {
        // WOOD -> BCOL
        float temp = p.temp;
        createSelf(i, p, x, y, PT_BCOL);
        p.temp = temp;
    }
    return 0;
}

// GOO.cpp: deforms under pressure, then moves with the air until its life runs out.
int updateGOO(uint i, inout Particle p, int x, int y)
{
    uint ci = airIndex(x, y);
    if (p.life == 0 && airPV(ci) > 1.0)
        p.life = rngBetween(300, 379);
    if (p.life != 0)
    {
        p.vx += 0.1 * airVX(ci);
        p.vy += 0.1 * airVY(ci);
    }
    return 0;
}

// SPNG.cpp
int updateSPNG(uint i, inout Particle p, int x, int y)
{
    const int limit = 50;
    float pv = airPV(airIndex(x, y));
    if (p.life < limit && pv <= 3.0 && pv >= -3.0 && p.temp <= 374.0)
    {
        int absorbChanceDenom = p.life * 10000 / limit + 500;
        for (int rx = -1; rx <= 1; rx++)
        {
            for (int ry = -1; ry <= 1; ry++)
            {
                if (rx == 0 && ry == 0)
                    continue;
                int rid = pmapId(pmap[cellIndex(x + rx, y + ry)]);
                if (rid < 0)
                    continue;
                uint rt = ptype[rid];
                if (rt == PT_WATR || rt == PT_DSTW || rt == PT_FRZW)
                {
                    if (p.life < limit && rngChance(500, absorbChanceDenom))
                    {
                        p.life++;
                        killPart(uint(rid), x + rx, y + ry);
                    }
                }
                else if (rt == PT_SLTW)
                {
                    if (p.life < limit && rngChance(50, absorbChanceDenom))
                    {
                        p.life++;
                        if (rngChance(3, 4))
                            killPart(uint(rid), x + rx, y + ry);
                        else
                            partChangeType(uint(rid), PT_SALT); // SPNG + SLTW -> SPNG + SALT
                    }
                }
                else if (rt == PT_CBNW && gpuHas(PT_CBNW))
                {
                    if (p.life < limit && rngChance(100, absorbChanceDenom))
                    {
                        // SPNG + CBNW -> SPNG + CO2
                        p.life++;
                        partChangeType(uint(rid), PT_CO2);
                    }
                }
                else if (rt == PT_PSTE && gpuHas(PT_CLST))
                {
                    if (p.life < limit && rngChance(20, absorbChanceDenom))
                    {
                        // SPNG + PSTE -> SPNG + CLST
                        p.life++;
                        createPartAt(uint(rid), x + rx, y + ry, PT_CLST);
                    }
                }
            }
        }
    }
    else
    {
        // SPNG -> SPNG + WATR: squeeze water out into empty neighbours
        for (int rx = -1; rx <= 1; rx++)
        {
            for (int ry = -1; ry <= 1; ry++)
            {
                if ((rx != 0 || ry != 0) && typeAt(x + rx, y + ry) == PT_NONE && p.life >= 1)
                {
                    if (createPart(x + rx, y + ry, PT_WATR) > -1)
                        p.life--;
                }
            }
        }
    }

    // Water diffuses to neighbouring SPNG
    for (int trade = 0; trade < 9; trade++)
    {
        int rx = rngBetween(-2, 2);
        int ry = rngBetween(-2, 2);
        if (rx == 0 && ry == 0)
            continue;
        int rid = pmapId(pmap[cellIndex(x + rx, y + ry)]);
        if (rid < 0)
            continue;
        if (ptype[rid] == PT_SPNG && p.life > parts[rid].life && p.life > 0)
        {
            int d = p.life - parts[rid].life;
            if (d == 1)
            {
                parts[rid].life++;
                p.life--;
                trade = 9;
            }
            else if (d > 0)
            {
                parts[rid].life += d / 2;
                p.life -= d / 2;
                trade = 9;
            }
        }
    }

    // Wet sponge damps fire
    int tmp = 0;
    if (p.life > 0)
    {
        for (int rx = -1; rx <= 1; rx++)
        {
            for (int ry = -1; ry <= 1; ry++)
            {
                if (rx == 0 && ry == 0)
                    continue;
                int rid = pmapId(pmap[cellIndex(x + rx, y + ry)]);
                if (rid < 0 || ptype[rid] != PT_FIRE)
                    continue;
                tmp++;
                int fl = parts[rid].life;
                if (fl > 60)
                    parts[rid].life = fl - fl / 60;
                else if (fl > 2)
                    parts[rid].life = fl - 1;
            }
        }
    }
    if (tmp != 0 && p.life > 3)
        p.life -= p.life / 3;
    if (tmp > 1)
        tmp = tmp / 2;
    if (tmp != 0 || p.temp >= 374.0)
    {
        // SPNG -> SPNG + WTRV
        for (int rx = -1; rx <= 1; rx++)
        {
            for (int ry = -1; ry <= 1; ry++)
            {
                if ((rx != 0 || ry != 0) && typeAt(x + rx, y + ry) == PT_NONE && p.life >= 1)
                {
                    int np = createPart(x + rx, y + ry, PT_WTRV);
                    if (np > -1)
                    {
                        parts[np].temp = p.temp;
                        tmp--;
                        p.life--;
                        p.temp = restrictFlt(p.temp - 20.0, MIN_TEMP, MAX_TEMP);
                    }
                }
            }
        }
    }
    if (tmp > 0)
    {
        if (p.life > tmp)
            p.life -= tmp;
        else
            p.life = 0;
    }
    return 0;
}

// O2.cpp
int updateO2(uint i, inout Particle p, int x, int y)
{
    for (int rx = -2; rx <= 2; rx++)
    {
        for (int ry = -2; ry <= 2; ry++)
        {
            if (rx == 0 && ry == 0)
                continue;
            int rid = pmapId(pmap[cellIndex(x + rx, y + ry)]);
            if (rid < 0)
                continue;
            uint rt = ptype[rid];
            if (rt == PT_FIRE)
            {
                parts[rid].temp += float(rngBetween(0, 99));
                if ((parts[rid].tmp & 0x01) != 0)
                    parts[rid].temp = 3473.0;
                parts[rid].tmp |= 2;

                // O2 + FIRE -> 2xFIRE
                createSelf(i, p, x, y, PT_FIRE);
                p.temp += float(rngBetween(0, 99));
                p.tmp |= 2;
            }
            else if (rt == PT_PLSM && (parts[rid].tmp & 4) == 0)
            {
                // O2 + PLSM -> FIRE + PLSM
                createSelf(i, p, x, y, PT_FIRE);
                p.temp += float(rngBetween(0, 99));
                p.tmp |= 2;
            }
        }
    }
    // O2 -> BRMT + NEUT + PHOT + GRVT needs Newtonian gravity (not ported yet), so it can't happen here.
    return 0;
}

// CO2.cpp
int updateCO2(uint i, inout Particle p, int x, int y)
{
    for (int rx = -1; rx <= 1; rx++)
    {
        for (int ry = -1; ry <= 1; ry++)
        {
            if (rx == 0 && ry == 0)
                continue;
            int rid = pmapId(pmap[cellIndex(x + rx, y + ry)]);
            uint rt = rid < 0 ? PT_NONE : ptype[rid];
            if (rt == PT_NONE)
            {
                // ctype 5: this CO2 still holds the water of a BUBW bubble
                if (p.ctype == 5 && rngChance(1, 2000))
                {
                    // CO2 -> WATR
                    if (createPart(x + rx, y + ry, PT_WATR) >= 0)
                        p.ctype = 0;
                }
                continue;
            }
            if (rt == PT_FIRE)
            {
                killPart(uint(rid), x + rx, y + ry);
                if (rngChance(1, 30))
                {
                    killPart(i, x, y);
                    return 1;
                }
            }
            else if ((rt == PT_WATR || rt == PT_DSTW) && gpuHas(PT_CBNW) && rngChance(1, 50))
            {
                partChangeType(uint(rid), PT_CBNW);
                if (p.ctype == 5)
                {
                    // CO2 + WATR/DSTW -> WATR + CBNW
                    createSelf(i, p, x, y, PT_WATR);
                    return 0;
                }
                // CO2 + WATR/DSTW -> CBNW
                killPart(i, x, y);
                return 1;
            }
        }
    }
    // CO2 -> O2 + NEUT (+ ELEC) above 9773 K and 200 pressure waits for NEUT and ELEC.
    return 0;
}

// COAL.cpp (Element_COAL_update, also BCOL): burns slowly once FIRE sets its life below 100.
int updateCOAL(uint i, inout Particle p, uint t, int x, int y)
{
    if (p.life <= 0)
    {
        createSelf(i, p, x, y, PT_FIRE);
        return 1;
    }
    else if (p.life < 100)
    {
        p.life--;
        int fx = x + rngBetween(-1, 1);
        int fy = y + rngBetween(-1, 1);
        createPart(fx, fy, PT_FIRE);
    }
    if (t == PT_COAL)
    {
        float pv = airPV(airIndex(x, y));
        if (pv > 4.3 && p.tmp > 40)
            p.tmp = 39;
        else if (p.tmp < 40 && p.tmp > 0)
            p.tmp--;
        else if (p.tmp <= 0)
        {
            // COAL -> BCOL
            partChangeType(i, PT_BCOL);
            return 1;
        }
    }
    if (p.temp > float(p.tmp2))
        p.tmp2 = int(p.temp);
    return 0;
}

// GLAS.cpp: shatters into BGLA when the pressure changes faster than its strength.
int updateGLAS(uint i, inout Particle p, int x, int y)
{
    uint ci = airIndex(x, y);
    int press = int(airPV(ci) * 64.0);
    int diff = press - p.tmp3;
    // Chemically strengthened glass has a higher life (250 = max, 16 = min).
    int strength = max(p.life / 120 + 16, 16);
    if (diff > strength || diff < -strength)
    {
        // GLAS -> BGLA
        partChangeType(i, PT_BGLA);
    }
    p.tmp3 = press;
    // Liquid nitrogen condenses on chemically strengthened glass
    if (strength > 200 && p.temp < 77.0 && airGet(AIR_HV, ci) < 77.0 && airPV(ci) > 5.0 && rngChance(1, 100))
    {
        // Sample one of the 4 adjacent cells
        int adj = rngBetween(0, 3);
        int rx = (1 - 2 * (adj % 2)) * (1 - adj / 2);
        int ry = (1 - 2 * (adj % 2)) * (adj / 2);
        if (typeAt(x + rx, y + ry) == PT_NONE)
        {
            // GLAS -> GLAS + LNTG
            if (createPart(x + rx, y + ry, PT_LNTG) > -1)
            {
                setAirPV(ci, airPV(ci) - 1.0);
                p.temp += 200.0;
            }
        }
    }
    return 0;
}

// BMTL.cpp: rust (tmp) spreads into METL and IRON, then crumbles into BRMT.
int updateBMTL(uint i, inout Particle p, int x, int y)
{
    if (p.tmp > 1)
    {
        p.tmp--;
        for (int rx = -1; rx <= 1; rx++)
        {
            for (int ry = -1; ry <= 1; ry++)
            {
                if (rx == 0 && ry == 0)
                    continue;
                int rid = pmapId(pmap[cellIndex(x + rx, y + ry)]);
                if (rid < 0)
                    continue;
                uint rt = ptype[rid];
                if ((rt == PT_METL || rt == PT_IRON) && rngChance(1, 100))
                {
                    // BMTL + METL/IRON -> 2xBMTL. (TPT's expression also sets this tmp to 1 when it is <= 7.)
                    partChangeType(uint(rid), PT_BMTL);
                    if (p.tmp <= 7)
                    {
                        p.tmp = 1;
                        parts[rid].tmp = 1;
                    }
                    else
                        parts[rid].tmp = p.tmp - rngBetween(0, 4);
                }
            }
        }
    }
    else if (p.tmp == 1 && rngChance(1, 1000))
    {
        // BMTL -> BRMT
        p.tmp = 0;
        partChangeType(i, PT_BRMT);
    }
    return 0;
}

// BRMT.cpp: hot BRMT and BREC make THRM.
int updateBRMT(uint i, inout Particle p, int x, int y)
{
    if (p.temp > 523.15)
    {
        int tempFactor = int(1000.0 - ((523.15 - p.temp) * 2.0));
        if (tempFactor < 2)
            tempFactor = 2;
        for (int rx = -1; rx <= 1; rx++)
        {
            for (int ry = -1; ry <= 1; ry++)
            {
                if (rx == 0 && ry == 0)
                    continue;
                int rid = pmapId(pmap[cellIndex(x + rx, y + ry)]);
                if (rid < 0 || ptype[rid] != PT_BREC || !rngChance(1, tempFactor))
                    continue;
                if (rngChance(1, 2))
                    createPartAt(uint(rid), x + rx, y + ry, PT_THRM); // BRMT + BREC -> BRMT + THRM
                else
                    createSelf(i, p, x, y, PT_THRM); // BRMT + BREC -> THRM + BREC
            }
        }
    }
    return 0;
}

// IRON.cpp: rusts into BMTL next to salt, water and oxygen.
int updateIRON(uint i, inout Particle p, int x, int y)
{
    if (p.life != 0)
        return 0;
    bool rust = false;
    for (int rx = -1; rx <= 1 && !rust; rx++)
    {
        for (int ry = -1; ry <= 1 && !rust; ry++)
        {
            if (rx == 0 && ry == 0)
                continue;
            uint rt = typeAt(x + rx, y + ry);
            if (rt == PT_SALT)
                rust = rngChance(1, 47);
            else if (rt == PT_SLTW)
                rust = rngChance(1, 67);
            else if (rt == PT_WATR)
                rust = rngChance(1, 1200);
            else if (rt == PT_O2)
                rust = rngChance(1, 250);
            else if (rt == PT_LO2)
                rust = true;
        }
    }
    if (rust)
    {
        // IRON + SALT/SLTW/WATR/O2/LO2 -> BMTL
        partChangeType(i, PT_BMTL);
        p.tmp = rngBetween(20, 29);
    }
    return 0;
}

// GOLD.cpp: turns rusted iron (BMTL with tmp) within 4 px back into IRON. Taking sparks and absorbing
// neutrons wait for SPRK and NEUT.
int updateGOLD(uint i, inout Particle p, int x, int y)
{
    for (int j = 0; j < 8; j++)
    {
        uint rnd = rngGen();
        int rx = int(rnd % 9) - 4;
        rnd >>= 4;
        int ry = int(rnd % 9) - 4;
        if ((rx == 0) != (ry == 0))
        {
            int rid = pmapId(pmap[cellIndex(x + rx, y + ry)]);
            if (rid >= 0 && ptype[rid] == PT_BMTL && parts[rid].tmp != 0)
            {
                // GOLD + BMTL -> GOLD + IRON
                parts[rid].tmp = 0;
                partChangeType(uint(rid), PT_IRON);
            }
        }
    }
    return 0;
}

// TTAN.cpp: a TTAN wall (TTAN with 2 or more TTAN neighbours) blocks air and ambient heat in its cell.
int updateTTAN(uint i, inout Particle p, int x, int y, int nt)
{
    int ttan = 0;
    if (nt <= 2)
        ttan = 2;
    else if (p.tmp != 0)
        ttan = 2;
    else if (nt <= 6)
    {
        for (int rx = -1; rx <= 1; rx++)
        {
            for (int ry = -1; ry <= 1; ry++)
            {
                if ((rx == 0) != (ry == 0) && typeAt(x + rx, y + ry) == PT_TTAN)
                    ttan++;
            }
        }
    }
    if (ttan >= 2)
    {
        // bmap_blockair = 1 and bmap_blockairh = 0x8 for the cell
        uint ci = airIndex(x, y);
        uint cur = walls[ci];
        for (int attempt = 0; attempt < 8; attempt++)
        {
            uint want = (cur & ~0x00FF0000u) | (0x8u << 16) | WALL_BLOCKAIR_BIT;
            if (want == cur)
                break;
            uint orig;
            InterlockedCompareExchange(walls[ci], cur, want, orig);
            if (orig == cur)
                break;
            cur = orig;
        }
    }
    return 0;
}

// CLST.cpp: clay turns to paste with water and clumps together.
int updateCLST(uint i, inout Particle p, int x, int y)
{
    for (int rx = -2; rx <= 2; rx++)
    {
        for (int ry = -2; ry <= 2; ry++)
        {
            if (rx == 0 && ry == 0)
                continue;
            int rid = pmapId(pmap[cellIndex(x + rx, y + ry)]);
            if (rid < 0)
                continue;
            uint rt = ptype[rid];
            if (rt == PT_WATR)
            {
                if (rngChance(1, 1500))
                {
                    // CLST + WATR -> PSTS
                    createSelf(i, p, x, y, PT_PSTS);
                    killPart(uint(rid), x + rx, y + ry);
                }
            }
            else if (rt == PT_NITR)
            {
                // CLST + NITR -> 2xBANG
                if (gpuHas(PT_BANG))
                {
                    createSelf(i, p, x, y, PT_BANG);
                    createPartAt(uint(rid), x + rx, y + ry, PT_BANG);
                }
            }
            else if (rt == PT_CLST)
            {
                float cxy;
                if (p.temp < 195.0)
                    cxy = 0.05;
                else if (p.temp < 295.0)
                    cxy = 0.015;
                else if (p.temp < 350.0)
                    cxy = 0.01;
                else
                    cxy = 0.005;
                p.vx += cxy * float(rx);
                p.vy += cxy * float(ry);
            }
        }
    }
    return 0;
}

// CRMC.cpp: ceramic crumbles back into clay at low pressure.
int updateCRMC(uint i, inout Particle p, int x, int y)
{
    float origTemp = p.temp;
    if (airPV(airIndex(x, y)) < -30.0)
        createSelf(i, p, x, y, PT_CLST); // CRMC -> CLST
    p.temp = origTemp;
    return 0;
}

// Calls the element's update function. Nonzero means skip the movement (the particle may have died).
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
    case PT_SEED:
        return updateSEED(i, p, x, y);
    case PT_PLNT:
        return updatePLNT(i, p, x, y, surroundSpace);
    case PT_VINE:
        return updateVINE(i, p, x, y);
    case PT_WOOD:
        return updateWOOD(i, p, x, y);
    case PT_GOO:
        return updateGOO(i, p, x, y);
    case PT_SPNG:
        return updateSPNG(i, p, x, y);
    case PT_O2:
        return updateO2(i, p, x, y);
    case PT_CO2:
        return updateCO2(i, p, x, y);
    case PT_COAL:
    case PT_BCOL:
        return updateCOAL(i, p, t, x, y);
    case PT_LAVA:
        return updateFIRE(i, p, t, x, y, surroundSpace);
    case PT_GLAS:
        return updateGLAS(i, p, x, y);
    case PT_BMTL:
        return updateBMTL(i, p, x, y);
    case PT_BRMT:
        return updateBRMT(i, p, x, y);
    case PT_IRON:
        return updateIRON(i, p, x, y);
    case PT_GOLD:
        return updateGOLD(i, p, x, y);
    case PT_TTAN:
        return updateTTAN(i, p, x, y, nt);
    case PT_CLST:
        return updateCLST(i, p, x, y);
    case PT_CRMC:
        return updateCRMC(i, p, x, y);
    default:
        return 0;
    }
}
