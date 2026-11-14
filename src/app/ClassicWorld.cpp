// Classic mode: TPT's own engine and renderer, shown through the viewer's camera.
//
// TPT's simulation and renderer run on a worker thread, so a slow tick doesn't hold up drawing. The drawing
// thread queues the tools' uses and ticks under a short lock (qm_) and shows the most recent picture the worker
// finished. TPT's engine itself is guarded by sm_, taken by the worker while it steps and by user actions
// (loading, saving, settings), never by the per-frame drawing path.
#include "ui/BrushCursor.h"
#include "app/World.h"
#include "classic/ClassicSim.h"
#include "classic/TptSave.h"
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
        names_.resize(1024);
        for (int t = 1; t < 1024; t++) {
            std::string n = sim_.elementName(t);
            names_[t] = n;
            if (!n.empty() && sim_.elementByName(n) == t)
                elements_.push_back(t);
        }
        sim_.render(tptDefaultView(), shown_);
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
            uses_.insert(uses_.end(), f.uses.begin(), f.uses.end());
            // If the worker falls behind, the simulation slows down instead of piling up ticks.
            pendingTicks_ = std::min(pendingTicks_ + f.ticks, 8);
            overlays_ = f.overlays;
            // (TPT draws every frame: a frame without a tick still moves the fire and the persistent display on)
            if (!f.uses.empty() || f.ticks > 0 || f.idleFrames > 0 || f.brushX != cursorX_ || f.brushY != cursorY_ ||
                !(f.view == view_))
                dirty_ = true;
            view_ = f.view;
            idleFrames_ = std::min(idleFrames_ + f.idleFrames, 8);
            cursorX_ = f.brushX;
            cursorY_ = f.brushY;
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
        // TPT inverts its cursor's pixels on the simulation's picture
        if (f.cursor.empty()) {
            blit_.record(cmd, f.slot, shown_, bp);
        } else {
            withCursor_ = shown_;
            xorCursor(withCursor_, ClassicSim::W, ClassicSim::H, f.cursor);
            blit_.record(cmd, f.slot, withCursor_, bp);
        }
    }

    UiSample cursorSample() override {
        std::lock_guard lk(qm_);
        return sample_;
    }
    std::vector<Sign> signs() override {
        std::lock_guard s(sm_);
        return sim_.signs();
    }
    void setSigns(const std::vector<Sign> &signs) override {
        {
            std::lock_guard s(sm_);
            sim_.setSigns(signs);
        }
        poke();
    }
    std::vector<SignSample> signSamples(bool) override {
        std::lock_guard s(sm_);
        std::vector<SignSample> out;
        for (const Sign &sg : sim_.signs())
            out.push_back(sim_.signSample(sg.x, sg.y));
        return out;
    }
    void setCustomGol(const std::vector<CustomGol> &types) override {
        std::lock_guard s(sm_);
        sim_.setCustomGol(types);
    }
    uint32_t picturePixel(int x, int y) override {
        if (x < 0 || y < 0 || x >= ClassicSim::W || y >= ClassicSim::H || shown_.empty())
            return 0;
        return shown_[size_t(y) * ClassicSim::W + size_t(x)] & 0xFFFFFF;
    }
    bool particleAt(int x, int y, bool photonsFirst, ClassicParticle &p) override {
        std::lock_guard s(sm_); // (after the worker's batch, if it is running one)
        catchUp();
        int id;
        return sim_.particleAt(x, y, photonsFirst, id, p);
    }
    void apply(const std::vector<ToolUse> &uses) override {
        std::lock_guard lk(qm_);
        uses_.insert(uses_.end(), uses.begin(), uses.end());
        dirty_ = true;
    }
    struct Snapshot : WorldSnapshot {
        std::shared_ptr<const ClassicSim::State> state;
    };
    std::shared_ptr<const WorldSnapshot> snapshot() override {
        std::lock_guard s(sm_);
        catchUp();
        auto out = std::make_shared<Snapshot>();
        out->state = sim_.snapshot();
        return out;
    }
    SaveArea copyArea(int x1, int y1, int x2, int y2, bool includePressure) override {
        std::lock_guard s(sm_);
        catchUp();
        return sim_.saveArea(x1, y1, x2, y2, includePressure);
    }
    std::string pasteArea(const SaveArea &a, bool includePressure, int bx, int by) override {
        {
            std::lock_guard s(sm_);
            catchUp();
            sim_.loadArea(a, includePressure, bx, by);
        }
        poke();
        return "";
    }
    void clearArea(int x, int y, int w, int h) override {
        {
            std::lock_guard s(sm_);
            catchUp();
            sim_.clearArea(x, y, w, h);
        }
        poke();
    }
    std::vector<uint32_t> renderSave(const SaveArea &a) override {
        ViewSettings view;
        {
            std::lock_guard q(qm_);
            view = view_;
        }
        std::lock_guard s(sm_); // (TPT's element tables, which the worker's ticks change too)
        return tptRenderSave(a, view, 0);
    }
    // GameController's actions on the air and sparks, after what is queued
    void act(void (ClassicSim::*f)()) {
        {
            std::lock_guard s(sm_);
            catchUp();
            (sim_.*f)();
        }
        poke();
    }
    void resetAir() override { act(&ClassicSim::resetAir); }
    void resetSpark() override { act(&ClassicSim::resetSpark); }
    void invertAir() override { act(&ClassicSim::invertAir); }
    void resetAmbientHeat() override { act(&ClassicSim::resetAmbientHeat); }
    std::vector<uint32_t> picture() override { return shown_; }
    uint32_t foundParticles() override { return uint32_t(found_.load()); }
    bool heatRange(float &lo, float &hi) override {
        std::lock_guard h(heatMutex_);
        lo = heatLo_, hi = heatHi_;
        return heatValid_;
    }
    void restore(const WorldSnapshot &snap) override {
        {
            std::lock_guard s(sm_);
            catchUp();
            sim_.restore(*static_cast<const Snapshot &>(snap).state);
        }
        poke();
    }
    bool gpuTimed() const override { return false; }
    double cpuTickMs() const override { return tickMs_.load(); }
    // (read now: a load, paste, clear or undo while paused changes it without a tick)
    uint64_t particleCount() override {
        std::lock_guard lk(sm_);
        return uint64_t(sim_.particleCount());
    }

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
            sim_.setOptions(s);
        }
        poke();
    }

    void setStickmanKeys(uint8_t comm1, uint8_t pcomm1, uint8_t comm2, uint8_t pcomm2) override {
        keys_ = uint32_t(comm1) | uint32_t(pcomm1) << 8 | uint32_t(comm2) << 16 | uint32_t(pcomm2) << 24;
    }
    bool stickman2Out() const override { return stkm2Out_; }

private:
    // What was queued first happens first: the uses and ticks not yet run are run now, under sm_
    void catchUp() {
        std::vector<ToolUse> uses;
        int ticks;
        {
            std::lock_guard q(qm_);
            uses.swap(uses_);
            ticks = pendingTicks_;
            pendingTicks_ = 0;
            dirty_ = true;
        }
        TptBrush brush;
        for (const ToolUse &u : uses)
            sim_.useTool(u, brush);
        if (ticks > 0) {
            uint32_t k = keys_;
            sim_.setStickmanKeys(k & 0xFF, (k >> 8) & 0xFF, (k >> 16) & 0xFF, k >> 24);
            for (int t = 0; t < ticks; t++)
                sim_.step();
        }
        cv_.notify_one();
    }

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
        std::vector<ToolUse> uses;
        TptBrush brush; // (set to each use's)
        std::unique_lock q(qm_);
        while (true) {
            cv_.wait(q, [this] { return quit_ || dirty_; });
            if (quit_)
                return;
            dirty_ = false;
            uses.swap(uses_);
            int ticks = pendingTicks_, idle = idleFrames_;
            pendingTicks_ = idleFrames_ = 0;
            int cx = cursorX_, cy = cursorY_;
            const ViewSettings view = view_;
            q.unlock();
            UiSample sample;
            {
                std::lock_guard s(sm_);
                for (const ToolUse &u : uses)
                    sim_.useTool(u, brush);
                if (ticks > 0) {
                    uint32_t k = keys_;
                    sim_.setStickmanKeys(k & 0xFF, (k >> 8) & 0xFF, (k >> 16) & 0xFF, k >> 24);
                    auto t0 = std::chrono::steady_clock::now();
                    for (int t = 0; t < ticks; t++)
                        sim_.step();
                    stkm2Out_ = sim_.elementCount(PT_STKM2) > 0;
                    double ms =
                        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
                    tickMs_ = tickMs_.load() * 0.9 + 0.1 * ms / ticks;
                } else
                    for (int k = 0; k < idle; k++)
                        sim_.idleFrame();
                sim_.render(view, picture, overlays_.load());
                found_ = sim_.foundParticles();
                {
                    float lo, hi;
                    bool valid = sim_.heatRange(lo, hi);
                    std::lock_guard h(heatMutex_);
                    heatLo_ = lo, heatHi_ = hi, heatValid_ = valid;
                }
                ClassicSim::Sample cs = sim_.sample(cx, cy);
                sample.inWorld = cs.inWorld;
                sample.hasPart = cs.hasPart;
                sample.type = cs.type, sample.ctype = cs.ctype, sample.life = cs.life, sample.tmp = cs.tmp;
                sample.tmp2 = cs.tmp2, sample.tmp4 = cs.tmp4, sample.wall = cs.wall;
                sample.temp = cs.temp;
                sample.pressure = cs.pressure;
                sample.id = cs.id, sample.x = cs.x, sample.y = cs.y;
                sample.gravX = cs.gravX, sample.gravY = cs.gravY, sample.airTemp = cs.airTemp;
            }
            uses.clear();
            q.lock();
            rendered_.swap(picture);
            sample_ = sample;
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
    std::vector<ToolUse> uses_;
    std::vector<uint32_t> rendered_;   // the worker's latest picture
    std::vector<uint32_t> shown_;      // the picture being drawn (drawing thread only)
    std::vector<uint32_t> withCursor_; // and with the cursor on it
    std::atomic<uint32_t> overlays_{0};
    ViewSettings view_ = tptDefaultView(); // (under qm_)
    int idleFrames_ = 0;                   // (under qm_)
    std::atomic<int> found_{0};
    std::mutex heatMutex_; // (the last render's heat display range)
    float heatLo_ = 0.0f, heatHi_ = 0.0f;
    bool heatValid_ = false;
    int cursorX_ = -1, cursorY_ = -1; // (under qm_)
    UiSample sample_;                 // (under qm_)
    std::atomic<uint32_t> keys_{0};
    std::atomic<bool> stkm2Out_{false};
    std::atomic<double> tickMs_{0.0};
};

} // namespace

std::unique_ptr<World> makeClassicWorld(VkContext &ctx, const AppOptions &o) {
    return std::make_unique<ClassicWorld>(ctx, o);
}

} // namespace bp
