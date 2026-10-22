#include "sim/Signs.h"
#include "sim/Elements.h"
#include "sim/RenderTables.h"

#include <algorithm>
#include <cstdio>

namespace bp {

namespace {

std::u32string fromAscii(const std::string &s) { return std::u32string(s.begin(), s.end()); }

// Format::Precision(Format::ShowPoint(v), 2): fixed, two decimals
std::u32string fixed2(float v) {
    char buf[64];
    std::snprintf(buf, sizeof buf, "%.2f", double(v));
    return fromAscii(buf);
}

std::u32string number(int v) { return fromAscii(std::to_string(v)); }

bool isElement(const ElementTable &e, int t) { return t > 0 && t < MAX_ELEMENTS && e.elements[t].existsInTpt; }
bool isElementOrNone(const ElementTable &e, int t) { return t >= 0 && t < MAX_ELEMENTS && e.elements[t].existsInTpt; }

// GOLString.cpp's SerialiseGOLRule
std::u32string golRule(int rule) {
    std::string s = "B";
    for (int i = 1; i < 9; ++i)
        if ((rule >> (i + 8)) & 1)
            s += char('0' + i);
    s += "/S";
    for (int i = 0; i < 9; ++i)
        if ((rule >> i) & 1)
            s += char('0' + i);
    if ((rule >> 17) & 0xF)
        s += "/" + std::to_string(((rule >> 17) & 0xF) + 2);
    return fromAscii(s);
}

// SimulationData::ElementResolve (custom Game of Life rules aren't kept by the GPU engine; they show as rules)
std::u32string resolve(const ElementTable &e, int type, int ctype) {
    if (type == PT_LIFE) {
        if (ctype >= 0 && ctype < int(e.golNames.size()))
            return fromAscii(e.golNames[size_t(ctype)]);
        return golRule(ctype);
    }
    if (type >= 0 && type < MAX_ELEMENTS)
        return fromAscii(e.elements[type].name);
    return U"Empty";
}

// SimulationData::BasicParticleInfo
std::u32string particleInfo(const ElementTable &e, const SignSample &p) {
    if (p.type == PT_LAVA && isElement(e, p.ctype))
        return U"Molten " + resolve(e, p.ctype, -1);
    if ((p.type == PT_PIPE || p.type == PT_PPIP) && isElement(e, p.ctype)) {
        if (p.ctype == PT_LAVA && isElement(e, p.tmp4))
            return resolve(e, p.type, -1) + U" with molten " + resolve(e, p.tmp4, -1);
        return resolve(e, p.type, -1) + U" with " + resolve(e, p.ctype, p.tmp4);
    }
    return resolve(e, p.type, p.ctype);
}

enum SignType { NORMAL, SAVE, THREAD, BUTTON, SEARCH };

// sign::split: link signs {c:id|text}, {t:id|text}, {b|text}, {s:query|text}
std::pair<size_t, SignType> split(const std::u32string &text) {
    size_t pipe = 0;
    if (text.size() >= 4 && text.front() == U'{' && text.back() == U'}') {
        switch (text[1]) {
        case U'c':
        case U't':
            if (text[2] == U':' && (pipe = text.find(U'|', 4)) != std::u32string::npos) {
                for (size_t i = 3; i < pipe; ++i)
                    if (text[i] < U'0' || text[i] > U'9')
                        return {0, NORMAL};
                return {pipe, text[1] == U'c' ? SAVE : THREAD};
            }
            break;
        case U'b':
            if (text[2] == U'|')
                return {2, BUTTON};
            break;
        case U's':
            if (text[2] == U':' && (pipe = text.find(U'|', 3)) != std::u32string::npos)
                return {pipe, SEARCH};
            break;
        }
    }
    return {0, NORMAL};
}

// RasterDrawMethods::TextSize's width
int textWidth(const ElementTable &e, const std::u32string &str) {
    int w = 0, curX = 0;
    for (size_t i = 0; i < str.size(); i++) {
        char32_t ch = str[i];
        if (ch == U'\n') {
            w = std::max(curX, w);
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
            curX += e.font.width(e.renderTables, e.font.glyph(ch));
    }
    return std::max(curX, w);
}

uint32_t packPos(int x, int y) { return (uint32_t(x) & 0xFFFFu) | (uint32_t(y) << 16); }
uint32_t rgba(int r, int g, int b, int a) { return uint32_t(r) | (uint32_t(g) << 8) | (uint32_t(b) << 16) | (uint32_t(a) << 24); }

} // namespace

uint32_t Font::glyph(char32_t ch) const {
    for (;;) {
        size_t offset = 0;
        for (const auto &r : ranges) {
            if (r[0] > uint32_t(ch))
                break;
            if (r[1] >= uint32_t(ch))
                return glyphs[offset + (ch - r[0])];
            offset += r[1] - r[0] + 1;
        }
        if (ch == 0xFFFD)
            return 0;
        ch = 0xFFFD;
    }
}

int Font::width(const std::vector<uint32_t> &tables, uint32_t glyph) const {
    return int((tables[RT_FONT + glyph / 4] >> (8 * (glyph % 4))) & 0xFF);
}

std::u32string signDisplayText(const ElementTable &e, const Sign &s, const SignSample &sample, int &x0, int &y0,
                               int &w, int &h) {
    const std::u32string &text = s.text;
    std::u32string drawable;
    auto si = std::make_pair(size_t(0), NORMAL);
    if (text.find(U'{') == std::u32string::npos)
        drawable = text;
    else {
        si = split(text);
        if (si.first) {
            size_t from = si.first + 1, to = text.size() - 1;
            drawable = from < to ? text.substr(from, to - from) : std::u32string();
        } else {
            const SignSample *part = sample.hasPart ? &sample : nullptr;
            std::u32string remaining = text, formatted;
            for (;;) {
                size_t l = remaining.find(U'{');
                if (l == std::u32string::npos)
                    break;
                std::u32string afterLeft = remaining.substr(l + 1);
                size_t r = afterLeft.find(U'}');
                if (r == std::u32string::npos)
                    break;
                formatted += remaining.substr(0, l);
                remaining = afterLeft.substr(r + 1);
                std::u32string between = afterLeft.substr(0, r);
                std::u32string empty = formatted.empty() ? U"Empty" : U"empty";
                if (between == U"t" || between == U"temp")
                    formatted += fixed2(part ? part->temp - 273.15f : 0.0f);
                else if (between == U"p" || between == U"pres")
                    formatted += fixed2(sample.pressure);
                else if (between == U"a" || between == U"aheat")
                    formatted += fixed2(sample.aheat);
                else if (between == U"type")
                    formatted += part ? particleInfo(e, *part) : empty;
                else if (between == U"ctype")
                    formatted += part ? (isElementOrNone(e, part->ctype) ? resolve(e, part->ctype, -1) : number(part->ctype))
                                      : empty;
                else if (between == U"life")
                    formatted += number(part ? part->life : 0);
                else if (between == U"tmp")
                    formatted += number(part ? part->tmp : 0);
                else if (between == U"tmp2")
                    formatted += number(part ? part->tmp2 : 0);
                else
                    formatted += U"{" + between + U"}";
            }
            drawable = formatted + remaining;
        }
    }
    switch (si.second) {
    case NORMAL: break;
    case SAVE: drawable = U"\bt" + drawable; break;
    case THREAD: drawable = U"\bl" + drawable; break;
    case BUTTON: drawable = U"\bo" + drawable; break;
    case SEARCH: drawable = U"\bu" + drawable; break;
    }
    w = textWidth(e, drawable) + 4;
    h = 15;
    x0 = (s.ju == 2) ? s.x - w : (s.ju == 0) ? s.x : s.x - w / 2;
    y0 = (s.y > 18) ? s.y - 18 : s.y + 4;
    return drawable;
}

SignDraw signDraw(const ElementTable &e, const Sign &s, const SignSample &sample) {
    SignDraw d;
    int x0, y0, w, h;
    std::u32string text = signDisplayText(e, s, sample, x0, y0, w, h);
    d.x0 = x0;
    d.y0 = y0;
    d.x1 = x0 + w + 1;
    d.y1 = y0 + h;
    auto cover = [&](int x, int y, int cw, int ch) {
        d.x0 = std::min(d.x0, x);
        d.y0 = std::min(d.y0, y);
        d.x1 = std::max(d.x1, x + cw);
        d.y1 = std::max(d.y1, y + ch);
    };
    // DrawFilledRect(RectSized({x + 1, y + 1}, {w, h - 1}), black), DrawRect(RectSized({x, y}, {w + 1, h}), C0C0C0)
    d.ops.push_back({SIGN_FILL, packPos(x0 + 1, y0 + 1), packPos(w, h - 1), 0x000000u});
    d.ops.push_back({SIGN_OUTLINE, packPos(x0, y0), packPos(w + 1, h), 0xC0C0C0u});
    // BlendText({x + 3, y + 4}, text, white): TPT's escape codes, then BlendChar per character
    {
        const int ox = x0 + 3, oy = y0 + 4;
        int px = ox, py = oy;
        bool underline = false, invert = false;
        int r = 255, g = 255, b = 255;
        const int alpha = 255;
        for (size_t i = 0; i < text.size(); i++) {
            char32_t ch = text[i];
            if (ch == U'\n') {
                px = ox;
                py += FONT_H;
            } else if (ch == 0x0F) {
                if (text.size() <= i + 3)
                    break;
                r = int(uint8_t(text[i + 1]));
                g = int(uint8_t(text[i + 2]));
                b = int(uint8_t(text[i + 3]));
                i += 3;
            } else if (ch == 0x0E) {
                r = g = b = 255;
            } else if (ch == 0x01) {
                invert = !invert;
                r = 255 - r;
                g = 255 - g;
                b = 255 - b;
            } else if (ch == U'\b') {
                if (text.size() <= i + 1)
                    break;
                bool colourCode = true;
                uint32_t c = 0xFFFFFFFFu;
                switch (text[i + 1]) {
                case U'U': underline = !underline; colourCode = false; break;
                case U'w': c = 0xFFFFFF; break;
                case U'g': c = 0xC0C0C0; break;
                case U'o': c = 0xFFD820; break;
                case U'r': c = 0xFF0000; break;
                case U'l': c = 0xFF4B4B; break;
                case U'b': c = 0x0000FF; break;
                case U't': c = 0x20AAFF; break;
                case U'u': c = 0x9353D3; break;
                }
                if (c != 0xFFFFFFFFu) {
                    r = int(c >> 16);
                    g = int((c >> 8) & 0xFF);
                    b = int(c & 0xFF);
                }
                if (colourCode && invert) {
                    r = 255 - r;
                    g = 255 - g;
                    b = 255 - b;
                }
                i++;
            } else {
                uint32_t glyph = e.font.glyph(ch);
                int dx = e.font.width(e.renderTables, glyph);
                d.ops.push_back({SIGN_GLYPH, packPos(px, py - 2), glyph, rgba(r, g, b, alpha)});
                cover(px, py - 2, dx, FONT_H);
                if (underline) {
                    d.ops.push_back({SIGN_UNDERLINE, packPos(px, py + FONT_H), uint32_t(dx), rgba(r, g, b, alpha)});
                    cover(px, py + FONT_H, dx, 1);
                }
                px += dx;
            }
        }
    }
    // The pointer to the sign's spot
    if (s.ju != 3) {
        int x = s.x, y = s.y, dx = 1 - s.ju, dy = (s.y > 18) ? -1 : 1;
        for (int j = 0; j < 4; j++) {
            d.ops.push_back({SIGN_PIXEL, packPos(x, y), 0, 0xC0C0C0u});
            cover(x, y, 1, 1);
            x += dx;
            y += dy;
        }
    }
    return d;
}

} // namespace bp
