// Movement of energy particles (TYPE_ENERGY: PHOT, NEUT, ELEC, PROT, GRVT...): TPT's MovementPhase energy branch
// with do_move/try_move/move for particles that live in the photons map. They pass through what they can,
// reflect off surfaces (the normal is found by tracing the boundary, get_normal), and PHOT refracts into and
// out of glass by wavelength.
#pragma once
#include "common.slang"
#include "move.slang"
#include "elements.slang"

static const int SURF_RANGE = 10;
static const int NORMAL_MIN_EST = 3;
static const int NORMAL_INTERP = 20;
static const float NORMAL_FRAC = 16.0;
static const float GLASS_IOR = 1.9;
static const float GLASS_DISP = 0.07;
static const uint FLAG_SKIPMOVE = 0x2;

// get_wavelength_bin: picks a band of the photon's wavelengths (narrowing ctype to it) for refraction.
int wavelengthBin(inout int wm)
{
    if ((wm & 0x3FFFFFFF) == 0)
        return -1;
    int w0 = int(firstbitlow(uint(wm) | 0xC0000000u));
    int wM = int(firstbithigh(uint(wm & 0x3FFFFFFF)));
    if (wM - w0 < 5)
        return wM + w0;
    uint r = rngGen();
    int i = int((r >> 1) % uint(wM - w0 - 4)) + w0;
    if ((r & 1) != 0)
    {
        wm &= 0x1F << i;
        return (i + 2) * 2;
    }
    wm &= 0xF << i;
    return (i + 2) * 2 - 1;
}

uint directionToMap(float dx, float dy)
{
    return (dx >= 0.0 ? 1u : 0u) | ((dx + dy) >= 0.0 ? 2u : 0u) | (dy >= 0.0 ? 4u : 0u) |
           ((dy - dx) >= 0.0 ? 8u : 0u) | (dx <= 0.0 ? 16u : 0u) | ((dx + dy) <= 0.0 ? 32u : 0u) |
           (dy <= 0.0 ? 64u : 0u) | ((dy - dx) <= 0.0 ? 128u : 0u);
}

// is_blocking: for refraction (REFRACT), only glass counts.
bool isBlocking(uint pt, bool refract, int x, int y)
{
    if (refract)
    {
        if (!inSim(x, y))
            return false;
        uint t = typeAt(x, y);
        return t == PT_GLAS || t == PT_BGLA;
    }
    return evalMove(pt, x, y) == 0;
}

bool isBoundary(uint pt, bool refract, int x, int y)
{
    if (!isBlocking(pt, refract, x, y))
        return false;
    return !(isBlocking(pt, refract, x, y - 1) && isBlocking(pt, refract, x, y + 1) &&
             isBlocking(pt, refract, x - 1, y) && isBlocking(pt, refract, x + 1, y));
}

static const int2 FNB_D[8] = { int2(1, 0), int2(1, 1), int2(0, 1), int2(-1, 1), int2(-1, 0), int2(-1, -1), int2(0, -1),
                               int2(1, -1) };
static const uint FNB_DE[8] = { 0x83, 0x07, 0x0E, 0x1C, 0x38, 0x70, 0xE0, 0xC1 };

bool findNextBoundary(uint pt, bool refract, inout int x, inout int y, uint dm, inout int em, bool reverse)
{
    if (x <= 0 || x >= int(pc.W) - 1 || y <= 0 || y >= int(pc.H) - 1)
        return false;
    if (em != -1)
        dm &= FNB_DE[em];
    uint mask = 0;
    for (int i = 0; i < 8; i++)
        if ((dm & (1u << i)) != 0 && isBlocking(pt, refract, x + FNB_D[i].x, y + FNB_D[i].y))
            mask |= 1u << i;
    for (int i = 0; i < 8; i++)
    {
        int n = (i + (reverse ? 1 : -1)) & 7;
        if ((mask & (1u << i)) != 0 && (mask & (1u << n)) == 0)
        {
            x += FNB_D[i].x;
            y += FNB_D[i].y;
            em = i;
            return true;
        }
    }
    return false;
}

struct NormalResult
{
    bool success;
    float nx, ny;
};

// get_normal: walk the boundary both ways from (x, y) and take the normal of the chord between the ends.
NormalResult getNormal(uint pt, bool refract, int x, int y, float dx, float dy)
{
    NormalResult res;
    res.success = false;
    res.nx = 0.0;
    res.ny = 0.0;
    if ((dx == 0.0 && dy == 0.0) || !isBoundary(pt, refract, x, y))
        return res;
    uint ldm = refract ? 0xFFu : directionToMap(-dy, dx);
    uint rdm = refract ? 0xFFu : directionToMap(dy, -dx);
    int lx = x, ly = y, rx = x, ry = y;
    bool lv = true, rv = true;
    int lm = -1, rm = -1;
    int j = 0;
    for (int i = 0; i < SURF_RANGE; i++)
    {
        if (lv)
            lv = findNextBoundary(pt, refract, lx, ly, ldm, lm, true);
        if (rv)
            rv = findNextBoundary(pt, refract, rx, ry, rdm, rm, false);
        j += (lv ? 1 : 0) + (rv ? 1 : 0);
        if (!lv && !rv)
            break;
    }
    if (j < NORMAL_MIN_EST || (lx == rx && ly == ry))
        return res;
    float ex = float(rx - lx), ey = float(ry - ly);
    float r = 1.0 / sqrt(ex * ex + ey * ey);
    res.success = true;
    res.nx = ey * r;
    res.ny = -ex * r;
    return res;
}

// photoelectric_effect: a photon hitting PSCN next to NSCN sparks it.
void photoelectricEffect(int nx, int ny)
{
    int rid = pmapId(pmap[cellIndex(nx, ny)]);
    if (rid < 0 || ptype[rid] != PT_PSCN || parts[rid].life != 0)
        return;
    if (typeAt(nx - 1, ny) == PT_NSCN || typeAt(nx + 1, ny) == PT_NSCN || typeAt(nx, ny - 1) == PT_NSCN ||
        typeAt(nx, ny + 1) == PT_NSCN)
    {
        parts[rid].ctype = int(PT_PSCN);
        partChangeType(uint(rid), PT_SPRK);
        parts[rid].life = 4;
    }
}

// get_normal_interp: step from the particle along its velocity to the boundary it is hitting.
NormalResult getNormalInterp(uint pt, bool refract, float x0, float y0, float dx, float dy)
{
    NormalResult fail;
    fail.success = false;
    fail.nx = 0.0;
    fail.ny = 0.0;
    dx /= NORMAL_FRAC;
    dy /= NORMAL_FRAC;
    int x = 0, y = 0;
    int i;
    for (i = 0; i < NORMAL_INTERP; i++)
    {
        x = int(x0 + 0.5);
        y = int(y0 + 0.5);
        if (!inSim(x, y))
            return fail;
        if (isBoundary(pt, refract, x, y))
            break;
        x0 += dx;
        y0 += dy;
    }
    if (i >= NORMAL_INTERP)
        return fail;
    if (!refract && pt == PT_PHOT)
        photoelectricEffect(x, y);
    return getNormal(pt, refract, x, y, dx, dy);
}

// create_gain_photon: a photon passing through GLOW that has recharged makes another, coloured by its heat.
void createGainPhoton(Particle pp)
{
    int lr = 2 * rngBetween(0, 1) - 1;
    float xx = pp.x - float(lr) * 0.3 * pp.vy;
    float yy = pp.y + float(lr) * 0.3 * pp.vx;
    int nx = int(xx + 0.5), ny = int(yy + 0.5);
    if (!inSim(nx, ny))
        return;
    int gid = pmapId(pmap[cellIndex(nx, ny)]);
    if (gid < 0 || ptype[gid] != PT_GLOW)
        return;
    float oldTemp = parts[gid].temp;
    int i = createPartStacked(nx, ny, PT_PHOT);
    if (i == -1)
        return;
    parts[i].x = xx;
    parts[i].y = yy;
    parts[i].vx = pp.vx;
    parts[i].vy = pp.vy;
    parts[i].temp = oldTemp;
    int tempBin = clamp(int((oldTemp - 273.0) * 0.25), 0, 25);
    parts[i].ctype = 0x1F << tempBin;
}

// move() for an energy particle: photons instead of pmap. Returns -1 if it left the play area.
int moveEnergy(uint i, inout Particle p, int x, int y, float nxf, float nyf)
{
    int nx = int(nxf + 0.5), ny = int(nyf + 0.5);
    p.x = nxf;
    p.y = nyf;
    parts[i].x = nxf;
    parts[i].y = nyf;
    if (nx != x || ny != y)
    {
        uint orig;
        if (inSim(x, y))
            InterlockedCompareExchange(photons[cellIndex(x, y)], pmapEnc(i), 0u, orig);
        if (!inPlayArea(nx, ny))
        {
            killPart(i, x, y);
            return -1;
        }
        InterlockedExchange(photons[cellIndex(nx, ny)], pmapEnc(i), orig);
    }
    return 1;
}

// create_cherenkov_photon: a fast neutron in glass gives off blue light.
void createCherenkovPhoton(Particle pp)
{
    int nx = int(pp.x + 0.5), ny = int(pp.y + 0.5);
    int gid = pmapId(pmap[cellIndex(nx, ny)]);
    if (gid < 0 || (ptype[gid] != PT_GLAS && ptype[gid] != PT_BGLA))
        return;
    if (sqrt(pp.vx * pp.vx + pp.vy * pp.vy) < 1.44)
        return;
    float oldTemp = parts[gid].temp;
    int i = createPartStacked(nx, ny, PT_PHOT);
    if (i == -1)
        return;
    int lr = 2 * rngBetween(0, 1) - 1;
    parts[i].ctype = 0x00000F80;
    parts[i].x = pp.x;
    parts[i].y = pp.y;
    parts[i].temp = oldTemp;
    float vx = pp.vx - float(lr) * 2.5 * pp.vy;
    float vy = pp.vy + float(lr) * 2.5 * pp.vx;
    float r = 1.269 / sqrt(vx * vx + vy * vy); // photons have the speed of light
    parts[i].vx = vx * r;
    parts[i].vy = vy * r;
}

// try_move's cases for an energy particle sharing a cell with another particle (e == 2). Returns false if it died.
bool energyOverlap(uint i, inout Particle p, uint t, int rid, uint rt, int x, int y)
{
    if (rid < 0)
        return true;
    if (t == PT_NEUT)
    {
        // NEUT + GLAS/BGLA -> NEUT + GLAS/BGLA + PHOT
        if ((rt == PT_GLAS || rt == PT_BGLA) && rngChance(1, 10))
            createCherenkovPhoton(p);
        return true;
    }
    if (t == PT_ELEC)
    {
        if (rt == PT_GLOW)
        {
            // ELEC + GLOW -> PHOT + GLOW
            partChangeType(i, PT_PHOT);
            p.ctype = 0x3FFFFFFF;
        }
        return true;
    }
    if (t == PT_PROT)
    {
        if (rt == PT_INVIS)
            partChangeType(i, PT_NEUT); // PROT + INVIS -> NEUT + INVIS
        return true;
    }
    if (t != PT_PHOT)
        return true;
    if (rt == PT_FILT)
    {
        p.ctype = filtInteract(parts[rid], p.ctype);
        return true;
    }
    if (rt == PT_INVIS)
    {
        Particle r = parts[rid];
        float resistance = r.tmp > 0 ? float(r.tmp) : 4.0;
        float pv = airPV(airIndex(int(r.x + 0.5), int(r.y + 0.5)));
        if (pv >= -resistance && pv <= resistance)
        {
            // PHOT + INVIS -> NEUT + INVIS
            partChangeType(i, PT_NEUT);
            p.ctype = 0;
        }
        return true;
    }
    if (rt == PT_BIZR || rt == PT_BIZRG || rt == PT_BIZRS)
    {
        // PHOT + BIZR/BIZRG/BIZRS -> ELEC + BIZR/BIZRG/BIZRS
        partChangeType(i, PT_ELEC);
        p.ctype = 0;
        return true;
    }
    if (rt == PT_GPMP)
    {
        if (parts[rid].life == 0)
        {
            // PHOT + GPMP -> GRVT + GPMP
            partChangeType(i, PT_GRVT);
            p.tmp = int(parts[rid].temp - 273.15);
        }
        return true;
    }
    if (rt == PT_H2)
    {
        if ((p.tmp & 0x1) == 0)
        {
            // PHOT + H2 -> PROT + ELEC (TPT puts the ELEC where the photon was)
            partChangeType(i, PT_PROT);
            p.ctype = 0;
            p.tmp2 = 0x1;
            createPartAt(uint(rid), x, y, PT_ELEC);
        }
        return true;
    }
    if (rt == PT_GLOW)
    {
        if (parts[rid].life == 0 && rngChance(1, 30))
        {
            parts[rid].life = 120;
            createGainPhoton(p);
        }
    }
    else if (rt == PT_C5)
    {
        Particle r = parts[rid];
        if (r.life > 0 && (r.ctype & p.ctype & int(0xFFFFFFC0u)) != 0)
        {
            // Two photons meet in C5: their momenta add
            float vx = float((r.tmp << 16) >> 16) / 255.0;
            float vy = float(r.tmp >> 16) / 255.0;
            float vn = p.vx * p.vx + p.vy * p.vy;
            if (p.vx + vx == 0.0 && p.vy + vy == 0.0)
            {
                p.vx = vy;
                p.vy = -vx;
            }
            else
            {
                p.ctype = (r.ctype & p.ctype) >> 6;
                p.vx += vx;
                p.vy += vy;
                vn /= p.vx * p.vx + p.vy * p.vy;
                vn = sqrt(vn);
                p.vx *= vn;
                p.vy *= vn;
            }
            parts[rid].life = 0;
            parts[rid].ctype = 0;
        }
        else if (r.ctype == 0 && (p.ctype & int(0xFFFFFFC0u)) != 0)
        {
            // C5 stores the photon
            parts[rid].life = 1;
            parts[rid].ctype = p.ctype;
            parts[rid].tmp = (0xFFFF & int(p.vx * 255.0)) | int(0xFFFF0000u & uint(int(p.vy * 16711680.0)));
            parts[rid].tmp2 = (0xFFFF & int((p.x - float(x)) * 255.0)) |
                              int(0xFFFF0000u & uint(int((p.y - float(y)) * 16711680.0)));
            killPart(i, x, y);
            return false;
        }
    }
    // (FILT, INVIS, BIZR, H2 -> PROT + ELEC and GPMP -> GRVT come with those elements.)
    return true;
}

// try_move's swap for NEUT moving into NEUTPENETRATE material: the material there and whatever is under the
// neutron (if that is NEUTPENETRATE too) change places, so neutrons push through it.
void neutronSwap(uint ri, uint rEnt, int x, int y, int nx, int ny)
{
    uint own = cellIndex(x, y), dst = cellIndex(nx, ny);
    uint sEnt = pmap[own];
    int sid = pmapId(sEnt);
    if (sid >= 0 && (elems[ptype[sid]].properties & PROP_NEUTPENETRATE) == 0)
        return; // don't move anything except the neutron
    if (bmapAt(x, y) == WL_ALLOWENERGY)
        return; // don't drag material into an energy-only wall
    if ((sEnt & PMAP_LOCK) != 0 || (rEnt & PMAP_LOCK) != 0)
        return;
    uint orig;
    InterlockedCompareExchange(pmap[own], sEnt, sEnt | PMAP_LOCK, orig);
    if (orig != sEnt)
        return;
    InterlockedCompareExchange(pmap[dst], rEnt, sid >= 0 ? (sEnt & PMAP_ID_MASK) : 0u, orig);
    if (orig != rEnt)
    {
        InterlockedExchange(pmap[own], sEnt, orig);
        return;
    }
    if (sid >= 0)
    {
        parts[sid].x = float(nx);
        parts[sid].y = float(ny);
    }
    parts[ri].x = float(x);
    parts[ri].y = float(y);
    DeviceMemoryBarrier();
    InterlockedExchange(pmap[own], pmapEnc(ri), orig);
}

// do_move / try_move / move for an energy particle. 0 = blocked, 1 = moved, -1 = died.
int doMoveEnergy(uint i, inout Particle p, uint t, int x, int y, float nxf, float nyf)
{
    int nx = int(nxf + 0.5);
    int ny = int(nyf + 0.5);
    if (params[0].edgeMode == 2)
    {
        bool xOk = nx >= CELL && nx < int(pc.W) - CELL;
        bool yOk = ny >= CELL && ny < int(pc.H) - CELL;
        if (!xOk)
            nxf = remainderP(nxf - float(CELL) + 0.5, float(pc.W) - float(CELL) * 2.0) + float(CELL) - 0.5;
        if (!yOk)
            nyf = remainderP(nyf - float(CELL) + 0.5, float(pc.H) - float(CELL) * 2.0) + float(CELL) - 0.5;
        nx = int(nxf + 0.5);
        ny = int(nyf + 0.5);
    }
    if (ptype[i] == PT_NONE)
        return 0;
    if ((nx != x || ny != y) && inSim(nx, ny))
    {
        uint rEnt = pmap[cellIndex(nx, ny)];
        int rid = pmapId(rEnt);
        uint rt = rid < 0 ? PT_NONE : ptype[rid];
        int e = evalMoveType(t, nx, ny, rt);
        // half-silvered mirror
        if (e == 0 && t == PT_PHOT && ((rt == PT_BMTL && rngChance(1, 2)) || typeAt(x, y) == PT_BMTL))
            e = 2;
        if (e == 0)
        {
            breakWood(p, rEnt, rt);
            if (params[0].legacyHeat == 0 && t == PT_PHOT && rt != PT_NONE)
            {
                // PHOT heat conduction
                if (rt == PT_COAL || rt == PT_BCOL)
                    parts[rid].temp = p.temp;
                if (!isHeatInsulator(rt, parts[rid].life) && rt != PT_FILT)
                {
                    float avg = restrictFlt((parts[rid].temp + p.temp) / 2.0, MIN_TEMP, MAX_TEMP);
                    p.temp = avg;
                    parts[rid].temp = avg;
                }
            }
            else if ((t == PT_NEUT || t == PT_ELEC) &&
                     (rt == PT_CLNE || rt == PT_PCLN || rt == PT_BCLN || rt == PT_PBCN))
            {
                if (parts[rid].ctype == 0)
                    parts[rid].ctype = int(t);
            }
            // (Portals come with PRTI.)
            return 0;
        }
        if (e == 2)
        {
            if (!energyOverlap(i, p, t, rid, rt, x, y))
                return -1;
        }
        else if (rt != PT_NONE)
        {
            if (rt == PT_VOID || rt == PT_PVOD)
            {
                killPart(i, x, y);
                return -1;
            }
            if (rt == PT_BHOL || rt == PT_NBHL)
            {
                if (params[0].legacyHeat == 0)
                    parts[rid].temp = restrictFlt(parts[rid].temp + p.temp / 2.0, MIN_TEMP, MAX_TEMP);
                killPart(i, x, y);
                return -1;
            }
            if (rt == PT_DEUT && t == PT_ELEC)
            {
                // ELEC charges DEUT
                if (parts[rid].life < 6000)
                    parts[rid].life += 1;
                parts[rid].temp = 0.0;
                killPart(i, x, y);
                return -1;
            }
            if (rt == PT_VIBR || rt == PT_BVBR)
            {
                parts[rid].tmp += 20;
                killPart(i, x, y);
                return -1;
            }
            if (t == PT_NEUT && (elems[rt].properties & PROP_NEUTABSORB) != 0)
            {
                killPart(i, x, y);
                return -1;
            }
            if (closedEhole(x, y) && !closedEhole(nx, ny))
                return 0;
            if (t == PT_NEUT)
                neutronSwap(uint(rid), rEnt, x, y, nx, ny);
            // (other energy particles have can_move 1 only into empty cells)
        }
        else if (closedEhole(x, y) && !closedEhole(nx, ny))
            return 0;
    }
    return moveEnergy(i, p, x, y, nxf, nyf);
}

// MovementPhase for energy particles. fin/clear come from planMove.
void energyMovementPhase(uint i, inout Particle p, uint t)
{
    int x = int(p.x + 0.5);
    int y = int(p.y + 0.5);
    PlanMoveResult mr = planMove(p, t, x, y);
    p.vx = mr.vx;
    p.vy = mr.vy;
    int finX = mr.finX, finY = mr.finY;
    float finXf = mr.finXf, finYf = mr.finYf;
    bool stagnant = (p.flags & FLAG_STAGNANT) != 0;
    p.flags &= ~FLAG_STAGNANT;

    if (t == PT_PHOT)
    {
        if ((p.flags & FLAG_SKIPMOVE) != 0)
        {
            p.flags &= ~FLAG_SKIPMOVE;
            return;
        }
        if (evalMove(PT_PHOT, finX, finY) != 0)
        {
            uint rt = typeAt(finX, finY), lt = typeAt(x, y);
            bool rGlass = rt == PT_GLAS || rt == PT_BGLA;
            bool lGlass = lt == PT_GLAS || lt == PT_BGLA;
            if (rGlass != lGlass)
            {
                // Refraction at a glass surface
                NormalResult gn = getNormalInterp(t, true, p.x, p.y, p.vx, p.vy);
                if (!gn.success)
                {
                    killPart(i, x, y);
                    return;
                }
                float nrx = gn.nx, nry = gn.ny;
                int r = wavelengthBin(p.ctype);
                if (r == -1 || (p.ctype & 0x3FFFFFFF) == 0)
                {
                    killPart(i, x, y);
                    return;
                }
                float nn = GLASS_IOR - GLASS_DISP * float(r - 30) / 30.0;
                nn *= nn;
                bool enter = rGlass && !lGlass;
                if (enter)
                {
                    nrx = -nrx;
                    nry = -nry;
                    nn = 1.0 / nn;
                }
                float ct1 = p.vx * nrx + p.vy * nry;
                float ct2 = 1.0 - (nn * nn) * (1.0 - ct1 * ct1);
                if (ct2 < 0.0)
                {
                    // total internal reflection
                    p.vx -= 2.0 * ct1 * nrx;
                    p.vy -= 2.0 * ct1 * nry;
                    finXf = p.x;
                    finYf = p.y;
                    finX = x;
                    finY = y;
                }
                else
                {
                    ct2 = sqrt(ct2) - nn * ct1;
                    p.vx = nn * p.vx + ct2 * nrx;
                    p.vy = nn * p.vy + ct2 * nry;
                }
            }
        }
    }
    if (stagnant)
    {
        // Reflected last tick: move to the whole-number position (as TPT does, for old saves)
        if (doMoveEnergy(i, p, t, x, y, float(finX), float(finY)) == 0 && ptype[i] != PT_NONE)
            killPart(i, x, y);
        return;
    }
    if (doMoveEnergy(i, p, t, x, y, finXf, finYf) != 0)
        return;
    if (ptype[i] == PT_NONE)
        return;
    // Reflection
    p.flags |= FLAG_STAGNANT;
    if (t == PT_NEUT && rngChance(1, 10))
    {
        killPart(i, x, y);
        return;
    }
    int rid = inSim(finX, finY) ? pmapId(pmap[cellIndex(finX, finY)]) : -1;
    uint rt = rid < 0 ? PT_NONE : ptype[rid];
    if (t == PT_PHOT)
    {
        uint mask = elems[rt].photonReflectWavelengths;
        if (rt == PT_LITH)
        {
            int wlBin = clamp(parts[rid].ctype / 4, 0, 25);
            mask = 0x1Fu << wlBin;
        }
        else if (rt == PT_SEED)
        {
            // Reflect wavelengths by SEED's colour genes
            int colour = (parts[rid].ctype >> PLNT_COLOUR) & 0x3f;
            mask |= (colour & 0x30) != 0 ? 0u : (1u << 25);
            mask |= (colour & 0x0C) != 0 ? 0u : (1u << 15);
            mask |= (colour & 0x03) != 0 ? 0u : (1u << 5);
        }
        p.ctype &= int(mask);
    }
    NormalResult gn = getNormalInterp(t, false, p.x, p.y, p.vx, p.vy);
    if (!gn.success)
    {
        if (t != PT_NEUT)
            killPart(i, x, y);
        return;
    }
    float nrx = gn.nx, nry = gn.ny;
    if (rt == PT_CRMC)
    {
        // Ceramic scatters
        float a = float(rngBetween(-50, 50)) * 0.01;
        a = a * a * a;
        float cx = cos(a), sy = sin(a);
        float anrx = cx * nrx + sy * nry;
        float anry = cx * nry - sy * nrx;
        float dp = anrx * p.vx + anry * p.vy;
        p.vx -= 2.0 * dp * anrx;
        p.vy -= 2.0 * dp * anry;
    }
    else
    {
        float dp = nrx * p.vx + nry * p.vy;
        p.vx -= 2.0 * dp * nrx;
        p.vy -= 2.0 * dp * nry;
    }
    // (the actual move waits until the next tick, so fast particles reflect and refract correctly)
    if ((p.ctype & 0x3FFFFFFF) == 0 && t == PT_PHOT)
        killPart(i, x, y);
}
