// TPT's Simulation::Save, Load and clear_area for the GPU worlds: an area of a GpuSim to a SaveArea (the clipboard,
// stamps), a SaveArea pasted into one, and an area cleared, as TPT does them in its own world (classic/TptSave.h).
// Load's decisions are made here one particle after another, as TPT makes them; the GPU then carries them out.
#pragma once
#include "classic/TptSave.h"
#include "sim/GpuSim.h"

#include <map>

namespace bp {

// The area between two corners (both in it, kept inside the world as GameController's SaneSaveRect does), in cells
SaveArea gpuSaveArea(GpuSim &sim, int x1, int y1, int x2, int y2, bool includePressure);

struct PasteResult {
    size_t placed = 0;
    std::map<int, int> leftOut; // elements TPT has that the GPU engine doesn't run yet: how many of each
};
// At block (bx, by)
PasteResult gpuLoadArea(GpuSim &sim, const SaveArea &s, bool includePressure, int bx, int by);

// clear_area: x, y, w, h in pixels
void gpuClearArea(GpuSim &sim, int x, int y, int w, int h);

} // namespace bp
