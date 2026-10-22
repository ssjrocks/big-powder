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
static const uint PROP_PHOTPASS = 0x40;
static const uint PROP_NEUTPENETRATE = 0x80;
static const uint PROP_NEUTABSORB = 0x100;
static const uint PROP_NEUTPASS = 0x200;
static const uint PROP_DEADLY = 0x400; // hurts stick men
static const uint PROP_RADIOACTIVE = 0x2000;
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
static const uint CTR_BOMB_REQ = 1043; // queued BOMB explosions and DMG blasts, particle ids
static const uint CTR_RAY_REQ = 1044;  // queued beams and lightning (three words each, after the explosions)
static const uint CTR_EMP = 1045;      // EMP triggers this tick
static const uint CTR_ETRD_SEARCHES = 1046; // ETRD arc searches this tick (the first one looks at every ETRD)
static const uint CTR_ETRD_COUNT = 1047; // ETRD particles listed by prepare, in emapReqs[6 cells, 7 cells)
static const uint CTR_RAY_ARGS = 1048; // indirect dispatch sizes for rays.slang and emp.slang
static const uint CTR_EMP_ARGS = 1052;
static const uint CTR_WIFI_CUR = 1056; // TPT's wireless[channel][0]: active this tick
static const uint CTR_WIFI_NEXT = 1160; // wireless[channel][1]: active next tick
static const uint WIFI_CHANNELS = 101; // (MAX_TEMP - 73.15) / 100 + 2
// Debug counters for moves between cells, only kept when the update pass has (pc.mode & 1).
static const uint CTR_DBG_MOVED = 1029;     // successful moves to another cell
static const uint CTR_DBG_LOCKED = 1030;    // target cell was locked by another mover
static const uint CTR_DBG_OWN_LOST = 1031;  // own cell had changed (displaced) when locking it
static const uint CTR_DBG_CAS_LOST = 1032;  // target changed between reading and claiming it
static const uint CTR_DBG_SAME_PASS = 1033; // swap refused: target is in the current pass
static const uint CTR_PSTN_REQ = 1036; // queued piston moves (deferred.slang)
static const uint CTR_PPIP_REQ = 1037; // queued PPIP floods (deferred.slang)
static const uint CTR_LIFE = 1038; // LIFE seen by prepare this tick (gol.slang)
static const uint MAX_COLOURS = 256;
static const uint CTR_PASS_RANK = 1280; // [1280, 1536): each colour's place in this tick's pass order
static const uint CTR_LOVE = 1039; // LOVE or LOLZ seen by prepare this tick (love.slang)
static const uint CTR_LOVE_ARGS = 1264; // indirect dispatch sizes (args.slang): love.slang, gol.slang per particle
static const uint CTR_GOLP_ARGS = 1268;
static const uint CTR_GOLC_ARGS = 1272; // and gol.slang per cell
// Stick men (stickmen.slang)
static const uint CTR_STKM_COUNT = 1536; // STKM, STKM2, FIGH, SPAWN and SPAWN2 listed by prepare this tick
static const uint CTR_STKM_FLAGS = 1537; // STKM (1), STKM2 (2), SPAWN (4), SPAWN2 (8) made since stickmen.slang ran
static const uint CTR_FIGH_NEW = 1538;   // FIGH made since stickmen.slang ran
static const uint CTR_FIGH_COUNT = 1539; // fighters (TPT's fighcount), counted by stickmen.slang
static const uint CTR_STKM_LIST = 1540;  // [1540, 1796): the ids prepare listed
static const uint STKM_LIST_CAP = 256;
static const uint CTR_STKM_LEFT = 1796;  // cells a stick man's head left this tick (stats_parts.slang), 1 + 128 words
static const uint STKM_LEFT_CAP = 128;
static const uint CTR_STACK_ARGS = 1928; // indirect dispatch sizes (stacking.slang): over particles, then over cells
static const uint CTR_STACK_CLEAR_ARGS = 1932;

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
    uint stkmComm;     // the players' keys: STKM's comm, pcomm, then STKM2's, a byte each
    uint pad1;
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
// TPT's photons map: where energy particles (TYPE_ENERGY) are, apart from pmap; they can share cells with
// each other and with other particles. Same encoding as pmap (no lock bit is used).
[[vk::binding(16, 0)]] RWStructuredBuffer<uint> photons;
// TPT's portalp[channel][direction][80]: particles inside the portals of each channel (pad2 holds the type, 0 when
// the slot is empty; see portalStore).
[[vk::binding(17, 0)]] RWStructuredBuffer<Particle> portals;

// TPT's playerst: STKM's (slot 0), STKM2's (1) and each fighter's (2 + FIGH's tmp). Legs are 4 points (knee and
// foot of each leg, each with its previous position): legs[0..3] left knee, [4..7] left foot, [8..11] right knee,
// [12..15] right foot.
struct Stickman
{
    uint comm, pcomm; // command bits: 1 left, 2 right, 4 jump, 8 use element; pcomm the last direction
    int elem;         // the element it shoots
    uint spwn;        // 1 while its particle exists (or is inside a portal)
    uint frames;      // ticks since it last shot (LIGH only every 30)
    uint rocketBoots, fan;
    uint spawnRef;    // ours: SPAWN's or SPAWN2's id + 1, 0 if none (TPT's spawnID; players only)
    uint partRef;     // ours: its particle's id + 1, STKM_IN_PORTAL while inside a portal
    uint pad0, pad1, pad2;
    float legs[16];
    float accs[8];
};
static const uint STICKMEN = 102;
static const uint MAX_FIGHTERS = 100;
static const uint STKM_IN_PORTAL = 0xFFFFFFFFu;
[[vk::binding(18, 0)]] RWStructuredBuffer<Stickman> stickmen;

// TPT's pmap_count, for CheckStacking (stacking.slang), kept only where particles share a cell: [0, W*H) per cell
// (the count, and flags), then at W*H the number of such cells listed this tick, followed by STACK_CAP cells,
// STACK_CAP counts and STACK_CAP lowest ids.
[[vk::binding(19, 0)]] RWStructuredBuffer<uint> stacking;
static const uint STACK_CAP = 4096;
static const uint STACK_SEEN = 0x80000000u;   // a second particle came into the cell this tick
static const uint STACK_MARK = 0x40000000u;   // marked by CheckStacking; the low bits are its list entry
static const uint STACK_COUNT = 0x3FFFFFFFu;
uint stackBase() { return pc.W * pc.H; }
// Particles pmap_count counts (not energy particles, THDR, EMBR, FIGH or PLSM)
bool stackCounted(uint t) { return t != PT_THDR && t != PT_EMBR && t != PT_FIGH && t != PT_PLSM; }

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

// ---- portals (PRTI.h) ----

// The eight directions particles enter a portal from: 0 1 2 / 7 . 3 / 6 5 4.
static const int PORTAL_RX[8] = { -1, 0, 1, 1, 1, 0, -1, -1 };
static const int PORTAL_RY[8] = { -1, -1, -1, 0, 1, 1, 1, 0 };
static const uint PORTAL_SLOTS = 80;
static const uint PORTAL_CLAIMED = 0xFFFFFFFFu; // a slot being written or read

uint portalSlot(uint channel, uint dir, uint n) { return (channel * 8 + dir) * PORTAL_SLOTS + n; }

// The portal channel from a temperature (same as WIFI).
int portalChannel(float temp) { return clamp(int((temp - 73.15) / 100.0 + 1.0), 0, int(WIFI_CHANNELS) - 1); }

// Puts particle q (of type t) into the first free slot of channel/dir. Portals of a channel can be updated in
// the same pass anywhere in the world, so a slot is claimed with an atomic swap and its type is set only once
// the particle is written. False if all 80 slots are taken.
bool portalStore(uint channel, uint dir, Particle q, uint t)
{
    for (uint n = 0; n < PORTAL_SLOTS; n++)
    {
        uint s = portalSlot(channel, dir, n);
        uint was;
        InterlockedCompareExchange(portals[s].pad2, 0u, PORTAL_CLAIMED, was);
        if (was != 0)
            continue;
        q.pad2 = PORTAL_CLAIMED;
        portals[s] = q;
        DeviceMemoryBarrier();
        InterlockedExchange(portals[s].pad2, t, was);
        return true;
    }
    return false;
}

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

// Ambient heat hv += delta for air cell (cx, cy), atomically, as LIGH does it: capped at MAX_TEMP, and an overflow
// (below 0) also becomes MAX_TEMP.
void airAddHVLigh(int cx, int cy, float delta)
{
    if (cx < 0 || cy < 0 || cx >= int(pc.CW) || cy >= int(pc.CH))
        return;
    uint k = AIR_HV * pc.CW * pc.CH + uint(cy) * pc.CW + uint(cx);
    uint cur = air[k];
    for (int attempt = 0; attempt < 16; attempt++)
    {
        float v = asfloat(cur) + delta;
        if (v > MAX_TEMP || v < 0.0)
            v = MAX_TEMP;
        uint orig;
        InterlockedCompareExchange(air[k], cur, asuint(v), orig);
        if (orig == cur)
            return;
        cur = orig;
    }
}

// TESC's pressure damping when it makes lightning: pv moves 0.5 towards 0 (to 0 if within 0.5), atomically.
void airDampPV(int cx, int cy)
{
    if (cx < 0 || cy < 0 || cx >= int(pc.CW) || cy >= int(pc.CH))
        return;
    uint k = AIR_PV * pc.CW * pc.CH + uint(cy) * pc.CW + uint(cx);
    uint cur = air[k];
    for (int attempt = 0; attempt < 16; attempt++)
    {
        float v = asfloat(cur);
        if (v == 0.0)
            return;
        v = abs(v) <= 0.5 ? 0.0 : v - (v > 0.0 ? 0.5 : -0.5);
        uint orig;
        InterlockedCompareExchange(air[k], cur, asuint(v), orig);
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

// Work that reaches too far for the colour passes and is done in parallel after them (rays.slang), three
// words each in emapReqs[4 cells, 6 cells):
// - POST_BEAM: ARAY/CRAY/DRAY id sparked from direction dir (0-8, the neighbour with the spark) by a spark on
//   sparkCtype; data = dir | sparkCtype << 8;
// - POST_LIGH: a LIGH segment's next stretch of lightning (see updateLIGH).
static const uint POST_BEAM = 0;
static const uint POST_LIGH = 1;

uint postCapacity() { return 2 * pc.CW * pc.CH / 3; }

void requestPost(uint kind, uint id, uint data)
{
    uint slot;
    InterlockedAdd(counters[CTR_RAY_REQ], 1u, slot);
    if (slot < postCapacity())
    {
        uint base = 4 * pc.CW * pc.CH + 3 * slot;
        emapReqs[base] = id;
        emapReqs[base + 1] = kind;
        emapReqs[base + 2] = data;
    }
}

// Element_PPIP_flood_trigger: a PSCN, NSCN or INST spark sets the trigger for all the PPIP connected to (x, y)
// (HEAC makes connected PIPE conduct heat). It reaches the whole network, so it runs after the passes like
// FloodINST, two words each in emapReqs[7 cells, 8 cells).
void requestPpipFlood(int x, int y, uint sparkedBy)
{
    if (!inSim(x, y))
        return;
    uint slot;
    InterlockedAdd(counters[CTR_PPIP_REQ], 1u, slot);
    if (slot < pc.CW * pc.CH / 2)
    {
        emapReqs[7 * pc.CW * pc.CH + 2 * slot] = uint(x) | (uint(y) << 16);
        emapReqs[7 * pc.CW * pc.CH + 2 * slot + 1] = sparkedBy;
    }
}

// A PSTN sparked to extend (2) or retract (1): its arm and what it pushes can reach far along a line, so it is moved
// after the passes, one piston at a time in the order they asked (deferred.slang), in emapReqs[8 cells, 9 cells).
void requestPiston(uint id, uint state)
{
    uint slot;
    InterlockedAdd(counters[CTR_PSTN_REQ], 1u, slot);
    if (slot < pc.CW * pc.CH)
        emapReqs[8 * pc.CW * pc.CH + slot] = id | (state << 30);
}

bool isStickman(uint t) { return t == PT_STKM || t == PT_STKM2 || t == PT_FIGH; }

// CreateAllowed of STKM, STKM2, FIGH, SPAWN and SPAWN2: one stick man and one spawn point per player, at most 100
// fighters, counting what was made since stickmen.slang last took stock. A true answer claims the place.
bool stickmanCreateAllowed(uint t)
{
    if (t == PT_FIGH)
    {
        uint made;
        InterlockedAdd(counters[CTR_FIGH_NEW], 1u, made);
        return counters[CTR_FIGH_COUNT] + made < MAX_FIGHTERS;
    }
    uint s = (t == PT_STKM || t == PT_SPAWN) ? 0u : 1u;
    uint bit = (t == PT_STKM ? 1u : t == PT_STKM2 ? 2u : t == PT_SPAWN ? 4u : 8u);
    if (t == PT_STKM || t == PT_STKM2)
    {
        uint ref = stickmen[s].partRef;
        if (stickmen[s].spwn != 0 && (ref == STKM_IN_PORTAL || (ref != 0 && ptype[ref - 1] == t)))
            return false;
    }
    else
    {
        uint ref = stickmen[s].spawnRef;
        if (ref != 0 && ptype[ref - 1] == t)
            return false;
    }
    uint old;
    InterlockedOr(counters[CTR_STKM_FLAGS], bit, old);
    return (old & bit) == 0;
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

// Element_SOAP_detach: a SOAP particle lets go of the bubble neighbours it is linked to (by id, tmp and tmp2),
// and they forget it. TPT also calls it whenever SOAP is killed or turned into something else; here the
// neighbours drop such links in their own update instead (updateSOAP), which keeps this out of every kill.
void soapDetach(uint i)
{
    Particle s = parts[i];
    if ((s.ctype & 2) != 0 && s.tmp >= 0 && uint(s.tmp) < pc.capacity && ptype[s.tmp] == PT_SOAP &&
        (parts[s.tmp].ctype & 4) != 0)
        parts[s.tmp].ctype ^= 4;
    if ((s.ctype & 4) != 0 && s.tmp2 >= 0 && uint(s.tmp2) < pc.capacity && ptype[s.tmp2] == PT_SOAP &&
        (parts[s.tmp2].ctype & 2) != 0)
        parts[s.tmp2].ctype ^= 2;
    parts[i].ctype = 0;
}

// TPT's kill_part: remove from pmap if it is there, then free the id. x, y is where the caller saw
// the particle; if another thread has moved it since, its current position is tried too. An entry
// that is left behind (the particle was mid-move) points at a dead particle and is treated as empty
// until the freelist pass clears it.
void killPart(uint id, int x, int y)
{
    // Energy particles are in photons, the rest in pmap.
    bool energy = (elems[ptype[id]].properties & TYPE_ENERGY) != 0;
    uint orig = 0;
    if (inSim(x, y))
    {
        if (energy)
            InterlockedCompareExchange(photons[cellIndex(x, y)], pmapEnc(id), 0u, orig);
        else
            InterlockedCompareExchange(pmap[cellIndex(x, y)], pmapEnc(id), 0u, orig);
    }
    if (orig != pmapEnc(id))
    {
        int cx = int(parts[id].x + 0.5);
        int cy = int(parts[id].y + 0.5);
        if ((cx != x || cy != y) && inSim(cx, cy))
        {
            if (energy)
                InterlockedCompareExchange(photons[cellIndex(cx, cy)], pmapEnc(id), 0u, orig);
            else
                InterlockedCompareExchange(pmap[cellIndex(cx, cy)], pmapEnc(id), 0u, orig);
        }
    }
    if (ptype[id] != PT_NONE)
        pushFreed(id);
    ptype[id] = PT_NONE;
}

bool isEnergy(uint t) { return (elems[t].properties & TYPE_ENERGY) != 0; }

// Element_FILT_getWavelengths: FILT's colour, from ctype or else from its temperature.
int filtWavelengths(Particle f)
{
    if ((f.ctype & 0x3FFFFFFF) != 0)
        return f.ctype;
    int tempBin = clamp(int((f.temp - 273.0) * 0.025), 0, 25);
    return 0x1F << tempBin;
}

// Element_FILT_interactWavelengths: what a FILT (by its mode, tmp) does to the colour of light passing through.
int filtInteract(Particle f, int origWl)
{
    const int mask = 0x3FFFFFFF;
    int filtWl = filtWavelengths(f);
    switch (f.tmp)
    {
    case 0:
        return filtWl; // assign
    case 1:
        return origWl & filtWl; // filter
    case 2:
        return origWl | filtWl; // add
    case 3:
        return origWl & ~filtWl; // filter the other way
    case 4:
    {
        int shift = int((f.temp - 273.0) * 0.025);
        if (shift <= 0)
            shift = 1;
        return (origWl << shift) & mask; // red shift
    }
    case 5:
    {
        int shift = int((f.temp - 273.0) * 0.025);
        if (shift <= 0)
            shift = 1;
        return (origWl >> shift) & mask; // blue shift
    }
    case 6:
        return origWl; // no change
    case 7:
        return origWl ^ filtWl; // XOR
    case 8:
        return ~origWl & mask; // invert
    case 9:
    {
        int t1 = (origWl & 0x0000FF) + rngBetween(-2, 2);
        int t2 = ((origWl & 0x00FF00) >> 8) + rngBetween(-2, 2);
        int t3 = ((origWl & 0xFF0000) >> 16) + rngBetween(-2, 2);
        return int(uint(origWl) & 0xFF000000u) | (t3 << 16) | (t2 << 8) | t1;
    }
    case 10:
    {
        int lsb = filtWl & -filtWl;
        return int(uint(origWl) * uint(lsb)) & 0x3FFFFFFF; // red shift by the lowest band
    }
    case 11:
    {
        int lsb = filtWl & -filtWl;
        return lsb != 0 ? (origWl / lsb) & 0x3FFFFFFF : 0; // blue shift
    }
    default:
        return filtWl;
    }
}

// The FILT serialisation sensors use: write value into a line of FILT starting next to the sensor.
void writeFiltLine(int x, int y, int rx, int ry, int value)
{
    int nx = x + rx, ny = y + ry;
    while (inSim(nx, ny))
    {
        int fid = pmapId(pmap[cellIndex(nx, ny)]);
        if (fid < 0 || ptype[fid] != PT_FILT)
            break;
        parts[fid].ctype = value;
        nx += rx;
        ny += ry;
    }
}

// pv += delta for air cell (cx, cy), atomically, kept within TPT's pressure limits.
void airAddPVClamped(int cx, int cy, float delta)
{
    if (cx < 0 || cy < 0 || cx >= int(pc.CW) || cy >= int(pc.CH))
        return;
    uint k = AIR_PV * pc.CW * pc.CH + uint(cy) * pc.CW + uint(cx);
    uint cur = air[k];
    for (int attempt = 0; attempt < 16; attempt++)
    {
        uint orig;
        InterlockedCompareExchange(air[k], cur, asuint(clamp(asfloat(cur) + delta, MIN_PRESSURE, MAX_PRESSURE)), orig);
        if (orig == cur)
            return;
        cur = orig;
    }
}

// The energy particle at (x, y), or PT_NONE.
uint photonTypeAt(int x, int y)
{
    if (!inSim(x, y))
        return PT_NONE;
    int id = pmapId(photons[cellIndex(x, y)]);
    return id < 0 ? PT_NONE : ptype[id];
}

void killPartById(uint id)
{
    Particle q = parts[id];
    killPart(id, int(q.x + 0.5), int(q.y + 0.5));
}

// PIPE's and PPIP's tmp flags (PIPE.cpp).
static const int PFLAG_CAN_CONDUCT = 0x00000001;
static const int PFLAG_PARTICLE_DECO = 0x00000002;
static const int PFLAG_NORMALSPEED = 0x00010000;
static const int PFLAG_INITIALIZING = 0x00020000;
static const int PFLAG_COLOR_RED = 0x00040000;
static const int PFLAG_COLOR_GREEN = 0x00080000;
static const int PFLAG_COLOR_BLUE = 0x000C0000;
static const int PFLAG_COLORS = 0x000C0000;
static const int PPIP_TMPFLAG_REVERSED = 0x01000000;
static const int PPIP_TMPFLAG_PAUSED = 0x02000000;
static const int PPIP_TMPFLAG_TRIGGER_REVERSE = 0x04000000;
static const int PPIP_TMPFLAG_TRIGGER_OFF = 0x08000000;
static const int PPIP_TMPFLAG_TRIGGER_ON = 0x10000000;
static const int PPIP_TMPFLAG_TRIGGERS = 0x1C000000;

// SimulationData::IsHeatInsulator: HSWC conducts only while switched on (life 10), PIPE and PPIP only when made
// to conduct (by HEAC next to them).
bool isHeatInsulator(uint t, int life, int tmp)
{
    return elems[t].heatConduct == 0 || (t == PT_HSWC && life != 10) ||
           ((t == PT_PIPE || t == PT_PPIP) && (tmp & PFLAG_CAN_CONDUCT) == 0);
}

// SimulationData::HeatCapacityOf: a PIPE also holds the heat of what it carries.
float heatCapacityOf(uint t, int ctype)
{
    if ((t == PT_PIPE || t == PT_PPIP) && ctype > 0 && ctype < MAX_ELEMENTS && elems[ctype].existsInTpt != 0)
        return elems[t].heatCapacity + elems[ctype].heatCapacity;
    return elems[t].heatCapacity;
}
