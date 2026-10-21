// Classic mode: TPT's own engine and renderer, shown through the viewer's camera.
#include "app/World.h"
#include "classic/ClassicSim.h"
#include "gpu/ImageBlit.h"
#include "sim/Scenes.h"

#include <chrono>

namespace bp {

namespace {

class ClassicWorld : public World {
public:
    ClassicWorld(VkContext &ctx, const AppOptions &o)
        : o_(o), blit_(ctx, ClassicSim::W, ClassicSim::H, o.shaderDir) {
        for (int p = 0; p < ClassicSim::presetCount(); p++)
            if (ClassicSim::presetName(p) == "Fire Display")
                preset_ = p; // TPT's default
        for (int t = 1; t < 1024; t++) {
            std::string n = sim_.elementName(t);
            if (!n.empty() && sim_.elementByName(n) == t)
                elements_.push_back(t);
        }
    }

    std::string modeName() const override { return "classic"; }
    uint32_t width() const override { return ClassicSim::W; }
    uint32_t height() const override { return ClassicSim::H; }
    void ensureFrameBuffer(uint32_t w, uint32_t h) override { blit_.ensureFrameBuffer(w, h); }
    const Buffer &frameBuffer() const override { return blit_.frameBuffer(); }

    void record(VkCommandBuffer cmd, const FrameRequest &f) override {
        for (const WallOp &w : f.walls)
            sim_.paintWalls(w.cx, w.cy, w.r, w.wall);
        for (const BrushOp &b : f.brush)
            sim_.brush(b.x, b.y, b.r, b.type);
        if (f.ticks > 0) {
            auto t0 = std::chrono::steady_clock::now();
            for (int t = 0; t < f.ticks; t++)
                sim_.step();
            double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
            tickMs_ = tickMs_ * 0.9 + 0.1 * ms / f.ticks;
        }
        sim_.render(preset_, pixels_);
        BlitParams bp;
        bp.camX = f.camX;
        bp.camY = f.camY;
        bp.zoom = f.zoom;
        bp.screenW = f.screenW;
        bp.screenH = f.screenH;
        bp.brushX = f.brushX;
        bp.brushY = f.brushY;
        bp.brushR = f.brushR;
        blit_.record(cmd, f.slot, pixels_, bp);
    }

    bool gpuTimed() const override { return false; }
    double cpuTickMs() const override { return tickMs_; }
    uint64_t particleCount() override { return uint64_t(sim_.particleCount()); }

    std::vector<int> elements() const override { return elements_; }
    std::string elementName(int type) const override { return type == 0 ? "erase" : sim_.elementName(type); }

    void loadScene(const std::string &name) override {
        // Built with TPT's create_part, so every element gets TPT's own defaults.
        ElementTable table = ElementTable::build();
        SceneData sd = makeScene(table, name, ClassicSim::W, ClassicSim::H, o_.seed, o_.fill);
        sim_.clear();
        for (size_t i = 0; i < sd.ptype.size(); i++)
            sim_.createPart(int(sd.parts[i].x), int(sd.parts[i].y), int(sd.ptype[i]));
    }

    bool loadSave(const std::string &path, int, int, std::string &message) override {
        std::string err;
        sim_.clear();
        if (!sim_.loadFile(path, err)) {
            message = "could not load " + path + ": " + err;
            return false;
        }
        message = "loaded " + path + " (" + std::to_string(sim_.particleCount()) + " particles)";
        return true;
    }

    bool save(const std::string &path, std::string &message) override {
        std::string err;
        if (!sim_.saveFile(path, err)) {
            message = "could not save: " + err;
            return false;
        }
        message = "saved " + path;
        return true;
    }
    std::string saveExtension() const override { return ".cps"; }

    void applySettings(const WorldSettings &s) override {
        ClassicOptions co = sim_.options();
        co.edgeMode = s.edgeMode;
        co.gravityMode = s.gravityMode;
        co.airMode = s.airMode;
        co.ambientHeat = s.ambientHeat;
        sim_.setOptions(co);
    }

    void cycleDisplay() override { preset_ = (preset_ + 1) % ClassicSim::presetCount(); }
    std::string displayName() const override { return ClassicSim::presetName(preset_); }

private:
    AppOptions o_;
    ClassicSim sim_;
    ImageBlit blit_;
    std::vector<uint32_t> pixels_;
    std::vector<int> elements_;
    int preset_ = 4;
    double tickMs_ = 0;
};

} // namespace

std::unique_ptr<World> makeClassicWorld(VkContext &ctx, const AppOptions &o) {
    return std::make_unique<ClassicWorld>(ctx, o);
}

} // namespace bp
