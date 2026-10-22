// Draws the visible part of the world into the frame buffer (one BGRA pixel per uint).
// mode 0: element colours, mode 1: temperature.
#include "common.slang"
#include "colours.slang"

// TPT's wall colours (SimulationData.cpp LoadWalls), indexed by wall id.
static const uint WALL_COLOURS[19] = { 0x808080, 0xC0C0C0, 0x808080, 0xFF8080, 0x808080, 0x8080FF, 0xC0C0C0,
                                       0x808080, 0x808080, 0x3C3C3C, 0x575757, 0xFFFF22, 0x242424, 0x579777,
                                       0xFFEE00, 0xFFAA00, 0xDCDCDC, 0x808080, 0x800080 };

float3 wallColour(int x, int y)
{
    uint w = bmapAt(x, y);
    if (w == 0 || w >= 19)
        return float3(0.0, 0.0, 0.0);
    float3 c = unpackRGB(WALL_COLOURS[w]);
    // Streamlines and E-holes are drawn faintly in TPT; electrified walls glow.
    if (w == WL_STREAM || w == WL_EHOLE)
        c *= 0.35;
    if (emapAt(x, y) >= 8)
        c = lerp(c, float3(1.0, 1.0, 0.6), 0.35);
    return c;
}

float3 cellColour(int x, int y)
{
    if (!inSim(x, y))
        return float3(0.07, 0.07, 0.08);
    uint e = pmap[cellIndex(x, y)];
    int id = pmapId(e);
    if (id < 0)
        return wallColour(x, y);
    uint t = ptype[id];
    if (t == PT_NONE)
        return float3(0.0, 0.0, 0.0);
    if (isStickman(t))
        return wallColour(x, y); // drawn as a figure around its cell (stickmen_draw.slang)
    if (pc.mode == 1)
        return heatColour(parts[id].temp);
    if (t == PT_FIRE)
    {
        // Stand-in for TPT's flame table and fire glow.
        float l = saturate(float(parts[id].life) / 160.0);
        return float3(min(1.0, 0.45 + l), 0.85 * l, 0.25 * l * l);
    }
    if (t == PT_LAVA)
    {
        // LAVA's graphics colour, brighter the hotter it is (its fire glow comes with the display modes).
        int l = max(parts[id].life, 0);
        return float3(float(min(l * 2 + 0xE0, 255)), float(min(l + 0x50, 192)), float(min(l / 2 + 0x10, 128))) / 255.0;
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
