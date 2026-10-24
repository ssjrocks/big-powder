// A picture laid over the world (ui/Canvas pixels, premultiplied, one a world pixel) drawn through the camera onto the
// frame, which is first darkened by `fade` (TPT's fade behind a window): the signs while TPT's sign window is up, which
// it draws over the darkened picture.

struct LayerConsts
{
    uint screenW, screenH;
    int x0, y0;  // the layer's world position
    uint w, h;
    float camX, camY, zoom;
    uint fade;   // 0-255
};

[[vk::push_constant]] ConstantBuffer<LayerConsts> lc;
[[vk::binding(0, 0)]] StructuredBuffer<uint> layer;   // r | g << 8 | b << 16 | coverage << 24
[[vk::binding(1, 0)]] RWStructuredBuffer<uint> frame; // 0xAARRGGBB

[shader("compute")]
[numthreads(16, 16, 1)]
void main(uint3 tid: SV_DispatchThreadID)
{
    if (tid.x >= lc.screenW || tid.y >= lc.screenH)
        return;
    uint i = tid.y * lc.screenW + tid.x;
    uint f = frame[i];
    int3 under = int3(int((f >> 16) & 0xFF), int((f >> 8) & 0xFF), int(f & 0xFF));
    under = (int(255 - lc.fade) * under) / 255; // (BlendFilledRect of black)
    // the world pixel under the screen pixel's centre (as render.slang draws the world zoomed in)
    int lx = int(floor(lc.camX + (float(tid.x) + 0.5) / lc.zoom)) - lc.x0;
    int ly = int(floor(lc.camY + (float(tid.y) + 0.5) / lc.zoom)) - lc.y0;
    if (lx >= 0 && ly >= 0 && lx < int(lc.w) && ly < int(lc.h))
    {
        uint p = layer[uint(ly) * lc.w + uint(lx)];
        uint a = p >> 24;
        int3 c = int3(int(p & 0xFF), int((p >> 8) & 0xFF), int((p >> 16) & 0xFF));
        under = min(c + (int(255 - a) * under) / 255, int3(255, 255, 255));
    }
    frame[i] = 0xFF000000u | (uint(under.x) << 16) | (uint(under.y) << 8) | uint(under.z);
}
