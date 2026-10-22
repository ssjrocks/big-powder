// Accuracy check: runs the same starting state through TPT's own engine (Classic) and the GPU engine at
// TPT's size, and measures how far apart the results drift.
//
// Falling-sand simulations are chaotic, so two TPT runs that differ only in their random seed also drift
// apart. The noise floor is therefore TPT against TPT with a different seed; the GPU engine is as accurate
// as it can be when its distance from TPT is no larger than that.
#include "sim/WorldFile.h"
#include "app/App.h"
#include "classic/ClassicSim.h"
#include "sim/GpuSim.h"

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
