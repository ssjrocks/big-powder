// A piece of a world as TPT's GameSave holds one: what Simulation::Save makes of an area (the clipboard, stamps) and
// what Simulation::Load places. It is laid out plainly so the GPU worlds can make and place them too (sim/GpuSave),
// and has none of TPT's limits (612x384, NPART particles), so it can be as big as a GPU world. The functions here
// are TPT's own code or ports of it, in the TPT glue (TptSave.cpp).
#pragma once
#include "classic/ClassicSim.h"

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace bp {

struct SaveArea {
    int cw = 0, ch = 0;                                          // blockSize, in cells
    int contentX = 0, contentY = 0, contentW = 0, contentH = 0; // blockContent: the cells that are its own
    std::vector<ClassicParticle> particles;                      // from its top-left corner; type 0 is none
    // cw * ch each
    std::vector<uint8_t> walls, blockAir, blockAirH;
    std::vector<float> fanVX, fanVY, pv, vx, vy, hv, gravMass, gravForceX, gravForceY;
    std::vector<uint32_t> gravMask;
    bool hasPressure = false, hasAmbientHeat = false, hasBlockAirMaps = false, hasGravityMaps = false;
    std::vector<Sign> signs;
    // StkmData: the stick men's rocket boots and fans (fighters by their slot)
    bool rocketBoots1 = false, rocketBoots2 = false, fan1 = false, fan2 = false;
    std::vector<uint32_t> rocketBootsFigh, fanFigh;
    std::array<int, 2> version{0, 0}; // the TPT version that wrote it (Load treats old stick men's air differently)
    bool paused = false;
    bool missingElements = false; // it has custom elements TPT doesn't have
    // Simulation::SaveSimOptions (kept with a stamp; Load doesn't use them)
    ClassicOptions options;

    // GameSave::setSize: cw x ch cells, all of them its own, nothing in it
    void setSize(int w, int h);
    size_t cells() const { return size_t(cw) * size_t(ch); }
    bool empty() const { return cw <= 0 || ch <= 0; }
};

// TPT's 2x2 integer matrices (Mat2<int>: {A, B, C, D} acting on column vectors, y pointing down)
using Mat2i = std::array<int, 4>;
constexpr Mat2i MAT_IDENTITY{1, 0, 0, 1}, MAT_MIRROR_X{-1, 0, 0, 1}, MAT_MIRROR_Y{1, 0, 0, -1}, MAT_CCW{0, 1, -1, 0};
constexpr Mat2i matMul(const Mat2i &a, const Mat2i &b) {
    return {a[0] * b[0] + a[1] * b[2], a[0] * b[1] + a[1] * b[3], a[2] * b[0] + a[3] * b[2], a[2] * b[1] + a[3] * b[3]};
}

// GameSave::Transform: m applied to it (positions, velocities, fans, PIPE's directions), moved by a nudge of 0..3
// pixels, and cut to a world of cellsW x cellsH cells (TPT cuts it to its own 153 x 96)
void tptTransform(SaveArea &s, const Mat2i &m, int nudgeX, int nudgeY, int cellsW, int cellsH);
// TPT's own GameSave::Transform (for TPT's world size; what tptTransform is checked against)
void tptTransformTpt(SaveArea &s, const Mat2i &m, int nudgeX, int nudgeY);
// A save file's contents: TPT's OPS or PSv (GameSave's reader), or ours for one bigger than TPT reads
bool tptReadSave(const std::vector<char> &data, SaveArea &out, std::string &error);
// GameSave::Serialise for what TPT can read (153 x 96 cells, NPART particles), ours for bigger
bool tptWriteSave(const SaveArea &s, std::vector<char> &out, std::string &error);
// SaveRenderer::Render: the picture TPT shows of it while it is being placed, drawn as the world is (and with the
// gravity displays, as RenderParams::overlays); (cw*CELL) x (ch*CELL) pixels, 0xRRGGBB. One bigger than TPT's world
// is drawn in pieces of it.
std::vector<uint32_t> tptRenderSave(const SaveArea &s, const ViewSettings &view, uint32_t overlays);
// ThumbnailRendererTask: a save drawn as TPT's stamp browser shows it (TPT's default renderer settings with
// decorations, no fire) and shrunk to fit boxW x boxH with TPT's resampling; its size in w and h
std::vector<uint32_t> tptRenderThumbnail(const SaveArea &s, int boxW, int boxH, int &w, int &h);
// PreviewView's picture of a save: SaveRenderer::Render with fire and the anti-clickbait decoration level, shrunk to
// fit boxW x boxH with TPT's resampling
std::vector<uint32_t> tptRenderPreview(const SaveArea &s, int boxW, int boxH, int &w, int &h);
// GameSave::PressureInTmp3: elements that keep their pressure in tmp3 (cleared when pasted without pressure)
bool tptPressureInTmp3(int type);

} // namespace bp
