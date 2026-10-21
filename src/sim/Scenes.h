// Built-in test scenes, generated on the CPU and uploaded once.
#pragma once
#include "classic/ClassicSim.h"
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

// Scenes: empty, water, sand, rain, mixed, pile, dam, layers, sink. `fill` scales how much material a scene
// starts with.
SceneData makeScene(const ElementTable &elements, const std::string &name, uint32_t W, uint32_t H,
                    uint32_t seed, float fill);
const std::vector<std::string> &sceneNames();

// Builds a GPU scene from particles (e.g. from TPT), placed at (offsetX, offsetY). Particles whose element
// the GPU engine doesn't have yet, or that land outside the play area, are skipped and counted in
// `skipped` (by element id).
SceneData sceneFromParticles(const ElementTable &elements, const std::vector<ClassicParticle> &particles,
                             uint32_t W, uint32_t H, int offsetX, int offsetY,
                             std::vector<int> *skipped = nullptr);

} // namespace bp
