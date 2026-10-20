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

int evalMove(uint pt, int nx, int ny)
{
    if (!inSim(nx, ny))
        return 0;
    uint rt = pmapType(pmap[cellIndex(nx, ny)]);
    // can_move == 3 special cases and walls are not ported yet.
    return int(canMove[pt * MAX_ELEMENTS + rt]);
}

bool inPlayArea(int x, int y)
{
    return x >= CELL && y >= CELL && x < int(pc.W) - CELL && y < int(pc.H) - CELL;
}

// try_move followed by move. Returns 1 when the particle moved (or stayed in its cell), 0 when it
// was blocked, and -1 when it left the play area and was killed.
int doMove(uint i, inout Particle p, uint t, int x, int y, float nxf, float nyf)
{
    int nx = int(nxf + 0.5);
    int ny = int(nyf + 0.5);
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
        if ((rEnt & PMAP_LOCK) != 0 || pmapType(rEnt) != PT_NONE || canMove[t * MAX_ELEMENTS] == 0)
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
        return 0;
    int rid = pmapId(rEnt);
    uint rt = rid < 0 ? PT_NONE : ptype[rid];
    if (canMove[t * MAX_ELEMENTS + rt] == 0)
        return 0;
    // canMove == 2 (both particles share the cell) only matters for energy particles, not ported yet.

    // Never displace a particle whose own update may be running right now (same pass): its thread
    // holds a copy of its state and would act on a position it no longer has. TPT never has two
    // updates in flight, so blocking here changes nothing TPT could show.
    if (rt != PT_NONE && parts[rid].pass == pc.colourIdx)
        return 0;

    if (!inPlayArea(nx, ny))
    {
        // move() kills particles that leave the play area.
        killPart(i, x, y);
        return -1;
    }

    InterlockedCompareExchange(pmap[own], me, me | PMAP_LOCK, orig);
    if (orig != me)
        return 0;
    InterlockedCompareExchange(pmap[dst], rEnt, me, orig);
    if (orig != rEnt)
    {
        InterlockedExchange(pmap[own], me, orig);
        return 0;
    }
    DeviceMemoryBarrier();
    float oldX = p.x, oldY = p.y;
    p.x = nxf; p.y = nyf;
    parts[i].x = nxf; parts[i].y = nyf;
    if (rid >= 0 && rt != PT_NONE)
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
        while (true)
        {
            mv -= float(ISTP);
            r.finXf += dx;
            r.finYf += dy;
            r.finX = int(r.finXf + 0.5);
            r.finY = int(r.finYf + 0.5);
            if (mv <= 0.0)
            {
                // Nothing found.
                r.finXf = p.x + vx;
                r.finYf = p.y + vy;
                r.finX = int(r.finXf + 0.5);
                r.finY = int(r.finYf + 0.5);
                r.clearXf = r.finXf - dx;
                r.clearYf = r.finYf - dy;
                r.clearX = int(r.clearXf + 0.5);
                r.clearY = int(r.clearYf + 0.5);
                break;
            }
            if (evalMove(t, r.finX, r.finY) == 0)
            {
                // Found an obstacle.
                r.clearXf = r.finXf - dx;
                r.clearYf = r.finYf - dy;
                r.clearX = int(r.clearXf + 0.5);
                r.clearY = int(r.clearYf + 0.5);
                break;
            }
        }
    }
    r.vx = vx;
    r.vy = vy;
    return r;
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

    // Stickmen and energy particles are not ported yet.
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

    // Only vertical gravity is ported so far (GRAV_VERTICAL, Newtonian gravity off).
    if (falldown > 1 && p.vy > abs(p.vx))
    {
        int s = 0;
        int rt = (!stagnant || nb.nt != 0) ? 30 : 10;
        int nx = -1, ny = -1;
        for (int j = clearX + r; j >= 0 && j >= clearX - rt && j < clearX + rt && j < int(pc.W); j += r)
        {
            if (typeAt(j, finY) != t)
            {
                s = doMove(i, p, t, x, y, float(j), finYf);
                if (s != 0)
                {
                    nx = int(p.x + 0.5);
                    ny = int(p.y + 0.5);
                    break;
                }
            }
            if (finY != clearY && typeAt(j, clearY) != t)
            {
                s = doMove(i, p, t, x, y, float(j), clearYf);
                if (s != 0)
                {
                    nx = int(p.x + 0.5);
                    ny = int(p.y + 0.5);
                    break;
                }
            }
            if (typeAt(j, clearY) != t)
                break;
        }

        r = (p.vy > 0) ? 1 : -1;
        if (s == 1)
        {
            for (int j = ny + r; j >= 0 && j < int(pc.H) && j >= ny - rt && j < ny + rt; j += r)
            {
                if (typeAt(nx, j) != t && doMove(i, p, t, nx, ny, float(nx), float(j)) != 0)
                    break;
                if (typeAt(nx, j) != t)
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
