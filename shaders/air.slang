// TPT's air solver (src/simulation/Air.cpp, Air::update_air and update_airh), one stage per dispatch. TPT runs
// these as full-grid loops one after another; each loop only reads values that loop doesn't write, so each
// one runs in parallel here with the same result. pc.mode selects the stage:
//   0: damp pressure/velocity at the edges, clear velocity next to air-blocking cells
//   1: pressure from velocity
//   2: velocity from pressure
//   3: diffusion, advection and vorticity confinement into the o* planes (copied back by the host)
//   4: ambient heat convection (update_airh) into OVX/OVY
//   5: ambient heat diffusion/advection (update_airh) into OHV. TPT writes the convection in place during its
//      raster-order loop, so cells it has already passed are read with this tick's convection (OVX/OVY).
//   6: copy OHV, OVX, OVY back
#include "common.slang"

static const float AIR_TSTEPP = 0.3;
static const float AIR_TSTEPV = 0.4;
static const float AIR_VADV = 0.3;
static const float AIR_VLOSS = 0.999;
static const float AIR_PLOSS = 0.9999;
static const float ADV_DISTANCE_MULT = 0.7; // advDistanceMult: used for velocity from far away

static const uint AIR_ON = 0, AIR_PRESSUREOFF = 1, AIR_VELOCITYOFF = 2, AIR_OFF = 3;

float mixf(float a, float b, float f) { return a + (b - a) * f; }

uint ai(int x, int y) { return uint(y) * pc.CW + uint(x); }
float vxAt(int x, int y) { return airGet(AIR_VX, ai(x, y)); }
float vyAt(int x, int y) { return airGet(AIR_VY, ai(x, y)); }
float pvAt(int x, int y) { return airGet(AIR_PV, ai(x, y)); }
bool blockAir(int x, int y) { return (walls[ai(x, y)] & WALL_BLOCKAIR_BIT) != 0; }
bool blockAirH(int x, int y) { return ((walls[ai(x, y)] >> 16) & 0x8) != 0; }
float hvAt(int x, int y) { return airGet(AIR_HV, ai(x, y)); }
// update_airh first sets the two outer rings of cells to the ambient temperature, then reads them.
float hvEff(int x, int y)
{
    if (x <= 1 || x >= int(pc.CW) - 2 || y <= 1 || y >= int(pc.CH) - 2)
        return params[0].ambientTemp;
    return hvAt(x, y);
}
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
        pv = mixf(params[0].edgePressure, pv, 0.8);
        vx = mixf(params[0].edgeVX, vx, 0.9);
        vy = mixf(params[0].edgeVY, vy, 0.9);
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
    float pv = mixf(params[0].edgePressure, airGet(AIR_PV, c), AIR_PLOSS);
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
    float vx = mixf(params[0].edgeVX, airGet(AIR_VX, c), AIR_VLOSS);
    float vy = mixf(params[0].edgeVY, airGet(AIR_VY, c), AIR_VLOSS);
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
    if (params[0].vorticity > 0.0 && x > 1 && x < XC - 2 && y > 1 && y < YC - 2)
    {
        float dwx = (abs(vorticityBmap(y, x + 1)) - abs(vorticityBmap(y, x - 1))) * 0.5;
        float dwy = (abs(vorticityBmap(y + 1, x)) - abs(vorticityBmap(y - 1, x))) * 0.5;
        float norm = sqrt(dwx * dwx + dwy * dwy);
        float w = vorticityBmap(y, x);
        dx += params[0].vorticity / 5.0 * dwy / (norm + 0.001) * w;
        dy += params[0].vorticity / 5.0 * (-dwx) / (norm + 0.001) * w;
    }

    if (bmapCell(x, y) == WL_FAN)
    {
        dx += airGet(AIR_FVX, ai(x, y));
        dy += airGet(AIR_FVY, ai(x, y));
    }

    dp = clamp(dp, MIN_PRESSURE, MAX_PRESSURE);
    dx = clamp(dx, MIN_PRESSURE, MAX_PRESSURE);
    dy = clamp(dy, MIN_PRESSURE, MAX_PRESSURE);

    switch (params[0].airMode)
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

// The convection velocity update_airh adds to cell (x, y) (it depends only on temperatures from before the
// loop), already capped as TPT stores it.
float2 convectedVelocity(int cx, int cy)
{
    int cXC = int(pc.CW), cYC = int(pc.CH);
    SimParams csp = params[0];
    float dvx = vxAt(cx, cy), dvy = vyAt(cx, cy);
    if (cx >= 2 && cx < cXC - 2 && cy >= 2 && cy < cYC - 2)
    {
        float cgx, cgy;
        gravityField(cx * CELL, cy * CELL, -1.0, -1.0, cgx, cgy);
        if (csp.convectionMode == 0)
        {
            // Air convection before TPT 99.0.
            float weight = ((hvEff(cx, cy) - hvEff(cx - 1, cy)) * cgx + (hvEff(cx, cy) - hvEff(cx, cy - 1)) * cgy) / 5000.0;
            if (weight > 0.0 && !blockAirH(cx, cy - 1))
            {
                dvx += weight * cgx;
                dvy += weight * cgy;
            }
        }
        else
        {
            // Boussinesq approximation.
            float gravMagn = sqrt(cgx * cgx + cgy * cgy);
            if (gravMagn > 10.0)
            {
                cgx /= 0.1 * gravMagn;
                cgy /= 0.1 * gravMagn;
            }
            float weight = min((hvEff(cx, cy) - csp.ambientTemp) / 10000.0, 0.01);
            dvx += weight * cgx;
            dvy += weight * cgy;
        }
    }
    return float2(clamp(dvx, MIN_PRESSURE, MAX_PRESSURE), clamp(dvy, MIN_PRESSURE, MAX_PRESSURE));
}

void stageAirH(int x, int y)
{
    int XC = int(pc.CW), YC = int(pc.CH);
    SimParams sp = params[0];
    uint c = ai(x, y);
    float dh = 0.0, dx = 0.0, dy = 0.0;
    for (int j = -1; j <= 1; j++)
    {
        for (int i = -1; i <= 1; i++)
        {
            float f = kernelAt(i, j);
            int xx = x + i, yy = y + j;
            if (yy > 0 && yy < YC - 1 && xx > 0 && xx < XC - 1 && !blockAirH(xx, yy))
            {
                // TPT's loop runs in raster order and writes convection into vx/vy as it goes, so cells it has
                // already passed are read with this tick's convection included.
                bool passed = yy < y || (yy == y && xx < x);
                uint nc = ai(xx, yy);
                float2 nv = passed ? float2(airGet(AIR_OVX, nc), airGet(AIR_OVY, nc)) : float2(vxAt(xx, yy), vyAt(xx, yy));
                dh += hvEff(xx, yy) * f;
                dx += nv.x * f;
                dy += nv.y * f;
            }
            else
            {
                dh += hvEff(x, y) * f;
                dx += vxAt(x, y) * f;
                dy += vyAt(x, y) * f;
            }
        }
    }
    float tx = float(x) - dx * ADV_DISTANCE_MULT;
    float ty = float(y) - dy * ADV_DISTANCE_MULT;
    if ((abs(dx * ADV_DISTANCE_MULT) > 1.0 || abs(dy * ADV_DISTANCE_MULT) > 1.0) &&
        (tx >= 2.0 && tx < float(XC - 2) && ty >= 2.0 && ty < float(YC - 2)))
    {
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
            if (!inCellBounds(cx, cy) || blockAirH(cx, cy))
            {
                tx -= stepX;
                ty -= stepY;
                break;
            }
        }
        if (step == stepLimit)
        {
            tx = float(x) - dx * ADV_DISTANCE_MULT;
            ty = float(y) - dy * ADV_DISTANCE_MULT;
        }
    }
    int i0 = int(tx), j0 = int(ty);
    tx -= float(i0);
    ty -= float(j0);
    if (!blockAirH(x, y) && i0 >= 0 && i0 < XC - 1 && j0 >= 0 && j0 < YC - 1 && tx >= 0.0 && ty >= 0.0)
    {
        float odh = dh;
        dh *= 1.0 - AIR_VADV;
        dh += AIR_VADV * (1.0 - tx) * (1.0 - ty) * (blockAirH(i0, j0) ? odh : hvEff(i0, j0));
        dh += AIR_VADV * tx * (1.0 - ty) * (blockAirH(i0 + 1, j0) ? odh : hvEff(i0 + 1, j0));
        dh += AIR_VADV * (1.0 - tx) * ty * (blockAirH(i0, j0 + 1) ? odh : hvEff(i0, j0 + 1));
        dh += AIR_VADV * tx * ty * (blockAirH(i0 + 1, j0 + 1) ? odh : hvEff(i0 + 1, j0 + 1));
    }
    if (blockAirH(x, y))
        dh = hvEff(x, y);
    dh = clamp(dh, MIN_TEMP, MAX_TEMP);
    airSet(AIR_OHV, c, dh);

}

// update_airh's convection for one cell, into OVX/OVY (stage 4, before the heat stage reads it).
void stageConvection(int x, int y)
{
    float2 v = convectedVelocity(x, y);
    airSet(AIR_OVX, ai(x, y), v.x);
    airSet(AIR_OVY, ai(x, y), v.y);
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
    case 3:
        stageAdvect(x, y);
        break;
    case 4:
        stageConvection(x, y);
        break;
    case 5:
        stageAirH(x, y);
        break;
    default:
        airSet(AIR_HV, ai(x, y), airGet(AIR_OHV, ai(x, y)));
        airSet(AIR_VX, ai(x, y), airGet(AIR_OVX, ai(x, y)));
        airSet(AIR_VY, ai(x, y), airGet(AIR_OVY, ai(x, y)));
        break;
    }
}
