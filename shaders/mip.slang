// The world image's pyramid for zooming out (render.slang): each level halves the one before, each texel the
// rounded average of the 2x2 below it (one texel, or two, at an odd edge). Level 1 is made from the world image.
// Push constants: colourIdx = the level made; colourOX/colourOY = the source's and destination's offsets in mips
// (the source is the world image for level 1); brushX/brushY = the source's size; screenW/screenH = this level's.
#include "render_common.slang"

[[vk::binding(32, 0)]] RWStructuredBuffer<uint> mips;

uint sourceAt(int x, int y)
{
    uint i = uint(y) * uint(pc.brushX) + uint(x);
    return pc.colourIdx == 1 ? video[i] : mips[pc.colourOX + i];
}

[shader("compute")]
[numthreads(16, 16, 1)]
void main(uint3 tid: SV_DispatchThreadID)
{
    if (tid.x >= pc.screenW || tid.y >= pc.screenH)
        return;
    int x0 = int(tid.x) * 2, y0 = int(tid.y) * 2;
    int x1 = min(x0 + 1, pc.brushX - 1), y1 = min(y0 + 1, pc.brushY - 1);
    int3 sum = unpackRGB(sourceAt(x0, y0)) + unpackRGB(sourceAt(x1, y0)) + unpackRGB(sourceAt(x0, y1)) +
               unpackRGB(sourceAt(x1, y1));
    mips[pc.colourOY + tid.y * pc.screenW + tid.x] = packRGB((sum + 2) / 4);
}
