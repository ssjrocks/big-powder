// TPT's fire buffer (fire_r/g/b, one colour per cell, kept from frame to frame).
//  - colourIdx 0, one thread per cell, before resolve.slang: an electrified wall's glow (DrawWalls), then what the
//    particles in the cell add or blend in (render_parts' fire effects), in id order: the top particles of its 16
//    pixels from pmap and photons, hidden ones from the cell's fragment list.
//  - fire_smooth.slang, after resolve.slang has drawn the glow, smooths it for the next frame.
#include "render_common.slang"

static const uint MAX_FIRE_OPS = 48;

int3 applyFire(int3 f, uint mode, uint colour)
{
    int4 c = unpackRGBA(colour);
    int a = c.w;
    if (a != 0 && (mode & FIRE_BLEND) != 0)
    {
        a /= 2;
        f = (a * c.xyz + (255 - a) * f) >> 8;
    }
    if (a != 0 && (mode & FIRE_ADD) != 0)
    {
        a /= 8;
        f = min(((a * c.xyz) >> 8) + f, int3(255, 255, 255));
    }
    if (a != 0 && (mode & FIRE_SPARK) != 0)
    {
        a /= 4;
        f = (a * c.xyz + (255 - a) * f) >> 8;
    }
    return f;
}

[shader("compute")]
[numthreads(1024, 1, 1)]
void main(uint3 tid: SV_DispatchThreadID, uint gi: SV_GroupIndex)
{
    int CW = int(pc.CW), CH = int(pc.CH);
    if (pc.colourIdx == 0)
    {
        uint cell = tid.x;
        if (cell >= pc.CW * pc.CH)
            return;
        int cx = int(cell % pc.CW), cy = int(cell / pc.CW);
        int3 f = unpackRGB(fireBuf[cell]);
        uint wt = bmapCell(cx, cy);
        if (wt != 0 && wt < 19 && emapCell(cx, cy) != 0)
        {
            uint glow = renderTables[RT_WALL_EGLOW + wt];
            if (glow != 0)
                f = (255 * unpackRGB(glow)) >> 8; // (alpha 255: the old value drops out)
        }
        uint ids[MAX_FIRE_OPS], modes[MAX_FIRE_OPS], cols[MAX_FIRE_OPS];
        uint n = 0;
        for (int j = 0; j < CELL; j++)
            for (int i = 0; i < CELL; i++)
            {
                int x = cx * CELL + i, y = cy * CELL + j;
                if (x >= int(pc.W) || y >= int(pc.H))
                    continue;
                uint c = cellIndex(x, y);
                for (int which = 0; which < 2; which++)
                {
                    int id = pmapId(which == 0 ? pmap[c] : photons[c]);
                    if (id < 0 || ptype[id] == PT_NONE)
                        continue;
                    uint mode = gfxMode(uint(id)), fc = gfxFire(uint(id));
                    if ((fc >> 24) == 0 || (mode & (FIRE_ADD | FIRE_BLEND | FIRE_SPARK)) == 0)
                        continue;
                    if (n < MAX_FIRE_OPS)
                    {
                        ids[n] = uint(id);
                        modes[n] = mode;
                        cols[n++] = fc;
                    }
                }
            }
        for (uint fl = fragHeads[fireList(cell)]; fl != 0 && n < MAX_FIRE_OPS;)
        {
            uint4 fr = frags[fl - 1];
            ids[n] = fr.x >> 5;
            modes[n] = fr.w;
            cols[n++] = fr.y;
            fl = fr.z & FRAG_NEXT;
        }
        for (uint a = 1; a < n; a++)
        {
            uint k = ids[a], m = modes[a], v = cols[a];
            int b = int(a) - 1;
            while (b >= 0 && ids[b] > k)
            {
                ids[b + 1] = ids[b];
                modes[b + 1] = modes[b];
                cols[b + 1] = cols[b];
                b--;
            }
            ids[b + 1] = k;
            modes[b + 1] = m;
            cols[b + 1] = v;
        }
        for (uint a = 0; a < n; a++)
            f = applyFire(f, modes[a], cols[a]);
        fireBuf[cell] = packRGB(f);
        if (f.x != 0 || f.y != 0 || f.z != 0)
        {
            // the area fire_smooth.slang has to go over
            uint o;
            InterlockedMin(counters[CTR_FIRE_BOX], uint(cx), o);
            InterlockedMin(counters[CTR_FIRE_BOX + 1], uint(cy), o);
            InterlockedMax(counters[CTR_FIRE_BOX + 2], uint(cx), o);
            InterlockedMax(counters[CTR_FIRE_BOX + 3], uint(cy), o);
        }
        return;
    }

    // (colourIdx 1 is fire_smooth.slang)
}
