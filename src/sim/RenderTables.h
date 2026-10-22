#pragma once
#include <cstdint>

namespace bp {

// Renderer tables (shaders/render_common.slang), exported from TPT by ClassicSim::renderTables: colours packed as
// 0xRRGGBB.
constexpr uint32_t RT_FLAME = 0;          // Renderer::flameTable (200)
constexpr uint32_t RT_PLASMA = 200;       // plasmaTable (200)
constexpr uint32_t RT_HEAT = 400;         // heatTable (1024)
constexpr uint32_t RT_CLFM = 1424;        // clfmTable (200)
constexpr uint32_t RT_FIRW = 1624;        // firwTable (200)
constexpr uint32_t RT_FIRE_ALPHA = 1824;  // fire_alpha[12][12]
constexpr uint32_t RT_GOL_COLOUR = 1968;  // builtinGol colour, colour2, ruleset (32 each)
constexpr uint32_t RT_GOL_COLOUR2 = 2000;
constexpr uint32_t RT_GOL_RULESET = 2032;
constexpr uint32_t RT_WALL_COLOUR = 2064; // wtypes colour, eglow, drawstyle (32 each)
constexpr uint32_t RT_WALL_EGLOW = 2096;
constexpr uint32_t RT_WALL_STYLE = 2128;
constexpr uint32_t RT_TRON = 2160;        // TRON's 32 colours
constexpr uint32_t RT_SLCN = 2192;        // SLCN's 16 colours
constexpr uint32_t RT_LEAF = 2208;        // PLNT's 8 leaf colours
constexpr uint32_t RT_SIZE = 2216;

} // namespace bp
