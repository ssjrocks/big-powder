// Compiled with TPT's include paths and flags (see CMakeLists.txt); everything TPT-specific stays here.
#include "classic/ClassicSim.h"

#include "client/GameSave.h"
#include "common/platform/Platform.h"
#include "graphics/Renderer.h"
#include "prefs/GlobalPrefs.h"
#include "simulation/Air.h"
#include "simulation/ElementClasses.h"
#include "simulation/Simulation.h"
#include "simulation/SimulationData.h"

#include <algorithm>

namespace bp {

static_assert(ClassicSim::W == XRES && ClassicSim::H == YRES && ClassicSim::CELL == CELL,
              "ClassicSim's size constants must match TPT's SimulationConfig.h");

struct ClassicSim::Impl {
    std::unique_ptr<SimulationData> data; // TPT's element tables (a singleton inside TPT)
    std::unique_ptr<Simulation> sim;
    std::unique_ptr<Renderer> ren;
    int lastPreset = -1;
};

ClassicSim::ClassicSim() : impl_(std::make_unique<Impl>()) {
    // TPT's gravity reads a preference through this singleton; see TptPrefsShim.cpp.
    static GlobalPrefs prefs;
    impl_->data = std::make_unique<SimulationData>();
    impl_->sim = Simulation::Factory();
    impl_->ren = std::make_unique<Renderer>();
    setOptions(opts_);
}

ClassicSim::~ClassicSim() {
    impl_->ren.reset();
    impl_->sim.reset();
    impl_->data.reset();
}

void ClassicSim::setOptions(const ClassicOptions &o) {
    opts_ = o;
    Simulation &sim = *impl_->sim;
    sim.legacy_enable = o.heat ? 0 : 1;
    sim.aheat_enable = o.ambientHeat ? 1 : 0;
    sim.water_equal_test = o.waterEqualisation ? 1 : 0;
    sim.SetEdgeMode(o.edgeMode);
    sim.gravityMode = o.gravityMode;
    sim.air->airMode = o.airMode;
    sim.EnableNewtonianGravity(o.newtonianGravity);
}

void ClassicSim::step() {
    // GameModel::UpdateUpTo(NPART) with the simulation running.
    Simulation &sim = *impl_->sim;
    sim.BeforeSim(true);
    sim.UpdateParticles(0, NPART);
    sim.AfterSim();
    tick_++;
}

void ClassicSim::seed(uint32_t s) { impl_->sim->rng.seed(s); }

void ClassicSim::clear() {
    impl_->sim->clear_sim();
    tick_ = 0;
}

bool ClassicSim::load(const std::vector<char> &data, std::string &error, int bx, int by) {
    try {
        GameSave save(data, false);
        impl_->sim->Load(&save, true, {bx, by});
        return true;
    } catch (const std::exception &e) {
        error = e.what();
        return false;
    }
}

bool ClassicSim::loadFile(const std::string &path, std::string &error) {
    std::vector<char> data;
    if (!Platform::ReadFile(data, ByteString(path))) {
        error = "cannot read " + path;
        return false;
    }
    return load(data, error);
}

bool ClassicSim::save(std::vector<char> &out, std::string &error) const {
    try {
        auto gs = impl_->sim->Save(true, RES.OriginRect());
        // The bool is "faked as from a newer version"; failure is an empty result.
        auto [fakeNewer, bytes] = gs->Serialise();
        (void)fakeNewer;
        if (bytes.empty()) {
            error = "TPT could not serialise the simulation";
            return false;
        }
        out = std::move(bytes);
        return true;
    } catch (const std::exception &e) {
        error = e.what();
        return false;
    }
}

bool ClassicSim::saveFile(const std::string &path, std::string &error) const {
    std::vector<char> bytes;
    if (!save(bytes, error))
        return false;
    if (!Platform::WriteFile(bytes, ByteString(path))) {
        error = "cannot write " + path;
        return false;
    }
    return true;
}

void ClassicSim::brush(int x, int y, int r, int type) {
    Simulation &sim = *impl_->sim;
    for (int dy = -r; dy <= r; dy++)
        for (int dx = -r; dx <= r; dx++)
            if (dx * dx + dy * dy <= r * r) {
                int px = x + dx, py = y + dy;
                if (px >= 0 && py >= 0 && px < XRES && py < YRES)
                    sim.CreatePartFlags(-1, px, py, type, 0);
            }
}

int ClassicSim::createPart(int x, int y, int type) { return impl_->sim->create_part(-1, x, y, type); }

int ClassicSim::presetCount() { return int(Renderer::renderModePresets.size()); }

std::string ClassicSim::presetName(int preset) {
    if (preset < 0 || preset >= presetCount())
        return "";
    return Renderer::renderModePresets[preset].Name.ToUtf8();
}

void ClassicSim::render(int preset, std::vector<uint32_t> &out) {
    Renderer &ren = *impl_->ren;
    preset = std::clamp(preset, 0, presetCount() - 1);
    if (preset != impl_->lastPreset) {
        const auto &p = Renderer::renderModePresets[preset];
        RendererSettings s;
        s.renderMode = p.renderMode;
        s.displayMode = p.displayMode;
        s.colorMode = p.colorMode;
        s.wantHdispLimitMin = p.wantHdispLimitMin;
        s.wantHdispLimitMax = p.wantHdispLimitMax;
        ren.ApplySettings(s);
        impl_->lastPreset = preset;
    }
    // GameView::RenderSimulation.
    ren.sim = impl_->sim.get();
    ren.Clear();
    ren.RenderBackground();
    {
        auto &sd = SimulationData::Ref();
        std::unique_lock lk(sd.elementGraphicsMx);
        ren.RenderSimulation();
    }
    ren.sim = nullptr;

    const auto &video = ren.GetVideo();
    out.resize(size_t(XRES) * YRES);
    for (int y = 0; y < YRES; y++)
        for (int x = 0; x < XRES; x++)
            out[size_t(y) * XRES + x] = video[{x, y}] & 0xFFFFFF;
}

int ClassicSim::particleCount() const {
    const Simulation &sim = *impl_->sim;
    int n = 0;
    for (int i = 0; i < sim.parts.active; i++)
        if (sim.parts[i].type)
            n++;
    return n;
}

int ClassicSim::elementCount(int type) const {
    const Simulation &sim = *impl_->sim;
    int n = 0;
    for (int i = 0; i < sim.parts.active; i++)
        if (sim.parts[i].type == type)
            n++;
    return n;
}

std::vector<ClassicParticle> ClassicSim::particles() const {
    const Simulation &sim = *impl_->sim;
    std::vector<ClassicParticle> out;
    for (int i = 0; i < sim.parts.active; i++) {
        const Particle &p = sim.parts[i];
        if (!p.type)
            continue;
        out.push_back({p.type, p.life, p.ctype, p.x, p.y, p.vx, p.vy, p.temp, p.tmp, p.tmp2, p.tmp3, p.tmp4,
                       uint32_t(p.flags), uint32_t(p.dcolour)});
    }
    return out;
}

float ClassicSim::pressure(int cx, int cy) const { return impl_->sim->pv[cy][cx]; }
float ClassicSim::ambientHeat(int cx, int cy) const { return impl_->sim->hv[cy][cx]; }

std::string ClassicSim::elementName(int type) const {
    const auto &sd = SimulationData::CRef();
    if (type < 0 || type >= PT_NUM)
        return "";
    return sd.elements[type].Name.ToUtf8();
}

void ClassicSim::setAirCoupling(int type, float advection, float airDrag) {
    auto &sd = SimulationData::Ref();
    sd.elements[type].Advection = advection;
    sd.elements[type].AirDrag = airDrag;
}

int ClassicSim::elementByName(const std::string &name) const {
    const auto &sd = SimulationData::CRef();
    for (int t = 1; t < PT_NUM; t++)
        if (sd.elements[t].Enabled && sd.elements[t].Name.ToUtf8() == ByteString(name))
            return t;
    return -1;
}

} // namespace bp
