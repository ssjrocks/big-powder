#include "sim/Scenes.h"

#include <algorithm>
#include <random>
#include <stdexcept>

namespace bp {

const std::vector<std::string> &sceneNames() {
    static const std::vector<std::string> names{"empty", "water", "sand", "rain", "mixed", "pile",
                                                "dam",   "layers", "sink", "walls", "detector", "open", "boil",
                                                "plants", "sponge", "coal", "lava", "heat", "thermite",
                                                "chem", "neutral", "rooms", "circuit", "gasfire"};
    return names;
}

namespace {

struct Builder {
    const ElementTable &elements;
    SceneData &s;
    std::mt19937 rng;

    void put(int x, int y, int type, float temp = -1.0f) {
        if (x < CELL || y < CELL || x >= int(s.W) - CELL || y >= int(s.H) - CELL)
            return;
        uint32_t &cell = s.pmap[size_t(y) * s.W + size_t(x)];
        if (cell)
            return;
        Particle p = elements.defaults[type];
        p.x = float(x);
        p.y = float(y);
        if (temp >= 0.0f)
            p.temp = temp;
        if (type == PT_FIRE)
            p.life = std::uniform_int_distribution<int>(120, 169)(rng);
        if (type == PT_LAVA)
            p.life = std::uniform_int_distribution<int>(240, 359)(rng);
        if (type == PT_SEED) {
            // SEED's Create: colour genes, growth direction up (vertical gravity), random growth program.
            p.ctype = 0xEC0;
            std::uniform_int_distribution<int> bits(0, (1 << 15) - 1);
            p.tmp = bits(rng);
            p.tmp2 = bits(rng);
            p.tmp3 = bits(rng);
            p.tmp4 = bits(rng);
        }
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

    void rect(int x0, int y0, int x1, int y1, int type, float density = 1.0f, float temp = -1.0f) {
        std::uniform_real_distribution<float> u(0.0f, 1.0f);
        for (int y = y0; y < y1; y++)
            for (int x = x0; x < x1; x++)
                if (density >= 1.0f || u(rng) < density)
                    put(x, y, type, temp);
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
    } else if (name == "boil") {
        // Water family: water boils off a red-hot plate into steam, which rises and condenses or freezes;
        // an ice block melts, salt dissolves into salt water, snow on a shelf melts.
        box();
        int iw = x1 - x0;
        b.rect(x0, y1 - 12, x0 + iw * 2 / 3, y1, PT_METL, 1.0f, 1200.0f);
        b.rect(x0, y1 - int(float(ih) * 0.35f * fill), x0 + iw * 2 / 3, y1 - 12, PT_WATR);
        b.rect(x0 + iw * 2 / 3, y1 - int(float(ih) * 0.3f), x1, y1, PT_ICEI, 1.0f, 230.0f);
        b.rect(x0 + iw / 3, y1 - int(float(ih) * 0.45f), x0 + iw / 2, y1 - int(float(ih) * 0.4f), PT_SALT, 0.5f);
        b.rect(x0 + iw / 10, y0 + ih / 4, x0 + iw / 3, y0 + ih / 4 + 3, PT_METL);
        b.rect(x0 + iw / 10, y0 + ih / 4 - 20, x0 + iw / 3, y0 + ih / 4, PT_SNOW, 0.7f);
    } else if (name == "plants") {
        // Seeds on sand under a light rain grow into trees; PLNT spreads through a pool of water; a VINE
        // climbs a WOOD pole.
        box();
        int iw = x1 - x0;
        int ground = y1 - 30;
        b.rect(x0, ground, x1, y1, PT_SAND);
        for (int x = x0 + 10; x < x0 + iw / 2; x += 16)
            b.put(x, ground - 1, PT_SEED);
        b.rect(x0, y0 + ih / 4, x0 + iw / 2, y0 + ih / 4 + 20, PT_WATR, 0.08f * fill);
        int pool = x0 + iw * 3 / 5;
        b.rect(pool, ground - 25, x1, ground, PT_WATR);
        b.rect(pool - 3, ground - 25, pool, ground, PT_PLNT);
        int pole = x0 + iw / 2 + 20;
        b.rect(pole, y0 + ih / 3, pole + 3, ground, PT_WOOD);
        b.rect(pole + 3, ground - 3, pole + 5, ground, PT_VINE);
    } else if (name == "sponge") {
        // Sponges soak up rain, share the water, and one on a hot plate gives it off as steam; a plant patch
        // breathes in CO2 and gives out O2.
        box();
        int iw = x1 - x0;
        b.rect(x0 + iw / 20, y1 - 40, x0 + iw / 4, y1, PT_SPNG);
        b.rect(x0 + iw / 20, y0 + ih / 6, x0 + iw / 4, y0 + ih / 6 + 40, PT_WATR, 0.3f * fill);
        int hx = x0 + iw * 7 / 20;
        b.rect(hx, y1 - 8, hx + iw / 5, y1, PT_METL, 1.0f, 600.0f);
        b.rect(hx, y1 - 30, hx + iw / 5, y1 - 8, PT_SPNG);
        b.rect(hx, y0 + ih / 6, hx + iw / 5, y0 + ih / 6 + 40, PT_WATR, 0.3f * fill);
        int px = x0 + iw * 7 / 10;
        b.rect(px, y1 - 10, x1, y1, PT_PLNT);
        b.rect(px, y1 - 60, x1, y1 - 10, PT_CO2, 0.3f * fill);
    } else if (name == "coal") {
        // Two sealed rooms: fire fed by O2 sets a coal pile smouldering; wood on a hot plate chars and burns.
        box();
        int iw = x1 - x0;
        int mid = x0 + iw / 2;
        b.rect(mid - 2, y0, mid + 2, y1, PT_METL);
        int cx = x0 + iw / 8;
        b.rect(cx, y1 - 30, mid - iw / 8, y1, PT_COAL);
        b.rect(cx, y1 - 34, mid - iw / 8, y1 - 30, PT_FIRE, 0.5f);
        b.rect(cx, y1 - 80, mid - iw / 8, y1 - 40, PT_O2, 0.3f * fill);
        int hx = mid + iw / 8;
        b.rect(hx, y1 - 8, x1 - iw / 8, y1, PT_METL, 1.0f, 1100.0f);
        b.rect(hx, y1 - 28, x1 - iw / 8, y1 - 8, PT_WOOD);
    } else if (name == "lava") {
        // Inside a wall box (a METL box would melt): lava poured onto a metal shelf over sand; thermite burning
        // into an iron block; water poured onto a lava pool; iron rusting in salt water next to gold.
        int cw = w / CELL, ch = h / CELL;
        b.wallRect(1, 1, cw - 1, 3, WL_WALL);
        b.wallRect(1, ch - 3, cw - 1, ch - 1, WL_WALL);
        b.wallRect(1, 1, 3, ch - 1, WL_WALL);
        b.wallRect(cw - 3, 1, cw - 1, ch - 1, WL_WALL);
        const int lx0 = 3 * CELL, lx1 = w - 3 * CELL, ly0 = 3 * CELL, ly1 = h - 3 * CELL;
        const int iw = lx1 - lx0, lh = ly1 - ly0;
        b.rect(lx0, ly1 - 20, lx0 + iw * 3 / 10, ly1, PT_SAND);
        b.rect(lx0 + iw / 10, ly0 + lh / 2, lx0 + iw / 4, ly0 + lh / 2 + 4, PT_METL);
        b.rect(lx0 + iw / 10, ly0 + lh / 4, lx0 + iw / 4, ly0 + lh / 4 + int(30 * fill), PT_LAVA);
        int tx = lx0 + iw * 7 / 20;
        b.rect(tx, ly1 - 30, tx + iw / 6, ly1, PT_IRON);
        b.rect(tx, ly1 - 40, tx + iw / 6, ly1 - 30, PT_THRM);
        b.rect(tx, ly1 - 43, tx + iw / 6, ly1 - 40, PT_FIRE, 0.5f);
        int wx = lx0 + iw * 6 / 10;
        b.rect(wx, ly1 - 25, wx + iw / 6, ly1, PT_LAVA);
        b.rect(wx, ly0 + lh / 3, wx + iw / 6, ly0 + lh / 3 + int(20 * fill), PT_WATR, 0.5f);
        int rx = lx0 + iw * 17 / 20;
        b.rect(rx, ly1 - 30, lx1, ly1, PT_SLTW);
        b.rect(rx + 5, ly1 - 25, rx + 15, ly1, PT_IRON);
        b.rect(rx + 25, ly1 - 25, rx + 28, ly1, PT_GOLD);
    } else if (name == "gasfire") {
        // A thin strip of GAS above a short line of FIRE, inside walls.
        int cw = w / CELL, ch = h / CELL;
        b.wallRect(1, 1, cw - 1, 3, WL_WALL);
        b.wallRect(1, ch - 3, cw - 1, ch - 1, WL_WALL);
        b.wallRect(1, 1, 3, ch - 1, WL_WALL);
        b.wallRect(cw - 3, 1, cw - 1, ch - 1, WL_WALL);
        b.rect(w / 4, h / 2 - 18, w * 3 / 4, h / 2, PT_GAS, 0.4f);
        b.rect(w / 4, h / 2, w / 4 + 20, h / 2 + 2, PT_FIRE);
    } else if (name == "circuit") {
        // Four strips divided by walls, each powered by a battery: a METL wire into water ending in IRON
        // (electrolysis), with GAS above it that sparks ignite; PSCN -> NSCN -> METL -> SWCH -> METL; a long
        // INST line; a WireWorld loop fed by PSCN.
        int cw = w / CELL, ch = h / CELL;
        b.wallRect(1, 1, cw - 1, 3, WL_WALL);
        b.wallRect(1, ch - 3, cw - 1, ch - 1, WL_WALL);
        b.wallRect(1, 1, 3, ch - 1, WL_WALL);
        b.wallRect(cw - 3, 1, cw - 1, ch - 1, WL_WALL);
        int strip = (ch - 6) / 4;
        for (int k = 1; k < 4; k++)
            b.wallRect(3, 3 + k * strip, cw - 3, 3 + k * strip + 1, WL_WALL);
        const int lx0 = 3 * CELL + 4;
        auto sy = [&](int k) { return (3 + k * strip + strip / 2) * CELL; };
        // Strip 0
        int y = sy(0);
        b.rect(lx0, y - 2, lx0 + 4, y + 2, PT_BTRY);
        b.rect(lx0 + 4, y, lx0 + 300, y + 1, PT_METL);
        b.rect(lx0 + 300, y, lx0 + 301, y + 30, PT_METL);
        b.rect(lx0 + 299, y + 30, lx0 + 302, y + 34, PT_IRON);
        b.rect(lx0 + 270, y + 20, lx0 + 340, y + 40, PT_WATR);
        b.rect(lx0 + 120, y - 20, lx0 + 180, y - 2, PT_GAS, 0.4f);
        // Strip 1
        y = sy(1);
        b.rect(lx0, y - 2, lx0 + 4, y + 2, PT_BTRY);
        b.rect(lx0 + 4, y, lx0 + 20, y + 1, PT_PSCN);
        b.rect(lx0 + 20, y, lx0 + 40, y + 1, PT_NSCN);
        b.rect(lx0 + 40, y, lx0 + 160, y + 1, PT_METL);
        b.rect(lx0 + 160, y - 1, lx0 + 180, y + 2, PT_SWCH);
        b.rect(lx0 + 180, y, lx0 + 400, y + 1, PT_METL);
        b.rect(lx0 + 150, y - 12, lx0 + 152, y - 1, PT_PSCN); // turns the switch on when sparked
        b.rect(lx0 + 150, y - 14, lx0 + 152, y - 12, PT_BTRY);
        // Strip 2
        y = sy(2);
        b.rect(lx0, y - 2, lx0 + 4, y + 2, PT_BTRY);
        b.rect(lx0 + 4, y, lx0 + 10, y + 1, PT_PSCN);
        b.rect(lx0 + 10, y, lx0 + 400, y + 1, PT_INST);
        b.rect(lx0 + 400, y, lx0 + 410, y + 1, PT_NSCN);
        b.rect(lx0 + 410, y, lx0 + 500, y + 1, PT_METL);
        // Strip 3: a WireWorld loop
        y = sy(3);
        b.rect(lx0, y - 2, lx0 + 4, y + 2, PT_BTRY);
        b.rect(lx0 + 4, y, lx0 + 8, y + 1, PT_PSCN);
        b.rect(lx0 + 8, y - 6, lx0 + 300, y - 5, PT_WIRE);
        b.rect(lx0 + 8, y + 6, lx0 + 300, y + 7, PT_WIRE);
        b.rect(lx0 + 8, y - 5, lx0 + 9, y + 6, PT_WIRE);
        b.rect(lx0 + 299, y - 5, lx0 + 300, y + 6, PT_WIRE);
    } else if (name == "rooms") {
        // Five walled rooms: GEL soaking rain next to a sponge; mercury on a hot plate (it spreads out as it
        // heats); FRZZ dropped into water (freeze water, then ice); a quartz crystal growing in salt water;
        // virus on sand next to warm yeast.
        int cw = w / CELL, ch = h / CELL;
        b.wallRect(1, 1, cw - 1, 3, WL_WALL);
        b.wallRect(1, ch - 3, cw - 1, ch - 1, WL_WALL);
        b.wallRect(1, 1, 3, ch - 1, WL_WALL);
        b.wallRect(cw - 3, 1, cw - 1, ch - 1, WL_WALL);
        const int rooms = 5;
        int room = (cw - 6) / rooms;
        for (int k = 1; k < rooms; k++)
            b.wallRect(3 + k * room, 3, 3 + k * room + 1, ch - 3, WL_WALL);
        const int ly0 = 3 * CELL, ly1 = h - 3 * CELL, lh = ly1 - ly0;
        auto rx0 = [&](int k) { return (3 + k * room + (k ? 1 : 0)) * CELL; };
        auto rx1 = [&](int k) { return (3 + (k + 1) * room) * CELL; };
        int mid = (rx0(0) + rx1(0)) / 2;
        b.rect(rx0(0), ly1 - 20, mid, ly1, PT_GEL);
        b.rect(mid, ly1 - 20, rx1(0), ly1, PT_SPNG);
        b.rect(rx0(0), ly0 + lh / 4, rx1(0), ly0 + lh / 4 + int(30 * fill), PT_WATR, 0.4f);
        b.rect(rx0(1), ly1 - 6, rx1(1), ly1, PT_METL, 1.0f, 700.0f);
        b.rect(rx0(1) + 10, ly1 - 30, rx1(1) - 10, ly1 - 6, PT_MERC);
        b.rect(rx0(2), ly1 - 40, rx1(2), ly1, PT_WATR);
        b.rect(rx0(2) + 20, ly0 + lh / 3, rx1(2) - 20, ly0 + lh / 3 + 10, PT_FRZZ, 0.5f);
        b.rect((rx0(3) + rx1(3)) / 2 - 3, ly1 - 6, (rx0(3) + rx1(3)) / 2 + 3, ly1, PT_QRTZ);
        b.rect(rx0(3), ly1 - 50, rx1(3), ly1, PT_SLTW);
        b.rect(rx0(4), ly1 - 15, rx1(4), ly1, PT_SAND);
        b.rect(rx0(4) + 5, ly1 - 25, rx0(4) + 10, ly1 - 15, PT_VIRS);
        b.rect(rx1(4) - 30, ly1 - 25, rx1(4) - 5, ly1 - 15, PT_YEST, 0.6f, 310.0f);
    } else if (name == "neutral") {
        // Base poured into a pool of acid: they neutralise each other into salt water. (Walls, not METL: acid
        // and base would eat a metal box.)
        int cw = w / CELL, ch = h / CELL;
        b.wallRect(1, 1, cw - 1, 3, WL_WALL);
        b.wallRect(1, ch - 3, cw - 1, ch - 1, WL_WALL);
        b.wallRect(1, 1, 3, ch - 1, WL_WALL);
        b.wallRect(cw - 3, 1, cw - 1, ch - 1, WL_WALL);
        const int lx0 = 3 * CELL, lx1 = w - 3 * CELL, ly0 = 3 * CELL, ly1 = h - 3 * CELL;
        const int iw = lx1 - lx0, lh = ly1 - ly0;
        b.rect(lx0 + iw / 4, ly1 - 40, lx0 + iw * 3 / 4, ly1, PT_ACID);
        b.rect(lx0 + iw / 3, ly0 + lh / 4, lx0 + iw * 2 / 3, ly0 + lh / 4 + int(30 * fill), PT_BASE, 0.6f);
    } else if (name == "chem") {
        // Four rooms divided by walls: acid poured onto stone; base poured into acid (salt water); CO2 over
        // water (carbonated water, which fizzes); hydrogen and oxygen lit by fire.
        int cw = w / CELL, ch = h / CELL;
        b.wallRect(1, 1, cw - 1, 3, WL_WALL);
        b.wallRect(1, ch - 3, cw - 1, ch - 1, WL_WALL);
        b.wallRect(1, 1, 3, ch - 1, WL_WALL);
        b.wallRect(cw - 3, 1, cw - 1, ch - 1, WL_WALL);
        const int rooms = 4;
        int room = (cw - 6) / rooms; // in cells
        for (int k = 1; k < rooms; k++)
            b.wallRect(3 + k * room, 3, 3 + k * room + 1, ch - 3, WL_WALL);
        const int ly0 = 3 * CELL, ly1 = h - 3 * CELL, lh = ly1 - ly0;
        auto rx0 = [&](int k) { return (3 + k * room + (k ? 1 : 0)) * CELL; };
        auto rx1 = [&](int k) { return (3 + (k + 1) * room) * CELL; };
        b.rect(rx0(0), ly1 - 20, rx1(0), ly1, PT_STNE);
        b.rect(rx0(0), ly0 + lh / 3, rx1(0), ly0 + lh / 3 + int(25 * fill), PT_ACID, 0.6f);
        b.rect(rx0(1), ly1 - 25, rx1(1), ly1, PT_ACID);
        b.rect(rx0(1), ly0 + lh / 3, rx1(1), ly0 + lh / 3 + int(20 * fill), PT_BASE, 0.6f);
        b.rect(rx0(2), ly1 - 40, rx1(2), ly1, PT_WATR);
        b.rect(rx0(2), ly1 - 70, rx1(2), ly1 - 40, PT_CO2, 0.5f * fill);
        b.rect(rx0(3), ly0 + lh / 3, rx1(3), ly0 + lh / 3 + 40, PT_H2, 0.3f * fill);
        b.rect(rx0(3), ly0 + lh / 3 + 40, rx1(3), ly0 + lh / 3 + 80, PT_O2, 0.3f * fill);
        b.rect(rx0(3) + 10, ly0 + lh / 3 + 36, rx0(3) + 30, ly0 + lh / 3 + 44, PT_FIRE);
    } else if (name == "thermite") {
        // Thermite lit by fire, burning into an iron block (inside walls).
        int cw = w / CELL, ch = h / CELL;
        b.wallRect(1, 1, cw - 1, 3, WL_WALL);
        b.wallRect(1, ch - 3, cw - 1, ch - 1, WL_WALL);
        b.wallRect(1, 1, 3, ch - 1, WL_WALL);
        b.wallRect(cw - 3, 1, cw - 1, ch - 1, WL_WALL);
        const int ly1 = h - 3 * CELL;
        int tx = w / 3;
        b.rect(tx, ly1 - 30, tx + w / 3, ly1, PT_IRON);
        b.rect(tx, ly1 - 33, tx + w / 3, ly1 - 30, PT_THRM);
        b.rect(tx, ly1 - 36, tx + w / 3, ly1 - 33, PT_FIRE, 0.5f);
    } else if (name == "heat") {
        // Heat conduction only: metal bars with one hot end (below METL's melting point) in empty space. One
        // horizontal bar, one vertical bar heated at the top and one heated at the bottom (TPT's update order
        // runs top to bottom, so direction can matter).
        int cw = w / CELL, ch = h / CELL;
        b.wallRect(1, 1, cw - 1, 3, WL_WALL);
        b.wallRect(1, ch - 3, cw - 1, ch - 1, WL_WALL);
        b.wallRect(1, 1, 3, ch - 1, WL_WALL);
        b.wallRect(cw - 3, 1, cw - 1, ch - 1, WL_WALL);
        int hw = w * 3 / 5;
        b.rect(w / 10, h / 6, w / 10 + hw / 5, h / 6 + 12, PT_METL, 1.0f, 1200.0f);
        b.rect(w / 10 + hw / 5, h / 6, w / 10 + hw, h / 6 + 12, PT_METL);
        int vx = w * 3 / 4, vh = h * 2 / 3, vy = h / 4;
        b.rect(vx, vy, vx + 12, vy + vh / 5, PT_METL, 1.0f, 1200.0f);
        b.rect(vx, vy + vh / 5, vx + 12, vy + vh, PT_METL);
        int ux = w * 3 / 4 + 40;
        b.rect(ux, vy, ux + 12, vy + vh * 4 / 5, PT_METL);
        b.rect(ux, vy + vh * 4 / 5, ux + 12, vy + vh, PT_METL, 1.0f, 1200.0f);
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
