// Big Powder's own world files (.bpw): a whole GPU world, for worlds bigger than TPT's saves allow. They keep what
// TPT's saves keep (particles, walls and fans, air, Newtonian gravity's maps, signs, the simulation's settings,
// stick men) and the tick count, and the engine's own state from tick to tick (what is inside portals, wifi
// channels, electrified walls), so a world continues exactly; all in one zstd stream; particles keep their ids. Elements are named by TPT's
// identifiers, as TPT's saves name them, so files stay readable if element ids change.
#pragma once
#include <cstdint>
#include <string>

namespace bp {

class GpuSim;

struct WorldFileInfo {
    uint32_t W = 0, H = 0;
    uint64_t particles = 0;
};

bool readWorldInfo(const std::string &path, WorldFileInfo &info, std::string &error);
// The world is written as it is after the work submitted so far (waits for the GPU).
bool saveWorld(GpuSim &sim, const std::string &path, std::string &error);
// Replaces sim's world with the file's; sim must have the file's size. message says what was loaded (and what
// was left out: elements this build doesn't know).
bool loadWorld(GpuSim &sim, const std::string &path, std::string &message);

// Writes the (up to TPT-sized) area at (x, y), w x h pixels, rounded out to whole cells, as a TPT save (.cps), to
// open in The Powder Toy: Simulation::Save's particles, walls, air, signs and settings.
bool exportTptArea(GpuSim &sim, int x, int y, int w, int h, const std::string &path, std::string &message);

} // namespace bp
