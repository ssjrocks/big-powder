// TPT's signs (simulation/Sign.cpp, Renderer::DrawSigns), without TPT: their text as TPT shows it, and what the GPU
// renderer draws for them (shaders/signs.slang).
#pragma once
#include <array>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace bp {

struct ElementTable;

// A sign: ju 0 left, 1 middle, 2 right, 3 none (sign::Justification)
struct Sign {
    int x = 0, y = 0, ju = 1;
    std::u32string text;
};

// What a sign's {t}, {p}, {type}... read at its spot: the particle there (photons first, then pmap), the air
struct SignSample {
    bool hasPart = false;
    int type = 0, ctype = 0, life = 0, tmp = 0, tmp2 = 0, tmp4 = 0;
    float temp = 0.0f, pressure = 0.0f, aheat = 0.0f;
};

// TPT's font, as ElementTable carries it (ClassicSim::exportFont): code point ranges and glyph offsets into the
// font data at the end of the render tables
struct Font {
    std::vector<std::array<uint32_t, 2>> ranges;
    std::vector<uint32_t> glyphs;
    uint32_t glyph(char32_t ch) const;                          // FontReader::lookupChar's offset
    int width(const std::vector<uint32_t> &tables, uint32_t glyph) const;
};

// sign::split: where a link sign's text starts (its '|'), 0 if it isn't one, and which kind it is
enum SignType { NORMAL, SAVE, THREAD, BUTTON, SEARCH };
std::pair<size_t, SignType> signSplit(const std::u32string &text);

// sign::getDisplayText: the text to draw and the box (x0, y0, w, h)
std::u32string signDisplayText(const ElementTable &elements, const Sign &s, const SignSample &sample, int &x0, int &y0,
                               int &w, int &h);

// One drawing operation of DrawSigns, in its order: 4 words (shaders/signs.slang)
enum SignOp : uint32_t { SIGN_FILL = 1, SIGN_OUTLINE = 2, SIGN_GLYPH = 3, SIGN_UNDERLINE = 4, SIGN_PIXEL = 5 };
struct SignDraw {
    std::vector<std::array<uint32_t, 4>> ops;
    int x0 = 0, y0 = 0, x1 = 0, y1 = 0; // the area they cover, [x0, x1) x [y0, y1)
};
// DrawSigns for one sign: its box, BlendText of its text (with TPT's escape codes), its pointer
SignDraw signDraw(const ElementTable &elements, const Sign &s, const SignSample &sample);

} // namespace bp
