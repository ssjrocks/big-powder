// The stick men: STKM and STKM2 (the players, steered from the keyboard), FIGH (fighters, which steer themselves)
// and the players' spawn points SPAWN and SPAWN2. Their state is TPT's playerst, kept in `stickmen` (common.slang).
//
// One thread does it all after the Game of Life and before the colour passes, in id order:
//  - taking stock of which stick men exist. TPT does that when they are created and killed (their ChangeType
//    functions); here the particles prepare listed are checked against the slots each tick, so a stick man made
//    or killed anywhere is picked up or let go here;
//  - respawning STKM and STKM2 at their spawn points (BeforeSim);
//  - their update: the fighters' aim (FIGH.cpp) and STKM.cpp's run_stickman (legs, keys, shooting, damage). TPT
//    runs it in the particle's own update, between its heat and its movement; here the head then moves in its
//    colour pass (stickmanMove in move.slang). STKM's air loss is 1, so its velocity comes out the same.
// Particles a stick man makes join this tick's colour lists when TPT's loop would still reach them (a higher id).
#include "common.slang"
#include "elements.slang"
#include "stickman_init.slang"

static const float STKM_DT = 0.9;

uint slotType(uint s) { return s == 0 ? PT_STKM : s == 1 ? PT_STKM2 : PT_FIGH; }

bool isElementOrNone(int t) { return t == 0 || (t > 0 && t < int(MAX_ELEMENTS) && elems[t].existsInTpt != 0); }

// create_part(-1, ...) by stick man i
int stkmCreate(int x, int y, int t, uint i)
{
    if (t <= 0 || t >= int(MAX_ELEMENTS))
        return -1;
    int np = createPart(x, y, uint(t));
    if (np > int(i))
        enlistNew(uint(np), x, y);
    return np;
}

// Element_STKM_set_element: what it picks up from what it touches (powders, liquids, gases, energy, LOVE/LOLZ,
// lightning from TESC and LIGH)
void setElement(inout Stickman st, int element)
{
    if (element < 0 || element >= int(MAX_ELEMENTS))
        return;
    ElementProps e = elems[element];
    if (e.falldown != 0 || (e.properties & (TYPE_GAS | TYPE_LIQUID | TYPE_ENERGY)) != 0 || element == int(PT_LOLZ) ||
        element == int(PT_LOVE))
    {
        if (st.rocketBoots == 0 || element != int(PT_PLSM))
        {
            st.elem = element;
            st.fan = 0;
        }
    }
    if (element == int(PT_TESC) || element == int(PT_LIGH))
    {
        st.elem = int(PT_LIGH);
        st.fan = 0;
    }
}

// die(): a gust of air with the fan, otherwise a burst of its element; then it is killed.
void die(inout Stickman st, uint i, Particle p)
{
    int x = int(p.x + 0.5);
    int y = int(p.y + 0.5);
    if (st.fan != 0)
        airAddPV(x / CELL, y / CELL, 64.0);
    else
        for (int r = -2; r <= 1; r++)
        {
            stkmCreate(x + r, y - 2, st.elem, i);
            stkmCreate(x + r + 1, y + 2, st.elem, i);
            stkmCreate(x - 2, y + r + 1, st.elem, i);
            stkmCreate(x + 2, y + r, st.elem, i);
        }
    killPart(i, x, y);
    st.spwn = 0; // (its ChangeType)
    st.partRef = 0;
}

// Element_STKM_interact: what a foot (or a portal near the head) at (x, y) does to it. True if it is gone.
bool interact(inout Stickman st, uint i, uint t, inout Particle p, int x, int y)
{
    if (!inSim(x, y))
        return false;
    int rid = pmapId(pmap[cellIndex(x, y)]);
    if (rid < 0)
        return false;
    uint rt = ptype[rid];
    if (rt == PT_NONE)
        return false;
    Particle q = parts[rid];
    int damage = 0;
    if (rt == PT_SPRK && st.elem != int(PT_LIGH))
        damage += rngBetween(32, 51); // on a spark
    if (!isHeatInsulator(rt, q.life, q.tmp) && ((st.elem != int(PT_LIGH) && q.temp >= 323.0) || q.temp <= 243.0) &&
        (st.rocketBoots == 0 || rt != PT_PLSM))
    {
        damage += 2;
        st.accs[3] -= 1.0;
    }
    uint props = elems[rt].properties;
    if ((props & PROP_DEADLY) != 0)
        damage += rt == PT_ACID ? 5 : 1;
    if ((props & PROP_RADIOACTIVE) != 0)
        damage++;
    if (damage != 0)
    {
        if (damage > p.life)
        {
            die(st, i, p);
            return true;
        }
        p.life -= damage;
    }
    if (rt == PT_PRTI)
    {
        // Into the portal (out of a PRTO, direction 1). It stays out while inside, so it doesn't respawn.
        int ch = portalChannel(q.temp);
        parts[rid].tmp = ch;
        Particle stored = p;
        stored.pass = 0;
        stored.pad1 = 0;
        if (portalStore(uint(ch), 1u, stored, t))
        {
            killPart(i, int(p.x + 0.5), int(p.y + 0.5));
            st.spwn = 1;
            st.partRef = STKM_IN_PORTAL;
            return true;
        }
    }
    if (rt == PT_BHOL || rt == PT_NBHL)
    {
        if (params[0].legacyHeat == 0)
            parts[rid].temp = clamp(q.temp + p.temp / 2.0, MIN_TEMP, MAX_TEMP);
        killPart(i, int(p.x + 0.5), int(p.y + 0.5));
        st.spwn = 0;
        st.partRef = 0;
        return true;
    }
    if ((rt == PT_VOID || (rt == PT_PVOD && q.life == 10)) &&
        (q.ctype == 0 || (q.ctype == int(t)) != ((q.tmp & 1) != 0)))
    {
        killPart(i, int(p.x + 0.5), int(p.y + 0.5));
        st.spwn = 0;
        st.partRef = 0;
        return true;
    }
    return false;
}

bool canStep(uint t, float lx, float ly)
{
    // InBounds(int(leg x), int(leg y)) && !eval_move(t, ...): the foot is on something
    int fx = int(lx), fy = int(ly);
    return inSim(fx, fy) && evalMove(t, fx, fy) == 0;
}

float sq(float v) { return v * v; }

// Element_STKM_run_stickman. False if it died or left.
bool runStickman(inout Stickman st, uint i)
{
    uint t = ptype[i];
    Particle p = parts[i];
    int x = int(p.x + 0.5);
    int y = int(p.y + 0.5);
    float dt = STKM_DT;
    float rbHead = 0.35, rbFeet = 0.15;
    float rbHeadV = 0.3, rbFeetV = 0.45; // stronger vertically, against gravity

    if (st.fan == 0 && p.ctype != 0 && isElementOrNone(p.ctype))
        setElement(st, p.ctype);
    st.frames++;

    // Temperature
    if (p.temp < 243.0)
        p.life -= 1;
    if (p.temp < 309.6 && p.temp >= 243.0)
        p.temp += 1.0;

    // Death: no health left, or a strong wind
    if (p.life < 1 || (airPV(airIndex(x, y)) >= 4.5 && st.fan == 0))
    {
        die(st, i, p);
        return false;
    }

    // Follow gravity
    float gvx = 0.0, gvy = 0.0;
    switch (params[0].gravityMode)
    {
    case 1: // off
        break;
    case 2: // radial
    {
        float gravd = 0.01 - sqrt(sq(p.x - float(pc.W / 2)) + sq(p.y - float(pc.H / 2)));
        gvx = (p.x - float(pc.W / 2)) / gravd;
        gvy = (p.y - float(pc.H / 2)) / gravd;
        break;
    }
    case 3: // custom
        gvx = params[0].customGravX;
        gvy = params[0].customGravY;
        break;
    default:
        gvy = 1.0;
        break;
    }
    float2 ng = gravForce(int(p.x), int(p.y)); // Newtonian gravity
    gvx += ng.x;
    gvy += ng.y;

    float mvx = gvx, mvy = gvy;
    bool rbLowGrav = false;
    float tmp = max(abs(mvx), abs(mvy));
    if (tmp < 0.001)
    {
        rbLowGrav = true;
        mvx = -p.vx;
        mvy = -p.vy;
        tmp = max(abs(mvx), abs(mvy));
    }
    if (tmp < 0.001)
    {
        mvx = 0.0;
        mvy = 1.0;
        tmp = 1.0;
    }
    else
        tmp = 1.0 / sqrt(mvx * mvx + mvy * mvy);
    mvx *= tmp; // a unit vector
    mvy *= tmp;
    if (rbLowGrav)
    {
        rbHeadV = rbHead;
        rbFeetV = rbFeet;
    }

    p.vx -= gvx * dt; // head up
    p.vy -= gvy * dt;

    // Verlet integration
    float pp;
    pp = 2.0 * st.legs[0] - st.legs[2] + st.accs[0] * dt * dt;
    st.legs[2] = st.legs[0];
    st.legs[0] = pp;
    pp = 2.0 * st.legs[1] - st.legs[3] + st.accs[1] * dt * dt;
    st.legs[3] = st.legs[1];
    st.legs[1] = pp;

    pp = 2.0 * st.legs[4] - st.legs[6] + (st.accs[2] + gvx) * dt * dt;
    st.legs[6] = st.legs[4];
    st.legs[4] = pp;
    pp = 2.0 * st.legs[5] - st.legs[7] + (st.accs[3] + gvy) * dt * dt;
    st.legs[7] = st.legs[5];
    st.legs[5] = pp;

    pp = 2.0 * st.legs[8] - st.legs[10] + st.accs[4] * dt * dt;
    st.legs[10] = st.legs[8];
    st.legs[8] = pp;
    pp = 2.0 * st.legs[9] - st.legs[11] + st.accs[5] * dt * dt;
    st.legs[11] = st.legs[9];
    st.legs[9] = pp;

    pp = 2.0 * st.legs[12] - st.legs[14] + (st.accs[6] + gvx) * dt * dt;
    st.legs[14] = st.legs[12];
    st.legs[12] = pp;
    pp = 2.0 * st.legs[13] - st.legs[15] + (st.accs[7] + gvy) * dt * dt;
    st.legs[15] = st.legs[13];
    st.legs[13] = pp;

    for (int k = 0; k < 8; k++)
        st.accs[k] = 0.0;

    float gx = (st.legs[4] + st.legs[12]) / 2.0 - gvy;
    float gy = (st.legs[5] + st.legs[13]) / 2.0 + gvx;
    float dl = sq(gx - st.legs[4]) + sq(gy - st.legs[5]);
    float dr = sq(gx - st.legs[12]) + sq(gy - st.legs[13]);

    uint comm = st.comm;
    // Go left
    if ((comm & 1) != 0)
    {
        bool moved = false;
        if (dl > dr)
        {
            if (canStep(t, st.legs[4], st.legs[5]))
            {
                st.accs[2] = -3.0 * mvy - 3.0 * mvx;
                st.accs[3] = 3.0 * mvx - 3.0 * mvy;
                st.accs[0] = -mvy;
                st.accs[1] = mvx;
                moved = true;
            }
        }
        else if (canStep(t, st.legs[12], st.legs[13]))
        {
            st.accs[6] = -3.0 * mvy - 3.0 * mvx;
            st.accs[7] = 3.0 * mvx - 3.0 * mvy;
            st.accs[0] = -mvy;
            st.accs[1] = mvx;
            moved = true;
        }
        if (!moved && st.rocketBoots != 0)
        {
            p.vx -= rbHead * mvy;
            p.vy += rbHead * mvx;
            st.accs[2] -= rbFeet * mvy;
            st.accs[6] -= rbFeet * mvy;
            st.accs[3] += rbFeet * mvx;
            st.accs[7] += rbFeet * mvx;
            for (int leg = 0; leg < 2; leg++)
            {
                if (leg == 1 && (comm & 2) != 0)
                    continue;
                int np = stkmCreate(int(st.legs[leg * 8 + 4]), int(st.legs[leg * 8 + 5]), int(PT_PLSM), i);
                if (np >= 0)
                {
                    parts[np].vx = p.vx + mvy * 25.0;
                    parts[np].vy = p.vy - mvx * 25.0;
                    parts[np].life += 30;
                }
            }
        }
    }

    // Go right
    if ((comm & 2) != 0)
    {
        bool moved = false;
        if (dl < dr)
        {
            if (canStep(t, st.legs[4], st.legs[5]))
            {
                st.accs[2] = 3.0 * mvy - 3.0 * mvx;
                st.accs[3] = -3.0 * mvx - 3.0 * mvy;
                st.accs[0] = mvy;
                st.accs[1] = -mvx;
                moved = true;
            }
        }
        else if (canStep(t, st.legs[12], st.legs[13]))
        {
            st.accs[6] = 3.0 * mvy - 3.0 * mvx;
            st.accs[7] = -3.0 * mvx - 3.0 * mvy;
            st.accs[0] = mvy;
            st.accs[1] = -mvx;
            moved = true;
        }
        if (!moved && st.rocketBoots != 0)
        {
            p.vx += rbHead * mvy;
            p.vy -= rbHead * mvx;
            st.accs[2] += rbFeet * mvy;
            st.accs[6] += rbFeet * mvy;
            st.accs[3] -= rbFeet * mvx;
            st.accs[7] -= rbFeet * mvx;
            for (int leg = 0; leg < 2; leg++)
            {
                if (leg == 0 && (comm & 1) != 0)
                    continue;
                int np = stkmCreate(int(st.legs[leg * 8 + 4]), int(st.legs[leg * 8 + 5]), int(PT_PLSM), i);
                if (np >= 0)
                {
                    parts[np].vx = p.vx - mvy * 25.0;
                    parts[np].vy = p.vy + mvx * 25.0;
                    parts[np].life += 30;
                }
            }
        }
    }

    if (st.rocketBoots != 0 && (comm & 3) == 3)
    {
        // Left and right together with rocket boots: brakes (useful without gravity)
        p.vx *= 0.5;
        p.vy *= 0.5;
        st.accs[2] = 0.0;
        st.accs[6] = 0.0;
        st.accs[3] = 0.0;
        st.accs[7] = 0.0;
    }

    // Jump
    if ((comm & 4) != 0)
    {
        if (st.rocketBoots != 0)
        {
            p.vx -= rbHeadV * mvx;
            p.vy -= rbHeadV * mvy;
            st.accs[2] -= rbFeetV * mvx;
            st.accs[6] -= rbFeetV * mvx;
            st.accs[3] -= rbFeetV * mvy;
            st.accs[7] -= rbFeetV * mvy;
            for (int leg = 0; leg < 2; leg++)
            {
                int np = stkmCreate(int(st.legs[leg * 8 + 4]), int(st.legs[leg * 8 + 5]) + 1, int(PT_PLSM), i);
                if (np >= 0)
                {
                    parts[np].vx = p.vx + mvx * 30.0;
                    parts[np].vy = p.vy + mvy * 30.0;
                    parts[np].life += 10;
                }
            }
        }
        else if (canStep(t, st.legs[4], st.legs[5]) || canStep(t, st.legs[12], st.legs[13]))
        {
            p.vx -= 4.0 * mvx;
            p.vy -= 4.0 * mvy;
            st.accs[2] -= mvx;
            st.accs[6] -= mvx;
            st.accs[3] -= mvy;
            st.accs[7] -= mvy;
        }
    }

    // A foot in a detector wall charges it
    {
        int cx = int(st.legs[4] + 0.5) / CELL, cy = int(st.legs[5] + 0.5) / CELL;
        if (cx >= 0 && cy >= 0 && cx < int(pc.CW) && cy < int(pc.CH) && bmapCell(cx, cy) == WL_DETECT)
            requestEmap(int(st.legs[4]) / CELL, int(st.legs[5]) / CELL);
        cx = int(st.legs[12] + 0.5) / CELL;
        cy = int(st.legs[13] + 0.5) / CELL;
        if (cx >= 0 && cy >= 0 && cx < int(pc.CW) && cy < int(pc.CH) && bmapCell(cx, cy) == WL_DETECT)
            requestEmap(cx, cy);
    }

    // What is near the head
    for (int rx = -2; rx < 3; rx++)
        for (int ry = -2; ry < 3; ry++)
        {
            int ax = x + rx, ay = y + ry;
            if (!(ax >= 0 && ay > 0 && ax < int(pc.W) && ay < int(pc.H) && (rx != 0 || ry != 0)))
                continue;
            uint c = cellIndex(ax, ay);
            int rid = pmapId(pmap[c]);
            if (rid < 0)
                rid = pmapId(photons[c]);
            uint wall = bmapAt(ax, ay);
            if (rid < 0 && wall == 0)
                continue;
            uint rt = rid >= 0 ? ptype[rid] : PT_NONE;
            setElement(st, int(rt));
            if (rt == PT_PLNT && p.life < 100)
            {
                // a plant gives 5 health
                if (p.life <= 95)
                    p.life += 5;
                else
                    p.life = 100;
                killPart(uint(rid), ax, ay);
            }
            if (rt == PT_NEUT)
            {
                if (p.life <= 100)
                    p.life -= (102 - p.life) / 2;
                else
                    p.life = int(float(p.life) * 0.9);
                killPart(uint(rid), ax, ay);
            }
            if (wall == WL_FAN)
                st.fan = 1;
            else if (wall == WL_EHOLE)
                st.rocketBoots = 0;
            else if (wall == WL_GRAV)
                st.rocketBoots = 1;
            // (TPT passes the offsets here, not the position, so this looks at the corner of the world)
            if (rt == PT_PRTI && interact(st, i, t, p, rx, ry))
                return false;
        }

    // Where it shoots from: in front of the head
    uint pcomm = st.pcomm;
    int fx = x + 3 * int((pcomm & 2) == 2) - 3 * int((pcomm & 1) == 1);
    int fy = y - 3 * int(pcomm == 0);

    // Use its element
    if ((comm & 8) != 0)
    {
        fy -= 2 * rngBetween(0, 1) + 1;
        int rid = inSim(fx, fy) ? pmapId(pmap[cellIndex(fx, fy)]) : -1;
        uint rt = rid >= 0 ? ptype[rid] : PT_NONE;
        if ((elems[rt].properties & TYPE_SOLID) != 0)
        {
            sparkAt(fx, fy, false);
            st.frames = 0;
        }
        else
        {
            int np = -1;
            int dir = int((pcomm & 2) == 2) - int((pcomm & 1) == 1);
            if (st.fan != 0)
            {
                for (int j = -4; j < 5; j++)
                    for (int k = -4; k < 5; k++)
                    {
                        int airx = fx + 3 * dir + j;
                        int airy = fy + k;
                        airAddPV(airx / CELL, airy / CELL, 0.03);
                        if (airy + CELL < int(pc.H))
                            airAddPV(airx / CELL, airy / CELL + 1, 0.03);
                        if (airx + CELL < int(pc.W))
                        {
                            airAddPV(airx / CELL + 1, airy / CELL, 0.03);
                            if (airy + CELL < int(pc.H))
                                airAddPV(airx / CELL + 1, airy / CELL + 1, 0.03);
                        }
                    }
            }
            else if (st.elem == int(PT_LIGH) && st.frames < 30)
                np = -1; // lightning only so often
            else
                np = stkmCreate(fx, fy, st.elem, i);
            if (np >= 0)
            {
                if (st.elem == int(PT_PHOT))
                {
                    int random = abs(rngBetween(-1, 1)) * 3;
                    if (random == 0)
                        killPart(uint(np), fx, fy);
                    else
                    {
                        parts[np].vy = 0.0;
                        if ((pcomm & 3) != 0)
                            parts[np].vx = float(dir * random);
                        else
                            parts[np].vx = float(random);
                    }
                }
                else if (st.elem == int(PT_LIGH))
                {
                    int angle;
                    int power = 100;
                    if (gvx != 0.0 || gvy != 0.0)
                        angle = int(atan2(mvx, mvy) * 180.0 / 3.14159265);
                    else
                        angle = rngBetween(0, 359);
                    if ((pcomm & 1) != 0)
                        angle += 180;
                    if (angle > 360)
                        angle -= 360;
                    if (angle < 0)
                        angle += 360;
                    parts[np].tmp = angle;
                    parts[np].life = rngBetween(0, 1 + power / 15) + power / 7;
                    parts[np].temp = float(parts[np].life * power) / 2.5;
                    parts[np].tmp2 = 1;
                }
                else if (st.fan == 0)
                {
                    parts[np].vx -= -mvy * float(5 * dir);
                    parts[np].vy -= mvx * float(5 * dir);
                    p.vx -= float(elems[st.elem].weight) * parts[np].vx / 1000.0;
                }
                st.frames = 0;
            }
        }
    }

    // Joints
    float d = 25.0 / (sq(st.legs[0] - st.legs[4]) + sq(st.legs[1] - st.legs[5]) + 25.0) - 0.5;
    st.legs[4] -= (st.legs[0] - st.legs[4]) * d;
    st.legs[5] -= (st.legs[1] - st.legs[5]) * d;
    st.legs[0] += (st.legs[0] - st.legs[4]) * d;
    st.legs[1] += (st.legs[1] - st.legs[5]) * d;

    d = 25.0 / (sq(st.legs[8] - st.legs[12]) + sq(st.legs[9] - st.legs[13]) + 25.0) - 0.5;
    st.legs[12] -= (st.legs[8] - st.legs[12]) * d;
    st.legs[13] -= (st.legs[9] - st.legs[13]) * d;
    st.legs[8] += (st.legs[8] - st.legs[12]) * d;
    st.legs[9] += (st.legs[9] - st.legs[13]) * d;

    d = 36.0 / (sq(st.legs[0] - p.x) + sq(st.legs[1] - p.y) + 36.0) - 0.5;
    p.vx -= (st.legs[0] - p.x) * d;
    p.vy -= (st.legs[1] - p.y) * d;
    st.legs[0] += (st.legs[0] - p.x) * d;
    st.legs[1] += (st.legs[1] - p.y) * d;

    d = 36.0 / (sq(st.legs[8] - p.x) + sq(st.legs[9] - p.y) + 36.0) - 0.5;
    p.vx -= (st.legs[8] - p.x) * d;
    p.vy -= (st.legs[9] - p.y) * d;
    st.legs[8] += (st.legs[8] - p.x) * d;
    st.legs[9] += (st.legs[9] - p.y) * d;

    // A foot inside something steps back, or pops out
    if (canStep(t, st.legs[4], st.legs[5]))
    {
        st.legs[4] = st.legs[6];
        st.legs[5] = st.legs[7];
    }
    if (canStep(t, st.legs[12], st.legs[13]))
    {
        st.legs[12] = st.legs[14];
        st.legs[13] = st.legs[15];
    }
    if (canStep(t, st.legs[4], st.legs[5]))
    {
        float sw = st.legs[4];
        st.legs[4] = st.legs[6];
        st.legs[6] = sw;
        sw = st.legs[5];
        st.legs[5] = st.legs[7];
        st.legs[7] = sw;
    }
    if (canStep(t, st.legs[12], st.legs[13]))
    {
        float sw = st.legs[12];
        st.legs[12] = st.legs[14];
        st.legs[14] = sw;
        sw = st.legs[13];
        st.legs[13] = st.legs[15];
        st.legs[15] = sw;
    }

    // Keeping the legs apart
    if (sq(st.legs[4] - st.legs[12]) + sq(st.legs[5] - st.legs[13]) < 16.0)
    {
        float tvx = -gvy, tvy = gvx;
        if (tvx != 0.0 || tvy != 0.0)
        {
            float h = sqrt(tvx * tvx + tvy * tvy);
            st.accs[2] -= 0.2 * tvx / h;
            st.accs[3] -= 0.2 * tvy / h;
            st.accs[6] += 0.2 * tvx / h;
            st.accs[7] += 0.2 * tvy / h;
        }
    }
    if (sq(st.legs[0] - st.legs[8]) + sq(st.legs[1] - st.legs[9]) < 16.0)
    {
        float tvx = -gvy, tvy = gvx;
        if (tvx != 0.0 || tvy != 0.0)
        {
            float h = sqrt(tvx * tvx + tvy * tvy);
            st.accs[0] -= 0.2 * tvx / h;
            st.accs[1] -= 0.2 * tvy / h;
            st.accs[4] += 0.2 * tvx / h;
            st.accs[5] += 0.2 * tvy / h;
        }
    }

    // What the feet touch
    if (interact(st, i, t, p, int(st.legs[4] + 0.5), int(st.legs[5] + 0.5)) ||
        interact(st, i, t, p, int(st.legs[12] + 0.5), int(st.legs[13] + 0.5)) ||
        interact(st, i, t, p, int(st.legs[4] + 0.5), int(st.legs[5])) ||
        interact(st, i, t, p, int(st.legs[12] + 0.5), int(st.legs[13])))
        return false;

    p.ctype = st.elem;
    parts[i].life = p.life;
    parts[i].temp = p.temp;
    parts[i].vx = p.vx;
    parts[i].vy = p.vy;
    parts[i].ctype = p.ctype;
    return true;
}

// FIGH.cpp's update before run_stickman: head for the nearer player; shoot when close with something dangerous,
// otherwise walk, and jump over what is in the way.
void fighterAim(inout Stickman figh, uint i, int x, int y)
{
    int tarx = 0, tary = 0;
    int seek = 0;
    Stickman p1 = stickmen[0], p2 = stickmen[1];
    if (p2.spwn != 0)
    {
        if (p1.spwn != 0 && sq(p1.legs[2] - float(x)) + sq(p1.legs[3] - float(y)) <=
                                sq(p2.legs[2] - float(x)) + sq(p2.legs[3] - float(y)))
        {
            tarx = int(p1.legs[2]);
            tary = int(p1.legs[3]);
        }
        else
        {
            tarx = int(p2.legs[2]);
            tary = int(p2.legs[3]);
        }
        seek = 1;
    }
    else if (p1.spwn != 0)
    {
        tarx = int(p1.legs[2]);
        tary = int(p1.legs[3]);
        seek = 1;
    }
    parts[i].tmp2 = seek;

    if (seek == 1)
    {
        if (sq(float(tarx - x)) + sq(float(tary - y)) < 600.0)
        {
            int el = figh.elem;
            ElementProps e = elems[clamp(el, 0, int(MAX_ELEMENTS) - 1)];
            if (el == int(PT_LIGH) || el == int(PT_NEUT) || (e.properties & (PROP_DEADLY | PROP_RADIOACTIVE)) != 0 ||
                e.defaultTemp >= 323.0 || e.defaultTemp <= 243.0)
                figh.comm |= 8;
        }
        else if (tarx < x)
        {
            if (figh.rocketBoots != 0 ||
                !(evalMove(PT_FIGH, int(figh.legs[4]) - 10, int(figh.legs[5]) + 6) != 0 &&
                  evalMove(PT_FIGH, int(figh.legs[4]) - 10, int(figh.legs[5]) + 3) != 0))
                figh.comm = 1;
            else
                figh.comm = 2;
            if (figh.rocketBoots != 0)
            {
                if (tary < y)
                    figh.comm |= 4;
            }
            else if (evalMove(PT_FIGH, int(figh.legs[4]) - 4, int(figh.legs[5]) - 1) == 0 ||
                     evalMove(PT_FIGH, int(figh.legs[12]) - 4, int(figh.legs[13]) - 1) == 0 ||
                     evalMove(PT_FIGH, 2 * int(figh.legs[4]) - int(figh.legs[6]), int(figh.legs[5]) + 5) != 0)
                figh.comm |= 4;
        }
        else
        {
            if (figh.rocketBoots != 0 ||
                !(evalMove(PT_FIGH, int(figh.legs[12]) + 10, int(figh.legs[13]) + 6) != 0 &&
                  evalMove(PT_FIGH, int(figh.legs[12]) + 10, int(figh.legs[13]) + 3) != 0))
                figh.comm = 2;
            else
                figh.comm = 1;
            if (figh.rocketBoots != 0)
            {
                if (tary < y)
                    figh.comm |= 4;
            }
            else if (evalMove(PT_FIGH, int(figh.legs[4]) + 4, int(figh.legs[5]) - 1) == 0 ||
                     evalMove(PT_FIGH, int(figh.legs[4]) + 4, int(figh.legs[5]) - 1) == 0 ||
                     evalMove(PT_FIGH, 2 * int(figh.legs[12]) - int(figh.legs[14]), int(figh.legs[13]) + 5) != 0)
                figh.comm |= 4; // (TPT checks the left foot twice here)
        }
    }
    else
        figh.comm = 0;
    figh.pcomm = figh.comm;
}

void killListed(uint id)
{
    killPart(id, int(parts[id].x + 0.5), int(parts[id].y + 0.5));
}

[shader("compute")]
[numthreads(1, 1, 1)]
void main()
{
    uint n = min(counters[CTR_STKM_COUNT], STKM_LIST_CAP);
    // In id order (insertion sort; there are few)
    for (uint a = 1; a < n; a++)
    {
        uint v = counters[CTR_STKM_LIST + a];
        int b = int(a) - 1;
        while (b >= 0 && counters[CTR_STKM_LIST + uint(b)] > v)
        {
            counters[CTR_STKM_LIST + uint(b) + 1] = counters[CTR_STKM_LIST + uint(b)];
            b--;
        }
        counters[CTR_STKM_LIST + uint(b + 1)] = v;
    }

    // Let go of stick men whose particle was killed or changed into something else (their ChangeType), and of
    // spawn points that are gone.
    for (uint s = 0; s < STICKMEN; s++)
    {
        if (stickmen[s].spwn == 0)
            continue;
        uint ref = stickmen[s].partRef;
        if (ref == STKM_IN_PORTAL || ref == STKM_LEAKED)
            continue;
        uint t = slotType(s);
        if (ref == 0 || ptype[ref - 1] != t || (t == PT_FIGH && parts[ref - 1].tmp != int(s) - 2))
        {
            stickmen[s].spwn = 0;
            stickmen[s].partRef = 0;
        }
    }
    for (uint s = 0; s < 2; s++)
    {
        uint ref = stickmen[s].spawnRef;
        if (ref != 0 && ptype[ref - 1] != (s == 0 ? PT_SPAWN : PT_SPAWN2))
            stickmen[s].spawnRef = 0;
    }

    // Spawn points first (one per player; SPAWN's CreateAllowed)
    for (uint k = 0; k < n; k++)
    {
        uint id = counters[CTR_STKM_LIST + k];
        uint t = ptype[id];
        if (t != PT_SPAWN && t != PT_SPAWN2)
            continue;
        uint s = t == PT_SPAWN ? 0u : 1u;
        if (stickmen[s].spawnRef == 0)
            stickmen[s].spawnRef = id + 1;
        else if (stickmen[s].spawnRef != id + 1)
            killListed(id);
    }

    // New stick men (their ChangeType): STKM and STKM2 get new legs (and, by their Create, a spawn point where
    // they appear if there is none); a fighter gets the first free slot, keeping its own if it comes back out of
    // a portal (with the legs it had, as in TPT).
    uint fighters = 0;
    for (uint f = 0; f < MAX_FIGHTERS; f++)
        if (stickmen[2 + f].spwn != 0)
            fighters++;
    for (uint k = 0; k < n; k++)
    {
        uint id = counters[CTR_STKM_LIST + k];
        uint t = ptype[id];
        if (!isStickman(t))
            continue;
        Particle p = parts[id];
        int x = int(p.x + 0.5), y = int(p.y + 0.5);
        if (t == PT_STKM || t == PT_STKM2)
        {
            uint s = t == PT_STKM ? 0u : 1u;
            uint ref = stickmen[s].partRef;
            if (stickmen[s].spwn != 0 && ref == id + 1)
                continue;
            if (stickmen[s].spwn != 0 && ref != STKM_IN_PORTAL)
            {
                killListed(id); // one each (CreateAllowed)
                continue;
            }
            Stickman st = stickmen[s];
            initLegs(st, x, y);
            st.spwn = 1;
            st.partRef = id + 1;
            stickmen[s] = st;
            if (stickmen[s].spawnRef == 0)
            {
                int sp = createPartStacked(x, y, s == 0 ? PT_SPAWN : PT_SPAWN2);
                if (sp >= 0)
                    stickmen[s].spawnRef = uint(sp) + 1;
            }
        }
        else
        {
            int f = p.tmp;
            if (f >= 0 && f < int(MAX_FIGHTERS) && stickmen[2 + f].spwn != 0)
            {
                uint ref = stickmen[2 + f].partRef;
                if (ref == id + 1)
                    continue;
                if (ref == STKM_IN_PORTAL)
                {
                    stickmen[2 + f].partRef = id + 1;
                    continue;
                }
            }
            int slot = -1;
            if (fighters < MAX_FIGHTERS)
                for (uint g = 0; g < MAX_FIGHTERS && slot < 0; g++)
                    if (stickmen[2 + g].spwn == 0)
                        slot = int(g);
            if (slot < 0)
            {
                killListed(id);
                continue;
            }
            fighters++;
            parts[id].tmp = slot;
            Stickman st = stickmen[2 + slot];
            initLegs(st, x, y);
            st.elem = (p.ctype > 0 && p.ctype < int(MAX_ELEMENTS)) ? p.ctype : int(PT_DUST);
            st.spwn = 1;
            st.partRef = id + 1;
            stickmen[2 + slot] = st;
        }
    }

    // The players' keys; a player who isn't out holds DUST again (GameController::Tick)
    uint keys = params[0].stkmComm;
    for (uint s = 0; s < 2; s++)
    {
        stickmen[s].comm = (keys >> (16 * s)) & 0xFF;
        stickmen[s].pcomm = (keys >> (16 * s + 8)) & 0xFF;
        if (stickmen[s].spwn == 0)
        {
            Stickman st = stickmen[s];
            setElement(st, int(PT_DUST));
            stickmen[s] = st;
        }
    }

    // BeforeSim: a player who isn't out comes back at its spawn point
    for (uint s = 0; s < 2; s++)
    {
        uint sp = stickmen[s].spawnRef;
        if (stickmen[s].spwn != 0 || sp == 0)
            continue;
        int x = int(parts[sp - 1].x), y = int(parts[sp - 1].y);
        uint t = slotType(s);
        int np = createPart(x, y, t);
        if (np < 0)
            continue;
        enlistNew(uint(np), x, y);
        Stickman st = stickmen[s];
        uint comm = st.comm, pcomm = st.pcomm;
        initLegs(st, x, y);
        st.comm = comm;
        st.pcomm = pcomm;
        st.spwn = 1;
        st.partRef = uint(np) + 1;
        stickmen[s] = st;
    }

    // What was made since last time is counted now
    counters[CTR_FIGH_COUNT] = fighters;
    counters[CTR_FIGH_NEW] = 0;
    counters[CTR_STKM_FLAGS] = 0;

    // Their updates, in id order
    uint ids[STICKMEN];
    uint slots[STICKMEN];
    uint m = 0;
    for (uint s = 0; s < STICKMEN; s++)
    {
        uint ref = stickmen[s].partRef;
        if (stickmen[s].spwn == 0 || ref == 0 || ref == STKM_IN_PORTAL || ref == STKM_LEAKED)
            continue;
        uint j = m++;
        while (j > 0 && ids[j - 1] > ref - 1)
        {
            ids[j] = ids[j - 1];
            slots[j] = slots[j - 1];
            j--;
        }
        ids[j] = ref - 1;
        slots[j] = s;
    }
    for (uint k = 0; k < m; k++)
    {
        uint i = ids[k], s = slots[k];
        if (ptype[i] != slotType(s) || stickmen[s].partRef != i + 1)
            continue; // killed by one updated before it
        int x = int(parts[i].x + 0.5), y = int(parts[i].y + 0.5);
        if (!inPlayArea(x, y))
            continue; // (killed in its colour pass)
        uint wall = bmapAt(x, y);
        if (wall == WL_STASIS && emapAt(x, y) < 8)
            continue;
        rngSeed(i ^ 0x9E3779B9u);
        Stickman st = stickmen[s];
        if (s >= 2)
            fighterAim(st, i, x, y);
        runStickman(st, i);
        stickmen[s] = st;
    }
}
