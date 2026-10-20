// Structures shared with the shaders. Keep in sync with shaders/common.slang.
#pragma once
#include <cstdint>

namespace bp {

constexpr int CELL = 4;              // air/heat grid block size, as in TPT
constexpr int MAX_ELEMENTS = 256;    // element table size (TPT ids are kept)
constexpr uint32_t MAX_COLOURS = 32; // colour lists for the update passes (N*N <= 25)

// Counter buffer layout (uint32 indices).
constexpr uint32_t CTR_COLOUR_COUNT = 0;   // [0, 32): particles per colour list
constexpr uint32_t CTR_DISPATCH = 32;      // [32, 32+3*32): indirect dispatch args per colour
constexpr uint32_t CTR_FREE_COUNT = 160;   // free ids found by the freelist pass
constexpr uint32_t CTR_FREE_TAKEN = 161;   // free ids handed out by spawn
constexpr uint32_t CTR_LIST_OVERFLOW = 162;
constexpr uint32_t CTR_STACKED = 164; // cell collisions found while rebuilding pmap, summed over ticks
constexpr uint32_t CTR_SIZE = 192;

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
};
static_assert(sizeof(ElementProps) == 112);

// Push constants, shared by every kernel.
struct PushConsts {
    uint32_t W, H, CW, CH;
    uint32_t tick, seed;
    uint32_t colourN, colourIdx;
    uint32_t colourOX, colourOY;
    uint32_t listCap, capacity;
    float camX, camY, zoom;
    uint32_t screenW, screenH;
    int32_t brushX, brushY, brushR, brushType;
    uint32_t mode;
    uint32_t pad0, pad1;
};
static_assert(sizeof(PushConsts) <= 128);

} // namespace bp
