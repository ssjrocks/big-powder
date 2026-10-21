#include "sim/Scenes.h"

#include <algorithm>
#include <random>
#include <stdexcept>

namespace bp {

const std::vector<std::string> &sceneNames() {
    static const std::vector<std::string> names{"empty", "water", "sand", "rain", "mixed",
                                                "pile", "dam", "layers", "sink", "walls", "detector", "open"};
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

    // Walls in CELL units, [cx0, cx1) x [cy0, cy1).
    void wallRect(int cx0, int cy0, int cx1, int cy1, uint8_t wall) {
        int cw = int(s.W) / CELL, ch = int(s.H) / CELL;
        if (s.walls.empty())
            s.walls.assign(size_t(cw) * ch, 0);
        for (int y = std::max(cy0, 0); y < std::min(cy1, ch); y++)
            for (int x = std::max(cx0, 0); x < std::min(cx1, cw); x++)
                s.walls[size_t(y) * cw + x] = wall;
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
    } else if (name == "pile") {
        // A column of dust falling onto the floor: tests the angle of repose.
        box();
        int iw = x1 - x0;
        b.rect(x0 + iw / 2 - iw / 16, y0, x0 + iw / 2 + iw / 16, y0 + int(float(ih) * 0.6f * fill), PT_DUST);
    } else if (name == "dam") {
        // A block of water released into an empty tank: tests how liquids spread and level out.
        box();
        int iw = x1 - x0;
        b.rect(x0, y0 + ih / 4, x0 + int(float(iw) * 0.3f * fill), y1, PT_WATR);
    } else if (name == "layers") {
        // Water on top of oil: the lighter oil has to rise through the water.
        box();
        int mid = y0 + ih / 2;
        b.rect(x0, mid - int(float(ih) * 0.25f * fill), x1, mid, PT_WATR);
        b.rect(x0, mid, x1, mid + int(float(ih) * 0.25f * fill), PT_OIL);
    } else if (name == "sink") {
        // Stone dropped into a pool of water: tests powders swapping through liquids.
        box();
        int iw = x1 - x0;
        b.rect(x0, y1 - int(float(ih) * 0.4f), x1, y1, PT_WATR);
        b.rect(x0 + iw / 4, y0, x0 + 3 * iw / 4, y0 + int(float(ih) * 0.15f * fill), PT_STNE);
    } else if (name == "walls") {
        // A tank of solid wall, a wall shelf, a liquid-only filter holding back sand while water passes, and a
        // strip of absorbing wall that eats whatever lands on it.
        int cw = w / CELL, ch = h / CELL;
        b.wallRect(1, 1, cw - 1, 3, WL_WALL);
        b.wallRect(1, ch - 3, cw - 1, ch - 1, WL_WALL);
        b.wallRect(1, 1, 3, ch - 1, WL_WALL);
        b.wallRect(cw - 3, 1, cw - 1, ch - 1, WL_WALL);
        b.wallRect(cw / 8, ch / 3, cw / 2, ch / 3 + 2, WL_WALL);
        b.wallRect(cw / 2, ch / 2, cw - 3, ch / 2 + 1, WL_ALLOWLIQUID);
        b.wallRect(cw * 3 / 4, ch - 5, cw - 3, ch - 3, WL_DESTROYALL);
        b.rect(w / 8 * 1 + CELL * 4, CELL * 4, w / 2 - CELL, h / 3 - CELL, PT_DUST, 0.6f * fill);
        b.rect(w / 2 + CELL * 2, CELL * 4, w - CELL * 4, h / 2 - CELL * 2, PT_WATR, 0.5f * fill);
        b.rect(w / 2 + CELL * 2, h / 2 - CELL * 12, w - CELL * 4, h / 2 - CELL * 2, PT_STNE, 0.5f * fill);
    } else if (name == "open") {
        // No container: tests the edge modes (void, solid, loop) and the gravity modes.
        b.rect(w / 3, h / 6, w / 2, h / 2, PT_WATR, 0.8f * fill);
        b.rect(w / 2, h / 6, 2 * w / 3, h / 2, PT_DUST, 0.8f * fill);
    } else if (name == "detector") {
        // Water drips into a detector wall wired to an E-wall floor holding sand: when the detector fires,
        // the E-wall opens for a while and the sand falls through.
        box();
        int cw = w / CELL, ch = h / CELL;
        int fy = ch / 2;
        b.wallRect(cw / 4, fy, cw * 3 / 4, fy + 1, WL_EWALL);        // gate
        b.wallRect(cw / 4 - 1, fy - 6, cw / 4, fy + 1, WL_WALLELEC); // wire up the left side
        b.wallRect(cw / 4 - 4, fy - 6, cw / 4 - 1, fy - 5, WL_WALLELEC);
        b.wallRect(cw / 4 - 5, fy - 6, cw / 4 - 4, fy - 5, WL_DETECT);
        b.rect(cw / 4 * CELL, CELL * 6, cw * 3 / 4 * CELL, fy * CELL, PT_DUST, 0.7f * fill);
        b.rect((cw / 4 - 5) * CELL, CELL * 6, (cw / 4 - 4) * CELL, CELL * 30, PT_WATR, 0.5f);
    } else {
        throw std::runtime_error("unknown scene " + name);
    }
    return s;
}

SceneData sceneFromParticles(const ElementTable &elements, const std::vector<ClassicParticle> &particles,
                             uint32_t W, uint32_t H, int offsetX, int offsetY, std::vector<int> *skipped) {
    SceneData s;
    s.W = W;
    s.H = H;
    s.pmap.assign(size_t(W) * H, 0);
    if (skipped)
        skipped->assign(MAX_ELEMENTS, 0);
    for (const ClassicParticle &cp : particles) {
        float fx = cp.x + float(offsetX), fy = cp.y + float(offsetY);
        int x = int(fx + 0.5f), y = int(fy + 0.5f);
        bool inside = x >= CELL && y >= CELL && x < int(W) - CELL && y < int(H) - CELL;
        if (!elements.enabled(cp.type) || !inside) {
            if (skipped && cp.type >= 0 && cp.type < MAX_ELEMENTS)
                (*skipped)[cp.type]++;
            continue;
        }
        Particle p{};
        p.life = cp.life;
        p.ctype = cp.ctype;
        p.x = fx;
        p.y = fy;
        p.vx = cp.vx;
        p.vy = cp.vy;
        p.temp = cp.temp;
        p.tmp = cp.tmp;
        p.tmp2 = cp.tmp2;
        p.tmp3 = cp.tmp3;
        p.tmp4 = cp.tmp4;
        p.flags = cp.flags;
        p.dcolour = cp.dcolour;
        uint32_t id = uint32_t(s.ptype.size());
        s.parts.push_back(p);
        s.ptype.push_back(uint32_t(cp.type));
        // Later particles win a shared cell, as in TPT's pmap; the GPU rebuilds pmap every tick anyway.
        s.pmap[size_t(y) * W + size_t(x)] = id + 1;
    }
    return s;
}

} // namespace bp
