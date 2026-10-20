// Shared definitions for all kernels. Keep in sync with src/sim/GpuTypes.h.
#pragma once

static const int CELL = 4;
static const int ISTP = CELL / 2;
static const float CFDS = 4.0 / float(CELL);
static const float MAX_VELOCITY = 1e4;
static const float R_TEMP = 22.0;
static const float MIN_TEMP = 0.0;
static const float MAX_TEMP = 9999.0;
static const int MAX_ELEMENTS = 256;

// Transition sentinels (TPT uses NT = -1 and ST = PT_NUM).
static const int NT = -1;
static const int ST = -2;

// Element ids (TPT numbering).
static const uint PT_NONE = 0;
static const uint PT_DUST = 1;
static const uint PT_WATR = 2;
static const uint PT_OIL = 3;
static const uint PT_FIRE = 4;
static const uint PT_STNE = 5;
static const uint PT_LAVA = 6;
static const uint PT_GAS = 10;
static const uint PT_METL = 14;
static const uint PT_PLSM = 49;
static const uint PT_CFLM = 68;
static const uint PT_SPNG = 90;

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

static const uint FLAG_STAGNANT = 0x1;
static const uint FLAG_MOVABLE = 0x8;

// Counter and stats layout (see GpuTypes.h).
static const uint CTR_COLOUR_COUNT = 0;
static const uint CTR_DISPATCH = 32;
static const uint CTR_FREE_COUNT = 160;
static const uint CTR_FREE_TAKEN = 161;
static const uint CTR_LIST_OVERFLOW = 162;
static const uint CTR_STACKED = 164;
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
};

struct PushConsts
{
    uint W, H, CW, CH;
    uint tick, seed;
    uint colourN, colourIdx;
    uint colourOX, colourOY;
    uint listCap, capacity;
    float camX, camY, zoom;
    uint screenW, screenH;
    int brushX, brushY, brushR, brushType;
    uint mode;
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
[[vk::binding(7, 0)]] RWStructuredBuffer<float> air; // vx | vy | pv planes, CW*CH each
[[vk::binding(8, 0)]] RWStructuredBuffer<uint> frame;
[[vk::binding(9, 0)]] RWStructuredBuffer<uint> freelist;
[[vk::binding(10, 0)]] RWStructuredBuffer<uint> stats;

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

float airVX(uint ci) { return air[ci]; }
float airVY(uint ci) { return air[pc.CW * pc.CH + ci]; }
float airPV(uint ci) { return air[2 * pc.CW * pc.CH + ci]; }
void setAirVX(uint ci, float v) { air[ci] = v; }
void setAirVY(uint ci, float v) { air[pc.CW * pc.CH + ci] = v; }
void setAirPV(uint ci, float v) { air[2 * pc.CW * pc.CH + ci] = v; }

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
    ptype[id] = PT_NONE;
}

void killPartById(uint id)
{
    Particle q = parts[id];
    killPart(id, int(q.x + 0.5), int(q.y + 0.5));
}

bool isHeatInsulator(uint t) { return elems[t].heatConduct == 0; }
