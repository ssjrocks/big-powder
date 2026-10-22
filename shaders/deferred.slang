// Work queued during the update passes and done by one thread afterwards, in this order:
// - FloodINST: spark a connected INST network, with TPT's rules for 1 px INST wires crossing, visiting cells in
//   TPT's order (a stack of scanline starts);
// - BANG's flood_prop: set tmp = 2 on a connected BANG blob;
// - PPIP's flood trigger: start, stop or reverse a connected PPIP network (or make a pipe conduct heat);
// - pistons extending and retracting;
// - BOMB explosions and DMG blasts (they reach further than the colour passes keep particles apart).
#include "common.slang"
#include "elements.slang"

// The stack lives in the colour lists buffer, free once the update passes have run (and between ticks).
static uint g_sp;

// ---- pistons (PSTN.cpp) ----
// A piston is a line of PSTN: the base (life 0), whose temperature sets how far it extends, then its arm (life 1).
// Sparked, it pushes the stack of particles in front of its arm along (up to tmp of them, 31 if not set), or pulls
// it back, and FRME next to the end moves together with what rests on it. TPT moves them during the piston's
// update; here, one piston at a time after the passes.
static const int MAX_FRAME = 15;
static const uint PSTN_NONE_SLOT = 0xFFFFFFFFu;

int pstnTop(int x, int y)
{
    int id = pmapId(pmap[cellIndex(x, y)]);
    return (id >= 0 && ptype[id] != PT_NONE) ? id : -1;
}

// Moves particle id by (dx, dy) in pmap, as MoveStack does.
void pstnMove(uint id, int dx, int dy)
{
    int sx = int(parts[id].x + 0.5), sy = int(parts[id].y + 0.5);
    pmap[cellIndex(sx, sy)] = 0;
    parts[id].x = float(sx + dx);
    parts[id].y = float(sy + dy);
    pmap[cellIndex(sx + dx, sy + dy)] = pmapEnc(id);
}

// CanMoveStack: how many particles in front of (stackX, stackY) would move and how many spaces there are for them,
// listing them (Element_PSTN_tempParts, kept in the colour lists buffer).
int2 pstnCanMoveStack(int stackX, int stackY, int dx, int dy, int maxSize, int amount, bool retract, uint block)
{
    int spaces = 0, currentPos = 0;
    if (amount <= 0)
        return int2(0, 0);
    for (int posX = stackX, posY = stackY; currentPos < maxSize + amount && currentPos < int(pc.W) - 1;
         posX += dx, posY += dy)
    {
        if (!inSim(posX, posY))
            break;
        int r = pstnTop(posX, posY);
        uint rt = r >= 0 ? ptype[r] : PT_NONE;
        if (isWallBlocking(posX, posY, PT_NONE) || (block != 0 && rt == block))
            return int2(currentPos - spaces, spaces);
        if (r < 0)
        {
            spaces++;
            lists[currentPos++] = PSTN_NONE_SLOT;
            if (spaces >= amount)
                break;
        }
        else if (currentPos - spaces < maxSize && (!retract || (rt == PT_FRME && posX == stackX && posY == stackY)))
            lists[currentPos++] = uint(r);
        else
            return int2(currentPos - spaces, spaces);
    }
    return int2(currentPos - spaces, spaces);
}

// MoveStack for one line (TPT's calls with callDepth 1, and the plain case of callDepth 0).
int pstnMoveLine(int stackX, int stackY, int dx, int dy, int maxSize, int amount, bool retract, uint block, bool sticky,
                 bool killArm)
{
    if (retract)
    {
        if (killArm)
            for (int j = 1; j <= amount; j++)
            {
                int a = pstnTop(stackX - dx * j, stackY - dy * j);
                if (a >= 0)
                    killPart(uint(a), stackX - dx * j, stackY - dy * j); // the arm section
            }
        int currentPos = 0;
        for (int posX = stackX, posY = stackY; currentPos < maxSize && currentPos < int(pc.W) - 1; posX += dx, posY += dy)
        {
            if (!inSim(posX, posY))
                break;
            int r = pstnTop(posX, posY);
            if (r < 0 || ptype[r] == block || (!sticky && ptype[r] != PT_FRME))
                break;
            lists[currentPos++] = uint(r);
        }
        if (currentPos == 0)
            return 0;
        for (int j = 0; j < currentPos; j++)
            pstnMove(lists[j], -dx * amount, -dy * amount);
        return amount;
    }
    int2 sd = pstnCanMoveStack(stackX, stackY, dx, dy, maxSize, amount, false, block);
    int currentPos = sd.x + sd.y;
    int possible = 0;
    for (int j = currentPos - 1; j >= 0; j--)
    {
        uint jP = lists[j];
        if (jP == PSTN_NONE_SLOT)
        {
            possible++;
            continue;
        }
        if (possible != 0)
            pstnMove(jP, dx * possible, dy * possible);
    }
    return possible;
}

bool pstnSticky(int x, int y)
{
    int r = pstnTop(x, y);
    return r < 0 || parts[r].tmp == 0;
}

// MoveStack from the piston's end: with FRME there, the whole frame (up to 15 each way) moves, and so does what
// rests on it; without, just the line in front.
int pstnMoveStack(int stackX, int stackY, int dx, int dy, int maxSize, int amount, bool retract, uint block, bool cancelOnObstacle)
{
    int r = pstnTop(stackX, stackY);
    if (r < 0 || ptype[r] != PT_FRME)
        return pstnMoveLine(stackX, stackY, dx, dy, maxSize, amount, retract, block, true, true);
    int newY = dx != 0 ? 1 : 0, newX = dy != 0 ? 1 : 0;
    int rdx = retract ? -dx : dx, rdy = retract ? -dy : dy;
    int maxRight = MAX_FRAME, maxLeft = MAX_FRAME;
    // Can all the FRME move?
    for (int c = retract ? 1 : 0; c < MAX_FRAME; c++)
    {
        int px = stackX + c * newX, py = stackY + c * newY;
        if (inSim(px, py) && typeAt(px, py) == PT_FRME)
        {
            int spaces = pstnCanMoveStack(px, py, rdx, rdy, maxSize, amount, retract, block).y;
            if (spaces < amount)
            {
                if (cancelOnObstacle)
                    return 0;
                amount = spaces;
            }
        }
        else
        {
            maxRight = c;
            break;
        }
    }
    for (int c = 1; c < MAX_FRAME; c++)
    {
        int px = stackX - c * newX, py = stackY - c * newY;
        if (inSim(px, py) && typeAt(px, py) == PT_FRME)
        {
            int spaces = pstnCanMoveStack(px, py, rdx, rdy, maxSize, amount, retract, block).y;
            if (spaces < amount)
            {
                if (cancelOnObstacle)
                    return 0;
                amount = spaces;
            }
        }
        else
        {
            maxLeft = c;
            break;
        }
    }
    // Push everything resting on the frame, from the centre outwards
    for (int c = 1; c < maxRight; c++)
    {
        int px = stackX + c * newX, py = stackY + c * newY;
        pstnMoveLine(px, py, dx, dy, maxSize, amount, retract, block, pstnSticky(px, py), false);
    }
    for (int c = 1; c < maxLeft; c++)
    {
        int px = stackX - c * newX, py = stackY - c * newY;
        pstnMoveLine(px, py, dx, dy, maxSize, amount, retract, block, pstnSticky(px, py), false);
    }
    if (retract)
        for (int j = 1; j <= amount; j++)
        {
            int a = pstnTop(stackX - dx * j, stackY - dy * j);
            if (a >= 0)
                killPart(uint(a), stackX - dx * j, stackY - dy * j); // the arm section
        }
    return pstnMoveLine(stackX, stackY, dx, dy, maxSize, amount, retract, block, pstnSticky(stackX, stackY), false);
}

// The rest of PSTN's update for a piston sparked to extend (2) or retract (1).
void pistonRun(uint i, uint state)
{
    if (ptype[i] != PT_PSTN || parts[i].life != 0)
        return;
    Particle p = parts[i];
    int x = int(p.x + 0.5), y = int(p.y + 0.5);
    int maxSize = p.tmp != 0 ? p.tmp : 31;
    int armLimit = p.tmp2 != 0 ? p.tmp2 : 255;
    uint block = uint(p.ctype);
    for (int rx = -1; rx <= 1; rx++)
    {
        for (int ry = -1; ry <= 1; ry++)
        {
            if ((rx == 0 && ry == 0) || (rx != 0 && ry != 0))
                continue;
            int nb = pstnTop(x + rx, y + ry);
            if (nb < 0 || ptype[nb] != PT_PSTN || parts[nb].life != 0)
                continue;
            // Along this way: the base's length in tens of degrees above 0 C, then the arm, then its end
            bool foundEnd = false;
            int endX = 0, endY = 0, pistonCount = -1, armCount = 0;
            for (int nxx = 0, nyy = 0;; nxx += rx, nyy += ry)
            {
                if (!inSim(x + nxx, y + nyy))
                    break;
                int r = pstnTop(x + nxx, y + nyy);
                if (r >= 0 && ptype[r] == PT_PSTN)
                {
                    if (parts[r].life != 0)
                        armCount++;
                    else if (armCount != 0)
                    {
                        endX = x + nxx;
                        endY = y + nyy;
                        foundEnd = true;
                        break;
                    }
                    else
                        pistonCount += int(floor((parts[r].temp - 268.15) / 10.0));
                }
                else if (nxx == 0 && nyy == 0)
                    pistonCount += int(floor((p.temp - 268.15) / 10.0)); // (a base under another particle)
                else
                {
                    endX = x + nxx;
                    endY = y + nyy;
                    foundEnd = true;
                    break;
                }
            }
            if (!foundEnd)
                continue;
            bool moved = false;
            if (state == 2)
            {
                if (armCount + pistonCount > armLimit)
                {
                    if ((p.tmp3 & 2) != 0)
                        continue; // don't extend at all past the arm limit
                    pistonCount = armLimit - armCount;
                }
                if (pistonCount > 0)
                {
                    int newSpace = pstnMoveStack(endX, endY, rx, ry, maxSize, pistonCount, false, block, (p.tmp3 & 1) != 0);
                    if (newSpace != 0)
                    {
                        // New arm sections, a little darker
                        for (int j = 0; j < newSpace; j++)
                        {
                            int nr = createPartStacked(endX + rx * j, endY + ry * j, PT_PSTN);
                            if (nr > -1)
                            {
                                parts[nr].life = 1;
                                uint c = p.dcolour;
                                if (c != 0)
                                    parts[nr].dcolour = (c & 0xFF000000u) |
                                                        uint(max(int(c & 0xFF0000u) - 0x3C0000, 0)) |
                                                        uint(max(int(c & 0xFF00u) - 0x3C00, 0)) |
                                                        uint(max(int(c & 0xFFu) - 0x3C, 0));
                            }
                        }
                        moved = true;
                    }
                }
            }
            else
            {
                if (pistonCount > armCount)
                {
                    if ((p.tmp3 & 8) != 0)
                        continue; // the arm is shorter than the retraction
                    pistonCount = armCount;
                }
                if (armCount != 0 && pistonCount > 0)
                {
                    pstnMoveStack(endX, endY, rx, ry, maxSize, pistonCount, true, block, (p.tmp3 & 4) != 0);
                    moved = true;
                }
            }
            if (moved)
                return;
        }
    }
}

bool isSparkableInst(int x, int y)
{
    int id = pmapId(pmap[cellIndex(x, y)]);
    return id >= 0 && ptype[id] == PT_INST && parts[id].life == 0;
}

bool isInst(int x, int y)
{
    int id = pmapId(pmap[cellIndex(x, y)]);
    if (id < 0)
        return false;
    uint t = ptype[id];
    return t == PT_INST || (t == PT_SPRK && parts[id].ctype == int(PT_INST));
}

void push(int x, int y)
{
    if (g_sp < pc.listCap * pc.colourNX * pc.colourNY)
        lists[g_sp++] = uint(x) | (uint(y) << 16);
}

void floodInst(int sx, int sy)
{
    const int W = int(pc.W), H = int(pc.H);
    if (!isSparkableInst(sx, sy))
        return;
    g_sp = 0;
    push(sx, sy);
    while (g_sp > 0)
    {
        uint v = lists[--g_sp];
        int x = int(v & 0xFFFF), y = int(v >> 16);
        int x1 = x, x2 = x;
        // go left and right as far as possible
        while (x1 >= CELL && isSparkableInst(x1 - 1, y))
            x1--;
        while (x2 < W - CELL && isSparkableInst(x2 + 1, y))
            x2++;
        // fill the span
        for (int xx = x1; xx <= x2; xx++)
            createPart(xx, y, PT_SPRK);

        // add vertically adjacent pixels (wire crossing for INST)
        if (y >= CELL + 1 && x1 == x2 && isInst(x1 - 1, y - 1) && isInst(x1, y - 1) && isInst(x1 + 1, y - 1) &&
            !isInst(x1 - 1, y - 2) && isInst(x1, y - 2) && !isInst(x1 + 1, y - 2))
        {
            // travelling vertically up, skipping a horizontal line
            if (isSparkableInst(x1, y - 2))
                push(x1, y - 2);
        }
        else if (y >= CELL + 1)
        {
            for (int xx = x1; xx <= x2; xx++)
            {
                // at the end of a horizontal section, or a T junction, or not a 1 px wire crossing
                if (isSparkableInst(xx, y - 1) &&
                    (xx == x1 || xx == x2 || y >= H - CELL - 1 || !isInst(xx, y + 1) || isInst(xx + 1, y + 1) ||
                     isInst(xx - 1, y + 1)))
                    push(xx, y - 1);
            }
        }

        if (y < H - CELL - 1 && x1 == x2 && isInst(x1 - 1, y + 1) && isInst(x1, y + 1) && isInst(x1 + 1, y + 1) &&
            !isInst(x1 - 1, y + 2) && isInst(x1, y + 2) && !isInst(x1 + 1, y + 2))
        {
            // travelling vertically down, skipping a horizontal line
            if (isSparkableInst(x1, y + 2))
                push(x1, y + 2);
        }
        else if (y < H - CELL - 1)
        {
            for (int xx = x1; xx <= x2; xx++)
            {
                if (isSparkableInst(xx, y + 1) &&
                    (xx == x1 || xx == x2 || y < 0 || !isInst(xx, y - 1) || isInst(xx + 1, y - 1) ||
                     isInst(xx - 1, y - 1)))
                    push(xx, y + 1);
            }
        }
    }
}

// flood_prop(x, y, tmp = 2) for BANG: scanline fill over connected BANG. TPT marks visited pixels in a bitmap;
// here a BANG that already has tmp 2 counts as visited.
bool bangToMark(int x, int y)
{
    int id = pmapId(pmap[cellIndex(x, y)]);
    return id >= 0 && ptype[id] == PT_BANG && parts[id].tmp != 2;
}

// Element_PPIP_flood_trigger: sets the spark's trigger (in the bits that take effect next tick) on every PPIP
// connected to (sx, sy), diagonals included; HEAC makes connected PIPE or PPIP conduct heat instead.
void floodPpip(int sx, int sy, uint sparkedBy)
{
    const int W = int(pc.W), H = int(pc.H);
    uint t = typeAt(sx, sy);
    if (t != PT_PIPE && t != PT_PPIP)
        return;
    uint prop = 0;
    if (sparkedBy == PT_PSCN)
        prop = uint(PPIP_TMPFLAG_TRIGGER_ON) << 3;
    else if (sparkedBy == PT_NSCN)
        prop = uint(PPIP_TMPFLAG_TRIGGER_OFF) << 3;
    else if (sparkedBy == PT_INST)
        prop = uint(PPIP_TMPFLAG_TRIGGER_REVERSE) << 3;
    else if (sparkedBy == PT_HEAC)
        prop = uint(PFLAG_CAN_CONDUCT);
    if (prop == 0 || (t != PT_PPIP && sparkedBy != PT_HEAC) ||
        (uint(parts[pmapId(pmap[cellIndex(sx, sy)])].tmp) & prop) != 0)
        return;
    g_sp = 0;
    push(sx, sy);
    while (g_sp > 0)
    {
        uint v = lists[--g_sp];
        int x = int(v & 0xFFFF), y = int(v >> 16);
        // (TPT fills a row again for every cell of it on the stack; skipping done ones gives the same result
        // without the quadratic cost, which one GPU thread feels)
        if ((uint(parts[pmapId(pmap[cellIndex(x, y)])].tmp) & prop) != 0)
            continue;
        int x1 = x, x2 = x;
        while (x1 >= CELL && typeAt(x1 - 1, y) == t)
            x1--;
        while (x2 < W - CELL && typeAt(x2 + 1, y) == t)
            x2++;
        for (int xx = x1; xx <= x2; xx++)
        {
            int id = pmapId(pmap[cellIndex(xx, y)]);
            parts[id].tmp = int(uint(parts[id].tmp) | prop);
        }
        for (int dy = -1; dy <= 1; dy += 2)
        {
            int ny = y + dy;
            if (dy < 0 ? y < CELL + 1 : y >= H - CELL - 1)
                continue;
            for (int xx = x1 - 1; xx <= x2 + 1; xx++)
            {
                if (typeAt(xx, ny) != t)
                    continue;
                int id = pmapId(pmap[cellIndex(xx, ny)]);
                if ((uint(parts[id].tmp) & prop) == 0)
                    push(xx, ny);
            }
        }
    }
}

void floodBang(int sx, int sy)
{
    const int W = int(pc.W), H = int(pc.H);
    if (!bangToMark(sx, sy))
        return;
    g_sp = 0;
    push(sx, sy);
    while (g_sp > 0)
    {
        uint v = lists[--g_sp];
        int x = int(v & 0xFFFF), y = int(v >> 16);
        if (!bangToMark(x, y))
            continue;
        int x1 = x, x2 = x;
        while (x1 >= CELL && bangToMark(x1 - 1, y))
            x1--;
        while (x2 < W - CELL && bangToMark(x2 + 1, y))
            x2++;
        for (int xx = x1; xx <= x2; xx++)
            parts[pmapId(pmap[cellIndex(xx, y)])].tmp = 2;
        if (y >= CELL + 1)
            for (int xx = x1; xx <= x2; xx++)
                if (bangToMark(xx, y - 1))
                    push(xx, y - 1);
        if (y < H - CELL - 1)
            for (int xx = x1; xx <= x2; xx++)
                if (bangToMark(xx, y + 1))
                    push(xx, y + 1);
    }
}

// DMG.cpp's blast: everything within 25 px is pushed away (and the air with it), the pressure rises, and
// breakable things break (their high-pressure transition, or BMTL, GLAS, COAL, QRTZ, TUNG, WOOD).
void blastDMG(uint id)
{
    int x = int(parts[id].x + 0.5), y = int(parts[id].y + 0.5);
    killPart(id, x, y);
    const int rad = 25;
    for (int nj = -rad; nj <= rad; nj++)
    {
        for (int ni = -rad; ni <= rad; ni++)
        {
            int xx = x + ni, yy = y + nj;
            if (!inSim(xx, yy) || (ni == 0 && nj == 0))
                continue;
            int dist = int(sqrt(float(ni * ni + nj * nj)));
            if (dist > rad)
                continue;
            int rid = pmapId(pmap[cellIndex(xx, yy)]);
            if (rid < 0 || ptype[rid] == PT_NONE)
                continue;
            float angle = atan2(float(nj), float(ni));
            float fx = cos(angle) * 7.0, fy = sin(angle) * 7.0;
            parts[rid].vx += fx;
            parts[rid].vy += fy;
            uint ci = airIndex(xx, yy);
            setAirVX(ci, airVX(ci) + fx);
            setAirVY(ci, airVY(ci) + fy);
            setAirPV(ci, airPV(ci) + 1.0);
            uint t = ptype[rid];
            int hpt = elems[t].highPressureT;
            if (hpt > -1 && hpt < MAX_ELEMENTS)
            {
                if (hpt == int(PT_NONE))
                    killPart(uint(rid), xx, yy);
                else if (gpuHas(uint(hpt)))
                    partChangeType(uint(rid), uint(hpt));
            }
            else if (t == PT_BMTL)
                partChangeType(uint(rid), PT_BRMT);
            else if (t == PT_GLAS)
                partChangeType(uint(rid), PT_BGLA);
            else if (t == PT_COAL)
                partChangeType(uint(rid), PT_BCOL);
            else if (t == PT_QRTZ)
                partChangeType(uint(rid), PT_PQRT);
            else if (t == PT_TUNG)
            {
                partChangeType(uint(rid), PT_BRMT);
                parts[rid].ctype = int(PT_TUNG);
            }
            else if (t == PT_WOOD)
                partChangeType(uint(rid), PT_SAWD);
        }
    }
}

// BOMB.cpp's explosion: everything within 8 px (except DMND, cloners and VIBR) becomes hot embers and the
// pressure rises; a ring of embers flies out from just beyond.
void explodeBomb(uint id)
{
    if (ptype[id] == PT_DMG)
    {
        blastDMG(id);
        return;
    }
    if (ptype[id] != PT_BOMB)
        return; // destroyed by an earlier explosion
    int x = int(parts[id].x + 0.5), y = int(parts[id].y + 0.5);
    killPart(id, x, y);
    const int rad = 8;
    for (int nj = -rad; nj <= rad; nj++)
    {
        for (int ni = -rad; ni <= rad; ni++)
        {
            if (ni * ni + nj * nj > rad * rad || !inSim(x + ni, y + nj))
                continue;
            int xx = x + ni, yy = y + nj;
            int rid = pmapId(pmap[cellIndex(xx, yy)]);
            uint nt = rid < 0 ? PT_NONE : ptype[rid];
            if (nt == PT_DMND || nt == PT_CLNE || nt == PT_PCLN || nt == PT_BCLN || nt == PT_VIBR)
                continue;
            if (nt != PT_NONE)
                killPart(uint(rid), xx, yy);
            uint ci = airIndex(xx, yy);
            setAirPV(ci, airPV(ci) + 0.1);
            // BOMB -> EMBR
            int nb = createPartStacked(xx, yy, PT_EMBR);
            if (nb >= 0)
            {
                parts[nb].tmp = 2;
                parts[nb].life = 2;
                parts[nb].temp = MAX_TEMP;
            }
        }
    }
    for (int nj = -(rad + 1); nj <= rad + 1; nj++)
    {
        for (int ni = -(rad + 1); ni <= rad + 1; ni++)
        {
            if (ni * ni + nj * nj > (rad + 1) * (rad + 1) || !inSim(x + ni, y + nj))
                continue;
            if (typeAt(x + ni, y + nj) != PT_NONE)
                continue;
            int nb = createPartStacked(x + ni, y + nj, PT_EMBR);
            if (nb >= 0)
            {
                parts[nb].tmp = 0;
                parts[nb].life = 50;
                parts[nb].temp = MAX_TEMP;
                parts[nb].vx = float(rngBetween(-20, 20));
                parts[nb].vy = float(rngBetween(-20, 20));
            }
        }
    }
}

[shader("compute")]
[numthreads(1, 1, 1)]
void main(uint3 tid: SV_DispatchThreadID)
{
    uint cells = pc.CW * pc.CH;
    uint n = min(counters[CTR_INST_REQ], cells);
    for (uint k = 0; k < n; k++)
    {
        uint v = emapReqs[cells + k];
        floodInst(int(v & 0xFFFF), int(v >> 16));
    }
    n = min(counters[CTR_BANG_REQ], cells);
    for (uint k = 0; k < n; k++)
    {
        uint v = emapReqs[2 * cells + k];
        floodBang(int(v & 0xFFFF), int(v >> 16));
    }
    n = min(counters[CTR_PPIP_REQ], cells / 2);
    for (uint k = 0; k < n; k++)
    {
        uint v = emapReqs[7 * cells + 2 * k];
        floodPpip(int(v & 0xFFFF), int(v >> 16), emapReqs[7 * cells + 2 * k + 1]);
    }
    n = min(counters[CTR_PSTN_REQ], cells);
    for (uint k = 0; k < n; k++)
    {
        uint v = emapReqs[8 * cells + k];
        rngSeed(0x9157u + k);
        pistonRun(v & 0x3FFFFFFFu, v >> 30);
    }
    n = min(counters[CTR_BOMB_REQ], cells);
    for (uint k = 0; k < n; k++)
    {
        rngSeed(0xB0B0u + k);
        explodeBomb(emapReqs[3 * cells + k]);
    }
    // Sizes of the beam and EMP passes that follow (empty unless there is work)
    uint rays = min(counters[CTR_RAY_REQ], 2 * cells / 3);
    counters[CTR_RAY_ARGS] = (rays + 63) / 64;
    counters[CTR_RAY_ARGS + 1] = 1;
    counters[CTR_RAY_ARGS + 2] = 1;
    counters[CTR_EMP_ARGS] = counters[CTR_EMP] != 0 ? (pc.capacity + 255) / 256 : 0;
    counters[CTR_EMP_ARGS + 1] = 1;
    counters[CTR_EMP_ARGS + 2] = 1;
}
