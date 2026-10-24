// What is drawn over the world in world pixels, through the camera, onto the frame (gpu/Overlay::recordLayer), in
// TPT's order:
//  - the frame darkened by `fade` (TPT's fade behind a window);
//  - TPT's selection (GameView::OnDraw): the world darkened by `dim` outside the area chosen (all of it while none
//    is); BlendFilledRect of black;
//  - a picture: ui/Canvas pixels with their coverage (the signs over the sign window), or opaque ones blended at
//    `alpha` (the save being placed; BlendImage);
//  - a dotted rectangle XORed around the area (XorDottedRect: RasterizeDottedRect's pixels, light or dark grey
//    against what is under them).

struct LayerConsts
{
    uint screenW, screenH;
    int x0, y0;  // the picture's world position and size
    uint w, h;
    float camX, camY, zoom;
    uint fade;   // 0-255
    uint flags;  // 1 the picture is opaque (blended at alpha), 2 the dotted rectangle
    uint alpha;
    uint worldW, worldH, dim;
    int rx0, ry0, rx1, ry1; // the area not darkened (none if rx1 < rx0)
    int dx0, dy0, dx1, dy1; // the dotted rectangle's corners
};

[[vk::push_constant]] ConstantBuffer<LayerConsts> lc;
[[vk::binding(0, 0)]] StructuredBuffer<uint> layer;   // r | g << 8 | b << 16 | coverage << 24
[[vk::binding(1, 0)]] RWStructuredBuffer<uint> frame; // 0xAARRGGBB

// RasterizeDottedRect's pixels
bool onDotted(int x, int y)
{
    int x1 = lc.dx0, y1 = lc.dy0, x2 = lc.dx1, y2 = lc.dy1;
    if (x < x1 || x > x2 || y < y1 || y > y2)
        return false;
    if (y == y1 && ((x - x1) & 1) == 0)
        return true;
    if (y1 != y2 && y == y2 && ((x - x1) & 1) == ((y2 - y1) & 1))
        return true;
    if (x == x1 && y >= y1 + 2 && y < y2 && ((y - y1) & 1) == 0)
        return true;
    int leftOff = (x2 - x1 + 1) & 1;
    if (x1 != x2 && x == x2 && y >= y1 + 1 + leftOff && y < y2 && ((y - y1 - 1 - leftOff) & 1) == 0)
        return true;
    return false;
}

// Zoomed out, a screen pixel takes the rectangle's lines if any of the world pixels it covers is on them
bool onLines(float fx0, float fy0, float fx1, float fy1)
{
    int x1 = lc.dx0, y1 = lc.dy0, x2 = lc.dx1, y2 = lc.dy1;
    bool xs = fx1 > float(x1) && fx0 < float(x2 + 1), ys = fy1 > float(y1) && fy0 < float(y2 + 1);
    bool row = (fy0 <= float(y1) && float(y1) < fy1) || (fy0 <= float(y2) && float(y2) < fy1);
    bool col = (fx0 <= float(x1) && float(x1) < fx1) || (fx0 <= float(x2) && float(x2) < fx1);
    return (row && xs) || (col && ys);
}

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
    int wx = int(floor(lc.camX + (float(tid.x) + 0.5) / lc.zoom));
    int wy = int(floor(lc.camY + (float(tid.y) + 0.5) / lc.zoom));
    if (lc.dim != 0 && wx >= 0 && wy >= 0 && wx < int(lc.worldW) && wy < int(lc.worldH) &&
        !(lc.rx1 >= lc.rx0 && wx >= lc.rx0 && wx <= lc.rx1 && wy >= lc.ry0 && wy <= lc.ry1))
        under = (int(255 - lc.dim) * under) / 255;
    if (lc.zoom >= 1.0)
    {
        int lx = wx - lc.x0, ly = wy - lc.y0;
        if (lx >= 0 && ly >= 0 && lx < int(lc.w) && ly < int(lc.h))
        {
            uint p = layer[uint(ly) * lc.w + uint(lx)];
            int3 c = int3(int(p & 0xFF), int((p >> 8) & 0xFF), int((p >> 16) & 0xFF));
            if ((lc.flags & 1) != 0) // RGB::Blend
                under = (int(lc.alpha) * c + int(255 - lc.alpha) * under) / 255;
            else
            {
                uint a = p >> 24;
                under = min(c + (int(255 - a) * under) / 255, int3(255, 255, 255));
            }
        }
    }
    else if (lc.w > 0)
    {
        // Zoomed out, the picture's pixels the screen pixel covers, averaged (up to 8 x 8 of them), as render.slang
        // averages the world's
        float inv = 1.0 / lc.zoom;
        float fx0 = lc.camX + float(tid.x) * inv - float(lc.x0), fy0 = lc.camY + float(tid.y) * inv - float(lc.y0);
        int x0 = int(floor(fx0)), y0 = int(floor(fy0)), x1 = int(floor(fx0 + inv)), y1 = int(floor(fy0 + inv));
        int step = max(1, (x1 - x0 + 7) / 8);
        float3 sum = float3(0, 0, 0);
        float cover = 0, samples = 0;
        for (int y = y0; y < y1; y += step)
            for (int x = x0; x < x1; x += step)
            {
                samples += 1;
                if (x < 0 || y < 0 || x >= int(lc.w) || y >= int(lc.h))
                    continue;
                uint p = layer[uint(y) * lc.w + uint(x)];
                float a = (lc.flags & 1) != 0 ? float(lc.alpha) / 255.0 : float(p >> 24) / 255.0;
                float3 c = float3(float(p & 0xFF), float((p >> 8) & 0xFF), float((p >> 16) & 0xFF));
                sum += (lc.flags & 1) != 0 ? c * a : c; // (premultiplied either way)
                cover += a;
            }
        if (samples > 0 && cover > 0)
        {
            sum /= samples;
            cover /= samples;
            under = int3(min(sum + float3(under) * (1.0 - cover) + 0.5, float3(255, 255, 255)));
        }
    }
    if ((lc.flags & 2) != 0)
    {
        bool on;
        if (lc.zoom >= 1.0)
            on = onDotted(wx, wy);
        else
        {
            float fx0 = lc.camX + float(tid.x) / lc.zoom, fy0 = lc.camY + float(tid.y) / lc.zoom;
            on = onLines(fx0, fy0, fx0 + 1.0 / lc.zoom, fy0 + 1.0 / lc.zoom);
        }
        if (on) // XorPixel
            under = (2 * under.x + 3 * under.y + under.z < 512) ? int3(0xC0, 0xC0, 0xC0) : int3(0x40, 0x40, 0x40);
    }
    frame[i] = 0xFF000000u | (uint(under.x) << 16) | (uint(under.y) << 8) | uint(under.z);
}
