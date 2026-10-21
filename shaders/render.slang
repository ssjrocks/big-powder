// Draws the visible part of the world into the frame buffer (one BGRA pixel per uint).
// mode 0: element colours, mode 1: temperature.
#include "common.slang"

float3 unpackRGB(uint c)
{
    return float3(float((c >> 16) & 0xFF), float((c >> 8) & 0xFF), float(c & 0xFF)) / 255.0;
}

uint packRGB(float3 c)
{
    uint3 u = uint3(saturate(c) * 255.0 + 0.5);
    return 0xFF000000u | (u.x << 16) | (u.y << 8) | u.z;
}

float3 heatColour(float temp)
{
    // Rough TPT-like heat gradient: blue (cold) -> green -> red -> yellow -> white (hot).
    float t = saturate((temp - 200.0) / 3000.0);
    float3 stops[5] = { float3(0.1, 0.1, 0.9), float3(0.1, 0.8, 0.2), float3(0.9, 0.1, 0.1),
                        float3(1.0, 0.85, 0.1), float3(1.0, 1.0, 1.0) };
    float f = t * 4.0;
    int k = min(int(f), 3);
    return lerp(stops[k], stops[k + 1], f - float(k));
}

float3 cellColour(int x, int y)
{
    if (!inSim(x, y))
        return float3(0.07, 0.07, 0.08);
    uint e = pmap[cellIndex(x, y)];
    int id = pmapId(e);
    if (id < 0)
        return float3(0.0, 0.0, 0.0);
    uint t = ptype[id];
    if (t == PT_NONE)
        return float3(0.0, 0.0, 0.0);
    if (pc.mode == 1)
        return heatColour(parts[id].temp);
    if (t == PT_FIRE)
    {
        // Stand-in for TPT's flame table and fire glow.
        float l = saturate(float(parts[id].life) / 160.0);
        return float3(min(1.0, 0.45 + l), 0.85 * l, 0.25 * l * l);
    }
    return unpackRGB(elems[t].colour);
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

    // When zoomed out, average a few cells per screen pixel.
    int k = clamp(int(ceil(inv)), 1, 4);
    float3 sum = float3(0.0, 0.0, 0.0);
    for (int sy = 0; sy < k; sy++)
    {
        for (int sx = 0; sx < k; sx++)
        {
            float wx = wx0 + (float(sx) + 0.5) * inv / float(k);
            float wy = wy0 + (float(sy) + 0.5) * inv / float(k);
            sum += cellColour(int(floor(wx)), int(floor(wy)));
        }
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
    frame[tid.y * pc.screenW + tid.x] = packRGB(col);
}
