// TPT's tools on the GPU engine: what gui/game/tool's ElementTool, WallTool, Element_LIGH_Tool, Element_TESC_Tool
// and PlopTool do through Simulation's editing functions (simulation/Editing.cpp), worked out on the CPU call by
// call as TPT makes them.
//   - Particles: which pixels each use reaches and how many times CreatePartFlags runs at each; spawn.slang then
//     runs it there. TPT's order between pixels only decides the new particles' ids.
//   - Walls: TPT's wall functions on the CPU's copy of the walls; the cells they change then go to the GPU.
//   - The simulation tools and the property tool: where SimTool and PropertyTool reach and how often, applied by
//     sim_tool.slang (the cyclone's amounts and MIX's random swaps worked out here).
//   - A flood fill first reads what FloodParts (or flood_prop) checks at every pixel back from the GPU
//     (fill_map.slang), so a list of uses with one runs on its own and waits for the GPU.
#pragma once
#include "classic/TptTools.h"
#include "sim/GpuSim.h"

#include <array>
#include <random>
#include <variant>

namespace bp {

class GpuEditor {
public:
    GpuEditor(VkContext &ctx, GpuSim &sim);

    // Whether the uses need the world read back first (element flood fills)
    static bool needsWorld(const std::vector<ToolUse> &uses);
    // Applies the uses in order, recorded into cmd; or, when needsWorld, each run on the GPU and waited for (cmd isn't
    // used then).
    void apply(VkCommandBuffer cmd, const std::vector<ToolUse> &uses);

private:
    // How often CreatePartFlags runs at each pixel, collected as row spans (a brush stamped along a line reaches
    // each pixel many times)
    class PixelCounts {
    public:
        PixelCounts(int W, int H) : W_(W), H_(H) {}
        void span(int y, int xa, int xb, int times = 1); // clipped to the world
        bool empty() const { return spans_.empty(); }
        PixelEdit edit(int type, uint32_t flags, int selected) const;
        // sim_tool.slang's box of counts (two or sixteen bits each) for a tool, with its three values
        ToolEdit toolEdit(uint32_t tool, int bits, uint32_t p0, uint32_t p1, uint32_t p2 = 0) const;
        // The box's counts packed (two or sixteen bits each, row by row) and where the box is; false if empty
        bool packed(int bits, std::vector<uint32_t> &out, int &x0, int &y0, int &w, int &h) const;
        // The counts added up by air cell: cell, count pairs
        std::vector<uint32_t> cellCounts() const;

    private:
        // Each row of the box with something in it: f(row, the counts along it)
        template <class F>
        void rows(F &&f) const;
        struct Span {
            int y, xa, xb, times;
        };
        std::vector<Span> spans_;
        int W_, H_;
        int x0_ = 1 << 30, y0_ = 1 << 30, x1_ = -1, y1_ = -1;
    };
    struct WallStep {
        std::vector<WallEdit> edits;
        bool gravity = false;
    };
    using Step = std::variant<std::vector<PixelEdit>, WallStep, std::vector<ToolEdit>>;

    void use(const ToolUse &u);
    void useElement(const ToolUse &u);
    void useWall(const ToolUse &u);
    void addPixels(PixelEdit &&e);
    void addTool(ToolEdit &&e);
    void flush(VkCommandBuffer cmd);
    // SimTool and PropertyTool
    void useSim(const ToolUse &u);
    void useProperty(const ToolUse &u);
    std::vector<int> stamps(const ToolUse &u) const; // where Draw and DrawLine stamp the brush, x, y pairs
    void mix(const ToolUse &u);
    void cyclone(const ToolUse &u);
    void floodProperty(int x, int y, const ParticleProperty &p); // Simulation::flood_prop
    // DecorationTool
    void useDeco(const ToolUse &u);
    void decorate(const PixelCounts &counts, uint32_t colour, int mode); // DECO with a table made by TPT's code
    void smudge(const ToolUse &u);
    void decoFill(const ToolUse &u); // Simulation::ApplyDecorationFill, over the world's picture as last drawn

    // Simulation's editing functions, adding to `counts` what CreatePartFlags would be called on
    void stamp(PixelCounts &counts, int x, int y);             // CreateParts' brush loop
    void createLine(PixelCounts &counts, int x1, int y1, int x2, int y2);
    void floodParts(int x, int y, int fullc, int flags, int selected);
    // TPT's span fills from (x, y) over the pixels `bits` lets them onto, within the bounds (inclusive)
    void scanFill(PixelCounts &counts, const std::vector<uint32_t> &bits, int x, int y, int xMin, int xMax, int yMin,
                  int yMax) const;
    // The walls: CreateWalls (with deletions and fans collected), CreateWallBox and FloodWalls, between beginWalls
    // and finishWalls
    void beginWalls();
    void finishWalls();
    WallEdit &wallEdit(uint32_t cell);
    void setWall(uint32_t cell, int wall);
    void createWalls(int x, int y, int rx, int ry, int wall, bool brush);
    void createWallBox(int x1, int y1, int x2, int y2, int wall);
    void floodWalls(int x, int y, int wall, int bm);
    uint8_t &bmap(int x, int y) { return walls_[size_t(y / CELL) * CW_ + size_t(x / CELL)]; }

    VkContext &ctx_;
    GpuSim &sim_;
    const int W_, H_, CW_, CH_;
    std::vector<uint8_t> &walls_;
    TptBrush brush_;
    std::vector<std::vector<std::array<int, 2>>> brushRows_; // the brush's runs of points, row by row
    int64_t lightningRecreate_ = 0;                          // Simulation::lightningRecreate
    // While a wall use runs: what it changes, what it deletes (WL_ERASEALL)
    std::vector<WallEdit> wallEdits_;
    std::vector<int> wallEditAt_; // per cell: its entry in wallEdits_ + 1, or 0
    bool gravityWalls_ = false, signsChanged_ = false;
    std::vector<Sign> signs_;
    PixelCounts erased_;
    std::vector<Step> steps_;
    bool immediate_ = false;
    std::mt19937 rng_; // where MIX mixes (TPT's rng: random either way)
};

} // namespace bp
