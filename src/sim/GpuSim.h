// The GPU simulation: buffers, compute pipelines and the per-tick command sequence.
#pragma once
#include "gpu/VkContext.h"
#include "sim/Elements.h"
#include "sim/Scenes.h"

#include <array>
#include <random>
#include <string>
#include <vector>

namespace bp {

struct SimConfig {
    uint32_t W = 3840, H = 2160;
    uint32_t colourN = 5; // update passes per tick = colourN^2
    uint32_t seed = 1;
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

    void setColourN(uint32_t n);
    uint32_t colourN() const { return colourN_; }
    uint32_t width() const { return W_; }
    uint32_t height() const { return H_; }
    uint64_t tick() const { return tick_; }

private:
    void createPipelines();
    void updateDescriptors();
    PushConsts basePush() const;
    void dispatch(VkCommandBuffer cmd, VkPipeline pipe, const PushConsts &pcs, uint32_t gx, uint32_t gy = 1);
    uint32_t listCapFor(uint32_t n) const;

    VkContext &ctx_;
    const ElementTable &elements_;
    std::string shaderDir_;
    uint32_t W_, H_, CW_, CH_, capacity_;
    uint32_t colourN_;
    uint32_t seed_;
    uint64_t tick_ = 0;
    std::mt19937 rng_;

    Buffer parts_, ptype_, pmap_, elems_, canMove_, lists_, counters_, air_, frame_, freelist_, stats_;

    VkDescriptorSetLayout setLayout_ = VK_NULL_HANDLE;
    VkPipelineLayout pipeLayout_ = VK_NULL_HANDLE;
    VkDescriptorPool descPool_ = VK_NULL_HANDLE;
    VkDescriptorSet set_ = VK_NULL_HANDLE;
    VkPipeline pPrepare_, pArgs_, pUpdate_, pFreelist_, pSpawn_, pRender_, pStatsParts_, pStatsCells_;
};

} // namespace bp
