// The GPU engine as a viewer world (Screen and Huge modes).
#include "sim/Editing.h"
#include "sim/GpuSave.h"
#include "sim/WorldFile.h"
#include "app/World.h"
#include "classic/ClassicSim.h"

#include <algorithm>
#include <cfloat>
#include <fstream>
#include <iterator>

namespace bp {

namespace {

// What a paste left out (elements the GPU engine doesn't run yet)
std::string leftOutText(const ElementTable &table, const PasteResult &r) {
    if (r.leftOut.empty())
        return "";
    std::string out = "; not ported to the GPU engine yet, left out:";
    for (auto [t, n] : r.leftOut)
        out += " " + table.elements[size_t(t)].name + " " + std::to_string(n);
    return out;
}

class GpuWorld : public World {
public:
    GpuWorld(VkContext &ctx, const AppOptions &o, uint32_t W, uint32_t H, const char *mode)
        : ctx_(ctx), o_(o), mode_(mode), elements_(ElementTable::build()) {
        SimConfig cfg;
        cfg.W = W;
        cfg.H = H;
        cfg.colourNX = o.colours.empty() ? 7 : o.colours[0].nx;
        cfg.colourNY = o.colours.empty() ? 7 : o.colours[0].ny;
        cfg.passOrder = uint32_t(o.passOrder);
        cfg.seed = o.seed;
        cfg.shaderDir = o.shaderDir;
        cfg.settings.edgeMode = uint32_t(o.edgeMode);
        cfg.settings.gravityMode = uint32_t(o.gravityMode);
        cfg.settings.ambientHeat = o.ambientHeat;
        cfg.settings.newtonianGravity = o.newtonian;
        sim_ = std::make_unique<GpuSim>(ctx, cfg, elements_);
        editor_ = std::make_unique<GpuEditor>(ctx, *sim_);
    }

    std::string modeName() const override { return mode_; }
    uint32_t width() const override { return sim_->width(); }
    uint32_t height() const override { return sim_->height(); }
    void ensureFrameBuffer(uint32_t w, uint32_t h) override { sim_->ensureFrameBuffer(w, h); }
    const Buffer &frameBuffer() const override { return sim_->frameBuffer(); }

    void record(VkCommandBuffer cmd, const FrameRequest &f) override {
        // Simulation::BeforeSim's sandcolour_interface, which goes round once every 360 of its calls
        sim_->setSandcolourInterface(tptSandColour(sandFrame_));
        sandFrame_ = (sandFrame_ + uint32_t(f.ticks + f.idleFrames)) % 360;
        editor_->apply(cmd, f.uses);
        for (int t = 0; t < f.ticks; t++)
            sim_->recordTick(cmd, t == 0 ? f.queries : VK_NULL_HANDLE, 0);
        RenderParams rp;
        rp.camX = f.camX;
        rp.camY = f.camY;
        rp.zoom = f.zoom;
        rp.screenW = f.screenW;
        rp.screenH = f.screenH;
        rp.view = f.view;
        view_ = f.view;
        rp.brushX = f.brushX;
        rp.brushY = f.brushY;
        rp.cursor = &f.cursor;
        rp.overlays = f.overlays;
        rp.slot = uint32_t(f.slot);
        cursorX_ = f.brushX;
        cursorY_ = f.brushY;
        sim_->recordRender(cmd, rp);
    }

    void apply(const std::vector<ToolUse> &uses) override {
        if (uses.empty())
            return;
        vkDeviceWaitIdle(ctx_.device());
        ctx_.immediate([&](VkCommandBuffer cmd) { editor_->apply(cmd, uses); });
    }
    struct Snapshot : WorldSnapshot {
        std::shared_ptr<GpuSnapshot> gpu;
        const GpuSim *of;
    };
    std::shared_ptr<const WorldSnapshot> snapshot() override {
        auto s = std::make_shared<Snapshot>();
        s->gpu = sim_->snapshot();
        s->of = sim_.get();
        return s;
    }
    void restore(const WorldSnapshot &s) override {
        const Snapshot &g = static_cast<const Snapshot &>(s);
        if (g.of == sim_.get())
            sim_->restore(*g.gpu);
    }

    SaveArea copyArea(int x1, int y1, int x2, int y2, bool includePressure) override {
        return gpuSaveArea(*sim_, x1, y1, x2, y2, includePressure);
    }
    std::string pasteArea(const SaveArea &s, bool includePressure, int bx, int by) override {
        PasteResult r = gpuLoadArea(*sim_, s, includePressure, bx, by);
        std::string left = leftOutText(elements_, r);
        return left.empty() ? "" : left.substr(2);
    }
    void clearArea(int x, int y, int w, int h) override { gpuClearArea(*sim_, x, y, w, h); }
    std::vector<uint32_t> renderSave(const SaveArea &s) override {
        return tptRenderSave(s, view_, 0);
    }

    bool gpuTimed() const override { return true; }
    uint64_t particleCount() override {
        SimStats st = sim_->readStats();
        stkm2Out_ = st.typeCount[PT_STKM2] > 0;
        return st.total;
    }
    void setStickmanKeys(uint8_t comm1, uint8_t pcomm1, uint8_t comm2, uint8_t pcomm2) override {
        sim_->setStickmanKeys(comm1, pcomm1, comm2, pcomm2);
    }
    bool stickman2Out() const override { return stkm2Out_; }

    std::vector<int> elements() const override {
        std::vector<int> out;
        for (int t = 1; t < MAX_ELEMENTS; t++)
            if (elements_.enabled(t))
                out.push_back(t);
        return out;
    }
    std::string elementName(int type) const override {
        return type == 0 ? "erase" : elements_.elements[type].name;
    }

    void loadScene(const std::string &name) override {
        sim_->load(makeScene(elements_, name, width(), height(), o_.seed, o_.fill));
    }

    bool loadSave(const std::string &path, int x, int y, std::string &message) override {
        // Our own world files replace the world; TPT's saves are pasted into it.
        if (path.size() > 4 && path.compare(path.size() - 4, 4, ".bpw") == 0)
            return loadWorld(*sim_, path, message);
        // TPT's own reader reads the save, which is then pasted centred on (x, y) as Simulation::Load would
        std::vector<char> data;
        {
            std::ifstream f(path, std::ios::binary);
            data.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
            if (!f && !f.eof()) {
                message = "could not read " + path;
                return false;
            }
        }
        SaveArea area;
        std::string err;
        if (!tptReadSave(data, area, err)) {
            message = "could not load " + path + ": " + err;
            return false;
        }
        auto floorDiv = [](int a, int b) { return a >= 0 ? a / b : -((-a + b - 1) / b); };
        PasteResult r = gpuLoadArea(*sim_, area, true, floorDiv(x - area.cw * CELL / 2, CELL),
                                    floorDiv(y - area.ch * CELL / 2, CELL));
        message = "placed " + std::to_string(r.placed) + " particles from " + path + leftOutText(elements_, r);
        return true;
    }

    bool save(const std::string &path, std::string &message) override {
        std::string err;
        if (!saveWorld(*sim_, path, err)) {
            message = err;
            return false;
        }
        message = "saved " + path;
        return true;
    }
    std::string saveExtension() const override { return ".bpw"; }
    UiSample cursorSample() override {
        UiSample s;
        const SignSample &g = sim_->cursorSample();
        s.inWorld = cursorX_ >= 0 && cursorY_ >= 0 && cursorX_ < int(sim_->width()) && cursorY_ < int(sim_->height());
        if (!s.inWorld)
            return s;
        s.hasPart = g.hasPart;
        s.type = g.type, s.ctype = g.ctype, s.life = g.life, s.tmp = g.tmp, s.tmp2 = g.tmp2, s.tmp4 = g.tmp4;
        s.temp = g.temp;
        s.pressure = g.pressure;
        s.wall = sim_->walls()[size_t(cursorY_ / CELL) * (sim_->width() / CELL) + size_t(cursorX_ / CELL)];
        s.id = g.id, s.x = cursorX_, s.y = cursorY_;
        s.gravX = g.gravX, s.gravY = g.gravY, s.airTemp = g.aheat + 273.15f;
        return s;
    }
    void resetAir() override { sim_->resetAir(); }
    void resetSpark() override { sim_->resetSpark(); }
    void invertAir() override { sim_->invertAir(); }
    void resetAmbientHeat() override { sim_->resetAmbientHeat(); }
    std::vector<uint32_t> picture() override { return sim_->readWorldImage(); }
    uint32_t foundParticles() override { return sim_->foundParticles(); }
    bool heatRange(float &lo, float &hi) override { return sim_->heatRange(lo, hi); }
    uint32_t picturePixel(int x, int y) override { return sim_->readWorldPixel(x, y); }
    void setCustomGol(const std::vector<CustomGol> &types) override { sim_->setCustomGol(types); }
    std::vector<Sign> signs() override { return sim_->signs(); }
    void setSigns(const std::vector<Sign> &signs) override { sim_->setSigns(signs); }
    std::vector<SignSample> signSamples(bool now) override {
        if (now)
            sim_->sampleSigns();
        return sim_->signSamples();
    }
    bool particleAt(int x, int y, bool photonsFirst, ClassicParticle &p) override {
        int id;
        return sim_->readParticleAt(x, y, photonsFirst, id, p);
    }
    void applySettings(const WorldSettings &s) override {
        SimSettings ss = sim_->settings();
        ss.edgeMode = uint32_t(s.edgeMode);
        ss.gravityMode = uint32_t(s.gravityMode);
        ss.customGravX = s.customGravX, ss.customGravY = s.customGravY;
        ss.ambientHeat = s.ambientHeat;
        ss.heat = s.heat;
        ss.waterEqualisation = s.waterEqualisation;
        ss.convectionMode = uint32_t(s.convectionMode);
        ss.newtonianGravity = s.newtonianGravity;
        sim_->setSettings(ss);
        AirSettings a = sim_->air();
        a.mode = uint32_t(s.airMode);
        a.ambientTemp = s.ambientTemp;
        a.edgePressure = s.edgePressure, a.edgeVX = s.edgeVX, a.edgeVY = s.edgeVY;
        a.vorticity = s.vorticity;
        sim_->setAir(a);
        sim_->setPrettyPowder(s.prettyPowder);
    }

    void cyclePasses() override {
        // Cycle 3x3, 4x4, 5x5, 7x7 (default), 8x8.
        static const uint32_t sizes[] = {3, 4, 5, 7, 8};
        uint32_t cur = sim_->colourNX();
        size_t i = 0;
        while (i < std::size(sizes) && sizes[i] != cur)
            i++;
        uint32_t n = sizes[(i + 1) % std::size(sizes)];
        sim_->setColours(n, n);
    }
    std::string passesName() const override {
        return std::to_string(sim_->colourNX()) + "x" + std::to_string(sim_->colourNY()) + " passes";
    }

private:
    VkContext &ctx_;
    AppOptions o_;
    std::string mode_;
    ElementTable elements_;
    std::unique_ptr<GpuSim> sim_;
    std::unique_ptr<GpuEditor> editor_; // TPT's tools
    int cursorX_ = -1, cursorY_ = -1;
    ViewSettings view_ = tptDefaultView(); // (the last frame's, for the paste preview)
    uint32_t sandFrame_ = 0;               // (Simulation::sandcolour_frame)
    bool stkm2Out_ = false;
};

} // namespace

std::unique_ptr<World> makeGpuWorld(VkContext &ctx, const AppOptions &o, uint32_t W, uint32_t H, const char *mode) {
    return std::make_unique<GpuWorld>(ctx, o, W, H, mode);
}

} // namespace bp
