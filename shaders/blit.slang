// Draws a CPU-rendered world image (Classic mode: TPT's own renderer) through the camera, with the brush
// outline. Same output format as render.slang: one BGRA pixel per uint.

struct BlitConsts
{
    float camX, camY, zoom;
    uint screenW, screenH;
    uint imageW, imageH;
    int brushX, brushY, brushR;
};

[[vk::push_constant]] ConstantBuffer<BlitConsts> bc;
[[vk::binding(0, 0)]] StructuredBuffer<uint> image; // 0xRRGGBB
[[vk::binding(1, 0)]] RWStructuredBuffer<uint> frame;

float3 unpackRGB(uint c)
{
    return float3(float((c >> 16) & 0xFF), float((c >> 8) & 0xFF), float(c & 0xFF)) / 255.0;
}

float3 sampleImage(int x, int y)
{
    if (x < 0 || y < 0 || x >= int(bc.imageW) || y >= int(bc.imageH))
        return float3(0.07, 0.07, 0.08);
    return unpackRGB(image[uint(y) * bc.imageW + uint(x)]);
}

[shader("compute")]
[numthreads(16, 16, 1)]
void main(uint3 tid: SV_DispatchThreadID)
{
    if (tid.x >= bc.screenW || tid.y >= bc.screenH)
        return;
    float inv = 1.0 / bc.zoom;
    float wx0 = bc.camX + float(tid.x) * inv;
    float wy0 = bc.camY + float(tid.y) * inv;
    int k = clamp(int(ceil(inv)), 1, 4);
    float3 sum = float3(0.0, 0.0, 0.0);
    for (int sy = 0; sy < k; sy++)
        for (int sx = 0; sx < k; sx++)
            sum += sampleImage(int(floor(wx0 + (float(sx) + 0.5) * inv / float(k))),
                               int(floor(wy0 + (float(sy) + 0.5) * inv / float(k))));
    float3 col = sum / float(k * k);
    if (bc.brushR >= 0)
    {
        float d = length(float2(wx0 + 0.5 * inv - float(bc.brushX) - 0.5, wy0 + 0.5 * inv - float(bc.brushY) - 0.5));
        if (abs(d - (float(bc.brushR) + 0.5)) < max(0.5, inv))
            col = lerp(col, float3(1.0, 1.0, 1.0), 0.6);
    }
    uint3 u = uint3(saturate(col) * 255.0 + 0.5);
    frame[tid.y * bc.screenW + tid.x] = 0xFF000000u | (u.x << 16) | (u.y << 8) | u.z;
}
