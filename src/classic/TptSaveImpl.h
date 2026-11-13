// SaveArea to and from TPT's GameSave, and what the glue shares to use TPT's engine outside a ClassicSim. Only for the
// glue compiled with TPT's settings (TptSave.cpp, ClassicSim.cpp).
#pragma once
#include "classic/TptSave.h"

#include "client/GameSave.h"
#include "graphics/RendererSettings.h"
#include "simulation/SimulationData.h"

#include <memory>

namespace bp {

extern int liveClassicSims; // TPT's SimulationData is a singleton owned by the ClassicSim alive (ClassicSim.cpp)

// TPT's element tables, made for the while if no ClassicSim is alive
struct TptTables {
    std::unique_ptr<SimulationData> temporary;
    TptTables() {
        if (liveClassicSims == 0)
            temporary = std::make_unique<SimulationData>();
    }
};

void toSaveArea(const GameSave &g, SaveArea &a);
// (up to NPART particles: TPT's GameSave holds no more)
std::unique_ptr<GameSave> toGameSave(const SaveArea &a);
// TPT's renderer settings for a view, with the gravity displays (RenderParams::overlays)
RendererSettings viewRendererSettings(const ViewSettings &v, uint32_t overlays);

} // namespace bp
