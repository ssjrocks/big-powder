// Beams from ARAY, CRAY and DRAY, and LIGH's lightning. TPT casts a beam inside the emitter's update, the moment
// a spark touches it, and LIGH draws its next stretch of lightning in its update; both reach much further than
// the colour passes keep particles apart, so they are queued (requestPost) and done here after the passes, one
// thread each. Serial mode does them one at a time, in the order they were queued.
#include "common.slang"
#include "elements.slang"

// Live particle on top at (x, y) in pmap, or -1.
int topAt(int x, int y)
{
    int id = pmapId(pmap[cellIndex(x, y)]);
    return (id >= 0 && ptype[id] != PT_NONE) ? id : -1;
}

// Live energy particle at (x, y), or -1.
int photonAt(int x, int y)
{
    int id = pmapId(photons[cellIndex(x, y)]);
    return (id >= 0 && ptype[id] != PT_NONE) ? id : -1;
}

// ARAY.cpp: a line of BRAY until the beam hits something, which it sparks. A beam from PSCN erases BRAY instead,
// one from INST goes on through conductors, and FILT colours the beam.
void castARAY(Particle em, int x, int y, int rx, int ry, int sc)
{
    int shortLife = em.life > 0 ? em.life : 30;
    int longLife = em.life > 0 ? em.life : 1020;
    bool isBlackDeco = false;
    bool destroy = sc == int(PT_PSCN);
    bool nostop = sc == int(PT_INST);
    int colored = 0;
    int nxi = -rx, nyi = -ry;
    bool docontinue = true;
    for (int nxx = 0, nyy = 0; docontinue; nyy += nyi, nxx += nxi)
    {
        int cx = x + nxi + nxx, cy = y + nyi + nyy;
        if (!inSim(cx, cy))
            break;
        bool first = nxx == 0 && nyy == 0;
        int r = topAt(cx, cy);
        uint rt = r >= 0 ? ptype[r] : PT_NONE;
        if (rt == PT_NONE)
        {
            int nr = createPart(cx, cy, PT_BRAY);
            if (nr >= 0)
            {
                if (destroy)
                {
                    parts[nr].tmp = 2;
                    parts[nr].life = 2;
                }
                else
                {
                    parts[nr].ctype = colored;
                    parts[nr].life = shortLife;
                }
                parts[nr].temp = em.temp;
                if (isBlackDeco)
                    parts[nr].dcolour = 0xFF000000u;
            }
        }
        else if (!destroy)
        {
            if (rt == PT_BRAY)
            {
                // What it does to BRAY already there depends on that BRAY's mode
                int mode = parts[r].tmp;
                if (mode == 1)
                    parts[r].life = longLife; // long-lived BRAY: refresh it and go on
                else
                {
                    if (mode == 0 && !first)
                    {
                        // a white beam crossing another: the crossing lasts a while, coloured if it wasn't
                        parts[r].life = longLife;
                        parts[r].tmp = 1;
                        if (parts[r].ctype == 0)
                            parts[r].ctype = colored;
                    }
                    docontinue = false;
                }
                if (isBlackDeco)
                    parts[r].dcolour = 0xFF000000u;
            }
            else if (rt == PT_FILT)
            {
                if (parts[r].tmp != 6)
                {
                    colored = filtInteract(parts[r], colored);
                    if (colored == 0)
                        break;
                }
                isBlackDeco = parts[r].dcolour == 0xFF000000u;
                parts[r].life = 4;
            }
            else if (rt == PT_STOR)
            {
                if (parts[r].tmp != 0)
                {
                    // Make STOR release what it holds, below or beside it
                    Particle s = parts[r];
                    bool done = false;
                    for (int ry1 = 1; ry1 >= -1 && !done; ry1--)
                    {
                        for (int rx1 = 0; rx1 >= -1 && rx1 <= 1; rx1 = -rx1 - rx1 + 1)
                        {
                            int np = createPart(cx + rx1, cy + ry1, uint(s.tmp & 0x1FF));
                            if (np != -1)
                            {
                                parts[np].temp = s.temp;
                                parts[np].life = s.tmp2;
                                parts[np].tmp = s.tmp3;
                                parts[np].ctype = s.tmp4;
                                parts[r].tmp = 0;
                                parts[r].life = 10;
                                done = true;
                                break;
                            }
                        }
                    }
                }
                else
                    parts[r].life = 10;
            }
            else if (rt != PT_INWR && (rt != PT_SPRK || parts[r].ctype != int(PT_INWR)) && rt != PT_ARAY &&
                     rt != PT_WIFI && !(rt == PT_SWCH && parts[r].life >= 10))
            {
                // Anything else stops the beam (INWR, ARAY, WIFI and on SWCH let it through), and is sparked
                if (!first)
                    sparkAt(cx, cy, false);
                int ct = parts[r].ctype;
                docontinue = nostop && ptype[r] == PT_SPRK && ct >= 0 && ct < MAX_ELEMENTS &&
                             (elems[ct].properties & PROP_CONDUCTS) != 0;
            }
        }
        else
        {
            if (rt == PT_BRAY)
            {
                parts[r].tmp = 2;
                parts[r].life = 1;
                if (isBlackDeco)
                    parts[r].dcolour = 0xFF000000u;
            }
            else if (rt == PT_STOR || rt == PT_INWR || (rt == PT_SPRK && parts[r].ctype == int(PT_INWR)) ||
                     rt == PT_ARAY || rt == PT_WIFI || rt == PT_FILT || (rt == PT_SWCH && parts[r].life >= 10))
            {
                // An erasing beam goes on through these
                if (rt == PT_STOR)
                {
                    parts[r].tmp = 0;
                    parts[r].life = 0;
                }
                else if (rt == PT_FILT)
                {
                    isBlackDeco = parts[r].dcolour == 0xFF000000u;
                    parts[r].life = 2;
                }
            }
            else
                docontinue = false;
        }
    }
}

// CRAY.cpp's wavelengthToDecoColour: the decoration colour for light of these wavelengths.
uint wavelengthToDecoColour(int wavelength)
{
    int colr = 0, colg = 0, colb = 0;
    for (int b = 0; b < 12; b++)
    {
        colr += (wavelength >> (b + 18)) & 1;
        colb += (wavelength >> b) & 1;
    }
    for (int b = 0; b < 12; b++)
        colg += (wavelength >> (b + 9)) & 1;
    int k = 624 / (colr + colg + colb + 1);
    colr = clamp(colr * k, 0, 255);
    colg = clamp(colg * k, 0, 255);
    colb = clamp(colb * k, 0, 255);
    return (255u << 24) | (uint(colr) << 16) | (uint(colg) << 8) | uint(colb);
}

// CRAY.cpp: a line of its ctype into the empty cells in front of it, tmp2 cells away, at most tmp particles
// (255 if not set). A beam from PSCN destroys what it hits instead, one from INST doesn't stop, one from INWR
// also creates on top of what is there (where the new element may share the cell). FILT colours it.
void castCRAY(Particle em, int x, int y, int rx, int ry, int sc)
{
    uint colored = 0;
    bool destroy = sc == int(PT_PSCN);
    bool nostop = sc == int(PT_INST);
    bool createSpark = sc == int(PT_INWR);
    int partsRemaining = em.tmp != 0 ? em.tmp : 255;
    int spacesRemaining = em.tmp2;
    uint ct = uint(em.ctype) & 0x1FFu;
    int nxi = -rx, nyi = -ry;
    bool docontinue = true;
    for (int nxx = spacesRemaining * nxi, nyy = spacesRemaining * nyi; docontinue; nyy += nyi, nxx += nxi)
    {
        int cx = x + nxi + nxx, cy = y + nyi + nyy;
        if (!inSim(cx, cy))
            break;
        int r = topAt(cx, cy);
        uint rt = r >= 0 ? ptype[r] : PT_NONE;
        if (!isWallBlocking(cx, cy, ct) && (r < 0 || createSpark))
        {
            int nr = createPart(cx, cy, ct);
            if (nr != -1)
            {
                // The rest of the ctype goes to the new particle's Create: FILT's mode, TESC's power, LIFE's rule,
                // LIGH's size
                int v = em.ctype >> 9;
                if (ct == PT_FILT)
                    parts[nr].tmp = v;
                else if (ct == PT_TESC)
                    parts[nr].tmp = min(v, 300);
                else if (ct == PT_LIFE)
                {
                    Particle q = parts[nr];
                    lifeCreate(q, v);
                    parts[nr] = q;
                }
                else if (ct == PT_LIGH)
                {
                    parts[nr].life = min(v, 55);
                    parts[nr].temp = float(parts[nr].life) * 150.0;
                }
                if (colored != 0)
                    parts[nr].dcolour = colored;
                parts[nr].temp = em.temp;
                if (em.life > 0)
                    parts[nr].life = em.life;
                if (--partsRemaining == 0)
                    docontinue = false;
            }
        }
        else if (rt == PT_FILT)
        {
            if (parts[r].dcolour == 0xFF000000u)
                colored = 0xFF000000u;
            else if (parts[r].tmp == 0)
                colored = wavelengthToDecoColour(filtWavelengths(parts[r]));
            else if (colored == 0xFF000000u)
                colored = 0;
            parts[r].life = 4;
        }
        else if (rt == PT_CRAY || nostop)
            docontinue = true;
        else if (destroy && r >= 0 && rt != PT_DMND)
        {
            killPart(uint(r), cx, cy);
            if (--partsRemaining == 0)
                docontinue = false;
        }
        else
            docontinue = false;
        if (partsRemaining == 0)
            docontinue = false;
    }
}

// DRAY.cpp: copies the line of particles in front of it further along that line. tmp is how many to copy
// (0: up to the first particle of its ctype), tmp2 the gap left before the copy. A beam from PSCN overwrites
// what is in the way of the copy.
void castDRAY(Particle em, int x, int y, int rx, int ry, int sc)
{
    int ctype = em.ctype & 0x1FF, ctypeExtra = em.ctype >> 9;
    int copyLength = max(em.tmp, 0), copySpaces = max(em.tmp2, 0);
    if (copyLength > 0)
        copySpaces++; // (TPT's "strange hack")
    bool overwrite = sc == int(PT_PSCN);
    int partsRemaining = copyLength, xCopyTo = -1, yCopyTo = -1;
    int localCopyLength = copyLength;
    int xStep = -rx, yStep = -ry;

    // Where the copying will start: the first particle seen decides whether energy particles are copied
    bool foundParticle = false;
    bool energyLine = false;
    for (int xc = x + xStep, yc = y + yStep;; xc += xStep, yc += yStep)
    {
        if (!inSim(xc, yc))
            break;
        if (!foundParticle)
        {
            if (topAt(xc, yc) >= 0)
                foundParticle = true;
            else if (photonAt(xc, yc) >= 0)
                foundParticle = energyLine = true;
        }
        int rr = ((ctype != 0 && isEnergy(uint(ctype))) || energyLine) ? photonAt(xc, yc) : topAt(xc, yc);
        int rrt = rr >= 0 ? int(ptype[rr]) : 0;
        // Stop at a particle of its ctype when tmp isn't set, else when tmp particles are covered
        bool stop = localCopyLength == 0 && rrt == ctype && (ctype != int(PT_LIFE) || parts[rr].ctype == ctypeExtra);
        if (!stop)
            stop = --partsRemaining == 0;
        if (stop)
        {
            localCopyLength -= partsRemaining;
            xCopyTo = xc + xStep * copySpaces;
            yCopyTo = yc + yStep * copySpaces;
            break;
        }
    }

    // Now copy the particles
    partsRemaining = localCopyLength + 1;
    for (int xc = x + xStep, yc = y + yStep; inSim(xCopyTo, yCopyTo) && --partsRemaining != 0;
         xc += xStep, yc += yStep, xCopyTo += xStep, yCopyTo += yStep)
    {
        int src = energyLine ? photonAt(xc, yc) : topAt(xc, yc);
        uint type = src >= 0 ? ptype[src] : PT_NONE;
        if (overwrite)
        {
            int victim = energyLine ? photonAt(xCopyTo, yCopyTo) : topAt(xCopyTo, yCopyTo);
            if (victim >= 0)
                killPart(uint(victim), xCopyTo, yCopyTo);
        }
        int np;
        if (type == PT_SPRK)
            np = createPart(xCopyTo, yCopyTo, PT_METL); // spark hack
        else if (type == PT_STKM || type == PT_STKM2 || type == PT_NONE)
            continue; // (stick men can't be copied)
        else
            np = createPart(xCopyTo, yCopyTo, type);
        if (np >= 0)
        {
            if (type == PT_SPRK)
                partChangeType(uint(np), PT_SPRK);
            Particle c = parts[src];
            if (type == PT_FIGH)
                c.tmp = parts[np].tmp; // (a fighter keeps its own slot; FIGH comes later)
            c.x = float(xCopyTo);
            c.y = float(yCopyTo);
            c.pass = 0xFFFFFFFFu;
            parts[np] = c;
        }
    }
}

// LIGH.cpp's create_LIGH: one point of a stretch of lightning. The last point starts the next segment, which
// branches (tmp2 2) or bends (0) while it has the power (life) for it. Returns true where the lightning stops:
// off the world, or eaten by VOID, PVOD or a black hole. (TPT holds back segments with a higher id than their
// parent for a tick, as they would otherwise be updated in the same tick; here none are updated before the next.)
bool createLighPoint(int x, int y, Particle parent, float temp, int life, int angle, bool last)
{
    int np = createPart(x, y, PT_LIGH);
    if (np != -1)
    {
        parts[np].temp = temp;
        parts[np].tmp = angle;
        parts[np].dcolour = parent.dcolour;
        if (last)
        {
            int nextLife = int(float(life) / 1.5 - float(rngBetween(0, 1)));
            parts[np].life = nextLife;
            if (nextLife > 1)
                parts[np].tmp2 = rngChance(7, 10) ? 2 : 0; // branch or bend
            else
                parts[np].tmp2 = 7; // not enough energy to go on
        }
        else
        {
            parts[np].life = life;
            parts[np].tmp2 = 7;
        }
        return false;
    }
    if (!inSim(x, y))
        return true;
    int r = topAt(x, y);
    if (r < 0)
        return false;
    uint rt = ptype[r];
    Particle q = parts[r];
    bool voidEats = (rt == PT_VOID || (rt == PT_PVOD && q.life >= 10)) &&
                    (q.ctype == 0 || (q.ctype == int(PT_LIGH)) != ((q.tmp & 1) != 0));
    return voidEats || rt == PT_BHOL || rt == PT_NBHL;
}

// LIGH.cpp's create_line_par: lightning from (x1, y1) to (x2, y2).
void lighLine(int x1, int y1, int x2, int y2, Particle parent, int angle)
{
    float temp = parent.temp;
    int life = parent.life;
    bool reverseXY = abs(y2 - y1) > abs(x2 - x1);
    if (reverseXY)
    {
        int t = x1;
        x1 = y1;
        y1 = t;
        t = x2;
        x2 = y2;
        y2 = t;
    }
    bool back = x1 > x2;
    int dx = x2 - x1;
    int dy = abs(y2 - y1);
    float de = dx != 0 ? float(dy) / float(dx) : 0.0;
    float e = 0.0;
    int y = y1;
    int ystep = y1 < y2 ? 1 : -1;
    if (!back)
    {
        for (int x = x1; x <= x2; x++)
        {
            if (reverseXY ? createLighPoint(y, x, parent, temp, life, angle, x == x2)
                          : createLighPoint(x, y, parent, temp, life, angle, x == x2))
                return;
            e += de;
            if (e >= 0.5)
            {
                y += ystep;
                e -= 1.0;
            }
        }
    }
    else
    {
        for (int x = x1; x >= x2; x--)
        {
            if (reverseXY ? createLighPoint(y, x, parent, temp, life, angle, x == x2)
                          : createLighPoint(x, y, parent, temp, life, angle, x == x2))
                return;
            e += de;
            if (e <= -0.5)
            {
                y += ystep;
                e += 1.0;
            }
        }
    }
}

void castRequest(uint k)
{
    uint base = 4 * pc.CW * pc.CH + 3 * k;
    uint id = emapReqs[base];
    uint kind = emapReqs[base + 1];
    uint v = emapReqs[base + 2];
    rngSeed(id ^ hash32(k + 0x52A1u));
    if (kind == POST_LIGH)
    {
        // The segment's state as it was in its update (LIGH doesn't move or conduct heat)
        Particle parent = parts[id];
        int x = int(parent.x + 0.5), y = int(parent.y + 0.5);
        int angle = int(v & 0x3FFu) - 360;
        int mult = int(v >> 10);
        float a = float(angle) * 3.14159265 / 180.0;
        lighLine(x, y, x + int(cos(a) * float(mult)), y + int(-sin(a) * float(mult)), parent, angle);
        return;
    }
    uint t = ptype[id];
    if (t != PT_ARAY && t != PT_CRAY && t != PT_DRAY)
        return; // the emitter was destroyed after its update
    int dir = int(v & 0xFFu);
    int rx = dir % 3 - 1, ry = dir / 3 - 1;
    int sc = int(v >> 8);
    Particle em = parts[id];
    int x = int(em.x + 0.5), y = int(em.y + 0.5);
    if (t == PT_ARAY)
        castARAY(em, x, y, rx, ry, sc);
    else if (t == PT_CRAY)
        castCRAY(em, x, y, rx, ry, sc);
    else
        castDRAY(em, x, y, rx, ry, sc);
}

[shader("compute")]
[numthreads(64, 1, 1)]
void main(uint3 tid: SV_DispatchThreadID)
{
    uint n = min(counters[CTR_RAY_REQ], postCapacity());
    if ((pc.mode & 2) != 0)
    {
        if (tid.x == 0)
            for (uint k = 0; k < n; k++)
                castRequest(k);
        return;
    }
    if (tid.x < n)
        castRequest(tid.x);
}
