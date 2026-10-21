// LOVE and LOLZ (Simulation::BeforeSim): every 9x9 block of the world (aligned to 9) that holds either is redrawn
// each tick as a heart (LOVE) or as "LOL" (LOLZ), and both die near the edges. One thread per block; a block's
// drawing stays inside it, so blocks don't affect each other. Dispatched only on ticks where prepare saw LOVE or
// LOLZ (walls_pre.slang).
#include "common.slang"
#include "elements.slang"

// Element_LOVE_RuleTable[nnx][nny] as bit nny of row nnx; Element_LOLZ_RuleTable[nny][nnx] as bit nnx of row nny.
static const uint LOVE_ROWS[9] = { 12, 50, 65, 129, 258, 129, 65, 50, 12 };
static const uint LOLZ_ROWS[9] = { 0, 65, 65, 153, 165, 165, 154, 130, 130 };

int liveAt(int x, int y)
{
    int id = pmapId(pmap[cellIndex(x, y)]);
    return (id >= 0 && ptype[id] != PT_NONE) ? id : -1;
}

[shader("compute")]
[numthreads(64, 1, 1)]
void main(uint3 tid: SV_DispatchThreadID)
{
    int W = int(pc.W), H = int(pc.H);
    int bw = (W + 8) / 9, bh = (H + 8) / 9;
    if (tid.x >= uint(bw * bh))
        return;
    int bx = int(tid.x) % bw, by = int(tid.x) / bw;
    rngSeed(tid.x ^ 0x10E10Eu);

    // Find LOVE and LOLZ in the block (TPT looks at all but the last 4 rows and columns), killing those near
    // the edges
    bool love = false, lolz = false;
    for (int ny = by * 9; ny < by * 9 + 9 && ny < H - 4; ny++)
    {
        for (int nx = bx * 9; nx < bx * 9 + 9 && nx < W - 4; nx++)
        {
            int id = liveAt(nx, ny);
            if (id < 0)
                continue;
            uint t = ptype[id];
            if (t != PT_LOVE && t != PT_LOLZ)
                continue;
            if (ny < 9 || nx < 9 || ny > H - 7 || nx > W - 10)
                killPart(uint(id), nx, ny);
            else if (t == PT_LOVE)
                love = true;
            else
                lolz = true;
        }
    }
    // Redraw the block from its corner, where TPT's loop over the world first reaches it
    int sx = bx * 9, sy = by * 9;
    if (!(love || lolz) || sx < 9 || sx > W - 18 || sy < 9 || sy > H - 7)
        return;
    for (int pass = 0; pass < 2; pass++)
    {
        if (pass == 0 ? !love : !lolz)
            continue;
        uint t = pass == 0 ? PT_LOVE : PT_LOLZ;
        for (int nnx = 0; nnx < 9; nnx++)
        {
            for (int nny = 0; nny < 9; nny++)
            {
                int px = sx + nnx, py = sy + nny;
                if (!(py > 0 && py < H && px >= 0 && px < W))
                    continue;
                bool want = pass == 0 ? ((LOVE_ROWS[nnx] >> uint(nny)) & 1u) != 0 : ((LOLZ_ROWS[nny] >> uint(nnx)) & 1u) != 0;
                int id = liveAt(px, py);
                if (id < 0)
                {
                    if (want)
                    {
                        int nid = createPart(px, py, t);
                        if (nid >= 0)
                            enlistNew(uint(nid), px, py);
                    }
                }
                else if (ptype[id] == t && !want)
                    killPart(uint(id), px, py);
            }
        }
    }
}
