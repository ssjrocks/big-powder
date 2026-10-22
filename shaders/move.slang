// Particle movement, ported from TPT's Simulation::PlanMove, try_move, do_move and MovementPhase.
//
// TPT moves one particle at a time. Here many particles move at once, so a move claims pmap cells
// atomically: the mover locks its own cell, then swaps itself into the target with a compare-and-swap.
// A move that loses a race fails exactly as if the target had been occupied, which is what TPT would
// have seen had the other particle been updated first.
#pragma once
#include "common.slang"

struct Neighbourhood
{
    uint surround[8];
    int surroundSpace; // empty neighbours
    int nt;            // neighbours that are empty or of a different type
    float pGravX, pGravY;
};

// Simulation::IsWallBlocking.
bool isWallBlocking(int x, int y, uint type)
{
    uint wall = bmapAt(x, y);
    if (wall == 0)
        return false;
    uint props = elems[type].properties;
    if (wall == WL_ALLOWGAS && (props & TYPE_GAS) == 0)
        return true;
    if (wall == WL_ALLOWENERGY && (props & TYPE_ENERGY) == 0)
        return true;
    if (wall == WL_ALLOWLIQUID && (props & TYPE_LIQUID) == 0)
        return true;
    if (wall == WL_ALLOWPOWDER && (props & TYPE_PART) == 0)
        return true;
    if (wall == WL_ALLOWAIR || wall == WL_WALL || wall == WL_WALLELEC)
        return true;
    if (wall == WL_EWALL && emapAt(x, y) == 0)
        return true;
    if (wall == WL_DETECT && (props & TYPE_SOLID) != 0)
        return true;
    return false;
}

bool closedEhole(int x, int y) { return bmapAt(x, y) == WL_EHOLE && emapAt(x, y) == 0; }

// Simulation::eval_move.
int evalMoveType(uint pt, int nx, int ny, uint rt)
{
    if (!inSim(nx, ny))
        return 0;
    int result = int(canMove[pt * MAX_ELEMENTS + rt]);
    if (result == 3)
    {
        // It depends on the particle there.
        int rid = pmapId(pmap[cellIndex(nx, ny)]);
        Particle r = rid >= 0 ? parts[rid] : parts[0];
        if (rt == PT_VOID || (rt == PT_PVOD && rid >= 0 && r.life == 10))
            // VOID (and PVOD while on) lets through what it will eat: everything, or only (tmp bit 0 clear) or
            // all but (set) its ctype
            result = (r.ctype == 0 || ((r.ctype == int(pt)) != ((r.tmp & 1) != 0))) ? 1 : 0;
        else if (rt == PT_PVOD)
            result = 0;
        else if (rt == PT_SWCH && pt == PT_TRON)
            return r.life >= 10 ? 2 : 0;
        else if (rt == PT_LCRY)
        {
            if (pt == PT_PHOT)
                result = r.life > 5 ? 2 : 0; // photons pass while it is on
        }
        else if (rt == PT_GPMP)
        {
            if (pt == PT_PHOT)
                result = r.life < 10 ? 2 : 0;
        }
        else if (rt == PT_INVIS)
        {
            // INVIS lets things through under pressure beyond its resistance (tmp, default 4)
            float resistance = r.tmp > 0 ? float(r.tmp) : 4.0;
            float pv = airPV(airIndex(nx, ny));
            result = (pv < -resistance || pv > resistance) ? 2 : 0;
        }
        else
            result = 1;
    }
    if (bmapAt(nx, ny) != 0)
    {
        if (isWallBlocking(nx, ny, pt))
            return 0;
        if (closedEhole(nx, ny) && (elems[pt].properties & TYPE_SOLID) == 0 &&
            (elems[rt].properties & TYPE_SOLID) == 0)
            return 2;
    }
    return result;
}

int evalMove(uint pt, int nx, int ny)
{
    if (!inSim(nx, ny))
        return 0;
    return evalMoveType(pt, nx, ny, pmapType(pmap[cellIndex(nx, ny)]));
}

bool inPlayArea(int x, int y)
{
    return x >= CELL && y >= CELL && x < int(pc.W) - CELL && y < int(pc.H) - CELL;
}

// try_move followed by move. Returns 1 when the particle moved (or stayed in its cell), 0 when it
// was blocked, and -1 when it left the play area and was killed.
// TPT's remainder_p: a remainder that is never negative.
float remainderP(float x, float y) { return fmod(x, y) + (x >= 0.0 ? 0.0 : y); }

// Edge mode "loop": wrap a position that left the play area to the other side.
void wrapLoop(inout float fx, inout float fy)
{
    float W = float(pc.W), H = float(pc.H);
    if (!(fx >= float(CELL) - 0.5 && fx < W - float(CELL) - 0.5))
        fx = remainderP(fx - float(CELL) + 0.5, W - float(CELL) * 2.0) + float(CELL) - 0.5;
    if (!(fy >= float(CELL) - 0.5 && fy < H - float(CELL) - 0.5))
        fy = remainderP(fy - float(CELL) + 0.5, H - float(CELL) * 2.0) + float(CELL) - 0.5;
}

// try_move, when the mover can't go into the target: hitting WOOD faster than 5 px per tick breaks it
// into SAWD.
void breakWood(Particle p, uint rEnt, uint rt)
{
    if (rt == PT_WOOD && sqrt(p.vx * p.vx + p.vy * p.vy) > 5.0)
        ptype[pmapId(rEnt)] = PT_SAWD;
}

int doMove(uint i, inout Particle p, uint t, int x, int y, float nxf, float nyf)
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
    uint own = cellIndex(x, y);
    uint me = pmapEnc(i);
    uint orig;
    // A stacked particle (another one is listed in pmap at its cell) moves only into empty cells and
    // leaves its old cell alone, like TPT's move().
    bool stacked = (pmap[own] & PMAP_ID_MASK) != me;
    if (stacked)
    {
        if (nx == x && ny == y)
        {
            p.x = nxf; p.y = nyf;
            parts[i].x = nxf; parts[i].y = nyf;
            return 1;
        }
        if (!inPlayArea(nx, ny))
        {
            ptype[i] = PT_NONE;
            return -1;
        }
        uint dst = cellIndex(nx, ny);
        uint rEnt = pmap[dst];
        if ((rEnt & PMAP_LOCK) != 0)
            return 0;
        uint srt = pmapType(rEnt);
        if (srt != PT_NONE)
        {
            if (evalMoveType(t, nx, ny, srt) == 0)
                breakWood(p, rEnt, srt);
            return 0;
        }
        if (evalMoveType(t, nx, ny, PT_NONE) == 0)
            return 0;
        InterlockedCompareExchange(pmap[dst], rEnt, me, orig);
        if (orig != rEnt)
            return 0;
        p.x = nxf; p.y = nyf;
        parts[i].x = nxf; parts[i].y = nyf;
        return 1;
    }
    if (nx == x && ny == y)
    {
        // Moving within the cell still takes the lock: another mover may have just displaced this
        // particle, and writing the position unlocked would undo that.
        InterlockedCompareExchange(pmap[own], me, me | PMAP_LOCK, orig);
        if (orig != me)
            return 0;
        p.x = nxf; p.y = nyf;
        parts[i].x = nxf; parts[i].y = nyf;
        DeviceMemoryBarrier();
        InterlockedExchange(pmap[own], me, orig);
        return 1;
    }
    if (!inSim(nx, ny))
    {
        killPart(i, x, y);
        return -1;
    }

    uint dst = cellIndex(nx, ny);
    uint rEnt = pmap[dst];
    if ((rEnt & PMAP_LOCK) != 0)
    {
        debugCount(CTR_DBG_LOCKED);
        return 0;
    }
    int rid = pmapId(rEnt);
    uint rt = rid < 0 ? PT_NONE : ptype[rid];
    int e = evalMoveType(t, nx, ny, rt);
    if (e == 0)
    {
        breakWood(p, rEnt, rt);
        return 0;
    }
    // e == 2: both particles share the cell (closed E-holes; energy particles are not ported yet). The
    // mover goes on top in pmap and the other particle stays where it is, as in TPT's move().
    bool share = e == 2;
    if (share && (t == PT_BIZR || t == PT_BIZRG) && rt == PT_FILT)
        p.ctype = filtInteract(parts[rid], p.ctype); // BIZR through FILT takes its colour
    if (!share && rid >= 0)
    {
        // try_move: VOID eats what moves into it, black holes too (and take its heat); white holes eat ANAR
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
        if ((rt == PT_WHOL || rt == PT_NWHL) && t == PT_ANAR)
        {
            if (params[0].legacyHeat == 0)
                parts[rid].temp = restrictFlt(parts[rid].temp - (MAX_TEMP - p.temp) / 2.0, MIN_TEMP, MAX_TEMP);
            killPart(i, x, y);
            return -1;
        }
    }
    // try_move: nothing leaves a closed E-hole by swapping or moving into empty space.
    if (!share && closedEhole(x, y) && !closedEhole(nx, ny))
        return 0;

    // try_move: CNCT won't move down (along gravity) onto CNCT or ROCK below it; an active GBMB doesn't move.
    if (!share && t == PT_GBMB && p.life > 0)
        return 0;
    if (!share && t == PT_CNCT)
    {
        float g = elems[PT_CNCT].gravity;
        float cgx, cgy;
        gravityField(x, y, g, g, cgx, cgy);
        int ox = cgx > 0.0 ? 1 : cgx < 0.0 ? -1 : 0;
        int oy = cgy > 0.0 ? 1 : cgy < 0.0 ? -1 : 0;
        if ((ox != 0) != (oy != 0) && ((nx - x) * ox > 0 || (ny - y) * oy > 0))
        {
            uint below = typeAt(x + ox, y + oy);
            if (below == PT_CNCT || below == PT_ROCK)
                return 0;
        }
    }

    // Never displace a particle whose own update may be running right now (same pass): its thread
    // holds a copy of its state and would act on a position it no longer has. TPT never has two
    // updates in flight, so blocking here changes nothing TPT could show.
    if (!share && rt != PT_NONE && (pc.mode & 2) == 0 && parts[rid].pass == pc.colourIdx)
    {
        debugCount(CTR_DBG_SAME_PASS);
        return 0;
    }

    if (!inPlayArea(nx, ny))
    {
        // move() kills particles that leave the play area.
        killPart(i, x, y);
        return -1;
    }

    InterlockedCompareExchange(pmap[own], me, me | PMAP_LOCK, orig);
    if (orig != me)
    {
        debugCount(CTR_DBG_OWN_LOST);
        return 0;
    }
    InterlockedCompareExchange(pmap[dst], rEnt, me, orig);
    if (orig != rEnt)
    {
        InterlockedExchange(pmap[own], me, orig);
        debugCount(CTR_DBG_CAS_LOST);
        return 0;
    }
    debugCount(CTR_DBG_MOVED);
    DeviceMemoryBarrier();
    float oldX = p.x, oldY = p.y;
    p.x = nxf; p.y = nyf;
    parts[i].x = nxf; parts[i].y = nyf;
    if (!share && rid >= 0 && rt != PT_NONE)
    {
        // Swap: the displaced particle takes our old position.
        parts[rid].x = oldX;
        parts[rid].y = oldY;
        DeviceMemoryBarrier();
        InterlockedExchange(pmap[own], pmapEnc(uint(rid)), orig);
    }
    else
    {
        DeviceMemoryBarrier();
        InterlockedExchange(pmap[own], 0u, orig);
    }
    return 1;
}

struct PlanMoveResult
{
    int finX, finY, clearX, clearY;
    float finXf, finYf, clearXf, clearYf;
    float vx, vy;
};

PlanMoveResult planMove(Particle p, uint t, int x, int y)
{
    PlanMoveResult r;
    float vx = p.vx;
    float vy = p.vy;
    float mv = max(abs(vx), abs(vy));
    if (mv < float(ISTP) || isnan(mv))
    {
        r.clearX = x; r.clearY = y;
        r.clearXf = p.x; r.clearYf = p.y;
        r.finXf = r.clearXf + vx;
        r.finYf = r.clearYf + vy;
        r.finX = int(r.finXf + 0.5);
        r.finY = int(r.finYf + 0.5);
    }
    else
    {
        if (mv > MAX_VELOCITY)
        {
            vx *= MAX_VELOCITY / mv;
            vy *= MAX_VELOCITY / mv;
            mv = MAX_VELOCITY;
        }
        // Step along the path looking for something in the way.
        float dx = vx * float(ISTP) / mv;
        float dy = vy * float(ISTP) / mv;
        r.finXf = p.x;
        r.finYf = p.y;
        r.finX = int(r.finXf + 0.5);
        r.finY = int(r.finYf + 0.5);
        bool loop = params[0].edgeMode == 2;
        bool closedEholeStart = inSim(r.finX, r.finY) && closedEhole(r.finX, r.finY);
        while (true)
        {
            mv -= float(ISTP);
            r.finXf += dx;
            r.finYf += dy;
            if (loop)
                wrapLoop(r.finXf, r.finYf);
            r.finX = int(r.finXf + 0.5);
            r.finY = int(r.finYf + 0.5);
            if (mv <= 0.0)
            {
                // Nothing found.
                r.finXf = p.x + vx;
                r.finYf = p.y + vy;
                if (loop)
                    wrapLoop(r.finXf, r.finYf);
                r.finX = int(r.finXf + 0.5);
                r.finY = int(r.finYf + 0.5);
                r.clearXf = r.finXf - dx;
                r.clearYf = r.finYf - dy;
                r.clearX = int(r.clearXf + 0.5);
                r.clearY = int(r.clearYf + 0.5);
                break;
            }
            // Blocked if the particle can't move there, if it would be eaten there (can_move 3 giving 1, e.g. a
            // void), for photons by any particle at all (they slow down), by DESTROYALL walls, or by crossing into
            // or out of a closed E-hole.
            bool inside = inSim(r.finX, r.finY);
            uint ft = inside ? pmapType(pmap[cellIndex(r.finX, r.finY)]) : PT_NONE;
            int eval = evalMove(t, r.finX, r.finY);
            if (eval == 0 || (canMove[t * MAX_ELEMENTS + ft] == 3 && eval == 1) || (t == PT_PHOT && ft != PT_NONE) ||
                (inside && bmapAt(r.finX, r.finY) == WL_DESTROYALL) ||
                closedEholeStart != (inside && closedEhole(r.finX, r.finY)))
            {
                // Found an obstacle.
                r.clearXf = r.finXf - dx;
                r.clearYf = r.finYf - dy;
                r.clearX = int(r.clearXf + 0.5);
                r.clearY = int(r.clearYf + 0.5);
                break;
            }
            // Detector walls the path crosses come on (PlanMove<true> in MovementPhase).
            if (inside && bmapAt(r.finX, r.finY) == WL_DETECT && emapAt(r.finX, r.finY) < 8)
                requestEmap(r.finX / CELL, r.finY / CELL);
        }
    }
    r.vx = vx;
    r.vy = vy;
    return r;
}

// MovementPhase for STKM, STKM2 and FIGH: the head passes through anything, and goes on top in pmap. In loop edge
// mode the legs go round with it.
void stickmanMove(uint i, inout Particle p, uint t, int x, int y)
{
    p.x += p.vx;
    p.y += p.vy;
    int nx = int(p.x + 0.5);
    int ny = int(p.y + 0.5);
    if (params[0].edgeMode == 2)
    {
        bool xOk = nx >= CELL && nx < int(pc.W) - CELL;
        bool yOk = ny >= CELL && ny < int(pc.H) - CELL;
        int oldNX = nx, oldNY = ny;
        if (!xOk || !yOk)
        {
            wrapLoop(p.x, p.y);
            nx = int(p.x + 0.5);
            ny = int(p.y + 0.5);
            int s = t == PT_STKM ? 0 : t == PT_STKM2 ? 1 : (p.tmp >= 0 && p.tmp < int(MAX_FIGHTERS) ? 2 + p.tmp : -1);
            if (s >= 0)
                for (int k = 0; k < 16; k += 2)
                {
                    stickmen[s].legs[k] += float(nx - oldNX);
                    stickmen[s].legs[k + 1] += float(ny - oldNY);
                    stickmen[s].accs[k / 2] *= 0.95;
                }
            p.vx *= 0.95;
            p.vy *= 0.95;
        }
    }
    parts[i].x = p.x;
    parts[i].y = p.y;
    if (nx == x && ny == y)
        return;
    uint me = pmapEnc(i), orig;
    if (inSim(x, y))
    {
        // (as in TPT, what the head was on top of is out of pmap until the next tick)
        InterlockedCompareExchange(pmap[cellIndex(x, y)], me, 0u, orig);
        if (orig == me)
        {
            uint k;
            InterlockedAdd(counters[CTR_STKM_LEFT], 1u, k);
            if (k < STKM_LEFT_CAP)
                counters[CTR_STKM_LEFT + 1 + k] = cellIndex(x, y);
        }
        InterlockedCompareExchange(photons[cellIndex(x, y)], me, 0u, orig);
    }
    if (!inPlayArea(nx, ny))
    {
        killPart(i, nx, ny);
        return;
    }
    uint c = cellIndex(nx, ny);
    uint cur = pmap[c];
    for (int attempt = 0; attempt < 16; attempt++)
    {
        if ((cur & PMAP_LOCK) != 0)
        {
            cur = pmap[c];
            continue; // a mover is claiming the cell right now
        }
        InterlockedCompareExchange(pmap[c], cur, me, orig);
        if (orig == cur)
            break;
        cur = orig;
    }
}

void movementPhase(uint i, inout Particle p, uint t, Neighbourhood nb)
{
    int x = int(p.x + 0.5);
    int y = int(p.y + 0.5);
    PlanMoveResult mr = planMove(p, t, x, y);
    p.vx = mr.vx;
    p.vy = mr.vy;
    int finX = mr.finX, finY = mr.finY, clearX = mr.clearX, clearY = mr.clearY;
    float finXf = mr.finXf, finYf = mr.finYf, clearXf = mr.clearXf, clearYf = mr.clearYf;

    bool stagnant = (p.flags & FLAG_STAGNANT) != 0;
    p.flags &= ~FLAG_STAGNANT;
    float collision = elems[t].collision;
    int falldown = elems[t].falldown;

    if (isStickman(t))
    {
        stickmanMove(i, p, t, x, y);
        return;
    }
    if (falldown == 0)
    {
        // Gases and solids (but not powders).
        if (doMove(i, p, t, x, y, finXf, finYf) == 0)
        {
            if (ptype[i] == PT_NONE)
                return;
            // Can't move there, so bounce off.
            finX = clamp(finX, x - ISTP, x + ISTP);
            finY = clamp(finY, y - ISTP, y + ISTP);
            if (doMove(i, p, t, x, y, float(2 * x - finX), float(finY)) != 0)
                p.vx *= collision;
            else if (doMove(i, p, t, x, y, float(finX), float(2 * y - finY)) != 0)
                p.vy *= collision;
            else
            {
                p.vx *= collision;
                p.vy *= collision;
            }
        }
        return;
    }

    // Liquids and powders.
    if (doMove(i, p, t, x, y, finXf, finYf) != 0)
        return;
    if (ptype[i] == PT_NONE)
        return;
    if (finX != x && doMove(i, p, t, x, y, finXf, clearYf) != 0)
    {
        p.vx *= collision;
        p.vy *= collision;
        return;
    }
    if (finY != y && doMove(i, p, t, x, y, clearXf, finYf) != 0)
    {
        p.vx *= collision;
        p.vy *= collision;
        return;
    }

    int r = rngBetween(0, 1) * 2 - 1; // search direction, left or right first
    if ((clearX != x || clearY != y || nb.nt != 0 || nb.surroundSpace != 0) &&
        (abs(p.vx) > 0.01 || abs(p.vy) > 0.01))
    {
        // Allow diagonal movement if the target position is blocked.
        float dx = p.vx - p.vy * float(r);
        float dy = p.vy + p.vx * float(r);
        float mv = max(abs(dx), abs(dy));
        dx /= mv;
        dy /= mv;
        if (doMove(i, p, t, x, y, clearXf + dx, clearYf + dy) != 0)
        {
            p.vx *= collision;
            p.vy *= collision;
            return;
        }
        float swappage = dx;
        dx = dy * float(r);
        dy = -swappage * float(r);
        if (doMove(i, p, t, x, y, clearXf + dx, clearYf + dy) != 0)
        {
            p.vx *= collision;
            p.vy *= collision;
            return;
        }
    }

    // (TPT's !grav: liquids spread like this only while Newtonian gravity is off)
    if (falldown > 1 && params[0].newtonian == 0 && params[0].gravityMode == 0 && p.vy > abs(p.vx))
    {
        int s = 0;
        int rt = (!stagnant || nb.nt != 0) ? 30 : 10;
        if (t == PT_GEL)
            rt = int(float(p.tmp) * 0.20 + 5.0);
        int nx = -1, ny = -1;
        for (int j = clearX + r; j >= 0 && j >= clearX - rt && j < clearX + rt && j < int(pc.W); j += r)
        {
            if (typeAt(j, finY) != t || bmapAt(j, finY) != 0)
            {
                s = doMove(i, p, t, x, y, float(j), finYf);
                if (s != 0)
                {
                    nx = int(p.x + 0.5);
                    ny = int(p.y + 0.5);
                    break;
                }
            }
            if (finY != clearY && (typeAt(j, clearY) != t || bmapAt(j, clearY) != 0))
            {
                s = doMove(i, p, t, x, y, float(j), clearYf);
                if (s != 0)
                {
                    nx = int(p.x + 0.5);
                    ny = int(p.y + 0.5);
                    break;
                }
            }
            uint bw = bmapAt(j, clearY);
            if (typeAt(j, clearY) != t || (bw != 0 && bw != WL_STREAM))
                break;
        }

        r = (p.vy > 0) ? 1 : -1;
        if (s == 1)
        {
            for (int j = ny + r; j >= 0 && j < int(pc.H) && j >= ny - rt && j < ny + rt; j += r)
            {
                if ((typeAt(nx, j) != t || bmapAt(nx, j) != 0) && doMove(i, p, t, nx, ny, float(nx), float(j)) != 0)
                    break;
                uint bw = bmapAt(nx, j);
                if (typeAt(nx, j) != t || (bw != 0 && bw != WL_STREAM))
                    break;
            }
        }
        else if (s == -1)
        {
            // Out of bounds.
        }
        else if ((clearX != x || clearY != y) && doMove(i, p, t, x, y, clearXf, clearYf) != 0)
        {
        }
        else
        {
            p.flags |= FLAG_STAGNANT;
        }
        p.vx *= collision;
        p.vy *= collision;
        return;
    }

    if (falldown > 1 && abs(nb.pGravX * p.vx + nb.pGravY * p.vy) > abs(nb.pGravY * p.vx - nb.pGravX * p.vy))
    {
        // Liquids under gravity that isn't straight down: move perpendicular to gravity, then along it.
        float pGravX = nb.pGravX, pGravY = nb.pGravY;
        float prevGX = 0.0, prevGY = 0.0;
        float ptGrav = elems[t].gravity;
        int s = 0;
        int rt = (!stagnant || nb.nt != 0) ? 30 : 10;
        float nxf = clearXf, nyf = clearYf;
        int nx = clearX, ny = clearY;
        for (int j = 0; j < rt; j++)
        {
            gravityField(nx, ny, ptGrav, 1.0, pGravX, pGravY);
            float mv = max(abs(pGravX), abs(pGravY));
            if (mv < 0.0001)
                break;
            pGravX /= mv;
            pGravY /= mv;
            if (j != 0)
            {
                // Gravity direction plus the last change in it, so liquid follows curved gravity better.
                nxf += float(r) * (pGravY * 2.0 - prevGY);
                nyf += -float(r) * (pGravX * 2.0 - prevGX);
            }
            else
            {
                nxf += float(r) * pGravY;
                nyf += -float(r) * pGravX;
            }
            prevGX = pGravX;
            prevGY = pGravY;
            nx = int(nxf + 0.5);
            ny = int(nyf + 0.5);
            if (nx < 0 || ny < 0 || nx >= int(pc.W) || ny >= int(pc.H))
                break;
            if (typeAt(nx, ny) != t || bmapAt(nx, ny) != 0)
            {
                s = doMove(i, p, t, x, y, nxf, nyf);
                if (s != 0)
                {
                    nx = int(p.x + 0.5);
                    ny = int(p.y + 0.5);
                    break;
                }
                if (typeAt(nx, ny) != t || bmapAt(nx, ny) != WL_STREAM)
                    break;
            }
        }
        if (s == 1)
        {
            // Moved sideways; now move along gravity until blocked by something other than this liquid.
            clearX = nx;
            clearY = ny;
            for (int j = 0; j < rt; j++)
            {
                gravityField(nx, ny, ptGrav, 1.0, pGravX, pGravY);
                float mv = max(abs(pGravX), abs(pGravY));
                if (mv < 0.0001)
                    break;
                pGravX /= mv;
                pGravY /= mv;
                nxf += pGravX;
                nyf += pGravY;
                nx = int(nxf + 0.5);
                ny = int(nyf + 0.5);
                if (nx < 0 || ny < 0 || nx >= int(pc.W) || ny >= int(pc.H))
                    break;
                if (typeAt(nx, ny) != t || bmapAt(nx, ny) != 0)
                {
                    s = doMove(i, p, t, clearX, clearY, nxf, nyf);
                    if (s != 0 || typeAt(nx, ny) != t || bmapAt(nx, ny) != WL_STREAM)
                        break;
                }
            }
        }
        else if (s == -1)
        {
        }
        else if ((clearX != x || clearY != y) && doMove(i, p, t, x, y, clearXf, clearYf) != 0)
        {
        }
        else
        {
            p.flags |= FLAG_STAGNANT;
        }
        p.vx *= collision;
        p.vy *= collision;
        return;
    }

    // If interpolation was done, try moving to the last clear position.
    if ((clearX != x || clearY != y) && doMove(i, p, t, x, y, clearXf, clearYf) != 0)
    {
    }
    else
    {
        p.flags |= FLAG_STAGNANT;
    }
    p.vx *= collision;
    p.vy *= collision;
}
