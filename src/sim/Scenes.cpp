#include "sim/Scenes.h"

#include <algorithm>
#include <random>
#include <stdexcept>

namespace bp {

const std::vector<std::string> &sceneNames() {
    static const std::vector<std::string> names{"empty", "water", "sand", "rain", "mixed"};
    return names;
}

namespace {

struct Builder {
    const ElementTable &elements;
    SceneData &s;
    std::mt19937 rng;

    void put(int x, int y, int type) {
        if (x < CELL || y < CELL || x >= int(s.W) - CELL || y >= int(s.H) - CELL)
            return;
        uint32_t &cell = s.pmap[size_t(y) * s.W + size_t(x)];
        if (cell)
            return;
        Particle p{};
        p.x = float(x);
        p.y = float(y);
        p.temp = elements.elements[type].props.defaultTemp;
        if (type == PT_FIRE)
            p.life = std::uniform_int_distribution<int>(120, 169)(rng);
        uint32_t id = uint32_t(s.ptype.size());
        s.parts.push_back(p);
        s.ptype.push_back(uint32_t(type));
        cell = id + 1;
    }

    void rect(int x0, int y0, int x1, int y1, int type, float density = 1.0f) {
        std::uniform_real_distribution<float> u(0.0f, 1.0f);
        for (int y = y0; y < y1; y++)
            for (int x = x0; x < x1; x++)
                if (density >= 1.0f || u(rng) < density)
                    put(x, y, type);
    }
};

} // namespace

SceneData makeScene(const ElementTable &elements, const std::string &name, uint32_t W, uint32_t H,
                    uint32_t seed, float fill) {
    SceneData s;
    s.W = W;
    s.H = H;
    s.pmap.assign(size_t(W) * H, 0);
    s.parts.reserve(size_t(W) * H);
    s.ptype.reserve(size_t(W) * H);
    Builder b{elements, s, std::mt19937(seed)};

    const int w = int(W), h = int(H);
    const int t = 3;                          // wall thickness
    const int x0 = CELL + t, x1 = w - CELL - t; // interior
    const int y0 = CELL + t, y1 = h - CELL - t;
    const int ih = y1 - y0;
    auto box = [&] {
        b.rect(CELL, CELL, w - CELL, y0, PT_METL);
        b.rect(CELL, y1, w - CELL, h - CELL, PT_METL);
        b.rect(CELL, y0, x0, y1, PT_METL);
        b.rect(x1, y0, w - CELL, y1, PT_METL);
    };

    if (name == "empty") {
        box();
    } else if (name == "water") {
        box();
        int top = y1 - int(float(ih) * fill);
        b.rect(x0, std::max(top, y0), x1, y1, PT_WATR);
    } else if (name == "sand") {
        box();
        int bottom = y0 + int(float(ih) * 0.75f * fill);
        for (int y = y0; y < bottom; y++)
            for (int x = x0; x < x1; x++) {
                float r = std::uniform_real_distribution<float>(0.0f, 1.0f)(b.rng);
                if (r < 0.3f)
                    b.put(x, y, PT_DUST);
                else if (r < 0.55f)
                    b.put(x, y, PT_STNE);
            }
    } else if (name == "rain") {
        box();
        b.rect(x0, y0, x1, y0 + int(float(ih) * 0.6f), PT_WATR, 0.3f * fill);
    } else if (name == "mixed") {
        box();
        int iw = x1 - x0;
        // Water at the bottom with a layer of oil on top, and fire on the oil.
        int waterTop = y1 - int(float(ih) * 0.2f * fill);
        int oilTop = waterTop - int(float(ih) * 0.08f * fill);
        b.rect(x0, waterTop, x1, y1, PT_WATR);
        b.rect(x0, oilTop, x1, waterTop, PT_OIL);
        b.rect(x0, oilTop - 3, x0 + iw / 3, oilTop, PT_FIRE, 0.5f);
        // Metal shelves with dust piles.
        for (int k = 0; k < 4; k++) {
            int sy = y0 + ih * (k + 1) / 7;
            int sx0 = x0 + iw * k / 4 + iw / 40;
            int sx1 = sx0 + iw / 5;
            b.rect(sx0, sy, sx1, sy + 3, PT_METL);
            b.rect(sx0, sy - int(float(ih) * 0.06f * fill), sx1, sy, PT_DUST);
        }
        // Stone falling from the top.
        b.rect(x0, y0, x1, y0 + int(float(ih) * 0.12f), PT_STNE, 0.25f * fill);
    } else {
        throw std::runtime_error("unknown scene " + name);
    }
    return s;
}

} // namespace bp
