// Headless benchmark: runs scenes for a number of ticks and reports GPU time per tick, plus checks that
// no particles were lost or duplicated.
#include "app/App.h"
#include "sim/GpuSim.h"

#include <algorithm>
#include <chrono>
#include <cstdio>

namespace bp {

namespace {

struct TickTimes {
    std::vector<double> listMs, updateMs, afterMs, totalMs; // updateMs: the update passes; afterMs: the work after them
};

double percentile(std::vector<double> v, double p) {
    if (v.empty())
        return 0;
    std::sort(v.begin(), v.end());
    return v[std::min(v.size() - 1, size_t(p * double(v.size() - 1) + 0.5))];
}

double mean(const std::vector<double> &v) {
    double s = 0;
    for (double x : v)
        s += x;
    return v.empty() ? 0 : s / double(v.size());
}

void printStats(const char *label, const SimStats &s, const ElementTable &el) {
    std::printf("  %s: %llu particles (", label, (unsigned long long)s.total);
    bool first = true;
    for (int t = 1; t < MAX_ELEMENTS; t++) {
        if (!s.typeCount[t])
            continue;
        std::printf("%s%s %u", first ? "" : ", ", el.elements[t].name.c_str(), s.typeCount[t]);
        first = false;
    }
    std::printf(")\n");
    std::printf("    checks: not-in-pmap %u, pmap-dangling %u, pmap-wrong-pos %u, pmap-locked %u, list-overflow %u, "
                "stacked %u\n",
                s.notInPmap, s.dangling, s.wrongPos, s.locked, s.listOverflow, s.stacked);
}

// Renders a view of the world and writes it as a binary PPM.
void writeSnapshot(VkContext &ctx, GpuSim &sim, const std::string &path, uint32_t w, uint32_t h, float zoom,
                   float centerX, float centerY, int preset) {
    sim.ensureFrameBuffer(w, h);
    RenderParams rp;
    rp.zoom = zoom;
    rp.camX = centerX - float(w) / zoom * 0.5f;
    rp.camY = centerY - float(h) / zoom * 0.5f;
    rp.screenW = w;
    rp.screenH = h;
    rp.preset = preset;
    ctx.immediate([&](VkCommandBuffer cmd) { sim.recordRender(cmd, rp); });
    std::vector<uint32_t> px(size_t(w) * h);
    ctx.download(sim.frameBuffer(), px.data(), px.size() * 4);
    FILE *f = std::fopen(path.c_str(), "wb");
    if (!f)
        throw std::runtime_error("cannot write " + path);
    std::fprintf(f, "P6\n%u %u\n255\n", w, h);
    std::vector<unsigned char> row(size_t(w) * 3);
    for (uint32_t y = 0; y < h; y++) {
        for (uint32_t x = 0; x < w; x++) {
            uint32_t c = px[size_t(y) * w + x];
            row[x * 3 + 0] = (c >> 16) & 0xFF;
            row[x * 3 + 1] = (c >> 8) & 0xFF;
            row[x * 3 + 2] = c & 0xFF;
        }
        std::fwrite(row.data(), 1, row.size(), f);
    }
    std::fclose(f);
    std::printf("  wrote %s\n", path.c_str());
}

} // namespace

int runBench(const AppOptions &o) {
    VkContext ctx({}, o.validate);
    ctx.createDevice();
    ElementTable elements = ElementTable::build();

    SimConfig cfg;
    cfg.W = o.W;
    cfg.H = o.H;
    cfg.colourNX = o.colours.empty() ? 7 : o.colours[0].nx;
    cfg.colourNY = o.colours.empty() ? 7 : o.colours[0].ny;
    cfg.seed = o.seed;
    cfg.passOrder = uint32_t(o.passOrder);
        cfg.shaderDir = o.shaderDir;
        cfg.settings.edgeMode = uint32_t(o.edgeMode);
        cfg.settings.gravityMode = uint32_t(o.gravityMode);
        cfg.settings.ambientHeat = o.ambientHeat;
    GpuSim sim(ctx, cfg, elements);

    const int batch = std::max(1, o.batch);
    VkQueryPoolCreateInfo qci{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
    qci.queryType = VK_QUERY_TYPE_TIMESTAMP;
    qci.queryCount = uint32_t(batch * 4); // per tick: start, after the lists, after the passes, end
    VkQueryPool pool;
    VK_CHECK(vkCreateQueryPool(ctx.device(), &qci, nullptr, &pool));
    const double tsPeriodMs = double(ctx.properties().limits.timestampPeriod) * 1e-6;

    VkCommandBufferAllocateInfo ai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    ai.commandPool = ctx.commandPool();
    ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    ai.commandBufferCount = 1;
    VkCommandBuffer cmd;
    VK_CHECK(vkAllocateCommandBuffers(ctx.device(), &ai, &cmd));
    VkFenceCreateInfo fci{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    VkFence fence;
    VK_CHECK(vkCreateFence(ctx.device(), &fci, nullptr, &fence));

    // Runs n ticks in batches; collects GPU times when `times` is set.
    auto run = [&](int n, TickTimes *times) {
        for (int done = 0; done < n; done += batch) {
            int k = std::min(batch, n - done);
            VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
            bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
            VK_CHECK(vkBeginCommandBuffer(cmd, &bi));
            vkCmdResetQueryPool(cmd, pool, 0, uint32_t(batch * 4));
            for (int t = 0; t < k; t++)
                sim.recordTick(cmd, times ? pool : VK_NULL_HANDLE, uint32_t(t * 4));
            VK_CHECK(vkEndCommandBuffer(cmd));
            VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
            si.commandBufferCount = 1;
            si.pCommandBuffers = &cmd;
            VK_CHECK(vkResetFences(ctx.device(), 1, &fence));
            VK_CHECK(vkQueueSubmit(ctx.queue(), 1, &si, fence));
            VK_CHECK(vkWaitForFences(ctx.device(), 1, &fence, VK_TRUE, UINT64_MAX));
            if (times) {
                std::vector<uint64_t> ts(size_t(k) * 4);
                VK_CHECK(vkGetQueryPoolResults(ctx.device(), pool, 0, uint32_t(k * 4), ts.size() * 8, ts.data(), 8,
                                               VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT));
                for (int t = 0; t < k; t++) {
                    const uint64_t *q = &ts[size_t(t) * 4];
                    times->listMs.push_back(double(q[1] - q[0]) * tsPeriodMs);
                    times->updateMs.push_back(double(q[3] - q[1]) * tsPeriodMs);
                    times->afterMs.push_back(double(q[2] - q[3]) * tsPeriodMs);
                    times->totalMs.push_back(double(q[2] - q[0]) * tsPeriodMs);
                }
            }
        }
    };

    std::printf("world %ux%u (%.1f MP), %d warmup + %d timed ticks, %d per submit\n", o.W, o.H,
                double(o.W) * o.H / 1e6, o.warmup, o.ticks, batch);
    for (const std::string &scene : o.scenes) {
        for (AppOptions::Colours n : o.colours) {
            sim.setColours(n.nx, n.ny);
            sim.load(makeScene(elements, scene, o.W, o.H, o.seed, o.fill));
            std::printf("\n== scene %s, colours %ux%u (%u passes/tick)\n", scene.c_str(), n.nx, n.ny, n.nx * n.ny);
            SimStats before = sim.readStats();
            printStats("start", before, elements);

            run(o.warmup, nullptr);
            TickTimes times;
            auto t0 = std::chrono::steady_clock::now();
            run(o.ticks, &times);
            double wallS = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();

            SimStats after = sim.readStats();
            printStats("end  ", after, elements);
            double m = mean(times.totalMs);
            std::printf("  GPU ms/tick: mean %.3f, p50 %.3f, p95 %.3f, max %.3f  (lists %.3f, passes %.3f, after %.3f, "
                        "after p95 %.3f)\n",
                        m, percentile(times.totalMs, 0.5), percentile(times.totalMs, 0.95), percentile(times.totalMs, 1.0),
                        mean(times.listMs), mean(times.updateMs), mean(times.afterMs), percentile(times.afterMs, 0.95));
            std::printf("  => %.0f ticks/s on the GPU alone; %.0f ticks/s wall clock incl. submits\n",
                        m > 0 ? 1000.0 / m : 0.0, double(o.ticks) / wallS);
            {
                // Drawing: TPT's renderer for the whole world (Fire Display) and the view of it on a 1920x1080 screen
                sim.ensureFrameBuffer(1920, 1080);
                RenderParams rp;
                rp.zoom = std::min(1920.0f / float(o.W), 1080.0f / float(o.H));
                rp.screenW = 1920;
                rp.screenH = 1080;
                std::vector<double> frameMs, stageMs[5];
                for (int f = 0; f < 30; f++) {
                    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
                    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
                    VK_CHECK(vkBeginCommandBuffer(cmd, &bi));
                    vkCmdResetQueryPool(cmd, pool, 0, 7);
                    vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, pool, 0);
                    sim.recordWorldImage(cmd, rp, pool, 1);
                    sim.recordRender(cmd, rp, false);
                    vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, pool, 6);
                    VK_CHECK(vkEndCommandBuffer(cmd));
                    VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
                    si.commandBufferCount = 1;
                    si.pCommandBuffers = &cmd;
                    VK_CHECK(vkResetFences(ctx.device(), 1, &fence));
                    VK_CHECK(vkQueueSubmit(ctx.queue(), 1, &si, fence));
                    VK_CHECK(vkWaitForFences(ctx.device(), 1, &fence, VK_TRUE, UINT64_MAX));
                    uint64_t ts[7];
                    VK_CHECK(vkGetQueryPoolResults(ctx.device(), pool, 0, 7, sizeof(ts), ts, 8,
                                                   VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT));
                    frameMs.push_back(double(ts[6] - ts[0]) * tsPeriodMs);
                    for (int k = 0; k < 5; k++)
                        stageMs[k].push_back(double(ts[k + 2] - ts[k + 1]) * tsPeriodMs);
                }
                std::printf("  drawing (%s, whole world): mean %.3f ms/frame, p95 %.3f (graphics %.3f, fire buffer %.3f, "
                            "pixels %.3f, fire smoothing %.3f, screen %.3f)\n",
                            ClassicSim::presetName(rp.preset).c_str(), mean(frameMs), percentile(frameMs, 0.95),
                            mean(stageMs[0]), mean(stageMs[1]), mean(stageMs[2]), mean(stageMs[3]), mean(stageMs[4]));
            }
            if (!o.snapshot.empty()) {
                std::string base = o.snapshot + "-" + scene + "-" + std::to_string(n.nx) + "x" + std::to_string(n.ny);
                float fit = std::min(1920.0f / float(o.W), 1080.0f / float(o.H));
                writeSnapshot(ctx, sim, base + ".ppm", 1920, 1080, fit, o.W * 0.5f, o.H * 0.5f, 4);
                // A close-up of the lower-left area, 6x zoom.
                writeSnapshot(ctx, sim, base + "-zoom.ppm", 960, 540, 6.0f, o.W * 0.2f, o.H * 0.8f, 4);
            }
            std::fflush(stdout);
        }
    }

    vkDestroyFence(ctx.device(), fence, nullptr);
    vkDestroyQueryPool(ctx.device(), pool, nullptr);
    return 0;
}

} // namespace bp
