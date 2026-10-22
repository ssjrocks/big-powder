// The GPU simulation: buffers, compute pipelines and the per-tick command sequence.
#pragma once
#include "classic/ClassicSim.h"
#include "gpu/VkContext.h"
#include "sim/Elements.h"
#include "sim/Scenes.h"

#include <array>
#include <functional>
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
    bool newtonianGravity = false; // TPT's Newtonian gravity (gravity.slang)
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
    int brushX = 0, brushY = 0, brushR = -1;
    // TPT's renderer settings: a display preset (ClassicSim::presetName; 4 is Fire Display, TPT's default),
    // decorations (0 off, 1 on, 2 only black ones) and the heat display's range
    int preset = 4;
    uint32_t decoLevel = 1;
    float hdispMin = 0.0f, hdispMax = 9999.0f;
    uint32_t overlays = 0; // TPT's gravity field display (1, ctrl+G) and gravity zones (2, with the GRAV wall tool)
    uint32_t slot = 0;     // frame in flight (signs read back what they show per slot)
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
    // With a query pool: timestamps at firstQuery (start), +1 (after the colour lists), +3 (after the update
    // passes) and +2 (end).
    void recordTick(VkCommandBuffer cmd, VkQueryPool pool = VK_NULL_HANDLE, uint32_t firstQuery = 0);
    void recordBrush(VkCommandBuffer cmd, const std::vector<BrushOp> &ops);
    // Draws the world image (unless drawWorld is false: already drawn this frame) and shows it on the screen buffer.
    void recordRender(VkCommandBuffer cmd, const RenderParams &rp, bool drawWorld = true);
    // Draws the world image only (TPT's renderer at one pixel per cell), e.g. to compare with TPT's own picture.
    // With a query pool, writes timestamps firstQuery..firstQuery+5: start, after gfx, after the fire buffer, after
    // resolve, after the fire smoothing, end.
    void recordWorldImage(VkCommandBuffer cmd, const RenderParams &rp, VkQueryPool pool = VK_NULL_HANDLE,
                          uint32_t firstQuery = 0);
    std::vector<uint32_t> readWorldImage(); // 0xRRGGBB per cell; waits for the GPU
    void clearRenderAccumulation();         // the fire buffer
    // Sets Newtonian gravity's field in use (e.g. TPT's, to compare drawings). Waits for the GPU.
    void setGravityField(const std::vector<float> &fx, const std::vector<float> &fy);
    void readGravityField(std::vector<float> &fx, std::vector<float> &fy); // the field in use, cell by cell

    // Signs (TPT's, drawn as Renderer::DrawSigns does). Their {t}, {p}, {type}... show what is at their spot,
    // read back a frame or two late; sampleSigns reads it now (waits for the GPU).
    static constexpr size_t SIGN_MAX = 256;
    const std::vector<Sign> &signs() const { return signs_; }
    void setSigns(const std::vector<Sign> &signs);
    void addSigns(const std::vector<Sign> &signs, int offX, int offY); // e.g. from a pasted save
    void sampleSigns();

    // Reallocates the frame buffer if needed. Waits for the device when it does.
    void ensureFrameBuffer(uint32_t w, uint32_t h);
    const Buffer &frameBuffer() const { return frame_; }

    SimStats readStats();

    // Copies all live particles back to the CPU (for comparisons and saving; slow for big worlds).
    std::vector<ClassicParticle> readParticles(std::vector<int> *ids = nullptr);
    // Calls f(id, type, particle) for every live particle in id order, reading a block at a time. Waits for the GPU.
    void forEachParticle(const std::function<void(uint32_t, uint32_t, const Particle &)> &f);
    uint32_t capacity() const { return capacity_; }
    const ElementTable &elementTable() const { return elements_; }

    // For saving a whole world (WorldFile.cpp). Each waits for the GPU.
    void readFans(std::vector<float> &fvx, std::vector<float> &fvy);
    std::vector<uint32_t> readGravityCells();              // gravity.slang's six planes
    void writeGravityCells(const std::vector<uint32_t> &cells);
    std::vector<uint8_t> readStickmen();                   // the players' and fighters' state
    void writeStickmen(const std::vector<uint8_t> &state);
    // What else the engine keeps from tick to tick (what is inside portals, wifi channels and the other counters,
    // walls' electrified state), as its buffers are laid out; ENGINE_STATE_VERSION changes with that layout.
    static constexpr uint32_t ENGINE_STATE_VERSION = 1;
    struct EngineState {
        std::vector<uint32_t> counters, walls;
        std::vector<uint8_t> portals;
    };
    EngineState readEngineState();
    void writeEngineState(const EngineState &state); // ignored unless the sizes match
    void setTick(uint64_t t) { tick_ = t; }
    uint32_t seed() const { return seed_; }
    void setSeed(uint32_t s) { seed_ = s; }

    // Air maps (pressure, velocity x/y, ambient heat), (W/CELL) x (H/CELL) each. Wait for the GPU.
    void readAir(std::vector<float> &pv, std::vector<float> &vx, std::vector<float> &vy, std::vector<float> &hv);
    void writeAir(const std::vector<float> &pv, const std::vector<float> &vx, const std::vector<float> &vy,
                  const std::vector<float> &hv);

    // Pastes particles (e.g. from a TPT save) at an offset, as TPT's Simulation::Load: what was where they go is
    // removed, stacked ones are kept. With `ids` (each particle's id where it came from), SOAP's links are
    // renumbered. Waits for the GPU.
    void insertParticles(const std::vector<ClassicParticle> &particles, int offsetX, int offsetY,
                         const std::vector<int> *ids = nullptr);
    // Copies an air area of (w x h) cells from maps (srcCW wide) at (sx, sy) to cell (cx, cy). Waits for the GPU.
    void stampAir(const std::vector<float> &pv, const std::vector<float> &vx, const std::vector<float> &vy,
                  const std::vector<float> &hv, int srcCW, int sx, int sy, int w, int h, int cx, int cy);

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
    // The players' keys (TPT's comm and pcomm for STKM and STKM2): bits 1 left, 2 right, 4 jump, 8 use element.
    void setStickmanKeys(uint8_t comm1, uint8_t pcomm1, uint8_t comm2, uint8_t pcomm2) {
        uint32_t k = uint32_t(comm1) | uint32_t(pcomm1) << 8 | uint32_t(comm2) << 16 | uint32_t(pcomm2) << 24;
        if (k != stkmKeys_) {
            stkmKeys_ = k;
            paramsDirty_ = true;
        }
    }
    const AirSettings &air() const { return air_; }
    uint32_t colourNX() const { return colourNX_; }
    uint32_t colourNY() const { return colourNY_; }
    uint32_t width() const { return W_; }
    uint32_t height() const { return H_; }
    uint64_t tick() const { return tick_; }

private:
    void createPipelines();
    VkPipeline makePipeline(const char *name);
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
    uint32_t stkmKeys_ = 0;
    std::vector<uint8_t> bmap_; // CPU copy of the wall types
    uint32_t passOrder_;
    bool debugCounters_ = false;
    bool serial_ = false;
    std::vector<uint32_t> fixedOrder_;
    uint64_t tick_ = 0;
    std::mt19937 rng_;
    std::minstd_rand stackRng_;       // CheckStacking's 1 tick in 10
    // Newtonian gravity (gravity.slang): the FFT grid's size, whether its arrays exist and hold the kernel's spectra,
    // and whether the mask (enclosures of GRAV walls) needs remaking from the walls
    uint32_t gravNX_ = 0, gravNY_ = 0;
    bool gravReady_ = false, gravMaskDirty_ = true;
    void ensureGravity();
    std::vector<uint32_t> gravityMask() const;
    void recordGravityMask(VkCommandBuffer cmd);
    void recordSigns(VkCommandBuffer cmd, uint32_t slot);
    void recordSignProbe(VkCommandBuffer cmd, uint32_t slot);
    void readSignProbe(uint32_t slot);
    void dispatchGravity(VkCommandBuffer cmd, uint32_t stage, uint32_t groups);
    bool forceStackingCheck_ = false; // after pasting a save, as TPT's Load

    Buffer parts_, ptype_, pmap_, elems_, canMove_, lists_, counters_, airBuf_, frame_, freelist_, stats_, inbox_,
        walls_, params_, emapReqs_, defaults_, photons_, portals_, stickmen_, stacking_,
        renderTables_, video_, gfx_, fragHeads_, frags_, fireBuf_, persistVideo_, gfxFlags_, gravCells_, gravFft_,
        gravDraw_, signData_, mips_;
    static constexpr uint32_t MIP_LEVELS = 4; // zooming out to 1:16 (render.slang, mip.slang)
    static constexpr uint32_t PROBE_SLOTS = 4;
    Buffer signProbe_[PROBE_SLOTS];
    uint64_t probeVersion_[PROBE_SLOTS] = {}; // the signs' version each slot's read-back belongs to (0: none)
    std::vector<Sign> signs_;
    std::vector<SignSample> signSamples_;
    uint64_t signsVersion_ = 1;

    VkDescriptorSetLayout setLayout_ = VK_NULL_HANDLE;
    VkPipelineLayout pipeLayout_ = VK_NULL_HANDLE;
    VkDescriptorPool descPool_ = VK_NULL_HANDLE;
    VkDescriptorSet set_ = VK_NULL_HANDLE;
    VkPipeline pLove_, pGol_, pWallsPre_, pEmapFill_, pDeferred_, pRays_, pEmp_, pInsert_, pAir_, pPrepare_, pArgs_, pUpdate_, pFreelist_, pSpawn_, pRender_, pStatsParts_, pStatsCells_, pStickmen_, pStacking_, pGfx_, pFire_, pResolve_, pHdisp_, pResolveHeavy_, pFireSmooth_, pGravity_, pLens_, pStreams_, pSigns_, pMip_;
};

} // namespace bp
