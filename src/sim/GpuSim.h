// The GPU simulation: buffers, compute pipelines and the per-tick command sequence.
#pragma once
#include "classic/ClassicSim.h"
#include "gpu/VkContext.h"
#include "sim/Elements.h"
#include "sim/Scenes.h"

#include <array>
#include <random>
#include <string>
#include <vector>

namespace bp {

// TPT's air settings (Air.h and the simulation options).
struct AirSettings {
    uint32_t mode = 0;          // 0 on, 1 pressure off, 2 velocity off, 3 off, 4 no update
    float vorticity = 0.1f;     // vorticityCoeff
    float edgePressure = 0.0f;  // ambient air pressure and velocity at the edges
    float edgeVX = 0.0f, edgeVY = 0.0f;
    float ambientTemp = 22.0f + 273.15f;
};

// TPT's other simulation options (the in-game "Simulation options" dialog).
struct SimSettings {
    uint32_t edgeMode = 0;      // 0 void, 1 solid, 2 loop
    uint32_t gravityMode = 0;   // 0 vertical, 1 off, 2 radial, 3 custom
    float customGravX = 0.0f, customGravY = 0.0f;
    bool ambientHeat = false;
    bool heat = true;           // false = TPT's legacy (no heat simulation)
    bool waterEqualisation = false;
    uint32_t convectionMode = 1; // 0 legacy, 1 Boussinesq
};

struct SimConfig {
    uint32_t W = 3840, H = 2160;
    // Colour grid; update passes per tick = colourNX * colourNY. 7x7 measured closest to TPT (docs/accuracy.md)
    // and is strictly race-free for reactions that reach up to 3 px.
    uint32_t colourNX = 7, colourNY = 7;
    uint32_t seed = 1;
    // Order of the colour passes within a tick: 0 = random each tick; 1 = raster order (top row of the N*N
    // pattern first, like TPT's top-to-bottom order after loading a save) with a random grid offset;
    // 2 = raster order with a fixed grid offset; 3 = one random order kept for every tick, fixed offset;
    // 4 = one random order kept for every tick, random offset.
    uint32_t passOrder = 0;
    AirSettings air;
    SimSettings settings;
    std::string shaderDir;
};

struct BrushOp {
    int x, y, r, type; // type 0 erases
};

struct RenderParams {
    float camX = 0, camY = 0, zoom = 1; // world position of the screen's top-left corner
    uint32_t screenW = 0, screenH = 0;
    uint32_t mode = 0;                  // 0 normal, 1 heat
    int brushX = 0, brushY = 0, brushR = -1;
};

struct SimStats {
    std::array<uint32_t, MAX_ELEMENTS> typeCount{};
    uint64_t total = 0;
    uint32_t notInPmap = 0, dangling = 0, wrongPos = 0, locked = 0, listOverflow = 0;
    uint32_t stacked = 0; // cell collisions found by the pmap rebuild, summed since load
    uint32_t moved = 0, lockedTarget = 0, ownLost = 0, casLost = 0, samePass = 0; // with setDebugCounters(true)
};

class GpuSim {
public:
    GpuSim(VkContext &ctx, const SimConfig &cfg, const ElementTable &elements);
    ~GpuSim();
    GpuSim(const GpuSim &) = delete;
    GpuSim &operator=(const GpuSim &) = delete;

    void load(const SceneData &scene);

    // Records one simulation tick. If pool is set, writes 3 timestamps from firstQuery:
    // start, after list building, end.
    void recordTick(VkCommandBuffer cmd, VkQueryPool pool = VK_NULL_HANDLE, uint32_t firstQuery = 0);
    void recordBrush(VkCommandBuffer cmd, const std::vector<BrushOp> &ops);
    void recordRender(VkCommandBuffer cmd, const RenderParams &rp);

    // Reallocates the frame buffer if needed. Waits for the device when it does.
    void ensureFrameBuffer(uint32_t w, uint32_t h);
    const Buffer &frameBuffer() const { return frame_; }

    SimStats readStats();

    // Copies all live particles back to the CPU (for comparisons and saving; slow for big worlds).
    std::vector<ClassicParticle> readParticles();

    // Air maps (pressure, velocity x/y, ambient heat), (W/CELL) x (H/CELL) each. Wait for the GPU.
    void readAir(std::vector<float> &pv, std::vector<float> &vx, std::vector<float> &vy, std::vector<float> &hv);
    void writeAir(const std::vector<float> &pv, const std::vector<float> &vx, const std::vector<float> &vy,
                  const std::vector<float> &hv);

    // Adds particles (e.g. from a TPT save) at an offset, into empty cells only. Waits for the GPU.
    void insertParticles(const std::vector<ClassicParticle> &particles, int offsetX, int offsetY);

    void setColours(uint32_t nx, uint32_t ny);
    void setAir(const AirSettings &a) { air_ = a; paramsDirty_ = true; }
    void setSettings(const SimSettings &s);
    const SimSettings &settings() const { return settings_; }
    // Walls, one TPT wall id per CELL x CELL block (width()/CELL by height()/CELL). Fan velocities are optional.
    void setWalls(const std::vector<uint8_t> &bmap, const std::vector<float> *fanVX = nullptr,
                  const std::vector<float> *fanVY = nullptr);
    // Paints walls in a circle of radius r cells around cell (cx, cy) (wall 0 erases), recorded into cmd so the
    // frame doesn't stall. Edge-mode border cells are left alone.
    void recordPaintWalls(VkCommandBuffer cmd, int cx, int cy, int r, uint8_t wall);
    // Copies the non-empty walls of a (srcCW x srcCH)-cell area to cell offset (cx, cy).
    void stampWalls(const std::vector<uint8_t> &src, int srcCW, int srcCH, int cx, int cy);
    const std::vector<uint8_t> &walls() const { return bmap_; }
    void setPassOrder(uint32_t m) { passOrder_ = m; }
    void setDebugCounters(bool on) { debugCounters_ = on; }
    // Testing only: update particles one at a time in id order, like TPT (slow).
    void setSerial(bool on) { serial_ = on; }
    const AirSettings &air() const { return air_; }
    uint32_t colourNX() const { return colourNX_; }
    uint32_t colourNY() const { return colourNY_; }
    uint32_t width() const { return W_; }
    uint32_t height() const { return H_; }
    uint64_t tick() const { return tick_; }

private:
    void createPipelines();
    void uploadWalls();
    void applyEdgeMode();
    SimParams currentParams() const;
    void updateDescriptors();
    PushConsts basePush() const;
    void dispatch(VkCommandBuffer cmd, VkPipeline pipe, const PushConsts &pcs, uint32_t gx, uint32_t gy = 1);
    uint32_t listCapFor(uint32_t nx, uint32_t ny) const;

    VkContext &ctx_;
    const ElementTable &elements_;
    std::string shaderDir_;
    uint32_t W_, H_, CW_, CH_, capacity_;
    uint32_t colourNX_, colourNY_;
    uint32_t seed_;
    AirSettings air_;
    SimSettings settings_;
    bool paramsDirty_ = true;
    std::vector<uint8_t> bmap_; // CPU copy of the wall types
    uint32_t passOrder_;
    bool debugCounters_ = false;
    bool serial_ = false;
    std::vector<uint32_t> fixedOrder_;
    uint64_t tick_ = 0;
    std::mt19937 rng_;

    Buffer parts_, ptype_, pmap_, elems_, canMove_, lists_, counters_, airBuf_, frame_, freelist_, stats_, inbox_,
        walls_, params_, emapReqs_, defaults_, photons_;

    VkDescriptorSetLayout setLayout_ = VK_NULL_HANDLE;
    VkPipelineLayout pipeLayout_ = VK_NULL_HANDLE;
    VkDescriptorPool descPool_ = VK_NULL_HANDLE;
    VkDescriptorSet set_ = VK_NULL_HANDLE;
    VkPipeline pWallsPre_, pEmapFill_, pDeferred_, pInsert_, pAir_, pPrepare_, pArgs_, pUpdate_, pFreelist_, pSpawn_, pRender_, pStatsParts_, pStatsCells_;
};

} // namespace bp
