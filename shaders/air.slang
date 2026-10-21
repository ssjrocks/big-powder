// TPT's air solver (src/simulation/Air.cpp, Air::update_air), one stage per dispatch. TPT runs these as
// full-grid loops one after another; each loop only reads values that loop doesn't write, so each one runs
// in parallel here with the same result. pc.mode selects the stage:
//   0: damp pressure/velocity at the edges, clear velocity next to air-blocking cells
//   1: pressure from velocity
//   2: velocity from pressure
//   3: diffusion, advection and vorticity confinement into the o* planes (copied back by the host)
#include "common.slang"

static const float AIR_TSTEPP = 0.3;
static const float AIR_TSTEPV = 0.4;
static const float AIR_VADV = 0.3;
static const float AIR_VLOSS = 0.999;
static const float AIR_PLOSS = 0.9999;
static const float MAX_PRESSURE = 256.0;
static const float MIN_PRESSURE = -256.0;
static const float ADV_DISTANCE_MULT = 0.7; // advDistanceMult: used for velocity from far away

static const uint AIR_ON = 0, AIR_PRESSUREOFF = 1, AIR_VELOCITYOFF = 2, AIR_OFF = 3;

float mixf(float a, float b, float f) { return a + (b - a) * f; }

uint ai(int x, int y) { return uint(y) * pc.CW + uint(x); }
float vxAt(int x, int y) { return airGet(AIR_VX, ai(x, y)); }
float vyAt(int x, int y) { return airGet(AIR_VY, ai(x, y)); }
float pvAt(int x, int y) { return airGet(AIR_PV, ai(x, y)); }
bool blockAir(int x, int y) { return airGet(AIR_BLOCKAIR, ai(x, y)) != 0.0; }
bool inCellBounds(int x, int y) { return x >= 0 && y >= 0 && x < int(pc.CW) && y < int(pc.CH); }

// Air::make_kernel: a 3x3 Gaussian, exp(-2(i^2+j^2)), normalised.
float kernelAt(int i, int j)
{
    float s = 0.0;
    for (int jj = -1; jj < 2; jj++)
        for (int ii = -1; ii < 2; ii++)
            s += exp(-2.0 * float(ii * ii + jj * jj));
    return exp(-2.0 * float(i * i + j * j)) / s;
}

// vorticityBmap in Air.cpp.
float vorticityBmap(int y, int x)
{
    int XC = int(pc.CW), YC = int(pc.CH);
    if (x > 1 && x < XC - 2 && y > 1 && y < YC - 2)
    {
        float dvydx = (blockAir(x, y) || blockAir(x + 1, y) || blockAir(x - 1, y)) ? 0.0 : vyAt(x + 1, y) - vyAt(x - 1, y);
        float dvxdy = (blockAir(x, y) || blockAir(x, y + 1) || blockAir(x, y - 1)) ? 0.0 : vxAt(x, y + 1) - vxAt(x, y - 1);
        return (dvydx - dvxdy) * 0.5;
    }
    return 0.0;
}

void stageEdgesAndWalls(int x, int y)
{
    int XC = int(pc.CW), YC = int(pc.CH);
    uint c = ai(x, y);
    // "reduces pressure/velocity on the edges every frame": TPT mixes the two outer columns, then the two
    // outer rows, so corner cells are mixed twice.
    int times = ((x <= 1 || x >= XC - 2) ? 1 : 0) + ((y <= 1 || y >= YC - 2) ? 1 : 0);
    float pv = airGet(AIR_PV, c), vx = airGet(AIR_VX, c), vy = airGet(AIR_VY, c);
    for (int k = 0; k < times; k++)
    {
        pv = mixf(pc.edgePressure, pv, 0.8);
        vx = mixf(pc.edgeVX, vx, 0.9);
        vy = mixf(pc.edgeVY, vy, 0.9);
    }
    // "clear some velocities near walls": for each air-blocking cell in [1, XC-2] x [1, YC-2], TPT zeroes vx
    // in it and its left/right neighbours, and vy in it and the cells above/below.
    bool clearVX = false, clearVY = false;
    for (int d = -1; d <= 1; d++)
    {
        int bx = x + d;
        if (bx >= 1 && bx <= XC - 2 && y >= 1 && y <= YC - 2 && blockAir(bx, y))
            clearVX = true;
        int by = y + d;
        if (by >= 1 && by <= YC - 2 && x >= 1 && x <= XC - 2 && blockAir(x, by))
            clearVY = true;
    }
    if (clearVX)
        vx = 0.0;
    if (clearVY)
        vy = 0.0;
    airSet(AIR_PV, c, pv);
    airSet(AIR_VX, c, vx);
    airSet(AIR_VY, c, vy);
}

void stagePressure(int x, int y)
{
    int XC = int(pc.CW), YC = int(pc.CH);
    if (x < 1 || y < 1 || x >= XC - 1 || y >= YC - 1)
        return;
    float dp = 0.0;
    dp += vxAt(x - 1, y) - vxAt(x + 1, y);
    dp += vyAt(x, y - 1) - vyAt(x, y + 1);
    uint c = ai(x, y);
    float pv = mixf(pc.edgePressure, airGet(AIR_PV, c), AIR_PLOSS);
    pv += dp * AIR_TSTEPP * 0.5;
    airSet(AIR_PV, c, pv);
}

void stageVelocity(int x, int y)
{
    int XC = int(pc.CW), YC = int(pc.CH);
    if (x < 1 || y < 1 || x >= XC - 1 || y >= YC - 1)
        return;
    float dx = pvAt(x - 1, y) - pvAt(x + 1, y);
    float dy = pvAt(x, y - 1) - pvAt(x, y + 1);
    uint c = ai(x, y);
    float vx = mixf(pc.edgeVX, airGet(AIR_VX, c), AIR_VLOSS);
    float vy = mixf(pc.edgeVY, airGet(AIR_VY, c), AIR_VLOSS);
    vx += dx * AIR_TSTEPV * 0.5;
    vy += dy * AIR_TSTEPV * 0.5;
    if (blockAir(x - 1, y) || blockAir(x, y) || blockAir(x + 1, y))
        vx = 0.0;
    if (blockAir(x, y - 1) || blockAir(x, y) || blockAir(x, y + 1))
        vy = 0.0;
    airSet(AIR_VX, c, vx);
    airSet(AIR_VY, c, vy);
}

void stageAdvect(int x, int y)
{
    int XC = int(pc.CW), YC = int(pc.CH);
    float dx = 0.0, dy = 0.0, dp = 0.0;
    for (int j = -1; j < 2; j++)
    {
        for (int i = -1; i < 2; i++)
        {
            float f = kernelAt(i, j);
            if (y + j > 0 && y + j < YC - 1 && x + i > 0 && x + i < XC - 1 && !blockAir(x + i, y + j))
            {
                dx += vxAt(x + i, y + j) * f;
                dy += vyAt(x + i, y + j) * f;
                dp += pvAt(x + i, y + j) * f;
            }
            else
            {
                dx += vxAt(x, y) * f;
                dy += vyAt(x, y) * f;
                dp += pvAt(x, y) * f;
            }
        }
    }

    float tx = float(x) - dx * ADV_DISTANCE_MULT;
    float ty = float(y) - dy * ADV_DISTANCE_MULT;
    if ((abs(dx * ADV_DISTANCE_MULT) > 1.0 || abs(dy * ADV_DISTANCE_MULT) > 1.0) &&
        (tx >= 2.0 && tx < float(XC - 2) && ty >= 2.0 && ty < float(YC - 2)))
    {
        // Taking velocity from far away: step towards the source looking for an intervening wall.
        float stepX, stepY;
        int stepLimit;
        if (abs(dx) > abs(dy))
        {
            stepX = (dx < 0.0) ? 1.0 : -1.0;
            stepY = -dy / abs(dx);
            stepLimit = int(abs(dx * ADV_DISTANCE_MULT));
        }
        else
        {
            stepY = (dy < 0.0) ? 1.0 : -1.0;
            stepX = -dx / abs(dy);
            stepLimit = int(abs(dy * ADV_DISTANCE_MULT));
        }
        tx = float(x);
        ty = float(y);
        int step = 0;
        for (; step < stepLimit; ++step)
        {
            tx += stepX;
            ty += stepY;
            int cx = int(tx + 0.5), cy = int(ty + 0.5);
            if (!inCellBounds(cx, cy) || blockAir(cx, cy))
            {
                tx -= stepX;
                ty -= stepY;
                break;
            }
        }
        if (step == stepLimit)
        {
            // No wall found.
            tx = float(x) - dx * ADV_DISTANCE_MULT;
            ty = float(y) - dy * ADV_DISTANCE_MULT;
        }
    }
    int i0 = int(tx);
    int j0 = int(ty);
    tx -= float(i0);
    ty -= float(j0);
    if (!blockAir(x, y) && i0 >= 2 && i0 < XC - 3 && j0 >= 2 && j0 < YC - 3 && tx >= 0.0 && ty >= 0.0)
    {
        dx *= 1.0 - AIR_VADV;
        dy *= 1.0 - AIR_VADV;
        dx += AIR_VADV * (1.0 - tx) * (1.0 - ty) * vxAt(i0, j0);
        dy += AIR_VADV * (1.0 - tx) * (1.0 - ty) * vyAt(i0, j0);
        dx += AIR_VADV * tx * (1.0 - ty) * vxAt(i0 + 1, j0);
        dy += AIR_VADV * tx * (1.0 - ty) * vyAt(i0 + 1, j0);
        dx += AIR_VADV * (1.0 - tx) * ty * vxAt(i0, j0 + 1);
        dy += AIR_VADV * (1.0 - tx) * ty * vyAt(i0, j0 + 1);
        dx += AIR_VADV * tx * ty * vxAt(i0 + 1, j0 + 1);
        dy += AIR_VADV * tx * ty * vyAt(i0 + 1, j0 + 1);
    }

    // Vorticity confinement.
    if (pc.vorticity > 0.0 && x > 1 && x < XC - 2 && y > 1 && y < YC - 2)
    {
        float dwx = (abs(vorticityBmap(y, x + 1)) - abs(vorticityBmap(y, x - 1))) * 0.5;
        float dwy = (abs(vorticityBmap(y + 1, x)) - abs(vorticityBmap(y - 1, x))) * 0.5;
        float norm = sqrt(dwx * dwx + dwy * dwy);
        float w = vorticityBmap(y, x);
        dx += pc.vorticity / 5.0 * dwy / (norm + 0.001) * w;
        dy += pc.vorticity / 5.0 * (-dwx) / (norm + 0.001) * w;
    }

    // Fan walls (WL_FAN) add fvx/fvy here; walls are not ported yet.

    dp = clamp(dp, MIN_PRESSURE, MAX_PRESSURE);
    dx = clamp(dx, MIN_PRESSURE, MAX_PRESSURE);
    dy = clamp(dy, MIN_PRESSURE, MAX_PRESSURE);

    switch (pc.airMode)
    {
    case AIR_PRESSUREOFF:
        dp = 0.0;
        break;
    case AIR_VELOCITYOFF:
        dx = 0.0;
        dy = 0.0;
        break;
    case AIR_OFF:
        dx = 0.0;
        dy = 0.0;
        dp = 0.0;
        break;
    default:
        break;
    }

    uint c = ai(x, y);
    airSet(AIR_OVX, c, dx);
    airSet(AIR_OVY, c, dy);
    airSet(AIR_OPV, c, dp);
}

[shader("compute")]
[numthreads(16, 16, 1)]
void main(uint3 tid: SV_DispatchThreadID)
{
    if (tid.x >= pc.CW || tid.y >= pc.CH)
        return;
    int x = int(tid.x), y = int(tid.y);
    switch (pc.mode)
    {
    case 0:
        stageEdgesAndWalls(x, y);
        break;
    case 1:
        stagePressure(x, y);
        break;
    case 2:
        stageVelocity(x, y);
        break;
    default:
        stageAdvect(x, y);
        break;
    }
}
