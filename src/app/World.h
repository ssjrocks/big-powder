// A game mode's world as the viewer sees it: something that can step, take TPT's tools, load and save, and draw
// itself through the camera into a frame buffer.
//   Classic: TPT's own engine at 612x384 (ClassicWorld.cpp).
//   Screen/Huge: the GPU engine at 3840x2160 or larger (GpuWorld.cpp).
#pragma once
#include "app/App.h"
#include "classic/TptSave.h"
#include "classic/TptTools.h"
#include "gpu/VkContext.h"
#include "sim/GpuSim.h"
#include "ui/GameUi.h"

#include <memory>
#include <string>
#include <vector>

namespace bp {

// Settings a world can be switched between (TPT's simulation options).
struct WorldSettings {
    int edgeMode = 0;    // 0 void, 1 solid, 2 loop
    int gravityMode = 0; // 0 vertical, 1 off, 2 radial
    int airMode = 0;     // 0 on, 1 pressure off, 2 velocity off, 3 off, 4 no update
    bool ambientHeat = false;
    bool newtonianGravity = false;
};

struct FrameRequest {
    int ticks = 0;
    std::vector<ToolUse> uses; // TPT's tools, used in order before the ticks
    float camX = 0, camY = 0, zoom = 1;
    uint32_t screenW = 0, screenH = 0;
    int brushX = 0, brushY = 0; // the mouse in the world (what the interface's text describes)
    std::vector<int> cursor;    // TPT's brush cursor (ui/BrushCursor)
    uint32_t overlays = 0;                // gravity field (1) and gravity zones (2) displays
    int slot = 0;                         // frame-in-flight index
    VkQueryPool queries = VK_NULL_HANDLE; // 3 timestamps for the first tick (GPU worlds)
};

// What World::snapshot keeps of a world, for undo; only the world that made it can restore it.
struct WorldSnapshot {
    virtual ~WorldSnapshot() = default;
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
    // Uses tools now, between frames, before whatever else is asked of the world (record's uses come after)
    virtual void apply(const std::vector<ToolUse> &uses) = 0;
    // Simulation::CreateSnapshot and Restore, for undo: the simulation and its signs, not the settings. Each
    // takes what is queued (Classic's worker) first.
    virtual std::shared_ptr<const WorldSnapshot> snapshot() = 0;
    virtual void restore(const WorldSnapshot &s) = 0;
    // Copy, paste and cut: Simulation::Save of the area between two corners (both in it, kept inside the world),
    // Simulation::Load of a save at block (bx, by) (with a message if some of it was left out), and clear_area
    virtual SaveArea copyArea(int x1, int y1, int x2, int y2, bool includePressure) = 0;
    virtual std::string pasteArea(const SaveArea &s, bool includePressure, int bx, int by) = 0;
    virtual void clearArea(int x, int y, int w, int h) = 0;
    // SaveRenderer::Render: a save drawn as the world is (its display preset and decorations), 0xRRGGBB
    virtual std::vector<uint32_t> renderSave(const SaveArea &s) = 0;

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
    // What is under the cursor (FrameRequest's brushX/brushY), for the interface's text; may be a frame or two old
    virtual UiSample cursorSample() = 0;
    virtual void setDecorations(bool on) = 0; // TPT's decoration display (ctrl+B)
    // The particle at (x, y), an energy particle there or the one in pmap, whichever comes first (the sample tool)
    virtual bool particleAt(int x, int y, bool photonsFirst, ClassicParticle &p) = 0;
    // The signs (Simulation::signs), and what each one's {t}, {p}, {type}... read at its spot now
    virtual std::vector<Sign> signs() = 0;
    virtual void setSigns(const std::vector<Sign> &signs) = 0;
    // (now: read for the signs as they are; otherwise what the last frame read, as the renderer drew them)
    virtual std::vector<SignSample> signSamples(bool now) = 0;
    // The custom Game of Life types (SimulationData::SetCustomGOL)
    virtual void setCustomGol(const std::vector<CustomGol> &types) = 0;
    // The picture as last drawn (TPT's renderer frame, before the cursor), at (x, y): 0xRRGGBB
    virtual uint32_t picturePixel(int x, int y) = 0;

    virtual void cycleDisplay() = 0;
    virtual std::string displayName() const = 0;
    virtual void cyclePasses() {}
    virtual std::string passesName() const { return ""; }

    // The players' keys, as TPT's GameController keeps them: comm and pcomm of STKM and STKM2 (bits 1 left,
    // 2 right, 4 jump, 8 use element; pcomm keeps the last direction).
    virtual void setStickmanKeys(uint8_t comm1, uint8_t pcomm1, uint8_t comm2, uint8_t pcomm2) = 0;
    // Whether STKM2 is out, as of the last particle count (WASD then steer it instead of their other uses).
    virtual bool stickman2Out() const = 0;
};

std::unique_ptr<World> makeGpuWorld(VkContext &ctx, const AppOptions &o, uint32_t W, uint32_t H, const char *mode);
std::unique_ptr<World> makeClassicWorld(VkContext &ctx, const AppOptions &o);

} // namespace bp
