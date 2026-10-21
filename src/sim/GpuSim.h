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

    // Adds particles (e.g. from a TPT save) at an offset, into empty cells only. Waits for the GPU.
    void insertParticles(const std::vector<ClassicParticle> &particles, int offsetX, int offsetY);

    void setColours(uint32_t nx, uint32_t ny);
    void setAir(const AirSettings &a) { air_ = a; }
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
    uint32_t passOrder_;
    bool debugCounters_ = false;
    bool serial_ = false;
    std::vector<uint32_t> fixedOrder_;
    uint64_t tick_ = 0;
    std::mt19937 rng_;

    Buffer parts_, ptype_, pmap_, elems_, canMove_, lists_, counters_, airBuf_, frame_, freelist_, stats_, inbox_;

    VkDescriptorSetLayout setLayout_ = VK_NULL_HANDLE;
    VkPipelineLayout pipeLayout_ = VK_NULL_HANDLE;
    VkDescriptorPool descPool_ = VK_NULL_HANDLE;
    VkDescriptorSet set_ = VK_NULL_HANDLE;
    VkPipeline pInsert_, pAir_, pPrepare_, pArgs_, pUpdate_, pFreelist_, pSpawn_, pRender_, pStatsParts_, pStatsCells_;
};

} // namespace bp
