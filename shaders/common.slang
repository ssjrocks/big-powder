// Shared definitions for all kernels. Keep in sync with src/sim/GpuTypes.h.
#pragma once
#include "element_ids.slang"

static const int CELL = 4;
static const int ISTP = CELL / 2;
static const float CFDS = 4.0 / float(CELL);
static const float MAX_VELOCITY = 1e4;
static const float R_TEMP = 22.0;
static const float MIN_TEMP = 0.0;
static const float MAX_TEMP = 9999.0;
static const float MIN_PRESSURE = -256.0;
static const float MAX_PRESSURE = 256.0;
static const int MAX_ELEMENTS = 256;

// Transition sentinels (TPT uses NT = -1 and ST = PT_NUM).
static const int NT = -1;
static const int ST = -2;


// Whether the GPU engine runs an element (see src/sim/Elements.cpp). Reactions that would create an element the
// GPU engine doesn't have yet are skipped, and switch on by themselves once it is ported.
bool gpuHas(uint t) { return t < uint(MAX_ELEMENTS) && elems[t].enabled != 0; }

// Element properties.
static const uint TYPE_PART = 0x1;
static const uint TYPE_LIQUID = 0x2;
static const uint TYPE_SOLID = 0x4;
static const uint TYPE_GAS = 0x8;
static const uint TYPE_ENERGY = 0x10;
static const uint PROP_CONDUCTS = 0x20;
static const uint PROP_LIFE_DEC = 0x4000;
static const uint PROP_LIFE_KILL = 0x8000;
static const uint PROP_LIFE_KILL_DEC = 0x10000;
static const uint PROP_SPARKSETTLE = 0x20000;
static const uint PROP_NOCTYPEDRAW = 0x100000;
static const uint PROP_NOAMBHEAT = 0x40000;

static const uint FLAG_STAGNANT = 0x1;
static const uint FLAG_MOVABLE = 0x8;

// Counter and stats layout (see GpuTypes.h).
static const uint CTR_COLOUR_COUNT = 0;
static const uint CTR_DISPATCH = 256;
static const uint CTR_FREE_COUNT = 1024;
static const uint CTR_FREE_TAKEN = 1025;
static const uint CTR_LIST_OVERFLOW = 1026;
static const uint CTR_STACKED = 1028;
static const uint CTR_EMAP_REQ = 1040; // queued set_emap requests
static const uint CTR_INST_REQ = 1041; // queued FloodINST requests, after the set_emap ones in emapReqs
static const uint CTR_BANG_REQ = 1042; // queued BANG flood_prop requests (third quarter of emapReqs)
static const uint CTR_BOMB_REQ = 1043; // queued BOMB explosions, particle ids (last quarter)
// Debug counters for moves between cells, only kept when the update pass has (pc.mode & 1).
static const uint CTR_DBG_MOVED = 1029;     // successful moves to another cell
static const uint CTR_DBG_LOCKED = 1030;    // target cell was locked by another mover
static const uint CTR_DBG_OWN_LOST = 1031;  // own cell had changed (displaced) when locking it
static const uint CTR_DBG_CAS_LOST = 1032;  // target changed between reading and claiming it
static const uint CTR_DBG_SAME_PASS = 1033; // swap refused: target is in the current pass

void debugCount(uint counter)
{
    if ((pc.mode & 1) != 0)
        InterlockedAdd(counters[counter], 1u);
}
static const uint STAT_PART_NOT_IN_PMAP = 256;
static const uint STAT_PMAP_DANGLING = 257;
static const uint STAT_PMAP_WRONG_POS = 258;
static const uint STAT_PMAP_LOCKED = 259;

// pmap entries: 0 = empty, otherwise (particle id + 1), top bit = locked by a mover.
static const uint PMAP_LOCK = 0x80000000u;
static const uint PMAP_ID_MASK = 0x7FFFFFFFu;

struct Particle
{
    int life;
    int ctype;
    float x, y;
    float vx, vy;
    float temp;
    int tmp, tmp2, tmp3, tmp4;
    uint flags;
    uint dcolour;
    uint pass; // ours: colour pass this tick, set by prepare
    uint pad1, pad2;
};

struct ElementProps
{
    float advection, airDrag, airLoss, loss, collision, gravity, diffusion, hotAir;
    int falldown, flammable, explosive, meltable, hardness, weight, heatConduct;
    uint properties;
    float lowPressure, highPressure, lowTemperature, highTemperature;
    int lowPressureT, highPressureT, lowTemperatureT, highTemperatureT;
    uint colour;
    float defaultTemp;
    float heatCapacity;
    int enabled;
    float newtonianGravity;
    uint photonReflectWavelengths;
    uint carriesTypeIn;
    int existsInTpt; // enabled in TPT, whether or not the GPU engine runs it
};

struct PushConsts
{
    uint W, H, CW, CH;
    uint tick, seed;
    uint colourNX, colourIdx; // colour grid width (see colourNY) and the pass being run
    uint colourOX, colourOY;
    uint listCap, capacity;
    float camX, camY, zoom;
    uint screenW, screenH;
    int brushX, brushY, brushR, brushType;
    uint mode;        // render: display mode; air: which stage; update: debug flags
    uint colourNY;
};

// Simulation settings (TPT's Simulation and Air options), one struct in a buffer.
struct SimParams
{
    uint airMode;      // 0 on, 1 pressure off, 2 velocity off, 3 off, 4 no update
    float vorticity;   // Air::vorticityCoeff
    float edgePressure, edgeVX, edgeVY;
    float ambientTemp; // Air::ambientAirTemp
    uint edgeMode;     // 0 void, 1 solid, 2 loop
    uint gravityMode;  // 0 vertical, 1 off, 2 radial, 3 custom
    float customGravX, customGravY;
    uint ambientHeat;  // aheat_enable
    uint legacyHeat;   // legacy_enable (heat simulation off)
    uint waterEqualisation;
    uint convectionMode; // 0 legacy, 1 Boussinesq
    uint pad0, pad1;
};

[[vk::push_constant]] ConstantBuffer<PushConsts> pc;

[[vk::binding(0, 0)]] RWStructuredBuffer<Particle> parts;
[[vk::binding(1, 0)]] RWStructuredBuffer<uint> ptype;
[[vk::binding(2, 0)]] RWStructuredBuffer<uint> pmap;
[[vk::binding(3, 0)]] StructuredBuffer<ElementProps> elems;
[[vk::binding(4, 0)]] StructuredBuffer<uint> canMove;
[[vk::binding(5, 0)]] RWStructuredBuffer<uint> lists;
[[vk::binding(6, 0)]] RWStructuredBuffer<uint> counters;
// Air maps, CW*CH floats per plane (TPT's sim.vx, vy, pv, hv and Air's work arrays). See AIR_* below.
// Stored as float bits so that air can be updated atomically (airAddPV).
[[vk::binding(7, 0)]] RWStructuredBuffer<uint> air;
[[vk::binding(8, 0)]] RWStructuredBuffer<uint> frame;
[[vk::binding(9, 0)]] RWStructuredBuffer<uint> freelist;
[[vk::binding(10, 0)]] RWStructuredBuffer<uint> stats;
// Particles to insert (insert.slang); the element type is in `pad1`.
[[vk::binding(11, 0)]] RWStructuredBuffer<Particle> inbox;
// Walls, one word per CELL x CELL block: bits 0-7 wall type (TPT's bmap), 8-15 electricity countdown (emap),
// 16-23 ambient-heat blocking count (Air::bmap_blockairh), bit 24 blocks air (bmap_blockair), bit 31 an
// emap flood fill is already requested this tick.
[[vk::binding(12, 0)]] RWStructuredBuffer<uint> walls;
[[vk::binding(13, 0)]] StructuredBuffer<SimParams> params;
// Cells to electrify at the end of the tick (TPT's set_emap), see emap_fill.slang. Count in counters.
[[vk::binding(14, 0)]] RWStructuredBuffer<uint> emapReqs;
// TPT's DefaultProperties per element (x, y, vx, vy unused).
[[vk::binding(15, 0)]] StructuredBuffer<Particle> elemDefaults;

// ---- walls (TPT's WL_* ids) ----
static const uint WL_ERASE = 0, WL_WALLELEC = 1, WL_EWALL = 2, WL_DETECT = 3, WL_STREAM = 4, WL_FAN = 5;
static const uint WL_ALLOWLIQUID = 6, WL_DESTROYALL = 7, WL_WALL = 8, WL_ALLOWAIR = 9, WL_ALLOWPOWDER = 10;
static const uint WL_ALLOWALLELEC = 11, WL_EHOLE = 12, WL_ALLOWGAS = 13, WL_GRAV = 14, WL_ALLOWENERGY = 15;
static const uint WL_BLOCKAIR = 16, WL_ERASEALL = 17, WL_STASIS = 18;
static const uint WALL_BLOCKAIR_BIT = 1u << 24;
static const uint WALL_EMAP_REQUESTED = 1u << 31;

uint cellWord(int cx, int cy) { return walls[uint(cy) * pc.CW + uint(cx)]; }
uint bmapCell(int cx, int cy) { return cellWord(cx, cy) & 0xFF; }
uint emapCell(int cx, int cy) { return (cellWord(cx, cy) >> 8) & 0xFF; }
// Pixel coordinates (TPT's bmap[y/CELL][x/CELL]).
uint bmapAt(int x, int y) { return bmapCell(x / CELL, y / CELL); }
uint emapAt(int x, int y) { return emapCell(x / CELL, y / CELL); }

bool isWire(int cx, int cy)
{
    uint b = bmapCell(cx, cy);
    return b == WL_DETECT || b == WL_EWALL || b == WL_ALLOWLIQUID || b == WL_WALLELEC || b == WL_ALLOWALLELEC ||
           b == WL_EHOLE || b == WL_STASIS;
}
bool isWireOff(int cx, int cy) { return isWire(cx, cy) && emapCell(cx, cy) < 8; }

// TPT's set_emap is a flood fill; here it is queued and run after the update passes (emap_fill.slang).
void requestEmap(int cx, int cy)
{
    if (cx < 0 || cy < 0 || cx >= int(pc.CW) || cy >= int(pc.CH) || !isWireOff(cx, cy))
        return;
    uint ci = uint(cy) * pc.CW + uint(cx);
    uint orig;
    InterlockedOr(walls[ci], WALL_EMAP_REQUESTED, orig);
    if ((orig & WALL_EMAP_REQUESTED) != 0)
        return;
    uint slot;
    InterlockedAdd(counters[CTR_EMAP_REQ], 1u, slot);
    if (slot < pc.CW * pc.CH)
        emapReqs[slot] = ci;
}

// Simulation::GetGravityField for the gravity modes (Newtonian gravity is not ported yet).
void gravityField(int x, int y, float particleGrav, float newtonGrav, out float gx, out float gy)
{
    SimParams sp = params[0];
    gx = 0.0;
    gy = 0.0;
    switch (sp.gravityMode)
    {
    case 1: // off
        break;
    case 2: // radial
    {
        float dx = float(x - int(pc.W / 2));
        float dy = float(y - int(pc.H / 2));
        if (dx != 0.0 || dy != 0.0)
        {
            float d = 0.01 - sqrt(dx * dx + dy * dy);
            gx = particleGrav * (dx / d);
            gy = particleGrav * (dy / d);
        }
        break;
    }
    case 3: // custom
        gx = particleGrav * sp.customGravX;
        gy = particleGrav * sp.customGravY;
        break;
    default: // vertical
        gy = particleGrav;
        break;
    }
}

// Air::bmap_blockairh: count particles that don't conduct heat, up to 8 (0x8 then blocks ambient heat).
void bumpBlockAirH(int x, int y)
{
    uint ci = uint(y / CELL) * pc.CW + uint(x / CELL);
    uint cur = walls[ci];
    for (int attempt = 0; attempt < 4; attempt++)
    {
        uint h = (cur >> 16) & 0xFF;
        if ((h & 0x8) != 0)
            return;
        uint orig;
        InterlockedCompareExchange(walls[ci], cur, cur + (1u << 16), orig);
        if (orig == cur)
            return;
        cur = orig;
    }
}


// ---- random numbers (one stream per thread, like TPT's sim->rng) ----

static uint g_rng;

uint hash32(uint x)
{
    x ^= x >> 16; x *= 0x7feb352du;
    x ^= x >> 15; x *= 0x846ca68bu;
    x ^= x >> 16;
    return x;
}

void rngSeed(uint stream)
{
    g_rng = hash32(stream ^ hash32(pc.tick * 0x9E3779B9u ^ pc.seed));
}

uint rngNext()
{
    g_rng = g_rng * 747796405u + 2891336453u;
    uint w = ((g_rng >> ((g_rng >> 28u) + 4u)) ^ g_rng) * 277803737u;
    return (w >> 22u) ^ w;
}

// Inclusive range, as TPT's RNG::between.
int rngBetween(int lo, int hi)
{
    return lo + int(rngNext() % uint(hi - lo + 1));
}

// RNG::gen: 31 random bits.
uint rngGen()
{
    return rngNext() & 0x7FFFFFFFu;
}

bool rngChance(int numerator, int denominator)
{
    if (numerator <= 0)
        return false;
    return int(rngNext() % uint(denominator)) < numerator;
}

float rngUniform01()
{
    return float(rngNext() >> 8) * (1.0 / 16777216.0);
}

// ---- grid helpers ----

uint cellIndex(int x, int y) { return uint(y) * pc.W + uint(x); }
uint airIndex(int x, int y) { return uint(y / CELL) * pc.CW + uint(x / CELL); }

static const uint AIR_VX = 0, AIR_VY = 1, AIR_PV = 2, AIR_OVX = 3, AIR_OVY = 4, AIR_OPV = 5;
static const uint AIR_HV = 6, AIR_OHV = 7, AIR_BLOCKAIR = 8, AIR_BLOCKAIRH = 9, AIR_FVX = 10, AIR_FVY = 11;
static const uint AIR_PLANES = 12;

float airGet(uint plane, uint ci) { return asfloat(air[plane * pc.CW * pc.CH + ci]); }
void airSet(uint plane, uint ci, float v) { air[plane * pc.CW * pc.CH + ci] = asuint(v); }

// pv += delta for air cell (cx, cy), atomically.
void airAddPV(int cx, int cy, float delta)
{
    if (cx < 0 || cy < 0 || cx >= int(pc.CW) || cy >= int(pc.CH))
        return;
    uint k = AIR_PV * pc.CW * pc.CH + uint(cy) * pc.CW + uint(cx);
    uint cur = air[k];
    for (int attempt = 0; attempt < 16; attempt++)
    {
        uint orig;
        InterlockedCompareExchange(air[k], cur, asuint(asfloat(cur) + delta), orig);
        if (orig == cur)
            return;
        cur = orig;
    }
}

// pv += rate * (target - pv) for air cell (cx, cy), atomically: used where particles in the same pass can
// reach the same cell. With onlyBelow, only while pv < target.
void airNudgePV(int cx, int cy, float target, float rate, bool onlyBelow)
{
    if (cx < 0 || cy < 0 || cx >= int(pc.CW) || cy >= int(pc.CH))
        return;
    uint k = AIR_PV * pc.CW * pc.CH + uint(cy) * pc.CW + uint(cx);
    uint cur = air[k];
    for (int attempt = 0; attempt < 16; attempt++)
    {
        float v = asfloat(cur);
        if (onlyBelow && !(v < target))
            return;
        uint orig;
        InterlockedCompareExchange(air[k], cur, asuint(v + rate * (target - v)), orig);
        if (orig == cur)
            return;
        cur = orig;
    }
}
float airVX(uint ci) { return airGet(AIR_VX, ci); }
float airVY(uint ci) { return airGet(AIR_VY, ci); }
float airPV(uint ci) { return airGet(AIR_PV, ci); }
void setAirVX(uint ci, float v) { airSet(AIR_VX, ci, v); }
void setAirVY(uint ci, float v) { airSet(AIR_VY, ci, v); }
void setAirPV(uint ci, float v) { airSet(AIR_PV, ci, v); }

bool inSim(int x, int y) { return x >= 0 && y >= 0 && x < int(pc.W) && y < int(pc.H); }

uint pmapEnc(uint id) { return id + 1; }

// Particle id in a pmap entry, or -1 for empty.
int pmapId(uint e)
{
    e &= PMAP_ID_MASK;
    return e == 0 ? -1 : int(e - 1);
}

// Type at a pmap entry (0 for empty or a stale entry).
uint pmapType(uint e)
{
    int id = pmapId(e);
    return id < 0 ? PT_NONE : ptype[id];
}

uint typeAt(int x, int y)
{
    if (!inSim(x, y))
        return PT_NONE;
    return pmapType(pmap[cellIndex(x, y)]);
}

float restrictFlt(float f, float lo, float hi) { return clamp(f, lo, hi); }
// std::round: halfway cases away from zero (GPU round() may round them to even).
float roundAway(float v) { return v >= 0.0 ? floor(v + 0.5) : ceil(v - 0.5); }

// TPT's FloodINST (spark a whole INST network) is queued too and run after the update passes (inst_fill.slang).
// Its SPRKs start at life 4 and can't conduct until the next tick, so this gives TPT's result.
void requestInstFlood(int x, int y)
{
    if (!inSim(x, y))
        return;
    uint slot;
    InterlockedAdd(counters[CTR_INST_REQ], 1u, slot);
    if (slot < pc.CW * pc.CH)
        emapReqs[pc.CW * pc.CH + slot] = uint(x) | (uint(y) << 16);
}

// BANG's flood_prop(x, y, tmp = 2) over its connected blob, deferred like FloodINST.
void requestBangFlood(int x, int y)
{
    uint slot;
    InterlockedAdd(counters[CTR_BANG_REQ], 1u, slot);
    if (slot < pc.CW * pc.CH)
        emapReqs[2 * pc.CW * pc.CH + slot] = uint(x) | (uint(y) << 16);
}

// A BOMB explosion reaches 9 px, further than the colour passes keep apart, so explosions run one at a time
// after the passes. A bomb destroyed by an earlier explosion by then doesn't explode, as in TPT.
void requestBombExplosion(uint id)
{
    uint slot;
    InterlockedAdd(counters[CTR_BOMB_REQ], 1u, slot);
    if (slot < pc.CW * pc.CH)
        emapReqs[3 * pc.CW * pc.CH + slot] = id;
}

// Ids freed by kill_part during the update passes in serial mode, reused last-in first-out as TPT's
// free list does (see allocPart in elements.slang). Count in CTR_FREE_COUNT.
void pushFreed(uint id)
{
    if ((pc.mode & 2) == 0)
        return;
    uint n = counters[CTR_FREE_COUNT];
    if (n < pc.capacity)
    {
        freelist[n] = id;
        counters[CTR_FREE_COUNT] = n + 1;
    }
}

// TPT's kill_part: remove from pmap if it is there, then free the id. x, y is where the caller saw
// the particle; if another thread has moved it since, its current position is tried too. An entry
// that is left behind (the particle was mid-move) points at a dead particle and is treated as empty
// until the freelist pass clears it.
void killPart(uint id, int x, int y)
{
    uint orig = 0;
    if (inSim(x, y))
        InterlockedCompareExchange(pmap[cellIndex(x, y)], pmapEnc(id), 0u, orig);
    if (orig != pmapEnc(id))
    {
        int cx = int(parts[id].x + 0.5);
        int cy = int(parts[id].y + 0.5);
        if ((cx != x || cy != y) && inSim(cx, cy))
            InterlockedCompareExchange(pmap[cellIndex(cx, cy)], pmapEnc(id), 0u, orig);
    }
    if (ptype[id] != PT_NONE)
        pushFreed(id);
    ptype[id] = PT_NONE;
}

void killPartById(uint id)
{
    Particle q = parts[id];
    killPart(id, int(q.x + 0.5), int(q.y + 0.5));
}

bool isHeatInsulator(uint t) { return elems[t].heatConduct == 0; }
