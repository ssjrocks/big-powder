// Colour helpers for the drawing kernels (render.slang, stickmen_draw.slang).

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
