// Shows the world image (resolve.slang: TPT's renderer at one pixel per cell) in the window, zoomed and panned,
// with TPT's brush cursor. Zoomed out, each screen pixel averages the world pixels it covers.
#include "render_common.slang"

[[vk::binding(32, 0)]] RWStructuredBuffer<uint> mips; // the world image's pyramid (mip.slang)
static const uint MIP_LEVELS = 4;
// TPT's brush cursor (GpuSim::recordRender): how many times TPT inverts each world pixel of a box, two bits each,
// row by row. The box is at (brushX, brushY), brushR wide (0: no cursor) and brushType high.
[[vk::binding(34, 0)]] RWStructuredBuffer<uint> cursor;

uint cursorCount(int x, int y)
{
    int cx = x - pc.brushX, cy = y - pc.brushY;
    if (cx < 0 || cy < 0 || cx >= pc.brushR || cy >= pc.brushType)
        return 0;
    uint i = uint(cy * pc.brushR + cx);
    return (cursor[i >> 4] >> ((i & 15) * 2)) & 3;
}

// RasterDrawMethods' XorPixel, n times
float3 xorPixel(float3 col, uint n)
{
    uint3 u = uint3(saturate(col) * 255.0 + 0.5);
    for (uint k = 0; k < n; k++)
        u = (2 * u.x + 3 * u.y + u.z < 512) ? uint3(0xC0, 0xC0, 0xC0) : uint3(0x40, 0x40, 0x40);
    return float3(u) / 255.0;
}

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

    // The cursor: on the world pixel shown, or zoomed out on any the screen pixel covers (a few of them when it
    // covers many)
    if (pc.brushR > 0)
    {
        uint n = 0;
        if (inv <= 1.0)
            n = cursorCount(int(floor(wx0 + 0.5 * inv)), int(floor(wy0 + 0.5 * inv)));
        else
        {
            int fx0 = max(int(floor(wx0)), pc.brushX), fy0 = max(int(floor(wy0)), pc.brushY);
            int fx1 = min(int(ceil(wx0 + inv)) - 1, pc.brushX + pc.brushR - 1);
            int fy1 = min(int(ceil(wy0 + inv)) - 1, pc.brushY + pc.brushType - 1);
            int step = max(1, int(ceil(inv / 16.0)));
            for (int y = fy0; y <= fy1; y += step)
                for (int x = fx0; x <= fx1; x += step)
                    n = max(n, cursorCount(x, y));
        }
        if (n > 0)
            col = xorPixel(col, n);
    }
    frame[tid.y * pc.screenW + tid.x] = packBGRA(col);
}
