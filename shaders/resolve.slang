// One thread per world pixel: TPT's frame at that pixel (resolve_common.slang), for the common case of at most the
// top particle and the top energy particle drawing there. Pixels with more going on (fragments, a glowing or
// blurred particle near, blob display's walls) are marked for resolve_heavy.slang.
#include "resolve_common.slang"

// Its own pixel's operations (flat, blend, add, blob centre) in TPT's order
int3 ownOps(int3 px, uint id)
{
    uint mode = gfxMode(id), col = gfxColour(id);
    if ((mode & PMODE_FLAT) != 0)
        px = unpackRGBA(col).xyz;
    if ((mode & PMODE_BLEND) != 0)
        px = blendRGB(px, unpackRGBA(col));
    if ((mode & PMODE_ADD) != 0)
        px = addRGB(px, unpackRGBA(col));
    if ((mode & PMODE_BLOB) != 0)
        px = unpackRGBA(col).xyz;
    return px;
}

bool blocksFlagged(int x, int y, int reach, uint flag)
{
    int bx0 = max(x - reach, 0) / CELL, bx1 = min(x + reach, int(pc.W) - 1) / CELL;
    int by0 = max(y - reach, 0) / CELL, by1 = min(y + reach, int(pc.H) - 1) / CELL;
    for (int by = by0; by <= by1; by++)
        for (int bx = bx0; bx <= bx1; bx++)
            if ((gfxFlags[uint(by) * pc.CW + uint(bx)] & flag) != 0)
                return true;
    return false;
}

[shader("compute")]
[numthreads(16, 16, 1)]
void main(uint3 tid: SV_DispatchThreadID)
{
    int x = int(tid.x), y = int(tid.y);
    if (x >= int(pc.W) || y >= int(pc.H))
        return;
    uint c = uint(y) * pc.W + uint(x);
    if (fragHeads[c] != 0 || (pc.renderMode & PMODE_BLOB) != 0 ||
        ((pc.renderMode & PMODE_BLUR) != 0 && blocksFlagged(x, y, 3, GFX_FLAG_BLUR)) ||
        ((pc.renderMode & PMODE_GLOW) != 0 && blocksFlagged(x, y, 5, GFX_FLAG_GLOW)))
    {
        video[c] = PIXEL_LATER;
        return;
    }
    int3 px = pixelBase(x, y, c, true);
    int a = pmapId(pmap[c]), b = pmapId(photons[c]);
    if (a >= 0 && ptype[a] == PT_NONE)
        a = -1;
    if (b >= 0 && ptype[b] == PT_NONE)
        b = -1;
    if (a >= 0 && b >= 0 && b < a) // in id order
    {
        int t = a;
        a = b;
        b = t;
    }
    if (a >= 0)
        px = ownOps(px, uint(a));
    if (b >= 0)
        px = ownOps(px, uint(b));
    finishPixel(x, y, c, px);
}
