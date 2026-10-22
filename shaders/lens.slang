// Renderer::render_gravlensing (the warp of Fancy Display), last in the frame: each pixel takes its red, green and
// blue from 0.75, 0.875 and 1 times the Newtonian gravity field back along it, from a copy of the frame (in
// fragHeads, free by now); where any of the three falls outside the world, the pixel is black. Without gravity
// it changes nothing.
#include "render_common.slang"

[shader("compute")]
[numthreads(16, 16, 1)]
void main(uint3 tid: SV_DispatchThreadID)
{
    int x = int(tid.x), y = int(tid.y);
    if (x >= int(pc.W) || y >= int(pc.H))
        return;
    float2 g = gravForce(x, y);
    int2 rp = int2(int(float(x) - g.x * 0.75 + 0.5), int(float(y) - g.y * 0.75 + 0.5));
    int2 gp = int2(int(float(x) - g.x * 0.875 + 0.5), int(float(y) - g.y * 0.875 + 0.5));
    int2 bp = int2(int(float(x) - g.x + 0.5), int(float(y) - g.y + 0.5));
    uint c = uint(y) * pc.W + uint(x);
    if (!inVideo(rp.x, rp.y) || !inVideo(gp.x, gp.y) || !inVideo(bp.x, bp.y))
    {
        video[c] = 0;
        return;
    }
    int r = unpackRGB(fragHeads[uint(rp.y) * pc.W + uint(rp.x)]).x;
    int gr = unpackRGB(fragHeads[uint(gp.y) * pc.W + uint(gp.x)]).y;
    int b = unpackRGB(fragHeads[uint(bp.y) * pc.W + uint(bp.x)]).z;
    video[c] = packRGB(int3(r, gr, b));
}
