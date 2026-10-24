// The interface (src/ui/Canvas) over the frame: each interface pixel covers scale x scale screen pixels; its
// premultiplied colour goes over what is there, as TPT's blends would have gone onto the picture.

struct OverlayConsts
{
    uint screenW, screenH;
    uint uiW, uiH, scale;
};

[[vk::push_constant]] ConstantBuffer<OverlayConsts> oc;
[[vk::binding(0, 0)]] StructuredBuffer<uint> overlay; // r | g << 8 | b << 16 | coverage << 24
[[vk::binding(1, 0)]] RWStructuredBuffer<uint> frame;  // 0xAARRGGBB

[shader("compute")]
[numthreads(16, 16, 1)]
void main(uint3 tid: SV_DispatchThreadID)
{
    if (tid.x >= oc.screenW || tid.y >= oc.screenH)
        return;
    uint u = tid.x / oc.scale, v = tid.y / oc.scale;
    if (u >= oc.uiW || v >= oc.uiH)
        return;
    uint p = overlay[v * oc.uiW + u];
    if (p == 0)
        return;
    uint a = p >> 24;
    uint i = tid.y * oc.screenW + tid.x;
    uint f = frame[i];
    int3 c = int3(int(p & 0xFF), int((p >> 8) & 0xFF), int((p >> 16) & 0xFF));
    int3 w = int3(int((f >> 16) & 0xFF), int((f >> 8) & 0xFF), int(f & 0xFF));
    int3 o = min(c + (int(255 - a) * w) / 255, int3(255, 255, 255));
    frame[i] = 0xFF000000u | (uint(o.x) << 16) | (uint(o.y) << 8) | uint(o.z);
}
