// Element update functions, ported from TPT's src/simulation/elements/*.cpp.
// Reactions that would create an element the GPU engine doesn't have yet are skipped (gpuHas).
#pragma once
#include "common.slang"
#include "move.slang"

// part_change_type. Changing between an energy particle and any other kind moves it between photons and pmap.
void partChangeType(uint id, uint t)
{
    uint old = ptype[id];
    ptype[id] = t;
    bool wasEnergy = (elems[old].properties & TYPE_ENERGY) != 0;
    bool energy = (elems[t].properties & TYPE_ENERGY) != 0;
    if (wasEnergy == energy)
        return;
    int x = int(parts[id].x + 0.5), y = int(parts[id].y + 0.5);
    if (!inSim(x, y))
        return;
    uint c = cellIndex(x, y);
    uint me = pmapEnc(id);
    uint orig;
    if (energy)
    {
        InterlockedCompareExchange(pmap[c], me, 0u, orig);
        InterlockedExchange(photons[c], me, orig);
    }
    else
    {
        InterlockedCompareExchange(photons[c], me, 0u, orig);
        uint e = pmap[c];
        if ((e & PMAP_LOCK) == 0)
            InterlockedCompareExchange(pmap[c], e, me, orig);
    }
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
    case PT_PLSM:
        p.life = rngBetween(50, 199);
        break;
    case PT_NEUT:
    {
        float r = float(rngBetween(128, 255)) / 127.0;
        float a = float(rngBetween(0, 359)) * 3.14159265 / 180.0;
        p.life = rngBetween(480, 959);
        p.vx = r * cos(a);
        p.vy = r * sin(a);
        break;
    }
    case PT_ELEC:
    {
        float a = float(rngBetween(0, 359)) * 3.14159265 / 180.0;
        p.life = 680;
        p.vx = 2.0 * cos(a);
        p.vy = 2.0 * sin(a);
        break;
    }
    case PT_PROT:
    {
        float a = float(rngBetween(0, 35)) * 0.17453;
        p.life = 680;
        p.vx = 2.0 * cos(a);
        p.vy = 2.0 * sin(a);
        break;
    }
    case PT_GRVT:
    {
        float a = float(rngBetween(0, 359)) * 3.14159265 / 180.0;
        p.life = 250 + rngBetween(0, 199);
        p.vx = 2.0 * cos(a);
        p.vy = 2.0 * sin(a);
        break;
    }
    case PT_PHOT:
    {
        // One of 8 directions at speed 3; a FILT it is created on colours it.
        float a = float(rngBetween(0, 7)) * 0.78540;
        p.vx = 3.0 * cos(a);
        p.vy = 3.0 * sin(a);
        int fid = inSim(x, y) ? pmapId(pmap[cellIndex(x, y)]) : -1;
        if (fid >= 0 && ptype[fid] == PT_FILT)
            p.ctype = filtInteract(parts[fid], p.ctype);
        break;
    }
    case PT_SING:
        p.life = rngBetween(60, 109);
        break;
    case PT_WARP:
        p.life = rngBetween(70, 164);
        break;
    case PT_TUNG:
        p.tmp3 = int(airPV(airIndex(x, y)) * 64.0);
        break;
    case PT_QRTZ:
    case PT_PQRT:
        p.tmp2 = rngBetween(0, 10);
        p.tmp3 = int(airPV(airIndex(x, y)) * 64.0);
        break;
    case PT_SLCN:
        p.tmp = rngBetween(0x100000, 0x9FFFFF); // sparkles, see updateSLCN
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

// What ELEC and the sensors see at (x, y): pmap's particle first, else an energy particle.
int photonsThenPmapReverse(int x, int y)
{
    uint c = cellIndex(x, y);
    int id = pmapId(pmap[c]);
    if (id >= 0 && ptype[id] != PT_NONE)
        return id;
    return pmapId(photons[c]);
}

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

// Puts particle id (now of type t) on top at (x, y) in pmap, or photons for energy particles, taking it out of
// both maps at (ox, oy) if it was there.
void placeInPmap(uint id, uint t, int ox, int oy, int x, int y)
{
    uint me = pmapEnc(id);
    uint orig;
    if (inSim(ox, oy))
    {
        uint oc = cellIndex(ox, oy);
        if (ox != x || oy != y || isEnergy(t))
            InterlockedCompareExchange(pmap[oc], me, 0u, orig);
        if (ox != x || oy != y || !isEnergy(t))
            InterlockedCompareExchange(photons[oc], me, 0u, orig);
    }
    uint c = cellIndex(x, y);
    if (isEnergy(t))
    {
        InterlockedExchange(photons[c], me, orig);
        return;
    }
    uint e = pmap[c];
    if ((e & PMAP_ID_MASK) != me && (e & PMAP_LOCK) == 0)
        InterlockedCompareExchange(pmap[c], e, me, orig);
}

// create_part(-1, x, y, t): a new particle, if the cell is empty (or the new particle may share it, which
// puts it on top) and walls allow it. Returns its id, or -1. The colour passes don't update it until the
// next tick; serial mode updates it later this tick when its id is higher, as TPT does.
// create_part's SPRK branch: "creating" SPRK sparks the conductor already there. From the brush, INST floods.
int sparkAt(int x, int y, bool brush)
{
    int id = pmapId(pmap[cellIndex(x, y)]);
    if (id < 0)
        return -1;
    uint type = ptype[id];
    if (type == PT_WIRE)
    {
        parts[id].ctype = int(PT_DUST); // WIRE's state: TPT's quirk, any nonzero ctype is a spark head
        return id;
    }
    if (!(type == PT_INST || (elems[type].properties & PROP_CONDUCTS) != 0) || parts[id].life != 0)
        return -1;
    if (brush && type == PT_INST)
    {
        requestInstFlood(x, y);
        return id;
    }
    ptype[id] = PT_SPRK;
    parts[id].life = 4;
    parts[id].ctype = int(type);
    float temp = parts[id].temp;
    if (temp + 10.0 < 673.0 && params[0].legacyHeat == 0 &&
        (type == PT_METL || type == PT_BMTL || type == PT_BRMT || type == PT_PSCN || type == PT_NSCN ||
         type == PT_ETRD || type == PT_NBLE || type == PT_IRON))
        parts[id].temp = temp + 10.0;
    return id;
}

int createPart(int x, int y, uint t)
{
    if (!inSim(x, y) || t == PT_NONE || !gpuHas(t))
        return -1;
    if (t == PT_SPRK)
        return sparkAt(x, y, false);
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
    if (isEnergy(t))
    {
        // Energy particles go on top in photons; pmap is unchanged.
        InterlockedExchange(photons[c], pmapEnc(uint(id)), orig);
        return id;
    }
    InterlockedCompareExchange(pmap[c], e, pmapEnc(uint(id)), orig);
    if (orig != e)
    {
        ptype[id] = PT_NONE; // a mover took the cell; the id is free again next tick
        return -1;
    }
    return id;
}

// create_part(-3, x, y, t): no checks on what is there; the new particle goes on top in pmap.
int createPartStacked(int x, int y, uint t)
{
    if (!inSim(x, y) || t == PT_NONE || !gpuHas(t))
        return -1;
    int id = allocPart();
    if (id < 0)
        return -1;
    parts[id] = newParticle(t, x, y, 0xFFFFFFFFu);
    ptype[id] = t;
    uint c = cellIndex(x, y);
    if (isEnergy(t))
    {
        uint o;
        InterlockedExchange(photons[c], pmapEnc(uint(id)), o);
        return id;
    }
    uint cur = pmap[c];
    for (int attempt = 0; attempt < 16; attempt++)
    {
        if ((cur & PMAP_LOCK) != 0)
        {
            cur = pmap[c];
            continue;
        }
        uint orig;
        InterlockedCompareExchange(pmap[c], cur, pmapEnc(uint(id)), orig);
        if (orig == cur)
            break;
        cur = orig;
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
    placeInPmap(id, t, int(old.x + 0.5), int(old.y + 0.5), x, y);
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
    placeInPmap(i, t, ox, oy, x, y);
    return true;
}

// createPartTempVel: create_part in place, keeping temperature and velocity.
void createSelfTempVel(uint i, inout Particle p, int x, int y, uint t)
{
    float temp = p.temp, vx = p.vx, vy = p.vy;
    if (createSelf(i, p, x, y, t))
    {
        p.temp = temp;
        p.vx = vx;
        p.vy = vy;
    }
}

void createPartTempVelAt(uint id, int x, int y, uint t)
{
    Particle old = parts[id];
    if (createPartAt(id, x, y, t))
    {
        parts[id].temp = old.temp;
        parts[id].vx = old.vx;
        parts[id].vy = old.vy;
    }
}

// parts_avg(i, r, PT_GLAS) == PT_GLAS: GLAS halfway between two particles shields them from acid.
bool glassBetween(Particle a, uint other)
{
    Particle b = parts[other];
    int mx = int((a.x + b.x) / 2.0 + 0.5);
    int my = int((a.y + b.y) / 2.0 + 0.5);
    return typeAt(mx, my) == PT_GLAS;
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

// FIRE.cpp (Element_FIRE_update), shared by FIRE, PLSM and LAVA: burning spreads to flammable
// neighbours within 2 px, and lava reacts with what it touches.
int updateFIRE(uint i, inout Particle p, uint t, int x, int y, int surroundSpace)
{
    if (t == PT_PLSM && p.life <= 1)
    {
        if (p.ctype == int(PT_NBLE))
        {
            // PLSM(NBLE) -> NBLE
            partChangeType(i, PT_NBLE);
            p.life = 0;
        }
        else if ((p.tmp & 0x3) == 3)
        {
            // PLSM + O2 + H2 -> WTRV(FIRE)
            partChangeType(i, PT_WTRV);
            p.life = 0;
            p.ctype = int(PT_FIRE);
        }
    }
    else if (t == PT_FIRE && p.life <= 1)
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
            // (t is what causes the spark, rt is what burns)
            if ((surroundSpace != 0 || re.explosive != 0) && re.flammable != 0 &&
                rngChance(int(float(re.flammable) + airPV(airIndex(x + rx, y + ry)) * 10.0), 1000) &&
                (t != PT_SPRK || (rt != PT_RBDM && rt != PT_LRBD && rt != PT_INSL)) &&
                (t != PT_PHOT || rt != PT_INSL) && (rt != PT_SPNG || parts[rid].life == 0))
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
    uint ci = airIndex(x, y);
    if (p.temp > 9773.15 && airPV(ci) > 200.0 && rngChance(1, 5))
    {
        // CO2 -> O2 + NEUT (+ ELEC)
        createSelf(i, p, x, y, PT_O2);
        int j = createPartStacked(x, y, PT_NEUT);
        if (j != -1)
            parts[j].temp = MAX_TEMP;
        if (rngChance(1, 50))
        {
            j = createPartStacked(x, y, PT_ELEC);
            if (j != -1)
                parts[j].temp = MAX_TEMP;
        }
        p.temp = MAX_TEMP;
        setAirPV(ci, airPV(ci) + 100.0);
    }
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

// GOLD's and SLCN's spark check: a fresh SPRK exactly 4 px away (up, down, left, right) sparks this particle.
void takeSparkAt4(uint i, inout Particle p, int x, int y, uint self)
{
    const int2 offs[4] = { int2(-4, 0), int2(4, 0), int2(0, -4), int2(0, 4) };
    for (int j = 0; j < 4; j++)
    {
        int rid = pmapId(pmap[cellIndex(x + offs[j].x, y + offs[j].y)]);
        if (rid >= 0 && ptype[rid] == PT_SPRK && parts[rid].life != 0 && parts[rid].life < 4)
        {
            partChangeType(i, PT_SPRK);
            p.life = 4;
            p.ctype = int(self);
        }
    }
}

// GOLD.cpp: turns rusted iron (BMTL with tmp) within 4 px back into IRON, and takes sparks from 4 px away.
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
    // Find sparks 4 px away
    if (p.life == 0)
        takeSparkAt4(i, p, x, y, PT_GOLD);
    // (Absorbing neutrons waits for NEUT.)
    return 0;
}

// Like a wall: bmap_blockair = 1 and bmap_blockairh = 0x8 for the particle's cell (until next tick's recompute).
void blockAirAt(int x, int y)
{
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
        blockAirAt(x, y);
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

// ACID.cpp: dissolves what it touches (harder things more slowly), weakening as it does.
int updateACID(uint i, inout Particle p, int x, int y)
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
            if (rt == PT_NONE || rt == PT_ACID || rt == PT_CAUS)
                continue;
            if (rt == PT_PLEX || rt == PT_NITR || rt == PT_GUNP || rt == PT_RBDM || rt == PT_LRBD)
            {
                // ACID + PLEX/NITR/GUNP/RBDM/LRBD -> 2xFIRE
                partChangeType(i, PT_FIRE);
                partChangeType(uint(rid), PT_FIRE);
                p.life = 4;
                parts[rid].life = 4;
            }
            else if (rt == PT_WTRV)
            {
                if (rngChance(1, 250))
                {
                    // ACID + WTRV -> CAUS
                    partChangeType(i, PT_CAUS);
                    p.life = rngBetween(25, 74);
                    killPart(uint(rid), x + rx, y + ry);
                }
            }
            else if (rt != PT_CLNE && rt != PT_PCLN && ((rt != PT_FOG && rt != PT_RIME) || parts[rid].tmp <= 5) &&
                     p.life > 50 && rngChance(elems[rt].hardness, 1000))
            {
                if (!glassBetween(p, uint(rid)))
                {
                    p.temp += max((60.0 - float(elems[rt].hardness)) * 7.0, 0.0);
                    p.life--;
                    if (rt == PT_LITH)
                        partChangeType(uint(rid), PT_H2); // ACID + LITH -> ACID + H2
                    else
                        killPart(uint(rid), x + rx, y + ry);
                }
            }
            else if (p.life <= 50)
            {
                killPart(i, x, y);
                return 1;
            }
        }
    }
    // Strength diffuses between neighbouring acid
    for (int trade = 0; trade < 2; trade++)
    {
        int rx = rngBetween(-2, 2);
        int ry = rngBetween(-2, 2);
        if (rx == 0 && ry == 0)
            continue;
        int rid = pmapId(pmap[cellIndex(x + rx, y + ry)]);
        if (rid < 0 || ptype[rid] != PT_ACID || !(p.life > parts[rid].life) || p.life <= 0)
            continue;
        int d = p.life - parts[rid].life;
        if (d == 1)
        {
            parts[rid].life++;
            p.life--;
        }
        else if (d > 0)
        {
            parts[rid].life += d / 2;
            p.life -= d / 2;
        }
    }
    return 0;
}

// CAUS.cpp: caustic gas, like acid; with GAS under pressure it makes refrigerant.
int updateCAUS(uint i, inout Particle p, int x, int y)
{
    bool converted = false;
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
            if (rt == PT_NONE)
                continue;
            if (rt == PT_GAS)
            {
                if (airPV(airIndex(x + rx, y + ry)) > 3.0)
                {
                    // CAUS + GAS -> 2xRFRG
                    partChangeType(uint(rid), PT_RFRG);
                    partChangeType(i, PT_RFRG);
                    converted = true;
                }
            }
            else if (rt != PT_ACID && rt != PT_CAUS && rt != PT_RFRG && rt != PT_RFGL)
            {
                if ((rt != PT_CLNE && rt != PT_PCLN && ((rt != PT_FOG && rt != PT_RIME) || parts[rid].tmp <= 5) &&
                     rngChance(elems[rt].hardness, 1000)) &&
                    p.life > 50)
                {
                    if (!glassBetween(p, uint(rid)))
                    {
                        p.temp += max((60.0 - float(elems[rt].hardness)) * 7.0, 0.0);
                        p.life--;
                        killPart(uint(rid), x + rx, y + ry);
                    }
                }
                else if (p.life <= 50)
                {
                    killPart(i, x, y);
                    return 1;
                }
            }
        }
    }
    return converted ? 1 : 0;
}

// CBNW.cpp: carbonated water fizzes, releasing CO2.
int updateCBNW(uint i, inout Particle p, int x, int y)
{
    uint ci = airIndex(x, y);
    if (airPV(ci) <= 3.0)
    {
        if (airPV(ci) <= -0.5 || rngChance(1, 4000))
        {
            // CBNW -> CO2
            partChangeType(i, PT_CO2);
            p.ctype = 5;
            setAirPV(ci, airPV(ci) + 0.5);
        }
    }
    if (p.tmp2 != 20)
        p.tmp2 -= (p.tmp2 > 20) ? 1 : -1;
    else if (rngChance(1, 200))
        p.tmp2 = rngBetween(0, 39);
    if (p.tmp > 0)
    {
        // Explode
        if (p.tmp == 1 && rngChance(3, 4))
        {
            // CBNW -> CO2
            partChangeType(i, PT_CO2);
            p.ctype = 5;
            setAirPV(ci, airPV(ci) + 0.2);
        }
        p.tmp--;
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
            if (rt == PT_NONE)
                continue;
            uint rprops = elems[rt].properties;
            if ((rprops & TYPE_PART) != 0 && p.tmp == 0 && rngChance(1, 83))
            {
                // Start exploding
                p.tmp = rngBetween(0, 24);
            }
            else if ((rprops & TYPE_SOLID) != 0 && rt != PT_DMND && rt != PT_GLAS && p.tmp == 0 &&
                     rngChance(int(2.0 - airPV(ci)), 6667))
            {
                // CBNW -> CO2
                partChangeType(i, PT_CO2);
                p.ctype = 5;
                setAirPV(ci, airPV(ci) + 0.2);
            }
            if (rt == PT_CBNW)
            {
                // Explosions spread; TPT adjusts by one for neighbours whose update hasn't run yet (higher id).
                if (p.tmp == 0)
                {
                    if (parts[rid].tmp != 0)
                    {
                        p.tmp = parts[rid].tmp;
                        if (uint(rid) > i)
                            p.tmp--;
                    }
                }
                else if (parts[rid].tmp == 0)
                {
                    parts[rid].tmp = p.tmp;
                    if (uint(rid) > i)
                        parts[rid].tmp++;
                }
            }
            else if (rt == PT_RBDM || rt == PT_LRBD)
            {
                if ((params[0].legacyHeat != 0 || p.temp > (273.15 + 12.0)) && rngChance(1, 166))
                {
                    // CBNW + RBDM/LRBD -> CBNW + FIRE
                    partChangeType(i, PT_FIRE);
                    p.life = 4;
                    p.ctype = int(PT_WATR);
                }
            }
            else if (rt == PT_FIRE && parts[rid].ctype != int(PT_WATR))
            {
                killPart(uint(rid), x + rx, y + ry);
                if (rngChance(1, 50))
                {
                    killPart(i, x, y);
                    return 1;
                }
            }
        }
    }
    return 0;
}

// BASE.cpp: concentration in life; boils into BOYL, freezes lower when concentrated, neutralises acid,
// dilutes in water, rusts conductive solids and slowly dissolves soft ones.
int updateBASE(uint i, inout Particle p, int x, int y)
{
    p.tmp = 0; // reset the spark effect
    if (p.life < 1)
        p.life = 1;
    if (p.life > 100)
        p.life = 100;
    float pres = airPV(airIndex(x, y));
    // Base evaporates into BOYL or gets more concentrated
    if (p.life < 100 && pres < 10.0 && p.temp > (120.0 + 273.15))
    {
        if (rngChance(1, 20)) // slow down boiling
        {
            // This keeps the total amount of concentrated BASE in the solution
            if (rngChance(1, p.life + 1))
            {
                // BASE -> BOYL
                createSelfTempVel(i, p, x, y, PT_BOYL);
                return 1;
            }
            p.life++;
            p.temp -= 20.0 / float(p.life); // enthalpy of vaporization
        }
    }
    // The freezing point drops with concentration
    if (p.temp < (273.15 - float(p.life) / 4.0))
    {
        // BASE -> ICE(BASE); the concentration isn't kept
        createSelfTempVel(i, p, x, y, PT_ICEI);
        p.ctype = int(PT_BASE);
        p.life = 0;
        return 1;
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
            if (rt == PT_NONE)
                continue;
            int rctype = parts[rid].ctype;
            // Things base doesn't react with
            if (rt == PT_BASE || rt == PT_SALT || rt == PT_SLTW || rt == PT_BOYL || rt == PT_MERC || rt == PT_BMTL ||
                rt == PT_BRMT || rt == PT_SOAP || rt == PT_CLNE || rt == PT_PCLN ||
                ((rt == PT_ICEI || rt == PT_SNOW) && rctype == int(PT_BASE)) ||
                (rt == PT_SPRK && (rctype == int(PT_BMTL) || rctype == int(PT_BRMT))))
                continue;
            ElementProps re = elems[rt];
            if (p.life > 1 && (rt == PT_WATR || rt == PT_DSTW || rt == PT_CBNW))
            {
                // Base is diluted by water: BASE + WATR/DSTW/CBNW -> 2xBASE
                if (rngChance(1, 20))
                {
                    int saturh = p.life / 2;
                    createPartTempVelAt(uint(rid), x + rx, y + ry, PT_BASE);
                    parts[rid].life = saturh;
                    parts[rid].temp += float(saturh) / 10.0;
                    p.life -= saturh;
                }
            }
            else if (rt == PT_ACID)
            {
                // Base neutralises acid: BASE + ACID -> 2xSLTW
                if (parts[rid].life > 50 && p.life > 0)
                {
                    parts[rid].life--;
                    p.life--;
                }
                if (parts[rid].life <= 50)
                    createPartTempVelAt(uint(rid), x + rx, y + ry, PT_SLTW);
                if (p.life <= 0)
                {
                    createSelfTempVel(i, p, x, y, PT_SLTW);
                    return 1;
                }
            }
            else if (rt == PT_CAUS)
            {
                // Base neutralises CAUS: BASE + CAUS -> SLTW
                if (parts[rid].life > 50 && p.life > 0)
                {
                    parts[rid].life--;
                    p.life--;
                }
                if (parts[rid].life <= 50)
                    killPart(uint(rid), x + rx, y + ry);
                if (p.life <= 0)
                {
                    createSelfTempVel(i, p, x, y, PT_SLTW);
                    return 1;
                }
            }
            else if (p.life >= 70 && rt == PT_OIL)
            {
                // BASE + OIL -> SOAP
                if (gpuHas(PT_SOAP))
                {
                    createSelfTempVel(i, p, x, y, PT_SOAP);
                    killPart(uint(rid), x + rx, y + ry);
                    return 1;
                }
            }
            else if (p.life > 1 && rt == PT_GOO)
            {
                // BASE + GOO -> BASE + GEL
                if (gpuHas(PT_GEL))
                {
                    createPartTempVelAt(uint(rid), x + rx, y + ry, PT_GEL);
                    p.life--;
                }
            }
            else if (p.life > 1 && rt == PT_BCOL)
            {
                // BASE + BCOL -> BASE + GUNP
                createPartTempVelAt(uint(rid), x + rx, y + ry, PT_GUNP);
                p.life--;
            }
            else if (rt == PT_LAVA && rctype == int(PT_ROCK) && pres >= 10.0 && rngChance(1, 1000))
            {
                // BASE + LAVA(ROCK) -> MERC
                if (gpuHas(PT_MERC))
                {
                    createSelfTempVel(i, p, x, y, PT_MERC);
                    killPart(uint(rid), x + rx, y + ry);
                    return 1;
                }
            }
            else if (p.life >= 10 && (re.properties & (TYPE_SOLID | PROP_CONDUCTS)) == (TYPE_SOLID | PROP_CONDUCTS) &&
                     rngChance(1, 10))
            {
                // Base rusts conductive solids: BASE + conductive solid -> BASE + BMTL
                createPartTempVelAt(uint(rid), x + rx, y + ry, PT_BMTL);
                parts[rid].tmp = rngBetween(20, 29);
                p.life--;
                p.tmp = 1; // spark effect
            }
            else if (re.hardness > 0 && re.hardness < 50 && p.life >= 2 * re.hardness &&
                     rngChance(50 - re.hardness, 1000))
            {
                // Base destroys a substance slower if acid destroys it faster
                killPart(uint(rid), x + rx, y + ry);
                p.life -= 2;
                p.tmp = 1;
            }
        }
    }
    // Diffusion
    for (int trade = 0; trade < 2; trade++)
    {
        int rx = rngBetween(-1, 1);
        int ry = rngBetween(-1, 1);
        if (rx == 0 && ry == 0)
            continue;
        int rid = pmapId(pmap[cellIndex(x + rx, y + ry)]);
        if (rid < 0 || ptype[rid] != PT_BASE || !(p.life > parts[rid].life) || p.life <= 1)
            continue;
        int d = p.life - parts[rid].life;
        if (d == 1)
        {
            parts[rid].life++;
            p.life--;
        }
        else if (d > 0)
        {
            parts[rid].life += d / 2;
            p.life -= d / 2;
        }
    }
    return 0;
}

// Fusion's plasma: into a random neighbour PLSM can move into (or one of the fusing gas).
void fusionPlasma(int x, int y, float temp, uint gas)
{
    int rx = x + rngBetween(-1, 1), ry = y + rngBetween(-1, 1);
    uint rt = typeAt(rx, ry);
    if (canMove[PT_PLSM * MAX_ELEMENTS + rt] != 0 || rt == gas)
    {
        int j = createPartStacked(rx, ry, PT_PLSM);
        if (j > -1)
        {
            parts[j].temp = temp;
            parts[j].tmp |= 4;
        }
    }
}

// H2.cpp: burns with fire, plasma and lava; makes OIL from DESL under pressure; fuses when hot and compressed.
int updateH2(uint i, inout Particle p, int x, int y)
{
    float pv = airPV(airIndex(x, y));
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
            if (rt == PT_NONE)
                continue;
            if (pv > 8.0 && rt == PT_DESL)
            {
                // H2 + DESL -> OIL + WATR
                partChangeType(uint(rid), PT_WATR);
                partChangeType(i, PT_OIL);
                return 1;
            }
            if (pv > 45.0)
            {
                if (parts[rid].temp > 2273.15)
                    continue;
            }
            else if (rt == PT_FIRE)
            {
                parts[rid].temp = (parts[rid].tmp & 0x02) != 0 ? 3473.0 : 2473.15;
                parts[rid].tmp |= 1;
                // H2 + FIRE -> 2xFIRE
                createSelf(i, p, x, y, PT_FIRE);
                p.temp += float(rngBetween(0, 99));
                p.tmp |= 1;
                return 1;
            }
            else if ((rt == PT_PLSM && (parts[rid].tmp & 4) == 0) || (rt == PT_LAVA && parts[rid].ctype != int(PT_BMTL)))
            {
                // H2 + PLSM/LAVA -> FIRE + PLSM/LAVA
                createSelf(i, p, x, y, PT_FIRE);
                p.temp += float(rngBetween(0, 99));
                p.tmp |= 1;
                return 1;
            }
        }
    }
    if (p.temp > 2273.15 && pv > 50.0 && rngChance(1, 5))
    {
        // H2 -> NBLE + NEUT + PHOT (+ ELEC, PLSM): fusion
        float temp = p.temp;
        createSelf(i, p, x, y, PT_NBLE);
        p.tmp = 0x1;
        int j = createPartStacked(x, y, PT_NEUT);
        if (j > -1)
            parts[j].temp = temp;
        if (rngChance(1, 10))
        {
            j = createPartStacked(x, y, PT_ELEC);
            if (j > -1)
                parts[j].temp = temp;
        }
        j = createPartStacked(x, y, PT_PHOT);
        if (j > -1)
        {
            parts[j].ctype = 0x7C0000;
            parts[j].temp = temp;
            parts[j].tmp = 0x1;
        }
        fusionPlasma(x, y, temp, PT_H2);
        p.temp = temp + float(rngBetween(750, 1249));
        uint ci = airIndex(x, y);
        setAirPV(ci, airPV(ci) + 30.0);
        return 1;
    }
    return 0;
}

// NBLE.cpp: fuses into CO2 when very hot and compressed.
int updateNBLE(uint i, inout Particle p, int x, int y)
{
    uint ci = airIndex(x, y);
    if (p.temp > 5273.15 && airPV(ci) > 100.0)
    {
        p.tmp |= 0x1;
        if (rngChance(1, 5))
        {
            // NBLE -> CO2 + NEUT + PHOT (+ ELEC, PLSM)
            float temp = p.temp;
            createSelf(i, p, x, y, PT_CO2);
            int j = createPartStacked(x, y, PT_NEUT);
            if (j != -1)
                parts[j].temp = temp;
            if (rngChance(1, 25))
            {
                j = createPartStacked(x, y, PT_ELEC);
                if (j != -1)
                    parts[j].temp = temp;
            }
            j = createPartStacked(x, y, PT_PHOT);
            if (j != -1)
            {
                parts[j].ctype = 0xF800000;
                parts[j].temp = temp;
                parts[j].tmp = 0x1;
            }
            fusionPlasma(x, y, temp, PT_NBLE);
            p.temp = temp + 1750.0 + float(rngBetween(0, 499));
            setAirPV(ci, airPV(ci) + 50.0);
        }
    }
    return 0;
}

// BOYL.cpp: raises the air pressure around it towards temp / 100; with water makes fog, with O2 water.
int updateBOYL(uint i, inout Particle p, int x, int y)
{
    float limit = p.temp / 100.0;
    int cx = x / CELL, cy = y / CELL;
    airNudgePV(cx, cy, limit, 0.001, true);
    airNudgePV(cx, cy + 1, limit, 0.001, true);
    airNudgePV(cx, cy - 1, limit, 0.001, true);
    airNudgePV(cx + 1, cy, limit, 0.001, false);
    airNudgePV(cx + 1, cy + 1, limit, 0.001, false);
    airNudgePV(cx - 1, cy, limit, 0.001, false);
    airNudgePV(cx - 1, cy - 1, limit, 0.001, false);
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
                // BOYL + WATR -> BOYL + FOG
                if (rngChance(1, 30))
                    partChangeType(uint(rid), PT_FOG);
            }
            else if (rt == PT_O2)
            {
                // BOYL + O2 -> WATR
                if (rngChance(1, 9))
                {
                    killPart(uint(rid), x + rx, y + ry);
                    partChangeType(i, PT_WATR);
                    airAddPV(cx, cy, 4.0);
                    return 1;
                }
            }
        }
    }
    return 0;
}

// RFRG.cpp (Element_RFRG_update, also RFGL): temperature follows pressure. tmp holds the last pressure as
// float bits.
int updateRFRG(uint i, inout Particle p, int x, int y)
{
    float newPressure = airPV(airIndex(x, y));
    float oldPressure = asfloat(p.tmp);
    if (isnan(oldPressure))
    {
        p.tmp = asint(newPressure);
        return 0;
    }
    p.temp = restrictFlt(p.temp * ((newPressure + 257.0) / (oldPressure + 257.0)), 0.0, MAX_TEMP);
    p.tmp = asint(newPressure);
    return 0;
}

// GEL.cpp: soaks up water (tmp, 0-100), trades it with SPNG, GEL and BASE, and sticks to things.
int updateGEL(uint i, inout Particle p, int x, int y)
{
    if (p.tmp > 100)
        p.tmp = 100;
    if (p.tmp < 0)
        p.tmp = 0;
    int absorbChanceDenom = p.tmp * 10 + 500;
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
            if (rt == PT_NONE)
                continue;
            bool gel = false;
            if (rt == PT_WATR || rt == PT_DSTW || rt == PT_FRZW)
            {
                if (p.tmp < 100 && rngChance(500, absorbChanceDenom))
                {
                    p.tmp++;
                    killPart(uint(rid), x + rx, y + ry);
                }
            }
            else if (rt == PT_PSTE)
            {
                if (p.tmp < 100 && rngChance(20, absorbChanceDenom))
                {
                    // GEL + PSTE -> GEL + CLST
                    p.tmp++;
                    createPartAt(uint(rid), x + rx, y + ry, PT_CLST);
                }
            }
            else if (rt == PT_SLTW)
            {
                if (p.tmp < 100 && rngChance(50, absorbChanceDenom))
                {
                    // GEL + SLTW -> GEL + SALT
                    p.tmp++;
                    if (rngChance(3, 4))
                        killPart(uint(rid), x + rx, y + ry);
                    else
                        partChangeType(uint(rid), PT_SALT);
                }
            }
            else if (rt == PT_CBNW)
            {
                if (p.tmp < 100 && rngChance(100, absorbChanceDenom))
                {
                    // GEL + CBNW -> GEL + CO2
                    p.tmp++;
                    partChangeType(uint(rid), PT_CO2);
                }
            }
            else if (rt == PT_SPNG)
            {
                // Water diffuses between SPNG and GEL (SPNG's limit is ignored here)
                int sl = parts[rid].life;
                if (sl > 0 && p.tmp < 100 && (sl + 1) > p.tmp)
                {
                    parts[rid].life = sl - 1;
                    p.tmp++;
                }
                else if (p.tmp > 0 && (sl + 1) < p.tmp)
                {
                    parts[rid].life = sl + 1;
                    p.tmp--;
                }
            }
            else if (rt == PT_GEL)
            {
                if ((parts[rid].tmp + 1) < p.tmp)
                {
                    parts[rid].tmp++;
                    p.tmp--;
                }
                gel = true;
            }
            else if (rt == PT_BASE)
            {
                // Base absorbs water from gel
                if (p.tmp > 0 && parts[rid].life > 1)
                {
                    parts[rid].life--;
                    p.tmp--;
                }
            }
            // Stickiness (also from a particle just absorbed above: TPT still reads its position)
            ElementProps re = elems[rt];
            float dx = p.x - parts[rid].x;
            float dy = p.y - parts[rid].y;
            float d2 = dx * dx + dy * dy;
            if (d2 > 1.5 && (gel || re.falldown == 0 || (abs(rx) < 2 && abs(ry) < 2)))
            {
                float nd = d2 - 0.5;
                // TPT divides tmp by 100 as integers: full strength until GEL is saturated
                float per = 5.0 * float(1 - p.tmp / 100) * (nd / (d2 + nd) - 0.5);
                if ((re.properties & TYPE_LIQUID) != 0)
                    per *= 0.1;
                dx *= per;
                dy *= per;
                p.vx += dx;
                p.vy += dy;
                if ((re.properties & TYPE_PART) != 0 || rt == PT_GOO)
                {
                    parts[rid].vx -= dx;
                    parts[rid].vy -= dy;
                }
            }
        }
    }
    return 0;
}

// MERC.cpp: mercury condenses into fewer particles when cold (tmp counts the ones absorbed) and expands
// again when hot.
int updateMERC(uint i, inout Particle p, int x, int y)
{
    const int absorbScale = 10000; // most particles one can hold
    p.temp = restrictFlt(p.temp, MIN_TEMP, MAX_TEMP);
    int maxtmp = int(float(absorbScale) / (p.temp + 1.0)) - 1;
    int it1 = max(int(p.temp) + 1, 1);
    if (rngChance(absorbScale % it1, it1))
        maxtmp++;
    if (p.tmp < 0)
        p.tmp = 0;
    if (p.tmp > absorbScale)
        p.tmp = absorbScale;
    if (p.tmp < maxtmp)
    {
        for (int rx = -1; rx <= 1; rx++)
        {
            for (int ry = -1; ry <= 1; ry++)
            {
                if (rx == 0 && ry == 0)
                    continue;
                int rid = pmapId(pmap[cellIndex(x + rx, y + ry)]);
                if (rid < 0 || p.tmp >= maxtmp)
                    continue;
                if (ptype[rid] == PT_MERC && rngChance(1, 3))
                {
                    if ((p.tmp + parts[rid].tmp + 1) <= maxtmp)
                    {
                        p.tmp += parts[rid].tmp + 1;
                        killPart(uint(rid), x + rx, y + ry);
                    }
                }
            }
        }
    }
    else
    {
        for (int rx = -1; rx <= 1; rx++)
        {
            for (int ry = -1; ry <= 1; ry++)
            {
                if (rx == 0 && ry == 0)
                    continue;
                if (p.tmp <= maxtmp)
                    continue;
                if (typeAt(x + rx, y + ry) == PT_NONE && p.tmp >= 1)
                {
                    int np = createPart(x + rx, y + ry, PT_MERC);
                    if (np < 0)
                        continue;
                    p.tmp--;
                    parts[np].temp = p.temp;
                    parts[np].tmp = 0;
                    parts[np].dcolour = p.dcolour;
                }
            }
        }
    }
    // Diffusion
    for (int trade = 0; trade < 4; trade++)
    {
        int rx = rngBetween(-2, 2);
        int ry = rngBetween(-2, 2);
        if (rx == 0 && ry == 0)
            continue;
        int rid = pmapId(pmap[cellIndex(x + rx, y + ry)]);
        if (rid < 0 || ptype[rid] != PT_MERC || !(p.tmp > parts[rid].tmp) || p.tmp <= 0)
            continue;
        int d = p.tmp - parts[rid].tmp;
        if (d == 1)
        {
            parts[rid].tmp++;
            p.tmp--;
        }
        else if (d > 0)
        {
            parts[rid].tmp += d / 2;
            p.tmp -= d / 2;
        }
    }
    return 0;
}

// FRZW.cpp: freeze water spreads into water and freezes into very cold ice.
int updateFRZW(uint i, inout Particle p, int x, int y)
{
    for (int rx = -1; rx <= 1; rx++)
    {
        for (int ry = -1; ry <= 1; ry++)
        {
            if (rx == 0 && ry == 0)
                continue;
            int rid = pmapId(pmap[cellIndex(x + rx, y + ry)]);
            if (rid >= 0 && ptype[rid] == PT_WATR && rngChance(1, 14))
                partChangeType(uint(rid), PT_FRZW); // FRZW + WATR -> 2xFRZW
        }
    }
    if ((p.life == 0 && rngChance(1, 192)) || rngChance(100 - p.life, 50000))
    {
        // FRZW -> ICEI(FRZW)
        partChangeType(i, PT_ICEI);
        p.ctype = int(PT_FRZW);
        p.temp = restrictFlt(p.temp - 200.0, MIN_TEMP, MAX_TEMP);
    }
    return 0;
}

// FRZZ.cpp
int updateFRZZ(uint i, inout Particle p, int x, int y)
{
    for (int rx = -1; rx <= 1; rx++)
    {
        for (int ry = -1; ry <= 1; ry++)
        {
            if (rx == 0 && ry == 0)
                continue;
            int rid = pmapId(pmap[cellIndex(x + rx, y + ry)]);
            if (rid >= 0 && ptype[rid] == PT_WATR && rngChance(1, 20))
            {
                // FRZZ + WATR -> FRZW
                partChangeType(uint(rid), PT_FRZW);
                parts[rid].life = 100;
                killPart(i, x, y);
                return 1;
            }
        }
    }
    return 0;
}

// YEST.cpp: grows when warm, and dead yeast spreads to it.
int updateYEST(uint i, inout Particle p, int x, int y)
{
    for (int rx = -1; rx <= 1; rx++)
    {
        for (int ry = -1; ry <= 1; ry++)
        {
            if (rx == 0 && ry == 0)
                continue;
            if (typeAt(x + rx, y + ry) == PT_DYST && rngChance(1, 6) && params[0].legacyHeat == 0)
                partChangeType(i, PT_DYST); // YEST -> DYST
        }
    }
    if (p.temp > 303.0 && p.temp < 317.0)
    {
        int nx = x + rngBetween(-1, 1);
        int ny = y + rngBetween(-1, 1);
        createPart(nx, ny, PT_YEST);
    }
    return 0;
}

// ANAR.cpp: anti-air powder; cold flame spreads through it.
int updateANAR(uint i, inout Particle p, int x, int y)
{
    for (int rx = -1; rx <= 1; rx++)
    {
        for (int ry = -1; ry <= 1; ry++)
        {
            if (rx == 0 && ry == 0)
                continue;
            int rid = pmapId(pmap[cellIndex(x + rx, y + ry)]);
            if (rid >= 0 && ptype[rid] == PT_CFLM && rngChance(1, 4))
            {
                // ANAR + CFLM -> 2xCFLM
                partChangeType(i, PT_CFLM);
                p.life = rngBetween(50, 199);
                parts[rid].temp = 0.0;
                p.temp = 0.0;
                uint ci = airIndex(x, y);
                setAirPV(ci, airPV(ci) - 0.5);
            }
        }
    }
    return 0;
}

// QRTZ.cpp (Element_QRTZ_update, also PQRT): breaks into PQRT under changing pressure; grows crystals from
// absorbed salt water.
int updateQRTZ(uint i, inout Particle p, uint t, int x, int y)
{
    if (t == PT_QRTZ)
    {
        int press = int(airPV(airIndex(x, y)) * 64.0);
        float diffTolerance = p.temp * 1.0666;
        if (float(press - p.tmp3) > diffTolerance || float(press - p.tmp3) < -diffTolerance)
        {
            // QRTZ -> PQRT
            partChangeType(i, PT_PQRT);
            p.life = 5; // timer before it can grow or diffuse again
        }
        p.tmp3 = press;
    }
    if (p.life > 5)
        p.life = 5;
    // Absorb SLTW
    if (p.tmp != -1)
    {
        for (int rx = -1; rx <= 1; rx++)
        {
            for (int ry = -1; ry <= 1; ry++)
            {
                if (rx == 0 && ry == 0)
                    continue;
                int rid = pmapId(pmap[cellIndex(x + rx, y + ry)]);
                if (rid >= 0 && ptype[rid] == PT_SLTW && rngChance(1, 500))
                {
                    killPart(uint(rid), x + rx, y + ry);
                    p.tmp++;
                }
            }
        }
    }
    // Grow and diffuse
    if (p.tmp > 0 && (p.vx * p.vx + p.vy * p.vy) < 0.2 && p.life <= 0)
    {
        bool stopgrow = false;
        for (int trade = 0; trade < 9; trade++)
        {
            uint rnd = rngGen() % 0x3FF;
            int rx = int(rnd % 5) - 2;
            int srx = int(rnd % 3) - 1;
            rnd >>= 3;
            int ry = int(rnd % 5) - 2;
            int sry = int(rnd % 3) - 1;
            if (rx == 0 && ry == 0)
                continue;
            if (!stopgrow && typeAt(x + srx, y + sry) == PT_NONE && p.tmp != 0)
            {
                // PQRT/QRTZ -> 2xQRTZ
                int np = createPart(x + srx, y + sry, PT_QRTZ);
                if (np > -1)
                {
                    parts[np].temp = p.temp;
                    int tmp2 = p.tmp2;
                    if (rngChance(1, 2))
                        tmp2 = clamp(tmp2 + rngBetween(-1, 1), 0, 10);
                    parts[np].tmp2 = tmp2;
                    p.tmp--;
                    if (t == PT_PQRT)
                    {
                        // Stationary PQRT growing a crystal is part of it now
                        partChangeType(i, PT_QRTZ);
                    }
                    if (rngChance(1, 2))
                        parts[np].tmp = -1; // dead qrtz
                    else if (p.tmp == 0 && rngChance(1, 15))
                        p.tmp = -1;
                    stopgrow = true;
                }
            }
            // Diffusion
            int rid = pmapId(pmap[cellIndex(x + rx, y + ry)]);
            if (rid < 0 || ptype[rid] != PT_QRTZ || !(p.tmp > parts[rid].tmp) || parts[rid].tmp < 0)
                continue;
            int d = p.tmp - parts[rid].tmp;
            if (d == 1)
            {
                parts[rid].tmp++;
                p.tmp--;
                break;
            }
            if (d > 0)
            {
                parts[rid].tmp += d / 2;
                p.tmp -= d / 2;
                break;
            }
        }
    }
    return 0;
}

// SLCN.cpp: sparkle animation in tmp (drawing only), and takes sparks from 4 px away when cool.
void slcnInitSparkles(inout Particle p)
{
    // bits 31-20 phase increment, 19-16 next colour, 15-12 current colour, 11-0 phase
    p.tmp = rngBetween(0x100000, 0x9FFFFF);
}

int updateSLCN(uint i, inout Particle p, int x, int y)
{
    if (p.tmp == 0)
        slcnInitSparkles(p);
    int phase = (p.tmp & 0xFFF) + ((p.tmp >> 20) & 0xFFF);
    if ((phase & 0x1000) != 0)
    {
        // discard current, current <- next, next <- random, wrap phase
        p.tmp = int((uint(p.tmp) & 0xFFF00000u) | uint(phase & 0xFFF) | (uint(rngBetween(0, 15)) << 16) |
                    ((uint(p.tmp) >> 4) & 0xF000u));
    }
    else
        p.tmp = int((uint(p.tmp) & 0xFFFFF000u) | uint(phase));
    if (p.life == 0 && p.temp < 373.15)
        takeSparkAt4(i, p, x, y, PT_SLCN);
    return 0;
}

// GLOW.cpp: glows with pressure (ctype) and motion (tmp); with water it makes DEUT, with GEL RSST.
int updateGLOW(uint i, inout Particle p, int x, int y)
{
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
            if (rt == PT_WATR && rngChance(1, 400))
            {
                // GLOW + WATR -> DEUT
                if (gpuHas(PT_DEUT))
                {
                    killPart(i, x, y);
                    partChangeType(uint(rid), PT_DEUT);
                    parts[rid].life = 10;
                    return 1;
                }
            }
            else if (rt == PT_GEL)
            {
                // GLOW + GEL -> RSST
                killPart(i, x, y);
                partChangeType(uint(rid), PT_RSST);
                parts[rid].tmp = 0;
                return 1;
            }
        }
    }
    uint ci = airIndex(x, y);
    p.ctype = max(int(airPV(ci) * 16.0), 0);
    p.tmp = abs(int((airVX(ci) + airVY(ci)) * 16.0)) + abs(int((p.vx + p.vy) * 64.0));
    return 0;
}

// RSST.cpp: resist; GUNP and BCOL turn it into FIRW and FSEP. (Its clone ctype copying waits for CLNE.)
int updateRSST(uint i, inout Particle p, int x, int y)
{
    for (int rx = -1; rx <= 1; rx++)
    {
        for (int ry = -1; ry <= 1; ry++)
        {
            int rid = pmapId(pmap[cellIndex(x + rx, y + ry)]);
            if (rid < 0)
                continue;
            uint rt = ptype[rid];
            if (rt == PT_GUNP && gpuHas(PT_FIRW))
            {
                // RSST + GUNP -> FIRW
                createSelf(i, p, x, y, PT_FIRW);
                killPart(uint(rid), x + rx, y + ry);
                return 1;
            }
            if (rt == PT_BCOL && gpuHas(PT_FSEP))
            {
                // RSST + BCOL -> FSEP
                createSelf(i, p, x, y, PT_FSEP);
                p.life = 50;
                killPart(uint(rid), x + rx, y + ry);
                return 1;
            }
            if ((rt == PT_CLNE || rt == PT_PCLN) && parts[rid].ctype != int(PT_RSST))
                p.ctype = parts[rid].ctype;
            if ((rt == PT_BCLN || rt == PT_PBCN) && parts[rid].ctype != int(PT_RSST))
                p.tmp = parts[rid].ctype;
        }
    }
    return 0;
}

// RSSS.cpp: solid resist blocks air like TTAN.
int updateRSSS(uint i, inout Particle p, int x, int y)
{
    for (int rx = -1; rx <= 1; rx++)
    {
        for (int ry = -1; ry <= 1; ry++)
        {
            int rid = pmapId(pmap[cellIndex(x + rx, y + ry)]);
            if (rid < 0)
                continue;
            uint rt = ptype[rid];
            if ((rt == PT_CLNE || rt == PT_PCLN) && parts[rid].ctype != int(PT_RSSS))
                p.ctype = parts[rid].ctype;
            if ((rt == PT_BCLN || rt == PT_PBCN) && parts[rid].ctype != int(PT_RSSS))
                p.tmp = parts[rid].ctype;
        }
    }
    blockAirAt(x, y);
    return 0;
}

// VIRS.cpp (Element_VIRS_update, also VRSS and VRSG): turns what it touches into virus, which remembers the
// original element (tmp2); SOAP cures it (tmp3 counts down to recovery); it dies off slowly (tmp4).
int updateVIRS(uint i, inout Particle p, int x, int y, int surroundSpace)
{
    int rndstore = int(rngGen());
    if (p.tmp3 != 0)
    {
        p.tmp3 -= (rndstore & 0x1) != 0 ? 0 : 1;
        // Cured: back to the original element
        if (p.tmp3 == 0)
        {
            uint orig = uint(p.tmp2);
            if (orig == PT_NONE || orig >= uint(MAX_ELEMENTS) || !gpuHas(orig))
            {
                killPart(i, x, y);
                return 1;
            }
            partChangeType(i, orig);
            p.tmp2 = 0;
            p.tmp3 = 0;
            p.tmp4 = 0;
        }
        return 0;
    }
    // tmp4 counts down so it slowly dies
    if (p.tmp4 != 0)
    {
        if ((rndstore & 0x7) == 0)
        {
            p.tmp4--;
            if (p.tmp4 <= 0)
            {
                killPart(i, x, y);
                return 1;
            }
        }
        rndstore >>= 3;
    }
    for (int rx = -1; rx <= 1; rx++)
    {
        for (int ry = -1; ry <= 1; ry++)
        {
            if (rx == 0 && ry == 0)
            {
                rndstore = int(rngGen()); // reset once, halfway through
                continue;
            }
            int rid = pmapId(pmap[cellIndex(x + rx, y + ry)]);
            if (rid < 0)
                continue;
            uint rt = ptype[rid];
            if (rt == PT_NONE)
                continue;
            bool rVirus = rt == PT_VIRS || rt == PT_VRSS || rt == PT_VRSG;
            if (parts[rid].tmp3 != 0 && rVirus)
            {
                // Spread the "being cured" state
                p.tmp3 = parts[rid].tmp3 + ((rndstore & 0x3) != 0 ? 2 : 1);
                return 0;
            }
            else if (rt == PT_SOAP)
            {
                // Soap cures virus
                p.tmp3 += 10;
                if ((rndstore & 0x3) == 0)
                    killPart(uint(rid), x + rx, y + ry);
                return 0;
            }
            else if (rt == PT_PLSM)
            {
                if (surroundSpace != 0 && rngChance(10 + int(airPV(airIndex(x + rx, y + ry))), 100))
                {
                    // VIRS/VRSS/VRSG + PLSM -> 2xPLSM
                    createSelf(i, p, x, y, PT_PLSM);
                    return 1;
                }
            }
            else if (!rVirus && rt != PT_DMND && rt != PT_BASE)
            {
                // Infect
                if ((rndstore & 0x7) == 0)
                {
                    parts[rid].tmp2 = int(rt);
                    parts[rid].tmp3 = 0;
                    parts[rid].tmp4 = p.tmp4 != 0 ? p.tmp4 + 1 : 0;
                    float rtemp = parts[rid].temp;
                    partChangeType(uint(rid), rtemp < 305.0 ? PT_VRSS : rtemp > 673.0 ? PT_VRSG : PT_VIRS);
                }
                rndstore >>= 3;
            }
            // (Protons making virus last forever wait for PROT.)
        }
    }
    return 0;
}

// parts_avg(a, b, PT_INSL): the element halfway between two particles (rounded positions averaged).
uint partsAvgType(Particle a, Particle b)
{
    int mx = (int(a.x + 0.5) + int(b.x + 0.5)) / 2;
    int my = (int(a.y + 0.5) + int(b.y + 0.5)) / 2;
    return typeAt(mx, my);
}

bool insulated(uint pavg) { return pavg == PT_INSL || pavg == PT_RSSS; }

// SPRK.cpp's conduction rules: can a spark on `sender` pass to `receiver`?
bool sprkConducts(uint sender, uint receiver, Particle self, Particle r, int rx, int ry)
{
    if (receiver == sender && receiver != PT_INST && receiver != PT_QRTZ)
        return true; // everything conducts to itself, except INST
    // Senders with specific outputs
    switch (sender)
    {
    case PT_INST:
        return receiver == PT_NSCN;
    case PT_SWCH:
        if (receiver == PT_PSCN || receiver == PT_NSCN || receiver == PT_WATR || receiver == PT_SLTW ||
            receiver == PT_NTCT || receiver == PT_PTCT || receiver == PT_INWR)
            return false;
        break;
    case PT_ETRD:
        return receiver == PT_METL || receiver == PT_BMTL || receiver == PT_BRMT || receiver == PT_LRBD ||
               receiver == PT_RBDM || receiver == PT_PSCN || receiver == PT_NSCN;
    case PT_NTCT:
        return receiver == PT_PSCN || (receiver == PT_NSCN && self.temp > 373.0);
    case PT_PTCT:
        return receiver == PT_PSCN || (receiver == PT_NSCN && self.temp < 373.0);
    case PT_INWR:
        return receiver == PT_NSCN || receiver == PT_PSCN;
    default:
        break;
    }
    // Receivers with specific inputs
    switch (receiver)
    {
    case PT_QRTZ:
        return (sender == PT_NSCN || sender == PT_METL || sender == PT_PSCN || sender == PT_QRTZ) &&
               (r.temp < 173.15 || airPV(airIndex(int(self.x + 0.5) + rx, int(self.y + 0.5) + ry)) > 8.0);
    case PT_NTCT:
        return sender == PT_NSCN || (sender == PT_PSCN && r.temp > 373.0);
    case PT_PTCT:
        return sender == PT_NSCN || (sender == PT_PSCN && r.temp < 373.0);
    case PT_INWR:
        return sender == PT_NSCN || sender == PT_PSCN;
    case PT_INST:
        return sender == PT_PSCN;
    case PT_NBLE:
        return (self.tmp & 0x1) == 0;
    case PT_PSCN:
        return sender != PT_NSCN;
    default:
        break;
    }
    return true;
}

// SPRK.cpp: a spark on a conductor (ctype). It burns like FIRE, runs its course (life), passes itself on to
// neighbouring conductors following TPT's rules, then reverts with a cooldown.
int updateSPRK(uint i, inout Particle p, int x, int y, int surroundSpace)
{
    int ct = p.ctype;
    updateFIRE(i, p, PT_SPRK, x, y, surroundSpace);

    if (p.life <= 0)
    {
        if (ct == int(PT_WATR) || ct == int(PT_SLTW) || ct == int(PT_PSCN) || ct == int(PT_NSCN) ||
            ct == int(PT_ETRD) || ct == int(PT_INWR))
            p.temp = R_TEMP + 273.15;
        if (ct <= 0 || ct >= MAX_ELEMENTS || !gpuHas(uint(ct)))
            ct = int(PT_METL);
        p.ctype = int(PT_NONE);
        p.life = 4;
        if (ct == int(PT_WATR))
            p.life = 64;
        else if (ct == int(PT_SLTW))
            p.life = 54;
        else if (ct == int(PT_SWCH))
            p.life = 14;
        else if (ct == int(PT_RSST))
        {
            // RSST disappears at the end of its spark cycle
            killPart(i, x, y);
            return 1;
        }
        partChangeType(i, uint(ct));
        return 0;
    }

    // What the spark does depending on what it is on
    switch (uint(ct))
    {
    case PT_SPRK:
        killPart(i, x, y);
        return 1;
    case PT_NTCT:
    case PT_PTCT:
        if (p.temp > 295.0)
            p.temp -= 2.5;
        break;
    case PT_NBLE:
        if (p.life <= 1 && (p.tmp & 0x1) == 0)
        {
            // NBLE -> PLSM(NBLE)
            p.life = rngBetween(50, 199);
            partChangeType(i, PT_PLSM);
            p.ctype = int(PT_NBLE);
            if (p.temp > 5273.15)
                p.tmp |= 0x4;
            p.temp = 3500.0;
            uint ci = airIndex(x, y);
            setAirPV(ci, airPV(ci) + 1.0);
        }
        break;
    case PT_IRON:
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
                if (rt == PT_DSTW || rt == PT_SLTW || rt == PT_WATR)
                {
                    // IRON + DSTW/SLTW/WATR -> IRON + O2/H2 (electrolysis)
                    uint rnd = rngGen() % 100;
                    if (rnd == 0)
                        partChangeType(uint(rid), PT_O2);
                    else if (rnd < 3)
                        partChangeType(uint(rid), PT_H2);
                }
            }
        }
        break;
    case PT_TUNG:
        if (p.temp < 3595.0)
            p.temp += float(rngBetween(-4, 15));
        break;
    default:
        // ETRD (arcs to the nearest electrode) and TESC (lightning) wait for those elements.
        break;
    }

    uint sender = uint(ct);
    for (int rx = -2; rx <= 2; rx++)
    {
        for (int ry = -2; ry <= 2; ry++)
        {
            if (rx == 0 && ry == 0)
                continue;
            int rid = pmapId(pmap[cellIndex(x + rx, y + ry)]);
            if (rid < 0)
                continue;
            uint receiver = ptype[rid];
            if (receiver == PT_NONE)
                continue;
            Particle r = parts[rid];
            uint pavg = partsAvgType(r, p);
            // (De)activation of some elements first
            bool next = false;
            switch (receiver)
            {
            case PT_SWCH:
                if (!insulated(pavg) && p.life < 4)
                {
                    if (sender == PT_PSCN && r.life < 10)
                        parts[rid].life = 10;
                    else if (sender == PT_NSCN)
                    {
                        parts[rid].ctype = int(PT_NONE);
                        parts[rid].life = 9;
                    }
                }
                break;
            case PT_SPRK:
                if (!insulated(pavg) && p.life < 4)
                {
                    if (r.ctype == int(PT_SWCH))
                    {
                        if (sender == PT_NSCN)
                        {
                            partChangeType(uint(rid), PT_SWCH);
                            parts[rid].ctype = int(PT_NONE);
                            parts[rid].life = 9;
                        }
                    }
                    else if ((r.ctype == int(PT_NTCT) || r.ctype == int(PT_PTCT)) && sender == PT_METL)
                        parts[rid].temp = 473.0;
                }
                next = true;
                break;
            case PT_PUMP:
            case PT_GPMP:
            case PT_HSWC:
            case PT_PBCN:
                if (p.life < 4)
                {
                    if (sender == PT_PSCN)
                        parts[rid].life = 10;
                    else if (sender == PT_NSCN && r.life >= 10)
                        parts[rid].life = 9;
                }
                next = true;
                break;
            case PT_LCRY:
                if (abs(rx) < 2 && abs(ry) < 2 && p.life < 4)
                {
                    if (sender == PT_PSCN && r.tmp == 0)
                        parts[rid].tmp = 2;
                    else if (sender == PT_NSCN && r.tmp == 3)
                        parts[rid].tmp = 1;
                }
                next = true;
                break;
            case PT_PPIP:
            case PT_EMP:
                next = true; // (their triggers come with those elements)
                break;
            case PT_NTCT:
            case PT_PTCT:
            case PT_INWR:
                if (sender == PT_METL && !insulated(pavg) && p.life < 4)
                {
                    parts[rid].temp = 473.0;
                    if (receiver == PT_NTCT || receiver == PT_PTCT)
                        next = true;
                }
                break;
            default:
                break;
            }
            if (next || insulated(pavg))
                continue; // insulation blocks everything past here
            // Only conductors, plus INST and QRTZ as special cases
            if (!((elems[receiver].properties & PROP_CONDUCTS) != 0 || receiver == PT_INST || receiver == PT_QRTZ))
                continue;
            // Only SWCH conducts really far
            if (abs(rx) + abs(ry) >= 4 && sender != PT_SWCH && receiver != PT_SWCH)
                continue;
            if (!sprkConducts(sender, receiver, p, r, rx, ry))
                continue;
            // Passed the conduction rules: turn the receiver into SPRK
            if (receiver == PT_WATR || receiver == PT_SLTW)
            {
                if (r.life == 0 && p.life < 3)
                {
                    partChangeType(uint(rid), PT_SPRK);
                    parts[rid].life = receiver == PT_WATR ? 6 : 5;
                    parts[rid].ctype = int(receiver);
                }
            }
            else if (receiver == PT_INST)
            {
                if (r.life == 0 && p.life < 4)
                    requestInstFlood(x + rx, y + ry); // spark the wire
            }
            else if (receiver == PT_RSST)
            {
                if (r.life == 0 && p.life < 4)
                {
                    partChangeType(uint(rid), PT_SPRK);
                    parts[rid].life = 5;
                    parts[rid].ctype = int(receiver);
                }
            }
            else if (r.life == 0 && p.life < 4)
            {
                parts[rid].life = 4;
                parts[rid].ctype = int(receiver);
                partChangeType(uint(rid), PT_SPRK);
                if (r.temp + 10.0 < 673.0 && params[0].legacyHeat == 0 &&
                    (receiver == PT_METL || receiver == PT_BMTL || receiver == PT_BRMT || receiver == PT_PSCN ||
                     receiver == PT_NSCN || receiver == PT_ETRD || receiver == PT_NBLE || receiver == PT_IRON))
                    parts[rid].temp = r.temp + 10.0;
            }
            // (ETRD's own rule for conducting at life 5 comes with ETRD.)
        }
    }
    return 0;
}

// NTCT.cpp / PTCT.cpp (Element_NTCT_update): semiconductors cool off by themselves.
int updateNTCT(uint i, inout Particle p, int x, int y)
{
    if (p.temp > 295.0)
        p.temp -= 2.5;
    return 0;
}

// BTRY.cpp: sparks the conductors around it (not WATR, SLTW, NTCT, PTCT, INWR).
int updateBTRY(uint i, inout Particle p, int x, int y)
{
    for (int rx = -2; rx <= 2; rx++)
    {
        for (int ry = -2; ry <= 2; ry++)
        {
            if ((rx == 0 && ry == 0) || abs(rx) + abs(ry) >= 4)
                continue;
            int rid = pmapId(pmap[cellIndex(x + rx, y + ry)]);
            if (rid < 0)
                continue;
            uint rt = ptype[rid];
            if (rt == PT_NONE || insulated(partsAvgType(p, parts[rid])))
                continue;
            if ((elems[rt].properties & PROP_CONDUCTS) != 0 &&
                !(rt == PT_WATR || rt == PT_SLTW || rt == PT_NTCT || rt == PT_PTCT || rt == PT_INWR) &&
                parts[rid].life == 0)
            {
                parts[rid].life = 4;
                parts[rid].ctype = int(rt);
                partChangeType(uint(rid), PT_SPRK);
            }
        }
    }
    return 0;
}

// SWCH.cpp: switch, on at life 10 (PSCN turns it on, NSCN off); a switch passes on its state to switches
// next to it, and only conducts while on.
int updateSWCH(uint i, inout Particle p, int x, int y)
{
    if (p.life > 0 && p.life != 10)
        p.life--;
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
            if (rt == PT_NONE || insulated(partsAvgType(p, parts[rid])))
                continue;
            int rlife = parts[rid].life;
            if (rt == PT_SWCH)
            {
                if (p.life >= 10 && rlife < 10 && rlife > 0)
                    p.life = 9;
                else if (p.life == 0 && rlife >= 10)
                    p.life = rlife; // not 10, or spark loops form when SWCH is sparked while turning on
            }
            else if (rt == PT_SPRK && p.life == 10 && rlife > 0 && parts[rid].ctype != int(PT_PSCN) &&
                     parts[rid].ctype != int(PT_NSCN))
            {
                partChangeType(i, PT_SPRK);
                p.ctype = int(PT_SWCH);
                p.life = 4;
            }
        }
    }
    // (Switching by two red BRAY beams comes with BRAY.)
    return 0;
}

// WIRE.cpp: WireWorld. ctype is the state (0 wire, 1 spark head, 2 tail); tmp the previous state, copied from
// ctype for every WIRE at the start of the tick (prepare.slang) so all cells update together.
int updateWIRE(uint i, inout Particle p, int x, int y)
{
    int count = 0;
    p.ctype = 0;
    if (p.tmp == 1)
        p.ctype = 2;
    else if (p.tmp == 2)
        p.ctype = 0;
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
            if (rt == PT_SPRK && parts[rid].life == 3 && parts[rid].ctype == int(PT_PSCN))
            {
                p.ctype = 1;
                return 0;
            }
            else if (rt == PT_NSCN && p.tmp == 1)
                createPart(x + rx, y + ry, PT_SPRK);
            else if (rt == PT_WIRE && parts[rid].tmp == 1 && p.tmp == 0)
                count++;
        }
    }
    if (count == 1 || count == 2)
        p.ctype = 1;
    return 0;
}

// Sensors: spark the conductors within 2 px (not WATR, SLTW, NTCT, PTCT, INWR), through no insulation.
void sparkConductorsAround(Particle p, int x, int y)
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
            if (rt == PT_NONE || insulated(partsAvgType(p, parts[rid])))
                continue;
            if ((elems[rt].properties & PROP_CONDUCTS) != 0 &&
                !(rt == PT_WATR || rt == PT_SLTW || rt == PT_NTCT || rt == PT_PTCT || rt == PT_INWR) &&
                parts[rid].life == 0)
            {
                parts[rid].life = 4;
                parts[rid].ctype = int(rt);
                partChangeType(uint(rid), PT_SPRK);
            }
        }
    }
}

// The detection range of DTEC, TSNS, LSNS, VSNS (tmp2, up to 25).
int sensorRange(inout Particle p)
{
    if (p.tmp2 > 25)
        p.tmp2 = 25;
    return p.tmp2;
}

// DTEC.cpp: detects its ctype within range (life 1 = detected last tick, which sparks conductors), and passes
// the colour of light in range on to FILT next to it.
int updateDTEC(uint i, inout Particle p, int x, int y)
{
    int rd = sensorRange(p);
    bool setFilt = false;
    int photonWl = 0;
    if (p.life != 0)
    {
        p.life = 0;
        sparkConductorsAround(p, x, y);
    }
    for (int rx = -rd; rx <= rd; rx++)
    {
        for (int ry = -rd; ry <= rd; ry++)
        {
            if ((rx == 0 && ry == 0) || !inSim(x + rx, y + ry))
                continue;
            int rid = photonsThenPmapReverse(x + rx, y + ry);
            if (rid < 0)
                continue;
            uint rt = ptype[rid];
            if (rt == PT_NONE)
                continue;
            if (int(rt) == p.ctype && (p.ctype != int(PT_LIFE) || p.tmp == parts[rid].ctype || p.tmp == 0))
                p.life = 1;
            if (rt == PT_PHOT || (rt == PT_BRAY && parts[rid].tmp != 2) || rt == PT_BIZR || rt == PT_BIZRG ||
                rt == PT_BIZRS)
            {
                setFilt = true;
                photonWl = parts[rid].ctype;
            }
        }
    }
    if (setFilt)
        sensorFiltOut(x, y, photonWl);
    return 0;
}

// Sensors writing what they measure into lines of FILT next to them (setFilt / serialisation).
void sensorFiltOut(int x, int y, int value)
{
    for (int rx = -1; rx <= 1; rx++)
        for (int ry = -1; ry <= 1; ry++)
            if (rx != 0 || ry != 0)
                writeFiltLine(x, y, rx, ry, value);
}

// TSNS.cpp: temperature sensor (tmp 0: anything hotter than its temp, 2: colder, 1: temperature into FILT).
int updateTSNS(uint i, inout Particle p, int x, int y)
{
    int rd = sensorRange(p);
    if (p.life != 0)
    {
        p.life = 0;
        sparkConductorsAround(p, x, y);
    }
    bool setFilt = false;
    int photonWl = 0;
    for (int rx = -rd; rx <= rd; rx++)
    {
        for (int ry = -rd; ry <= rd; ry++)
        {
            if ((rx == 0 && ry == 0) || !inSim(x + rx, y + ry))
                continue;
            int rid = photonsThenPmapReverse(x + rx, y + ry);
            if (rid < 0)
                continue;
            uint rt = ptype[rid];
            if (rt == PT_NONE || rt == PT_TSNS)
                continue;
            float rtemp = parts[rid].temp;
            if (p.tmp == 0 && rt != PT_METL && rtemp > p.temp)
                p.life = 1;
            if (p.tmp == 2 && rt != PT_METL && rtemp < p.temp)
                p.life = 1;
            if (p.tmp == 1 && rt != PT_FILT)
            {
                setFilt = true;
                photonWl = int(rtemp);
            }
        }
    }
    if (setFilt)
        sensorFiltOut(x, y, 0x10000000 + photonWl);
    return 0;
}

// PSNS.cpp: pressure sensor (tmp 0: pressure above its temp - 273.15, 2: below, 1: pressure into FILT).
int updatePSNS(uint i, inout Particle p, int x, int y)
{
    float pv = airPV(airIndex(x, y));
    if ((p.tmp == 0 && pv > p.temp - 273.15) || (p.tmp == 2 && pv < p.temp - 273.15))
        sparkConductorsAround(p, x, y);
    if (p.tmp == 1)
        sensorFiltOut(x, y, 0x10000000 + int(roundAway(pv) - MIN_PRESSURE));
    return 0;
}

// LSNS.cpp: life sensor (tmp 0: life above its temp - 273.15, 2: at or below; not METL; 1: life into FILT,
// 3: life from FILT into its neighbours).
int updateLSNS(uint i, inout Particle p, int x, int y)
{
    int rd = sensorRange(p);
    if (p.life != 0)
    {
        p.life = 0;
        sparkConductorsAround(p, x, y);
    }
    bool doSerialization = false, doDeserialization = false;
    int life = 0;
    for (int rx = -rd; rx <= rd; rx++)
    {
        for (int ry = -rd; ry <= rd; ry++)
        {
            if ((rx == 0 && ry == 0) || !inSim(x + rx, y + ry))
                continue;
            int rid = photonsThenPmapReverse(x + rx, y + ry);
            if (rid < 0)
                continue;
            uint rt = ptype[rid];
            if (rt == PT_NONE)
                continue;
            float rlife = float(parts[rid].life);
            if (p.tmp == 1)
            {
                if (rt != PT_LSNS && rt != PT_FILT && parts[rid].life >= 0)
                {
                    doSerialization = true;
                    life = parts[rid].life;
                }
            }
            else if (p.tmp == 3)
            {
                if (rt == PT_FILT)
                {
                    doDeserialization = true;
                    life = parts[rid].ctype;
                }
            }
            else if (rt != PT_METL)
            {
                if (p.tmp == 2 ? rlife <= p.temp - 273.15 : rlife > p.temp - 273.15)
                    p.life = 1;
            }
        }
    }
    for (int rx = -1; rx <= 1; rx++)
    {
        for (int ry = -1; ry <= 1; ry++)
        {
            if ((rx == 0 && ry == 0) || (!doSerialization && !doDeserialization))
                continue;
            if (doSerialization)
                writeFiltLine(x, y, rx, ry, 0x10000000 + life);
            else
            {
                int rid = pmapId(pmap[cellIndex(x + rx, y + ry)]);
                if (rid >= 0 && ptype[rid] != PT_NONE && ptype[rid] != PT_FILT)
                    parts[rid].life = life - 0x10000000;
            }
        }
    }
    return 0;
}

// VSNS.cpp: velocity sensor for non-solids (tmp 0: faster than its temp - 273.15, 2: not faster; 1: speed into
// FILT, 3: speed from FILT onto its neighbours).
int updateVSNS(uint i, inout Particle p, int x, int y)
{
    int rd = sensorRange(p);
    if (p.life != 0)
    {
        p.life = 0;
        sparkConductorsAround(p, x, y);
    }
    bool doSerialization = false, doDeserialization = false;
    float vs = 0.0;
    for (int rx = -rd; rx <= rd; rx++)
    {
        for (int ry = -rd; ry <= rd; ry++)
        {
            if ((rx == 0 && ry == 0) || !inSim(x + rx, y + ry))
                continue;
            int rid = photonsThenPmapReverse(x + rx, y + ry);
            if (rid < 0)
                continue;
            uint rt = ptype[rid];
            if (rt == PT_NONE)
                continue;
            bool solid = (elems[rt].properties & TYPE_SOLID) != 0;
            float vx = parts[rid].vx, vy = parts[rid].vy;
            float vm = sqrt(vx * vx + vy * vy);
            if (p.tmp == 1)
            {
                if (rt != PT_VSNS && rt != PT_FILT && !solid)
                {
                    doSerialization = true;
                    vs = vm;
                }
            }
            else if (p.tmp == 3)
            {
                if (rt == PT_FILT)
                {
                    int vel = parts[rid].ctype - 0x10000000;
                    if (vel >= 0 && float(vel) < MAX_VELOCITY)
                    {
                        doDeserialization = true;
                        vs = float(vel);
                    }
                }
            }
            else if (!solid)
            {
                if (p.tmp == 2 ? vm <= p.temp - 273.15 : vm > p.temp - 273.15)
                    p.life = 1;
            }
        }
    }
    for (int rx = -1; rx <= 1; rx++)
    {
        for (int ry = -1; ry <= 1; ry++)
        {
            if ((rx == 0 && ry == 0) || (!doSerialization && !doDeserialization))
                continue;
            if (doSerialization)
                writeFiltLine(x, y, rx, ry, 0x10000000 + int(vs + 0.5));
            if (doDeserialization)
            {
                int rid = pmapId(pmap[cellIndex(x + rx, y + ry)]);
                if (rid < 0)
                    continue;
                uint rt = ptype[rid];
                if (rt != PT_NONE && rt != PT_FILT && (elems[rt].properties & TYPE_SOLID) == 0)
                {
                    float vx = parts[rid].vx, vy = parts[rid].vy;
                    float vm = sqrt(vx * vx + vy * vy);
                    if (vm > 0.0)
                    {
                        parts[rid].vx *= vs / vm;
                        parts[rid].vy *= vs / vm;
                    }
                    break; // (only the inner loop, as in TPT)
                }
            }
        }
    }
    return 0;
}

// LDTC.cpp: linear detector. Looks along the line from each conductor next to it, away from it, skipping
// life px and up to tmp px further (0 = no limit), for its ctype (any element if 0); sparks that conductor
// when found. tmp2 flags: 1 invert the filter, 2 ignore energy particles, 4 don't copy colour, 8 keep
// searching.
int updateLDTC(uint i, inout Particle p, int x, int y)
{
    int ctype = p.ctype & 0x1FF;      // TYP
    int ctypeExtra = p.ctype >> 9;    // ID
    if (p.tmp2 < 0)
        p.tmp2 = 0;
    if (p.tmp < 0)
        p.tmp = 0;
    bool invertFilter = (p.tmp2 & 0x1) != 0;
    bool ignoreEnergy = (p.tmp2 & 0x2) != 0;
    bool copyColor = (p.tmp2 & 0x4) == 0;
    bool keepSearching = (p.tmp2 & 0x8) != 0;
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
            bool boolMode = (elems[rt].properties & PROP_CONDUCTS) != 0 &&
                            !(rt == PT_WATR || rt == PT_SLTW || rt == PT_NTCT || rt == PT_PTCT || rt == PT_INWR) &&
                            parts[rid].life == 0;
            bool filtMode = copyColor && rt == PT_FILT;
            if (!boolMode && !filtMode)
                continue;
            int maxRange = p.life + p.tmp;
            int xStep = -rx, yStep = -ry;
            int xc = x + xStep * (p.life + 1), yc = y + yStep * (p.life + 1);
            for (; p.tmp == 0 || (xStep * (xc - x) <= maxRange && yStep * (yc - y) <= maxRange); xc += xStep, yc += yStep)
            {
                if (!inSim(xc, yc))
                    break;
                uint fc = cellIndex(xc, yc);
                int fid = pmapId(pmap[fc]);
                if ((fid < 0 || ptype[fid] == PT_NONE) && !ignoreEnergy)
                    fid = pmapId(photons[fc]);
                if (fid < 0)
                    continue;
                uint ft = ptype[fid];
                if (ft == PT_NONE)
                    continue;
                bool matchesCtype = ctype == int(ft) && (ctype != int(PT_LIFE) || parts[fid].ctype == ctypeExtra);
                bool matchesFilter = ctype == 0 || (invertFilter != matchesCtype);
                if (!matchesFilter)
                {
                    if (keepSearching)
                        continue;
                    break;
                }
                if (boolMode)
                {
                    parts[rid].life = 4;
                    parts[rid].ctype = int(rt);
                    partChangeType(uint(rid), PT_SPRK);
                    break;
                }
                // Copy the colour of the light (or FILT) found into the FILT next to it
                if (!(ft == PT_FILT || ft == PT_PHOT || ft == PT_BRAY || ft == PT_BIZR || ft == PT_BIZRG || ft == PT_BIZRS))
                    continue;
                writeFiltLine(x, y, rx, ry, ft == PT_FILT ? filtWavelengths(parts[fid]) : parts[fid].ctype);
                break;
            }
        }
    }
    return 0;
}

// FUSE.cpp: burns slowly (life) once lit by a spark or heat, giving off plasma; pressure turns it to FSEP.
int updateFUSE(uint i, inout Particle p, int x, int y)
{
    if (p.life <= 0)
    {
        // FUSE -> PLSM
        if (createSelf(i, p, x, y, PT_PLSM))
            p.life = 50;
        return 1;
    }
    else if (p.life < 40)
    {
        p.life--;
        if (rngChance(1, 100))
        {
            int px = x + rngBetween(-1, 1);
            int py = y + rngBetween(-1, 1);
            int r = createPart(px, py, PT_PLSM);
            if (r > -1)
                parts[r].life = 50;
        }
    }
    if (airPV(airIndex(x, y)) > 2.7 && p.tmp > 40)
        p.tmp = 39;
    else if (p.tmp <= 0)
    {
        // FUSE -> FSEP
        createSelf(i, p, x, y, PT_FSEP);
        return 1;
    }
    else if (p.tmp < 40)
        p.tmp--;
    for (int rx = -2; rx <= 2; rx++)
    {
        for (int ry = -2; ry <= 2; ry++)
        {
            if (rx == 0 && ry == 0)
                continue;
            int rid = pmapId(pmap[cellIndex(x + rx, y + ry)]);
            if (rid < 0 || ptype[rid] == PT_NONE)
                continue;
            if (ptype[rid] == PT_SPRK || (p.temp >= (273.15 + 700.0) && rngChance(1, 20)))
            {
                if (p.life > 40)
                    p.life = 39;
            }
        }
    }
    return 0;
}

// FSEP.cpp: fuse powder.
int updateFSEP(uint i, inout Particle p, int x, int y)
{
    if (p.life <= 0)
    {
        // FSEP -> PLSM
        if (createSelf(i, p, x, y, PT_PLSM))
            p.life = 50;
        return 1;
    }
    else if (p.life < 40)
    {
        p.life--;
        if (rngChance(1, 10))
        {
            int px = x + rngBetween(-1, 1);
            int py = y + rngBetween(-1, 1);
            int r = createPart(px, py, PT_PLSM);
            if (r > -1)
                parts[r].life = 50;
        }
    }
    else
    {
        for (int rx = -2; rx <= 2; rx++)
        {
            for (int ry = -2; ry <= 2; ry++)
            {
                if (rx == 0 && ry == 0)
                    continue;
                int rid = pmapId(pmap[cellIndex(x + rx, y + ry)]);
                if (rid < 0 || ptype[rid] == PT_NONE)
                    continue;
                if ((ptype[rid] == PT_SPRK || p.temp >= (273.15 + 400.0)) && p.life > 40 && rngChance(1, 15))
                    p.life = 39;
            }
        }
    }
    return 0;
}

// IGNT.cpp: ignition cord; once lit it throws embers and fire for a few ticks, and lights its neighbours.
int updateIGNT(uint i, inout Particle p, int x, int y)
{
    if (p.tmp == 0)
    {
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
                // A burning neighbour lights it at life 1. TPT's update order never changes, so every particle
                // sees that tick; the colour passes are reshuffled each tick and can skip it, which would break the
                // cord, so a burnt-out lit neighbour (life 0, gone next tick) counts too, one tick late at most.
                if (rt == PT_FIRE || rt == PT_PLSM || rt == PT_SPRK || rt == PT_LIGH ||
                    (rt == PT_IGNT && (parts[rid].life == 1 || (parts[rid].life == 0 && parts[rid].tmp != 0))))
                    p.tmp = 1;
            }
        }
    }
    else if (p.life > 0)
    {
        if (rngChance(2, 3))
        {
            // IGNT -> IGNT + EMBR
            int ex = x + rngBetween(-1, 1);
            int ey = y + rngBetween(-1, 1);
            int nb = createPart(ex, ey, PT_EMBR);
            if (nb != -1)
            {
                parts[nb].tmp = 0;
                parts[nb].life = 30;
                parts[nb].vx = float(rngBetween(-10, 10));
                parts[nb].vy = float(rngBetween(-10, 10));
                parts[nb].temp = restrictFlt(p.temp - 273.15 + 400.0, MIN_TEMP, MAX_TEMP);
            }
        }
        else
        {
            // IGNT -> IGNT + FIRE
            int fx = x + rngBetween(-1, 1);
            int fy = y + rngBetween(-1, 1);
            createPart(fx, fy, PT_FIRE);
        }
        p.life--;
    }
    return 0;
}

// BANG.cpp: TNT. Lit by heat, fire, plasma or sparks, it sets the whole connected blob off (flood_prop, done
// after the passes) and explodes two ticks later.
int updateBANG(uint i, inout Particle p, int x, int y)
{
    if (p.tmp == 0)
    {
        if (p.temp >= 673.0)
            p.tmp = 1;
        else
        {
            for (int rx = -1; rx <= 1; rx++)
            {
                for (int ry = -1; ry <= 1; ry++)
                {
                    if (rx == 0 && ry == 0)
                        continue;
                    uint rt = typeAt(x + rx, y + ry);
                    if (rt == PT_FIRE || rt == PT_PLSM || rt == PT_SPRK || rt == PT_LIGH)
                        p.tmp = 1;
                }
            }
        }
    }
    else if (p.tmp == 1)
    {
        if (pmapId(pmap[cellIndex(x, y)]) == int(i))
            requestBangFlood(x, y);
        p.tmp = 2;
    }
    else if (p.tmp == 2)
        p.tmp = 3;
    else
    {
        float otemp = p.temp - 273.15;
        // Explode!
        uint ci = airIndex(x, y);
        setAirPV(ci, airPV(ci) + 0.5);
        p.tmp = 0;
        if (rngChance(1, 3))
        {
            if (rngChance(1, 2))
                createSelf(i, p, x, y, PT_FIRE); // BANG -> FIRE
            else
            {
                createSelf(i, p, x, y, PT_SMKE); // BANG -> SMKE
                p.life = rngBetween(500, 549);
            }
            p.temp = restrictFlt((MAX_TEMP / 4.0) + otemp, MIN_TEMP, MAX_TEMP);
        }
        else if (rngChance(1, 15))
        {
            // BANG -> EMBR
            createSelf(i, p, x, y, PT_EMBR);
            p.tmp = 0;
            p.life = 50;
            p.temp = restrictFlt((MAX_TEMP / 3.0) + otemp, MIN_TEMP, MAX_TEMP);
            p.vx = float(rngBetween(-10, 10));
            p.vy = float(rngBetween(-10, 10));
        }
        else
            killPart(i, x, y);
        return 1;
    }
    return 0;
}

// BOMB.cpp: explodes on touching almost anything (the explosion itself runs after the passes).
int updateBOMB(uint i, inout Particle p, int x, int y)
{
    for (int rx = -1; rx <= 1; rx++)
    {
        for (int ry = -1; ry <= 1; ry++)
        {
            if (rx == 0 && ry == 0)
                continue;
            uint rt = typeAt(x + rx, y + ry);
            if (rt != PT_NONE && rt != PT_BOMB && rt != PT_EMBR && rt != PT_DMND && rt != PT_CLNE && rt != PT_PCLN &&
                rt != PT_BCLN && rt != PT_VIBR)
            {
                requestBombExplosion(i);
                return 1;
            }
        }
    }
    return 0;
}

// EMBR.cpp: embers die on touching anything solid, powdery or liquid that sparks don't settle on.
int updateEMBR(uint i, inout Particle p, int x, int y)
{
    for (int rx = -1; rx <= 1; rx++)
    {
        for (int ry = -1; ry <= 1; ry++)
        {
            if (rx == 0 && ry == 0)
                continue;
            uint rt = typeAt(x + rx, y + ry);
            if (rt == PT_NONE)
                continue;
            uint props = elems[rt].properties;
            if ((props & (TYPE_SOLID | TYPE_PART | TYPE_LIQUID)) != 0 && (props & PROP_SPARKSETTLE) == 0)
            {
                killPart(i, x, y);
                return 1;
            }
        }
    }
    return 0;
}

// C5.cpp: cold explosive, set off by anything colder than 100 K (or CFLM). Releasing a stored photon waits
// for PHOT.
int updateC5(uint i, inout Particle p, int x, int y)
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
            if (rt == PT_NONE)
                continue;
            if ((rt != PT_C5 && parts[rid].temp < 100.0 && !isHeatInsulator(rt, parts[rid].life)) || rt == PT_CFLM)
            {
                if (rngChance(1, 6))
                {
                    // C5 -> CFLM
                    partChangeType(i, PT_CFLM);
                    parts[rid].temp = 0.0;
                    p.temp = 0.0;
                    p.life = rngBetween(50, 199);
                    uint ci = airIndex(x, y);
                    setAirPV(ci, airPV(ci) + 1.5);
                }
            }
        }
    }
    return 0;
}

// DEST.cpp: destroys what it hits, with heat and huge pressure.
int updateDEST(uint i, inout Particle p, int x, int y)
{
    int rx = rngBetween(-2, 2);
    int ry = rngBetween(-2, 2);
    int rid = pmapId(pmap[cellIndex(x + rx, y + ry)]);
    if (rid < 0)
        return 0;
    uint rt = ptype[rid];
    if (rt == PT_NONE || rt == PT_DEST || rt == PT_DMND || rt == PT_BCLN || rt == PT_CLNE || rt == PT_PCLN ||
        rt == PT_PBCN)
        return 0;
    uint ci = airIndex(x, y);
    if (p.life <= 0 || p.life > 37)
    {
        p.life = rngBetween(30, 49);
        setAirPV(ci, airPV(ci) + 60.0);
    }
    if (rt == PT_PLUT || rt == PT_DEUT)
    {
        setAirPV(ci, airPV(ci) + 20.0);
        if (rngChance(1, 2) && gpuHas(PT_NEUT))
        {
            // DEST + PLUT/DEUT -> DEST + NEUT
            createPartAt(uint(rid), x + rx, y + ry, PT_NEUT);
            parts[rid].temp = MAX_TEMP;
            setAirPV(ci, airPV(ci) + 10.0);
            p.life -= 4;
        }
    }
    else if (rt == PT_INSL)
    {
        // DEST + INSL -> DEST + PLSM
        createPartAt(uint(rid), x + rx, y + ry, PT_PLSM);
    }
    else if (rngChance(1, 3))
    {
        killPart(uint(rid), x + rx, y + ry);
        p.life -= 4 * ((elems[rt].properties & TYPE_SOLID) != 0 ? 3 : 1);
        if (p.life <= 0)
            p.life = 1;
    }
    else if (!isHeatInsulator(rt, parts[rid].life))
        parts[rid].temp = MAX_TEMP;
    p.temp = MAX_TEMP;
    setAirPV(ci, restrictFlt(airPV(ci) + 80.0, MIN_PRESSURE, MAX_PRESSURE));
    return 0;
}

// Renderer::firwTable: the colours FIRW's embers take.
static const uint FIRW_TABLE[200] = {
    0xFF00FFu, 0xF900FFu, 0xF300FFu, 0xEC00FFu, 0xE600FFu, 0xE000FFu, 0xD900FFu, 0xD300FFu,
    0xCD00FFu, 0xC600FFu, 0xC000FFu, 0xB900FFu, 0xB300FFu, 0xAD00FFu, 0xA600FFu, 0xA000FFu,
    0x9A00FFu, 0x9300FFu, 0x8D00FFu, 0x8600FFu, 0x8000FFu, 0x7A00FFu, 0x7300FFu, 0x6D00FFu,
    0x6700FFu, 0x6000FFu, 0x5A00FFu, 0x5300FFu, 0x4D00FFu, 0x4700FFu, 0x4000FFu, 0x3A00FFu,
    0x3400FFu, 0x2D00FFu, 0x2700FFu, 0x2000FFu, 0x1A00FFu, 0x1400FFu, 0x0D00FFu, 0x0700FFu,
    0x0000FFu, 0x0006FFu, 0x000CFFu, 0x0013FFu, 0x0019FFu, 0x001FFFu, 0x0026FFu, 0x002CFFu,
    0x0032FFu, 0x0039FFu, 0x003FFFu, 0x0046FFu, 0x004CFFu, 0x0052FFu, 0x0059FFu, 0x005FFFu,
    0x0065FFu, 0x006CFFu, 0x0072FFu, 0x0079FFu, 0x007FFFu, 0x0085FFu, 0x008CFFu, 0x0092FFu,
    0x0098FFu, 0x009FFFu, 0x00A5FFu, 0x00ACFFu, 0x00B2FFu, 0x00B8FFu, 0x00BFFFu, 0x00C5FFu,
    0x00CCFFu, 0x00D2FFu, 0x00D8FFu, 0x00DFFFu, 0x00E5FFu, 0x00EBFFu, 0x00F2FFu, 0x00F8FFu,
    0x00FFFFu, 0x00FFF9u, 0x00FFF3u, 0x00FFECu, 0x00FFE6u, 0x00FFE0u, 0x00FFD9u, 0x00FFD3u,
    0x00FFCDu, 0x00FFC6u, 0x00FFC0u, 0x00FFB9u, 0x00FFB3u, 0x00FFADu, 0x00FFA6u, 0x00FFA0u,
    0x00FF9Au, 0x00FF93u, 0x00FF8Du, 0x00FF86u, 0x00FF80u, 0x00FF7Au, 0x00FF73u, 0x00FF6Du,
    0x00FF67u, 0x00FF60u, 0x00FF5Au, 0x00FF53u, 0x00FF4Du, 0x00FF47u, 0x00FF40u, 0x00FF3Au,
    0x00FF34u, 0x00FF2Du, 0x00FF27u, 0x00FF20u, 0x00FF1Au, 0x00FF14u, 0x00FF0Du, 0x00FF07u,
    0x00FF00u, 0x06FF00u, 0x0CFF00u, 0x13FF00u, 0x19FF00u, 0x1FFF00u, 0x26FF00u, 0x2CFF00u,
    0x32FF00u, 0x39FF00u, 0x3FFF00u, 0x46FF00u, 0x4CFF00u, 0x52FF00u, 0x59FF00u, 0x5FFF00u,
    0x65FF00u, 0x6CFF00u, 0x72FF00u, 0x79FF00u, 0x7FFF00u, 0x85FF00u, 0x8CFF00u, 0x92FF00u,
    0x99FF00u, 0x9FFF00u, 0xA5FF00u, 0xACFF00u, 0xB2FF00u, 0xB8FF00u, 0xBFFF00u, 0xC5FF00u,
    0xCBFF00u, 0xD2FF00u, 0xD8FF00u, 0xDFFF00u, 0xE5FF00u, 0xEBFF00u, 0xF2FF00u, 0xF8FF00u,
    0xFFFF00u, 0xFFF900u, 0xFFF300u, 0xFFEC00u, 0xFFE600u, 0xFFE000u, 0xFFD900u, 0xFFD300u,
    0xFFCD00u, 0xFFC600u, 0xFFC000u, 0xFFB900u, 0xFFB300u, 0xFFAD00u, 0xFFA600u, 0xFFA000u,
    0xFF9A00u, 0xFF9300u, 0xFF8D00u, 0xFF8600u, 0xFF8000u, 0xFF7A00u, 0xFF7300u, 0xFF6D00u,
    0xFF6600u, 0xFF6000u, 0xFF5A00u, 0xFF5300u, 0xFF4D00u, 0xFF4700u, 0xFF4000u, 0xFF3A00u,
    0xFF3400u, 0xFF2D00u, 0xFF2700u, 0xFF2000u, 0xFF1A00u, 0xFF1400u, 0xFF0D00u, 0xFF0700u,
};

// FWRK and FIRW: burst into 40 coloured embers at their own position.
void fireworkBurst(uint i, Particle p, int x, int y, uint col)
{
    for (int n = 0; n < 40; n++)
    {
        int np = createPartStacked(x, y, PT_EMBR);
        if (np > -1)
        {
            float magnitude = float(rngBetween(40, 99)) * 0.05;
            float angle = float(rngBetween(0, 6283)) * 0.001;
            parts[np].vx = p.vx * 0.5 + cos(angle) * magnitude;
            parts[np].vy = p.vy * 0.5 + sin(angle) * magnitude;
            parts[np].ctype = int(col);
            parts[np].tmp = 1;
            parts[np].life = rngBetween(70, 109);
            parts[np].temp = float(rngBetween(5750, 6249));
            parts[np].dcolour = p.dcolour;
        }
    }
    uint ci = airIndex(x, y);
    setAirPV(ci, airPV(ci) + 8.0);
    killPart(i, x, y);
}

// FWRK.cpp: firework that launches when hot (or ctype DUST) and bursts.
int updateFWRK(uint i, inout Particle p, int x, int y, int surroundSpace)
{
    if (p.life == 0 && ((surroundSpace != 0 && p.temp > 400.0 && rngChance(int(9.0 + p.temp / 40.0), 100000)) ||
                        p.ctype == int(PT_DUST)))
    {
        float fg = elems[PT_FWRK].gravity;
        float gx, gy;
        gravityField(x, y, fg, 1.0, gx, gy);
        if (gx * gx + gy * gy < 0.001)
        {
            float angle = float(rngBetween(0, 6283)) * 0.001;
            gx += sin(angle) * fg * 0.5;
            gy += cos(angle) * fg * 0.5;
        }
        float gmax = max(abs(gx), abs(gy));
        if (evalMove(PT_FWRK, int(float(x) - (gx / gmax) + 0.5), int(float(y) - (gy / gmax) + 0.5)) != 0)
        {
            float multiplier = 15.0 / sqrt(gx * gx + gy * gy);
            // Some variation in speed along gravity, and a bit more across it
            int randTmp = rngBetween(-100, 100);
            gx += gx * float(randTmp) * 0.002;
            gy += gy * float(randTmp) * 0.002;
            randTmp = rngBetween(-100, 100);
            gx += -gy * float(randTmp) * 0.005;
            gy += gx * float(randTmp) * 0.005;
            p.life = rngBetween(18, 27);
            p.ctype = 0;
            p.vx -= gx * multiplier;
            p.vy -= gy * multiplier;
            return 0;
        }
    }
    if (p.life < 3 && p.life > 0)
    {
        // FWRK -> EMBR
        uint r = uint(rngBetween(11, 255));
        uint g = uint(rngBetween(11, 255));
        uint b = uint(rngBetween(11, 255));
        fireworkBurst(i, p, x, y, (r << 16) | (g << 8) | b);
        return 1;
    }
    if (p.life >= 45)
        p.life = 0;
    return 0;
}

// FIRW.cpp: firework lit by fire, plasma or thunder; flies, then bursts.
int updateFIRW(uint i, inout Particle p, int x, int y)
{
    if (p.tmp <= 0)
    {
        for (int rx = -1; rx <= 1; rx++)
        {
            for (int ry = -1; ry <= 1; ry++)
            {
                if (rx == 0 && ry == 0)
                    continue;
                uint rt = typeAt(x + rx, y + ry);
                if (rt == PT_FIRE || rt == PT_PLSM || rt == PT_THDR)
                {
                    float fg = elems[PT_FIRW].gravity;
                    float gx, gy;
                    gravityField(x, y, fg, 1.0, gx, gy);
                    if (gx * gx + gy * gy < 0.001)
                    {
                        float angle = float(rngBetween(0, 6283)) * 0.001;
                        gx += sin(angle) * fg * 0.5;
                        gy += cos(angle) * fg * 0.5;
                    }
                    p.tmp = 1;
                    p.life = rngBetween(20, 29);
                    float multiplier = float(p.life + 20) * 0.2 / sqrt(gx * gx + gy * gy);
                    p.vx -= gx * multiplier;
                    p.vy -= gy * multiplier;
                    return 0;
                }
            }
        }
    }
    else if (p.tmp == 1)
    {
        if (p.life <= 0)
            p.tmp = 2;
        else
            p.flags &= ~FLAG_STAGNANT;
    }
    else
    {
        // FIRW -> EMBR
        fireworkBurst(i, p, x, y, FIRW_TABLE[rngBetween(0, 199)]);
        return 1;
    }
    return 0;
}

// THDR.cpp: thunder sparks conductors and blasts the air when it hits something, then vanishes.
int updateTHDR(uint i, inout Particle p, int x, int y)
{
    bool kill = false;
    uint ci = airIndex(x, y);
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
            if (rt == PT_NONE)
                continue;
            if ((elems[rt].properties & PROP_CONDUCTS) != 0 && parts[rid].life == 0 &&
                !(rt == PT_WATR || rt == PT_SLTW) && parts[rid].ctype != int(PT_SPRK))
            {
                parts[rid].ctype = int(rt);
                partChangeType(uint(rid), PT_SPRK);
                parts[rid].life = 4;
                kill = true;
            }
            else if (rt != PT_CLNE && rt != PT_THDR && rt != PT_SPRK && rt != PT_DMND && rt != PT_FIRE)
            {
                setAirPV(ci, restrictFlt(airPV(ci) + 100.0, MIN_PRESSURE, MAX_PRESSURE));
                if (params[0].legacyHeat != 0 && rngChance(1, 200))
                {
                    // THDR -> FIRE
                    p.life = rngBetween(120, 169);
                    partChangeType(i, PT_FIRE);
                }
                else
                    kill = true;
            }
        }
    }
    if (kill)
    {
        killPart(i, x, y);
        return 1;
    }
    return 0;
}

// Cloners (CLNE, BCLN, PCLN, PBCN): pick up the element that touches them, then make copies of it.
bool clonerHasCtype(Particle p)
{
    return p.ctype > 0 && p.ctype < MAX_ELEMENTS && elems[p.ctype].existsInTpt != 0;
}

// The particle a cloner, CONV or sensor sees at (x, y): an energy particle there first, else pmap's.
int photonsThenPmap(int x, int y)
{
    uint c = cellIndex(x, y);
    int id = pmapId(photons[c]);
    if (id >= 0 && ptype[id] != PT_NONE)
        return id;
    return pmapId(pmap[c]);
}

void clonerPickCtype(inout Particle p, int x, int y, bool powered)
{
    for (int rx = -1; rx <= 1; rx++)
    {
        for (int ry = -1; ry <= 1; ry++)
        {
            int rid = photonsThenPmap(x + rx, y + ry);
            if (rid < 0)
                continue;
            uint rt = ptype[rid];
            if (rt == PT_NONE || rt == PT_CLNE || rt == PT_PCLN || rt == PT_BCLN || rt == PT_STKM || rt == PT_PBCN ||
                rt == PT_STKM2)
                continue;
            if (powered && (rt == PT_SPRK || rt == PT_NSCN || rt == PT_PSCN))
                continue;
            p.ctype = int(rt);
            if (rt == PT_LIFE || rt == PT_LAVA)
                p.tmp = parts[rid].ctype;
        }
    }
}

// PCLN and PBCN make photons a different way: one into each of the 8 neighbours, flying straight out.
void clonerEmitPhotons(uint i, int x, int y)
{
    for (int rx = -1; rx <= 1; rx++)
    {
        for (int ry = -1; ry <= 1; ry++)
        {
            if (rx == 0 && ry == 0)
                continue;
            int r = createPart(x + rx, y + ry, PT_PHOT);
            if (r != -1)
            {
                parts[r].vx = float(rx * 3);
                parts[r].vy = float(ry * 3);
                // (TPT keeps a photon with a higher id from moving until next frame, to avoid gaps in the beam;
                // new particles here aren't updated until next tick anyway.)
            }
        }
    }
}

void clonerEmit(Particle p, int x, int y)
{
    // (LIFE is cloned its own way, which comes with LIFE.)
    if (p.ctype == int(PT_LIFE))
        return;
    if (p.ctype != int(PT_LIGH) || rngChance(1, 30))
    {
        int cx = x + rngBetween(-1, 1);
        int cy = y + rngBetween(-1, 1);
        int np = createPart(cx, cy, uint(p.ctype & 0x1FF));
        if (np >= 0 && p.ctype == int(PT_LAVA) && p.tmp > 0 && p.tmp < MAX_ELEMENTS &&
            elems[p.tmp].highTemperatureT == int(PT_LAVA))
            parts[np].ctype = p.tmp;
    }
}

// CLNE.cpp
int updateCLNE(uint i, inout Particle p, int x, int y)
{
    if (!clonerHasCtype(p))
        clonerPickCtype(p, x, y, false);
    else
        clonerEmit(p, x, y);
    return 0;
}

// BCLN.cpp: breakable clone, knocked loose by pressure.
int updateBCLN(uint i, inout Particle p, int x, int y)
{
    uint ci = airIndex(x, y);
    if (p.life == 0 && airPV(ci) > 4.0)
        p.life = rngBetween(80, 119);
    if (p.life != 0)
    {
        p.vx += 0.1 * airVX(ci);
        p.vy += 0.1 * airVY(ci);
    }
    if (!clonerHasCtype(p))
        clonerPickCtype(p, x, y, false);
    else
        clonerEmit(p, x, y);
    return 0;
}

// PCLN.cpp: powered clone, on (life 10) after PSCN sparks, off after NSCN; the state spreads to neighbours.
int updatePCLN(uint i, inout Particle p, int x, int y)
{
    if (p.life > 0 && p.life != 10)
        p.life--;
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
            Particle r = parts[rid];
            if (rt == PT_SPRK)
            {
                if (r.life > 0 && r.life < 4)
                {
                    if (r.ctype == int(PT_PSCN))
                        p.life = 10;
                    else if (r.ctype == int(PT_NSCN))
                        p.life = 9;
                }
            }
            else if (rt == PT_PCLN)
            {
                if (p.life == 10 && r.life < 10 && r.life > 0)
                    p.life = 9;
                else if (p.life == 0 && r.life == 10)
                    p.life = 10;
            }
        }
    }
    if (!clonerHasCtype(p))
        clonerPickCtype(p, x, y, true);
    if (clonerHasCtype(p) && p.life == 10)
    {
        if (p.ctype == int(PT_PHOT))
            clonerEmitPhotons(i, x, y);
        else
            clonerEmit(p, x, y);
    }
    return 0;
}

// PBCN.cpp: powered breakable clone.
int updatePBCN(uint i, inout Particle p, int x, int y)
{
    uint ci = airIndex(x, y);
    if (p.tmp2 == 0 && airPV(ci) > 4.0)
        p.tmp2 = rngBetween(80, 119);
    if (p.tmp2 != 0)
    {
        p.vx += 0.1 * airVX(ci);
        p.vy += 0.1 * airVY(ci);
        p.tmp2--;
        if (p.tmp2 == 0)
        {
            killPart(i, x, y);
            return 1;
        }
    }
    if (!clonerHasCtype(p))
        clonerPickCtype(p, x, y, true);
    if (p.life != 10)
    {
        if (p.life > 0)
            p.life--;
    }
    else
    {
        for (int rx = -2; rx <= 2; rx++)
        {
            for (int ry = -2; ry <= 2; ry++)
            {
                if (rx == 0 && ry == 0)
                    continue;
                int rid = pmapId(pmap[cellIndex(x + rx, y + ry)]);
                if (rid < 0 || ptype[rid] != PT_PBCN)
                    continue;
                int rl = parts[rid].life;
                if (rl < 10 && rl > 0)
                    p.life = 9;
                else if (rl == 0)
                    parts[rid].life = 10;
            }
        }
        if (clonerHasCtype(p))
        {
            if (p.ctype == int(PT_PHOT))
                clonerEmitPhotons(i, x, y);
            else
                clonerEmit(p, x, y);
        }
    }
    return 0;
}

// Powered elements switched by sparks like PCLN (PVOD) or passing their state on like PUMP (HSWC, PUMP).
void poweredToggle(inout Particle p, int x, int y, uint self)
{
    if (p.life > 0 && p.life != 10)
        p.life--;
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
            Particle r = parts[rid];
            if (rt == PT_SPRK)
            {
                if (r.life > 0 && r.life < 4)
                {
                    if (r.ctype == int(PT_PSCN))
                        p.life = 10;
                    else if (r.ctype == int(PT_NSCN))
                        p.life = 9;
                }
            }
            else if (rt == self)
            {
                if (p.life == 10 && r.life < 10 && r.life > 0)
                    p.life = 9;
                else if (p.life == 0 && r.life == 10)
                    p.life = 10;
            }
        }
    }
}

// PVOD.cpp: powered void (eats while on; see evalMoveType).
int updatePVOD(uint i, inout Particle p, int x, int y)
{
    poweredToggle(p, x, y, PT_PVOD);
    return 0;
}

// The PUMP/HSWC/GPMP way of passing "on" along: an on particle switches on neighbours that are off and
// turns off if a neighbour is turning off. (SPRK switches them, see updateSPRK.)
void passOnState(inout Particle p, int x, int y, uint self)
{
    for (int rx = -2; rx <= 2; rx++)
    {
        for (int ry = -2; ry <= 2; ry++)
        {
            if (rx == 0 && ry == 0)
                continue;
            int rid = pmapId(pmap[cellIndex(x + rx, y + ry)]);
            if (rid < 0 || ptype[rid] != self)
                continue;
            int rl = parts[rid].life;
            if (rl < 10 && rl > 0)
                p.life = 9;
            else if (rl == 0)
                parts[rid].life = 10;
        }
    }
}

// PUMP.cpp: while on, pulls the pressure in its cell and the four next to it towards its temperature - 273.15.
// (Setting pressure from FILT, tmp 1, waits for FILT.)
int updatePUMP(uint i, inout Particle p, int x, int y)
{
    if (p.life != 10)
    {
        if (p.life > 0)
            p.life--;
        return 0;
    }
    p.temp = clamp(p.temp, MIN_PRESSURE + 273.15, MAX_PRESSURE + 273.15);
    if (p.tmp != 1)
    {
        int cx = x / CELL, cy = y / CELL;
        float target = p.temp - 273.15;
        airNudgePV(cx, cy - 1, target, 0.1, false);
        airNudgePV(cx - 1, cy, target, 0.1, false);
        airNudgePV(cx, cy, target, 0.1, false);
        airNudgePV(cx + 1, cy, target, 0.1, false);
        airNudgePV(cx, cy + 1, target, 0.1, false);
    }
    else
    {
        // Pressure from FILT next to it
        for (int rx = -1; rx <= 1; rx++)
        {
            for (int ry = -1; ry <= 1; ry++)
            {
                int fid = pmapId(pmap[cellIndex(x + rx, y + ry)]);
                if (fid < 0 || ptype[fid] != PT_FILT)
                    continue;
                int newPressure = parts[fid].ctype - 0x10000000;
                if (newPressure >= 0 && float(newPressure) <= MAX_PRESSURE - MIN_PRESSURE)
                    airSet(AIR_PV, airIndex(x + rx, y + ry), float(newPressure) + MIN_PRESSURE);
            }
        }
    }
    passOnState(p, x, y, PT_PUMP);
    return 0;
}

// HSWC.cpp: heat switch, conducts heat only while on (see isHeatInsulator); tmp 1 takes its temperature from FILT.
int updateHSWC(uint i, inout Particle p, int x, int y)
{
    if (p.life != 10)
    {
        if (p.life > 0)
            p.life--;
        return 0;
    }
    passOnState(p, x, y, PT_HSWC);
    if (p.tmp == 1)
    {
        for (int rx = -1; rx <= 1; rx++)
        {
            for (int ry = -1; ry <= 1; ry++)
            {
                if (rx == 0 && ry == 0)
                    continue;
                int fid = pmapId(pmap[cellIndex(x + rx, y + ry)]);
                if (fid < 0 || ptype[fid] != PT_FILT)
                    continue;
                int newTemp = parts[fid].ctype - 0x10000000;
                if (float(newTemp) >= MIN_TEMP && float(newTemp) <= MAX_TEMP)
                    p.temp = float(newTemp);
            }
        }
    }
    return 0;
}

// DLAY.cpp: delay; a PSCN spark starts a countdown of temp - 273.15 ticks, and NSCN next to it is sparked when it
// runs out. DLAY next to each other share the countdown.
int updateDLAY(uint i, inout Particle p, int x, int y)
{
    int oldl = p.life;
    if (p.life > 0)
        p.life--;
    if (p.temp <= 1.0 + 273.15)
        p.temp = 1.0 + 273.15;
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
            if (rt == PT_NONE || insulated(partsAvgType(parts[rid], p)))
                continue;
            Particle r = parts[rid];
            if (rt == PT_SPRK && p.life == 0 && r.life > 0 && r.life < 4 && r.ctype == int(PT_PSCN))
                p.life = int(p.temp - 273.15 + 0.5);
            else if (rt == PT_DLAY)
            {
                // TPT adjusts by one for neighbours whose update hasn't run yet (higher id)
                if (p.life == 0)
                {
                    if (r.life != 0)
                    {
                        p.life = r.life;
                        if (uint(rid) > i)
                            p.life--;
                    }
                }
                else if (r.life == 0)
                {
                    parts[rid].life = p.life;
                    if (uint(rid) > i)
                        parts[rid].life++;
                }
            }
            else if (rt == PT_NSCN && oldl == 1)
                createPart(x + rx, y + ry, PT_SPRK);
        }
    }
    return 0;
}

// CONV.cpp: converter; turns what touches it into its ctype (optionally only, or all but, tmp's element).
int updateCONV(uint i, inout Particle p, int x, int y)
{
    int ctype = p.ctype & 0x1FF;
    if (ctype <= 0 || ctype >= MAX_ELEMENTS || elems[ctype].existsInTpt == 0 || ctype == int(PT_CONV))
    {
        for (int rx = -1; rx <= 1; rx++)
        {
            for (int ry = -1; ry <= 1; ry++)
            {
                int rid = photonsThenPmap(x + rx, y + ry);
                if (rid < 0)
                    continue;
                uint rt = ptype[rid];
                if (rt == PT_NONE || rt == PT_CLNE || rt == PT_PCLN || rt == PT_BCLN || rt == PT_STKM ||
                    rt == PT_PBCN || rt == PT_STKM2 || rt == PT_CONV)
                    continue;
                p.ctype = int(rt);
                if (rt == PT_LIFE)
                    p.ctype |= parts[rid].ctype << 9;
            }
        }
        return 0;
    }
    int restrictElement = (p.tmp > 0 && p.tmp < MAX_ELEMENTS && elems[p.tmp].existsInTpt != 0) ? p.tmp : 0;
    for (int rx = -1; rx <= 1; rx++)
    {
        for (int ry = -1; ry <= 1; ry++)
        {
            if (!inSim(x + rx, y + ry))
                continue;
            // An energy particle there first, unless the restriction rules it out; then pmap's
            uint c = cellIndex(x + rx, y + ry);
            int rid = pmapId(photons[c]);
            uint rt = rid < 0 ? PT_NONE : ptype[rid];
            if (rt == PT_NONE || (restrictElement != 0 && ((int(rt) == restrictElement) == (p.tmp2 == 1))))
            {
                rid = pmapId(pmap[c]);
                rt = rid < 0 ? PT_NONE : ptype[rid];
            }
            if (rt == PT_NONE || (restrictElement != 0 && ((int(rt) == restrictElement) == (p.tmp2 == 1))))
                continue;
            if (rt != PT_CONV && rt != PT_DMND && int(rt) != ctype)
                createPartAt(uint(rid), x + rx, y + ry, uint(ctype));
        }
    }
    return 0;
}

// SHLD1-4: shields grow layers around sparks and get stronger.
int updateSHLD(uint i, inout Particle p, uint t, int x, int y)
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
                if (t == PT_SHLD2)
                {
                    if (p.life > 0)
                        createPart(x + rx, y + ry, PT_SHLD1);
                }
                else if (t == PT_SHLD3 || t == PT_SHLD4)
                {
                    if (rngChance(1, t == PT_SHLD3 ? 2500 : 5500))
                    {
                        int np = createPart(x + rx, y + ry, PT_SHLD1);
                        if (np < 0)
                            continue;
                        parts[np].life = 7;
                        partChangeType(i, PT_SHLD2);
                    }
                }
                continue;
            }
            if (t == PT_SHLD3 && rt == PT_SHLD1 && p.life > 3)
            {
                partChangeType(uint(rid), PT_SHLD2);
                parts[rid].life = 7;
            }
            else if (t == PT_SHLD4 && rt == PT_SHLD2 && p.life > 3)
            {
                partChangeType(uint(rid), PT_SHLD3);
                parts[rid].life = 7;
            }
            else if (rt == PT_SPRK && p.life == 0)
            {
                // Grow stronger, and grow a layer of SHLD1 around the spark
                if (t == PT_SHLD1 && rngChance(11, 40))
                {
                    partChangeType(i, PT_SHLD2);
                    p.life = 7;
                }
                else if (t == PT_SHLD2 && rngChance(1, 8))
                {
                    partChangeType(i, PT_SHLD3);
                    p.life = 7;
                }
                else if (t == PT_SHLD3 && rngChance(3, 500))
                {
                    partChangeType(i, PT_SHLD4);
                    p.life = 7;
                }
                for (int nnx = -1; nnx <= 1; nnx++)
                {
                    for (int nny = -1; nny <= 1; nny++)
                    {
                        if (typeAt(x + rx + nnx, y + ry + nny) != PT_NONE)
                            continue;
                        int np = createPart(x + rx + nnx, y + ry + nny, PT_SHLD1);
                        if (np >= 0 && t != PT_SHLD1)
                            parts[np].life = 7;
                    }
                }
            }
            else if (t == PT_SHLD1 && rt == PT_SHLD3 && rngChance(2, 5))
            {
                partChangeType(i, PT_SHLD2);
                p.life = 7;
            }
            else if (t == PT_SHLD2 && rt == PT_SHLD4 && rngChance(2, 5))
            {
                partChangeType(i, PT_SHLD3);
                p.life = 7;
            }
        }
    }
    return 0;
}

// PHOT.cpp: light. Dies without wavelengths; hot photons can set things alight; QRTZ scatters and splits the
// spectrum, BGLA scatters a little, RSST hardens into its ctype (or RSSS) where light hits it.
int updatePHOT(uint i, inout Particle p, int x, int y, int surroundSpace)
{
    if ((p.ctype & 0x3FFFFFFF) == 0)
    {
        killPart(i, x, y);
        return 1;
    }
    if (p.temp > 506.0 && rngChance(1, 10))
        updateFIRE(i, p, PT_PHOT, x, y, surroundSpace);
    for (int rx = -1; rx <= 1; rx++)
    {
        for (int ry = -1; ry <= 1; ry++)
        {
            int rid = pmapId(pmap[cellIndex(x + rx, y + ry)]);
            if (rid < 0)
                continue;
            uint rt = ptype[rid];
            if (rt == PT_NONE)
                continue;
            bool here = rx == 0 && ry == 0;
            if (rt == PT_ISOZ || rt == PT_ISZS)
            {
                // PHOT + ISOZ/ISZS -> 2xPHOT
                if (rngChance(1, 400) && gpuHas(rt))
                {
                    p.vx *= 0.90;
                    p.vy *= 0.90;
                    createPartAt(uint(rid), x + rx, y + ry, PT_PHOT);
                    float a = float(rngBetween(0, 359)) * 3.14159265 / 180.0;
                    int rr = rt == PT_ISOZ ? int(float(rngBetween(128, 255)) / 127.0) : int(float(rngBetween(128, 355)) / 127.0);
                    parts[rid].vx = float(rr) * cos(a);
                    parts[rid].vy = float(rr) * sin(a);
                    uint ci = airIndex(x, y);
                    setAirPV(ci, airPV(ci) - 15.0 * CFDS);
                }
            }
            else if ((rt == PT_QRTZ || rt == PT_PQRT) && here)
            {
                float a = float(rngBetween(0, 359)) * 3.14159265 / 180.0;
                p.vx = 3.0 * cos(a);
                p.vy = 3.0 * sin(a);
                if (p.ctype == 0x3FFFFFFF)
                    p.ctype = 0x1F << rngBetween(0, 25);
                if (p.life != 0)
                    p.life++; // delay death
            }
            else if (rt == PT_BGLA && here)
            {
                float a = float(rngBetween(-50, 50)) * 0.001;
                float cx = cos(a), sy = sin(a);
                float vx = cx * p.vx + sy * p.vy;
                float vy = cx * p.vy - sy * p.vx;
                p.vx = vx;
                p.vy = vy;
            }
            else if (rt == PT_FILT && parts[rid].tmp == 9)
            {
                p.vx += float(rngBetween(-500, 500)) / 1000.0;
                p.vy += float(rngBetween(-500, 500)) / 1000.0;
            }
            else if (rt == PT_RSST && here)
            {
                int ctUnder = parts[rid].ctype;
                int tmpUnder = parts[rid].tmp;
                if (ctUnder > 0 && ctUnder < MAX_ELEMENTS)
                {
                    // PHOT + RSST(ctype) -> ctype
                    createPartAt(uint(rid), x, y, uint(ctUnder));
                    if (tmpUnder > 0 && tmpUnder < MAX_ELEMENTS && (elems[ctUnder].carriesTypeIn & (1u << 2)) != 0)
                        parts[rid].ctype = tmpUnder;
                }
                else
                    partChangeType(uint(rid), PT_RSSS); // PHOT + RSST -> RSSS
                killPart(i, x, y);
                return 1;
            }
        }
    }
    return 0;
}

// NEUT.cpp: neutrons. Fission in PLUT (more neutrons, or URAN and molten PLUT), fusion in DEUT, and a list of
// transmutations of what they pass through.
int updateNEUT(uint i, inout Particle p, int x, int y, int surroundSpace)
{
    int pressureFactor = 3 + int(airPV(airIndex(x, y)));
    for (int rx = -1; rx <= 1; rx++)
    {
        for (int ry = -1; ry <= 1; ry++)
        {
            int rid = pmapId(pmap[cellIndex(x + rx, y + ry)]);
            if (rid < 0)
                continue;
            uint rt = ptype[rid];
            int rxx = x + rx, ryy = y + ry;
            if (rt == PT_WATR || rt == PT_ICEI || rt == PT_SNOW)
            {
                // NEUT + WATR -> NEUT + DSTW (falls through to slowing down, like ICE and SNOW)
                if (rt == PT_WATR && rngChance(3, 20))
                    partChangeType(uint(rid), PT_DSTW);
                p.vx *= 0.995;
                p.vy *= 0.995;
            }
            else if (rt == PT_PLUT)
            {
                if (rngChance(pressureFactor, 1000))
                {
                    if (rngChance(1, 3))
                    {
                        // NEUT + PLUT -> NEUT + URAN/LAVA(PLUT)
                        uint nt = rngChance(2, 3) ? PT_LAVA : PT_URAN;
                        createPartAt(uint(rid), rxx, ryy, nt);
                        parts[rid].temp = MAX_TEMP;
                        if (ptype[rid] == PT_LAVA)
                        {
                            parts[rid].tmp = 100;
                            parts[rid].ctype = int(PT_PLUT);
                        }
                    }
                    else
                    {
                        // NEUT + PLUT -> 2xNEUT
                        createPartAt(uint(rid), rxx, ryy, PT_NEUT);
                        parts[rid].vx = 0.25 * parts[rid].vx + p.vx;
                        parts[rid].vy = 0.25 * parts[rid].vy + p.vy;
                    }
                    uint ci = airIndex(x, y);
                    setAirPV(ci, airPV(ci) + 10.0 * CFDS);
                    updateFIRE(i, p, PT_NEUT, x, y, surroundSpace);
                }
            }
            else if (rt == PT_DEUT)
            {
                if (rngChance(pressureFactor + 1 + parts[rid].life / 100, 1000))
                {
                    // NEUT + DEUT -> 2xNEUT
                    deutBurst(parts[rid].life, rxx, ryy,
                              restrictFlt(parts[rid].temp + float(parts[rid].life) * 500.0, MIN_TEMP, MAX_TEMP), PT_NEUT, 1.0);
                    killPart(uint(rid), rxx, ryy);
                }
            }
            else if (rt == PT_GUNP)
            {
                if (rngChance(3, 200))
                    partChangeType(uint(rid), PT_DUST);
            }
            else if (rt == PT_DYST)
            {
                if (rngChance(3, 200))
                    partChangeType(uint(rid), PT_YEST);
            }
            else if (rt == PT_YEST)
                partChangeType(uint(rid), PT_DYST);
            else if (rt == PT_PLEX)
            {
                if (rngChance(3, 200))
                    partChangeType(uint(rid), PT_GOO);
            }
            else if (rt == PT_NITR)
            {
                if (rngChance(3, 200))
                    partChangeType(uint(rid), PT_DESL);
            }
            else if (rt == PT_PLNT || rt == PT_COAL)
            {
                if (rngChance(1, 20))
                    createPartAt(uint(rid), rxx, ryy, PT_WOOD);
            }
            else if (rt == PT_DESL || rt == PT_OIL)
            {
                if (rngChance(3, 200))
                    partChangeType(uint(rid), PT_GAS);
            }
            else if (rt == PT_BCOL)
            {
                if (rngChance(1, 20))
                    createPartAt(uint(rid), rxx, ryy, PT_SAWD);
            }
            else if (rt == PT_DUST)
            {
                if (rngChance(1, 20))
                    partChangeType(uint(rid), PT_FWRK);
            }
            else if (rt == PT_FWRK)
            {
                if (rngChance(1, 20))
                    parts[rid].ctype = int(PT_DUST);
            }
            else if (rt == PT_ACID)
            {
                if (rngChance(1, 20))
                    createPartAt(uint(rid), rxx, ryy, PT_ISOZ);
            }
            else if (rt == PT_TTAN)
            {
                if (rngChance(1, 20))
                {
                    killPart(i, x, y);
                    return 1;
                }
            }
            else if (rt == PT_EXOT)
            {
                if (rngChance(1, 20))
                    parts[rid].life = 1500;
            }
            else if (rt == PT_RFRG)
                createPartAt(uint(rid), rxx, ryy, rngChance(1, 2) ? PT_GAS : PT_CAUS);
            else if (rt == PT_RSSS)
            {
                if (rx == 0 && ry == 0)
                {
                    int ctUnder = parts[rid].ctype, tmpUnder = parts[rid].tmp;
                    if (ctUnder > 0 && ctUnder < MAX_ELEMENTS)
                    {
                        // NEUT + RSSS(ctype) -> ctype
                        createPartAt(uint(rid), x, y, uint(ctUnder));
                        if (tmpUnder > 0 && tmpUnder < MAX_ELEMENTS && (elems[ctUnder].carriesTypeIn & (1u << 2)) != 0)
                            parts[rid].ctype = tmpUnder;
                    }
                    else
                        partChangeType(uint(rid), PT_RSST); // NEUT + RSSS -> RSST
                    killPart(i, x, y);
                    return 1;
                }
            }
            else if (rt == PT_BASE)
            {
                if (parts[rid].temp > (50.0 + 273.15) && rngChance(1, 35))
                    createPartAt(uint(rid), rxx, ryy, PT_LRBD);
            }
            else if (rt == PT_SEED)
            {
                if (rx == 0 && ry == 0)
                {
                    // Flip a random gene with 1/2 chance
                    switch (rngBetween(0, 9))
                    {
                    case 0:
                        parts[rid].ctype ^= 1 << rngBetween(PLNT_COLOUR, PLNT_LIFE - 1);
                        break;
                    case 1:
                        parts[rid].tmp ^= 1 << rngBetween(0, PLNT_TOTAL_TMP - 1);
                        break;
                    case 2:
                        parts[rid].tmp2 ^= 1 << rngBetween(0, PLNT_TOTAL_TMP - 1);
                        break;
                    case 3:
                        parts[rid].tmp3 ^= 1 << rngBetween(0, PLNT_TOTAL_TMP - 1);
                        break;
                    case 4:
                        parts[rid].tmp4 ^= 1 << rngBetween(0, PLNT_TOTAL_TMP - 1);
                        break;
                    default:
                        break;
                    }
                }
            }
        }
    }
    return 0;
}

// DeutExplosion / DeutImplosion: n/50 (1 to 340) energy particles of type t burst out of one cell.
void deutBurst(int n, int x, int y, float temp, uint t, float pressureSign)
{
    n = clamp(n / 50, 1, 340);
    for (int c = 0; c < n; c++)
    {
        int j = createPartStacked(x, y, t);
        if (j >= 0)
            parts[j].temp = temp;
        else
            break;
    }
    uint ci = airIndex(x, y);
    setAirPV(ci, airPV(ci) + pressureSign * (6.0 * CFDS) * float(n));
}

// ELEC.cpp: electrons. Shatter glass into embers, split water into O2/H2, make H2 with protons or neutrons,
// charge DEUT, and spark conductors.
int updateELEC(uint i, inout Particle p, int x, int y)
{
    for (int rx = -2; rx <= 2; rx++)
    {
        for (int ry = -2; ry <= 2; ry++)
        {
            int rid = photonsThenPmapReverse(x + rx, y + ry);
            if (rid < 0)
                continue;
            uint rt = ptype[rid];
            if (rt == PT_NONE)
                continue;
            int rxx = x + rx, ryy = y + ry;
            if (rt == PT_GLAS)
            {
                // ELEC + GLAS -> EMBR + GLAS
                for (int rrx = -1; rrx <= 1; rrx++)
                {
                    for (int rry = -1; rry <= 1; rry++)
                    {
                        if (!inSim(rxx + rrx, ryy + rry))
                            continue;
                        int nb = createPart(rxx + rrx, ryy + rry, PT_EMBR);
                        if (nb != -1)
                        {
                            parts[nb].tmp = 0;
                            parts[nb].life = 50;
                            parts[nb].temp = p.temp * 0.8;
                            parts[nb].vx = float(rngBetween(-10, 10));
                            parts[nb].vy = float(rngBetween(-10, 10));
                        }
                    }
                }
                killPart(i, x, y);
                return 1;
            }
            else if (rt == PT_LCRY)
                parts[rid].tmp2 = rngBetween(5, 9);
            else if (rt == PT_WATR || rt == PT_DSTW || rt == PT_SLTW || rt == PT_CBNW)
            {
                // ELEC + WATR/DSTW/SLTW/CBNW -> O2/H2
                createPartAt(uint(rid), rxx, ryy, rngChance(1, 3) ? PT_O2 : PT_H2);
                killPart(i, x, y);
                return 1;
            }
            else if ((rt == PT_PROT && (parts[rid].tmp2 & 0x1) == 0) || rt == PT_NEUT)
            {
                // ELEC + PROT/NEUT -> H2 (TPT carries on through the loop after this)
                partChangeType(uint(rid), PT_H2);
                parts[rid].life = 0;
                parts[rid].ctype = 0;
                killPart(i, x, y);
            }
            else if (rt == PT_DEUT)
            {
                if (parts[rid].life < 6000)
                    parts[rid].life += 1;
                parts[rid].temp = 0.0;
                killPart(i, x, y);
                return 1;
            }
            else if (rt == PT_EXOT)
            {
                parts[rid].tmp2 += 5;
                parts[rid].life = 1000;
            }
            else if (rt == PT_RSST)
            {
                if (rx == 0 && ry == 0)
                {
                    killPart(uint(rid), x, y);
                    killPart(i, x, y);
                    return 1;
                }
            }
            else if (rt != PT_PROT && (elems[rt].properties & PROP_CONDUCTS) != 0 &&
                     (rt != PT_NBLE || p.temp < 2273.15))
            {
                createPart(rxx, ryy, PT_SPRK);
                killPart(i, x, y);
                return 1;
            }
        }
    }
    return 0;
}

// PROT.cpp: protons. Interact with what they are inside, set off hot explosives, and protons colliding head on
// at speed fuse into heavier elements.
int updatePROT(uint i, inout Particle p, int x, int y)
{
    uint ci = airIndex(x, y);
    setAirPV(ci, airPV(ci) - 0.003);
    int uID = pmapId(pmap[cellIndex(x, y)]);
    uint utype = uID < 0 ? PT_NONE : ptype[uID];
    if (utype == PT_SPRK)
    {
        // Remove active sparks
        int sparked = parts[uID].ctype;
        if (sparked > 0 && sparked < MAX_ELEMENTS && gpuHas(uint(sparked)))
        {
            partChangeType(uint(uID), uint(sparked));
            parts[uID].life = 44 + parts[uID].life;
            parts[uID].ctype = 0;
        }
        else
        {
            killPart(uint(uID), x, y);
            utype = PT_NONE;
        }
    }
    else if (utype == PT_DEUT)
    {
        if (rngChance(-(int(airPV(ci)) - 4) + parts[uID].life / 100, 200))
        {
            deutBurst(parts[uID].life, x, y,
                      restrictFlt(parts[uID].temp + float(parts[uID].life * 500), MIN_TEMP, MAX_TEMP), PT_PROT, -1.0);
            killPart(uint(uID), x, y);
        }
    }
    else if (utype == PT_LCRY)
    {
        // Powered LCRY: PROT -> PHOT
        if (parts[uID].life > 5 && rngChance(1, 10))
        {
            partChangeType(i, PT_PHOT);
            p.life *= 2;
            p.ctype = 0x3FFFFFFF;
        }
    }
    else if (utype == PT_EXOT)
        parts[uID].ctype = int(PT_PROT);
    else if (utype == PT_WIFI)
    {
        float change = p.temp < 173.15 ? -1000.0 : p.temp < 273.15 ? -100.0 : p.temp > 473.15 ? 1000.0 :
                       p.temp > 373.15 ? 100.0 : 0.0;
        parts[uID].temp = restrictFlt(parts[uID].temp + change, MIN_TEMP, MAX_TEMP);
    }
    else if (utype == PT_RSSS)
    {
        killPart(uint(uID), x, y);
        killPart(i, x, y);
        return 1;
    }
    else if (utype == PT_SEED)
    {
        // Mutate seeds: swap the genes of two phases
        if (rngChance(1, 2))
        {
            int j = rngBetween(0, 3), k = rngBetween(0, 3);
            Particle q = parts[uID];
            int tmps[4] = { q.tmp, q.tmp2, q.tmp3, q.tmp4 };
            int a = tmps[j];
            tmps[j] = tmps[k];
            tmps[k] = a;
            parts[uID].tmp = tmps[0];
            parts[uID].tmp2 = tmps[1];
            parts[uID].tmp3 = tmps[2];
            parts[uID].tmp4 = tmps[3];
        }
    }
    else if (utype == PT_NONE)
    {
        // Slowly dies outside material
        if (p.life != 0)
        {
            p.life--;
            if (p.life == 0)
            {
                killPart(i, x, y);
                return 1;
            }
        }
    }
    else
    {
        ElementProps ue = elems[utype];
        if (p.temp > 273.15 + 500.0 && (ue.flammable != 0 || ue.explosive != 0 || utype == PT_BANG))
        {
            // Set off explosives (only when hot)
            createPartAt(uint(uID), x, y, PT_FIRE);
            parts[uID].temp += restrictFlt(float(ue.flammable * 5), MIN_TEMP, MAX_TEMP);
            setAirPV(ci, airPV(ci) + 1.0);
        }
        else if ((ue.properties & PROP_CONDUCTS) != 0 && parts[uID].life <= 4)
            parts[uID].life = 40 + parts[uID].life; // keep it from being sparked
    }
    // Bring what it is in towards its own temperature
    if (utype != PT_NONE && utype != PT_WIFI && uID >= 0 && ptype[uID] != PT_NONE)
        parts[uID].temp = restrictFlt(parts[uID].temp - (parts[uID].temp - p.temp) / 4.0, MIN_TEMP, MAX_TEMP);

    // Collided with another proton last tick: become a heavier element
    if (p.tmp != 0)
    {
        uint element;
        if (p.tmp > 500000)
            element = utype == PT_TUNG ? PT_AMTR : PT_SING;
        else if (p.tmp > 700)
            element = PT_PLUT;
        else if (p.tmp > 420)
            element = PT_URAN;
        else if (p.tmp > 310)
            element = PT_POLO;
        else if (p.tmp > 250)
            element = PT_PLSM;
        else if (p.tmp > 100)
            element = PT_O2;
        else if (p.tmp > 50)
            element = PT_CO2;
        else
            element = PT_NBLE;
        int nx = x + rngBetween(-1, 1), ny = y + rngBetween(-1, 1);
        int newID = createPart(nx, ny, element);
        if (newID >= 0)
            parts[newID].temp = restrictFlt(100.0 * float(p.tmp), MIN_TEMP, MAX_TEMP);
        killPart(i, x, y);
        return 1;
    }
    // Collide with other protons
    int aid = pmapId(photons[cellIndex(x, y)]);
    if (aid >= 0 && uint(aid) != i && ptype[aid] == PT_PROT)
    {
        Particle a = parts[aid];
        float v1 = p.vx * p.vx + p.vy * p.vy;
        float v2 = a.vx * a.vx + a.vy * a.vy;
        float d1 = atan2(-p.vy, p.vx);
        float d2 = atan2(-a.vy, a.vx);
        float difference = d1 - d2;
        if (difference < 0.0)
            difference += 6.28319;
        if (difference > 3.12659 && difference < 3.15659 && v1 + v2 > 10.0)
        {
            parts[aid].tmp += int(v1 + v2);
            killPart(i, x, y);
            return 1;
        }
    }
    return 0;
}

// GRVT.cpp: gravity particles. (Their pull on the Newtonian gravity field comes with Phase 4.)
int updateGRVT(uint i, inout Particle p, int x, int y)
{
    p.tmp = clamp(p.tmp, -100, 100);
    if (typeAt(x, y) == PT_RSSS && rngChance(1, 5))
    {
        killPart(i, x, y);
        return 1;
    }
    return 0;
}

// PLUT.cpp: plutonium decays into neutrons, faster under pressure.
int updatePLUT(uint i, inout Particle p, int x, int y)
{
    if (rngChance(1, 100) && rngChance(int(5.0 * airPV(airIndex(x, y))), 1000))
        createSelf(i, p, x, y, PT_NEUT); // PLUT -> NEUT
    return 0;
}

// URAN.cpp: uranium heats up under pressure.
int updateURAN(uint i, inout Particle p, int x, int y)
{
    float pv = airPV(airIndex(x, y));
    if (params[0].legacyHeat == 0 && pv > 0.0)
    {
        if (p.temp == MIN_TEMP)
            p.temp += 0.01;
        else
            p.temp = restrictFlt(p.temp * (1.0 + pv / 2000.0) + MIN_TEMP, MIN_TEMP, MAX_TEMP);
    }
    return 0;
}

// DEUT.cpp: heavy water condenses into fewer particles when cold (life counts the absorbed ones) and expands
// when hot. (Newtonian gravity's compression comes with Phase 4.)
int updateDEUT(uint i, inout Particle p, int x, int y)
{
    float temp = max(1.0, p.temp + 1.0);
    int t1 = int(temp + 1.0);
    int maxlife = int((10000.0 / (temp + 1.0)) - 1.0);
    if (rngChance(10000 % t1, t1))
        maxlife++;
    maxlife = maxlife * int(5.0 - 8.0 / 2.0); // gravtot = 0
    if (p.life < maxlife)
    {
        for (int rx = -1; rx <= 1; rx++)
        {
            for (int ry = -1; ry <= 1; ry++)
            {
                if (rx == 0 && ry == 0)
                    continue;
                int rid = pmapId(pmap[cellIndex(x + rx, y + ry)]);
                if (rid < 0 || p.life >= maxlife)
                    continue;
                if (ptype[rid] == PT_DEUT && rngChance(1, 3) && parts[rid].life <= maxlife - p.life - 1)
                {
                    p.life += parts[rid].life + 1;
                    killPart(uint(rid), x + rx, y + ry);
                }
            }
        }
    }
    else
    {
        bool done = false;
        for (int rx = -1; rx <= 1 && !done; rx++)
        {
            for (int ry = -1; ry <= 1 && !done; ry++)
            {
                if (rx == 0 && ry == 0)
                    continue;
                if (p.life <= maxlife)
                {
                    done = true;
                    continue;
                }
                if (typeAt(x + rx, y + ry) == PT_NONE && p.life >= 1)
                {
                    int np = createPart(x + rx, y + ry, PT_DEUT);
                    if (np < 0)
                        continue;
                    p.life--;
                    parts[np].temp = p.temp;
                    parts[np].life = 0;
                }
            }
        }
    }
    for (int trade = 0; trade < 4; trade++)
    {
        int rx = rngBetween(-2, 2);
        int ry = rngBetween(-2, 2);
        if (rx == 0 && ry == 0)
            continue;
        int rid = pmapId(pmap[cellIndex(x + rx, y + ry)]);
        if (rid < 0 || ptype[rid] != PT_DEUT || !(p.life > parts[rid].life) || p.life <= 0)
            continue;
        int d = p.life - parts[rid].life;
        if (d == 1)
        {
            parts[rid].life++;
            p.life--;
        }
        else if (d > 0)
        {
            parts[rid].life += d / 2;
            p.life -= d / 2;
        }
    }
    return 0;
}

// ISOZ.cpp / ISZS.cpp: isotope Z gives off photons under negative pressure.
int updateISOZ(uint i, inout Particle p, int x, int y)
{
    if (rngChance(1, 200) && rngChance(int(-4.0 * airPV(airIndex(x, y))), 1000))
    {
        // ISOZ/ISZS -> PHOT
        createSelf(i, p, x, y, PT_PHOT);
        float rr = float(rngBetween(128, 355)) / 127.0;
        float rrr = float(rngBetween(0, 359)) * 3.14159265 / 180.0;
        p.vx = rr * cos(rrr);
        p.vy = rr * sin(rrr);
    }
    return 0;
}

// POLO.cpp: polonium gives off neutrons (a few times, with a cooldown) and turns into PLUT after absorbing
// enough protons.
int updatePOLO(uint i, inout Particle p, int x, int y)
{
    const int COOLDOWN = 15, LIMIT = 5;
    int rid = pmapId(photons[cellIndex(x, y)]);
    if (rid >= 0 && ptype[rid] == PT_NONE)
        rid = -1;
    if (p.tmp < LIMIT && p.life == 0)
    {
        if (rngChance(1, 10000) && p.tmp == 0)
        {
            // POLO -> POLO + NEUT
            int sid = createPartStacked(x, y, PT_NEUT);
            if (sid >= 0)
            {
                p.life = COOLDOWN;
                p.tmp++;
                p.temp = ((p.temp + parts[sid].temp) + 600.0) / 2.0;
                parts[sid].temp = p.temp;
            }
        }
        if (rid >= 0 && rngChance(1, 100))
        {
            int sid = createPartStacked(x, y, PT_NEUT);
            if (sid >= 0)
            {
                p.temp = ((p.temp + parts[rid].temp + parts[rid].temp) + 600.0) / 3.0;
                p.life = COOLDOWN;
                p.tmp++;
                parts[rid].temp = p.temp;
                parts[sid].temp = p.temp;
                parts[sid].vx = parts[rid].vx;
                parts[sid].vy = parts[rid].vy;
            }
        }
    }
    if (p.tmp2 >= 10)
    {
        // POLO -> PLUT
        partChangeType(i, PT_PLUT);
        p.temp = (p.temp + 600.0) / 2.0;
        return 1;
    }
    if (rid >= 0 && ptype[rid] == PT_PROT)
    {
        p.tmp2++;
        killPart(uint(rid), x, y);
    }
    if (p.temp < 388.15)
        p.temp += 0.2;
    return 0;
}

// AMTR.cpp: antimatter annihilates what it touches (mostly; a tenth become photons), and dies after a few.
int updateAMTR(uint i, inout Particle p, int x, int y)
{
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
            if (rt == PT_NONE || rt == PT_AMTR || rt == PT_DMND || rt == PT_CLNE || rt == PT_PCLN || rt == PT_VOID ||
                rt == PT_BHOL || rt == PT_NBHL || rt == PT_PRTI || rt == PT_PRTO)
                continue;
            p.life++;
            if (p.life == 4)
            {
                killPart(i, x, y);
                return 1;
            }
            // AMTR + anything -> AMTR + PHOT
            if (rngChance(1, 10))
                createPartAt(uint(rid), x + rx, y + ry, PT_PHOT);
            else
                killPart(uint(rid), x + rx, y + ry);
            uint ci = airIndex(x, y);
            setAirPV(ci, airPV(ci) - 2.0);
        }
    }
    return 0;
}

// SING.cpp: singularity. Sucks the pressure down and swallows what touches it, then pops into energy particles.
int updateSING(uint i, inout Particle p, int x, int y)
{
    float singularity = float(-p.life);
    int cx = x / CELL, cy = y / CELL;
    airNudgePV(cx, cy, singularity, 0.1, true);
    airNudgePV(cx, cy + 1, singularity, 0.1, true);
    airNudgePV(cx, cy - 1, singularity, 0.1, true);
    airNudgePV(cx + 1, cy, singularity, 0.1, false);
    airNudgePV(cx + 1, cy + 1, singularity, 0.1, false);
    airNudgePV(cx - 1, cy, singularity, 0.1, false);
    airNudgePV(cx - 1, cy - 1, singularity, 0.1, false);
    if (p.life < 1)
    {
        // Pop!
        for (int rx = -1; rx <= 1; rx++)
            for (int ry = -1; ry <= 1; ry++)
                airAddPVClamped(cx + rx, cy + ry, float(p.tmp));
        int n = abs(p.tmp);
        int spawncount = n > 255 ? 3019 : int(float((n / 8) * (n / 8)) * 3.14159265);
        for (int j = 0; j < spawncount; j++)
        {
            uint kind = rngGen() % 3;
            int nb = createPartStacked(x, y, kind == 0 ? PT_PHOT : kind == 1 ? PT_NEUT : PT_ELEC);
            if (nb == -1)
                break;
            parts[nb].life = rngBetween(0, 299);
            parts[nb].temp = MAX_TEMP / 2.0;
            float angle = rngUniform01() * 2.0 * 3.14159265;
            float v = rngUniform01() * 5.0;
            parts[nb].vx = v * cos(angle);
            parts[nb].vy = v * sin(angle);
        }
        killPart(i, x, y);
        return 1;
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
            if (rt == PT_NONE || rt == PT_DMND || !rngChance(1, 3))
                continue;
            if (rt == PT_SING && parts[rid].life > 10)
            {
                if (p.life + parts[rid].life > 255)
                    continue;
                p.life += parts[rid].life;
            }
            else
            {
                if (p.life + 3 > 255)
                {
                    if (rt != PT_SING && rngChance(1, 1000))
                    {
                        createPartAt(uint(rid), x + rx, y + ry, PT_SING);
                        parts[rid].life = rngBetween(60, 109);
                    }
                    continue;
                }
                p.life += 3;
                p.tmp++;
            }
            p.temp = restrictFlt(parts[rid].temp + p.temp, MIN_TEMP, MAX_TEMP);
            killPart(uint(rid), x + rx, y + ry);
        }
    }
    return 0;
}

// WARP.cpp: swaps places with what is next to it (making it hop), getting a little longer-lived each time.
int updateWARP(uint i, inout Particle p, int x, int y)
{
    if (p.tmp2 > 2000)
    {
        p.temp = 10000.0;
        uint ci = airIndex(x, y);
        setAirPV(ci, restrictFlt(airPV(ci) + float(p.tmp2 / 5000) * CFDS, MIN_PRESSURE, MAX_PRESSURE));
        if (rngChance(1, 50))
            createPartStacked(x, y, PT_ELEC); // WARP -> WARP + ELEC
    }
    for (int trade = 0; trade < 5; trade++)
    {
        int rx = rngBetween(-1, 1);
        int ry = rngBetween(-1, 1);
        if (rx == 0 && ry == 0)
            continue;
        uint dst = cellIndex(x + rx, y + ry);
        uint rEnt = pmap[dst];
        int rid = pmapId(rEnt);
        if (rid < 0 || (rEnt & PMAP_LOCK) != 0)
            continue;
        uint rt = ptype[rid];
        if (rt == PT_NONE || rt == PT_WARP || rt == PT_STKM || rt == PT_STKM2 || rt == PT_DMND || rt == PT_CLNE ||
            rt == PT_BCLN || rt == PT_PCLN)
            continue;
        // Swap places (locked, as a move is)
        uint own = cellIndex(x, y), me = pmapEnc(i), orig;
        InterlockedCompareExchange(pmap[own], me, me | PMAP_LOCK, orig);
        if (orig != me)
            break;
        InterlockedCompareExchange(pmap[dst], rEnt, me, orig);
        if (orig != rEnt)
        {
            InterlockedExchange(pmap[own], me, orig);
            break;
        }
        p.x = parts[rid].x;
        p.y = parts[rid].y;
        parts[i].x = p.x;
        parts[i].y = p.y;
        parts[rid].x = float(x);
        parts[rid].y = float(y);
        parts[rid].vx = float(rngBetween(-2, 1)) + 0.5;
        parts[rid].vy = float(rngBetween(-2, 1));
        p.life += 4;
        DeviceMemoryBarrier();
        InterlockedExchange(pmap[own], rEnt & PMAP_ID_MASK, orig);
        break;
    }
    return 0;
}

// EXOT.cpp: exotic matter. Spreads its energy (tmp2), turns into WARP when it builds up, mimics what touches it
// when charged, and with TTAN or GOLD lava makes VIBR.
int updateEXOT(uint i, inout Particle p, int x, int y)
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
            if (rt == PT_NONE)
                continue;
            if (rt == PT_WARP)
            {
                if (parts[rid].tmp2 > 2000 && rngChance(1, 100))
                    p.tmp2 += 100;
            }
            else if (rt == PT_EXOT)
            {
                if (parts[rid].ctype == int(PT_PROT))
                    p.ctype = int(PT_PROT);
                if (parts[rid].life == 1500 && rngChance(1, 1000))
                    p.life = 1500;
            }
            else if (rt == PT_LAVA)
            {
                int lc = parts[rid].ctype;
                if (lc == int(PT_TTAN) || lc == int(PT_GOLD))
                {
                    if (rngChance(1, 10))
                    {
                        // EXOT + LAVA(TTAN/GOLD) -> LAVA(VIBR)
                        parts[rid].ctype = int(PT_VIBR);
                        killPart(i, x, y);
                        return 1;
                    }
                }
                else if (lc == int(PT_VIBR))
                {
                    if (rngChance(1, 1000))
                    {
                        killPart(i, x, y);
                        return 1;
                    }
                }
            }
            if (p.tmp > 245 && p.life > 1337 && rt != PT_EXOT && rt != PT_BREC && rt != PT_DMND && rt != PT_CLNE &&
                rt != PT_PRTI && rt != PT_PRTO && rt != PT_PCLN && rt != PT_VOID && rt != PT_NBHL && rt != PT_WARP)
            {
                if (createSelf(i, p, x, y, rt))
                    return 1;
            }
        }
    }
    p.tmp--;
    p.tmp2--;
    // tmp cycles every 250 ticks (its slow flashing)
    if (p.tmp < 1 || p.tmp > 250)
        p.tmp = 250;
    uint ci = airIndex(x, y);
    if (p.tmp2 < 1)
        p.tmp2 = 1;
    else if (p.tmp2 > 6000)
    {
        p.tmp2 = 10000;
        if (p.life < 1001)
        {
            // EXOT -> WARP
            partChangeType(i, PT_WARP);
            return 1;
        }
    }
    else if (p.life < 1001)
        setAirPV(ci, airPV(ci) + (float(p.tmp2) * CFDS) / 160000.0);
    if (airPV(ci) > 200.0 && p.temp > 9000.0 && p.tmp2 > 200)
    {
        p.tmp2 = 6000;
        partChangeType(i, PT_WARP); // EXOT -> WARP
        return 1;
    }
    if (p.tmp2 > 100)
    {
        for (int trade = 0; trade < 9; trade++)
        {
            int rx = rngBetween(-2, 2);
            int ry = rngBetween(-2, 2);
            if (rx == 0 && ry == 0)
                continue;
            int rid = pmapId(pmap[cellIndex(x + rx, y + ry)]);
            if (rid < 0 || ptype[rid] != PT_EXOT || !(p.tmp2 > parts[rid].tmp2) || parts[rid].tmp2 < 0)
                continue;
            int tym = p.tmp2 - parts[rid].tmp2;
            if (tym == 1)
            {
                parts[rid].tmp2++;
                p.tmp2--;
                break;
            }
            if (tym > 0)
            {
                parts[rid].tmp2 += tym / 2;
                p.tmp2 -= tym / 2;
                break;
            }
        }
    }
    if (p.ctype == int(PT_PROT))
    {
        if (p.temp < 50.0)
        {
            // EXOT(PROT) -> CFLM
            if (createSelf(i, p, x, y, PT_CFLM))
                return 1;
        }
        else
            p.temp -= 1.0;
    }
    else if (p.temp < 273.15)
    {
        p.vx = 0.0;
        p.vy = 0.0;
        setAirPV(ci, airPV(ci) - 0.01);
        p.tmp--;
    }
    return 0;
}

static const int ORBIT_RX[8] = { -1, 0, 1, 1, 1, 0, -1, -1 };
static const int ORBIT_RY[8] = { -1, -1, -1, 0, 1, 1, 1, 0 };

// VIBR.cpp (Element_VIBR_update, also BVBR): vibranium soaks up heat and pressure (tmp) until it explodes into
// EXOT, sparks, ELEC and PHOT.
int updateVIBR(uint i, inout Particle p, uint t, int x, int y)
{
    uint rndstore = 0;
    uint ci = airIndex(x, y);
    if (p.life == 0)
    {
        // Heat absorption
        if (p.temp > 274.65)
        {
            p.tmp++;
            p.temp -= 3.0;
        }
        else if (p.temp < 271.65)
        {
            p.tmp--;
            p.temp += 3.0;
        }
        // Pressure absorption
        float pv = airPV(ci);
        if (pv > 2.5)
        {
            p.tmp += 7;
            setAirPV(ci, pv - 1.0);
        }
        else if (pv < -2.5)
        {
            p.tmp -= 2;
            setAirPV(ci, pv + 1.0);
        }
        if (p.tmp > 1000)
            p.life = 750; // start the explosion countdown
    }
    else
    {
        rndstore = rngGen();
        if (p.life < 300)
        {
            // Release sparks before exploding
            int rx = ORBIT_RX[rndstore & 7], ry = ORBIT_RY[rndstore & 7];
            rndstore >>= 3;
            int rid = pmapId(pmap[cellIndex(x + rx, y + ry)]);
            if (rid >= 0)
            {
                uint rt = ptype[rid];
                if (rt != PT_NONE && rt != PT_BREC && (elems[rt].properties & PROP_CONDUCTS) != 0 && parts[rid].life == 0)
                {
                    parts[rid].life = 4;
                    parts[rid].ctype = int(rt);
                    partChangeType(uint(rid), PT_SPRK);
                }
            }
        }
        if (p.life < 500)
        {
            // Release all heat
            int rx = int(rndstore % 7) - 3;
            int ry = int((rndstore >> 3) % 7) - 3;
            int rid = pmapId(pmap[cellIndex(x + rx, y + ry)]);
            if (rid >= 0)
            {
                uint rt = ptype[rid];
                if (rt != PT_NONE && rt != PT_VIBR && rt != PT_BVBR && !isHeatInsulator(rt, parts[rid].life))
                {
                    parts[rid].temp = restrictFlt(parts[rid].temp + float(p.tmp * 3), MIN_TEMP, MAX_TEMP);
                    p.tmp = 0;
                }
            }
        }
        if (p.life == 1)
        {
            if (p.tmp2 == 0)
            {
                // VIBR/BVBR -> EXOT + ELEC + PHOT + BREC
                rndstore = rngGen();
                int index = createPartStacked(x + ORBIT_RX[rndstore & 7], y + ORBIT_RY[rndstore & 7], PT_ELEC);
                if (index != -1)
                    parts[index].temp = 7000.0;
                index = createPartStacked(x + ORBIT_RX[(rndstore >> 3) & 7], y + ORBIT_RY[(rndstore >> 3) & 7], PT_PHOT);
                if (index != -1)
                    parts[index].temp = 7000.0;
                index = createPart(x + ORBIT_RX[(rndstore >> 6) & 7], y + ORBIT_RY[(rndstore >> 6) & 7], PT_BREC);
                if (index != -1)
                    parts[index].temp = 7000.0;
                createSelf(i, p, x, y, PT_EXOT);
                p.tmp2 = int((rndstore >> 9) % 1000);
                p.temp = 9000.0;
                setAirPV(ci, airPV(ci) + 50.0);
                return 1;
            }
            p.tmp2 = 0;
            p.temp = 273.15;
            p.tmp = 0;
        }
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
            if (rt == PT_NONE)
                continue;
            if (p.life != 0)
            {
                // Exploding VIBR sets its neighbours off too
                if (rt == PT_VIBR || rt == PT_BVBR)
                {
                    if (parts[rid].life == 0)
                        parts[rid].tmp += 45;
                    else if (p.tmp2 != 0 && p.life > 75 && rngChance(1, 2))
                    {
                        parts[rid].tmp2 = 1;
                        p.tmp = 0;
                    }
                }
                else if (rt == PT_CFLM)
                {
                    p.tmp2 = 1;
                    p.tmp = 0;
                }
            }
            else if (rt == PT_EXOT && rngChance(1, 25))
            {
                // VIBR/BVBR + EXOT -> 2xEXOT
                partChangeType(i, PT_EXOT);
                return 1;
            }
            if (ptype[i] != PT_BVBR && rt == PT_ANAR)
            {
                // VIBR + ANAR -> BVBR
                partChangeType(i, PT_BVBR);
                setAirPV(ci, airPV(ci) - 1.0);
            }
        }
    }
    for (int trade = 0; trade < 9; trade++)
    {
        if (trade % 2 == 0)
            rndstore = rngGen();
        int rx = int(rndstore % 7) - 3;
        rndstore >>= 3;
        int ry = int(rndstore % 7) - 3;
        rndstore >>= 3;
        if (rx == 0 && ry == 0)
            continue;
        int rid = pmapId(pmap[cellIndex(x + rx, y + ry)]);
        if (rid < 0 || (ptype[rid] != PT_VIBR && ptype[rid] != PT_BVBR))
            continue;
        if (p.tmp > parts[rid].tmp)
        {
            int transfer = p.tmp - parts[rid].tmp;
            parts[rid].tmp += transfer / 2;
            p.tmp -= transfer / 2;
            break;
        }
    }
    p.tmp = clamp(p.tmp, 0, 1 << 15);
    return 0;
}

// BIZR.cpp (Element_BIZR_update, all three forms): its decoration colour bleeds into what it touches.
int updateBIZR(uint i, inout Particle p, int x, int y)
{
    if (p.dcolour == 0)
        return 0;
    const float BLEND = 21.0;
    int mr = int((p.dcolour >> 16) & 0xFF), mg = int((p.dcolour >> 8) & 0xFF), mb = int(p.dcolour & 0xFF);
    int ma = int((p.dcolour >> 24) & 0xFF);
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
            if (rt == PT_NONE || rt == PT_BIZR || rt == PT_BIZRG || rt == PT_BIZRS)
                continue;
            uint dc = parts[rid].dcolour;
            int tr = int((dc >> 16) & 0xFF), tg = int((dc >> 8) & 0xFF), tb = int(dc & 0xFF), ta = int((dc >> 24) & 0xFF);
            int nr = tr + int(sign(float(mr - tr))) + int(roundAway(float(mr - tr) / BLEND));
            int ng = tg + int(sign(float(mg - tg))) + int(roundAway(float(mg - tg) / BLEND));
            int nb = tb + int(sign(float(mb - tb))) + int(roundAway(float(mb - tb) / BLEND));
            int na = ta + int(sign(float(ma - ta))) + int(roundAway(float(ma - ta) / BLEND));
            parts[rid].dcolour = uint(nr << 16 | ng << 8 | nb | na << 24);
        }
    }
    return 0;
}

// INVIS.cpp: becomes passable (tmp2 1) when the pressure is beyond its resistance.
int updateINVIS(uint i, inout Particle p, int x, int y)
{
    float resistance = p.tmp > 0 ? float(p.tmp) : 4.0;
    float pv = airPV(airIndex(x, y));
    p.tmp2 = (pv < -resistance || pv > resistance) ? 1 : 0;
    return 0;
}

// LCRY.cpp: liquid crystal turns on (life rises to 10, photons pass) or off over a few ticks, spreading along.
int updateLCRY(uint i, inout Particle p, int x, int y)
{
    int check, setto;
    if (p.tmp == 1 || p.tmp == 0)
    {
        if (p.tmp == 1)
        {
            if (p.life <= 0)
                p.tmp = 0;
            else
            {
                p.life = max(p.life - 2, 0);
                p.tmp2 = p.life;
            }
        }
        check = 3;
        setto = 1;
    }
    else if (p.tmp == 2 || p.tmp == 3)
    {
        if (p.tmp == 2)
        {
            if (p.life >= 10)
                p.tmp = 3;
            else
            {
                p.life = min(p.life + 2, 10);
                p.tmp2 = p.life;
            }
        }
        check = 0;
        setto = 2;
    }
    else
    {
        p.tmp = 0;
        p.life = 0;
        return 0;
    }
    for (int rx = -1; rx <= 1; rx++)
    {
        for (int ry = -1; ry <= 1; ry++)
        {
            if (rx == 0 && ry == 0)
                continue;
            int rid = pmapId(pmap[cellIndex(x + rx, y + ry)]);
            if (rid >= 0 && ptype[rid] == PT_LCRY && parts[rid].tmp == check)
                parts[rid].tmp = setto;
        }
    }
    return 0;
}

// GPMP.cpp: gravity pump. (Its pull on the Newtonian gravity field comes with Phase 4.)
int updateGPMP(uint i, inout Particle p, int x, int y)
{
    if (p.life != 10)
    {
        if (p.life > 0)
            p.life--;
        return 0;
    }
    p.temp = clamp(p.temp, -256.0 + 273.15, 256.0 + 273.15);
    passOnState(p, x, y, PT_GPMP);
    return 0;
}

// TUNG.cpp: tungsten. Melts violently (or burns with O2 when very hot) and breaks under changing pressure.
int updateTUNG(uint i, inout Particle p, int x, int y)
{
    bool splode = false;
    float meltingPoint = elems[PT_TUNG].highTemperature;
    if (p.temp > 2400.0)
    {
        for (int rx = -1; rx <= 1; rx++)
            for (int ry = -1; ry <= 1; ry++)
                if ((rx != 0 || ry != 0) && typeAt(x + rx, y + ry) == PT_O2)
                    splode = true;
    }
    uint ci = airIndex(x, y);
    if ((p.temp > meltingPoint && rngChance(1, 20)) || splode)
    {
        if (rngChance(1, 50))
            setAirPV(ci, airPV(ci) + 50.0);
        else if (rngChance(1, 100))
        {
            // TUNG -> FIRE
            partChangeType(i, PT_FIRE);
            p.life = rngBetween(0, 499);
            return 1;
        }
        else
        {
            // TUNG -> LAVA(TUNG)
            partChangeType(i, PT_LAVA);
            p.ctype = int(PT_TUNG);
            return 1;
        }
        if (splode)
            p.temp = restrictFlt(meltingPoint + float(rngBetween(200, 799)), MIN_TEMP, MAX_TEMP);
        p.vx += float(rngBetween(-50, 50));
        p.vy += float(rngBetween(-50, 50));
        return 1;
    }
    int press = int(airPV(ci) * 64.0);
    int diff = press - p.tmp3;
    if (diff > 32 || diff < -32)
    {
        // TUNG -> BRMT(TUNG)
        partChangeType(i, PT_BRMT);
        p.ctype = int(PT_TUNG);
        return 1;
    }
    p.tmp3 = press;
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
    case PT_PLSM:
        return updateFIRE(i, p, t, x, y, surroundSpace);
    case PT_ACID:
        return updateACID(i, p, x, y);
    case PT_CAUS:
        return updateCAUS(i, p, x, y);
    case PT_CBNW:
        return updateCBNW(i, p, x, y);
    case PT_BASE:
        return updateBASE(i, p, x, y);
    case PT_H2:
        return updateH2(i, p, x, y);
    case PT_BOYL:
        return updateBOYL(i, p, x, y);
    case PT_RFRG:
    case PT_RFGL:
        return updateRFRG(i, p, x, y);
    case PT_GEL:
        return updateGEL(i, p, x, y);
    case PT_MERC:
        return updateMERC(i, p, x, y);
    case PT_FRZW:
        return updateFRZW(i, p, x, y);
    case PT_FRZZ:
        return updateFRZZ(i, p, x, y);
    case PT_YEST:
        return updateYEST(i, p, x, y);
    case PT_ANAR:
        return updateANAR(i, p, x, y);
    case PT_QRTZ:
    case PT_PQRT:
        return updateQRTZ(i, p, t, x, y);
    case PT_SLCN:
        return updateSLCN(i, p, x, y);
    case PT_GLOW:
        return updateGLOW(i, p, x, y);
    case PT_RSST:
        return updateRSST(i, p, x, y);
    case PT_RSSS:
        return updateRSSS(i, p, x, y);
    case PT_VIRS:
    case PT_VRSS:
    case PT_VRSG:
        return updateVIRS(i, p, x, y, surroundSpace);
    case PT_SPRK:
        return updateSPRK(i, p, x, y, surroundSpace);
    case PT_NTCT:
    case PT_PTCT:
        return updateNTCT(i, p, x, y);
    case PT_BTRY:
        return updateBTRY(i, p, x, y);
    case PT_SWCH:
        return updateSWCH(i, p, x, y);
    case PT_WIRE:
        return updateWIRE(i, p, x, y);
    case PT_DTEC:
        return updateDTEC(i, p, x, y);
    case PT_TSNS:
        return updateTSNS(i, p, x, y);
    case PT_PSNS:
        return updatePSNS(i, p, x, y);
    case PT_LSNS:
        return updateLSNS(i, p, x, y);
    case PT_VSNS:
        return updateVSNS(i, p, x, y);
    case PT_LDTC:
        return updateLDTC(i, p, x, y);
    case PT_FUSE:
        return updateFUSE(i, p, x, y);
    case PT_FSEP:
        return updateFSEP(i, p, x, y);
    case PT_IGNT:
        return updateIGNT(i, p, x, y);
    case PT_BANG:
        return updateBANG(i, p, x, y);
    case PT_BOMB:
        return updateBOMB(i, p, x, y);
    case PT_EMBR:
        return updateEMBR(i, p, x, y);
    case PT_C5:
        return updateC5(i, p, x, y);
    case PT_DEST:
        return updateDEST(i, p, x, y);
    case PT_FWRK:
        return updateFWRK(i, p, x, y, surroundSpace);
    case PT_FIRW:
        return updateFIRW(i, p, x, y);
    case PT_THDR:
        return updateTHDR(i, p, x, y);
    case PT_PHOT:
        return updatePHOT(i, p, x, y, surroundSpace);
    case PT_NEUT:
        return updateNEUT(i, p, x, y, surroundSpace);
    case PT_ELEC:
        return updateELEC(i, p, x, y);
    case PT_PROT:
        return updatePROT(i, p, x, y);
    case PT_GRVT:
        return updateGRVT(i, p, x, y);
    case PT_PLUT:
        return updatePLUT(i, p, x, y);
    case PT_URAN:
        return updateURAN(i, p, x, y);
    case PT_DEUT:
        return updateDEUT(i, p, x, y);
    case PT_ISOZ:
    case PT_ISZS:
        return updateISOZ(i, p, x, y);
    case PT_POLO:
        return updatePOLO(i, p, x, y);
    case PT_NBLE:
        return updateNBLE(i, p, x, y);
    case PT_AMTR:
        return updateAMTR(i, p, x, y);
    case PT_SING:
        return updateSING(i, p, x, y);
    case PT_WARP:
        return updateWARP(i, p, x, y);
    case PT_EXOT:
        return updateEXOT(i, p, x, y);
    case PT_VIBR:
    case PT_BVBR:
        return updateVIBR(i, p, t, x, y);
    case PT_BIZR:
    case PT_BIZRG:
    case PT_BIZRS:
        return updateBIZR(i, p, x, y);
    case PT_INVIS:
        return updateINVIS(i, p, x, y);
    case PT_LCRY:
        return updateLCRY(i, p, x, y);
    case PT_GPMP:
        return updateGPMP(i, p, x, y);
    case PT_TUNG:
        return updateTUNG(i, p, x, y);
    case PT_CLNE:
        return updateCLNE(i, p, x, y);
    case PT_BCLN:
        return updateBCLN(i, p, x, y);
    case PT_PCLN:
        return updatePCLN(i, p, x, y);
    case PT_PBCN:
        return updatePBCN(i, p, x, y);
    case PT_PVOD:
        return updatePVOD(i, p, x, y);
    case PT_PUMP:
        return updatePUMP(i, p, x, y);
    case PT_HSWC:
        return updateHSWC(i, p, x, y);
    case PT_DLAY:
        return updateDLAY(i, p, x, y);
    case PT_CONV:
        return updateCONV(i, p, x, y);
    case PT_SHLD1:
    case PT_SHLD2:
    case PT_SHLD3:
    case PT_SHLD4:
        return updateSHLD(i, p, t, x, y);
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
