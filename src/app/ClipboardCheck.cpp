// --check-clipboard: TPT's copy, transform, paste and cut (Simulation::Save, GameSave::Transform, Simulation::Load and
// clear_area) in TPT itself and on the GPU (sim/GpuSave, and tptTransform against TPT's own Transform), on the same
// 612x384 world, compared exactly: the saves made, and the worlds after pasting and cutting.
#include "app/App.h"
#include "classic/TptSave.h"
#include "sim/ElementIds.h"
#include "sim/GpuSave.h"
#include "sim/Scenes.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <random>
#include <unordered_map>

namespace bp {

namespace {

template <class T>
bool sameBits(const std::vector<T> &a, const std::vector<T> &b) {
    return a.size() == b.size() && (a.empty() || std::memcmp(a.data(), b.data(), a.size() * sizeof(T)) == 0);
}

bool sameParticle(const ClassicParticle &a, const ClassicParticle &b) { return std::memcmp(&a, &b, sizeof a) == 0; }

std::string describe(const ClassicParticle &p) {
    char b[256];
    std::snprintf(b, sizeof b, "type %d at (%.3f, %.3f) life %d ctype %d tmp %d tmp2 %d tmp3 %d tmp4 %d temp %.2f v (%.3f, %.3f) "
                  "flags %u dcolour %08X",
                  p.type, double(p.x), double(p.y), p.life, p.ctype, p.tmp, p.tmp2, p.tmp3, p.tmp4, double(p.temp),
                  double(p.vx), double(p.vy), p.flags, p.dcolour);
    return b;
}

// What two saves differ in first ("" if nothing)
std::string saveDiff(const SaveArea &a, const SaveArea &b) {
    if (a.cw != b.cw || a.ch != b.ch)
        return "size " + std::to_string(a.cw) + "x" + std::to_string(a.ch) + " / " + std::to_string(b.cw) + "x" +
               std::to_string(b.ch);
    if (a.contentX != b.contentX || a.contentY != b.contentY || a.contentW != b.contentW || a.contentH != b.contentH)
        return "content rectangle";
    if (a.particles.size() != b.particles.size())
        return std::to_string(a.particles.size()) + " / " + std::to_string(b.particles.size()) + " particles";
    for (size_t i = 0; i < a.particles.size(); i++)
        if (!sameParticle(a.particles[i], b.particles[i]))
            return "particle " + std::to_string(i) + ": " + describe(a.particles[i]) + " / " + describe(b.particles[i]);
    const std::pair<const char *, bool> maps[] = {
        {"walls", sameBits(a.walls, b.walls)},
        {"block air", sameBits(a.blockAir, b.blockAir)},
        {"block air heat", sameBits(a.blockAirH, b.blockAirH)},
        {"fans", sameBits(a.fanVX, b.fanVX) && sameBits(a.fanVY, b.fanVY)},
        {"pressure", sameBits(a.pv, b.pv)},
        {"velocity", sameBits(a.vx, b.vx) && sameBits(a.vy, b.vy)},
        {"ambient heat", sameBits(a.hv, b.hv)},
        {"gravity", sameBits(a.gravMass, b.gravMass) && sameBits(a.gravMask, b.gravMask) &&
                        sameBits(a.gravForceX, b.gravForceX) && sameBits(a.gravForceY, b.gravForceY)},
    };
    for (auto [name, same] : maps)
        if (!same)
            return name;
    if (a.hasPressure != b.hasPressure || a.hasAmbientHeat != b.hasAmbientHeat || a.hasBlockAirMaps != b.hasBlockAirMaps ||
        a.hasGravityMaps != b.hasGravityMaps)
        return "which maps it has";
    if (a.signs.size() != b.signs.size())
        return std::to_string(a.signs.size()) + " / " + std::to_string(b.signs.size()) + " signs";
    for (size_t i = 0; i < a.signs.size(); i++)
        if (a.signs[i].x != b.signs[i].x || a.signs[i].y != b.signs[i].y || a.signs[i].ju != b.signs[i].ju ||
            a.signs[i].text != b.signs[i].text)
            return "sign " + std::to_string(i);
    if (a.rocketBoots1 != b.rocketBoots1 || a.rocketBoots2 != b.rocketBoots2 || a.fan1 != b.fan1 || a.fan2 != b.fan2 ||
        a.rocketBootsFigh != b.rocketBootsFigh || a.fanFigh != b.fanFigh)
        return "stick men";
    return "";
}

// A world as both engines see it
struct WorldState {
    std::vector<ClassicParticle> particles; // by cell, then by their bytes
    std::vector<uint8_t> walls;
    std::vector<float> fvx, fvy, pv, vx, vy, hv;
    std::vector<Sign> signs;
};

void sortParticles(std::vector<ClassicParticle> &ps) {
    std::sort(ps.begin(), ps.end(), [](const ClassicParticle &a, const ClassicParticle &b) {
        int ay = int(a.y + 0.5f), by = int(b.y + 0.5f), ax = int(a.x + 0.5f), bx = int(b.x + 0.5f);
        if (ay != by)
            return ay < by;
        if (ax != bx)
            return ax < bx;
        return std::memcmp(&a, &b, sizeof a) < 0;
    });
}

// SOAP's links are particle ids, which differ between the engines: they are compared by the cell of the SOAP they
// join (-1 - its cell), a link to nothing as it is
void soapByCell(std::vector<ClassicParticle> &ps, const std::vector<int> &ids) {
    std::unordered_map<int, size_t> at;
    for (size_t i = 0; i < ps.size(); i++)
        at[ids[i]] = i;
    std::vector<ClassicParticle> out = ps;
    for (size_t i = 0; i < ps.size(); i++) {
        if (ps[i].type != PT_SOAP)
            continue;
        auto cell = [&](int id) {
            auto it = at.find(id);
            if (it == at.end() || ps[it->second].type != PT_SOAP)
                return id;
            const ClassicParticle &q = ps[it->second];
            return -1 - (int(q.y + 0.5f) * ClassicSim::W + int(q.x + 0.5f));
        };
        if (ps[i].ctype & 2)
            out[i].tmp = cell(ps[i].tmp);
        if (ps[i].ctype & 4)
            out[i].tmp2 = cell(ps[i].tmp2);
    }
    ps = std::move(out);
}

WorldState stateOf(ClassicSim &c) {
    WorldState s;
    std::vector<int> ids;
    s.particles = c.particles(&ids);
    soapByCell(s.particles, ids);
    sortParticles(s.particles);
    s.walls = c.walls();
    s.fvx = c.fanVX(), s.fvy = c.fanVY();
    c.airMaps(s.pv, s.vx, s.vy, s.hv);
    s.signs = c.signs();
    return s;
}

WorldState stateOf(GpuSim &g) {
    WorldState s;
    std::vector<int> ids;
    s.particles = g.readParticles(&ids);
    soapByCell(s.particles, ids);
    sortParticles(s.particles);
    s.walls = g.walls();
    g.readFans(s.fvx, s.fvy);
    g.readAir(s.pv, s.vx, s.vy, s.hv);
    s.signs = g.signs();
    return s;
}

std::string worldDiff(const WorldState &a, const WorldState &b) {
    if (a.particles.size() != b.particles.size()) {
        // the first cell they differ at
        size_t i = 0;
        while (i < a.particles.size() && i < b.particles.size() && sameParticle(a.particles[i], b.particles[i]))
            i++;
        return std::to_string(a.particles.size()) + " / " + std::to_string(b.particles.size()) +
               " particles, first differing: " + (i < a.particles.size() ? describe(a.particles[i]) : "-") + " / " +
               (i < b.particles.size() ? describe(b.particles[i]) : "-");
    }
    for (size_t i = 0; i < a.particles.size(); i++)
        if (!sameParticle(a.particles[i], b.particles[i]))
            return describe(a.particles[i]) + " / " + describe(b.particles[i]);
    if (a.walls != b.walls)
        return "walls";
    if (!sameBits(a.fvx, b.fvx) || !sameBits(a.fvy, b.fvy))
        return "fans";
    if (!sameBits(a.pv, b.pv) || !sameBits(a.vx, b.vx) || !sameBits(a.vy, b.vy) || !sameBits(a.hv, b.hv))
        return "air";
    if (a.signs.size() != b.signs.size())
        return std::to_string(a.signs.size()) + " / " + std::to_string(b.signs.size()) + " signs";
    for (size_t i = 0; i < a.signs.size(); i++)
        if (a.signs[i].x != b.signs[i].x || a.signs[i].y != b.signs[i].y || a.signs[i].text != b.signs[i].text)
            return "sign " + std::to_string(i);
    return "";
}

} // namespace

int runClipboardCheck(VkContext &ctx, ClassicSim &classic, const ElementTable &elements, const AppOptions &o) {
    const int W = ClassicSim::W, H = ClassicSim::H, CW = W / CELL, CH = H / CELL;
    int failures = 0, checks = 0;
    auto check = [&](const std::string &what, const std::string &diff) {
        checks++;
        failures += !diff.empty();
        std::printf("  %s %s%s\n", diff.empty() ? "ok  " : "FAIL", what.c_str(), diff.empty() ? "" : (": " + diff).c_str());
    };

    // The world pasted into: walls, a few elements, air with some pressure, signs
    auto base = [&] {
        classic.clear();
        classic.seed(o.seed);
        std::vector<uint8_t> walls(size_t(CW) * CH, 0);
        auto box = [&](int x0, int y0, int x1, int y1, uint8_t w) {
            for (int y = y0; y <= y1; y++)
                for (int x = x0; x <= x1; x++)
                    walls[size_t(y) * CW + size_t(x)] = w;
        };
        box(20, 20, 24, 22, WL_WALL);
        box(30, 40, 33, 41, WL_EWALL);
        box(60, 70, 66, 73, WL_FAN);
        box(120, 70, 122, 72, WL_GRAV);
        box(52, 24, 54, 25, WL_ALLOWGAS);
        classic.setWalls(walls);
        auto part = [&](int x0, int y0, int x1, int y1, int t) {
            for (int y = y0; y <= y1; y++)
                for (int x = x0; x <= x1; x++)
                    classic.createPart(x, y, t);
        };
        part(100, 200, 250, 203, PT_METL);
        part(420, 220, 520, 260, PT_WATR);
        part(100, 120, 160, 130, PT_GLAS);
        part(110, 122, 150, 128, PT_PHOT); // photons over the glass
        part(30, 300, 90, 330, PT_DUST);
        part(196, 96, 214, 112, PT_SAND);
        std::vector<float> pv, vx, vy, hv;
        classic.airMaps(pv, vx, vy, hv);
        for (int y = 0; y < CH; y++)
            for (int x = 0; x < CW; x++) {
                size_t c = size_t(y) * CW + size_t(x);
                pv[c] = float((x * 7 + y * 3) % 23) - 11.0f;
                vx[c] = float((x + y) % 5) * 0.25f, vy[c] = float(x % 3) * -0.5f, hv[c] = 300.0f + float(y);
            }
        classic.setAirMaps(pv, vx, vy, hv);
        std::vector<Sign> signs(2);
        signs[0].x = 205, signs[0].y = 245, signs[0].text = U"sign";
        signs[1].x = 500, signs[1].y = 330, signs[1].text = U"{t}";
        classic.setSigns(signs);
    };
    // ...and what is copied from: the same with stick men, linked SOAP, PIPE, QRTZ's pressure, INVIS under FILT
    auto source = [&] {
        base();
        classic.createPart(220, 100, PT_STKM); // (and its spawn point)
        classic.createPart(240, 100, PT_FIGH);
        int s0 = classic.createPart(260, 100, PT_SOAP), s1 = classic.createPart(261, 100, PT_SOAP),
            s2 = classic.createPart(262, 100, PT_SOAP);
        classic.setParticleFields(s0, 2, s1, 0, 0, 0); // (ctype 2: tmp is the next, 4: tmp2 the one before)
        classic.setParticleFields(s1, 6, s2, s0, 0, 0);
        classic.setParticleFields(s2, 4, 0, s1, 0, 0);
        int pipe = classic.createPart(270, 110, PT_PIPE);
        classic.setParticleFields(pipe, PT_WATR, 0x200 | 3 << 10 | 0x2000 | 5 << 14 | 0x40000, 0, 0, 0); // its directions
        int q = classic.createPart(275, 115, PT_QRTZ);
        classic.setParticleFields(q, 0, 0, 0, 42, 0);
        classic.createPart(280, 120, PT_INVIS);
        classic.createPartStacked(280, 120, PT_FILT);
        std::vector<Sign> signs = classic.signs();
        signs.push_back(Sign{250, 130, 0, U"copied"});
        classic.setSigns(signs);
    };

    SimConfig cfg;
    cfg.W = W;
    cfg.H = H;
    cfg.seed = o.seed;
    cfg.shaderDir = o.shaderDir;
    GpuSim gpu(ctx, cfg, elements);
    auto mirror = [&] { // the GPU world to TPT's
        SceneData state = sceneFromParticles(elements, classic.particles(), W, H, 0, 0);
        state.walls = classic.walls();
        state.signs = classic.signs();
        gpu.load(state);
        std::vector<float> fvx = classic.fanVX(), fvy = classic.fanVY();
        gpu.setWalls(classic.walls(), &fvx, &fvy);
        std::vector<float> pv, vx, vy, hv;
        classic.airMaps(pv, vx, vy, hv);
        gpu.writeAir(pv, vx, vy, hv);
    };
    auto worlds = [&] { return worldDiff(stateOf(classic), stateOf(gpu)); };

    // Copying: Simulation::Save of an area, with and without the air
    source();
    mirror();
    check("the worlds copied from", worlds());
    SaveArea copied[2];
    for (int pressure = 0; pressure < 2; pressure++) {
        SaveArea t = classic.saveArea(190, 80, 291, 141, pressure != 0);
        SaveArea g = gpuSaveArea(gpu, 291, 141, 190, 80, pressure != 0); // (corners either way round)
        check(std::string("copy of (190,80)-(291,141)") + (pressure ? " with the air: " : ": ") +
                  std::to_string(t.particles.size()) + " particles, " + std::to_string(t.cw) + "x" +
                  std::to_string(t.ch) + " cells",
              saveDiff(t, g));
        copied[pressure] = t;
    }
    {
        SaveArea t = classic.saveArea(-50, 300, 40, 500, true), g = gpuSaveArea(gpu, -50, 300, 40, 500, true);
        check("copy of an area past the corner, kept inside the world", saveDiff(t, g));
    }

    // GameSave::Transform: the port against TPT's own, with fans and velocities everywhere
    {
        SaveArea src = copied[1];
        std::mt19937 rng(7);
        std::uniform_real_distribution<float> d(-3.0f, 3.0f);
        for (size_t c = 0; c < src.cells(); c++) {
            src.fanVX[c] = d(rng), src.fanVY[c] = d(rng), src.gravForceX[c] = d(rng), src.gravForceY[c] = d(rng);
            if (c % 7 == 0)
                src.walls[c] = WL_FAN;
        }
        const std::pair<const char *, Mat2i> ms[] = {
            {"identity", MAT_IDENTITY},
            {"rotated", MAT_CCW},
            {"rotated twice", matMul(MAT_CCW, MAT_CCW)},
            {"rotated three times", matMul(MAT_CCW, matMul(MAT_CCW, MAT_CCW))},
            {"mirrored", MAT_MIRROR_X},
            {"flipped", MAT_MIRROR_Y},
            {"rotated and mirrored", matMul(MAT_MIRROR_X, MAT_CCW)},
        };
        for (auto [name, m] : ms)
            for (auto [nx, ny] : {std::pair{0, 0}, std::pair{1, 2}, std::pair{3, 3}}) {
                SaveArea a = src, b = src;
                tptTransform(a, m, nx, ny, CW, CH);
                tptTransformTpt(b, m, nx, ny);
                check(std::string("transform: ") + name + ", nudged " + std::to_string(nx) + "," + std::to_string(ny),
                      saveDiff(b, a));
            }
        // one as big as the world is cut to it
        SaveArea big;
        big.setSize(CW, CH);
        big.particles.push_back(ClassicParticle{PT_DUST, 0, 0, 600.5f, 10.5f, 0, 0, 300, 0, 0, 0, 0, 0, 0});
        SaveArea a = big, b = big;
        tptTransform(a, MAT_CCW, 2, 1, CW, CH);
        tptTransformTpt(b, MAT_CCW, 2, 1);
        check("transform: the whole world rotated (cut to it)", saveDiff(b, a));
    }

    // Pasting: Simulation::Load at places in, over and past the edges of the world, with the air and without, plain
    // and turned
    struct Paste {
        const char *what;
        int bx, by;
        bool pressure;
        Mat2i m;
    };
    const Paste pastes[] = {
        {"in the open", 10, 40, true, MAT_IDENTITY},
        {"over water, without the air", 103, 52, false, MAT_IDENTITY},
        {"over the glass and its photons", 23, 27, true, MAT_IDENTITY},
        {"past the top left corner", -10, -6, true, MAT_IDENTITY},
        {"past the bottom right corner", 140, 88, true, MAT_IDENTITY},
        {"rotated, over the metal", 25, 45, true, MAT_CCW},
        {"mirrored, nudged", 60, 10, true, MAT_MIRROR_X},
    };
    for (const Paste &p : pastes) {
        SaveArea s = copied[1];
        tptTransform(s, p.m, p.m == MAT_MIRROR_X ? 2 : 0, p.m == MAT_MIRROR_X ? 1 : 0, CW, CH);
        base();
        mirror();
        classic.loadArea(s, p.pressure, p.bx, p.by);
        gpuLoadArea(gpu, s, p.pressure, p.bx, p.by);
        check(std::string("paste ") + p.what + " at block (" + std::to_string(p.bx) + "," + std::to_string(p.by) + ")",
              worlds());
    }
    // The same twice: the stick man and its spawn point aren't pasted again while they are out; then once more where
    // the fighter lands on the first stick man (removing it, after its turn has passed)
    {
        base();
        mirror();
        const SaveArea &s = copied[0];
        for (int k = 0; k < 2; k++) {
            classic.loadArea(s, false, 5, 5);
            gpuLoadArea(gpu, s, false, 5, 5);
        }
        check("paste twice at the same place", worlds());
        classic.loadArea(s, false, 0, 5);
        gpuLoadArea(gpu, s, false, 0, 5);
        check("and once more, 20 pixels left", worlds());
        int stkm = 0;
        for (const ClassicParticle &p : classic.particles())
            stkm += p.type == PT_STKM;
        std::printf("       (%d STKM in TPT after them)\n", stkm);
    }

    // Cutting: clear_area of the area copied
    {
        source();
        mirror();
        classic.clearArea(190, 80, 291 - 190, 141 - 80);
        gpuClearArea(gpu, 190, 80, 291 - 190, 141 - 80);
        check("cut of (190,80)-(291,141): clear_area", worlds());
    }

    // A stamp bigger than TPT reads goes into our own file and comes back as it was
    {
        SaveArea big;
        big.setSize(300, 200);
        big.particles = copied[1].particles;
        big.signs = copied[1].signs;
        big.walls[1234] = WL_FAN, big.fanVX[1234] = 0.5f;
        std::vector<char> data;
        std::string err;
        SaveArea back;
        bool ok = tptWriteSave(big, data, err) && tptReadSave(data, back, err);
        check("a 1200x800 stamp written and read back (" + std::to_string(data.size()) + " bytes)",
              ok ? saveDiff(big, back) : err);
    }

    std::printf("%d of %d as expected\n", checks - failures, checks);
    return failures ? 1 : 0;
}

} // namespace bp
