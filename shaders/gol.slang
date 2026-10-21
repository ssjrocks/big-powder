// TPT's Simulation::SimulateGoL, before the update passes on ticks where there is LIFE (dispatch sizes from
// walls_pre.slang). Every LIFE at full strength counts towards its 8 neighbours, the Game of Life space wrapping around
// inside the CELL border; each cell then applies its ruleset: a LIFE that may not stay starts dying, and an empty
// cell may be born into, as the kind of its neighbours that is in the majority (the lowest ruleset among equals).
// Three stages (pc.colourIdx): 0 per particle marks which LIFE count (pad1) and counts down the dying ones; 1 per
// cell gathers the counting neighbours in id order, building TPT's five-entry neighbour list exactly; 2 per
// particle removes LIFE that has died.
#include "common.slang"
#include "elements.slang"

bool golStasis(int x, int y) { return bmapAt(x, y) == WL_STASIS && emapAt(x, y) < 8; }

int golWrapX(int x) { return ((x + int(pc.W) - 3 * CELL) % (int(pc.W) - 2 * CELL)) + CELL; }
int golWrapY(int y) { return ((y + int(pc.H) - 3 * CELL) % (int(pc.H) - 2 * CELL)) + CELL; }

bool golInside(int x, int y) { return x >= CELL && y >= CELL && x < int(pc.W) - CELL && y < int(pc.H) - CELL; }

int golTop(int x, int y)
{
    int id = pmapId(pmap[cellIndex(x, y)]);
    return (id >= 0 && ptype[id] != PT_NONE) ? id : -1;
}

void stageMark(uint i)
{
    if (i >= pc.capacity || ptype[i] != PT_LIFE)
        return;
    Particle p = parts[i];
    int x = int(p.x + 0.5), y = int(p.y + 0.5);
    if (!golInside(x, y))
    {
        parts[i].pad1 = 0;
        return;
    }
    uint ruleset = golRuleset(p.ctype);
    if (p.tmp2 == int((ruleset >> 17) & 0xFu) + 1)
        parts[i].pad1 = 1; // counts towards its neighbours this step
    else
    {
        parts[i].pad1 = 0;
        if (!golStasis(x, y))
            parts[i].tmp2 = p.tmp2 - 1;
    }
}

void stageCell(uint c)
{
    int W = int(pc.W), H = int(pc.H);
    if (c >= uint(W * H))
        return;
    int x = int(c) % W, y = int(c) / W;
    if (!golInside(x, y))
        return;
    int r = golTop(x, y);
    if (r >= 0 && ptype[r] != PT_LIFE)
        return;
    // The LIFE that count towards this cell, in id order (TPT goes through the particles in id order)
    uint ids[8] = {};
    uint dirs[8] = {};
    uint n = 0;
    for (int yy = -1; yy <= 1; yy++)
    {
        for (int xx = -1; xx <= 1; xx++)
        {
            if (xx == 0 && yy == 0)
                continue;
            int cid = golTop(golWrapX(x - xx), golWrapY(y - yy));
            if (cid < 0 || ptype[cid] != PT_LIFE || parts[cid].pad1 != 1)
                continue;
            uint j = n++;
            while (j > 0 && ids[j - 1] > uint(cid))
            {
                ids[j] = ids[j - 1];
                dirs[j] = dirs[j - 1];
                j--;
            }
            ids[j] = uint(cid);
            dirs[j] = (uint(yy & 3) << 26) | (uint(xx & 3) << 24);
        }
    }
    if (r < 0 && n == 0)
        return;
    // TPT's neighbour list: up to five kinds of LIFE (ruleset + 1 for built-in ones), each with its count minus
    // one in bits 21-23 and where its first neighbour is in bits 24-27
    uint list[5] = {};
    for (uint k = 0; k < n; k++)
    {
        uint golnum = uint(parts[ids[k]].ctype);
        if (golnum < NGOL)
            golnum += 1;
        for (uint l = 0; l < 5; l++)
        {
            uint have = list[l] & 0x1FFFFFu;
            if (have == golnum)
            {
                list[l] += 1u << 21;
                break;
            }
            if (have == 0)
            {
                list[l] = dirs[k] | golnum;
                break;
            }
        }
    }
    if (golStasis(x, y))
        return;
    uint neighbours = n;
    if (r >= 0)
    {
        // A LIFE that may not stay with this many neighbours starts dying
        Particle p = parts[r];
        uint ruleset = golRuleset(p.ctype);
        if (((ruleset >> neighbours) & 1u) == 0 && p.tmp2 == int(ruleset >> 17) + 1)
            parts[r].tmp2 = p.tmp2 - 1;
        return;
    }
    uint majority = neighbours / 2 + neighbours % 2;
    uint golnumToCreate = 0xFFFFFFFFu, createFrom = 0;
    for (uint l = 0; l < 5; l++)
    {
        uint golnum = list[l] & 0x1FFFFFu;
        if (golnum == 0)
            break;
        uint ruleset = golnum;
        if (golnum - 1 < NGOL)
        {
            ruleset = GOL_RULES[golnum - 1];
            golnum -= 1;
        }
        if (((ruleset >> (neighbours + 8)) & 1u) != 0 && ((list[l] >> 21) & 7u) + 1 >= majority &&
            golnum < golnumToCreate)
        {
            golnumToCreate = golnum;
            createFrom = list[l];
        }
    }
    if (golnumToCreate == 0xFFFFFFFFu)
        return;
    rngSeed(c ^ 0x60160Bu);
    int nid = createPart(x, y, PT_LIFE);
    if (nid < 0)
        return;
    Particle q = parts[nid];
    lifeCreate(q, int(golnumToCreate));
    // Its colours come from the first neighbour of its kind
    int sx = int((createFrom >> 24) & 3u), sy = int((createFrom >> 26) & 3u);
    if (sx == 3)
        sx = -1;
    if (sy == 3)
        sy = -1;
    int sample = golTop(golWrapX(x - sx), golWrapY(y - sy));
    if (sample >= 0)
    {
        q.dcolour = parts[sample].dcolour;
        q.tmp = parts[sample].tmp;
    }
    parts[nid] = q;
    enlistNew(uint(nid), x, y);
}

void stageRemove(uint i)
{
    if (i >= pc.capacity || ptype[i] != PT_LIFE)
        return;
    Particle p = parts[i];
    int x = int(p.x + 0.5), y = int(p.y + 0.5);
    if (golInside(x, y) && golTop(x, y) == int(i) && p.tmp2 <= 0)
        killPart(i, x, y);
}

[shader("compute")]
[numthreads(256, 1, 1)]
void main(uint3 tid: SV_DispatchThreadID)
{
    if (pc.colourIdx == 0)
        stageMark(tid.x);
    else if (pc.colourIdx == 1)
        stageCell(tid.x);
    else
        stageRemove(tid.x);
}
