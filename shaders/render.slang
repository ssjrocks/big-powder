// Shows the world image (resolve.slang: TPT's renderer at one pixel per cell) in the window, zoomed and panned,
// with the brush outline. Zoomed out, each screen pixel averages the world pixels it covers.
#include "render_common.slang"

float3 worldPixel(int x, int y)
{
    if (!inVideo(x, y))
        return float3(0.07, 0.07, 0.08);
    return float3(unpackRGB(video[uint(y) * pc.W + uint(x)])) / 255.0;
}

uint packBGRA(float3 c)
{
    uint3 u = uint3(saturate(c) * 255.0 + 0.5);
    return 0xFF000000u | (u.x << 16) | (u.y << 8) | u.z;
}

[shader("compute")]
[numthreads(16, 16, 1)]
void main(uint3 tid: SV_DispatchThreadID)
{
    if (tid.x >= pc.screenW || tid.y >= pc.screenH)
        return;
    float inv = 1.0 / pc.zoom;
    float wx0 = pc.camX + float(tid.x) * inv;
    float wy0 = pc.camY + float(tid.y) * inv;

    // When zoomed out, average a few world pixels per screen pixel.
    int k = clamp(int(ceil(inv)), 1, 4);
    float3 sum = float3(0.0, 0.0, 0.0);
    for (int sy = 0; sy < k; sy++)
        for (int sx = 0; sx < k; sx++)
        {
            float wx = wx0 + (float(sx) + 0.5) * inv / float(k);
            float wy = wy0 + (float(sy) + 0.5) * inv / float(k);
            sum += worldPixel(int(floor(wx)), int(floor(wy)));
        }
    float3 col = sum / float(k * k);

    // Brush outline.
    if (pc.brushR >= 0)
    {
        float wx = wx0 + 0.5 * inv;
        float wy = wy0 + 0.5 * inv;
        float d = length(float2(wx - float(pc.brushX) - 0.5, wy - float(pc.brushY) - 0.5));
        if (abs(d - (float(pc.brushR) + 0.5)) < max(0.5, inv))
            col = lerp(col, float3(1.0, 1.0, 1.0), 0.6);
    }
    frame[tid.y * pc.screenW + tid.x] = packBGRA(col);
}
