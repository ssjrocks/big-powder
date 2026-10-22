// The GPU engine as a viewer world (Screen and Huge modes).
#include "sim/WorldFile.h"
#include "app/World.h"
#include "classic/ClassicSim.h"

#include <algorithm>
#include <cfloat>

namespace bp {

namespace {

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
    }

    std::string modeName() const override { return mode_; }
    uint32_t width() const override { return sim_->width(); }
    uint32_t height() const override { return sim_->height(); }
    void ensureFrameBuffer(uint32_t w, uint32_t h) override { sim_->ensureFrameBuffer(w, h); }
    const Buffer &frameBuffer() const override { return sim_->frameBuffer(); }

    void record(VkCommandBuffer cmd, const FrameRequest &f) override {
        for (const WallOp &w : f.walls)
            sim_->recordPaintWalls(cmd, w.cx, w.cy, w.r, w.wall);
        sim_->recordBrush(cmd, f.brush);
        for (int t = 0; t < f.ticks; t++)
            sim_->recordTick(cmd, t == 0 ? f.queries : VK_NULL_HANDLE, 0);
        RenderParams rp;
        rp.camX = f.camX;
        rp.camY = f.camY;
        rp.zoom = f.zoom;
        rp.screenW = f.screenW;
        rp.screenH = f.screenH;
        rp.preset = preset_;
        rp.brushX = f.brushX;
        rp.brushY = f.brushY;
        rp.brushR = f.brushR;
        rp.overlays = f.overlays;
        rp.slot = uint32_t(f.slot);
        sim_->recordRender(cmd, rp);
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
        // TPT's own loader reads the save; its particles are then placed in this world as a stamp.
        ClassicSim reader;
        std::string err;
        if (!reader.loadFile(path, err)) {
            message = "could not load " + path + ": " + err;
            return false;
        }
        std::vector<int> psIds;
        std::vector<ClassicParticle> ps = reader.particles(&psIds);
        if (ps.empty()) {
            message = path + " has no particles";
            return true;
        }
        float minX = FLT_MAX, minY = FLT_MAX, maxX = -FLT_MAX, maxY = -FLT_MAX;
        for (const auto &p : ps) {
            minX = std::min(minX, p.x);
            minY = std::min(minY, p.y);
            maxX = std::max(maxX, p.x);
            maxY = std::max(maxY, p.y);
        }
        int offX = x - int((minX + maxX) * 0.5f);
        int offY = y - int((minY + maxY) * 0.5f);
        std::vector<ClassicParticle> supported;
        std::vector<int> supportedIds;
        std::vector<int> skipped(1024, 0);
        for (size_t i = 0; i < ps.size(); i++) {
            const auto &p = ps[i];
            if (elements_.enabled(p.type)) {
                supported.push_back(p);
                supportedIds.push_back(psIds[i]);
            } else if (p.type >= 0 && p.type < 1024)
                skipped[p.type]++;
        }
        // Walls go to whole cells, so the stamp is placed on the cell grid, like TPT's stamps.
        offX = offX / CELL * CELL;
        offY = offY / CELL * CELL;
        sim_->stampWalls(reader.walls(), ClassicSim::W / CELL, ClassicSim::H / CELL, offX / CELL, offY / CELL);
        sim_->insertParticles(supported, offX, offY, &supportedIds);
        sim_->addSigns(reader.signs(), offX, offY); // (Simulation::Load moves them with the save)
        // The save's air (pressure, velocity, ambient heat) comes with it, over the save's own area
        {
            std::vector<float> pv, vx, vy, hv;
            reader.airMaps(pv, vx, vy, hv);
            sim_->stampAir(pv, vx, vy, hv, ClassicSim::W / CELL, 0, 0, reader.loadedCW(), reader.loadedCH(),
                           offX / CELL, offY / CELL);
        }
        message = "placed " + std::to_string(supported.size()) + " particles from " + path;
        std::string missing;
        for (int t = 0; t < 1024; t++)
            if (skipped[t])
                missing += " " + reader.elementName(t) + " " + std::to_string(skipped[t]);
        if (!missing.empty())
            message += "; not ported to the GPU engine yet, left out:" + missing;
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
    bool exportTpt(const std::string &path, int x, int y, std::string &message) override {
        const int w = std::min(int(sim_->width()), ClassicSim::W), h = std::min(int(sim_->height()), ClassicSim::H);
        int x0 = std::clamp(x - w / 2, 0, int(sim_->width()) - w), y0 = std::clamp(y - h / 2, 0, int(sim_->height()) - h);
        x0 -= x0 % CELL;
        y0 -= y0 % CELL;
        return exportTptArea(*sim_, x0, y0, w, h, path, message);
    }

    void applySettings(const WorldSettings &s) override {
        SimSettings ss = sim_->settings();
        ss.edgeMode = uint32_t(s.edgeMode);
        ss.gravityMode = uint32_t(s.gravityMode);
        ss.ambientHeat = s.ambientHeat;
        ss.newtonianGravity = s.newtonianGravity;
        sim_->setSettings(ss);
        AirSettings a = sim_->air();
        a.mode = uint32_t(s.airMode);
        sim_->setAir(a);
    }

    // TPT's display presets, as in Classic mode (Fire Display first, as TPT starts)
    void cycleDisplay() override { preset_ = (preset_ + 1) % ClassicSim::presetCount(); }
    std::string displayName() const override { return ClassicSim::presetName(preset_); }

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
    int preset_ = 4;
    bool stkm2Out_ = false;
};

} // namespace

std::unique_ptr<World> makeGpuWorld(VkContext &ctx, const AppOptions &o, uint32_t W, uint32_t H, const char *mode) {
    return std::make_unique<GpuWorld>(ctx, o, W, H, mode);
}

} // namespace bp
