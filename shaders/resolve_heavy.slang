// The pixels resolve.slang left for later (marked in the image): those with fragments, glow or blur around, or blob
// display's walls. Every operation there is put into TPT's order; this kernel's arrays would slow the common case
// down, so it only does work where needed.
//
// Overwrites and blends (A) are sorted by (particle id, step); saturating adds commute with each other, so they are
// summed into the gaps between A's operations instead of being sorted. A blurring particle's up to three blends on
// a pixel are one entry.
#include "resolve_common.slang"

// The top particle (which 0) or top energy particle (1) at (x, y)
bool topAt(int x, int y, uint which, out uint id)
{
    id = 0;
    if (!inVideo(x, y))
        return false;
    uint c = cellIndex(x, y);
    int i = pmapId(which == 0 ? pmap[c] : photons[c]);
    if (i < 0 || ptype[i] == PT_NONE)
        return false;
    id = uint(i);
    return true;
}

static const uint OP_BLUR = 3; // a blurring particle's blends on this pixel: bits 1, 2, 4 for alpha 30, 20, 10

[shader("compute")]
[numthreads(16, 16, 1)]
void main(uint3 tid: SV_DispatchThreadID)
{
    int x = int(tid.x), y = int(tid.y);
    if (x >= int(pc.W) || y >= int(pc.H))
        return;
    uint c = uint(y) * pc.W + uint(x);
    if (video[c] != PIXEL_LATER)
        return;
    int3 px = pixelBase(x, y, c);

    uint aKey[MAX_A], aOp[MAX_A], aCol[MAX_A];
    uint na = 0;
    // own pixels of the top particle and the top energy particle
    for (uint which = 0; which < 2; which++)
    {
        uint id;
        if (!topAt(x, y, which, id))
            continue;
        uint mode = gfxMode(id), col = gfxColour(id), key = id << 5;
        if ((mode & PMODE_FLAT) != 0)
            pushA(aKey, aOp, aCol, na, key | STEP_FLAT, OP_SET, col | 0xFF000000u);
        if ((mode & PMODE_BLEND) != 0)
            pushA(aKey, aOp, aCol, na, key | STEP_BLEND, OP_BLEND, col);
        if ((mode & PMODE_BLOB) != 0)
            pushA(aKey, aOp, aCol, na, key | STEP_BLOB, OP_SET, col | 0xFF000000u);
    }
    for (uint f = fragHeads[c]; f != 0;)
    {
        uint4 fr = frags[f - 1];
        if ((fr.z >> 28) != OP_ADD)
            pushA(aKey, aOp, aCol, na, fr.x, fr.z >> 28, fr.y);
        f = fr.z & FRAG_NEXT;
    }
    // the blur of the top particles around (PMODE_BLUR: blends within 3 pixels), one entry per particle
    if ((pc.renderMode & PMODE_BLUR) != 0)
        for (int by = -3; by < 4; by++)
            for (int bx = -3; bx < 4; bx++)
            {
                int d = abs(bx) + abs(by);
                uint bits = (d < 2 && !(abs(bx) == 2 || abs(by) == 2) ? 1u : 0u) | (d <= 3 && d != 0 ? 2u : 0u) |
                            (d == 2 ? 4u : 0u);
                if (bits == 0)
                    continue;
                for (uint which = 0; which < 2; which++)
                {
                    uint id;
                    if (!topAt(x - bx, y - by, which, id) || (gfxMode(id) & PMODE_BLUR) == 0)
                        continue;
                    pushA(aKey, aOp, aCol, na, (id << 5) | STEP_BLUR, OP_BLUR, (gfxColour(id) & 0xFFFFFFu) | (bits << 24));
                }
            }
    for (uint a = 1; a < na; a++) // insertion sort by key, keeping the order of equal keys
    {
        uint k = aKey[a], o = aOp[a], v = aCol[a];
        int b = int(a) - 1;
        while (b >= 0 && aKey[b] > k)
        {
            aKey[b + 1] = aKey[b];
            aOp[b + 1] = aOp[b];
            aCol[b + 1] = aCol[b];
            b--;
        }
        aKey[b + 1] = k;
        aOp[b + 1] = o;
        aCol[b + 1] = v;
    }
    uint sums[MAX_A + 1];
    for (uint a = 0; a <= na; a++)
        sums[a] = 0;
    // own ADDs
    for (uint which = 0; which < 2; which++)
    {
        uint id;
        if (!topAt(x, y, which, id) || (gfxMode(id) & PMODE_ADD) == 0)
            continue;
        addOp(aKey, na, sums, (id << 5) | STEP_ADD, gfxColour(id));
    }
    for (uint f = fragHeads[c]; f != 0;)
    {
        uint4 fr = frags[f - 1];
        if ((fr.z >> 28) == OP_ADD)
            addOp(aKey, na, sums, fr.x, fr.y);
        f = fr.z & FRAG_NEXT;
    }
    // the glow of the top particles around (PMODE_GLOW: adds within 5 pixels)
    if ((pc.renderMode & PMODE_GLOW) != 0)
        for (int gy = -5; gy < 6; gy++)
            for (int gx = -5; gx < 6; gx++)
            {
                int ax = abs(gx), ay = abs(gy);
                if (ax != 0 && ay != 0 && ax + ay > 7)
                    continue;
                for (uint which = 0; which < 2; which++)
                {
                    uint id;
                    if (!topAt(x - gx, y - gy, which, id) || (gfxMode(id) & PMODE_GLOW) == 0)
                        continue;
                    uint col = gfxColour(id), key = (id << 5) | STEP_GLOW;
                    int cola = int(col >> 24), cola1 = (5 * cola) / 255;
                    uint rgb = col & 0xFFFFFFu;
                    if (ax == 0 && ay == 0)
                        addOp(aKey, na, sums, key, rgb | (uint((192 * cola) / 255) << 24));
                    else
                    {
                        if (ax + ay == 1)
                            addOp(aKey, na, sums, key, rgb | (uint((96 * cola) / 255) << 24));
                        if (cola1 > 0)
                            addOp(aKey, na, sums, key, rgb | (uint(cola1) << 24));
                    }
                }
            }
    for (uint a = 0; a < na; a++)
    {
        px = min(px + unpackRGB(sums[a]), int3(0xFF, 0xFF, 0xFF));
        if (aOp[a] == OP_BLUR)
        {
            int3 rgb = unpackRGBA(aCol[a]).xyz;
            uint bits = aCol[a] >> 24;
            if ((bits & 1) != 0)
                px = blendRGB(px, int4(rgb, 30));
            if ((bits & 2) != 0)
                px = blendRGB(px, int4(rgb, 20));
            if ((bits & 4) != 0)
                px = blendRGB(px, int4(rgb, 10));
        }
        else
            px = applyOp(px, aOp[a], aCol[a]);
    }
    px = min(px + unpackRGB(sums[na]), int3(0xFF, 0xFF, 0xFF));
    finishPixel(x, y, c, px);
}
