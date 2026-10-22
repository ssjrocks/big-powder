#include "sim/Scenes.h"

#include <algorithm>
#include <cmath>
#include <random>
#include <stdexcept>

namespace bp {

const std::vector<std::string> &sceneNames() {
    static const std::vector<std::string> names{"empty", "water", "sand", "rain", "mixed", "pile",
                                                "dam",   "layers", "sink", "walls", "detector", "open", "boil",
                                                "plants", "sponge", "coal", "lava", "heat", "thermite",
                                                "chem", "neutral", "rooms", "circuit", "gasfire",
                                                "explosives", "tntcord", "specials", "light",
                                                "nuclear", "exotic", "sing", "force", "rays", "emp", "electric", "portals", "life", "soap", "pipes", "pistons", "tron", "stickmen", "stacking"};
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
        if (type == PT_NEUT || type == PT_ELEC) {
            // NEUT's and ELEC's Create: a random direction (NEUT at a random speed, ELEC at 2).
            const float pi = 3.14159265f;
            float r = type == PT_NEUT ? float(std::uniform_int_distribution<int>(128, 255)(rng)) / 127.0f : 2.0f;
            float a = float(std::uniform_int_distribution<int>(0, 359)(rng)) * pi / 180.0f;
            p.life = type == PT_NEUT ? std::uniform_int_distribution<int>(480, 959)(rng) : 680;
            p.vx = r * std::cos(a);
            p.vy = r * std::sin(a);
        }
        if (type == PT_PHOT) {
            // PHOT's Create: speed 3 in one of 8 directions.
            float a = float(std::uniform_int_distribution<int>(0, 7)(rng)) * 0.78540f;
            p.vx = 3.0f * std::cos(a);
            p.vy = 3.0f * std::sin(a);
        }
        if (type == PT_SEED) {
            // SEED's Create: colour genes, growth direction up (vertical gravity), random growth program.
            p.ctype = 0xEC0;
            std::uniform_int_distribution<int> bits(0, (1 << 15) - 1);
            p.tmp = bits(rng);
            p.tmp2 = bits(rng);
            p.tmp3 = bits(rng);
            p.tmp4 = bits(rng);
        }
        if (type == PT_TRON) {
            // TRON's Create: a head in a random direction and colour, tail length 4.
            int hue = std::uniform_int_distribution<int>(0, 359)(rng);
            int dir = std::uniform_int_distribution<int>(0, 3)(rng);
            p.tmp = 1 | (dir << 5) | (hue << 7);
            p.tmp2 = 4;
            p.life = 5;
        }
        uint32_t id = uint32_t(s.ptype.size());
        s.parts.push_back(p);
        s.ptype.push_back(uint32_t(type));
        cell = id + 1;
    }

    // n particles in one cell, as TPT's create_part(-3) can make; the last one is on top in pmap.
    void stack(int x, int y, int type, int n) {
        for (int k = 0; k < n; k++) {
            uint32_t &cell = s.pmap[size_t(y) * s.W + size_t(x)];
            uint32_t was = cell;
            cell = 0;
            put(x, y, type);
            if (cell == 0)
                cell = was;
        }
        s.stacked = true;
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
    } else if (name == "force") {
        // Five walled rooms: water poured past ACEL and DCEL strips; a hot RPEL block in falling dust; DMG dropped
        // into a sand pile with glass and wood; a hot-to-cold HEAC bar between two blocks; CNCT poured into a pile.
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
        b.rect(rx0(0) + 10, ly0 + 10, rx1(0) - 10, ly0 + 40, PT_WATR, 0.5f);
        b.rect(rx0(0) + 10, ly0 + lh / 2, rx0(0) + 30, ly0 + lh / 2 + 3, PT_ACEL);
        b.rect(rx1(0) - 30, ly0 + lh / 2, rx1(0) - 10, ly0 + lh / 2 + 3, PT_DCEL);
        int m1 = (rx0(1) + rx1(1)) / 2;
        b.rect(m1 - 4, ly0 + lh / 2, m1 + 4, ly0 + lh / 2 + 8, PT_RPEL, 1.0f, 333.15f);
        b.rect(rx0(1) + 5, ly0 + 10, rx1(1) - 5, ly0 + 50, PT_DUST, 0.4f);
        b.rect(rx0(2), ly1 - 30, rx1(2), ly1 - 15, PT_SAND);
        b.rect(rx0(2), ly1 - 15, (rx0(2) + rx1(2)) / 2, ly1, PT_GLAS);
        b.rect((rx0(2) + rx1(2)) / 2, ly1 - 15, rx1(2), ly1, PT_WOOD);
        b.rect((rx0(2) + rx1(2)) / 2 - 2, ly0 + lh / 3, (rx0(2) + rx1(2)) / 2 + 2, ly0 + lh / 3 + 4, PT_DMG);
        b.rect(rx0(3) + 5, ly1 - 30, rx0(3) + 25, ly1, PT_METL, 1.0f, 1000.0f);
        b.rect(rx0(3) + 25, ly1 - 16, rx1(3) - 25, ly1 - 14, PT_HEAC);
        b.rect(rx1(3) - 25, ly1 - 30, rx1(3) - 5, ly1, PT_METL, 1.0f, 100.0f);
        b.rect(rx0(4) + 20, ly0 + 20, rx1(4) - 20, ly0 + 60, PT_CNCT, 0.6f);
    } else if (name == "rays") {
        // Four strips divided by walls, each powered by batteries: an ARAY beam through a FILT column onto a METL
        // bar, with dust falling through the beam, and an erasing beam from PSCN across it; a CRAY shooting sand and a DRAY
        // copying a stone and gold pattern; a WIFI pair on one channel driving electrolysis, one on another channel
        // that stays off; a beam from INST through METL bars and a STOR.
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
        auto at = [&](int x, int y) -> Particle & { return s.parts[s.pmap[size_t(y) * s.W + size_t(x)] - 1]; };
        // Strip 0: ARAY beams
        int y = sy(0);
        b.rect(lx0, y - 2, lx0 + 4, y + 2, PT_BTRY);
        b.rect(lx0 + 4, y, lx0 + 20, y + 1, PT_METL);
        b.put(lx0 + 20, y, PT_ARAY);
        b.rect(lx0 + 150, y - 3, lx0 + 153, y + 4, PT_FILT, 1.0f, 600.0f);
        b.rect(lx0 + 400, y - 6, lx0 + 403, y + 7, PT_METL);
        b.rect(lx0 + 200, y - 30, lx0 + 300, y - 20, PT_DUST, 0.3f);
        b.rect(lx0 + 348, y - 34, lx0 + 352, y - 30, PT_BTRY); // PSCN sparks fire an erasing beam down through it
        b.rect(lx0 + 350, y - 30, lx0 + 351, y - 20, PT_PSCN);
        b.put(lx0 + 350, y - 20, PT_ARAY);
        // Strip 1: CRAY and DRAY
        y = sy(1);
        b.rect(lx0, y - 2, lx0 + 4, y + 2, PT_BTRY);
        b.rect(lx0 + 4, y, lx0 + 20, y + 1, PT_METL);
        b.put(lx0 + 20, y, PT_CRAY);
        at(lx0 + 20, y).ctype = PT_SAND;
        at(lx0 + 20, y).tmp = 12;
        at(lx0 + 20, y).tmp2 = 3;
        b.rect(lx0 + 300, y - 2, lx0 + 304, y + 2, PT_BTRY);
        b.rect(lx0 + 304, y, lx0 + 320, y + 1, PT_METL);
        b.put(lx0 + 320, y, PT_DRAY);
        at(lx0 + 320, y).ctype = PT_DMND;
        at(lx0 + 320, y).tmp2 = 2;
        for (int k = 0; k < 6; k++)
            b.put(lx0 + 321 + k, y, k % 3 == 1 ? PT_GOLD : PT_STNE);
        b.put(lx0 + 327, y, PT_DMND);
        b.rect(lx0 + 300, y + 20, lx0 + 420, y + 22, PT_GLAS); // a floor under the copies
        // Strip 2: WIFI
        y = sy(2);
        b.rect(lx0, y - 2, lx0 + 4, y + 2, PT_BTRY);
        b.rect(lx0 + 4, y, lx0 + 20, y + 1, PT_METL);
        b.put(lx0 + 20, y, PT_WIFI, 600.0f);
        b.put(lx0 + 300, y, PT_WIFI, 600.0f);
        b.rect(lx0 + 301, y, lx0 + 306, y + 1, PT_PSCN);
        b.rect(lx0 + 306, y, lx0 + 380, y + 1, PT_METL);
        b.rect(lx0 + 380, y, lx0 + 381, y + 39, PT_METL);
        b.rect(lx0 + 379, y + 39, lx0 + 382, y + 44, PT_IRON); // on the floor, in the water that spreads there
        b.rect(lx0 + 350, y + 15, lx0 + 420, y + 35, PT_WATR);
        b.put(lx0 + 450, y, PT_WIFI, 900.0f); // another channel
        b.rect(lx0 + 451, y, lx0 + 456, y + 1, PT_PSCN);
        b.rect(lx0 + 456, y, lx0 + 520, y + 1, PT_METL);
        // Strip 3: a beam from INST goes through METL bars, sparking each, and makes a STOR let go of its water
        y = sy(3);
        b.rect(lx0, y - 2, lx0 + 4, y + 2, PT_BTRY);
        b.rect(lx0 + 4, y, lx0 + 8, y + 1, PT_PSCN); // (only PSCN sparks INST)
        b.rect(lx0 + 8, y, lx0 + 20, y + 1, PT_INST);
        b.put(lx0 + 20, y, PT_ARAY);
        b.put(lx0 + 60, y, PT_STOR);
        at(lx0 + 60, y).tmp = PT_WATR;
        at(lx0 + 60, y).temp = 300.0f;
        for (int k = 1; k <= 3; k++)
            b.rect(lx0 + 100 * k, y - 4, lx0 + 100 * k + 3, y + 5, PT_METL);
        b.rect(lx0 + 420, y - 4, lx0 + 423, y + 5, PT_GLAS);
    } else if (name == "emp") {
        // EMP set off by a spark (eight at once) next to a battery-sparked PSCN line, with METL, BMTL, SWCH, DLAY,
        // WIFI and ARAY within reach, and GLAS that EMP doesn't affect.
        int cw = w / CELL, ch = h / CELL;
        b.wallRect(1, 1, cw - 1, 3, WL_WALL);
        b.wallRect(1, ch - 3, cw - 1, ch - 1, WL_WALL);
        b.wallRect(1, 1, 3, ch - 1, WL_WALL);
        b.wallRect(cw - 3, 1, cw - 1, ch - 1, WL_WALL);
        const int lx0 = 3 * CELL + 4;
        for (int k = 0; k < 3; k++) {
            int y = h / 4 + k * (h / 4);
            b.rect(lx0, y - 2, lx0 + 4, y + 2, PT_BTRY);
            b.rect(lx0 + 4, y, lx0 + 60, y + 1, PT_METL);
            if (k == 1)
                b.rect(lx0 + 20, y - 1, lx0 + 28, y, PT_EMP);
            b.rect(lx0 + 100, y - 2, lx0 + 104, y + 2, PT_BTRY);
            b.rect(lx0 + 104, y, lx0 + 500, y + 1, PT_PSCN);
            b.rect(lx0 + 110, y + 2, lx0 + 210, y + 3, PT_METL);
            b.rect(lx0 + 210, y + 2, lx0 + 280, y + 3, PT_BMTL);
            b.rect(lx0 + 290, y + 2, lx0 + 390, y + 3, PT_GLAS);
            b.rect(lx0 + 110, y - 2, lx0 + 150, y - 1, PT_SWCH);
            b.rect(lx0 + 160, y - 2, lx0 + 200, y - 1, PT_DLAY);
            b.rect(lx0 + 210, y - 2, lx0 + 250, y - 1, PT_WIFI);
            b.rect(lx0 + 260, y - 2, lx0 + 300, y - 1, PT_ARAY);
        }
    } else if (name == "electric") {
        // Three walled rooms: battery-fed ETRD (single electrodes 40 px apart, a pair with INSL halfway, two ETRD
        // lines facing each other); a Tesla coil over wood, DEUT and METL; lightning striking a METL plate,
        // gunpowder, water and wood.
        int cw = w / CELL, ch = h / CELL;
        b.wallRect(1, 1, cw - 1, 3, WL_WALL);
        b.wallRect(1, ch - 3, cw - 1, ch - 1, WL_WALL);
        b.wallRect(1, 1, 3, ch - 1, WL_WALL);
        b.wallRect(cw - 3, 1, cw - 1, ch - 1, WL_WALL);
        const int rooms = 3;
        int room = (cw - 6) / rooms;
        for (int k = 1; k < rooms; k++)
            b.wallRect(3 + k * room, 3, 3 + k * room + 1, ch - 3, WL_WALL);
        const int ly0 = 3 * CELL, ly1 = h - 3 * CELL, lh = ly1 - ly0;
        auto rx0 = [&](int k) { return (3 + k * room + (k ? 1 : 0)) * CELL; };
        auto at = [&](int x, int y) -> Particle & { return s.parts[s.pmap[size_t(y) * s.W + size_t(x)] - 1]; };
        // Room 0: ETRD
        int x = rx0(0) + 6;
        for (int k = 0; k < 3; k++) {
            int y = ly0 + lh / 6 + k * (lh / 3);
            b.rect(x, y - 2, x + 4, y + 2, PT_BTRY);
            b.rect(x + 4, y, x + 30, y + 1, PT_METL);
            if (k == 0) {
                b.put(x + 30, y, PT_ETRD);
                b.put(x + 70, y, PT_ETRD);
                b.rect(x + 71, y, x + 140, y + 1, PT_METL);
            } else if (k == 1) {
                b.put(x + 30, y, PT_ETRD);
                b.rect(x + 49, y - 6, x + 52, y + 7, PT_INSL);
                b.put(x + 70, y, PT_ETRD);
                b.rect(x + 71, y, x + 140, y + 1, PT_METL);
            } else {
                b.rect(x + 30, y - 10, x + 31, y + 11, PT_ETRD);
                b.rect(x + 60, y - 10, x + 61, y + 11, PT_ETRD);
            }
        }
        // Room 1: a Tesla coil
        x = rx0(1) + 10;
        int y = ly0 + lh / 3;
        b.rect(x, y - 2, x + 4, y + 2, PT_BTRY);
        b.rect(x + 4, y, x + 60, y + 1, PT_METL);
        b.rect(x + 60, y - 2, x + 63, y + 3, PT_TESC);
        for (int ty = y - 2; ty < y + 3; ty++)
            for (int tx = x + 60; tx < x + 63; tx++)
                at(tx, ty).tmp = 120;
        b.rect(x + 20, ly1 - 12, x + 100, ly1, PT_WOOD);
        b.rect(x + 100, ly1 - 12, x + 130, ly1, PT_DEUT);
        b.rect(x + 40, y + 30, x + 90, y + 32, PT_METL);
        // Room 2: lightning
        x = rx0(2);
        for (int k = 0; k < 4; k++)
            b.put(x + 20 + k * 30, ly0 + 10 + 5 * k, PT_LIGH);
        b.rect(x + 10, ly0 + lh / 2, x + 60, ly0 + lh / 2 + 2, PT_METL);
        b.rect(x + 70, ly1 - 30, x + 110, ly1, PT_GUNP);
        b.rect(x + 10, ly1 - 15, x + 60, ly1, PT_WATR);
        b.rect(x + 115, ly1 - 25, x + 150, ly1, PT_WOOD);
    } else if (name == "portals") {
        // Five walled rooms: sand poured onto a PRTI comes out of a PRTO in the next room, and a battery's sparks
        // go through too (channel 3); water goes through a hotter channel (6) and photons into a PRTI; LITH dropped
        // into water, and a LITH battery charged by PSCN and discharged into NSCN; PTNM in H2 and O2 and under
        // smoke, MORT, and LOVE and LOLZ.
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
        auto mid = [&](int k) { return (rx0(k) + rx1(k)) / 2; };
        // Room 0: sand onto PRTI (channel 3); a battery sparking METL that touches another PRTI
        int m = mid(0);
        b.rect(m - 10, ly0 + 100, m + 10, ly0 + 101, PT_PRTI);
        b.rect(m - 30, ly0 + 20, m + 30, ly0 + 60, PT_SAND, 0.5f);
        b.rect(m - 30, ly1 - 40, m - 26, ly1 - 36, PT_BTRY);
        b.rect(m - 26, ly1 - 38, m, ly1 - 37, PT_METL);
        b.put(m, ly1 - 38, PT_PRTI);
        // Room 1: the PRTO for both (channel 3), the sparks reach METL around a PRTO into a GAS cloud
        m = mid(1);
        b.rect(m - 10, ly0 + 40, m + 10, ly0 + 41, PT_PRTO);
        b.put(m, ly1 - 60, PT_PRTO);
        b.rect(m - 15, ly1 - 61, m - 1, ly1 - 60, PT_METL);
        b.rect(m - 40, ly1 - 90, m + 40, ly1 - 70, PT_GAS, 0.3f);
        // Room 2: water into PRTI at 600 K (channel 6) out of a PRTO below; photons into a third PRTI
        m = mid(2);
        b.rect(m - 30, ly0 + 10, m + 30, ly0 + 40, PT_WATR, 0.6f);
        b.rect(m - 10, ly0 + 70, m + 10, ly0 + 71, PT_PRTI, 1.0f, 600.0f);
        b.rect(m - 10, ly0 + 200, m + 10, ly0 + 201, PT_PRTO, 1.0f, 600.0f);
        b.rect(m - 20, ly0 + lh / 2 + 60, m + 20, ly0 + lh / 2 + 61, PT_PRTI, 1.0f, 1000.0f);
        b.rect(m - 15, ly0 + lh / 2 + 50, m + 15, ly0 + lh / 2 + 58, PT_PHOT, 0.2f);
        // Room 3: LITH into water; a LITH battery between PSCN (charging) and NSCN (discharging into METL)
        m = mid(3);
        b.rect(rx0(3) + 4, ly1 - 30, rx1(3) - 4, ly1, PT_WATR);
        b.rect(m - 8, ly1 - 80, m + 8, ly1 - 70, PT_LITH);
        b.rect(m - 40, ly0 + 60, m - 36, ly0 + 64, PT_BTRY);
        b.rect(m - 36, ly0 + 62, m - 20, ly0 + 63, PT_PSCN);
        b.rect(m - 20, ly0 + 58, m, ly0 + 67, PT_LITH);
        b.rect(m - 20, ly0 + 67, m, ly0 + 69, PT_GLAS);
        b.rect(m, ly0 + 62, m + 4, ly0 + 63, PT_NSCN);
        b.rect(m + 4, ly0 + 62, m + 40, ly0 + 63, PT_METL);
        // Room 4: PTNM with H2 and O2, under smoke; MORT; LOVE and LOLZ
        m = mid(4);
        b.rect(m - 20, ly1 - 10, m + 20, ly1, PT_PTNM);
        b.rect(m - 30, ly1 - 40, m + 30, ly1 - 12, PT_H2, 0.3f);
        b.rect(m - 30, ly1 - 40, m + 30, ly1 - 12, PT_O2, 0.3f);
        b.rect(m - 20, ly0 + lh / 2, m + 20, ly0 + lh / 2 + 2, PT_PTNM);
        b.rect(m - 20, ly0 + lh / 2 + 3, m + 20, ly0 + lh / 2 + 20, PT_SMKE, 0.5f);
        b.rect(m - 3, ly0 + 140, m + 3, ly0 + 143, PT_MORT);
        b.rect(m - 30, ly0 + 20, m - 24, ly0 + 26, PT_LOVE);
        b.rect(m + 10, ly0 + 20, m + 16, ly0 + 26, PT_LOLZ);
    } else if (name == "life") {
        // Game of Life: random soups of four rulesets (GOL, HLIF, DANI, and Brian 6 with three states), a glider
        // heading for a sand pile LIFE can't grow into, all in an open world (the GoL space wraps around).
        auto at = [&](int x, int y) -> Particle & { return s.parts[s.pmap[size_t(y) * s.W + size_t(x)] - 1]; };
        auto soup = [&](int x0, int y0, int size, int rule, int states, uint32_t colour2) {
            for (int y = y0; y < y0 + size; y++)
                for (int x = x0; x < x0 + size; x++)
                    if (std::uniform_real_distribution<float>(0.0f, 1.0f)(b.rng) < 0.35f) {
                        b.put(x, y, PT_LIFE);
                        Particle &q = at(x, y);
                        q.ctype = rule;
                        q.tmp = int(colour2);
                        q.tmp2 = states;
                    }
        };
        soup(40, 40, 70, 0, 1, 0x0CAC00);
        soup(200, 40, 70, 1, 1, 0xFF0000);
        soup(360, 40, 70, 4, 1, 0x00FFFF);
        soup(40, 220, 70, 23, 3, 0x969600);
        const int glider[5][2] = {{1, 0}, {2, 1}, {0, 2}, {1, 2}, {2, 2}};
        for (auto &g : glider) {
            b.put(250 + g[0], 250 + g[1], PT_LIFE);
            Particle &q = at(250 + g[0], 250 + g[1]);
            q.tmp = 0x0CAC00;
            q.tmp2 = 1;
        }
        b.rect(330, 300, 420, 330, PT_SAND);
        b.rect(320, 330, 430, 334, PT_DMND);
    } else if (name == "soap") {
        // Three walled rooms: a SOAP pool under gunpowder that fire sets off (the pressure turns SOAP into bubble
        // film); SOAP and OIL poured together (foam); SOAP poured onto a PRTI, with its PRTO above a glass floor.
        int cw = w / CELL, ch = h / CELL;
        b.wallRect(1, 1, cw - 1, 3, WL_WALL);
        b.wallRect(1, ch - 3, cw - 1, ch - 1, WL_WALL);
        b.wallRect(1, 1, 3, ch - 1, WL_WALL);
        b.wallRect(cw - 3, 1, cw - 1, ch - 1, WL_WALL);
        const int rooms = 3;
        int room = (cw - 6) / rooms;
        for (int k = 1; k < rooms; k++)
            b.wallRect(3 + k * room, 3, 3 + k * room + 1, ch - 3, WL_WALL);
        const int ly0 = 3 * CELL, ly1 = h - 3 * CELL;
        auto rx0 = [&](int k) { return (3 + k * room + (k ? 1 : 0)) * CELL; };
        auto rx1 = [&](int k) { return (3 + (k + 1) * room) * CELL; };
        b.rect(rx0(0) + 4, ly1 - 60, rx1(0) - 4, ly1, PT_SOAP);
        b.rect(rx0(0) + 60, ly1 - 90, rx1(0) - 60, ly1 - 70, PT_GUNP);
        b.rect(rx0(0) + 90, ly1 - 95, rx0(0) + 94, ly1 - 91, PT_FIRE);
        b.rect(rx0(1) + 10, ly0 + 20, rx1(1) - 10, ly0 + 80, PT_SOAP, 0.5f);
        b.rect(rx0(1) + 10, ly0 + 20, rx1(1) - 10, ly0 + 80, PT_OIL, 0.5f);
        int m = (rx0(2) + rx1(2)) / 2;
        b.rect(m - 20, ly0 + 20, m + 20, ly0 + 50, PT_SOAP);
        b.rect(m - 10, ly0 + 100, m + 10, ly0 + 101, PT_PRTI);
        b.rect(m - 10, ly0 + 200, m + 10, ly0 + 201, PT_PRTO);
        b.rect(rx0(2) + 4, ly0 + 260, rx1(2) - 4, ly0 + 263, PT_GLAS);
    } else if (name == "pipes") {
        // Three strips, each a pipe fed from a tank at its left end. Its right end touches a liquid- or
        // powder-only wall, which BRCK can't grow into, so the pipe opens there (a player would erase BRCK) and
        // what it carries pours out. Strip 0: PIPE carrying water, with HEAC and a hot METL block touching it;
        // strip 1: PPIP carrying sand, sparked on by PSCN; strip 2: PPIP carrying water, paused by NSCN.
        int cw = w / CELL, ch = h / CELL;
        b.wallRect(1, 1, cw - 1, 3, WL_WALL);
        b.wallRect(1, ch - 3, cw - 1, ch - 1, WL_WALL);
        b.wallRect(1, 1, 3, ch - 1, WL_WALL);
        b.wallRect(cw - 3, 1, cw - 1, ch - 1, WL_WALL);
        int strip = (ch - 6) / 3;
        for (int k = 1; k < 3; k++)
            b.wallRect(3, 3 + k * strip, cw - 3, 3 + k * strip + 1, WL_WALL);
        for (int k = 0; k < 3; k++) {
            int y = (3 + k * strip + strip / 2) * CELL;
            int a = 80, e = 80 + 4 * 15 - 1;  // the pipe's last column, just left of a wall cell
            int pipe = k == 0 ? PT_PIPE : PT_PPIP;
            int fill = k == 1 ? PT_SAND : PT_WATR;
            b.rect(a, y, e + 1, y + 3, pipe);
            // the tank at its left end
            b.rect(a - 34, y - 30, a - 30, y + 7, PT_GLAS);
            b.rect(a - 34, y + 3, a, y + 7, PT_GLAS);
            b.rect(a - 30, y - 30, a, y + 3, fill);
            b.rect(a - 4, y - 30, a, y - 1, PT_GLAS);
            // the opening: a wall only what it carries can pass
            b.wallRect((e + 1) / CELL, (y - 8) / CELL, (e + 1) / CELL + 2, (y + 12) / CELL, k == 1 ? WL_ALLOWPOWDER : WL_ALLOWLIQUID);
            if (k == 0) {
                b.rect(a + 20, y - 3, a + 30, y, PT_HEAC);
                b.rect(a + 40, y - 6, a + 50, y, PT_METL, 1.0f, 900.0f);
            } else {
                int sw = k == 1 ? PT_PSCN : PT_NSCN;
                b.rect(a + 30, y + 3, a + 31, y + 9, sw);
                b.rect(a + 28, y + 9, a + 33, y + 13, PT_BTRY);
            }
        }
    } else if (name == "pistons") {
        // Three strips with pistons (a PSTN base, sparked from the side): one pushing a column of stone, one pushing a
        // FRME bar up with sand on it, and one that PSCN extends and NSCN (on a longer wire) pulls back, with a
        // brick block in front.
        int cw = w / CELL, ch = h / CELL;
        b.wallRect(1, 1, cw - 1, 3, WL_WALL);
        b.wallRect(1, ch - 3, cw - 1, ch - 1, WL_WALL);
        b.wallRect(1, 1, 3, ch - 1, WL_WALL);
        b.wallRect(cw - 3, 1, cw - 1, ch - 1, WL_WALL);
        int strip = (ch - 6) / 3;
        for (int k = 1; k < 3; k++)
            b.wallRect(3, 3 + k * strip, cw - 3, 3 + k * strip + 1, WL_WALL);
        auto sy = [&](int k) { return (3 + k * strip + strip / 2) * CELL; };
        // Strip 0: pushing stone to the right
        int y = sy(0), x0 = 60;
        b.rect(x0, y, x0 + 5, y + 1, PT_PSTN);
        b.rect(x0 - 20, y - 6, x0 - 16, y - 2, PT_BTRY);
        b.rect(x0 - 16, y - 4, x0 + 1, y - 3, PT_PSCN);
        b.rect(x0, y - 3, x0 + 1, y - 1, PT_PSCN);
        b.rect(x0 + 5, y - 10, x0 + 9, y + 11, PT_STNE);
        b.rect(x0 + 5, y + 11, x0 + 200, y + 13, PT_GLAS);
        // Strip 1: pushing a frame with sand on it upwards
        y = sy(1);
        int xm = 200;
        b.rect(xm, y + 20, xm + 1, y + 26, PT_PSTN);
        b.rect(xm + 2, y + 30, xm + 6, y + 34, PT_BTRY);
        b.rect(xm + 1, y + 25, xm + 2, y + 32, PT_PSCN);
        b.rect(xm - 12, y + 19, xm + 13, y + 20, PT_FRME);
        b.rect(xm - 10, y + 5, xm + 11, y + 19, PT_SAND);
        // Strip 2: pushed out by PSCN, pulled back by NSCN
        y = sy(2);
        b.rect(x0, y, x0 + 5, y + 1, PT_PSTN);
        b.rect(x0 - 20, y - 6, x0 - 16, y - 2, PT_BTRY);
        b.rect(x0 - 16, y - 4, x0 + 1, y - 3, PT_PSCN);
        b.rect(x0, y - 3, x0 + 1, y - 1, PT_PSCN);
        b.rect(x0 + 2, y + 2, x0 + 3, y + 4, PT_NSCN);
        b.rect(x0 + 3, y + 3, x0 + 300, y + 4, PT_METL);
        b.rect(x0 + 300, y + 1, x0 + 304, y + 5, PT_BTRY);
        b.rect(x0 + 5, y - 3, x0 + 12, y + 3, PT_BRCK);
    } else if (name == "tron") {
        // Three rooms of tron bikes among brick pillars: ordinary heads; heads that never turn at random (so only
        // the update order decides where they go); heads whose tails never die.
        int cw = w / CELL, ch = h / CELL;
        b.wallRect(1, 1, cw - 1, 3, WL_WALL);
        b.wallRect(1, ch - 3, cw - 1, ch - 1, WL_WALL);
        b.wallRect(1, 1, 3, ch - 1, WL_WALL);
        b.wallRect(cw - 3, 1, cw - 1, ch - 1, WL_WALL);
        const int rooms = 3;
        int room = (cw - 6) / rooms;
        for (int k = 1; k < rooms; k++)
            b.wallRect(3 + k * room, 3, 3 + k * room + 1, ch - 3, WL_WALL);
        const int ly0 = 3 * CELL, ly1 = h - 3 * CELL;
        for (int k = 0; k < rooms; k++) {
            int rx0 = (3 + k * room + (k ? 1 : 0)) * CELL, rx1 = (3 + (k + 1) * room) * CELL;
            int rw = rx1 - rx0, rh = ly1 - ly0;
            for (int py = 1; py <= 3; py++)
                for (int px = 1; px <= 3; px++) {
                    int cx = rx0 + px * rw / 4, cy = ly0 + py * rh / 4;
                    b.rect(cx - 4, cy - 4, cx + 4, cy + 4, PT_BRCK);
                }
            int heads = k == 2 ? 4 : 6;
            for (int n = 0; n < heads; n++) {
                int hx = rx0 + 12 + n * (rw - 24) / heads, hy = ly0 + 12 + (n % 3) * (rh - 24) / 2;
                size_t first = s.ptype.size();
                b.put(hx, hy, PT_TRON);
                if (s.ptype.size() > first && k > 0)
                    s.parts[first].tmp |= k == 1 ? 0x10000 : 0x8; // TRON_NORANDOM, TRON_NODIE
            }
        }
    } else if (name == "stacking") {
        // Piles of particles in single cells, as saves and some tools leave them: METL piles of 4, 8, 20, 50, 200 and
        // 2,000 (CheckStacking turns piles of more than 5 into NBHL, sooner the bigger), and a SAND pile of 50 that
        // spreads out before it can.
        int cw = w / CELL, ch = h / CELL;
        b.wallRect(1, 1, cw - 1, 3, WL_WALL);
        b.wallRect(1, ch - 3, cw - 1, ch - 1, WL_WALL);
        b.wallRect(1, 1, 3, ch - 1, WL_WALL);
        b.wallRect(cw - 3, 1, cw - 1, ch - 1, WL_WALL);
        const int ly1 = h - 3 * CELL;
        b.rect(3 * CELL, ly1 - 20, w - 3 * CELL, ly1, PT_BRCK);
        const int piles[] = {4, 8, 20, 50, 200, 2000};
        for (int k = 0; k < 6; k++)
            b.stack(60 + 70 * k, ly1 - 60, PT_METL, piles[k]);
        b.stack(w - 60, ly1 - 60, PT_SAND, 50);
    } else if (name == "stickmen") {
        // Two rooms with a brick floor. Left: STKM on the right, three fighters on the left that walk to it over a
        // brick step. Right: STKM2, and two fighters that cross a strip of hot metal to reach it, one of them
        // holding FIRE (fighters shoot what they hold when it is dangerous and they are close). STKM holds water. A
        // cloud of GRAV floats near the ceiling.
        int cw = w / CELL, ch = h / CELL;
        b.wallRect(1, 1, cw - 1, 3, WL_WALL);
        b.wallRect(1, ch - 3, cw - 1, ch - 1, WL_WALL);
        b.wallRect(1, 1, 3, ch - 1, WL_WALL);
        b.wallRect(cw - 3, 1, cw - 1, ch - 1, WL_WALL);
        int room = (cw - 6) / 2;
        b.wallRect(3 + room, 3, 3 + room + 1, ch - 3, WL_WALL);
        const int ly1 = h - 3 * CELL, floorTop = ly1 - 12;
        for (int k = 0; k < 2; k++) {
            int rx0 = (3 + k * room + (k ? 1 : 0)) * CELL, rx1 = (3 + (k + 1) * room) * CELL;
            int mid = (rx0 + rx1) / 2;
            if (k == 1)
                b.rect(mid - 10, floorTop, mid + 10, floorTop + 3, PT_METL, 1.0f, 600.0f);
            b.rect(rx0, floorTop, rx1, ly1, PT_BRCK);
            if (k == 0) {
                b.rect(mid - 5, floorTop - 5, mid + 5, floorTop, PT_BRCK);
                b.rect(rx0 + 20, 3 * CELL + 10, rx0 + 80, 3 * CELL + 30, PT_GRAV, 0.5f); // light dust, out of reach
            }
            size_t player = s.ptype.size();
            b.put(rx1 - 40, floorTop - 13, k == 0 ? PT_STKM : PT_STKM2);
            if (k == 0 && s.ptype.size() > player)
                s.parts[player].ctype = PT_WATR; // what STKM shoots (with --stickman-keys)
            for (int n = 0; n < (k == 0 ? 3 : 2); n++) {
                size_t first = s.ptype.size();
                int jitter = std::uniform_int_distribution<int>(-8, 8)(b.rng); // (the seed varies where they start)
                b.put(rx0 + 20 + 30 * n + jitter, floorTop - 13, PT_FIGH);
                if (k == 1 && n == 0 && s.ptype.size() > first)
                    s.parts[first].ctype = PT_FIRE;
            }
        }
    } else if (name == "sing") {
        // A singularity on a bed of sand, inside walls.
        int cw = w / CELL, ch = h / CELL;
        b.wallRect(1, 1, cw - 1, 3, WL_WALL);
        b.wallRect(1, ch - 3, cw - 1, ch - 1, WL_WALL);
        b.wallRect(1, 1, 3, ch - 1, WL_WALL);
        b.wallRect(cw - 3, 1, cw - 1, ch - 1, WL_WALL);
        const int ly1 = h - 3 * CELL;
        b.rect(w / 2 - 50, ly1 - 40, w / 2 + 50, ly1, PT_SAND);
        b.rect(w / 2 - 2, ly1 - 45, w / 2 + 2, ly1 - 41, PT_SING);
    } else if (name == "exotic") {
        // Six walled rooms: light through a FILT column; antimatter dropped on stone; a singularity in sand; an EXOT
        // pool; VIBR on a hot plate; tungsten above its melting point.
        int cw = w / CELL, ch = h / CELL;
        b.wallRect(1, 1, cw - 1, 3, WL_WALL);
        b.wallRect(1, ch - 3, cw - 1, ch - 1, WL_WALL);
        b.wallRect(1, 1, 3, ch - 1, WL_WALL);
        b.wallRect(cw - 3, 1, cw - 1, ch - 1, WL_WALL);
        const int rooms = 6;
        int room = (cw - 6) / rooms;
        for (int k = 1; k < rooms; k++)
            b.wallRect(3 + k * room, 3, 3 + k * room + 1, ch - 3, WL_WALL);
        const int ly0 = 3 * CELL, ly1 = h - 3 * CELL, lh = ly1 - ly0;
        auto rx0 = [&](int k) { return (3 + k * room + (k ? 1 : 0)) * CELL; };
        auto rx1 = [&](int k) { return (3 + (k + 1) * room) * CELL; };
        int cy = ly0 + lh / 2;
        b.rect(rx0(0) + 10, cy - 2, rx0(0) + 14, cy + 2, PT_CLNE);
        b.put(rx0(0) + 14, cy, PT_PHOT);
        b.rect(rx0(0) + 40, ly0, rx0(0) + 44, ly1, PT_FILT, 1.0f, 600.0f);
        b.rect(rx0(1), ly1 - 20, rx1(1), ly1, PT_STNE);
        b.rect(rx0(1) + 30, ly0 + lh / 4, rx0(1) + 40, ly0 + lh / 4 + 6, PT_AMTR);
        b.rect(rx0(2), ly1 - 40, rx1(2), ly1, PT_SAND);
        b.rect((rx0(2) + rx1(2)) / 2 - 2, ly1 - 45, (rx0(2) + rx1(2)) / 2 + 2, ly1 - 41, PT_SING);
        b.rect(rx0(3), ly1 - 25, rx1(3), ly1, PT_EXOT);
        b.rect(rx0(4), ly1 - 6, rx1(4), ly1, PT_METL, 1.0f, 1000.0f);
        b.rect(rx0(4) + 20, ly1 - 26, rx1(4) - 20, ly1 - 6, PT_VIBR);
        b.rect(rx0(5) + 20, ly1 - 20, rx1(5) - 20, ly1, PT_TUNG, 1.0f, 3800.0f);
    } else if (name == "nuclear") {
        // Four walled rooms: a few neutrons in a block of PLUT (fission chain); neutrons in a DEUT pool (fusion
        // bursts); electrons over water and a METL bar (splitting water, sparking metal); POLO and URAN.
        int cw = w / CELL, ch = h / CELL;
        b.wallRect(1, 1, cw - 1, 3, WL_WALL);
        b.wallRect(1, ch - 3, cw - 1, ch - 1, WL_WALL);
        b.wallRect(1, 1, 3, ch - 1, WL_WALL);
        b.wallRect(cw - 3, 1, cw - 1, ch - 1, WL_WALL);
        const int rooms = 4;
        int room = (cw - 6) / rooms;
        for (int k = 1; k < rooms; k++)
            b.wallRect(3 + k * room, 3, 3 + k * room + 1, ch - 3, WL_WALL);
        const int ly0 = 3 * CELL, ly1 = h - 3 * CELL, lh = ly1 - ly0;
        auto rx0 = [&](int k) { return (3 + k * room + (k ? 1 : 0)) * CELL; };
        auto rx1 = [&](int k) { return (3 + (k + 1) * room) * CELL; };
        int m0 = (rx0(0) + rx1(0)) / 2;
        b.rect(m0 - 20, ly1 - 40, m0 + 20, ly1, PT_PLUT);
        b.rect(m0 - 3, ly1 - 46, m0 + 3, ly1 - 42, PT_NEUT);
        b.rect(rx0(1), ly1 - 30, rx1(1), ly1, PT_DEUT);
        b.rect(rx0(1) + 20, ly1 - 40, rx0(1) + 26, ly1 - 34, PT_NEUT);
        b.rect(rx0(2), ly1 - 20, rx1(2), ly1, PT_WATR);
        b.rect(rx0(2) + 10, ly0 + lh / 2, rx1(2) - 10, ly0 + lh / 2 + 4, PT_METL);
        b.rect(rx0(2) + 10, ly0 + lh / 3, rx1(2) - 10, ly0 + lh / 3 + 10, PT_ELEC, 0.2f);
        int m3 = (rx0(3) + rx1(3)) / 2;
        b.rect(m3 - 15, ly1 - 20, m3, ly1, PT_POLO);
        b.rect(m3, ly1 - 20, m3 + 15, ly1, PT_URAN);
    } else if (name == "light") {
        // A CLNE turned into a light source by a photon touching it shines on a glass block (refraction), a
        // metal wall (reflection), a GLOW pool (gain), quartz (scattering) and an RSST blob (hardening).
        int cw = w / CELL, ch = h / CELL;
        b.wallRect(1, 1, cw - 1, 3, WL_WALL);
        b.wallRect(1, ch - 3, cw - 1, ch - 1, WL_WALL);
        b.wallRect(1, 1, 3, ch - 1, WL_WALL);
        b.wallRect(cw - 3, 1, cw - 1, ch - 1, WL_WALL);
        const int lx0 = 3 * CELL, lx1 = w - 3 * CELL, ly0 = 3 * CELL, ly1 = h - 3 * CELL;
        const int iw = lx1 - lx0, lh = ly1 - ly0;
        int cx = lx0 + iw / 4, cy = ly0 + lh / 2;
        b.rect(cx - 2, cy - 2, cx + 2, cy + 2, PT_CLNE);
        b.put(cx + 2, cy, PT_PHOT);
        b.rect(lx0 + iw / 2, ly0 + lh / 4, lx0 + iw / 2 + 30, ly0 + lh * 3 / 4, PT_GLAS);
        b.rect(lx1 - 8, ly0, lx1, ly1, PT_METL);
        b.rect(lx0, ly1 - 15, lx0 + iw / 3, ly1, PT_GLOW);
        b.rect(lx0 + iw / 2 + 60, ly1 - 30, lx0 + iw / 2 + 90, ly1, PT_QRTZ);
        b.rect(lx0 + iw / 4 - 15, ly0 + 10, lx0 + iw / 4 + 15, ly0 + 25, PT_RSST);
    } else if (name == "specials") {
        // Five walled rooms: CLNE picks up water and floods a room with a VOID floor; sand rains onto a black hole;
        // CONV (made SAND by touching sand) under rain; PSCN-powered PCLN cloning DUST; a shield grows from sparks.
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
        int mid0 = (rx0(0) + rx1(0)) / 2;
        b.rect(mid0 - 10, ly0 + 20, mid0 + 10, ly0 + 23, PT_CLNE);
        b.rect(mid0 - 2, ly0 + 23, mid0 + 2, ly0 + 26, PT_WATR);
        b.rect(rx0(0), ly1 - 3, rx1(0), ly1, PT_VOID);
        int mid1 = (rx0(1) + rx1(1)) / 2;
        b.rect(mid1 - 3, ly0 + lh / 2, mid1 + 3, ly0 + lh / 2 + 6, PT_BHOL);
        b.rect(rx0(1) + 10, ly0 + 10, rx1(1) - 10, ly0 + 40, PT_SAND, 0.3f);
        int mid2 = (rx0(2) + rx1(2)) / 2;
        b.rect(mid2 - 15, ly1 - 10, mid2 + 15, ly1, PT_CONV);
        b.rect(mid2 + 15, ly1 - 3, mid2 + 18, ly1, PT_SAND);
        b.rect(rx0(2) + 5, ly0 + 20, rx1(2) - 5, ly0 + 50, PT_WATR, 0.2f);
        int y3 = ly0 + 30;
        b.rect(rx0(3) + 4, y3 - 2, rx0(3) + 8, y3 + 2, PT_BTRY);
        b.rect(rx0(3) + 8, y3, rx0(3) + 14, y3 + 1, PT_PSCN);
        b.rect(rx0(3) + 14, y3 - 2, rx0(3) + 30, y3 + 3, PT_PCLN);
        b.rect(rx0(3) + 30, y3 - 2, rx0(3) + 33, y3 + 3, PT_DUST);
        int y4 = ly1 - 30;
        b.rect(rx0(4) + 4, y4 - 2, rx0(4) + 8, y4 + 2, PT_BTRY);
        b.rect(rx0(4) + 8, y4, rx0(4) + 40, y4 + 1, PT_METL);
        b.rect(rx0(4) + 40, y4 - 5, rx0(4) + 50, y4 + 6, PT_SHLD1);
    } else if (name == "tntcord") {
        // Fire lights an IGNT cord that runs up to a block of TNT (BANG), inside walls.
        int cw = w / CELL, ch = h / CELL;
        b.wallRect(1, 1, cw - 1, 3, WL_WALL);
        b.wallRect(1, ch - 3, cw - 1, ch - 1, WL_WALL);
        b.wallRect(1, 1, 3, ch - 1, WL_WALL);
        b.wallRect(cw - 3, 1, cw - 1, ch - 1, WL_WALL);
        const int lx0 = 3 * CELL, ly1 = h - 3 * CELL;
        b.rect(lx0 + 4, ly1 - 8, lx0 + 10, ly1 - 4, PT_FIRE);
        b.rect(lx0 + 10, ly1 - 6, lx0 + 60, ly1 - 5, PT_IGNT);
        b.rect(lx0 + 60, ly1 - 20, lx0 + 90, ly1, PT_BANG);
    } else if (name == "explosives") {
        // Six walled rooms: a battery sparks a wire into FUSE; fire lights an IGNT cord into a block of BANG; BOMB
        // dropped on stone; a hot block next to TSNS, wired to IGNT; DEST dropped on brick; LNTG poured on C5.
        int cw = w / CELL, ch = h / CELL;
        b.wallRect(1, 1, cw - 1, 3, WL_WALL);
        b.wallRect(1, ch - 3, cw - 1, ch - 1, WL_WALL);
        b.wallRect(1, 1, 3, ch - 1, WL_WALL);
        b.wallRect(cw - 3, 1, cw - 1, ch - 1, WL_WALL);
        const int rooms = 6;
        int room = (cw - 6) / rooms;
        for (int k = 1; k < rooms; k++)
            b.wallRect(3 + k * room, 3, 3 + k * room + 1, ch - 3, WL_WALL);
        const int ly0 = 3 * CELL, ly1 = h - 3 * CELL, lh = ly1 - ly0;
        auto rx0 = [&](int k) { return (3 + k * room + (k ? 1 : 0)) * CELL; };
        auto rx1 = [&](int k) { return (3 + (k + 1) * room) * CELL; };
        int fy = ly1 - 20;
        b.rect(rx0(0) + 4, fy - 2, rx0(0) + 8, fy + 2, PT_BTRY);
        b.rect(rx0(0) + 8, fy, rx0(0) + 30, fy + 1, PT_METL);
        b.rect(rx0(0) + 30, fy - 1, rx1(0) - 4, fy + 2, PT_FUSE);
        b.rect(rx0(1) + 4, ly1 - 8, rx0(1) + 10, ly1 - 4, PT_FIRE);
        b.rect(rx0(1) + 10, ly1 - 6, rx0(1) + 60, ly1 - 5, PT_IGNT);
        b.rect(rx0(1) + 60, ly1 - 20, rx1(1) - 4, ly1, PT_BANG);
        b.rect(rx0(2), ly1 - 15, rx1(2), ly1, PT_STNE);
        b.rect(rx0(2) + 30, ly0 + lh / 3, rx0(2) + 40, ly0 + lh / 3 + 10, PT_BOMB);
        b.rect(rx0(3) + 4, ly1 - 30, rx0(3) + 24, ly1 - 10, PT_METL, 1.0f, 900.0f);
        b.rect(rx0(3) + 24, ly1 - 22, rx0(3) + 26, ly1 - 18, PT_TSNS);
        b.rect(rx0(3) + 26, ly1 - 20, rx0(3) + 60, ly1 - 19, PT_METL);
        b.rect(rx0(3) + 60, ly1 - 20, rx1(3) - 4, ly1 - 19, PT_IGNT);
        b.rect(rx0(4), ly1 - 30, rx1(4), ly1, PT_BRCK);
        b.rect(rx0(4) + 30, ly0 + lh / 4, rx0(4) + 36, ly0 + lh / 4 + 6, PT_DEST);
        b.rect(rx0(5) + 10, ly1 - 20, rx1(5) - 10, ly1, PT_C5);
        b.rect(rx0(5) + 10, ly0 + lh / 3, rx1(5) - 10, ly0 + lh / 3 + 10, PT_LNTG, 0.5f);
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
