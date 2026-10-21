// Built-in test scenes, generated on the CPU and uploaded once.
#pragma once
#include "sim/Elements.h"

#include <string>
#include <vector>

namespace bp {

struct SceneData {
    uint32_t W = 0, H = 0;
    std::vector<Particle> parts;  // indexed by particle id
    std::vector<uint32_t> ptype;  // indexed by particle id
    std::vector<uint32_t> pmap;   // W*H, 0 or id+1
};

// Scenes: empty, water, sand, rain, mixed. `fill` scales how much material a scene starts with.
SceneData makeScene(const ElementTable &elements, const std::string &name, uint32_t W, uint32_t H,
                    uint32_t seed, float fill);
const std::vector<std::string> &sceneNames();

} // namespace bp
