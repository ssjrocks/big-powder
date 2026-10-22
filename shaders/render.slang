// Shows the world image (resolve.slang: TPT's renderer at one pixel per cell) in the window, zoomed and panned,
// with the brush outline. Zoomed out, each screen pixel averages the world pixels it covers.
#include "render_common.slang"

[[vk::binding(32, 0)]] RWStructuredBuffer<uint> mips; // the world image's pyramid (mip.slang)
static const uint MIP_LEVELS = 4;

// Level `level` of the world image (0: the image itself): its size and where it starts in mips
uint2 levelSize(uint level) { return uint2((pc.W + (1u << level) - 1) >> level, (pc.H + (1u << level) - 1) >> level); }
uint levelOffset(uint level)
{
    uint at = 0;
    for (uint l = 1; l < level; l++)
    {
        uint2 s = levelSize(l);
        at += s.x * s.y;
    }
    return at;
}

float3 levelPixel(uint level, uint offset, int x, int y)
{
    uint2 size = levelSize(level);
    if (x < 0 || y < 0 || x >= int(size.x) || y >= int(size.y))
        return float3(0.07, 0.07, 0.08);
    uint c = level == 0 ? video[uint(y) * pc.W + uint(x)] : mips[offset + uint(y) * size.x + uint(x)];
    return float3(unpackRGB(c)) / 255.0;
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

    // When zoomed out, the average of what the screen pixel covers: up to 4x4 texels of the pyramid level whose
    // texels are at most a quarter of the pixel's width (level 0, the world image itself, down to 1:4)
    uint level = 0;
    while (level < MIP_LEVELS && inv / float(1u << level) > 4.0)
        level++;
    float scale = 1.0 / float(1u << level), span = inv * scale;
    uint offset = levelOffset(level);
    int k = clamp(int(ceil(span)), 1, 4);
    float3 sum = float3(0.0, 0.0, 0.0);
    for (int sy = 0; sy < k; sy++)
        for (int sx = 0; sx < k; sx++)
        {
            float lx = wx0 * scale + (float(sx) + 0.5) * span / float(k);
            float ly = wy0 * scale + (float(sy) + 0.5) * span / float(k);
            sum += levelPixel(level, offset, int(floor(lx)), int(floor(ly)));
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
