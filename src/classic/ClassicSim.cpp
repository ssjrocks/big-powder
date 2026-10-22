// Compiled with TPT's include paths and flags (see CMakeLists.txt); everything TPT-specific stays here.
#include "classic/ClassicSim.h"
#include "sim/RenderTables.h"

#include "client/GameSave.h"
#include "common/platform/Platform.h"
#include "Misc.h"
#include "graphics/FontReader.h"
#include "graphics/Renderer.h"
#include "prefs/GlobalPrefs.h"
#include "simulation/Air.h"
#include "simulation/ElementClasses.h"
#include "simulation/Simulation.h"
#include "simulation/SimulationData.h"
#include "simulation/TransitionConstants.h"
#include "simulation/elements/STKM.h"

#include <algorithm>

// FontReader.cpp's tables, filled the first time a FontReader is made
extern unsigned char *font_data;
extern unsigned int *font_ptrs;
extern unsigned int (*font_ranges)[2];

namespace bp {

static_assert(ClassicSim::W == XRES && ClassicSim::H == YRES && ClassicSim::CELL == CELL,
              "ClassicSim's size constants must match TPT's SimulationConfig.h");

static int liveClassicSims = 0; // TPT's SimulationData is a singleton owned by the ClassicSim alive

struct ClassicSim::Impl {
    std::unique_ptr<SimulationData> data; // TPT's element tables (a singleton inside TPT)
    std::unique_ptr<Simulation> sim;
    std::unique_ptr<Renderer> ren;
    int lastPreset = -1;
    uint32_t lastOverlays = 0;
};

ClassicSim::ClassicSim() : impl_(std::make_unique<Impl>()) {
    liveClassicSims++;
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
    liveClassicSims--;
}

std::vector<ClassicElement> ClassicSim::exportElements(std::vector<uint8_t> &canMove, int maxElements) {
    std::unique_ptr<SimulationData> temporary;
    if (liveClassicSims == 0)
        temporary = std::make_unique<SimulationData>();
    const SimulationData &sd = SimulationData::CRef();
    auto transition = [](int t) { return t == ST ? -2 : t; };
    std::vector<ClassicElement> out(PT_NUM);
    for (int t = 0; t < PT_NUM; t++) {
        const Element &e = sd.elements[t];
        ClassicElement &c = out[t];
        c.id = t;
        c.name = e.Name.ToUtf8();
        c.identifier = e.Identifier;
        c.description = e.Description.ToUtf8();
        c.enabled = e.Enabled != 0;
        c.hasUpdate = e.Update != nullptr;
        c.hasGraphics = e.Graphics != nullptr && e.Graphics != &Element::defaultGraphics;
        c.hasCreate = e.Create != nullptr;
        c.hasCreateAllowed = e.CreateAllowed != nullptr;
        c.hasChangeType = e.ChangeType != nullptr;
        c.colour = e.Colour.Pack() & 0xFFFFFF;
        c.menuSection = e.MenuSection;
        c.menuVisible = e.MenuVisible;
        c.advection = e.Advection;
        c.airDrag = e.AirDrag;
        c.airLoss = e.AirLoss;
        c.loss = e.Loss;
        c.collision = e.Collision;
        c.gravity = e.Gravity;
        c.newtonianGravity = e.NewtonianGravity;
        c.diffusion = e.Diffusion;
        c.hotAir = e.HotAir;
        c.falldown = e.Falldown;
        c.flammable = e.Flammable;
        c.explosive = e.Explosive;
        c.meltable = e.Meltable;
        c.hardness = e.Hardness;
        c.photonReflectWavelengths = e.PhotonReflectWavelengths;
        c.weight = e.Weight;
        c.heatConduct = e.HeatConduct;
        c.heatCapacity = e.HeatCapacity;
        c.properties = e.Properties;
        c.carriesTypeIn = e.CarriesTypeIn;
        c.lowPressure = e.LowPressure;
        c.highPressure = e.HighPressure;
        c.lowTemperature = e.LowTemperature;
        c.highTemperature = e.HighTemperature;
        c.lowPressureT = transition(e.LowPressureTransition);
        c.highPressureT = transition(e.HighPressureTransition);
        c.lowTemperatureT = transition(e.LowTemperatureTransition);
        c.highTemperatureT = transition(e.HighTemperatureTransition);
        const Particle &d = e.DefaultProperties;
        c.defaults = {t, d.life, d.ctype, 0, 0, 0, 0, d.temp, d.tmp, d.tmp2, d.tmp3, d.tmp4, uint32_t(d.flags),
                      uint32_t(d.dcolour)};
    }
#define BP_CONST_NAME(name, id) out[id].constName = #name;
    ELEMENT_NUMBERS(BP_CONST_NAME)
#undef BP_CONST_NAME
    canMove.assign(size_t(maxElements) * maxElements, 0);
    for (int m = 0; m < std::min(maxElements, PT_NUM); m++)
        for (int d = 0; d < std::min(maxElements, PT_NUM); d++)
            canMove[size_t(m) * maxElements + d] = uint8_t(sd.can_move[m][d]);
    return out;
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
    // GameController::Tick: a player who isn't out holds DUST again (TPT's default right-hand element).
    if (!sim.player.spwn)
        Element_STKM_set_element(&sim, &sim.player, PT_DUST);
    if (!sim.player2.spwn)
        Element_STKM_set_element(&sim, &sim.player2, PT_DUST);
}

void ClassicSim::setStickmanKeys(int comm1, int pcomm1, int comm2, int pcomm2) {
    Simulation &sim = *impl_->sim;
    sim.player.comm = char(comm1);
    sim.player.pcomm = char(pcomm1);
    sim.player2.comm = char(comm2);
    sim.player2.pcomm = char(pcomm2);
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
        loadedCW_ = save.blockSize.X;
        loadedCH_ = save.blockSize.Y;
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

bool ClassicSim::writeTptSave(const TptArea &a, const std::string &path, std::string &error) {
    if (a.cw <= 0 || a.ch <= 0 || a.cw > XCELLS || a.ch > YCELLS) {
        error = "a TPT save is at most " + std::to_string(XRES) + "x" + std::to_string(YRES);
        return false;
    }
    std::unique_ptr<SimulationData> temporary;
    if (liveClassicSims == 0)
        temporary = std::make_unique<SimulationData>();
    try {
        const auto &elements = SimulationData::CRef().elements;
        auto gs = std::make_unique<GameSave>(Vec2<int>{a.cw, a.ch});
        std::map<int, int> newId; // (Simulation::Save's particleMap, for SOAP)
        int stored = 0, soap = 0;
        for (size_t k = 0; k < a.particles.size(); k++) {
            const ClassicParticle &c = a.particles[k];
            if (c.type <= 0 || c.type >= PT_NUM || !elements[c.type].Enabled)
                continue;
            Particle p{};
            p.type = c.type, p.life = c.life, p.ctype = c.ctype, p.x = c.x, p.y = c.y, p.vx = c.vx, p.vy = c.vy;
            p.temp = c.temp, p.tmp = c.tmp, p.tmp2 = c.tmp2, p.tmp3 = c.tmp3, p.tmp4 = c.tmp4;
            p.flags = c.flags, p.dcolour = c.dcolour;
            if (k < a.ids.size())
                newId[a.ids[k]] = stored;
            *gs << p;
            stored++;
            soap += c.type == PT_SOAP;
        }
        if (soap)
            for (int i = 0; i < gs->particlesCount; i++) {
                Particle &p = gs->particles[i];
                if (p.type != PT_SOAP)
                    continue;
                if ((p.ctype & 2) == 2) {
                    auto n = newId.find(p.tmp);
                    if (n != newId.end())
                        p.tmp = n->second;
                    else
                        p.tmp = 0, p.ctype ^= 2;
                }
                if ((p.ctype & 4) == 4) {
                    auto n = newId.find(p.tmp2);
                    if (n != newId.end())
                        p.tmp2 = n->second;
                    else
                        p.tmp2 = 0, p.ctype ^= 4;
                }
            }
        for (const Sign &s : a.signs) {
            sign t(String(s.text.data(), s.text.size()), s.x, s.y, sign::Justification(s.ju));
            *gs << t;
        }
        const size_t cells = size_t(a.cw) * a.ch;
        auto at = [&](const std::vector<float> &v, int x, int y) {
            return v.size() == cells ? v[size_t(y) * a.cw + x] : 0.0f;
        };
        for (int y = 0; y < a.ch; y++)
            for (int x = 0; x < a.cw; x++) {
                Vec2<int> b{x, y};
                uint8_t wall = a.walls.size() == cells ? a.walls[size_t(y) * a.cw + x] : 0;
                if (wall) {
                    gs->blockMap[b] = wall;
                    gs->fanVelX[b] = at(a.fanVX, x, y);
                    gs->fanVelY[b] = at(a.fanVY, x, y);
                }
                gs->pressure[b] = at(a.pv, x, y);
                gs->velocityX[b] = at(a.vx, x, y);
                gs->velocityY[b] = at(a.vy, x, y);
                gs->ambientHeat[b] = at(a.hv, x, y);
            }
        gs->hasPressure = true;
        gs->hasAmbientHeat = true;
        // SaveSimOptions
        gs->gravityMode = a.options.gravityMode;
        gs->customGravityX = a.customGravX;
        gs->customGravityY = a.customGravY;
        gs->airMode = a.options.airMode;
        gs->ambientAirTemp = a.ambientTemp;
        gs->edgePressure = a.edgePressure;
        gs->edgeVelocityX = a.edgeVX;
        gs->edgeVelocityY = a.edgeVY;
        gs->vorticityCoeff = a.vorticity;
        gs->convectionMode = a.convectionMode;
        gs->edgeMode = a.options.edgeMode;
        gs->legacyEnable = !a.options.heat;
        gs->waterEEnabled = a.options.waterEqualisation;
        gs->gravityEnable = a.options.newtonianGravity;
        gs->aheatEnable = a.options.ambientHeat;
        gs->pmapbits = PMAPBITS;
        auto [fakeNewer, bytes] = gs->Serialise();
        (void)fakeNewer;
        if (bytes.empty()) {
            error = "TPT could not serialise the save";
            return false;
        }
        if (!Platform::WriteFile(bytes, ByteString(path))) {
            error = "cannot write " + path;
            return false;
        }
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

int ClassicSim::createPartStacked(int x, int y, int type) { return impl_->sim->create_part(-3, x, y, type); }

void ClassicSim::setParticleTemp(int id, float temp) {
    if (id >= 0 && id < NPART && impl_->sim->parts[id].type)
        impl_->sim->parts[id].temp = temp;
}

void ClassicSim::setParticleFields(int id, int ctype, int tmp, int tmp2, int tmp3, int tmp4) {
    if (id < 0 || id >= NPART || !impl_->sim->parts[id].type)
        return;
    Particle &p = impl_->sim->parts[id];
    p.ctype = ctype;
    p.tmp = tmp;
    p.tmp2 = tmp2;
    p.tmp3 = tmp3;
    p.tmp4 = tmp4;
}

uint32_t ClassicSim::fontGlyph(char32_t ch, int *width) {
    FontReader init(U'A');
    (void)init;
    for (;;) {
        size_t offset = 0;
        for (int i = 0; font_ranges[i][1]; i++) {
            if (font_ranges[i][0] > uint32_t(ch))
                break;
            if (font_ranges[i][1] >= uint32_t(ch)) {
                uint32_t at = font_ptrs[offset + (ch - font_ranges[i][0])];
                if (width)
                    *width = font_data[at];
                return at;
            }
            offset += font_ranges[i][1] - font_ranges[i][0] + 1;
        }
        if (ch == 0xFFFD) {
            if (width)
                *width = font_data[0];
            return 0;
        }
        ch = 0xFFFD;
    }
}

void ClassicSim::exportFont(std::vector<std::array<uint32_t, 2>> &ranges, std::vector<uint32_t> &glyphs) {
    FontReader init(U'A');
    (void)init;
    ranges.clear();
    glyphs.clear();
    size_t glyph = 0;
    for (int i = 0; font_ranges[i][1]; i++) {
        ranges.push_back({font_ranges[i][0], font_ranges[i][1]});
        for (uint32_t ch = font_ranges[i][0]; ch <= font_ranges[i][1]; ch++)
            glyphs.push_back(font_ptrs[glyph++]);
    }
}

std::vector<std::string> ClassicSim::golNames() {
    std::vector<std::string> out;
    for (const auto &g : SimulationData::builtinGol)
        out.push_back(g.name.ToUtf8());
    return out;
}

std::vector<Sign> ClassicSim::signs() const {
    std::vector<Sign> out;
    for (const sign &s : impl_->sim->signs)
        out.push_back(Sign{s.x, s.y, int(s.ju), std::u32string(s.text.begin(), s.text.end())});
    return out;
}

void ClassicSim::setSigns(const std::vector<Sign> &signs) {
    auto &out = impl_->sim->signs;
    out.clear();
    for (const Sign &s : signs)
        if (out.size() < MAXSIGNS && !s.text.empty())
            out.emplace_back(String(s.text.data(), s.text.size()), s.x, s.y, sign::Justification(s.ju));
}

std::vector<uint32_t> ClassicSim::renderTables() {
    std::unique_ptr<SimulationData> temporary;
    if (liveClassicSims == 0)
        temporary = std::make_unique<SimulationData>();
    Renderer::PopulateTables();
    std::vector<uint32_t> t(RT_SIZE, 0);
    auto put = [&](uint32_t at, const std::vector<RGB> &table) {
        for (size_t i = 0; i < table.size(); i++)
            t[at + i] = table[i].Pack() & 0xFFFFFF;
    };
    put(RT_FLAME, Renderer::flameTable);
    put(RT_PLASMA, Renderer::plasmaTable);
    put(RT_HEAT, Renderer::heatTable);
    put(RT_CLFM, Renderer::clfmTable);
    put(RT_FIRW, Renderer::firwTable);
    // Renderer::prepare_alpha(CELL, 1.0f)
    {
        float multiplier = 255.0f * 1.0f;
        float temp[CELL * 3][CELL * 3] = {};
        for (int x = 0; x < CELL; x++)
            for (int y = 0; y < CELL; y++)
                for (int i = -CELL; i < CELL; i++)
                    for (int j = -CELL; j < CELL; j++)
                        temp[y + CELL + j][x + CELL + i] += expf(-0.1f * (i * i + j * j));
        for (int x = 0; x < CELL * 3; x++)
            for (int y = 0; y < CELL * 3; y++)
                t[RT_FIRE_ALPHA + y * CELL * 3 + x] = uint32_t((int)(multiplier * temp[y][x] / (CELL * CELL)));
    }
    for (size_t i = 0; i < SimulationData::builtinGol.size() && i < 32; i++) {
        const auto &g = SimulationData::builtinGol[i];
        t[RT_GOL_COLOUR + i] = g.colour.Pack() & 0xFFFFFF;
        t[RT_GOL_COLOUR2 + i] = g.colour2.Pack() & 0xFFFFFF;
        t[RT_GOL_RULESET + i] = uint32_t(g.ruleset);
    }
    const auto &wtypes = SimulationData::CRef().wtypes;
    for (size_t i = 0; i < wtypes.size() && i < 32; i++) {
        t[RT_WALL_COLOUR + i] = wtypes[i].colour.Pack() & 0xFFFFFF;
        t[RT_WALL_EGLOW + i] = wtypes[i].eglow.Pack() & 0xFFFFFF;
        t[RT_WALL_STYLE + i] = uint32_t(wtypes[i].drawstyle);
    }
    // TRON.cpp's MakeTronColors (r, g and b carry over when HSV_to_RGB leaves them alone)
    {
        int r = 0, g = 0, b = 0;
        for (int i = 0; i < 32; i++) {
            HSV_to_RGB(i << 4, 255, 255, &r, &g, &b);
            t[RT_TRON + i] = uint32_t(r << 16 | g << 8 | b);
        }
    }
    const uint32_t slcn[16] = {0x5A6679, 0x6878A1, 0xABBFDD, 0x838490, 0xBCCDDF, 0x82A0D2, 0x5B6680, 0x232C3B,
                               0x485067, 0x8B9AB6, 0xADB1C1, 0xC3C6D1, 0x8594AD, 0x262F47, 0xA9AEBC, 0xC2E1F7};
    for (int i = 0; i < 16; i++)
        t[RT_SLCN + i] = slcn[i]; // SLCN.cpp's SLCN_COLOUR
    const uint32_t leaf[8] = {0xF3F6F4, 0xFFDF32, 0xFFB7C5, 0xFA0019, 0x80CEC4, 0x7FFF00, 0x004AB2, 0x0CAC00};
    for (int i = 0; i < 8; i++)
        t[RT_LEAF + i] = leaf[i]; // PLNT.cpp's leafColor
    // The font: all its glyphs (a width byte and width * FONT_H 2-bit pixels each), packed four bytes to a word
    t[RT_GLYPH_STREAM] = fontGlyph(0xE00D);
    size_t bytes = 0, glyph = 0;
    for (int i = 0; font_ranges[i][1]; i++)
        for (uint32_t ch = font_ranges[i][0]; ch <= font_ranges[i][1]; ch++, glyph++) {
            uint32_t at = font_ptrs[glyph];
            bytes = std::max(bytes, size_t(at) + 1 + size_t(font_data[at]) * FONT_H / 4);
        }
    t.resize(RT_FONT + (bytes + 3) / 4, 0);
    for (size_t b = 0; b < bytes; b++)
        t[RT_FONT + b / 4] |= uint32_t(font_data[b]) << (8 * (b % 4));
    return t;
}

int ClassicSim::particleTmp(int id) const {
    return id >= 0 && id < NPART ? impl_->sim->parts[id].tmp : 0;
}

void ClassicSim::presetModes(int preset, uint32_t &renderMode, uint32_t &displayMode, uint32_t &colorMode,
                             bool &autoHeat) {
    const auto &p = Renderer::renderModePresets[std::clamp(preset, 0, presetCount() - 1)];
    renderMode = p.renderMode;
    displayMode = p.displayMode;
    colorMode = p.colorMode;
    autoHeat = std::holds_alternative<HdispLimitAuto>(p.wantHdispLimitMin);
}

int ClassicSim::presetCount() { return int(Renderer::renderModePresets.size()); }

std::string ClassicSim::presetName(int preset) {
    if (preset < 0 || preset >= presetCount())
        return "";
    return Renderer::renderModePresets[preset].Name.ToUtf8();
}

void ClassicSim::render(int preset, std::vector<uint32_t> &out, uint32_t overlays) {
    Renderer &ren = *impl_->ren;
    preset = std::clamp(preset, 0, presetCount() - 1);
    if (preset != impl_->lastPreset || overlays != impl_->lastOverlays) {
        const auto &p = Renderer::renderModePresets[preset];
        RendererSettings s;
        s.renderMode = p.renderMode;
        s.displayMode = p.displayMode;
        s.colorMode = p.colorMode;
        s.wantHdispLimitMin = p.wantHdispLimitMin;
        s.wantHdispLimitMax = p.wantHdispLimitMax;
        s.gravityFieldEnabled = (overlays & 1) != 0;
        s.gravityZonesEnabled = (overlays & 2) != 0;
        ren.ApplySettings(s);
        impl_->lastPreset = preset;
        impl_->lastOverlays = overlays;
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

void ClassicSim::clearRenderAccumulation() { impl_->ren->ClearAccumulation(); }

void ClassicSim::gravityField(std::vector<float> &fx, std::vector<float> &fy) const {
    const Simulation &sim = *impl_->sim;
    fx.resize(size_t(XCELLS) * YCELLS);
    fy.resize(fx.size());
    for (int y = 0; y < YCELLS; y++)
        for (int x = 0; x < XCELLS; x++) {
            fx[size_t(y) * XCELLS + x] = sim.gravOut.forceX[Vec2<int>{x, y}];
            fy[size_t(y) * XCELLS + x] = sim.gravOut.forceY[Vec2<int>{x, y}];
        }
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

std::vector<ClassicParticle> ClassicSim::particles(std::vector<int> *ids) const {
    const Simulation &sim = *impl_->sim;
    std::vector<ClassicParticle> out;
    if (ids)
        ids->clear();
    for (int i = 0; i < sim.parts.active; i++) {
        const Particle &p = sim.parts[i];
        if (!p.type)
            continue;
        if (ids)
            ids->push_back(i);
        out.push_back({p.type, p.life, p.ctype, p.x, p.y, p.vx, p.vy, p.temp, p.tmp, p.tmp2, p.tmp3, p.tmp4,
                       uint32_t(p.flags), uint32_t(p.dcolour)});
    }
    return out;
}

float ClassicSim::pressure(int cx, int cy) const { return impl_->sim->pv[cy][cx]; }

void ClassicSim::airMaps(std::vector<float> &pv, std::vector<float> &vx, std::vector<float> &vy,
                         std::vector<float> &hv) const {
    const Simulation &sim = *impl_->sim;
    size_t n = size_t(XCELLS) * YCELLS;
    pv.resize(n);
    vx.resize(n);
    vy.resize(n);
    hv.resize(n);
    for (int y = 0; y < YCELLS; y++)
        for (int x = 0; x < XCELLS; x++) {
            size_t i = size_t(y) * XCELLS + x;
            pv[i] = sim.pv[y][x];
            vx[i] = sim.vx[y][x];
            vy[i] = sim.vy[y][x];
            hv[i] = sim.hv[y][x];
        }
}

void ClassicSim::setAirMaps(const std::vector<float> &pv, const std::vector<float> &vx, const std::vector<float> &vy,
                            const std::vector<float> &hv) {
    Simulation &sim = *impl_->sim;
    for (int y = 0; y < YCELLS; y++)
        for (int x = 0; x < XCELLS; x++) {
            size_t i = size_t(y) * XCELLS + x;
            sim.pv[y][x] = pv[i];
            sim.vx[y][x] = vx[i];
            sim.vy[y][x] = vy[i];
            sim.hv[y][x] = hv[i];
        }
}
float ClassicSim::ambientHeat(int cx, int cy) const { return impl_->sim->hv[cy][cx]; }

std::string ClassicSim::elementName(int type) const {
    const auto &sd = SimulationData::CRef();
    if (type < 0 || type >= PT_NUM)
        return "";
    return sd.elements[type].Name.ToUtf8();
}

std::vector<uint8_t> ClassicSim::walls() const {
    std::vector<uint8_t> out(size_t(XCELLS) * YCELLS);
    for (int y = 0; y < YCELLS; y++)
        for (int x = 0; x < XCELLS; x++)
            out[size_t(y) * XCELLS + x] = impl_->sim->bmap[y][x];
    return out;
}

void ClassicSim::setWalls(const std::vector<uint8_t> &bmap) {
    Simulation &sim = *impl_->sim;
    for (int y = 0; y < YCELLS; y++)
        for (int x = 0; x < XCELLS; x++)
            sim.bmap[y][x] = bmap[size_t(y) * XCELLS + x];
    sim.SetEdgeMode(sim.edgeMode); // keeps the border ring consistent with the edge mode, as TPT does
    sim.air->ApproximateBlockAirMaps(CELLS.OriginRect());
    sim.gravWallChanged = true; // (GRAV walls bound Newtonian gravity)
}

void ClassicSim::paintWalls(int cx, int cy, int r, uint8_t wall) {
    Simulation &sim = *impl_->sim;
    for (int y = std::max(cy - r, 1); y <= std::min(cy + r, YCELLS - 2); y++)
        for (int x = std::max(cx - r, 1); x <= std::min(cx + r, XCELLS - 2); x++)
            if ((x - cx) * (x - cx) + (y - cy) * (y - cy) <= r * r) {
                sim.bmap[y][x] = wall;
                if (wall == WL_FAN) {
                    sim.fvx[y][x] = 0.0f;
                    sim.fvy[y][x] = 0.0f;
                }
            }
    sim.gravWallChanged = true;
}

std::vector<float> ClassicSim::fanVX() const {
    std::vector<float> out(size_t(XCELLS) * YCELLS);
    for (int y = 0; y < YCELLS; y++)
        for (int x = 0; x < XCELLS; x++)
            out[size_t(y) * XCELLS + x] = impl_->sim->fvx[y][x];
    return out;
}

std::vector<float> ClassicSim::fanVY() const {
    std::vector<float> out(size_t(XCELLS) * YCELLS);
    for (int y = 0; y < YCELLS; y++)
        for (int x = 0; x < XCELLS; x++)
            out[size_t(y) * XCELLS + x] = impl_->sim->fvy[y][x];
    return out;
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
