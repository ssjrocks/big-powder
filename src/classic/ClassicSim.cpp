// Compiled with TPT's include paths and flags (see CMakeLists.txt); everything TPT-specific stays here.
#include "classic/ClassicSim.h"
#include "classic/TptSaveImpl.h"
#include "classic/TptToolsImpl.h"
#include "sim/RenderTables.h"

#include "client/GameSave.h"
#include "common/platform/Platform.h"
#include "Misc.h"
#include "graphics/FontReader.h"
#include "graphics/Renderer.h"
#include "gui/game/tool/ElementTool.h"
#include "gui/game/tool/WallTool.h"
#include "prefs/GlobalPrefs.h"
#include "simulation/Air.h"
#include "simulation/ElementClasses.h"
#include "simulation/Sample.h"
#include "simulation/Simulation.h"
#include "simulation/SimulationData.h"
#include "simulation/Snapshot.h"
#include "simulation/TransitionConstants.h"
#include "simulation/elements/STKM.h"
#include "simulation/AccessProperty.h"
#include "simulation/GOLString.h"
#include "simulation/SimTool.h"
#include "simulation/ToolClasses.h"
#include "Format.h"

#include <algorithm>
#include <bit>
#include <optional>

// FontReader.cpp's tables, filled the first time a FontReader is made
extern unsigned char *font_data;
extern unsigned int *font_ptrs;
extern unsigned int (*font_ranges)[2];

namespace bp {

static_assert(ClassicSim::W == XRES && ClassicSim::H == YRES && ClassicSim::CELL == CELL,
              "ClassicSim's size constants must match TPT's SimulationConfig.h");

int liveClassicSims = 0;

struct ClassicSim::Impl {
    std::unique_ptr<SimulationData> data; // TPT's element tables (a singleton inside TPT)
    std::unique_ptr<Simulation> sim;
    std::unique_ptr<Renderer> ren;
    int lastPreset = -1;
    uint32_t lastOverlays = 0;
    int lastDeco = 1;
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

struct ClassicSim::State {
    std::unique_ptr<Snapshot> snap;
    uint64_t tick;
};

std::shared_ptr<const ClassicSim::State> ClassicSim::snapshot() const {
    auto s = std::make_shared<State>();
    s->snap = impl_->sim->CreateSnapshot();
    s->tick = tick_;
    return s;
}

void ClassicSim::restore(const State &s) {
    impl_->sim->Restore(*s.snap);
    tick_ = s.tick;
}

SaveArea ClassicSim::saveArea(int x1, int y1, int x2, int y2, bool includePressure) const {
    // GameController's SaneSaveRect
    Vec2<int> point1{x1, y1}, point2{x2, y2};
    point1 = point1.Clamp(RES.OriginRect());
    point2 = point2.Clamp(RES.OriginRect());
    auto tlx = std::min(point1.X, point2.X);
    auto tly = std::min(point1.Y, point2.Y);
    auto brx = std::max(point1.X, point2.X);
    auto bry = std::max(point1.Y, point2.Y);
    auto save = impl_->sim->Save(includePressure, RectBetween(Vec2{tlx, tly}, Vec2{brx, bry}));
    SaveArea out;
    if (save)
        toSaveArea(*save, out);
    return out;
}

void ClassicSim::loadArea(const SaveArea &s, bool includePressure, int bx, int by) {
    auto save = toGameSave(s);
    impl_->sim->Load(save.get(), includePressure, {bx, by});
}

void ClassicSim::clearArea(int x, int y, int w, int h) { impl_->sim->clear_area(x, y, w, h); }

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

namespace {

AccessProperty accessProperty(const ParticleProperty &p) {
    AccessProperty a;
    a.propertyIndex = p.field;
    switch (p.kind) {
    case ParticleProperty::Kind::Int: a.propertyValue = int(p.bits); break;
    case ParticleProperty::Kind::UInt: a.propertyValue = (unsigned int)p.bits; break;
    case ParticleProperty::Kind::Float: a.propertyValue = std::bit_cast<float>(p.bits); break;
    }
    return a;
}

// gui/game/tool/PropertyTool's drawing (its window is ours, ui/Dialogs)
class PropertyDrawTool : public Tool {
public:
    explicit PropertyDrawTool(const AccessProperty &a) : change(a) {}
    AccessProperty change;

    void SetProperty(Simulation *sim, ui::Point position)
    {
        if(position.X<0 || position.X>XRES || position.Y<0 || position.Y>YRES)
            return;
        int i = sim->pmap[position.Y][position.X];
        if(!i)
            i = sim->photons[position.Y][position.X];
        if(!i)
            return;
        change.Set(sim, ID(i));
    }

    void Click(Simulation *, Brush const &, ui::Point) override { }

    void Draw(Simulation *sim, Brush const &cBrush, ui::Point position) override
    {
        for (ui::Point off : cBrush)
        {
            ui::Point coords = position + off;
            if (coords.X >= 0 && coords.Y >= 0 && coords.X < XRES && coords.Y < YRES)
                SetProperty(sim, coords);
        }
    }

    void DrawLine(Simulation *sim, Brush const &cBrush, ui::Point position, ui::Point position2, bool dragging) override
    {
        int x1 = position.X, y1 = position.Y, x2 = position2.X, y2 = position2.Y;
        bool reverseXY = abs(y2-y1) > abs(x2-x1);
        int x, y, dx, dy, sy, rx = cBrush.GetRadius().X, ry = cBrush.GetRadius().Y;
        float e = 0.0f, de;
        if (reverseXY)
        {
            y = x1;
            x1 = y1;
            y1 = y;
            y = x2;
            x2 = y2;
            y2 = y;
        }
        if (x1 > x2)
        {
            y = x1;
            x1 = x2;
            x2 = y;
            y = y1;
            y1 = y2;
            y2 = y;
        }
        dx = x2 - x1;
        dy = abs(y2 - y1);
        if (dx)
            de = dy/(float)dx;
        else
            de = 0.0f;
        y = y1;
        sy = (y1<y2) ? 1 : -1;
        for (x=x1; x<=x2; x++)
        {
            if (reverseXY)
                Draw(sim, cBrush, ui::Point(y, x));
            else
                Draw(sim, cBrush, ui::Point(x, y));
            e += de;
            if (e >= 0.5f)
            {
                y += sy;
                if (!(rx+ry) && ((y1<y2) ? (y<=y2) : (y>=y2)))
                {
                    if (reverseXY)
                        Draw(sim, cBrush, ui::Point(y, x));
                    else
                        Draw(sim, cBrush, ui::Point(x, y));
                }
                e -= 1.0f;
            }
        }
    }

    void DrawRect(Simulation *sim, Brush const &, ui::Point position, ui::Point position2) override
    {
        int x1 = position.X, y1 = position.Y, x2 = position2.X, y2 = position2.Y;
        int i, j;
        if (x1>x2)
        {
            i = x2;
            x2 = x1;
            x1 = i;
        }
        if (y1>y2)
        {
            j = y2;
            y2 = y1;
            y1 = j;
        }
        for (j=y1; j<=y2; j++)
            for (i=x1; i<=x2; i++)
                SetProperty(sim, ui::Point(i, j));
    }

    void DrawFill(Simulation *sim, Brush const &, ui::Point position) override
    {
        sim->flood_prop(position.X, position.Y, change);
    }
};

// gui/game/tool/DecorationTool's drawing, its fill reading the frame the renderer last drew (GameView's)
class DecorationDrawTool : public Tool {
public:
    DecorationDrawTool(int mode, RGBA colour, const RendererFrame &frame) : Colour(colour), rendererFrame(frame)
    {
        ToolID = mode;
    }
    RGBA Colour;
    const RendererFrame &rendererFrame;

    void Draw(Simulation * sim, Brush const &brush, ui::Point position) override
    {
        sim->ApplyDecorationPoint(position.X, position.Y, Colour.Red, Colour.Green, Colour.Blue, Colour.Alpha, ToolID, brush);
    }

    void DrawLine(Simulation * sim, Brush const &brush, ui::Point position1, ui::Point position2, bool dragging) override
    {
        sim->ApplyDecorationLine(position1.X, position1.Y, position2.X, position2.Y, Colour.Red, Colour.Green, Colour.Blue, Colour.Alpha, ToolID, brush);
    }

    void DrawRect(Simulation * sim, Brush const &brush, ui::Point position1, ui::Point position2) override
    {
        sim->ApplyDecorationBox(position1.X, position1.Y, position2.X, position2.Y, Colour.Red, Colour.Green, Colour.Blue, Colour.Alpha, ToolID);
    }

    void DrawFill(Simulation * sim, Brush const &brush, ui::Point position) override
    {
        if (!rendererFrame.Size().OriginRect().Contains(position))
        {
            return;
        }
        auto loc = RGB::Unpack(rendererFrame[position]);
        if (ToolID == DECO_CLEAR)
            // TODO: this is actually const-correct
            sim->ApplyDecorationFill(rendererFrame, position.X, position.Y, 0, 0, 0, 0, loc.Red, loc.Green, loc.Blue);
        else
            sim->ApplyDecorationFill(rendererFrame, position.X, position.Y, Colour.Red, Colour.Green, Colour.Blue, Colour.Alpha, loc.Red, loc.Green, loc.Blue);
    }
};

} // namespace

void ClassicSim::useTool(const ToolUse &u, TptBrush &brush) {
    Simulation &sim = *impl_->sim;
    sim.replaceModeFlags = u.replaceFlags;
    sim.replaceModeSelected = u.replaceSelected;
    brush.setIndex(u.brush);
    brush.setRadius(u.rx, u.ry);
    const Brush &b = *brush.impl().brushes[size_t(brush.index())];
    if (u.kind == ToolUse::Kind::Property && (u.property.field < 0 || u.property.field >= ParticleProperty::COUNT))
        return;
    if (u.action == ToolUse::Action::Create) {
        sim.create_part(-1, u.x1, u.y1, TYP(u.tool), ID(u.tool));
        return;
    }
    // GameModel::AllocElementTool's tool for each element, and the walls' WallTool
    std::unique_ptr<Tool> tool;
    if (u.kind == ToolUse::Kind::Sim) {
        if (u.tool < 0 || u.tool >= int(GetTools().size()))
            return;
        tool = std::make_unique<SimTool>(GetTools()[size_t(u.tool)]);
    } else if (u.kind == ToolUse::Kind::Property)
        tool = std::make_unique<PropertyDrawTool>(accessProperty(u.property));
    else if (u.kind == ToolUse::Kind::Deco) {
        sim.deco_space = u.decoSpace;
        tool = std::make_unique<DecorationDrawTool>(u.tool, RGB::Unpack(u.decoColour & 0xFFFFFF).WithAlpha(u.decoColour >> 24),
                                                    impl_->ren->GetVideo());
    }
    else if (u.kind == ToolUse::Kind::Wall)
        tool = std::make_unique<WallTool>(u.tool, String(), 0x000000_rgb, ByteString());
    else if (TYP(u.tool) == PT_LIGH)
        tool = std::make_unique<Element_LIGH_Tool>(u.tool, ByteString());
    else if (TYP(u.tool) == PT_TESC)
        tool = std::make_unique<Element_TESC_Tool>(u.tool, ByteString());
    else if (TYP(u.tool) == PT_STKM || TYP(u.tool) == PT_FIGH || TYP(u.tool) == PT_STKM2)
        tool = std::make_unique<PlopTool>(u.tool, ByteString());
    else
        tool = std::make_unique<ElementTool>(u.tool, ByteString());
    tool->Strength = u.strength;
    ui::Point p1(u.x1, u.y1), p2(u.x2, u.y2);
    switch (u.action) {
    case ToolUse::Action::Draw:
        tool->Draw(&sim, b, p1);
        break;
    case ToolUse::Action::DrawLine:
        tool->DrawLine(&sim, b, p1, p2, u.dragging);
        break;
    case ToolUse::Action::DrawRect:
        tool->DrawRect(&sim, b, p1, p2);
        break;
    case ToolUse::Action::DrawFill:
        tool->DrawFill(&sim, b, p1);
        break;
    case ToolUse::Action::Click:
        tool->Click(&sim, b, p1);
        break;
    case ToolUse::Action::Drag:
        tool->Drag(&sim, b, p1, p2);
        break;
    case ToolUse::Action::Create:
        break;
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

std::vector<std::string> ClassicSim::golDescriptions() {
    std::vector<std::string> out;
    for (const auto &g : SimulationData::builtinGol)
        out.push_back(g.description.ToUtf8());
    return out;
}

std::vector<ClassicSim::Wall> ClassicSim::exportWalls() {
    std::unique_ptr<SimulationData> temporary;
    if (liveClassicSims == 0)
        temporary = std::make_unique<SimulationData>();
    std::vector<Wall> out;
    for (const auto &w : SimulationData::CRef().wtypes)
        out.push_back({w.name.ToUtf8(), w.descs.ToUtf8(), w.identifier, w.colour.Pack() & 0xFFFFFF,
                       w.eglow.Pack() & 0xFFFFFF, w.drawstyle});
    return out;
}

std::vector<ClassicSim::MenuSection> ClassicSim::exportMenuSections() {
    std::unique_ptr<SimulationData> temporary;
    if (liveClassicSims == 0)
        temporary = std::make_unique<SimulationData>();
    std::vector<MenuSection> out;
    for (const auto &m : SimulationData::CRef().msections)
        out.push_back({char32_t(m.icon), m.name.ToUtf8(), m.doshow != 0});
    return out;
}

ClassicSim::Sample ClassicSim::sample(int x, int y) const {
    SimulationSample s = impl_->sim->GetSample(x, y);
    Sample out;
    out.inWorld = s.isMouseInSim;
    out.hasPart = s.particle.type != 0;
    out.type = s.particle.type, out.ctype = s.particle.ctype, out.life = s.particle.life;
    out.tmp = s.particle.tmp, out.tmp2 = s.particle.tmp2, out.tmp4 = s.particle.tmp4;
    out.temp = s.particle.temp;
    out.pressure = s.AirPressure;
    out.wall = s.WallType;
    out.numParts = s.NumParts;
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
        if (out.size() < MAXSIGNS)
            out.emplace_back(String(s.text.data(), s.text.size()), s.x, s.y, sign::Justification(s.ju));
}

// What sign::getDisplayText reads at a sign's spot
SignSample ClassicSim::signSample(int x, int y) const {
    const Simulation &sim = *impl_->sim;
    SignSample out;
    if (x < 0 || x >= XRES || y < 0 || y >= YRES)
        return out;
    int r = sim.photons[y][x] ? sim.photons[y][x] : sim.pmap[y][x];
    if (r) {
        const Particle &p = sim.parts[ID(r)];
        out.hasPart = true;
        out.type = p.type, out.ctype = p.ctype, out.life = p.life, out.tmp = p.tmp, out.tmp2 = p.tmp2, out.tmp4 = p.tmp4;
        out.temp = p.temp;
    }
    out.pressure = sim.pv[y / CELL][x / CELL];
    out.aheat = sim.hv[y / CELL][x / CELL] - 273.15f;
    return out;
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

RendererSettings presetSettings(int preset, uint32_t overlays, int decorationLevel) {
    const auto &p = Renderer::renderModePresets[std::clamp(preset, 0, ClassicSim::presetCount() - 1)];
    RendererSettings s;
    s.renderMode = p.renderMode;
    s.displayMode = p.displayMode;
    s.colorMode = p.colorMode;
    s.wantHdispLimitMin = p.wantHdispLimitMin;
    s.wantHdispLimitMax = p.wantHdispLimitMax;
    s.gravityFieldEnabled = (overlays & 1) != 0;
    s.gravityZonesEnabled = (overlays & 2) != 0;
    s.decorationLevel = RendererSettings::DecorationLevel(decorationLevel);
    return s;
}

void ClassicSim::render(int preset, std::vector<uint32_t> &out, uint32_t overlays, int decorationLevel) {
    Renderer &ren = *impl_->ren;
    preset = std::clamp(preset, 0, presetCount() - 1);
    if (preset != impl_->lastPreset || overlays != impl_->lastOverlays || decorationLevel != impl_->lastDeco) {
        ren.ApplySettings(presetSettings(preset, overlays, decorationLevel));
        impl_->lastPreset = preset;
        impl_->lastOverlays = overlays;
        impl_->lastDeco = decorationLevel;
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

std::vector<SimToolInfo> tptSimTools() {
    std::vector<SimToolInfo> out;
    for (const SimTool &t : GetTools()) {
        SimToolInfo i;
        i.identifier = t.Identifier;
        i.name = t.Name.ToUtf8();
        i.description = t.Description.ToUtf8();
        i.colour = t.Colour.Pack() & 0xFFFFFF;
        out.push_back(i);
    }
    return out;
}

const std::vector<std::string> &tptPropertyNames() {
    static const std::vector<std::string> names = [] {
        std::vector<std::string> n;
        for (const auto &p : Particle::GetProperties())
            n.push_back(p.Name);
        return n;
    }();
    return names;
}

ParticleProperty::Kind tptPropertyKind(int field) {
    const auto &props = Particle::GetProperties();
    if (field < 0 || field >= int(props.size()))
        return ParticleProperty::Kind::Int;
    switch (props[size_t(field)].Type) {
    case StructProperty::Float: return ParticleProperty::Kind::Float;
    case StructProperty::UInteger: return ParticleProperty::Kind::UInt;
    default: return ParticleProperty::Kind::Int;
    }
}

namespace {

// AccessProperty::Parse (simulation/AccessPropertyParse.cpp) with the temperature scale passed in rather than read
// from GameController, and its errors as false
std::optional<AccessProperty> parseProperty(int prop, String value, TempScale scale)
{
	AccessProperty changeProperty;
	auto &sd = SimulationData::CRef();
	auto &properties = Particle::GetProperties();
	auto &elements = sd.elements;
	if (prop < 0 || prop >= int(properties.size()) || !value.size())
	{
		return std::nullopt;
	}
	value = value.ToUpper();
	try
	{
		switch(properties[prop].Type)
		{
		case StructProperty::Integer:
		case StructProperty::ParticleType:
		{
			int v;
			if(value.length() > 2 && value.BeginsWith("0X"))
			{
				//0xC0FFEE
				v = value.Substr(2).ToNumber<unsigned int>(Format::Hex());
			}
			else if(value.length() > 1 && value.BeginsWith("#"))
			{
				//#C0FFEE
				v = value.Substr(1).ToNumber<unsigned int>(Format::Hex());
			}
			else if(value.length() > 2 && value.BeginsWith("0B"))
			{
				//0b1001100110
				ByteString substr = value.Substr(2).ToUtf8();
				size_t converted;
				try
				{
					v = std::stoi(substr, &converted, 2);
				}
				catch (const std::invalid_argument &)
				{
					return std::nullopt;
				}
				catch (const std::out_of_range &)
				{
					return std::nullopt;
				}
				if (converted != substr.length())
					return std::nullopt;
			}
			else
			{
				// Try to parse as particle name
				v = sd.GetParticleType(value.ToUtf8());

				// Try to parse special GoL rules
				if (v == -1 && properties[prop].Name == "ctype")
				{
					if (value.length() > 1 && value.BeginsWith("B") && value.Contains("/"))
					{
						v = ParseGOLString(value);
						if (v == -1)
						{
							return std::nullopt;
						}
					}
					else
					{
						v = sd.GetParticleType(value.ToUtf8());
						auto &builtinGol = SimulationData::builtinGol;
						auto &customGol = sd.GetCustomGol();
						if (v == -1)
						{
							for (int i = 0; i < int(builtinGol.size()); ++i)
							{
								if (builtinGol[i].name == value)
								{
									v = PMAP(i, PT_LIFE);
									break;
								}
							}
						}
						if (v == -1)
						{
							for (int i = 0; i < int(customGol.size()); ++i)
							{
								if (customGol[i].nameString == value)
								{
									v = PMAP(customGol[i].rule, PT_LIFE);
									break;
								}
							}
						}
					}
				}

				// Parse as plain number
				if (v == -1)
				{
					v = value.ToNumber<int>();
				}
			}

			if (properties[prop].Name == "type" && (v < 0 || v >= PT_NUM || !elements[v].Enabled))
			{
				return std::nullopt;
			}
			changeProperty.propertyValue = v;
			break;
		}
		case StructProperty::UInteger:
		{
			unsigned int v;
			if(value.length() > 2 && value.BeginsWith("0X"))
			{
				//0xC0FFEE
				v = value.Substr(2).ToNumber<unsigned int>(Format::Hex());
			}
			else if(value.length() > 1 && value.BeginsWith("#"))
			{
				//#C0FFEE
				v = value.Substr(1).ToNumber<unsigned int>(Format::Hex());
			}
			else if(value.length() > 2 && value.BeginsWith("0B"))
			{
				//0b1001100110
				ByteString substr = value.Substr(2).ToUtf8();
				size_t converted;
				v = std::stol(substr, &converted, 2);
				if (converted != substr.length())
					return std::nullopt;
			}
			else
			{
				v = value.ToNumber<unsigned int>();
			}
			changeProperty.propertyValue = v;
			break;
		}
		case StructProperty::Float:
		{
			if (properties[prop].Name == "temp")
				changeProperty.propertyValue = format::StringToTemperature(value, scale);
			else
				changeProperty.propertyValue = value.ToNumber<float>();
		}
			break;
		default:
			return std::nullopt;
		}
		changeProperty.propertyIndex = prop;
	}
	catch (const std::exception& ex)
	{
		return std::nullopt;
	}
	return changeProperty;
}

} // namespace

bool tptParseProperty(int field, const std::u32string &value, int temperatureScale, ParticleProperty &out) {
    std::unique_ptr<SimulationData> temporary;
    if (liveClassicSims == 0)
        temporary = std::make_unique<SimulationData>();
    auto a = parseProperty(field, String(value.begin(), value.end()), TempScale(temperatureScale));
    if (!a)
        return false;
    out.field = a->propertyIndex;
    out.kind = tptPropertyKind(out.field);
    if (auto *i = std::get_if<int>(&a->propertyValue))
        out.bits = uint32_t(*i);
    else if (auto *u = std::get_if<unsigned int>(&a->propertyValue))
        out.bits = *u;
    else
        out.bits = std::bit_cast<uint32_t>(std::get<float>(a->propertyValue));
    return true;
}

std::u32string tptPropertyText(const ParticleProperty &p, int temperatureScale) {
    const auto &props = Particle::GetProperties();
    if (p.field < 0 || p.field >= int(props.size()))
        return U"";
    const StructProperty &prop = props[size_t(p.field)];
    AccessProperty a = accessProperty(p);
    String valueString;
    if (prop.Name == "temp")
    {
        StringBuilder sb;
        format::RenderTemperature(sb, std::get<float>(a.propertyValue), TempScale(temperatureScale));
        valueString = sb.Build();
    }
    else
    {
        valueString = prop.ToString(a.propertyValue);
    }
    return std::u32string(valueString.begin(), valueString.end());
}

bool tptValidGolName(const std::u32string &name) { return ValidateGOLName(String(name.begin(), name.end())); }

int tptParseGolRule(const std::u32string &rule) { return ParseGOLString(String(rule.begin(), rule.end())); }

std::u32string tptGolRuleString(int rule) {
    String s = SerialiseGOLRule(rule);
    return std::u32string(s.begin(), s.end());
}

void ClassicSim::setCustomGol(const std::vector<CustomGol> &types) {
    std::vector<CustomGOLData> list;
    for (const CustomGol &t : types)
        list.push_back(CustomGOLData{t.rule, RGB::Unpack(t.colour1), RGB::Unpack(t.colour2),
                                     String(t.name.begin(), t.name.end())});
    SimulationData::Ref().SetCustomGOL(list);
}

bool ClassicSim::particleAt(int x, int y, bool photonsFirst, int &id, ClassicParticle &out) const {
    const Simulation &sim = *impl_->sim;
    if (x < 0 || y < 0 || x >= XRES || y >= YRES)
        return false;
    int r = photonsFirst ? sim.photons[y][x] : sim.pmap[y][x];
    if (!r)
        r = photonsFirst ? sim.pmap[y][x] : sim.photons[y][x];
    if (!r)
        return false;
    id = ID(r);
    const Particle &p = sim.parts[id];
    out = {p.type, p.life, p.ctype, p.x, p.y, p.vx, p.vy, p.temp, p.tmp, p.tmp2, p.tmp3, p.tmp4, uint32_t(p.flags),
           uint32_t(p.dcolour)};
    return true;
}

std::vector<float> ClassicSim::gravityMass() const {
    const Simulation &sim = *impl_->sim;
    std::vector<float> out(size_t(XCELLS) * YCELLS);
    for (int y = 0; y < YCELLS; y++)
        for (int x = 0; x < XCELLS; x++)
            out[size_t(y) * XCELLS + x] = sim.gravIn.mass[Vec2{x, y}];
    return out;
}

} // namespace bp
