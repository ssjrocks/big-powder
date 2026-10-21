// Classic mode: TPT's own engine and renderer, shown through the viewer's camera.
//
// TPT's simulation and renderer run on a worker thread, so a slow tick doesn't hold up drawing. The drawing
// thread queues brush strokes and ticks under a short lock (qm_) and shows the most recent picture the worker
// finished. TPT's engine itself is guarded by sm_, taken by the worker while it steps and by user actions
// (loading, saving, settings), never by the per-frame drawing path.
#include "app/World.h"
#include "classic/ClassicSim.h"
#include "gpu/ImageBlit.h"
#include "sim/Scenes.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>

namespace bp {

namespace {

class ClassicWorld : public World {
public:
    ClassicWorld(VkContext &ctx, const AppOptions &o)
        : o_(o), blit_(ctx, ClassicSim::W, ClassicSim::H, o.shaderDir) {
        for (int p = 0; p < ClassicSim::presetCount(); p++)
            if (ClassicSim::presetName(p) == "Fire Display")
                preset_ = p; // TPT's default
        names_.resize(1024);
        for (int t = 1; t < 1024; t++) {
            std::string n = sim_.elementName(t);
            names_[t] = n;
            if (!n.empty() && sim_.elementByName(n) == t)
                elements_.push_back(t);
        }
        sim_.render(preset_, shown_);
        particles_ = sim_.particleCount();
        worker_ = std::thread([this] { run(); });
    }

    ~ClassicWorld() override {
        {
            std::lock_guard lk(qm_);
            quit_ = true;
        }
        cv_.notify_all();
        worker_.join();
    }

    std::string modeName() const override { return "classic"; }
    uint32_t width() const override { return ClassicSim::W; }
    uint32_t height() const override { return ClassicSim::H; }
    void ensureFrameBuffer(uint32_t w, uint32_t h) override { blit_.ensureFrameBuffer(w, h); }
    const Buffer &frameBuffer() const override { return blit_.frameBuffer(); }

    void record(VkCommandBuffer cmd, const FrameRequest &f) override {
        {
            std::lock_guard lk(qm_);
            walls_.insert(walls_.end(), f.walls.begin(), f.walls.end());
            brush_.insert(brush_.end(), f.brush.begin(), f.brush.end());
            // If the worker falls behind, the simulation slows down instead of piling up ticks.
            pendingTicks_ = std::min(pendingTicks_ + f.ticks, 8);
            if (!f.walls.empty() || !f.brush.empty() || f.ticks > 0)
                dirty_ = true;
            if (fresh_) {
                shown_.swap(rendered_);
                fresh_ = false;
            }
        }
        cv_.notify_one();
        BlitParams bp;
        bp.camX = f.camX;
        bp.camY = f.camY;
        bp.zoom = f.zoom;
        bp.screenW = f.screenW;
        bp.screenH = f.screenH;
        bp.brushX = f.brushX;
        bp.brushY = f.brushY;
        bp.brushR = f.brushR;
        blit_.record(cmd, f.slot, shown_, bp);
    }

    bool gpuTimed() const override { return false; }
    double cpuTickMs() const override { return tickMs_.load(); }
    uint64_t particleCount() override { return uint64_t(particles_.load()); }

    std::vector<int> elements() const override { return elements_; }
    std::string elementName(int type) const override {
        if (type == 0)
            return "erase";
        return type > 0 && type < int(names_.size()) ? names_[type] : "";
    }

    void loadScene(const std::string &name) override {
        // Built with TPT's create_part, so every element gets TPT's own defaults.
        ElementTable table = ElementTable::build();
        SceneData sd = makeScene(table, name, ClassicSim::W, ClassicSim::H, o_.seed, o_.fill);
        {
            std::lock_guard lk(sm_);
            sim_.clear();
            if (!sd.walls.empty())
                sim_.setWalls(sd.walls);
            for (size_t i = 0; i < sd.ptype.size(); i++) {
                int id = sim_.createPart(int(sd.parts[i].x), int(sd.parts[i].y), int(sd.ptype[i]));
                if (sd.parts[i].temp != table.defaults[sd.ptype[i]].temp)
                    sim_.setParticleTemp(id, sd.parts[i].temp);
            }
        }
        poke();
    }

    bool loadSave(const std::string &path, int, int, std::string &message) override {
        std::string err;
        bool ok;
        int n = 0;
        {
            std::lock_guard lk(sm_);
            sim_.clear();
            ok = sim_.loadFile(path, err);
            n = sim_.particleCount();
        }
        poke();
        if (!ok) {
            message = "could not load " + path + ": " + err;
            return false;
        }
        message = "loaded " + path + " (" + std::to_string(n) + " particles)";
        return true;
    }

    bool save(const std::string &path, std::string &message) override {
        std::string err;
        std::lock_guard lk(sm_);
        if (!sim_.saveFile(path, err)) {
            message = "could not save: " + err;
            return false;
        }
        message = "saved " + path;
        return true;
    }
    std::string saveExtension() const override { return ".cps"; }

    void applySettings(const WorldSettings &s) override {
        {
            std::lock_guard lk(sm_);
            ClassicOptions co = sim_.options();
            co.edgeMode = s.edgeMode;
            co.gravityMode = s.gravityMode;
            co.airMode = s.airMode;
            co.ambientHeat = s.ambientHeat;
            sim_.setOptions(co);
        }
        poke();
    }

    void cycleDisplay() override {
        preset_ = (preset_ + 1) % ClassicSim::presetCount();
        poke();
    }
    std::string displayName() const override { return ClassicSim::presetName(preset_); }

private:
    // Asks the worker for a new picture (after the simulation changed outside of a tick).
    void poke() {
        {
            std::lock_guard lk(qm_);
            dirty_ = true;
        }
        cv_.notify_one();
    }

    void run() {
        std::vector<uint32_t> picture;
        std::vector<BrushOp> brush;
        std::vector<WallOp> walls;
        std::unique_lock q(qm_);
        while (true) {
            cv_.wait(q, [this] { return quit_ || dirty_; });
            if (quit_)
                return;
            dirty_ = false;
            brush.swap(brush_);
            walls.swap(walls_);
            int ticks = pendingTicks_;
            pendingTicks_ = 0;
            q.unlock();
            {
                std::lock_guard s(sm_);
                for (const WallOp &w : walls)
                    sim_.paintWalls(w.cx, w.cy, w.r, w.wall);
                for (const BrushOp &b : brush)
                    sim_.brush(b.x, b.y, b.r, b.type);
                if (ticks > 0) {
                    auto t0 = std::chrono::steady_clock::now();
                    for (int t = 0; t < ticks; t++)
                        sim_.step();
                    double ms =
                        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
                    tickMs_ = tickMs_.load() * 0.9 + 0.1 * ms / ticks;
                    particles_ = sim_.particleCount();
                }
                sim_.render(preset_.load(), picture);
            }
            brush.clear();
            walls.clear();
            q.lock();
            rendered_.swap(picture);
            fresh_ = true;
        }
    }

    AppOptions o_;
    ClassicSim sim_;
    ImageBlit blit_;
    std::vector<int> elements_;
    std::vector<std::string> names_;

    std::mutex sm_; // TPT's engine (sim_)
    std::mutex qm_; // the queue and the finished picture below
    std::condition_variable cv_;
    std::thread worker_;
    bool quit_ = false, dirty_ = false, fresh_ = false;
    int pendingTicks_ = 0;
    std::vector<BrushOp> brush_;
    std::vector<WallOp> walls_;
    std::vector<uint32_t> rendered_; // the worker's latest picture
    std::vector<uint32_t> shown_;    // the picture being drawn (drawing thread only)
    std::atomic<int> preset_{4};
    std::atomic<double> tickMs_{0.0};
    std::atomic<int> particles_{0};
};

} // namespace

std::unique_ptr<World> makeClassicWorld(VkContext &ctx, const AppOptions &o) {
    return std::make_unique<ClassicWorld>(ctx, o);
}

} // namespace bp
