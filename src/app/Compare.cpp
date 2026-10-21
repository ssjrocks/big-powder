// Accuracy check: runs the same starting state through TPT's own engine (Classic) and the GPU engine at
// TPT's size, and measures how far apart the results drift.
//
// Falling-sand simulations are chaotic, so two TPT runs that differ only in their random seed also drift
// apart. The noise floor is therefore TPT against TPT with a different seed; the GPU engine is as accurate
// as it can be when its distance from TPT is no larger than that.
#include "app/App.h"
#include "classic/ClassicSim.h"
#include "sim/GpuSim.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <map>
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
    std::map<int, std::vector<int>> bins; // per type, (W/BIN)*(H/BIN) counts
};

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
        if (b.empty())
            b.assign(size_t(bw) * bh, 0);
        b[size_t(y / BIN) * bw + x / BIN]++;
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
                             const std::vector<ClassicParticle> &initial, const std::vector<int> &checkpoints,
                             const std::string &imagePath = "") {
    sim.load(sceneFromParticles(elements, initial, ClassicSim::W, ClassicSim::H, 0, 0));
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
    classic.setOptions(copts);

    std::vector<int> checkpoints;
    for (int t = o.interval; t <= o.ticks; t += o.interval)
        checkpoints.push_back(t);
    if (checkpoints.empty())
        checkpoints.push_back(o.ticks);

    std::printf("Comparing the GPU engine with TPT v100.1.400 at %dx%d. Distance = share of\n"
                "particles that would have to move to make the two element/position distributions match, at\n"
                "%d px resolution. Baselines: 'TPT seed' = TPT with another random seed; 'TPT order' = TPT with\n"
                "the same particles created in random order (TPT updates in creation order).\n",
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
                for (size_t i : order)
                    classic.createPart(int(sd.parts[i].x), int(sd.parts[i].y), int(sd.ptype[i]));
            };
            build(false);
            std::vector<ClassicParticle> initialParticles = classic.particles();

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
            cfg.passOrder = uint32_t(o.passOrder);
            cfg.shaderDir = o.shaderDir;
            GpuSim gpu(ctx, cfg, elements);
            // The GPU engine starts from exactly TPT run A's starting particles.
            gpu.setDebugCounters(true);
            gpu.setSerial(o.serial);
            auto g = runGpu(ctx, gpu, elements, initialParticles, checkpoints, img.empty() ? "" : img + "-gpu.ppm");
            SimStats gs = gpu.readStats();

            std::printf("\n== %s, %zu particles, GPU %ux%u passes (TPT %.2f ms/tick on one CPU core)\n", scene.c_str(),
                        initialParticles.size(), n.nx, n.ny, msA);
            std::printf("  %6s %10s %10s %10s %12s %12s\n", "tick", "TPT parts", "GPU parts", "TPT seed", "TPT order",
                        "TPT vs GPU");
            for (size_t i = 0; i < checkpoints.size(); i++) {
                std::printf("  %6d %10d %10d %9.1f%% %11.1f%% %11.1f%%\n", checkpoints[i], a[i].total, g[i].total,
                            100 * distance(a[i], b[i]), 100 * distance(a[i], c[i]), 100 * distance(a[i], g[i]));
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
