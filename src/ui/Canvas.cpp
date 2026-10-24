#include "ui/Canvas.h"
#include "ui/Raster.h"
#include "sim/Elements.h"
#include "sim/RenderTables.h"

#include <algorithm>
#include <cstdlib>

namespace bp {

namespace {

uint32_t pack(int r, int g, int b, int a) { return uint32_t(r) | (uint32_t(g) << 8) | (uint32_t(b) << 16) | (uint32_t(a) << 24); }

} // namespace

void Canvas::resize(int w, int h) {
    w_ = std::max(w, 0);
    h_ = std::max(h, 0);
    px_.assign(size_t(w_) * h_, 0);
    resetClip();
}

void Canvas::clear() { std::fill(px_.begin(), px_.end(), 0u); }

void Canvas::setClip(int x, int y, int w, int h) {
    cx0_ = std::max(x, 0);
    cy0_ = std::max(y, 0);
    cx1_ = std::min(x + w, w_);
    cy1_ = std::min(y + h, h_);
}

void Canvas::drawPixel(int x, int y, uint32_t c) {
    if (inClip(x, y))
        px_[size_t(y) * w_ + x] = pack(int(c >> 16) & 0xFF, int(c >> 8) & 0xFF, int(c) & 0xFF, 255);
}

// RGB::Blend over a premultiplied pixel: the colour part blends as TPT blends, and the uncovered part shrinks the
// same way, so compositing over the world afterwards blends the world in the same proportion
void Canvas::blendPixel(int x, int y, Rgba c) {
    if (!inClip(x, y))
        return;
    uint32_t &p = px_[size_t(y) * w_ + x];
    if (c.a == 255) {
        p = pack(c.r, c.g, c.b, 255);
        return;
    }
    int a = c.a, r = int(p & 0xFF), g = int((p >> 8) & 0xFF), b = int((p >> 16) & 0xFF), cov = int(p >> 24);
    r = (a * c.r + (255 - a) * r) / 255;
    g = (a * c.g + (255 - a) * g) / 255;
    b = (a * c.b + (255 - a) * b) / 255;
    cov = a + (255 - a) * cov / 255;
    p = pack(r, g, b, cov);
}

void Canvas::addPixel(int x, int y, Rgba c) {
    if (!inClip(x, y))
        return;
    uint32_t &p = px_[size_t(y) * w_ + x];
    int r = std::min(255, int(p & 0xFF) + c.a * c.r / 255);
    int g = std::min(255, int((p >> 8) & 0xFF) + c.a * c.g / 255);
    int b = std::min(255, int((p >> 16) & 0xFF) + c.a * c.b / 255);
    p = pack(r, g, b, int(p >> 24));
}

void Canvas::drawRect(int x, int y, int w, int h, uint32_t c) {
    rasterizeRect(x, y, w, h, [&](int px, int py) { drawPixel(px, py, c); });
}
void Canvas::blendRect(int x, int y, int w, int h, Rgba c) {
    rasterizeRect(x, y, w, h, [&](int px, int py) { blendPixel(px, py, c); });
}
void Canvas::drawFilledRect(int x, int y, int w, int h, uint32_t c) {
    for (int j = y; j < y + h; j++)
        for (int i = x; i < x + w; i++)
            drawPixel(i, j, c);
}
void Canvas::blendFilledRect(int x, int y, int w, int h, Rgba c) {
    for (int j = y; j < y + h; j++)
        for (int i = x; i < x + w; i++)
            blendPixel(i, j, c);
}
void Canvas::drawLine(int x0, int y0, int x1, int y1, uint32_t c) {
    rasterizeLine(x0, y0, x1, y1, [&](int px, int py) { drawPixel(px, py, c); });
}
void Canvas::blendLine(int x0, int y0, int x1, int y1, Rgba c) {
    rasterizeLine(x0, y0, x1, y1, [&](int px, int py) { blendPixel(px, py, c); });
}

int Canvas::charWidth(char32_t ch) const { return elements_.font.width(elements_.renderTables, elements_.font.glyph(ch)); }

// RasterDrawMethods::BlendChar: the glyph's rows start 2 px above y; each pixel at (glyph level 0..3) * alpha / 3
int Canvas::blendChar(int x, int y, char32_t ch, Rgba c) {
    const auto &t = elements_.renderTables;
    uint32_t glyph = elements_.font.glyph(ch);
    int w = elements_.font.width(t, glyph);
    for (int j = 0; j < FONT_H; j++)
        for (int i = 0; i < w; i++) {
            uint32_t p = uint32_t(j * w + i), at = glyph + 1 + p / 4;
            int level = int((t[RT_FONT + at / 4] >> (8 * (at % 4) + 2 * (p % 4))) & 3u);
            blendPixel(x + i, y - 2 + j, Rgba{c.r, c.g, c.b, uint8_t(level * c.a / 3)});
        }
    return w;
}

int Canvas::addChar(int x, int y, char32_t ch, Rgba c) {
    const auto &t = elements_.renderTables;
    uint32_t glyph = elements_.font.glyph(ch);
    int w = elements_.font.width(t, glyph);
    for (int j = 0; j < FONT_H; j++)
        for (int i = 0; i < w; i++) {
            uint32_t p = uint32_t(j * w + i), at = glyph + 1 + p / 4;
            int level = int((t[RT_FONT + at / 4] >> (8 * (at % 4) + 2 * (p % 4))) & 3u);
            addPixel(x + i, y - 2 + j, Rgba{c.r, c.g, c.b, uint8_t(level * c.a / 3)});
        }
    return w;
}

// RasterDrawMethods::BlendText
void Canvas::blendText(int x0, int y0, const std::u32string &str, Rgba orig, int *endX, int *endY) {
    bool underline = false, invert = false;
    Rgba c = orig;
    int x = x0, y = y0;
    for (size_t i = 0; i < str.size(); i++) {
        char32_t ch = str[i];
        if (ch == U'\n') {
            x = x0;
            y += FONT_H;
        } else if (ch == 0x0F) {
            if (str.size() <= i + 3)
                break;
            c.r = uint8_t(str[i + 1]);
            c.g = uint8_t(str[i + 2]);
            c.b = uint8_t(str[i + 3]);
            i += 3;
        } else if (ch == 0x0E) {
            c = Rgba{orig.r, orig.g, orig.b, c.a};
        } else if (ch == 0x01) {
            invert = !invert;
            c.r = uint8_t(255 - c.r), c.g = uint8_t(255 - c.g), c.b = uint8_t(255 - c.b);
        } else if (ch == U'\b') {
            if (str.size() <= i + 1)
                break;
            bool colourCode = true;
            uint32_t set = 0xFFFFFFFFu;
            switch (str[i + 1]) {
            case U'U': underline = !underline; colourCode = false; break;
            case U'w': set = 0xFFFFFF; break;
            case U'g': set = 0xC0C0C0; break;
            case U'o': set = 0xFFD820; break;
            case U'r': set = 0xFF0000; break;
            case U'l': set = 0xFF4B4B; break;
            case U'b': set = 0x0000FF; break;
            case U't': set = 0x20AAFF; break;
            case U'u': set = 0x9353D3; break;
            }
            if (set != 0xFFFFFFFFu)
                c.r = uint8_t(set >> 16), c.g = uint8_t(set >> 8), c.b = uint8_t(set);
            if (colourCode && invert)
                c.r = uint8_t(255 - c.r), c.g = uint8_t(255 - c.g), c.b = uint8_t(255 - c.b);
            i++;
        } else {
            int dx = blendChar(x, y, ch, Rgba{c.r, c.g, c.b, orig.a});
            if (underline)
                for (int k = 0; k < dx; k++)
                    blendPixel(x + k, y + FONT_H, Rgba{c.r, c.g, c.b, orig.a});
            x += dx;
        }
    }
    if (endX)
        *endX = x - x0;
    if (endY)
        *endY = y - y0;
}

// RasterDrawMethods::TextSize
void Canvas::textSize(const std::u32string &str, int &w, int &h) const {
    w = 0;
    h = FONT_H - 2;
    int curX = 0;
    for (size_t i = 0; i < str.size(); i++) {
        char32_t ch = str[i];
        if (ch == U'\n') {
            w = std::max(curX, w);
            h += FONT_H;
            curX = 0;
        } else if (ch == 0x0F) {
            if (str.size() <= i + 3)
                break;
            i += 3;
        } else if (ch == 0x0E || ch == 0x01)
            continue;
        else if (ch == U'\b') {
            if (str.size() <= i + 1)
                break;
            i++;
        } else
            curX += charWidth(ch);
    }
    w = std::max(curX, w);
}

// Graphics::draw_icon
void Canvas::drawIcon(int x, int y, Icon icon, uint8_t a, bool invert) {
    y--;
    auto bc = [&](int dx, int dy, char32_t ch, uint32_t c) { blendChar(x + dx, y + dy, ch, rgb(c, a)); };
    auto plain = [&](char32_t ch, int dy = 0) { bc(0, dy, ch, invert ? 0x000000 : 0xFFFFFF); };
    switch (icon) {
    case Icon::None: break;
    case Icon::Open: plain(0xE001); break;
    case Icon::Reload: plain(0xE011); break;
    case Icon::Save: plain(0xE002); break;
    case Icon::VoteUp:
        bc(-11, 1, 0xE04B, invert ? 0x006400 : 0x00BB12);
        blendText(x + 2, y + 1, U"Vote", rgb(invert ? 0x006400 : 0x00BB12, a));
        break;
    case Icon::VoteDown: bc(0, 0, 0xE04A, invert ? 0x640A00 : 0xBB2800); break;
    case Icon::Tag: plain(0xE003); break;
    case Icon::New: plain(0xE012); break;
    case Icon::Login: plain(0xE004, 1); break;
    case Icon::SimulationSettings: plain(0xE04F, 1); break;
    case Icon::RenderSettings:
        if (invert) {
            bc(0, 1, 0xE058, 0xFF0000);
            bc(0, 1, 0xE059, 0x00FF00);
            bc(0, 1, 0xE05A, 0x0000FF);
        } else {
            addChar(x, y + 1, 0xE058, rgb(0xFF0000, a));
            addChar(x, y + 1, 0xE059, rgb(0x00FF00, a));
            addChar(x, y + 1, 0xE05A, rgb(0x0000FF, a));
        }
        break;
    case Icon::Pause: plain(0xE010); break;
    case Icon::Favourite: bc(0, 0, 0xE04C, invert ? 0x645020 : 0xC0A040); break;
    case Icon::Report: bc(0, 0, 0xE063, invert ? 0x8C8C00 : 0xFFFF00); break;
    case Icon::Username:
        bc(0, 0, 0xE00B, 0x204080);
        bc(0, 0, 0xE00A, invert ? 0x000000 : 0xFFFFFF);
        break;
    case Icon::Password:
        bc(0, 0, 0xE00C, 0xA09020);
        bc(0, 0, 0xE004, invert ? 0x000000 : 0xFFFFFF);
        break;
    case Icon::Close: bc(0, 0, 0xE02A, invert ? 0x141414 : 0xE6E6E6); break;
    case Icon::Search:
        bc(0, 0, 0xE00E, 0x1E1EB4);
        bc(0, 0, 0xE00F, 0xFFFFFF);
        break;
    case Icon::Delete:
        bc(0, 0, 0xE006, 0x9F2F1F);
        bc(0, 0, 0xE005, invert ? 0x000000 : 0xFFFFFF);
        break;
    case Icon::Add:
        bc(0, 0, 0xE006, 0x209020);
        bc(0, 0, 0xE009, invert ? 0x000000 : 0xFFFFFF);
        break;
    default: break; // (the rest come with the dialogs that use them)
    }
}

std::u32string utf32(const std::string &s) {
    std::u32string out;
    for (size_t i = 0; i < s.size();) {
        unsigned char c = (unsigned char)s[i];
        char32_t cp;
        int n;
        if (c < 0x80)
            cp = c, n = 1;
        else if ((c >> 5) == 6)
            cp = c & 0x1F, n = 2;
        else if ((c >> 4) == 14)
            cp = c & 0x0F, n = 3;
        else
            cp = c & 0x07, n = 4;
        for (int k = 1; k < n && i + k < s.size(); k++)
            cp = (cp << 6) | (char32_t(s[i + k]) & 0x3F);
        out += cp;
        i += size_t(n);
    }
    return out;
}

} // namespace bp
