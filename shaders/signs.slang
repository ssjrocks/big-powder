// Signs (Renderer::DrawSigns), after the frame's other drawing and before the warp effect.
//
// signData: per sign, its spot (x, y), then what is there (stage 0 writes it, 4 uint4 each: the particle, the air, its
// id and Newtonian gravity's field there, for the HUD's sample at the cursor), then the drawing
// operations the host made for this frame (src/sim/Signs.cpp: box, outline, glyphs, underlines, pointer).
// Stage 1 runs once per sign, over its area, in the signs' order; each pixel applies that sign's operations in turn.
// Push constants: colourIdx = stage; colourOX/colourOY = (stage 0) the number of signs, (stage 1) the first
// operation and how many; brushX/brushY and screenW/screenH = the area.
#include "render_common.slang"

[[vk::binding(31, 0)]] RWStructuredBuffer<uint4> signData;
static const uint SIGN_MAX = 256, SIGN_SAMPLES = SIGN_MAX, SIGN_OPS = 5 * SIGN_MAX;
static const uint SIGN_FILL = 1, SIGN_OUTLINE = 2, SIGN_GLYPH = 3, SIGN_UNDERLINE = 4, SIGN_PIXEL = 5;

int2 unpackPos(uint v) { return int2(int(v << 16) >> 16, int(v) >> 16); }

[shader("compute")]
[numthreads(16, 16, 1)]
void main(uint3 tid: SV_DispatchThreadID)
{
    if (pc.colourIdx == 0)
    {
        // sign::getDisplayText's sample: the particle at the spot (photons first), the pressure and ambient heat
        uint k = tid.y * 16 + tid.x; // (one group: 256 signs)
        if (k >= pc.colourOX)
            return;
        int x = int(signData[k].x), y = int(signData[k].y);
        uint4 a = uint4(0, 0, 0, 0), b = uint4(0, 0, 0, 0), c = uint4(0, 0, 0, 0), d = uint4(0, 0, 0, 0);
        if (inVideo(x, y))
        {
            uint cell = cellIndex(x, y);
            int id = pmapId(photons[cell]);
            if (id < 0)
                id = pmapId(pmap[cell]);
            if (id >= 0)
            {
                Particle p = parts[id];
                a = uint4(1, ptype[id], uint(p.ctype), uint(p.life));
                b = uint4(uint(p.tmp), uint(p.tmp2), uint(p.tmp4), asuint(p.temp));
            }
            uint ci = uint(y / CELL) * pc.CW + uint(x / CELL);
            c = uint4(asuint(airPV(ci)), asuint(airGet(AIR_HV, ci) - 273.15), uint(id + 1), 0);
            if (params[0].newtonian != 0)
            {
                float2 g = gravForce(x, y);
                d = uint4(asuint(g.x), asuint(g.y), 0, 0);
            }
        }
        signData[SIGN_SAMPLES + 4 * k] = a;
        signData[SIGN_SAMPLES + 4 * k + 1] = b;
        signData[SIGN_SAMPLES + 4 * k + 2] = c;
        signData[SIGN_SAMPLES + 4 * k + 3] = d;
        return;
    }

    if (tid.x >= pc.screenW || tid.y >= pc.screenH)
        return;
    int x = pc.brushX + int(tid.x), y = pc.brushY + int(tid.y);
    if (!inVideo(x, y))
        return;
    uint c = uint(y) * pc.W + uint(x);
    int3 px = unpackRGB(video[c]);
    bool changed = false;
    for (uint o = 0; o < pc.colourOY; o++)
    {
        uint4 op = signData[SIGN_OPS + pc.colourOX + o];
        int2 at = unpackPos(op.y);
        if (op.x == SIGN_FILL || op.x == SIGN_OUTLINE)
        {
            int2 size = unpackPos(op.z);
            bool inside = x >= at.x && y >= at.y && x < at.x + size.x && y < at.y + size.y;
            bool edge = x == at.x || y == at.y || x == at.x + size.x - 1 || y == at.y + size.y - 1;
            if (inside && (op.x == SIGN_FILL || edge))
            {
                px = unpackRGB(op.w);
                changed = true;
            }
        }
        else if (op.x == SIGN_GLYPH)
        {
            int i = x - at.x, j = y - at.y;
            if (i >= 0 && j >= 0 && j < FONT_H && i < glyphWidth(op.z))
            {
                int4 col = unpackRGBA(op.w);
                int a = glyphPixel(op.z, i, j) * col.w / 3; // BlendChar
                if (a != 0)
                {
                    px = blendRGB(px, int4(col.xyz, a));
                    changed = true;
                }
            }
        }
        else if (op.x == SIGN_UNDERLINE)
        {
            if (y == at.y && x >= at.x && x < at.x + int(op.z))
            {
                px = blendRGB(px, unpackRGBA(op.w));
                changed = true;
            }
        }
        else if (op.x == SIGN_PIXEL && x == at.x && y == at.y)
        {
            px = unpackRGB(op.w);
            changed = true;
        }
    }
    if (changed)
        video[c] = packRGB(px);
}
