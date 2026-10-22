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
constexpr uint32_t RT_GLYPH_STREAM = 2216; // the stream wall's icon (0xE00D): its glyph's offset in the font
// TPT's font (graphics/FontReader.cpp) from here on, its bytes packed four to a word: each glyph is a width byte,
// then 2-bit pixels (0..3), row by row (FONT_H rows), four to a byte from the low bits up.
constexpr uint32_t RT_FONT = 2224;
constexpr uint32_t RT_SIZE = RT_FONT; // the fixed tables before the font
constexpr int FONT_H = 12;

} // namespace bp
