// A game mode's world as the viewer sees it: something that can step, take brush strokes, load and save, and
// draw itself through the camera into a frame buffer.
//   Classic: TPT's own engine at 612x384 (ClassicWorld.cpp).
//   Screen/Huge: the GPU engine at 3840x2160 or larger (GpuWorld.cpp).
#pragma once
#include "app/App.h"
#include "gpu/VkContext.h"
#include "sim/GpuSim.h"

#include <memory>
#include <string>
#include <vector>

namespace bp {

struct WallOp {
    int cx, cy, r; // cells
    uint8_t wall;  // 0 erases
};

// Settings a world can be switched between (TPT's simulation options).
struct WorldSettings {
    int edgeMode = 0;    // 0 void, 1 solid, 2 loop
    int gravityMode = 0; // 0 vertical, 1 off, 2 radial
    int airMode = 0;     // 0 on, 1 pressure off, 2 velocity off, 3 off, 4 no update
    bool ambientHeat = false;
};

struct FrameRequest {
    int ticks = 0;
    std::vector<BrushOp> brush;
    std::vector<WallOp> walls;
    float camX = 0, camY = 0, zoom = 1;
    uint32_t screenW = 0, screenH = 0;
    int brushX = 0, brushY = 0, brushR = -1;
    int slot = 0;                         // frame-in-flight index
    VkQueryPool queries = VK_NULL_HANDLE; // 3 timestamps for the first tick (GPU worlds)
};

class World {
public:
    virtual ~World() = default;
    virtual std::string modeName() const = 0;
    virtual uint32_t width() const = 0;
    virtual uint32_t height() const = 0;

    virtual void ensureFrameBuffer(uint32_t w, uint32_t h) = 0;
    virtual const Buffer &frameBuffer() const = 0;
    // Steps `ticks` times, applies the brush and draws the view. May do CPU work (Classic).
    virtual void record(VkCommandBuffer cmd, const FrameRequest &f) = 0;

    // Tick time comes from the GPU timestamps the viewer reads, or from cpuTickMs().
    virtual bool gpuTimed() const = 0;
    virtual double cpuTickMs() const { return 0; }
    virtual uint64_t particleCount() = 0;

    virtual std::vector<int> elements() const = 0; // element ids that can be drawn
    virtual std::string elementName(int type) const = 0;

    virtual void loadScene(const std::string &name) = 0;
    // Loads a TPT save (.cps/.stm). Classic replaces its simulation; GPU worlds place it centred on (x, y).
    virtual bool loadSave(const std::string &path, int x, int y, std::string &message) = 0;
    virtual bool save(const std::string &path, std::string &message) = 0;
    virtual std::string saveExtension() const = 0;

    virtual void applySettings(const WorldSettings &s) = 0;

    virtual void cycleDisplay() = 0;
    virtual std::string displayName() const = 0;
    virtual void cyclePasses() {}
    virtual std::string passesName() const { return ""; }
};

std::unique_ptr<World> makeGpuWorld(VkContext &ctx, const AppOptions &o, uint32_t W, uint32_t H, const char *mode);
std::unique_ptr<World> makeClassicWorld(VkContext &ctx, const AppOptions &o);

} // namespace bp
