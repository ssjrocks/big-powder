// TPT's Newtonian gravity (simulation/gravity/Fft.cpp): the field each cell feels is the sum over all cells q of
// -M_GRAV * mass(q) * (p - q) / |p - q|^3, computed by FFT convolution. TPT pads its grid to twice the world, which
// makes its cyclic convolution exactly this linear one; here the grid is padded to powers of two of at least
// 2 * cells - 1, which gives the same sums (up to float rounding). TPT computes the field on a thread while the
// next frame runs, so a tick uses the field from two ticks' mass before: stage 0 takes the field the last tick
// computed, the rest compute the next one from the mass gathered during the last tick (and clear it).
//
// gravFft holds complex arrays of NX x NY: the kernel's spectra (x and y, made once), then A (the mass), B and C
// (the field's x and y). The kernel already carries the inverse transform's 1/(NX*NY). Stage 10 draws the field
// display.
#include "common.slang"

[[vk::binding(29, 0)]] RWStructuredBuffer<float2> gravFft;

uint nextPow2(uint v) { return v <= 1 ? 1u : 1u << (firstbithigh(v - 1) + 1); }
uint fftNX() { return nextPow2(2 * pc.CW - 1); }
uint fftNY() { return nextPow2(2 * pc.CH - 1); }

static const uint FFT_THREADS = 256;
groupshared float2 fftShared[4096];

float2 cmul(float2 a, float2 b) { return float2(a.x * b.x - a.y * b.y, a.x * b.y + a.y * b.x); }

// In-place radix-2 FFT of L (a power of 2, at most 4096) values at base, base + stride, ...; dir -1 forward,
// +1 inverse (unscaled). One workgroup per line.
void fftLine(uint base, uint stride, uint L, float dir, uint gi)
{
    uint bits = firstbithigh(L);
    for (uint k = gi; k < L; k += FFT_THREADS)
        fftShared[reversebits(k) >> (32 - bits)] = gravFft[base + k * stride];
    GroupMemoryBarrierWithGroupSync();
    for (uint len = 2; len <= L; len <<= 1)
    {
        uint half = len >> 1;
        float ang = dir * 6.28318530718 / float(len);
        for (uint b = gi; b < L / 2; b += FFT_THREADS)
        {
            uint j = b % half;
            uint i0 = (b / half) * len + j, i1 = i0 + half;
            float s, c;
            sincos(ang * float(j), s, c);
            float2 u = fftShared[i0], v = cmul(fftShared[i1], float2(c, s));
            fftShared[i0] = u + v;
            fftShared[i1] = u - v;
        }
        GroupMemoryBarrierWithGroupSync();
    }
    for (uint k = gi; k < L; k += FFT_THREADS)
        gravFft[base + k * stride] = fftShared[k];
}

[shader("compute")]
[numthreads(256, 1, 1)]
void main(uint3 tid: SV_DispatchThreadID, uint3 gid: SV_GroupID, uint gi: SV_GroupIndex)
{
    uint NX = fftNX(), NY = fftNY(), N = NX * NY;
    uint cells = pc.CW * pc.CH;
    uint KX = 0, KY = N, A = 2 * N, B = 3 * N, C = 4 * N;
    uint g = gid.x;
    switch (pc.colourIdx)
    {
    case 0: // the field in use: the one computed last tick
    {
        uint k = tid.x;
        if (k < cells)
        {
            gravCells[GRAV_FX * cells + k] = gravCells[GRAV_NEXT_X * cells + k];
            gravCells[GRAV_FY * cells + k] = gravCells[GRAV_NEXT_Y * cells + k];
        }
        break;
    }
    case 1: // the masked mass, zero-padded; the mass is cleared for this tick
    {
        uint k = tid.x;
        if (k >= N)
            break;
        uint x = k % NX, y = k / NX;
        float m = 0.0;
        if (x < pc.CW && y < pc.CH)
        {
            uint c = y * pc.CW + x;
            if (gravCells[GRAV_MASK * cells + c] != 0)
                m = asfloat(gravCells[GRAV_MASS * cells + c]);
            gravCells[GRAV_MASS * cells + c] = 0;
        }
        gravFft[A + k] = float2(m, 0.0);
        break;
    }
    case 2: // forward along rows (only the rows that hold mass; the rest stay 0)
        if (g < pc.CH)
            fftLine(A + g * NX, 1, NX, -1.0, gi);
        break;
    case 3: // forward along columns
        if (g < NX)
            fftLine(A + g, NX, NY, -1.0, gi);
        break;
    case 4: // times the kernel's spectra
    {
        uint k = tid.x;
        if (k < N)
        {
            float2 a = gravFft[A + k];
            gravFft[B + k] = cmul(a, gravFft[KX + k]);
            gravFft[C + k] = cmul(a, gravFft[KY + k]);
        }
        break;
    }
    case 5: // back along columns, for both components
        if (g < 2 * NX)
            fftLine((g < NX ? B : C) + g % NX, NX, NY, 1.0, gi);
        break;
    case 6: // back along rows (only those of the world)
        if (g < 2 * pc.CH)
            fftLine((g < pc.CH ? B : C) + (g % pc.CH) * NX, 1, NX, 1.0, gi);
        break;
    case 7: // the next field, masked
    {
        uint k = tid.x;
        if (k < cells)
        {
            uint x = k % pc.CW, y = k / pc.CW;
            bool on = gravCells[GRAV_MASK * cells + k] != 0;
            gravCells[GRAV_NEXT_X * cells + k] = asuint(on ? gravFft[B + y * NX + x].x : 0.0);
            gravCells[GRAV_NEXT_Y * cells + k] = asuint(on ? gravFft[C + y * NX + x].x : 0.0);
        }
        break;
    }
    case 10: // Renderer::draw_grav: from each cell's corner, four points stepped back along its field
    {
        uint k = tid.x;
        if (k < cells)
        {
            float gx = asfloat(gravCells[GRAV_FX * cells + k]), gy = asfloat(gravCells[GRAV_FY * cells + k]);
            float agx = abs(gx), agy = abs(gy);
            uint alpha = uint(int((agx + agy) * 20.0)) & 0xFFu; // (RGB::WithAlpha takes a uint8_t)
            if ((agx > 0.001 || agy > 0.001) && alpha != 0)
            {
                float2 np = float2(float((k % pc.CW) * CELL), float((k / pc.CW) * CELL));
                for (int s = 0; s < 4; s++)
                {
                    np -= float2(gx * 0.5, gy * 0.5);
                    int px = int(np.x + 0.5), py = int(np.y + 0.5);
                    if (px >= 0 && py >= 0 && px < int(pc.W) && py < int(pc.H))
                        InterlockedAdd(gravDraw[uint(py) * pc.W + uint(px)], alpha);
                }
            }
        }
        break;
    }
    case 8: // the kernel's spectra (made once): rows, then (9) columns
        if (g < 2 * NY)
            fftLine((g < NY ? KX : KY) + (g % NY) * NX, 1, NX, -1.0, gi);
        break;
    case 9:
        if (g < 2 * NX)
            fftLine((g < NX ? KX : KY) + g % NX, NX, NY, -1.0, gi);
        break;
    }
}
