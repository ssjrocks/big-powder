// Draws the stick men over the rendered frame, one thread each (TPT's PSPEC_STICKMAN in Renderer.cpp): a square
// head (a diamond for fighters) in the colour of the element it holds, two legs of two lines each, and the
// flames of rocket boots.
#include "common.slang"
#include "colours.slang"

// The screen pixels showing world pixel (wx, wy), as render.slang samples them.
void plot(int wx, int wy, float3 colour, float alpha)
{
    int x0 = int(ceil((float(wx) - pc.camX) * pc.zoom - 0.5));
    int x1 = int(ceil((float(wx) + 1.0 - pc.camX) * pc.zoom - 0.5)) - 1;
    int y0 = int(ceil((float(wy) - pc.camY) * pc.zoom - 0.5));
    int y1 = int(ceil((float(wy) + 1.0 - pc.camY) * pc.zoom - 0.5)) - 1;
    x1 = max(x1, x0); // zoomed out: at least one pixel
    y1 = max(y1, y0);
    x0 = max(x0, 0);
    y0 = max(y0, 0);
    x1 = min(x1, int(pc.screenW) - 1);
    y1 = min(y1, int(pc.screenH) - 1);
    for (int sy = y0; sy <= y1; sy++)
        for (int sx = x0; sx <= x1; sx++)
        {
            uint k = uint(sy) * pc.screenW + uint(sx);
            float3 c = alpha >= 1.0 ? colour : lerp(unpackRGB(frame[k]), colour, alpha);
            frame[k] = packRGB(c);
        }
}

// Bresenham, like TPT's DrawLine
void line(int ax, int ay, int bx, int by, float3 colour)
{
    int dx = abs(bx - ax), dy = -abs(by - ay);
    int sx = ax < bx ? 1 : -1, sy = ay < by ? 1 : -1;
    int err = dx + dy;
    for (int n = 0; n < 512; n++)
    {
        plot(ax, ay, colour, 1.0);
        if (ax == bx && ay == by)
            break;
        int e2 = 2 * err;
        if (e2 >= dy)
        {
            err += dy;
            ax += sx;
        }
        if (e2 <= dx)
        {
            err += dx;
            ay += sy;
        }
    }
}

[shader("compute")]
[numthreads(128, 1, 1)]
void main(uint3 tid: SV_DispatchThreadID)
{
    uint s = tid.x;
    if (s >= STICKMEN)
        return;
    Stickman st = stickmen[s];
    uint ref = st.partRef;
    if (st.spwn == 0 || ref == 0 || ref == STKM_IN_PORTAL)
        return;
    uint i = ref - 1;
    uint t = ptype[i];
    if (!isStickman(t))
        return;
    int nx = int(parts[i].x + 0.5), ny = int(parts[i].y + 0.5);

    float3 head, legs;
    if (pc.mode == 1)
    {
        head = heatColour(parts[i].temp);
        legs = head;
    }
    else
    {
        if (st.fan != 0)
            head = unpackRGB(0x8080FF);
        else if (st.elem > 0 && st.elem < int(MAX_ELEMENTS))
            head = unpackRGB(elems[st.elem].colour);
        else
            head = unpackRGB(0x8080FF);
        legs = t == PT_STKM2 ? unpackRGB(0x6464FF) : float3(1.0, 1.0, 1.0);
    }

    if (t == PT_FIGH)
    {
        line(nx, ny + 2, nx + 2, ny, head);
        line(nx + 2, ny, nx, ny - 2, head);
        line(nx, ny - 2, nx - 2, ny, head);
        line(nx - 2, ny, nx, ny + 2, head);
    }
    else
    {
        line(nx - 2, ny + 2, nx + 2, ny + 2, head);
        line(nx - 2, ny - 2, nx + 2, ny - 2, head);
        line(nx - 2, ny - 2, nx - 2, ny + 2, head);
        line(nx + 2, ny - 2, nx + 2, ny + 2, head);
    }
    line(nx, ny + 3, int(st.legs[0]), int(st.legs[1]), legs);
    line(int(st.legs[0]), int(st.legs[1]), int(st.legs[4]), int(st.legs[5]), legs);
    line(nx, ny + 3, int(st.legs[8]), int(st.legs[9]), legs);
    line(int(st.legs[8]), int(st.legs[9]), int(st.legs[12]), int(st.legs[13]), legs);
    if (st.rocketBoots != 0)
    {
        for (int leg = 0; leg < 2; leg++)
        {
            int fx = int(st.legs[leg * 8 + 4]), fy = int(st.legs[leg * 8 + 5]);
            bool firing = (st.comm & 4) != 0 || ((st.comm & 1) != 0 && leg == 0) || ((st.comm & 2) != 0 && leg == 1);
            plot(fx, fy, firing ? float3(0.0, 1.0, 0.0) : float3(1.0, 0.0, 0.0), 1.0);
            float3 flame = float3(1.0, 0.0, 1.0);
            plot(fx + 1, fy, flame, 223.0 / 255.0);
            plot(fx - 1, fy, flame, 223.0 / 255.0);
            plot(fx, fy + 1, flame, 223.0 / 255.0);
            plot(fx, fy - 1, flame, 223.0 / 255.0);
            plot(fx + 1, fy - 1, flame, 112.0 / 255.0);
            plot(fx - 1, fy - 1, flame, 112.0 / 255.0);
            plot(fx + 1, fy + 1, flame, 112.0 / 255.0);
            plot(fx - 1, fy + 1, flame, 112.0 / 255.0);
        }
    }
}
