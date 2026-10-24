// Accuracy check: runs the same starting state through TPT's own engine (Classic) and the GPU engine at
// TPT's size, and measures how far apart the results drift.
//
// Falling-sand simulations are chaotic, so two TPT runs that differ only in their random seed also drift
// apart. The noise floor is therefore TPT against TPT with a different seed; the GPU engine is as accurate
// as it can be when its distance from TPT is no larger than that.
#include "ui/BrushCursor.h"
#include "ui/GameInput.h"
#include "gpu/Overlay.h"
#include "ui/Dialogs.h"
#include "ui/GameUi.h"
#include "ui/Widgets.h"
#include "ui/Canvas.h"
#include "sim/WorldFile.h"
#include "app/App.h"
#include "classic/ClassicSim.h"
#include "sim/Editing.h"
#include "sim/GpuSim.h"

#include <SDL3/SDL_keycode.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstring>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <iterator>
#include <tuple>
#include <set>
#include <random>

namespace bp {

namespace {

constexpr int BIN = 8; // spatial bin size for the distribution comparison, in px

struct TypeStats {
    int count = 0;
    double sumX = 0, sumY = 0, sumY2 = 0, sumTemp = 0, sumSpeed = 0;
};

struct Snapshot {
    int tick = 0;
    int total = 0;
    int holes = 0; // empty cells whose 4 neighbours are all occupied
    std::map<int, TypeStats> types;
    std::map<int, std::vector<int>> bins;       // per type, (W/BIN)*(H/BIN) counts
    std::map<int, std::vector<double>> tempSum; // per type and bin, sum of temperatures
    std::vector<float> gfx, gfy;                 // Newtonian gravity's field
};

// How far field b is from a: the RMS of the difference over the RMS of a.
double fieldDistance(const Snapshot &a, const Snapshot &b) {
    double d = 0, n = 0;
    for (size_t i = 0; i < a.gfx.size() && i < b.gfx.size(); i++) {
        d += double(a.gfx[i] - b.gfx[i]) * (a.gfx[i] - b.gfx[i]) + double(a.gfy[i] - b.gfy[i]) * (a.gfy[i] - b.gfy[i]);
        n += double(a.gfx[i]) * a.gfx[i] + double(a.gfy[i]) * a.gfy[i];
    }
    return n > 0 ? std::sqrt(d / n) : 0.0;
}
double fieldRms(const Snapshot &a) {
    double n = 0;
    for (size_t i = 0; i < a.gfx.size(); i++)
        n += double(a.gfx[i]) * a.gfx[i] + double(a.gfy[i]) * a.gfy[i];
    return a.gfx.empty() ? 0.0 : std::sqrt(n / a.gfx.size());
}

void writePpm(const std::string &path, const std::vector<uint32_t> &px, int w, int h) {
    FILE *f = std::fopen(path.c_str(), "wb");
    if (!f)
        throw std::runtime_error("cannot write " + path);
    std::fprintf(f, "P6\n%d %d\n255\n", w, h);
    for (uint32_t c : px) {
        unsigned char rgb[3] = {uint8_t(c >> 16), uint8_t(c >> 8), uint8_t(c)};
        std::fwrite(rgb, 1, 3, f);
    }
    std::fclose(f);
    std::printf("  wrote %s\n", path.c_str());
}

Snapshot snapshot(const std::vector<ClassicParticle> &ps, int tick) {
    const int bw = (ClassicSim::W + BIN - 1) / BIN, bh = (ClassicSim::H + BIN - 1) / BIN;
    Snapshot s;
    s.tick = tick;
    for (const auto &p : ps) {
        int x = int(p.x + 0.5f), y = int(p.y + 0.5f);
        if (x < 0 || y < 0 || x >= ClassicSim::W || y >= ClassicSim::H)
            continue;
        auto &t = s.types[p.type];
        t.count++;
        t.sumX += p.x;
        t.sumY += p.y;
        t.sumY2 += double(p.y) * p.y;
        t.sumTemp += p.temp;
        t.sumSpeed += std::sqrt(double(p.vx) * p.vx + double(p.vy) * p.vy);
        auto &b = s.bins[p.type];
        auto &ts = s.tempSum[p.type];
        if (b.empty()) {
            b.assign(size_t(bw) * bh, 0);
            ts.assign(size_t(bw) * bh, 0.0);
        }
        b[size_t(y / BIN) * bw + x / BIN]++;
        ts[size_t(y / BIN) * bw + x / BIN] += p.temp;
        s.total++;
    }
    std::vector<uint8_t> occ(size_t(ClassicSim::W) * ClassicSim::H, 0);
    for (const auto &p : ps) {
        int x = int(p.x + 0.5f), y = int(p.y + 0.5f);
        if (x >= 0 && y >= 0 && x < ClassicSim::W && y < ClassicSim::H)
            occ[size_t(y) * ClassicSim::W + x] = 1;
    }
    for (int y = 1; y < ClassicSim::H - 1; y++)
        for (int x = 1; x < ClassicSim::W - 1; x++) {
            size_t c = size_t(y) * ClassicSim::W + x;
            if (!occ[c] && occ[c - 1] && occ[c + 1] && occ[c - ClassicSim::W] && occ[c + ClassicSim::W])
                s.holes++;
        }
    return s;
}

// How far apart the temperatures are: the mean |difference| of the average temperature per (element, bin),
// over bins both have, weighted by the smaller particle count. In kelvin.
double tempDistance(const Snapshot &a, const Snapshot &b) {
    double diff = 0, weight = 0;
    for (auto &[t, ca] : a.bins) {
        auto cb = b.bins.find(t);
        if (cb == b.bins.end())
            continue;
        const auto &ta = a.tempSum.at(t), &tb = b.tempSum.at(t);
        for (size_t i = 0; i < ca.size(); i++) {
            if (!ca[i] || !cb->second[i])
                continue;
            double w = std::min(ca[i], cb->second[i]);
            diff += w * std::abs(ta[i] / ca[i] - tb[i] / cb->second[i]);
            weight += w;
        }
    }
    return weight > 0 ? diff / weight : 0.0;
}

// Total variation distance between the two (element, position) distributions at BIN resolution:
// 0 = the same, 1 = nothing in common.
double distance(const Snapshot &a, const Snapshot &b) {
    double diff = 0;
    std::map<int, int> keys;
    for (auto &[t, v] : a.bins)
        keys[t] = 1;
    for (auto &[t, v] : b.bins)
        keys[t] = 1;
    for (auto &[t, unused] : keys) {
        auto ia = a.bins.find(t), ib = b.bins.find(t);
        size_t n = ia != a.bins.end() ? ia->second.size() : ib->second.size();
        for (size_t i = 0; i < n; i++) {
            int va = ia != a.bins.end() ? ia->second[i] : 0;
            int vb = ib != b.bins.end() ? ib->second[i] : 0;
            diff += std::abs(va - vb);
        }
    }
    int tot = a.total + b.total;
    return tot ? diff / tot : 0.0;
}

// Runs TPT after `setup` has built the starting state, with the given seed, and returns snapshots at the
// checkpoints.
template <class Setup>
std::vector<Snapshot> runClassic(ClassicSim &sim, Setup &&setup, uint32_t seed, const std::vector<int> &checkpoints,
                                 double &msPerTick, const std::string &imagePath = "") {
    setup();
    sim.seed(seed);
    std::vector<Snapshot> out;
    int tick = 0;
    auto t0 = std::chrono::steady_clock::now();
    for (int cp : checkpoints) {
        while (tick < cp) {
            sim.step();
            tick++;
        }
        out.push_back(snapshot(sim.particles(), tick));
        sim.gravityField(out.back().gfx, out.back().gfy);
    }
    double s = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    msPerTick = tick ? 1000.0 * s / tick : 0;
    if (!imagePath.empty()) {
        std::vector<uint32_t> px;
        sim.render(4, px); // "Fire Display", TPT's default
        writePpm(imagePath, px, ClassicSim::W, ClassicSim::H);
    }
    return out;
}

std::vector<Snapshot> runGpu(VkContext &ctx, GpuSim &sim, const ElementTable &elements,
                             const std::vector<ClassicParticle> &initial, const std::vector<uint8_t> &walls,
                             const std::vector<int> &checkpoints, const std::string &imagePath = "") {
    SceneData scene = sceneFromParticles(elements, initial, ClassicSim::W, ClassicSim::H, 0, 0);
    scene.walls = walls;
    sim.load(scene);
    std::vector<Snapshot> out;
    int tick = 0;
    for (int cp : checkpoints) {
        while (tick < cp) {
            int k = std::min(10, cp - tick);
            ctx.immediate([&](VkCommandBuffer cmd) {
                for (int i = 0; i < k; i++)
                    sim.recordTick(cmd);
            });
            tick += k;
        }
        out.push_back(snapshot(sim.readParticles(), tick));
        sim.readGravityField(out.back().gfx, out.back().gfy);
    }
    if (!imagePath.empty()) {
        sim.ensureFrameBuffer(ClassicSim::W, ClassicSim::H);
        RenderParams rp;
        rp.screenW = ClassicSim::W;
        rp.screenH = ClassicSim::H;
        ctx.immediate([&](VkCommandBuffer cmd) { sim.recordRender(cmd, rp); });
        std::vector<uint32_t> px(size_t(ClassicSim::W) * ClassicSim::H);
        ctx.download(sim.frameBuffer(), px.data(), px.size() * 4);
        for (auto &c : px)
            c &= 0xFFFFFF;
        writePpm(imagePath, px, ClassicSim::W, ClassicSim::H);
    }
    return out;
}

void printTypes(const char *label, const Snapshot &s, const ClassicSim &names) {
    std::printf("    %-5s", label);
    for (auto &[t, st] : s.types) {
        double my = st.sumY / st.count;
        double sd = std::sqrt(std::max(0.0, st.sumY2 / st.count - my * my));
        std::printf("  %s %d (y %.1f±%.1f, %.0fK, |v| %.3f)", names.elementName(t).c_str(), st.count, my, sd,
                    st.sumTemp / st.count, st.sumSpeed / st.count);
    }
    std::printf("  holes %d\n", s.holes);
}

} // namespace

// Puts the same pressure blob into both engines' air (no particles) and compares the fields tick by tick.
int runAirTest(VkContext &ctx, ClassicSim &classic, const ElementTable &elements, const AppOptions &o) {
    const int CW = ClassicSim::W / ClassicSim::CELL, CH = ClassicSim::H / ClassicSim::CELL;
    std::vector<float> pv(size_t(CW) * CH, 0.0f), vx = pv, vy = pv, hv(pv.size(), 295.15f);
    if (!std::getenv("BP_HEAT_ONLY")) {
        for (int y = CH / 2 - 3; y <= CH / 2 + 3; y++)
            for (int x = CW / 3 - 3; x <= CW / 3 + 3; x++)
                pv[size_t(y) * CW + x] = 40.0f;
        for (int y = CH / 3; y < CH / 3 + 6; y++)
            for (int x = CW / 2; x < CW / 2 + 20; x++)
                vx[size_t(y) * CW + x] = 3.0f;
    }
    for (int y = CH / 2; y < CH / 2 + 8; y++)
        for (int x = 2 * CW / 3; x < 2 * CW / 3 + 8; x++)
            hv[size_t(y) * CW + x] = 1500.0f;
    classic.clear();
    ClassicOptions copts;
    copts.ambientHeat = o.ambientHeat;
    copts.newtonianGravity = o.newtonian;
    classic.setOptions(copts);
    classic.setAirMaps(pv, vx, vy, hv);
    SimConfig cfg;
    cfg.W = ClassicSim::W;
    cfg.H = ClassicSim::H;
    cfg.shaderDir = o.shaderDir;
    cfg.settings.ambientHeat = o.ambientHeat;
    cfg.settings.newtonianGravity = o.newtonian;
    GpuSim gpu(ctx, cfg, elements);
    gpu.load(sceneFromParticles(elements, {}, ClassicSim::W, ClassicSim::H, 0, 0));
    gpu.writeAir(pv, vx, vy, hv);
    std::printf("air test: pressure blob and a jet, no particles. Max |TPT - GPU| over all cells:\n");
    std::printf("  %6s %12s %12s %12s %12s   %s\n", "tick", "pressure", "vel x", "vel y", "heat", "(max |TPT pressure|)");
    for (int t = 1; t <= o.ticks; t++) {
        classic.step();
        ctx.immediate([&](VkCommandBuffer cmd) { gpu.recordTick(cmd); });
        if (t % o.interval == 0 || t == 1) {
            std::vector<float> a[4], b[4];
            classic.airMaps(a[0], a[1], a[2], a[3]);
            gpu.readAir(b[0], b[1], b[2], b[3]);
            double d[4] = {0, 0, 0, 0}, pmax = 0;
            for (int k = 0; k < 4; k++)
                for (size_t i = 0; i < a[k].size(); i++)
                    d[k] = std::max(d[k], double(std::abs(a[k][i] - b[k][i])));
            for (float v : a[0])
                pmax = std::max(pmax, double(std::abs(v)));
            std::printf("  %6d %12.3g %12.3g %12.3g %12.3g   (%.3g)\n", t, d[0], d[1], d[2], d[3], pmax);
            if (t == 1 && d[3] > 0.01) {
                size_t worst = 0;
                for (size_t i = 0; i < a[3].size(); i++)
                    if (std::abs(a[3][i] - b[3][i]) > std::abs(a[3][worst] - b[3][worst]))
                        worst = i;
                std::printf("         worst heat cell (%zu, %zu): TPT %.3f GPU %.3f\n", worst % CW, worst / CW,
                            a[3][worst], b[3][worst]);
                for (int yy = 53; yy <= 56; yy++)
                    for (int xx = 102; xx <= 104; xx++) {
                        size_t k = size_t(yy) * CW + xx;
                        std::printf("         (%d,%d) vy TPT %.5f GPU %.5f  heat TPT %.3f GPU %.3f\n", xx, yy, a[2][k],
                                    b[2][k], a[3][k], b[3][k]);
                    }
            }
        }
    }
    return 0;
}

// --check-save: pastes a TPT save into an empty GPU world the way the viewer does (GpuSim::insertParticles,
// stampWalls, stampAir) and checks the result against TPT's own load of it: the particles (type and cell), SOAP's
// links (compared by the cells they join, since ids differ) and the air. Then runs both and compares as usual.
int runSaveCheck(VkContext &ctx, ClassicSim &classic, const ElementTable &elements, const AppOptions &o) {
    std::string err;
    if (!classic.loadFile(o.checkSave, err)) {
        std::fprintf(stderr, "could not load %s: %s\n", o.checkSave.c_str(), err.c_str());
        return 1;
    }
    classic.seed(o.seed);
    std::vector<int> ids;
    std::vector<ClassicParticle> ps = classic.particles(&ids);
    std::vector<ClassicParticle> sup;
    std::vector<int> supIds;
    std::map<int, int> left;
    for (size_t i = 0; i < ps.size(); i++) {
        if (elements.enabled(ps[i].type)) {
            sup.push_back(ps[i]);
            supIds.push_back(ids[i]);
        } else
            left[ps[i].type]++;
    }
    SimConfig cfg;
    cfg.W = ClassicSim::W;
    cfg.H = ClassicSim::H;
    cfg.seed = o.seed;
    cfg.shaderDir = o.shaderDir;
    GpuSim gpu(ctx, cfg, elements);
    SceneData empty;
    empty.W = cfg.W;
    empty.H = cfg.H;
    empty.pmap.assign(size_t(cfg.W) * cfg.H, 0);
    gpu.load(empty);
    const int CW = ClassicSim::W / CELL, CH = ClassicSim::H / CELL;
    gpu.stampWalls(classic.walls(), CW, CH, 0, 0);
    gpu.insertParticles(sup, 0, 0, &supIds);
    gpu.addSigns(classic.signs(), 0, 0);
    {
        std::vector<Sign> a = classic.signs(), b = gpu.signs();
        int same = 0;
        for (size_t i = 0; i < a.size() && i < b.size(); i++)
            same += a[i].x == b[i].x && a[i].y == b[i].y && a[i].ju == b[i].ju && a[i].text == b[i].text;
        std::printf("signs: %zu in TPT, %zu pasted, %d the same\n", a.size(), b.size(), same);
    }
    std::vector<float> pv, vx, vy, hv, gpv, gvx, gvy, ghv;
    classic.airMaps(pv, vx, vy, hv);
    gpu.stampAir(pv, vx, vy, hv, CW, 0, 0, classic.loadedCW(), classic.loadedCH(), 0, 0);

    // Particles by (type, cell)
    std::vector<ClassicParticle> gp = gpu.readParticles(); // (ids are positions in this list only if no gaps)
    std::map<std::tuple<int, int, int>, int> cells;
    auto cellOf = [](const ClassicParticle &p) { return std::pair{int(p.x + 0.5f), int(p.y + 0.5f)}; };
    for (const auto &p : sup) {
        auto [x, y] = cellOf(p);
        cells[{p.type, x, y}]++;
    }
    for (const auto &p : gp) {
        auto [x, y] = cellOf(p);
        cells[{p.type, x, y}]--;
    }
    int mismatched = 0;
    for (auto &[k, v] : cells)
        mismatched += std::abs(v);
    std::printf("%s: %zu particles in TPT (%zu the GPU runs), %zu pasted; %d (type, cell) mismatches\n",
                o.checkSave.c_str(), ps.size(), sup.size(), gp.size(), mismatched);
    for (auto &[t, n] : left)
        std::printf("  not on the GPU: %s %d\n", classic.elementName(t).c_str(), n);

    // SOAP links as pairs of cells
    auto soapLinks = [&](const std::vector<ClassicParticle> &list, const std::vector<int> &idOf) {
        std::map<int, size_t> index;
        for (size_t i = 0; i < list.size(); i++)
            index[idOf[i]] = i;
        std::multiset<std::tuple<int, int, int, int, int>> links;
        for (const auto &p : list) {
            if (p.type != PT_SOAP)
                continue;
            auto [x, y] = cellOf(p);
            for (int k = 0; k < 2; k++) {
                if (!(p.ctype & (k ? 4 : 2)))
                    continue;
                auto it = index.find(k ? p.tmp2 : p.tmp);
                if (it == index.end() || list[it->second].type != PT_SOAP)
                    links.insert({k, x, y, -1, -1});
                else {
                    auto [tx, ty] = cellOf(list[it->second]);
                    links.insert({k, x, y, tx, ty});
                }
            }
        }
        return links;
    };
    std::vector<int> gpuIds;
    gpu.readParticles(&gpuIds);
    auto tl = soapLinks(sup, supIds), gl = soapLinks(gp, gpuIds);
    std::vector<std::tuple<int, int, int, int, int>> diff;
    std::set_symmetric_difference(tl.begin(), tl.end(), gl.begin(), gl.end(), std::back_inserter(diff));
    std::printf("  SOAP links: TPT %zu, GPU %zu, differing %zu\n", tl.size(), gl.size(), diff.size());

    gpu.readAir(gpv, gvx, gvy, ghv);
    float worst = 0;
    for (size_t i = 0; i < pv.size(); i++)
        worst = std::max({worst, std::abs(pv[i] - gpv[i]), std::abs(vx[i] - gvx[i]), std::abs(vy[i] - gvy[i]),
                          std::abs(hv[i] - ghv[i])});
    std::printf("  air: largest difference %.6f\n", worst);

    // Then both run on
    std::printf("  %6s %10s %10s %12s %9s\n", "tick", "TPT parts", "GPU parts", "TPT vs GPU", "dT GPU");
    int tick = 0;
    for (int cp = o.interval; cp <= o.ticks; cp += o.interval) {
        while (tick < cp) {
            int k = std::min(10, cp - tick);
            for (int i = 0; i < k; i++)
                classic.step();
            ctx.immediate([&](VkCommandBuffer cmd) {
                for (int i = 0; i < k; i++)
                    gpu.recordTick(cmd);
            });
            tick += k;
        }
        Snapshot a = snapshot(classic.particles(), tick), g = snapshot(gpu.readParticles(), tick);
        std::printf("  %6d %10d %10d %11.1f%% %9.1f\n", tick, a.total, g.total, 100 * distance(a, g), tempDistance(a, g));
        if (cp + o.interval > o.ticks) {
            printTypes("TPT", a, classic);
            printTypes("GPU", g, classic);
        }
    }
    return 0;
}

// --check-world PATH: runs each scene on the GPU for --ticks, saves it as a world file (PATH), loads that into a
// new GPU world and checks that everything saved came back exactly; then runs both 50 ticks more, one particle at a
// time (so the runs are deterministic), and checks they are still the same.
int runWorldCheck(VkContext &ctx, const ElementTable &elements, const AppOptions &o) {
    int failures = 0;
    for (const std::string &scene : o.scenes) {
        SimConfig cfg;
        cfg.W = o.W;
        cfg.H = o.H;
        cfg.seed = o.seed;
        cfg.shaderDir = o.shaderDir;
        cfg.settings.gravityMode = uint32_t(o.gravityMode);
        cfg.settings.edgeMode = uint32_t(o.edgeMode);
        cfg.settings.ambientHeat = o.ambientHeat;
        cfg.settings.newtonianGravity = o.newtonian;
        GpuSim a(ctx, cfg, elements);
        a.load(makeScene(elements, scene, cfg.W, cfg.H, o.seed, o.fill));
        auto run = [&](GpuSim &sim, int ticks) {
            for (int t = 0; t < ticks; t += 10)
                ctx.immediate([&](VkCommandBuffer cmd) {
                    for (int k = 0; k < 10 && t + k < ticks; k++)
                        sim.recordTick(cmd);
                });
        };
        run(a, o.ticks);
        std::string err;
        auto t0 = std::chrono::steady_clock::now();
        if (!saveWorld(a, o.checkWorld, err)) {
            std::printf("%s: %s\n", scene.c_str(), err.c_str());
            return 1;
        }
        double saveMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        SimConfig cfgB = cfg;
        cfgB.seed = o.seed + 77; // (the file's seed must win)
        cfgB.settings = SimSettings{};
        GpuSim b(ctx, cfgB, elements);
        t0 = std::chrono::steady_clock::now();
        if (!loadWorld(b, o.checkWorld, err)) {
            std::printf("%s: %s\n", scene.c_str(), err.c_str());
            return 1;
        }
        double loadMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        auto particles = [](GpuSim &sim) {
            std::vector<std::tuple<uint32_t, uint32_t, std::array<uint32_t, 13>>> out;
            sim.forEachParticle([&](uint32_t id, uint32_t type, const Particle &p) {
                std::array<uint32_t, 13> f;
                std::memcpy(f.data(), &p, sizeof f); // life .. dcolour
                out.push_back({id, type, f});
            });
            return out;
        };
        auto compare = [&](const char *when, GpuSim &a, GpuSim &b) {
            auto pa = particles(a), pb = particles(b);
            size_t differ = 0;
            for (size_t k = 0; k < std::min(pa.size(), pb.size()); k++)
                differ += pa[k] != pb[k];
            differ += std::max(pa.size(), pb.size()) - std::min(pa.size(), pb.size());
            // the same particles, whatever their ids?
            auto unordered = [](auto v) {
                std::vector<std::pair<uint32_t, std::array<uint32_t, 13>>> out;
                for (auto &[id, type, f] : v)
                    out.push_back({type, f});
                std::sort(out.begin(), out.end());
                return out;
            };
            bool sameSet = unordered(pa) == unordered(pb);
            std::vector<float> a1, a2, a3, a4, b1, b2, b3, b4, fa1, fa2, fb1, fb2;
            a.readAir(a1, a2, a3, a4);
            b.readAir(b1, b2, b3, b4);
            a.readFans(fa1, fa2);
            b.readFans(fb1, fb2);
            bool air = a1 == b1 && a2 == b2 && a3 == b3 && a4 == b4;
            bool walls = a.walls() == b.walls() && fa1 == fb1 && fa2 == fb2;
            bool grav = a.readGravityCells() == b.readGravityCells();
            bool stkm = a.readStickmen() == b.readStickmen();
            bool signs = a.signs().size() == b.signs().size();
            for (size_t k = 0; signs && k < a.signs().size(); k++)
                signs = a.signs()[k].x == b.signs()[k].x && a.signs()[k].y == b.signs()[k].y &&
                        a.signs()[k].ju == b.signs()[k].ju && a.signs()[k].text == b.signs()[k].text;
            const SimSettings &sa = a.settings(), &sb = b.settings();
            bool rest = a.tick() == b.tick() && a.seed() == b.seed() && sa.edgeMode == sb.edgeMode &&
                        sa.gravityMode == sb.gravityMode && sa.customGravX == sb.customGravX &&
                        sa.customGravY == sb.customGravY && sa.ambientHeat == sb.ambientHeat && sa.heat == sb.heat &&
                        sa.waterEqualisation == sb.waterEqualisation && sa.convectionMode == sb.convectionMode &&
                        sa.newtonianGravity == sb.newtonianGravity;
            bool ok = !differ && air && walls && grav && stkm && signs && rest;
            failures += !ok;
            std::printf("  %-6s %zu particles, %zu differ by id (%s as a set); air %s, walls %s, gravity %s, stick men %s, signs %s, "
                        "tick/seed/settings %s\n",
                        when, pa.size(), differ, sameSet ? "same" : "DIFFERENT", air ? "same" : "DIFFER", walls ? "same" : "DIFFER",
                        grav ? "same" : "DIFFER", stkm ? "same" : "DIFFER", signs ? "same" : "DIFFER",
                        rest ? "same" : "DIFFER");
        };
        FILE *f = std::fopen(o.checkWorld.c_str(), "rb");
        long bytes = 0;
        if (f) {
            std::fseek(f, 0, SEEK_END);
            bytes = std::ftell(f);
            std::fclose(f);
        }
        std::printf("== %s at %ux%u after %d ticks: %.1f MB, saved in %.0f ms, loaded in %.0f ms\n", scene.c_str(),
                    cfg.W, cfg.H, o.ticks, bytes / 1e6, saveMs, loadMs);
        compare("loaded", a, b);
        if (a.readStats().total > 2000000) {
            std::printf("  (big world: not run on one particle at a time)\n");
            continue;
        }
        // A control: the file loaded a second time, run alongside. Where it differs from b too, the engine's runs
        // aren't repeatable there (new particles' ids come from a free list built in parallel), so the comparison
        // with the original says nothing about the file.
        GpuSim c(ctx, cfgB, elements);
        loadWorld(c, o.checkWorld, err);
        for (GpuSim *s : {&a, &b, &c}) {
            s->setSerial(true);
            run(*s, 50);
        }
        compare("+50", a, b);
        compare("ctrl", b, c);
    }
    return failures ? 1 : 0;
}

// --check-export PATH: runs each scene on the GPU for --ticks, writes a 612x384 area of it as a TPT save, loads that
// with TPT's own loader and compares what TPT got with what was in the area.
int runExportCheck(VkContext &ctx, ClassicSim &classic, const ElementTable &elements, const AppOptions &o) {
    int failures = 0;
    for (const std::string &scene : o.scenes) {
        SimConfig cfg;
        cfg.W = o.W;
        cfg.H = o.H;
        cfg.seed = o.seed;
        cfg.shaderDir = o.shaderDir;
        GpuSim gpu(ctx, cfg, elements);
        gpu.load(makeScene(elements, scene, cfg.W, cfg.H, o.seed, o.fill));
        for (int t = 0; t < o.ticks; t += 10)
            ctx.immediate([&](VkCommandBuffer cmd) {
                for (int k = 0; k < 10 && t + k < o.ticks; k++)
                    gpu.recordTick(cmd);
            });
        const int w = ClassicSim::W, h = ClassicSim::H;
        // the scene's middle in big worlds (the built-in scenes there are scaled), one cell in otherwise
        int x = cfg.W >= 2 * ClassicSim::W ? int(cfg.W) / 2 - w / 2 : CELL;
        int y = cfg.H >= 2 * ClassicSim::H ? int(cfg.H) / 2 - h / 2 : 2 * CELL;
        x -= x % CELL, y -= y % CELL; // (a whole-cell area, so it fits in a TPT save)
        std::string msg;
        if (!exportTptArea(gpu, x, y, std::min(w, int(cfg.W)), std::min(h, int(cfg.H)), o.checkExport, msg)) {
            std::printf("%s: %s\n", scene.c_str(), msg.c_str());
            return 1;
        }
        std::string err;
        classic.clear();
        if (!classic.loadFile(o.checkExport, err)) {
            std::printf("%s: TPT could not load it: %s\n", scene.c_str(), err.c_str());
            return 1;
        }
        // TPT's saves keep positions in whole pixels, and temperatures and velocities as GameSave::serialiseOPS
        // encodes them; the GPU's particles are put through the same encoding (TPT loses the rest with its own saves)
        using Key = std::tuple<int, int, int, float, float, float, int, int, int, int, int, uint32_t>;
        auto opsVelocity = [](float v) {
            if (std::fabs(v) <= 0.001f)
                return 0.0f;
            int b = std::clamp(int(v * 16.0f + 127.5f), 0, 255);
            return (float(b) - 127.0f) / 16.0f;
        };
        auto opsTemp = [](float t) {
            if (std::fabs(t - 294.15f) < 127)
                return float(int(std::floor(t - 294.15f + 0.5f))) + 294.15f;
            return float(int(t + 0.5f));
        };
        // (life is kept as 0..65535; FIGH's tmp is its fighter slot, which TPT gives out again when loading)
        auto key = [&](const ClassicParticle &p, float dx, float dy, bool encode) {
            return Key{p.type, int(p.x - dx + 0.5f), int(p.y - dy + 0.5f), encode ? opsVelocity(p.vx) : p.vx,
                       encode ? opsVelocity(p.vy) : p.vy, encode ? opsTemp(p.temp) : p.temp,
                       encode ? std::clamp(p.life, 0, 0xFFFF) : p.life, p.type == PT_SOAP ? p.ctype & ~6 : p.ctype,
                       p.type == PT_FIGH || p.type == PT_SOAP ? 0 : p.tmp, p.type == PT_SOAP ? 0 : p.tmp2, p.tmp4,
                       p.dcolour};
        };
        // SOAP's forward links (tmp: a particle id, renumbered by saving and loading) as pairs of positions; a link
        // to a particle outside the area goes, as with Simulation::Save. (OPS keeps only these: loading rebuilds the
        // back links in tmp2 from them.)
        using Link = std::tuple<int, int, int, int, int>;
        auto links = [](const std::vector<ClassicParticle> &ps, const std::vector<int> &ids, int dx, int dy,
                        auto inside) {
            std::map<int, std::pair<int, int>> at;
            for (size_t k = 0; k < ps.size(); k++)
                if (inside(ps[k]))
                    at[ids[k]] = {int(ps[k].x + 0.5f) - dx, int(ps[k].y + 0.5f) - dy};
            std::multiset<Link> out;
            for (size_t k = 0; k < ps.size(); k++) {
                const ClassicParticle &p = ps[k];
                if (p.type != PT_SOAP || !inside(p))
                    continue;
                auto [x0, y0] = at[ids[k]];
                for (int which : {2})
                    if (p.ctype & which) {
                        auto it = at.find(which == 2 ? p.tmp : p.tmp2);
                        if (it != at.end())
                            out.insert({which, x0, y0, it->second.first, it->second.second});
                    }
            }
            return out;
        };
        std::multiset<Key> want, got;
        std::vector<int> gIds, tIds;
        std::vector<ClassicParticle> gps = gpu.readParticles(&gIds), tps = classic.particles(&tIds);
        auto inArea = [&](const ClassicParticle &p) {
            int px = int(p.x + 0.5f), py = int(p.y + 0.5f);
            return px >= x && py >= y && px < x + w && py < y + h && elements.elements[p.type].existsInTpt;
        };
        for (const auto &p : gps)
            if (inArea(p))
                want.insert(key(p, float(x), float(y), true));
        for (const auto &p : tps)
            got.insert(key(p, 0.0f, 0.0f, false));
        auto gLinks = links(gps, gIds, x, y, inArea);
        auto tLinks = links(tps, tIds, 0, 0, [](const ClassicParticle &) { return true; });
        bool linksSame = gLinks == tLinks;
        if (!linksSame) {
            std::vector<Link> onlyG, onlyT;
            std::set_difference(gLinks.begin(), gLinks.end(), tLinks.begin(), tLinks.end(), std::back_inserter(onlyG));
            std::set_difference(tLinks.begin(), tLinks.end(), gLinks.begin(), gLinks.end(), std::back_inserter(onlyT));
            std::printf("  SOAP links: GPU %zu, TPT %zu; only GPU %zu, only TPT %zu\n", gLinks.size(), tLinks.size(),
                        onlyG.size(), onlyT.size());
            for (size_t k = 0; k < std::min<size_t>(3, onlyG.size()); k++)
                std::printf("    only GPU: %d (%d,%d)->(%d,%d)\n", std::get<0>(onlyG[k]), std::get<1>(onlyG[k]),
                            std::get<2>(onlyG[k]), std::get<3>(onlyG[k]), std::get<4>(onlyG[k]));
            for (size_t k = 0; k < std::min<size_t>(3, onlyT.size()); k++)
                std::printf("    only TPT: %d (%d,%d)->(%d,%d)\n", std::get<0>(onlyT[k]), std::get<1>(onlyT[k]),
                            std::get<2>(onlyT[k]), std::get<3>(onlyT[k]), std::get<4>(onlyT[k]));
        }
        std::vector<Key> missing, extra;
        std::set_difference(want.begin(), want.end(), got.begin(), got.end(), std::back_inserter(missing));
        std::set_difference(got.begin(), got.end(), want.begin(), want.end(), std::back_inserter(extra));
        int wallsDiffer = 0;
        std::vector<uint8_t> tw = classic.walls();
        const int CW = int(cfg.W) / CELL, tcw = ClassicSim::W / CELL;
        for (int cy = 0; cy < h / CELL && cy + y / CELL < int(cfg.H) / CELL; cy++)
            for (int cx = 0; cx < w / CELL && cx + x / CELL < CW; cx++)
                wallsDiffer += tw[size_t(cy) * tcw + cx] != gpu.walls()[size_t(cy + y / CELL) * CW + cx + x / CELL];
        size_t signsIn = 0;
        for (const Sign &s : gpu.signs())
            signsIn += s.x >= x && s.y >= y && s.x < x + w && s.y < y + h;
        bool ok = missing.empty() && extra.empty() && !wallsDiffer && linksSame &&
                  classic.signs().size() == std::min<size_t>(signsIn, 16);
        failures += !ok;
        std::printf("== %s at %ux%u, area (%d, %d): %s\n  %zu particles in the area, TPT loaded %zu: %zu missing, %zu "
                    "extra; %d walls differ; SOAP links %s; signs %zu of %zu\n",
                    scene.c_str(), cfg.W, cfg.H, x, y, msg.c_str(), want.size(), got.size(), missing.size(),
                    extra.size(), wallsDiffer, linksSame ? "same" : "DIFFER", classic.signs().size(), signsIn);
        auto show = [&](const char *what, const Key &k) {
            std::printf("    %s %s at %d,%d v %g,%g temp %g life %d ctype %d tmp %d tmp2 %d tmp4 %d dcolour %08x\n", what,
                        classic.elementName(std::get<0>(k)).c_str(), std::get<1>(k), std::get<2>(k), std::get<3>(k),
                        std::get<4>(k), std::get<5>(k), std::get<6>(k), std::get<7>(k), std::get<8>(k), std::get<9>(k),
                        std::get<10>(k), std::get<11>(k));
        };
        for (size_t k = 0; k < std::min<size_t>(2, missing.size()); k++) {
            show("missing", missing[k]);
            for (const Key &e : extra)
                if (std::get<0>(e) == std::get<0>(missing[k]) && std::get<1>(e) == std::get<1>(missing[k]) &&
                    std::get<2>(e) == std::get<2>(missing[k])) {
                    show("  TPT", e);
                    break;
                }
        }
    }
    return failures ? 1 : 0;
}

// --sleep-stats: how much of each scene could sleep exactly: after --ticks, the share of particles in 64x64 chunks
// where nothing (particles in and next to the chunk, its air) changed at all over 1 and over 10 more ticks.
int runSleepStats(VkContext &ctx, const ElementTable &elements, const AppOptions &o) {
    for (const std::string &scene : o.scenes) {
        SimConfig cfg;
        cfg.W = o.W;
        cfg.H = o.H;
        cfg.seed = o.seed;
        cfg.shaderDir = o.shaderDir;
        GpuSim gpu(ctx, cfg, elements);
        gpu.load(makeScene(elements, scene, cfg.W, cfg.H, o.seed, o.fill));
        auto run = [&](int ticks) {
            for (int t = 0; t < ticks; t += 10)
                ctx.immediate([&](VkCommandBuffer cmd) {
                    for (int k = 0; k < 10 && t + k < ticks; k++)
                        gpu.recordTick(cmd);
                });
        };
        struct Snap {
            std::vector<uint32_t> type;
            std::vector<Particle> parts;
            std::vector<float> pv, vx, vy, hv;
        };
        auto snap = [&] {
            Snap s;
            s.type.assign(gpu.capacity(), 0);
            s.parts.assign(gpu.capacity(), Particle{});
            gpu.forEachParticle([&](uint32_t id, uint32_t type, const Particle &p) {
                s.type[id] = type;
                s.parts[id] = p;
            });
            gpu.readAir(s.pv, s.vx, s.vy, s.hv);
            return s;
        };
        run(o.ticks);
        Snap a = snap();
        const int CH = 64, ncx = int(cfg.W + CH - 1) / CH, ncy = int(cfg.H + CH - 1) / CH;
        auto measure = [&](const Snap &b, const char *label) {
            std::vector<uint8_t> changed(size_t(ncx) * ncy, 0);
            std::vector<uint32_t> count(size_t(ncx) * ncy, 0);
            auto mark = [&](float fx, float fy) {
                int x = int(fx + 0.5f), y = int(fy + 0.5f);
                // the chunk and, within 2 px of its edge, its neighbours (what reaches into a chunk)
                for (int dy = -2; dy <= 2; dy += 2)
                    for (int dx = -2; dx <= 2; dx += 2) {
                        int cx = (x + dx) / CH, cy = (y + dy) / CH;
                        if (x + dx >= 0 && y + dy >= 0 && cx < ncx && cy < ncy)
                            changed[size_t(cy) * ncx + cx] = 1;
                    }
            };
            size_t live = 0;
            for (size_t i = 0; i < a.type.size(); i++) {
                bool la = a.type[i] != 0, lb = b.type[i] != 0;
                if (lb) {
                    live++;
                    int x = int(b.parts[i].x + 0.5f), y = int(b.parts[i].y + 0.5f);
                    if (x >= 0 && y >= 0 && x / CH < ncx && y / CH < ncy)
                        count[size_t(y / CH) * ncx + x / CH]++;
                }
                if (la != lb || (la && (a.type[i] != b.type[i] ||
                                        std::memcmp(&a.parts[i], &b.parts[i], 52) != 0))) { // life .. dcolour
                    if (la)
                        mark(a.parts[i].x, a.parts[i].y);
                    if (lb)
                        mark(b.parts[i].x, b.parts[i].y);
                }
            }
            const int CW = int(cfg.W) / CELL;
            for (size_t c = 0; c < a.pv.size(); c++)
                if (a.pv[c] != b.pv[c] || a.vx[c] != b.vx[c] || a.vy[c] != b.vy[c] || a.hv[c] != b.hv[c])
                    mark(float(int(c % size_t(CW)) * CELL + 1), float(int(c / size_t(CW)) * CELL + 1));
            size_t still = 0, stillChunks = 0, chunks = 0;
            for (size_t c = 0; c < changed.size(); c++) {
                chunks += count[c] > 0;
                if (!changed[c] && count[c]) {
                    still += count[c];
                    stillChunks++;
                }
            }
            std::printf("  %-10s %zu particles; in chunks where nothing changed: %.1f%% (%zu of %zu chunks with particles)\n",
                        label, live, live ? 100.0 * double(still) / double(live) : 0.0, stillChunks, chunks);
        };
        std::printf("== %s at %ux%u after %d ticks\n", scene.c_str(), cfg.W, cfg.H, o.ticks);
        run(1);
        measure(snap(), "+1 tick");
        run(9);
        measure(snap(), "+10 ticks");
    }
    return 0;
}

// --check-tools: uses TPT's tools the same way in TPT (its own tool code) and on the GPU (sim/Editing), each case
// from the same start (particles, photons over glass, walls, a fan, signs), and compares what they made: each cell's
// particles and their fields (fields TPT sets at random are counted apart), the walls, the fans' velocities and
// the signs.
int runToolCheck(VkContext &ctx, ClassicSim &classic, const ElementTable &elements, const AppOptions &o) {
    const int W = ClassicSim::W, H = ClassicSim::H, CW = W / CELL, CH = H / CELL;
    auto setup = [&] {
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
        box(40, 60, 52, 61, WL_ALLOWGAS);
        box(60, 70, 66, 73, WL_FAN);
        box(80, 20, 80, 20, WL_STREAM);
        box(120, 70, 122, 72, WL_GRAV);
        classic.setWalls(walls);
        auto part = [&](int x0, int y0, int x1, int y1, int t) {
            for (int y = y0; y <= y1; y++)
                for (int x = x0; x <= x1; x++)
                    classic.createPart(x, y, t);
        };
        part(100, 200, 250, 203, PT_METL);
        part(260, 200, 330, 201, PT_INST);
        part(140, 220, 170, 240, PT_CLNE);
        part(420, 220, 520, 260, PT_WATR);
        part(450, 240, 480, 250, PT_OIL); // (onto the water: TPT only puts it where there's room)
        part(100, 120, 160, 130, PT_GLAS);
        part(110, 122, 150, 128, PT_PHOT); // photons over the glass
        // a glass ring with nothing inside, to fill
        part(300, 50, 400, 50, PT_GLAS);
        part(300, 120, 400, 120, PT_GLAS);
        part(300, 51, 300, 119, PT_GLAS);
        part(400, 51, 400, 119, PT_GLAS);
        part(160, 260, 200, 262, PT_DUST); // under the gas-only wall
        part(560, 340, 580, 350, PT_PUMP); // (HEAT and COOL change PUMP and GPMP by less)
        part(581, 340, 590, 350, PT_GPMP);
        std::vector<Sign> signs(2);
        signs[0].x = 205, signs[0].y = 245, signs[0].text = U"sign";
        signs[1].x = 500, signs[1].y = 330, signs[1].text = U"{t}";
        classic.setSigns(signs);
    };
    using A = ToolUse::Action;
    using K = ToolUse::Kind;
    struct Case {
        std::string what;
        std::vector<ToolUse> uses;
        std::string randomFields; // fields TPT sets at random for this case
        int options = 0;          // OPT_AMBIENT: ambient heat on; OPT_NEWTONIAN: Newtonian gravity on
    };
    enum { OPT_AMBIENT = 1, OPT_NEWTONIAN = 2, OPT_RENDER = 4 }; // OPT_RENDER: draw the picture first (deco fills)
    auto U = [](A a, K k, int tool, int x1, int y1, int x2 = -1, int y2 = -1, int brushIndex = 0, int rx = 4,
                int ry = 4, int flags = 0, int selected = 0, bool dragging = true) {
        ToolUse u;
        u.action = a, u.kind = k, u.tool = tool;
        u.x1 = x1, u.y1 = y1, u.x2 = x2 < 0 ? x1 : x2, u.y2 = y2 < 0 ? y1 : y2;
        u.brush = brushIndex, u.rx = rx, u.ry = ry;
        u.replaceFlags = flags, u.replaceSelected = selected, u.dragging = dragging;
        return u;
    };
    const K E = K::Element, WL = K::Wall, SIM = K::Sim;
    auto strong = [](ToolUse u, float strength) {
        u.strength = strength;
        return u;
    };
    // The property tool's uses, with the value as its window reads it
    auto P = [&](A a, const char *field, const char32_t *value, int x1, int y1, int x2 = -1, int y2 = -1, int rx = 4,
                 int ry = 4, bool dragging = true) {
        ToolUse u = U(a, K::Property, 0, x1, y1, x2, y2, 0, rx, ry, 0, 0, dragging);
        const auto &names = tptPropertyNames();
        int f = int(std::find(names.begin(), names.end(), field) - names.begin());
        if (!tptParseProperty(f, value, TEMPSCALE_CELSIUS, u.property))
            std::printf("  (can't read %s for %s)\n", utf8(value).c_str(), field);
        return u;
    };
    // A custom Game of Life type (B36/S23 with two colours of its own), known to both
    const int customRule = tptParseGolRule(U"B36/S23");
    const std::vector<CustomGol> customTypes = {CustomGol{U"HIGHL", customRule, 0x40C0F0, 0x102030}};
    classic.setCustomGol(customTypes);
    // The decoration tools' uses, with their colour (0xAARRGGBB) and colour space
    auto D = [&](A a, int mode, uint32_t colour, int x1, int y1, int x2 = -1, int y2 = -1, int rx = 4, int ry = 4,
                 int space = DECOSPACE_SRGB) {
        ToolUse u = U(a, K::Deco, mode, x1, y1, x2, y2, 0, rx, ry);
        u.decoColour = colour;
        u.decoSpace = space;
        return u;
    };
    // two colours side by side on the water, to smudge
    const std::vector<ToolUse> stripes = {D(A::DrawRect, DECO_DRAW, 0xFFFF2000, 425, 222, 465, 258),
                                          D(A::DrawRect, DECO_DRAW, 0xFF00A0FF, 466, 222, 515, 258)};
    auto with = [](std::vector<ToolUse> a, std::initializer_list<ToolUse> b) {
        a.insert(a.end(), b.begin(), b.end());
        return a;
    };
    std::vector<Case> cases = {
        {"point DUST circle r4", {U(A::Draw, E, PT_DUST, 150, 205)}, ""},
        {"point DUST circle r10 on CLNE", {U(A::Draw, E, PT_DUST, 145, 222, -1, -1, 0, 10, 10)}, ""},
        {"point WATR square r6", {U(A::Draw, E, PT_WATR, 60, 300, -1, -1, 1, 6, 6)}, ""},
        {"point SAND triangle r8", {U(A::Draw, E, PT_SAND, 220, 320, -1, -1, 2, 8, 8)}, ""},
        {"point OIL ellipse 12x3", {U(A::Draw, E, PT_OIL, 260, 300, -1, -1, 0, 12, 3)}, ""},
        {"point DUST at the edge", {U(A::Draw, E, PT_DUST, 2, 3, -1, -1, 0, 6, 6)}, ""},
        {"drag DUST r3", {U(A::DrawLine, E, PT_DUST, 60, 60, 200, 140, 0, 3, 3)}, ""},
        {"drag DUST r0", {U(A::DrawLine, E, PT_DUST, 60, 330, 220, 290, 0, 0, 0)}, ""},
        {"line STNE r2", {U(A::DrawLine, E, PT_STNE, 30, 150, 90, 340, 0, 2, 2, 0, 0, false)}, ""},
        {"stroke DUST", {U(A::Draw, E, PT_DUST, 500, 100), U(A::DrawLine, E, PT_DUST, 500, 100, 540, 130),
                         U(A::DrawLine, E, PT_DUST, 540, 130, 540, 130), U(A::Click, E, PT_DUST, 540, 130)}, ""},
        {"box WATR", {U(A::DrawRect, E, PT_WATR, 500, 150, 420, 180)}, ""},
        {"fill WATR in the ring", {U(A::DrawFill, E, PT_WATR, 350, 80)}, ""},
        {"fill WATR outside", {U(A::DrawFill, E, PT_WATR, 50, 350)}, ""},
        {"fill GAS by the gas wall", {U(A::DrawFill, E, PT_GAS, 170, 250)}, ""},
        {"erase point on photons", {U(A::Draw, E, PT_NONE, 130, 125, -1, -1, 0, 3, 3)}, ""},
        {"erase drag on photons", {U(A::DrawLine, E, PT_NONE, 112, 124, 148, 126, 0, 2, 2)}, ""},
        {"erase fill METL", {U(A::DrawFill, E, PT_NONE, 150, 201)}, ""},
        {"erase fill PHOT", {U(A::DrawFill, E, PT_NONE, 130, 125)}, ""},
        {"erase fill on a wall", {U(A::DrawFill, E, PT_NONE, 90, 85)}, ""},
        {"replace anything with SAND", {U(A::Draw, E, PT_SAND, 440, 245, -1, -1, 0, 8, 8, REPLACE_MODE, 0)}, ""},
        {"replace WATR with SAND", {U(A::Draw, E, PT_SAND, 465, 245, -1, -1, 0, 20, 6, REPLACE_MODE, PT_WATR)}, ""},
        {"replace PHOT with DUST", {U(A::Draw, E, PT_DUST, 130, 125, -1, -1, 0, 6, 6, REPLACE_MODE, PT_PHOT)}, ""},
        {"replace GLAS under PHOT", {U(A::DrawLine, E, PT_NONE, 112, 124, 148, 126, 0, 2, 2, REPLACE_MODE, PT_GLAS)},
         ""},
        {"replace GLAS with nothing", {U(A::DrawLine, E, PT_NONE, 100, 120, 160, 120, 0, 1, 1, REPLACE_MODE, PT_GLAS)},
         ""},
        {"specific delete METL", {U(A::Draw, E, PT_DUST, 150, 202, -1, -1, 0, 10, 10, SPECIFIC_DELETE, PT_METL)}, ""},
        {"specific delete anything", {U(A::DrawLine, E, PT_DUST, 450, 230, 500, 250, 0, 3, 3, SPECIFIC_DELETE, 0)},
         ""},
        {"spark METL", {U(A::Draw, E, PT_SPRK, 180, 201, -1, -1, 0, 2, 2)}, ""},
        {"spark INST", {U(A::Draw, E, PT_SPRK, 300, 200, -1, -1, 0, 1, 1)}, ""},
        {"spark CLNE (its ctype)", {U(A::Draw, E, PT_SPRK, 150, 230, -1, -1, 0, 2, 2)}, ""},
        {"LIGH", {U(A::Draw, E, PT_LIGH, 300, 300, -1, -1, 0, 10, 10), U(A::DrawLine, E, PT_LIGH, 300, 300, 310, 310)},
         "tmp"},
        {"TESC point", {U(A::Draw, E, PT_TESC, 80, 250, -1, -1, 0, 5, 5)}, ""},
        {"TESC box", {U(A::DrawRect, E, PT_TESC, 520, 300, 560, 320, 0, 50, 50)}, ""},
        {"TESC fill", {U(A::DrawFill, E, PT_TESC, 350, 80, -1, -1, 0, 3, 3)}, ""},
        {"LIFE rule 3", {U(A::Draw, E, (3 << 9) | PT_LIFE, 400, 330, -1, -1, 0, 4, 4)}, ""},
        {"LIFE on CLNE", {U(A::Draw, E, (5 << 9) | PT_LIFE, 160, 230, -1, -1, 0, 3, 3)}, ""},
        {"STKM", {U(A::Draw, E, PT_STKM, 100, 300), U(A::Click, E, PT_STKM, 100, 300)}, ""},
        {"FIGH", {U(A::Click, E, PT_FIGH, 120, 300), U(A::Click, E, PT_FIGH, 130, 300)}, ""},
        {"WALL point r8", {U(A::Draw, WL, WL_WALL, 100, 100, -1, -1, 0, 8, 8)}, ""},
        {"WALL drag r4", {U(A::DrawLine, WL, WL_WALL, 200, 60, 260, 100, 0, 4, 4)}, ""},
        {"WALL box", {U(A::DrawRect, WL, WL_WALL, 430, 300, 470, 330)}, ""},
        {"WALL fill", {U(A::DrawFill, WL, WL_WALL, 50, 350)}, ""},
        {"ERASE fill a wall", {U(A::DrawFill, WL, WL_ERASE, 85, 85)}, ""},
        {"ERASEALL point", {U(A::Draw, WL, WL_ERASEALL, 205, 245, -1, -1, 0, 8, 8)}, ""},
        {"ERASEALL box", {U(A::DrawRect, WL, WL_ERASEALL, 430, 230, 470, 250)}, ""},
        {"ERASEALL fill", {U(A::DrawFill, WL, WL_ERASEALL, 170, 245)}, ""},
        {"STREAM by a stream", {U(A::Draw, WL, WL_STREAM, 324, 84, -1, -1, 0, 0, 0)}, ""},
        {"STREAM drag", {U(A::DrawLine, WL, WL_STREAM, 340, 150, 400, 180, 0, 0, 0)}, ""},
        {"FAN velocity, then a point", {U(A::DrawLine, WL, WL_FAN, 250, 285, 290, 265, 0, 4, 4, 0, 0, false),
                                        U(A::Draw, WL, WL_FAN, 255, 290, -1, -1, 0, 4, 4)}, ""},
        {"GRAV", {U(A::Draw, WL, WL_GRAV, 470, 70, -1, -1, 0, 8, 8)}, ""},
        // the simulation tools
        {"HEAT point r6 on WATR", {U(A::Draw, SIM, TOOL_HEAT, 440, 230, -1, -1, 0, 6, 6)}, ""},
        {"COOL drag x10 on WATR", {strong(U(A::DrawLine, SIM, TOOL_COOL, 460, 225, 500, 250, 0, 4, 4), 10.0f)}, ""},
        {"HEAT box on PUMP, GPMP", {U(A::DrawRect, SIM, TOOL_HEAT, 555, 335, 595, 355)}, ""},
        {"HEAT drag x0.1 on photons", {strong(U(A::DrawLine, SIM, TOOL_HEAT, 112, 124, 148, 126, 0, 3, 3), 0.1f)}, ""},
        {"COOL to zero", {strong(U(A::Draw, SIM, TOOL_COOL, 150, 201, -1, -1, 0, 5, 5), 10.0f),
                          strong(U(A::DrawLine, SIM, TOOL_COOL, 150, 201, 150, 201, 0, 5, 5), 10.0f),
                          strong(U(A::DrawLine, SIM, TOOL_COOL, 150, 201, 160, 202, 0, 5, 5), 10.0f)}, ""},
        {"AIR point r8", {U(A::Draw, SIM, TOOL_AIR, 200, 150, -1, -1, 0, 8, 8)}, ""},
        {"VAC drag r3", {U(A::DrawLine, SIM, TOOL_VAC, 50, 50, 300, 200, 0, 3, 3)}, ""},
        {"AIR box", {U(A::DrawRect, SIM, TOOL_AIR, 10, 10, 100, 60)}, ""},
        {"AIR x10 to the limit", {strong(U(A::DrawLine, SIM, TOOL_AIR, 300, 300, 420, 300, 0, 40, 40), 10.0f)}, ""},
        {"AMBP point", {U(A::Draw, SIM, TOOL_AMBP, 250, 100, -1, -1, 0, 10, 10)}, "", OPT_AMBIENT},
        {"AMBM drag x10", {strong(U(A::DrawLine, SIM, TOOL_AMBM, 100, 300, 300, 320, 0, 6, 6), 10.0f)}, "", OPT_AMBIENT},
        {"AMBP with ambient heat off", {U(A::Draw, SIM, TOOL_AMBP, 250, 100, -1, -1, 0, 10, 10)}, ""},
        {"PGRV point", {U(A::Draw, SIM, TOOL_PGRV, 300, 200, -1, -1, 0, 8, 8)}, "", OPT_NEWTONIAN},
        {"NGRV drag", {U(A::DrawLine, SIM, TOOL_NGRV, 100, 100, 200, 180, 0, 3, 3)}, "", OPT_NEWTONIAN},
        {"CYCL point r10", {U(A::Draw, SIM, TOOL_CYCL, 300, 250, -1, -1, 0, 10, 10)}, ""},
        {"CYCL drag r6", {U(A::DrawLine, SIM, TOOL_CYCL, 100, 100, 180, 160, 0, 6, 6)}, ""},
        {"CYCL square drag x10", {strong(U(A::DrawLine, SIM, TOOL_CYCL, 400, 300, 420, 340, 1, 9, 5), 10.0f)}, ""},
        {"CYCL box", {U(A::DrawRect, SIM, TOOL_CYCL, 200, 300, 261, 342)}, ""},
        {"WIND drag r6", {U(A::DrawLine, SIM, TOOL_WIND, 300, 300, 340, 320, 0, 6, 6)}, ""},
        {"WIND line", {U(A::DrawLine, SIM, TOOL_WIND, 100, 300, 50, 250, 0, 6, 6, 0, 0, false)}, ""},
        {"WIND Drag x10", {strong(U(A::Drag, SIM, TOOL_WIND, 400, 100, 430, 60, 0, 12, 4), 10.0f)}, ""},
        {"WIND point (nothing)", {U(A::Draw, SIM, TOOL_WIND, 400, 100, -1, -1, 0, 12, 4)}, ""},
        // the property tool
        {"PROP temp point", {P(A::Draw, "temp", U"1000", 440, 230, -1, -1, 6, 6)}, ""},
        {"PROP temp in K drag", {P(A::DrawLine, "temp", U"500K", 460, 225, 500, 250, 4, 4)}, ""},
        {"PROP ctype WATR on CLNE", {P(A::Draw, "ctype", U"watr", 150, 230, -1, -1, 3, 3)}, ""},
        {"PROP type GLAS box", {P(A::DrawRect, "type", U"GLAS", 430, 225, 470, 245)}, ""},
        {"PROP life fill METL", {P(A::DrawFill, "life", U"50", 150, 201)}, ""},
        {"PROP type PHOT fill GLAS", {P(A::DrawFill, "type", U"PHOT", 300, 80)}, ""},
        {"PROP dcolour on photons", {P(A::Draw, "dcolour", U"#FF00FF00", 130, 125, -1, -1, 3, 3)}, ""},
        {"PROP fill on photons", {P(A::DrawFill, "tmp", U"0x7", 130, 125)}, ""},
        {"PROP type NONE drag", {P(A::DrawLine, "type", U"NONE", 420, 230, 520, 240, 2, 2)}, ""},
        {"PROP vx line", {P(A::DrawLine, "vx", U"-2.5", 100, 200, 250, 203, 1, 1, false)}, ""},
        {"PROP flags 0b101", {P(A::Draw, "flags", U"0b101", 300, 201, -1, -1, 4, 4)}, ""},
        {"PROP ctype LIFE rule", {P(A::Draw, "ctype", U"B36/S23", 160, 225, -1, -1, 4, 4)}, ""},
        // the decoration tools
        {"SET point", {D(A::Draw, DECO_DRAW, 0xFF8040C0, 440, 230, -1, -1, 6, 6)}, ""},
        {"SET drag at half alpha", {D(A::DrawLine, DECO_DRAW, 0x80FF0000, 430, 225, 510, 255, 3, 3)}, ""},
        {"SET drag r0", {D(A::DrawLine, DECO_DRAW, 0xFF00FF00, 430, 225, 470, 255, 0, 0)}, ""},
        {"SET box", {D(A::DrawRect, DECO_DRAW, 0xFF123456, 100, 199, 260, 204)}, ""},
        {"SET on photons", {D(A::Draw, DECO_DRAW, 0xFFFFFF00, 130, 125, -1, -1, 3, 3)}, ""},
        {"CLR after SET", with(stripes, {D(A::Draw, DECO_CLEAR, 0xFF000000, 465, 240, -1, -1, 8, 8)}), ""},
        {"ADD drag over SET", with(stripes, {D(A::DrawLine, DECO_ADD, 0xFF40FF20, 430, 230, 500, 250, 5, 5)}), ""},
        {"ADD at half alpha onto nothing", {D(A::DrawLine, DECO_ADD, 0x80FFFFFF, 430, 230, 500, 250, 5, 5)}, ""},
        {"SUB drag over SET", with(stripes, {D(A::DrawLine, DECO_SUBTRACT, 0xFFFFFFFF, 430, 240, 510, 240, 6, 6)}),
         ""},
        {"MUL drag over SET", with(stripes, {D(A::DrawLine, DECO_MULTIPLY, 0xFF80FF80, 430, 250, 510, 230, 6, 6)}),
         ""},
        {"DIV box over SET", with(stripes, {D(A::DrawRect, DECO_DIVIDE, 0xFFFF00FF, 440, 225, 490, 255)}), ""},
        {"SMDG drag across two colours", with(stripes, {D(A::DrawLine, DECO_SMUDGE, 0, 455, 230, 480, 250, 4, 4)}),
         ""},
        {"SMDG linear", with(stripes, {D(A::DrawLine, DECO_SMUDGE, 0, 455, 240, 480, 240, 4, 4, DECOSPACE_LINEAR)}),
         ""},
        {"SMDG gamma 2.2", with(stripes, {D(A::Draw, DECO_SMUDGE, 0, 466, 240, -1, -1, 6, 6, DECOSPACE_GAMMA22)}), ""},
        {"SMDG gamma 1.8 box", with(stripes, {D(A::DrawRect, DECO_SMUDGE, 0, 460, 230, 472, 250, 0, 0,
                                                DECOSPACE_GAMMA18)}), ""},
        {"SMDG by the edge", {U(A::Draw, E, PT_DUST, 4, 4, -1, -1, 0, 6, 6), D(A::DrawRect, DECO_DRAW, 0xFFFF0000, 0, 0, 10, 10),
                              D(A::Draw, DECO_DRAW, 0xFF0000FF, 3, 3), D(A::Draw, DECO_SMUDGE, 0, 3, 3, -1, -1, 4, 4)}, ""},
        {"SET fill on WATR", {D(A::DrawFill, DECO_DRAW, 0xFF30C030, 450, 240)}, "", OPT_RENDER},
        {"CLR fill on METL", {D(A::DrawRect, DECO_DRAW, 0xFF30C030, 100, 200, 250, 203),
                              D(A::DrawFill, DECO_CLEAR, 0xFF000000, 150, 201)}, "", OPT_RENDER},
        // create_part(-1, x, y, SPRK), as a button sign makes it: METL sparks, INST only where it is (no flood)
        {"a button sign's spark on METL", {U(A::Create, E, PT_SPRK, 150, 201)}, ""},
        {"a button sign's spark on INST", {U(A::Create, E, PT_SPRK, 280, 200)}, ""},
        // custom Game of Life types: LIFE made with their rules takes their colours (and a rule that isn't one, none)
        {"LIFE of a custom type", {U(A::Draw, E, (customRule << 9) | PT_LIFE, 400, 330, -1, -1, 0, 4, 4)}, ""},
        {"LIFE of an unknown rule", {U(A::Draw, E, ((customRule + 2) << 9) | PT_LIFE, 400, 330, -1, -1, 0, 4, 4)}, ""},
    };

    int failures = 0;
    for (const Case &cs : cases) {
        {
            ClassicOptions co;
            co.ambientHeat = (cs.options & OPT_AMBIENT) != 0;
            co.newtonianGravity = (cs.options & OPT_NEWTONIAN) != 0;
            classic.setOptions(co);
        }
        setup();
        SimConfig cfg;
        cfg.W = W;
        cfg.H = H;
        cfg.seed = o.seed;
        cfg.shaderDir = o.shaderDir;
        cfg.settings.ambientHeat = (cs.options & OPT_AMBIENT) != 0;
        cfg.settings.newtonianGravity = (cs.options & OPT_NEWTONIAN) != 0;
        GpuSim gpu(ctx, cfg, elements);
        SceneData state = sceneFromParticles(elements, classic.particles(), W, H, 0, 0);
        state.walls = classic.walls();
        state.signs = classic.signs();
        gpu.load(state);
        gpu.setCustomGol(customTypes);
        {
            std::vector<float> fvx = classic.fanVX(), fvy = classic.fanVY();
            gpu.setWalls(classic.walls(), &fvx, &fvy);
        }
        {
            std::vector<float> pv, vx, vy, hv;
            classic.airMaps(pv, vx, vy, hv);
            gpu.writeAir(pv, vx, vy, hv);
        }
        if (cs.options & OPT_RENDER) {
            // the picture a decoration fill looks at: TPT's renderer and the GPU's (Fire Display, decorations on)
            std::vector<uint32_t> picture;
            classic.render(4, picture);
            RenderParams rp;
            ctx.immediate([&](VkCommandBuffer cmd) { gpu.recordWorldImage(cmd, rp); });
            std::vector<uint32_t> gpuPicture = gpu.readWorldImage();
            int differ = 0;
            for (size_t i = 0; i < picture.size() && i < gpuPicture.size(); i++)
                differ += picture[i] != gpuPicture[i];
            if (differ) {
                int x0 = W, y0 = H, x1 = -1, y1 = -1;
                for (size_t i = 0; i < picture.size() && i < gpuPicture.size(); i++)
                    if (picture[i] != gpuPicture[i]) {
                        int px = int(i % size_t(W)), py = int(i / size_t(W));
                        x0 = std::min(x0, px), y0 = std::min(y0, py), x1 = std::max(x1, px), y1 = std::max(y1, py);
                    }
                std::printf("  (the pictures differ at %d pixels, in (%d,%d)-(%d,%d))\n", differ, x0, y0, x1, y1);
            }
        }
        std::vector<ClassicParticle> before = classic.particles();
        std::vector<float> airBefore[4];
        classic.airMaps(airBefore[0], airBefore[1], airBefore[2], airBefore[3]);
        std::vector<float> massBefore = classic.gravityMass();
        std::vector<uint8_t> wallsBefore = classic.walls();
        std::vector<float> fansBefore = classic.fanVX();
        size_t signsBefore = classic.signs().size();
        TptBrush brush;
        for (const ToolUse &u : cs.uses)
            classic.useTool(u, brush);
        GpuEditor editor(ctx, gpu);
        if (GpuEditor::needsWorld(cs.uses))
            editor.apply(VK_NULL_HANDLE, cs.uses);
        else
            ctx.immediate([&](VkCommandBuffer cmd) { editor.apply(cmd, cs.uses); });

        // each cell's particles, energy particles apart
        auto byCell = [&](const std::vector<ClassicParticle> &ps) {
            std::multimap<std::tuple<int, int, bool>, ClassicParticle> m;
            for (const auto &p : ps) {
                bool energy = (elements.elements[p.type].props.properties & 0x10) != 0;
                m.emplace(std::make_tuple(int(p.x + 0.5f), int(p.y + 0.5f), energy), p);
            }
            return m;
        };
        auto t = byCell(classic.particles()), gp = byCell(gpu.readParticles());
        // what the uses changed in TPT (a case that changes nothing tests little)
        int changed = 0;
        {
            auto b = byCell(before);
            for (auto &[cell, p] : t) {
                auto [ba, bb] = b.equal_range(cell);
                bool found = false;
                for (auto it = ba; it != bb && !found; ++it)
                    found = std::memcmp(&it->second, &p, sizeof p) == 0;
                changed += !found;
            }
            changed += int(b.size() > t.size() ? b.size() - t.size() : 0);
            std::vector<uint8_t> wa = classic.walls();
            std::vector<float> fa = classic.fanVX();
            for (size_t c = 0; c < wa.size(); c++)
                changed += wa[c] != wallsBefore[c] || fa[c] != fansBefore[c];
            changed += int(signsBefore - classic.signs().size());
        }
        std::string fields, example;
        // the air (pressure, velocity, ambient heat) and Newtonian gravity's mass, cell by cell
        int airDiff = 0;
        {
            std::vector<float> ta[4], ga[4];
            classic.airMaps(ta[0], ta[1], ta[2], ta[3]);
            gpu.readAir(ga[0], ga[1], ga[2], ga[3]);
            for (int m = 0; m < 4; m++)
                for (size_t c = 0; c < ta[m].size(); c++) {
                    changed += ta[m][c] != airBefore[m][c];
                    if (ta[m][c] != ga[m][c]) {
                        if (!airDiff && example.empty()) {
                            char b[160];
                            std::snprintf(b, sizeof b, " e.g. %s at cell (%zu,%zu) TPT %.9g GPU %.9g",
                                          m == 0 ? "pv" : m == 1 ? "vx" : m == 2 ? "vy" : "hv", c % size_t(CW),
                                          c / size_t(CW), double(ta[m][c]), double(ga[m][c]));
                            example = b;
                        }
                        airDiff++;
                    }
                }
            std::vector<float> tm = classic.gravityMass();
            std::vector<uint32_t> gc = gpu.readGravityCells();
            for (size_t c = 0; c < tm.size(); c++) {
                float g;
                std::memcpy(&g, &gc[c], 4); // (the mass is the first plane)
                changed += tm[c] != massBefore[c];
                if (tm[c] != g && (cs.options & OPT_NEWTONIAN))
                    airDiff++;
            }
        }
        int onlyT = 0, onlyG = 0, differ = 0, random = 0;
        for (auto it = t.begin(); it != t.end(); it = t.upper_bound(it->first)) {
            auto [ta, tb] = t.equal_range(it->first);
            auto [ga, gb] = gp.equal_range(it->first);
            int nt = int(std::distance(ta, tb)), ng = int(std::distance(ga, gb));
            if (nt != ng) {
                onlyT += std::max(0, nt - ng);
                onlyG += std::max(0, ng - nt);
                if (example.empty())
                    example = " e.g. (" + std::to_string(std::get<0>(it->first)) + "," +
                              std::to_string(std::get<1>(it->first)) + ") TPT " + std::to_string(nt) + " GPU " +
                              std::to_string(ng);
                continue;
            }
            for (auto a = ta, b = ga; a != tb; ++a, ++b) {
                const ClassicParticle &p = a->second, &q = b->second;
                bool same = true, rnd = false;
                auto check = [&](const char *name, bool eq) {
                    if (eq)
                        return;
                    if (cs.randomFields.find(name) != std::string::npos || !std::strcmp(name, "life")) {
                        rnd = true;
                        return;
                    }
                    same = false;
                    if (fields.find(name) == std::string::npos)
                        fields += std::string(" ") + name;
                };
                check("type", p.type == q.type);
                check("ctype", p.ctype == q.ctype);
                check("tmp", p.tmp == q.tmp);
                check("tmp2", p.tmp2 == q.tmp2);
                check("tmp3", p.tmp3 == q.tmp3);
                check("tmp4", p.tmp4 == q.tmp4);
                check("dcolour", p.dcolour == q.dcolour);
                check("temp", p.temp == q.temp);
                check("life", p.life == q.life);
                check("vx", p.vx == q.vx);
                check("vy", p.vy == q.vy);
                check("flags", p.flags == q.flags);
                if (!same) {
                    differ++;
                    if (example.empty())
                        example = " e.g. (" + std::to_string(std::get<0>(it->first)) + "," +
                                  std::to_string(std::get<1>(it->first)) + ") TPT " + std::to_string(p.type) + "/" +
                                  std::to_string(p.ctype) + "/" + std::to_string(p.tmp) + " GPU " +
                                  std::to_string(q.type) + "/" + std::to_string(q.ctype) + "/" + std::to_string(q.tmp);
                } else if (rnd)
                    random++;
            }
        }
        for (auto it = gp.begin(); it != gp.end(); it = gp.upper_bound(it->first))
            if (!t.count(it->first)) {
                onlyG += int(gp.count(it->first));
                if (example.empty())
                    example = " e.g. (" + std::to_string(std::get<0>(it->first)) + "," +
                              std::to_string(std::get<1>(it->first)) + ") only on the GPU";
            }
        // walls (the CPU's copy and the GPU's), fans, signs
        std::vector<uint8_t> tw = classic.walls();
        GpuSim::EngineState es = gpu.readEngineState();
        int wallDiff = 0;
        for (size_t c = 0; c < tw.size(); c++)
            wallDiff += tw[c] != gpu.walls()[c] || tw[c] != (es.walls[c] & 0xFF);
        std::vector<float> tfx = classic.fanVX(), tfy = classic.fanVY(), gfx, gfy;
        gpu.readFans(gfx, gfy);
        int fanDiff = 0;
        for (size_t c = 0; c < tfx.size(); c++)
            fanDiff += tfx[c] != gfx[c] || tfy[c] != gfy[c];
        std::vector<Sign> ts = classic.signs();
        const std::vector<Sign> &gs = gpu.signs();
        bool signsSame = ts.size() == gs.size();
        for (size_t k = 0; signsSame && k < ts.size(); k++)
            signsSame = ts[k].x == gs[k].x && ts[k].y == gs[k].y && ts[k].text == gs[k].text;

        bool ok = !onlyT && !onlyG && !differ && !wallDiff && !fanDiff && signsSame && !airDiff;
        failures += !ok;
        std::printf("  %s %-30s %6d changed; TPT %6zu GPU %6zu: %d only TPT, %d only GPU, %d differ%s%s",
                    ok ? "ok  " : "FAIL", cs.what.c_str(), changed, t.size(), gp.size(), onlyT, onlyG, differ,
                    fields.empty() ? "" : " in", fields.c_str());
        if (wallDiff || fanDiff || !signsSame)
            std::printf("; walls %d, fans %d, signs %s", wallDiff, fanDiff, signsSame ? "same" : "differ");
        if (airDiff)
            std::printf("; %d air cells differ", airDiff);
        if (random)
            std::printf(" (%d differ only where TPT picks at random)", random);
        std::printf("%s\n", ok ? "" : example.c_str());
    }
    std::printf("%zu cases, %d failed\n", cases.size(), failures);

    // MIX: where it swaps particles is random in TPT too, so it is checked by what it does: on stripes of SAND and DUST
    // (and WATR beside them), no particle is made or lost, each swap is of two of the same state, and about as many
    // happen as in TPT
    {
        classic.setOptions(ClassicOptions{});
        setup();
        for (int y = 290; y < 340; y++)
            for (int x = 20; x < 90; x++)
                classic.createPart(x, y, x >= 80 ? PT_WATR : (x / 3) % 2 ? PT_SAND : PT_DUST);
        SimConfig cfg;
        cfg.W = W;
        cfg.H = H;
        cfg.seed = o.seed;
        cfg.shaderDir = o.shaderDir;
        GpuSim gpu(ctx, cfg, elements);
        SceneData state = sceneFromParticles(elements, classic.particles(), W, H, 0, 0);
        state.walls = classic.walls();
        gpu.load(state);
        std::vector<ToolUse> uses;
        for (int k = 0; k < 30; k++) {
            uses.push_back(U(A::Draw, SIM, TOOL_MIX, 55, 315, -1, -1, 0, 20, 20));
            uses.push_back(U(A::DrawLine, SIM, TOOL_MIX, 55, 315, 75, 320, 0, 20, 20));
        }
        auto grid = [&](const std::vector<ClassicParticle> &ps) {
            std::vector<int> g(size_t(W) * H, 0);
            for (const auto &p : ps)
                if (!(elements.elements[p.type].props.properties & 0x10))
                    g[size_t(int(p.y + 0.5f)) * W + size_t(int(p.x + 0.5f))] = p.type;
            return g;
        };
        std::vector<int> start = grid(classic.particles());
        TptBrush brush;
        for (const ToolUse &u : uses)
            classic.useTool(u, brush);
        GpuEditor editor(ctx, gpu);
        ctx.immediate([&](VkCommandBuffer cmd) { editor.apply(cmd, uses); });
        std::vector<int> tg = grid(classic.particles()), gg = grid(gpu.readParticles());
        auto summary = [&](const std::vector<int> &g, int &moved, bool &statesKept) {
            std::map<int, int> before, after;
            moved = 0;
            statesKept = true;
            for (size_t i = 0; i < g.size(); i++) {
                before[start[i]]++;
                after[g[i]]++;
                if (g[i] != start[i]) {
                    moved++;
                    statesKept &= (elements.elements[g[i]].props.properties & 0x1F) ==
                                  (elements.elements[start[i]].props.properties & 0x1F);
                }
            }
            return before == after;
        };
        int tm, gm;
        bool tk, gk;
        bool tKept = summary(tg, tm, tk), gKept = summary(gg, gm, gk);
        bool ok = tKept && gKept && tk && gk && tm > 0 && gm > 0 && gm * 3 > tm * 2 && gm * 2 < tm * 3;
        failures += !ok;
        std::printf("  %s MIX on SAND, DUST and WATR: cells changed TPT %d GPU %d; particles kept: TPT %s GPU %s; "
                    "states kept: TPT %s GPU %s\n",
                    ok ? "ok  " : "FAIL", tm, gm, tKept ? "yes" : "NO", gKept ? "yes" : "NO", tk ? "yes" : "NO",
                    gk ? "yes" : "NO");
    }

    // GameView's mouse and keys (ui/GameInput): what scripted presses, drags and frames make, against what TPT's
    // GameView.cpp makes of them (worked out from its code)
    {
        GameUi ui(elements);
        auto describe = [](const std::vector<ToolUse> &uses) {
            std::string out;
            for (const ToolUse &u : uses) {
                char b[96];
                const char *a[] = {"Draw", "Line", "Rect", "Fill", "Click"};
                if (u.action == A::DrawLine || u.action == A::DrawRect)
                    std::snprintf(b, sizeof b, "%s(%d,%d>%d,%d%s)", a[int(u.action)], u.x1, u.y1, u.x2, u.y2,
                                  u.action == A::DrawLine && !u.dragging ? " shift" : "");
                else
                    std::snprintf(b, sizeof b, "%s(%d,%d)", a[int(u.action)], u.x1, u.y1);
                out += (out.empty() ? "" : " ") + std::string(b);
            }
            return out;
        };
        struct Script {
            const char *what, *expected;
            void (*run)(GameInput &);
        };
        const Script scripts[] = {
            {"drag, hold still, let go",
             "Draw(10,10) Line(10,10>20,15) Line(20,15>30,20) Line(30,20>30,20) Line(30,20>31,21) Click(31,21)",
             [](GameInput &in) {
                 in.mouseDown(10, 10, 0);
                 in.mouseMove(20, 15);
                 in.mouseMove(30, 20);
                 in.tick(30, 20); // (the moves drew this frame)
                 in.tick(30, 20); // held still: draws again
                 in.mouseUp(31, 21);
             }},
            {"shift: a line", "Line(10,10>50,18 shift)",
             [](GameInput &in) {
                 in.setModifiers(true, false, false);
                 in.mouseDown(10, 10, 0);
                 in.mouseMove(40, 15);
                 in.tick(40, 15);
                 in.mouseUp(50, 18);
                 in.setModifiers(false, false, false);
             }},
            {"shift+alt: a snapped line", "Line(10,10>50,10 shift)",
             [](GameInput &in) {
                 in.setModifiers(true, false, true);
                 in.mouseDown(10, 10, 0);
                 in.mouseUp(50, 18);
                 in.setModifiers(false, false, false);
             }},
            {"ctrl: a box", "Rect(10,10>50,30)",
             [](GameInput &in) {
                 in.setModifiers(false, true, false);
                 in.mouseDown(10, 10, 0);
                 in.mouseMove(30, 20);
                 in.tick(30, 20);
                 in.mouseUp(50, 30);
                 in.setModifiers(false, false, false);
             }},
            {"ctrl+alt: a snapped box", "Rect(10,10>40,40)",
             [](GameInput &in) {
                 in.setModifiers(false, true, true);
                 in.mouseDown(10, 10, 0);
                 in.mouseUp(50, 30);
                 in.setModifiers(false, false, false);
             }},
            {"ctrl+shift: fills", "Fill(10,10) Fill(20,20) Fill(25,25) Fill(26,26)",
             [](GameInput &in) {
                 in.setModifiers(true, true, false);
                 in.mouseDown(10, 10, 0);
                 in.mouseMove(20, 20);
                 in.tick(20, 20);
                 in.tick(25, 25);
                 in.mouseUp(26, 26);
                 in.setModifiers(false, false, false);
             }},
            {"shift while drawing changes nothing yet", "Draw(10,10) Line(10,10>20,20) Click(20,20) Line(30,30>40,40 shift)",
             [](GameInput &in) {
                 in.mouseDown(10, 10, 0);
                 in.setModifiers(true, false, false);
                 in.mouseUp(20, 20);
                 in.mouseDown(30, 30, 0);
                 in.mouseUp(40, 40);
                 in.setModifiers(false, false, false);
             }},
        };
        int inputOk = 0;
        for (const Script &sc : scripts) {
            GameInput in(ui);
            sc.run(in);
            std::string got = describe(in.takeUses());
            bool ok = got == sc.expected;
            inputOk += ok;
            failures += !ok;
            std::printf("  %s %-40s %s%s%s\n", ok ? "ok  " : "FAIL", sc.what, got.c_str(), ok ? "" : "  expected ",
                        ok ? "" : sc.expected);
        }
        // the brush and replace mode keys
        GameInput in(ui);
        std::string keys;
        auto note = [&](const char *what) {
            char b[96];
            std::snprintf(b, sizeof b, "%s %d %d,%d flags %d; ", what, in.brush.index(), in.brush.rx(), in.brush.ry(),
                          in.replaceFlags());
            keys += b;
        };
        note("start");
        in.wheel(1);
        note("wheel");
        in.setModifiers(false, true, false);
        in.wheel(3);
        note("ctrl+wheel");
        in.setModifiers(false, false, false);
        for (int k = 0; k < 4; k++)
            in.bracket(1, false);
        note("4x]");
        in.bracket(-1, true);
        note("alt+[");
        in.changeBrush();
        note("tab");
        in.toggleReplace(false);
        note(";");
        in.toggleReplace(true);
        note("ctrl+;");
        in.toggleSpecificDelete();
        note("delete");
        const char *expectedKeys = "start 0 4,4 flags 0; wheel 0 5,5 flags 0; ctrl+wheel 0 5,8 flags 0; "
                                   "4x] 0 9,14 flags 0; alt+[ 0 8,13 flags 0; tab 1 8,13 flags 0; ; 1 8,13 flags 1; "
                                   "ctrl+; 1 8,13 flags 2; delete 1 8,13 flags 0; ";
        bool keysOk = keys == expectedKeys;
        failures += !keysOk;
        std::printf("  %s %-40s %s%s%s\n", keysOk ? "ok  " : "FAIL", "brush and replace keys", keys.c_str(),
                    keysOk ? "" : "\n       expected ", keysOk ? "" : expectedKeys);
        std::printf("%d of %zu input scripts as GameView\n", inputOk + keysOk, std::size(scripts) + 1);
    }

    // How long the GPU's tools take in a big world (--size), the GPU's work included
    {
        SimConfig cfg;
        cfg.W = o.W;
        cfg.H = o.H;
        cfg.seed = o.seed;
        cfg.shaderDir = o.shaderDir;
        GpuSim gpu(ctx, cfg, elements);
        gpu.load(makeScene(elements, "empty", cfg.W, cfg.H, o.seed, o.fill));
        GpuEditor editor(ctx, gpu);
        auto time = [&](const char *what, const std::vector<ToolUse> &uses) {
            auto t0 = std::chrono::steady_clock::now();
            if (GpuEditor::needsWorld(uses))
                editor.apply(VK_NULL_HANDLE, uses);
            else
                ctx.immediate([&](VkCommandBuffer cmd) { editor.apply(cmd, uses); });
            double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
            std::printf("  %ux%u %-40s %8.1f ms (%llu particles)\n", cfg.W, cfg.H, what, ms,
                        (unsigned long long)gpu.readStats().total);
        };
        const int cx = int(cfg.W) / 2, cy = int(cfg.H) / 2;
        time("a press, r 20", {U(A::Draw, E, PT_DUST, cx, cy, -1, -1, 0, 20, 20)});
        time("a drag of 200 px, r 20", {U(A::DrawLine, E, PT_DUST, cx, cy, cx + 200, cy + 50, 0, 20, 20)});
        time("a line across the world, r 200",
             {U(A::DrawLine, E, PT_SAND, 300, 300, int(cfg.W) - 300, int(cfg.H) - 300, 0, 200, 200, 0, 0, false)});
        time("a flood fill of the rest", {U(A::DrawFill, E, PT_WATR, 10, 10)});
        time("a flood fill erasing the water", {U(A::DrawFill, E, PT_NONE, 10, 10)});
        time("a box of walls, 1000 x 1000", {U(A::DrawRect, WL, WL_WALL, 100, 100, 1100, 1100)});
        time("HEAT along a line, r 200",
             {U(A::DrawLine, SIM, TOOL_HEAT, 300, 300, int(cfg.W) - 300, int(cfg.H) - 300, 0, 200, 200, 0, 0, false)});
        time("AIR along a line, r 200",
             {U(A::DrawLine, SIM, TOOL_AIR, 300, 300, int(cfg.W) - 300, int(cfg.H) - 300, 0, 200, 200, 0, 0, false)});
        time("CYCL along a line, r 200",
             {U(A::DrawLine, SIM, TOOL_CYCL, 300, 300, int(cfg.W) - 300, int(cfg.H) - 300, 0, 200, 200, 0, 0, false)});
        time("CYCL drag of 200 px, r 20", {U(A::DrawLine, SIM, TOOL_CYCL, cx, cy, cx + 200, cy + 50, 0, 20, 20)});
        time("MIX drag of 200 px, r 50", {U(A::DrawLine, SIM, TOOL_MIX, cx, cy, cx + 200, cy + 50, 0, 50, 50)});
        time("PROP temp, a flood fill", {P(A::DrawFill, "temp", U"500", 10, 10)});
        time("SET drag of 200 px, r 20", {D(A::DrawLine, DECO_DRAW, 0xFF8040C0, cx, cy, cx + 200, cy + 50, 20, 20)});
        time("ADD along a line, r 200", {D(A::DrawLine, DECO_ADD, 0xFF8040C0, 300, 300, int(cfg.W) - 300,
                                           int(cfg.H) - 300, 200, 200)});
        time("SMDG drag of 200 px, r 20", {D(A::DrawLine, DECO_SMUDGE, 0, cx, cy, cx + 200, cy + 50, 20, 20)});
        time("SET fill of the picture", {D(A::DrawFill, DECO_DRAW, 0xFF8040C0, 10, 10)});
    }
    return failures ? 1 : 0;
}

// --ui-snapshot PATH: the viewer's picture without a window: the first scene at --size, shown in TPT's window
// (629x424) at interface scale 2 with TPT's game screen over it, the mouse resting on a palette button; written as
// PATH-<n>.ppm for a few mouse positions.
int runUiSnapshot(VkContext &ctx, const ElementTable &elements, const AppOptions &o) {
    SimConfig cfg;
    cfg.W = o.W;
    cfg.H = o.H;
    cfg.seed = o.seed;
    cfg.shaderDir = o.shaderDir;
    GpuSim gpu(ctx, cfg, elements);
    gpu.load(makeScene(elements, o.scenes.empty() ? "mixed" : o.scenes[0], cfg.W, cfg.H, o.seed, o.fill));
    for (int t = 0; t < o.ticks; t += 10)
        ctx.immediate([&](VkCommandBuffer cmd) {
            for (int k = 0; k < 10 && t + k < o.ticks; k++)
                gpu.recordTick(cmd);
        });
    const int scale = o.uiScale > 0 ? o.uiScale : 2, uw = 629, uh = 424;
    const uint32_t sw = uint32_t(uw * scale), sh = uint32_t(uh * scale);
    gpu.ensureFrameBuffer(sw, sh);
    Canvas canvas(elements);
    GameUi ui(elements);
    Overlay overlay(ctx, o.shaderDir);
    ui.setSize(uw, uh);
    // the world fitted into the view, as Camera::fit does
    float vw = float((uw - GameUi::BARSIZE) * scale), vh = float((uh - GameUi::MENUSIZE) * scale);
    RenderParams rp;
    rp.zoom = std::min(vw / float(cfg.W), vh / float(cfg.H));
    rp.camX = (float(cfg.W) - vw / rp.zoom) * 0.5f;
    rp.camY = (float(cfg.H) - vh / rp.zoom) * 0.5f;
    rp.screenW = sw;
    rp.screenH = sh;
    struct Shot { int x, y; const char *what; };
    const Shot shots[] = {{300, 200, "over the world"}, {uw - 56 + 10, uh - 40 + 8, "over the first palette button"},
                          {uw - 8, uh - 48 - 16 * 6 + 7, "over a menu section"}, {uw - 8, uh - 8, "over pause"}};
    for (size_t k = 0; k < std::size(shots); k++) {
        const Shot &s = shots[k];
        float wx = rp.camX + float(s.x * scale) / rp.zoom, wy = rp.camY + float(s.y * scale) / rp.zoom;
        rp.brushX = int(wx);
        rp.brushY = int(wy);
        // TPT's brush cursor (its default circle) where the mouse is over the world
        std::vector<int> cursor;
        if (ui.overWorld(s.x, s.y)) {
            CursorState cs;
            cs.x = rp.brushX, cs.y = rp.brushY;
            brushCursor(cs, TptBrush(), cursor);
        }
        rp.cursor = &cursor;
        // two frames, so the cursor's sample has come back
        for (int frame = 0; frame < 2; frame++) {
            ui.mouseMove(s.x, s.y, false);
            ui.tick(100);
            UiState st;
            st.fps = 60.0;
            const SignSample &g = gpu.cursorSample();
            st.sample.inWorld = ui.overWorld(s.x, s.y);
            st.sample.hasPart = g.hasPart && st.sample.inWorld;
            st.sample.type = g.type, st.sample.ctype = g.ctype, st.sample.temp = g.temp, st.sample.pressure = g.pressure;
            ui.draw(canvas, st);
            ctx.immediate([&](VkCommandBuffer cmd) {
                gpu.recordRender(cmd, rp);
                overlay.record(cmd, 0, canvas.pixels(), uint32_t(canvas.width()), uint32_t(canvas.height()),
                               uint32_t(scale), gpu.frameBuffer(), sw, sh);
            });
        }
        std::vector<uint32_t> px(size_t(sw) * sh);
        ctx.download(gpu.frameBuffer(), px.data(), px.size() * 4);
        for (auto &p : px)
            p &= 0xFFFFFF;
        std::string path = o.uiSnapshot + "-" + std::to_string(k) + ".ppm";
        writePpm(path, px, int(sw), int(sh));
        std::printf("  (%s)\n", s.what);
    }
    // Clicks: right on the second palette button, left on a section, left on pause, and a press on one tool
    // released over another (no choice, as in TPT)
    auto click = [&](int x, int y, int button) {
        ui.mouseMove(x, y, false);
        ui.mouseDown(x, y, button);
        ui.mouseUp(x, y, button);
    };
    auto toolName = [&](int slot) {
        const UiTool *t = ui.activeTool(slot);
        std::string n;
        if (t)
            for (char32_t ch : t->name)
                n += char(ch);
        return t ? n : std::string("-");
    };
    int ok = 0;
    click(uw - 56 - 31 + 10, uh - 40 + 8, 1);
    std::printf("  right click on the 2nd palette button: tools %s / %s\n", toolName(0).c_str(), toolName(1).c_str());
    ok += toolName(1) != "NONE";
    click(uw - 8, uh - 48 - 16 * 7 + 7, 0);
    std::printf("  click on a section: menu %d\n", ui.activeSection());
    click(uw - 8, uh - 8, 0);
    UiAction a = ui.takeAction();
    std::printf("  click on pause: action %d (Pause is %d)\n", int(a), int(UiAction::Pause));
    ok += a == UiAction::Pause;
    std::string before = toolName(0);
    ui.mouseMove(uw - 56 + 10, uh - 40 + 8, false);
    ui.mouseDown(uw - 56 + 10, uh - 40 + 8, 0);
    ui.mouseUp(uw - 56 - 31 + 10, uh - 40 + 8, 0);
    std::printf("  press on one tool, release on another: tool %s (was %s)\n", toolName(0).c_str(), before.c_str());
    ok += toolName(0) == before;
    ui.setModifiers(false, true, true);
    click(uw - 56 + 10, uh - 40 + 8, 0);
    ui.setModifiers(false, false, false);
    std::string replaced = toolName(3), left = toolName(0);
    click(uw - 56 + 10, uh - 40 + 8, 0);
    std::printf("  ctrl+alt click on the 1st palette button: replace mode works on %s, the left button kept %s "
                "(a plain click then chose %s)\n", replaced.c_str(), left.c_str(), toolName(0).c_str());
    ok += left == before && replaced != "NONE" && toolName(0) == replaced;
    // The simulation tools' menu (a section of its own, as in TPT): the Tools button, then the first tool in it
    {
        int sx = 0, sy = 0;
        ui.sectionButton(SC_TOOL, sx, sy);
        click(sx, sy, 0);
        click(uw - 56 + 10, uh - 40 + 8, 0);
        std::printf("  the Tools section (menu %d), its first button: %s\n", ui.activeSection(), toolName(0).c_str());
        ok += ui.activeSection() == SC_TOOL && toolName(0) == "HEAT";
    }

    // TPT's windows (ui/Dialogs), driven by keys and drawn over the screen as the viewer draws them
    ui::host().font = &canvas;
    ui::Engine windows;
    auto shot = [&](int k, const char *what) {
        UiState st;
        st.fps = 60.0;
        ui.draw(canvas, st);
        canvas.blendFilledRect(0, 0, uw, uh, rgb(0x000000, 0x54)); // (TPT's fade, 20 frames in)
        windows.draw(canvas);
        ctx.immediate([&](VkCommandBuffer cmd) {
            gpu.recordRender(cmd, rp);
            overlay.record(cmd, 0, canvas.pixels(), uint32_t(canvas.width()), uint32_t(canvas.height()), uint32_t(scale),
                           gpu.frameBuffer(), sw, sh);
        });
        std::vector<uint32_t> px(size_t(sw) * sh);
        ctx.download(gpu.frameBuffer(), px.data(), px.size() * 4);
        for (auto &p : px)
            p &= 0xFFFFFF;
        writePpm(o.uiSnapshot + "-" + std::to_string(k) + ".ppm", px, int(sw), int(sh));
        std::printf("  (%s)\n", what);
    };
    auto key = [&](SDL_Keycode k, SDL_Scancode sc) {
        windows.keyPress(int(k), int(sc), false, false, false, false);
        windows.tick(16);
    };
    {
        int slot = -1, tool = -1;
        windows.show(std::make_unique<ElementSearchWindow>(ui, [&](int sl, int t) { slot = sl, tool = t; }), uw, uh);
        windows.tick(16); // (what comes in the frame a window opens goes nowhere)
        windows.textInput(U"wat");
        windows.tick(16);
        shot(4, "element search for \"wat\"");
        key(SDLK_RETURN, SDL_SCANCODE_RETURN);
        std::string name = tool >= 0 ? utf8(ui.tools()[size_t(tool)].name) : "-";
        std::printf("  element search, \"wat\" and Enter: %s for slot %d, window %s\n", name.c_str(), slot,
                    windows.empty() ? "closed" : "still open");
        ok += name == "WATR" && slot == 0 && windows.empty();
    }
    {
        ParticleProperty set;
        int field = -1;
        windows.show(std::make_unique<PropertyWindow>(
                         ParticleProperty::LIFE, U"", TEMPSCALE_CELSIUS,
                         [&](const ParticleProperty &p, int f, const std::u32string &) { set = p, field = f; }),
                     uw, uh);
        windows.tick(16);
        key(SDLK_DOWN, SDL_SCANCODE_DOWN); // life -> ctype -> x ... temp
        for (int k = 0; k < 5; k++)
            key(SDLK_DOWN, SDL_SCANCODE_DOWN);
        windows.textInput(U"10x");
        windows.tick(16);
        key(SDLK_RETURN, SDL_SCANCODE_RETURN);
        bool heldOpen = !windows.empty() && field == -1; // (OK is off while the value can't be read)
        shot(5, "the property window, a value it can't read");
        key(SDLK_BACKSPACE, SDL_SCANCODE_BACKSPACE);
        windows.textInput(U"00");
        windows.tick(16);
        shot(6, "the property window, temp 1000");
        key(SDLK_RETURN, SDL_SCANCODE_RETURN);
        float v;
        std::memcpy(&v, &set.bits, 4);
        std::printf("  property window: \"10x\" kept it open: %s; then temp \"1000\": field %d, %g K, window %s\n",
                    heldOpen ? "yes" : "no", field, double(v), windows.empty() ? "closed" : "still open");
        ok += heldOpen && field == ParticleProperty::TEMP && v == 1273.15f && windows.empty();
    }
    std::printf("  %d of 7 as expected\n", ok);
    return ok == 7 ? 0 : 1;
}

// --zoom-check: draws each scene's world image, then shows it zoomed out (1/3 to 1/16) and checks every screen pixel
// against the average of the world pixels it covers.
int runZoomCheck(VkContext &ctx, const ElementTable &elements, const AppOptions &o) {
    int failures = 0;
    for (const std::string &scene : o.scenes) {
        SimConfig cfg;
        cfg.W = o.W;
        cfg.H = o.H;
        cfg.seed = o.seed;
        cfg.shaderDir = o.shaderDir;
        GpuSim gpu(ctx, cfg, elements);
        gpu.load(makeScene(elements, scene, cfg.W, cfg.H, o.seed, o.fill));
        for (int t = 0; t < o.ticks; t += 10)
            ctx.immediate([&](VkCommandBuffer cmd) {
                for (int k = 0; k < 10 && t + k < o.ticks; k++)
                    gpu.recordTick(cmd);
            });
        RenderParams rp;
        ctx.immediate([&](VkCommandBuffer cmd) { gpu.recordWorldImage(cmd, rp); });
        std::vector<uint32_t> world = gpu.readWorldImage();
        std::printf("== %s at %ux%u\n", scene.c_str(), cfg.W, cfg.H);
        for (int f : {1, 2, 3, 6, 8, 12, 16}) {
            uint32_t sw = cfg.W / uint32_t(f), sh = cfg.H / uint32_t(f);
            gpu.ensureFrameBuffer(sw, sh);
            RenderParams zp;
            zp.zoom = 1.0f / float(f);
            zp.screenW = sw;
            zp.screenH = sh;
            ctx.immediate([&](VkCommandBuffer cmd) { gpu.recordRender(cmd, zp, false); });
            std::vector<uint32_t> screen(size_t(sw) * sh);
            ctx.download(gpu.frameBuffer(), screen.data(), screen.size() * 4);
            int worst = 0;
            double sum = 0;
            for (uint32_t sy = 0; sy < sh; sy++)
                for (uint32_t sx = 0; sx < sw; sx++) {
                    int ref[3] = {0, 0, 0};
                    for (int y = 0; y < f; y++)
                        for (int x = 0; x < f; x++) {
                            uint32_t c = world[size_t(sy * f + y) * cfg.W + sx * f + x];
                            ref[0] += int((c >> 16) & 0xFF), ref[1] += int((c >> 8) & 0xFF), ref[2] += int(c & 0xFF);
                        }
                    uint32_t s = screen[size_t(sy) * sw + sx];
                    int got[3] = {int((s >> 16) & 0xFF), int((s >> 8) & 0xFF), int(s & 0xFF)};
                    for (int ch = 0; ch < 3; ch++) {
                        int d = std::abs(got[ch] - int(std::lround(double(ref[ch]) / (f * f))));
                        worst = std::max(worst, d);
                        sum += d;
                    }
                }
            bool ok = worst <= 2;
            failures += !ok;
            std::printf("  1/%-2d %4ux%-4u largest difference %d, mean %.3f%s\n", f, sw, sh, worst,
                        sum / (3.0 * sw * sh), ok ? "" : "  (more than rounding)");
        }
    }
    return failures ? 1 : 0;
}

// --render-check: runs TPT for --ticks on each scene, gives the GPU engine exactly that state (particles, walls,
// air) and draws it with both renderers for each display preset, comparing pixel by pixel. Elements whose looks
// flicker at random (sparks, flares, GLOW, GOLD...) differ where they are drawn; everything else should match.
int runRenderCheck(VkContext &ctx, ClassicSim &classic, const ElementTable &elements, const AppOptions &o) {
    std::vector<int> presets = o.renderPresets;
    if (presets.empty())
        for (int p = 0; p < ClassicSim::presetCount(); p++)
            presets.push_back(p);
    const int W = ClassicSim::W, H = ClassicSim::H;
    for (const std::string &scene : o.scenes) {
        SceneData sd = makeScene(elements, scene, W, H, o.seed, o.fill);
        classic.clear();
        classic.seed(o.seed);
        if (!sd.walls.empty())
            classic.setWalls(sd.walls);
        classic.setSigns(sd.signs);
        for (size_t i = 0; i < sd.ptype.size(); i++) {
            const Particle &sp = sd.parts[i], &dp = elements.defaults[sd.ptype[i]];
            int id = classic.createPart(int(sp.x), int(sp.y), int(sd.ptype[i]));
            if (id < 0 && sd.stacked)
                id = classic.createPartStacked(int(sp.x), int(sp.y), int(sd.ptype[i]));
            if (sp.temp != dp.temp)
                classic.setParticleTemp(id, sp.temp);
            if (sp.ctype != dp.ctype || sp.tmp != dp.tmp || sp.tmp2 != dp.tmp2 || sp.tmp3 != dp.tmp3 || sp.tmp4 != dp.tmp4)
                classic.setParticleFields(id, sp.ctype, sd.ptype[i] == PT_FIGH ? classic.particleTmp(id) : sp.tmp,
                                          sp.tmp2, sp.tmp3, sp.tmp4);
        }
        for (int t = 0; t < o.ticks; t++)
            classic.step();

        SimConfig cfg;
        cfg.W = W;
        cfg.H = H;
        cfg.seed = o.seed;
        cfg.shaderDir = o.shaderDir;
        cfg.settings.newtonianGravity = o.newtonian;
        GpuSim gpu(ctx, cfg, elements);
        SceneData state = sceneFromParticles(elements, classic.particles(), W, H, 0, 0);
        state.walls = classic.walls();
        state.signs = classic.signs();
        gpu.load(state);
        std::vector<float> pv, vx, vy, hv;
        classic.airMaps(pv, vx, vy, hv);
        gpu.writeAir(pv, vx, vy, hv);
        std::vector<float> gfx, gfy;
        classic.gravityField(gfx, gfy);
        gpu.setGravityField(gfx, gfy);
        gpu.sampleSigns();

        std::printf("\n== %s after %d ticks, %d particles: pixels that match TPT's renderer\n", scene.c_str(), o.ticks,
                    classic.particleCount());
        std::printf("  %-30s %9s %9s %9s\n", "preset", "exact", "within 8", "mean diff");
        for (int preset : presets) {
            classic.clearRenderAccumulation();
            gpu.clearRenderAccumulation();
            std::vector<uint32_t> tpt, g;
            RenderParams rp;
            rp.preset = preset;
            rp.overlays = uint32_t(o.overlays);
            for (int f = 0; f < o.renderFrames; f++) {
                classic.render(preset, tpt, rp.overlays);
                ctx.immediate([&](VkCommandBuffer cmd) { gpu.recordWorldImage(cmd, rp); });
            }
            g = gpu.readWorldImage();
            size_t exact = 0, close = 0;
            double sum = 0;
            std::vector<uint32_t> diff(tpt.size());
            for (size_t k = 0; k < tpt.size(); k++) {
                int d = 0;
                for (int s = 0; s < 24; s += 8)
                    d = std::max(d, std::abs(int((tpt[k] >> s) & 0xFF) - int((g[k] >> s) & 0xFF)));
                exact += d == 0;
                close += d <= 8;
                sum += d;
                int v = std::min(255, d * 4);
                diff[k] = uint32_t(v << 16 | v << 8 | v);
            }
            double n = double(tpt.size());
            std::printf("  %-30s %8.3f%% %8.3f%% %9.3f\n", ClassicSim::presetName(preset).c_str(), 100 * exact / n,
                        100 * close / n, sum / n);
            if (!o.snapshot.empty()) {
                // TPT, GPU and the difference (x4) side by side
                std::vector<uint32_t> side(size_t(W) * 3 * H);
                for (int y = 0; y < H; y++)
                    for (int x = 0; x < W; x++) {
                        side[size_t(y) * 3 * W + x] = tpt[size_t(y) * W + x];
                        side[size_t(y) * 3 * W + W + x] = g[size_t(y) * W + x] & 0xFFFFFF;
                        side[size_t(y) * 3 * W + 2 * W + x] = diff[size_t(y) * W + x];
                    }
                writePpm(o.snapshot + "-" + scene + "-p" + std::to_string(preset) + ".ppm", side, 3 * W, H);
            }
        }
        std::fflush(stdout);
    }
    return 0;
}

int runCompare(const AppOptions &o) {
    if (o.screenCheck)
        return runScreenCheck(o); // (it makes its own TPT engine, and there may be only one)
    VkContext ctx({}, o.validate);
    ctx.createDevice();
    ElementTable elements = ElementTable::build();
    ClassicSim classic;
    if (o.noAirCoupling) {
        for (int t = 1; t < MAX_ELEMENTS; t++) {
            elements.elements[t].props.advection = 0;
            elements.elements[t].props.airDrag = 0;
            if (!classic.elementName(t).empty())
                classic.setAirCoupling(t, 0, 0);
        }
    }
    ClassicOptions copts;
    copts.airMode = o.tptAirMode;
    copts.edgeMode = o.edgeMode;
    copts.gravityMode = o.gravityMode;
    copts.ambientHeat = o.ambientHeat;
    copts.newtonianGravity = o.newtonian;
    classic.setOptions(copts);

    if (o.airTest)
        return runAirTest(ctx, classic, elements, o);
    if (!o.checkSave.empty())
        return runSaveCheck(ctx, classic, elements, o);
    if (o.renderCheck)
        return runRenderCheck(ctx, classic, elements, o);
    if (!o.checkWorld.empty())
        return runWorldCheck(ctx, elements, o);
    if (!o.checkExport.empty())
        return runExportCheck(ctx, classic, elements, o);
    if (o.zoomCheck)
        return runZoomCheck(ctx, elements, o);
    if (o.sleepStats)
        return runSleepStats(ctx, elements, o);
    if (!o.uiSnapshot.empty())
        return runUiSnapshot(ctx, elements, o);
    if (o.toolCheck)
        return runToolCheck(ctx, classic, elements, o);

    std::vector<int> checkpoints;
    for (int t = o.interval; t <= o.ticks; t += o.interval)
        checkpoints.push_back(t);
    if (checkpoints.empty())
        checkpoints.push_back(o.ticks);

    std::printf("Comparing the GPU engine with TPT v100.1.400 at %dx%d. Distance = share of\n"
                "particles that would have to move to make the two element/position distributions match, at\n"
                "%d px resolution. Baselines: 'TPT seed' = TPT with another random seed; 'TPT order' = TPT with\n"
                "the same particles created in random order (TPT updates in creation order). dT = mean\n"
                "|temperature difference| per element and bin, in K, for the same three pairs.\n",
                ClassicSim::W, ClassicSim::H, BIN);

    for (const std::string &scene : o.scenes) {
        for (AppOptions::Colours n : o.colours) {
            // Build the scene in TPT with TPT's create_part, so every element gets TPT's own defaults.
            SceneData sd = makeScene(elements, scene, ClassicSim::W, ClassicSim::H, o.seed, o.fill);
            std::string err;
            // TPT updates particles in id order, i.e. creation order. The scene is created row by row from the
            // top (like a brush), and a second time in random order to measure how much the order alone matters.
            // (Not through a save: loading a save renumbers particles in screen order.)
            auto build = [&](bool shuffled) {
                std::vector<size_t> order(sd.ptype.size());
                for (size_t i = 0; i < order.size(); i++)
                    order[i] = i;
                if (shuffled)
                    std::shuffle(order.begin(), order.end(), std::mt19937(o.seed + 7));
                classic.clear();
                classic.seed(o.seed);
                if (!sd.walls.empty())
                    classic.setWalls(sd.walls);
                for (size_t i : order) {
                    int id = classic.createPart(int(sd.parts[i].x), int(sd.parts[i].y), int(sd.ptype[i]));
                    if (id < 0 && sd.stacked)
                        id = classic.createPartStacked(int(sd.parts[i].x), int(sd.parts[i].y), int(sd.ptype[i]));
                    // Scenes may set temperatures (e.g. a red-hot plate); otherwise this is TPT's default.
                    if (sd.parts[i].temp != elements.defaults[sd.ptype[i]].temp)
                        classic.setParticleTemp(id, sd.parts[i].temp);
                    // They may also set ctype and tmp (what a CRAY shoots, what a STOR holds). SEED keeps TPT's own
                    // random genes (the scene builder only imitates its Create).
                    const Particle &sp = sd.parts[i], &dp = elements.defaults[sd.ptype[i]];
                    if (sd.ptype[i] != PT_SEED && (sp.ctype != dp.ctype || sp.tmp != dp.tmp || sp.tmp2 != dp.tmp2 ||
                                                   sp.tmp3 != dp.tmp3 || sp.tmp4 != dp.tmp4))
                        // (a fighter's tmp is the slot TPT gave it)
                        classic.setParticleFields(id, sp.ctype, sd.ptype[i] == PT_FIGH ? classic.particleTmp(id) : sp.tmp,
                                                  sp.tmp2, sp.tmp3, sp.tmp4);
                }
                // Keys held for the players (after creating them, which clears their keys)
                const std::vector<int> &k = o.stickmanKeys;
                classic.setStickmanKeys(k[0], k[1], k[2], k[3]);
            };
            build(false);
            std::vector<ClassicParticle> initialParticles = classic.particles();
            std::vector<uint8_t> initialWalls = classic.walls();

            double msA, msB;
            std::string img = o.snapshot.empty() ? "" : o.snapshot + "-" + scene;
            auto a = runClassic(classic, [&] { build(false); }, o.seed, checkpoints, msA, img.empty() ? "" : img + "-tpt.ppm");
            auto b = runClassic(classic, [&] { build(false); }, o.seed + 1000, checkpoints, msB);
            auto c = runClassic(classic, [&] { build(true); }, o.seed, checkpoints, msB,
                                img.empty() ? "" : img + "-tpt-shuffled.ppm");

            SimConfig cfg;
            cfg.W = ClassicSim::W;
            cfg.H = ClassicSim::H;
            cfg.colourNX = n.nx;
            cfg.colourNY = n.ny;
            cfg.seed = o.seed;
            cfg.air.mode = uint32_t(o.tptAirMode);
            cfg.settings.edgeMode = uint32_t(o.edgeMode);
            cfg.settings.gravityMode = uint32_t(o.gravityMode);
            cfg.settings.ambientHeat = o.ambientHeat;
            cfg.settings.newtonianGravity = o.newtonian;
            cfg.passOrder = uint32_t(o.passOrder);
            cfg.shaderDir = o.shaderDir;
            GpuSim gpu(ctx, cfg, elements);
            // The GPU engine starts from exactly TPT run A's starting particles.
            gpu.setDebugCounters(true);
            gpu.setSerial(o.serial);
            gpu.setStickmanKeys(uint8_t(o.stickmanKeys[0]), uint8_t(o.stickmanKeys[1]), uint8_t(o.stickmanKeys[2]),
                                uint8_t(o.stickmanKeys[3]));
            auto g = runGpu(ctx, gpu, elements, initialParticles, initialWalls, checkpoints,
                            img.empty() ? "" : img + "-gpu.ppm");
            SimStats gs = gpu.readStats();

            std::printf("\n== %s, %zu particles, GPU %ux%u passes (TPT %.2f ms/tick on one CPU core)\n", scene.c_str(),
                        initialParticles.size(), n.nx, n.ny, msA);
            std::printf("  %6s %10s %10s %10s %12s %12s %9s %9s %9s\n", "tick", "TPT parts", "GPU parts", "TPT seed",
                        "TPT order", "TPT vs GPU", "dT seed", "dT order", "dT GPU");
            for (size_t i = 0; i < checkpoints.size(); i++) {
                std::printf("  %6d %10d %10d %9.1f%% %11.1f%% %11.1f%% %9.1f %9.1f %9.1f\n", checkpoints[i], a[i].total,
                            g[i].total, 100 * distance(a[i], b[i]), 100 * distance(a[i], c[i]),
                            100 * distance(a[i], g[i]), tempDistance(a[i], b[i]), tempDistance(a[i], c[i]),
                            tempDistance(a[i], g[i]));
            }
            if (o.newtonian) {
                std::printf("    gravity field off from TPT's (RMS):");
                for (size_t i = 0; i < checkpoints.size(); i++)
                    std::printf(" %d: seed %.1f%% GPU %.1f%% (TPT %.3g GPU %.3g)", checkpoints[i],
                                100 * fieldDistance(a[i], b[i]), 100 * fieldDistance(a[i], g[i]), fieldRms(a[i]),
                                fieldRms(g[i]));
                std::printf("\n");
            }
            printTypes("TPT", a.back(), classic);
            printTypes("TPT'", b.back(), classic);
            printTypes("TPT~", c.back(), classic);
            printTypes("GPU", g.back(), classic);
            std::printf("    GPU moves: %u done; failed only because of another thread: %u locked target, %u displaced,"
                        " %u lost race, %u same-pass swap refused\n",
                        gs.moved, gs.lockedTarget, gs.ownLost, gs.casLost, gs.samePass);
            std::fflush(stdout);
        }
    }
    return 0;
}

} // namespace bp
