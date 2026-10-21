// Structures shared with the shaders. Keep in sync with shaders/common.slang.
#pragma once
#include <cstdint>

namespace bp {

constexpr int CELL = 4;              // air/heat grid block size, as in TPT
constexpr int MAX_ELEMENTS = 256;    // element table size (TPT ids are kept)
constexpr uint32_t MAX_COLOURS = 256; // colour lists for the update passes (NX*NY <= 256)
constexpr uint32_t MAX_COLOUR_SIDE = 16;
constexpr uint32_t AIR_PLANES = 12;  // float planes in the air buffer, see shaders/common.slang

// Counter buffer layout (uint32 indices).
constexpr uint32_t CTR_COLOUR_COUNT = 0;    // [0, 256): particles per colour list
constexpr uint32_t CTR_DISPATCH = 256;      // [256, 256+3*256): indirect dispatch args per colour
constexpr uint32_t CTR_FREE_COUNT = 1024;   // free ids found by the freelist pass
constexpr uint32_t CTR_FREE_TAKEN = 1025;   // free ids handed out by spawn
constexpr uint32_t CTR_LIST_OVERFLOW = 1026;
constexpr uint32_t CTR_STACKED = 1028; // cell collisions found while rebuilding pmap, summed over ticks
constexpr uint32_t CTR_EMAP_REQ = 1040; // queued set_emap requests (cleared every tick)
constexpr uint32_t CTR_INST_REQ = 1041; // queued FloodINST requests (cleared every tick)
constexpr uint32_t CTR_DBG_MOVED = 1029; // debug move counters, see shaders/common.slang
constexpr uint32_t CTR_DBG_LOCKED = 1030;
constexpr uint32_t CTR_DBG_OWN_LOST = 1031;
constexpr uint32_t CTR_DBG_CAS_LOST = 1032;
constexpr uint32_t CTR_DBG_SAME_PASS = 1033;
constexpr uint32_t CTR_SIZE = 1056;

// Stats buffer layout.
constexpr uint32_t STAT_TYPE_COUNT = 0;      // [0, 256)
constexpr uint32_t STAT_PART_NOT_IN_PMAP = 256;
constexpr uint32_t STAT_PMAP_DANGLING = 257; // pmap points at a dead particle
constexpr uint32_t STAT_PMAP_WRONG_POS = 258;
constexpr uint32_t STAT_PMAP_LOCKED = 259;
constexpr uint32_t STAT_SIZE = 264;

// TPT's Particle without `type` (types live in their own array so neighbour scans stay cheap).
struct Particle {
    int32_t life;
    int32_t ctype;
    float x, y;
    float vx, vy;
    float temp;
    int32_t tmp, tmp2, tmp3, tmp4;
    uint32_t flags;
    uint32_t dcolour;
    uint32_t pass;       // ours: colour pass this tick, set by prepare
    uint32_t pad1, pad2;
};
static_assert(sizeof(Particle) == 64);

struct ElementProps {
    float advection, airDrag, airLoss, loss, collision, gravity, diffusion, hotAir;
    int32_t falldown, flammable, explosive, meltable, hardness, weight, heatConduct;
    uint32_t properties;
    float lowPressure, highPressure, lowTemperature, highTemperature;
    int32_t lowPressureT, highPressureT, lowTemperatureT, highTemperatureT;
    uint32_t colour;
    float defaultTemp;
    float heatCapacity;
    int32_t enabled;
    float newtonianGravity;
    uint32_t photonReflectWavelengths;
    uint32_t carriesTypeIn;
    int32_t existsInTpt; // enabled in TPT, whether or not the GPU engine runs it
};
static_assert(sizeof(ElementProps) == 128);

// Push constants, shared by every kernel.
struct PushConsts {
    uint32_t W, H, CW, CH;
    uint32_t tick, seed;
    uint32_t colourNX, colourIdx;
    uint32_t colourOX, colourOY;
    uint32_t listCap, capacity;
    float camX, camY, zoom;
    uint32_t screenW, screenH;
    int32_t brushX, brushY, brushR, brushType;
    uint32_t mode;        // render: display mode; air: which stage; update: debug flags
    uint32_t colourNY;
};
static_assert(sizeof(PushConsts) <= 128);

// Simulation settings, mirrored by SimParams in shaders/common.slang.
struct SimParams {
    uint32_t airMode;
    float vorticity;
    float edgePressure, edgeVX, edgeVY;
    float ambientTemp;
    uint32_t edgeMode;
    uint32_t gravityMode;
    float customGravX, customGravY;
    uint32_t ambientHeat;
    uint32_t legacyHeat;
    uint32_t waterEqualisation;
    uint32_t convectionMode;
    uint32_t pad0, pad1;
};
static_assert(sizeof(SimParams) == 64);

// TPT's wall ids (WL_*).
enum WallId : uint8_t {
    WL_ERASE = 0, WL_WALLELEC = 1, WL_EWALL = 2, WL_DETECT = 3, WL_STREAM = 4, WL_FAN = 5, WL_ALLOWLIQUID = 6,
    WL_DESTROYALL = 7, WL_WALL = 8, WL_ALLOWAIR = 9, WL_ALLOWPOWDER = 10, WL_ALLOWALLELEC = 11, WL_EHOLE = 12,
    WL_ALLOWGAS = 13, WL_GRAV = 14, WL_ALLOWENERGY = 15, WL_BLOCKAIR = 16, WL_ERASEALL = 17, WL_STASIS = 18,
};

} // namespace bp
