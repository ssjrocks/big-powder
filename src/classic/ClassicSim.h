// Classic mode: The Powder Toy's own simulation (third_party/tpt, compiled with TPT's flags), driven
// headlessly. It behaves exactly like TPT and is also the reference the GPU engine is tested against.
//
// This header deliberately includes nothing from TPT, so the rest of the app is built independently of
// TPT's compile settings (TPT builds with -fno-rtti and -ffast-math).
#pragma once
#include "classic/TptTools.h"
#include "sim/Signs.h"

#include <array>
#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

namespace bp {

struct SaveArea; // (classic/TptSave.h)

struct ClassicParticle {
    int type, life, ctype;
    float x, y, vx, vy, temp;
    int tmp, tmp2, tmp3, tmp4;
    uint32_t flags, dcolour;
};

// A particle's value of one of TPT's properties (AccessProperty::Get)
inline ParticleProperty particleProperty(const ClassicParticle &p, int field) {
    ParticleProperty out;
    out.field = field;
    out.kind = tptPropertyKind(field);
    auto f = [](float v) { uint32_t b; std::memcpy(&b, &v, 4); return b; };
    switch (field) {
    case ParticleProperty::TYPE: out.bits = uint32_t(p.type); break;
    case ParticleProperty::LIFE: out.bits = uint32_t(p.life); break;
    case ParticleProperty::CTYPE: out.bits = uint32_t(p.ctype); break;
    case ParticleProperty::X: out.bits = f(p.x); break;
    case ParticleProperty::Y: out.bits = f(p.y); break;
    case ParticleProperty::VX: out.bits = f(p.vx); break;
    case ParticleProperty::VY: out.bits = f(p.vy); break;
    case ParticleProperty::TEMP: out.bits = f(p.temp); break;
    case ParticleProperty::FLAGS: out.bits = p.flags; break;
    case ParticleProperty::TMP: out.bits = uint32_t(p.tmp); break;
    case ParticleProperty::TMP2: out.bits = uint32_t(p.tmp2); break;
    case ParticleProperty::TMP3: out.bits = uint32_t(p.tmp3); break;
    case ParticleProperty::TMP4: out.bits = uint32_t(p.tmp4); break;
    case ParticleProperty::DCOLOUR: out.bits = p.dcolour; break;
    default: out.field = -1; break;
    }
    return out;
}

// An element as TPT defines it (src/simulation/elements/*.cpp), read from TPT's own tables.
struct ClassicElement {
    int id = 0;
    std::string name, identifier, description;
    std::string constName; // the X in TPT's PT_X constant ("" for unused ids)
    bool enabled = false;
    bool hasUpdate = false, hasGraphics = false, hasCreate = false, hasCreateAllowed = false, hasChangeType = false;
    uint32_t colour = 0; // 0xRRGGBB
    int menuSection = 0, menuVisible = 0;
    float advection = 0, airDrag = 0, airLoss = 0, loss = 0, collision = 0, gravity = 0, newtonianGravity = 0;
    float diffusion = 0, hotAir = 0;
    int falldown = 0, flammable = 0, explosive = 0, meltable = 0, hardness = 0;
    uint32_t photonReflectWavelengths = 0;
    int weight = 0, heatConduct = 0;
    float heatCapacity = 1;
    uint32_t properties = 0, carriesTypeIn = 0;
    float lowPressure = 0, highPressure = 0, lowTemperature = 0, highTemperature = 0;
    int lowPressureT = -1, highPressureT = -1, lowTemperatureT = -1, highTemperatureT = -1; // -1 none, -2 special
    ClassicParticle defaults{}; // DefaultProperties (x, y, vx, vy unused)
};

struct ClassicOptions {
    bool heat = true;              // false = TPT's "legacy" heat mode
    bool ambientHeat = false;
    bool newtonianGravity = false;
    bool waterEqualisation = false;
    int edgeMode = 0;              // 0 void, 1 solid, 2 loop
    int gravityMode = 0;           // 0 vertical, 1 off, 2 radial, 3 custom
    int airMode = 0;               // 0 on, 1 pressure off, 2 velocity off, 3 off, 4 no update
    float customGravX = 0.0f, customGravY = 0.0f;
    // Air's ambient air temperature, pressure and velocity at the edges, vorticity confinement, convection mode
    // (TPT's AIRC_*: 0 none, 1 legacy, 2 Boussinesq)
    float ambientTemp = 22.0f + 273.15f, edgePressure = 0.0f, edgeVX = 0.0f, edgeVY = 0.0f, vorticity = 0.1f;
    int convectionMode = 2;
    bool prettyPowder = false; // the sand effect
};

// TPT keeps its element tables in a process-wide singleton, so only one ClassicSim may exist at a time.
class ClassicSim {
public:
    static constexpr int W = 612; // TPT's XRES, YRES (checked against TPT in the .cpp)
    static constexpr int H = 384;
    static constexpr int CELL = 4;

    ClassicSim();
    ~ClassicSim();
    ClassicSim(const ClassicSim &) = delete;
    ClassicSim &operator=(const ClassicSim &) = delete;

    void setOptions(const ClassicOptions &o);
    const ClassicOptions &options() const { return opts_; }

    // One TPT frame: BeforeSim, UpdateParticles over all particles, AfterSim.
    void step();
    // A frame without a tick: TPT's BeforeSim still runs (the sand effect's tint moves on)
    void idleFrame();
    int sandcolourInterface() const; // Simulation::sandcolour_interface (the sand effect's tint for tools)
    void clear();
    void seed(uint32_t s); // TPT's simulation RNG (also used when elements are created)
    // Simulation::CreateSnapshot and Restore, for undo: everything a tick carries on (not the settings)
    struct State;
    std::shared_ptr<const State> snapshot() const;
    void restore(const State &s);
    // Simulation::Save of the area between two corners (both in it, kept inside the world as GameController's
    // SaneSaveRect does), Simulation::Load of one at block (bx, by), and Simulation::clear_area
    SaveArea saveArea(int x1, int y1, int x2, int y2, bool includePressure) const;
    void loadArea(const SaveArea &s, bool includePressure, int bx, int by);
    void clearArea(int x, int y, int w, int h);

    // Loads a .cps/.stm save (OPS or the old PSv format). The save is placed at block (bx, by), in units
    // of CELL pixels, like TPT's stamps.
    bool load(const std::vector<char> &data, std::string &error, int bx = 0, int by = 0);
    bool loadFile(const std::string &path, std::string &error);
    bool save(std::vector<char> &out, std::string &error) const;
    bool saveFile(const std::string &path, std::string &error) const;

    // Uses one of TPT's tools with TPT's own code (gui/game/tool's ElementTool, WallTool and the rest), as its
    // GameController does; the brush is set to the use's brush and radius first.
    void useTool(const ToolUse &use, TptBrush &brush);
    // Creates one particle with TPT's create_part (defaults set by the element); returns its id or -1.
    int createPart(int x, int y, int type);
    // create_part(-3, ...): no check of what is there, so particles can stack.
    int createPartStacked(int x, int y, int type);
    void setParticleTemp(int id, float temp);
    void setParticleFields(int id, int ctype, int tmp, int tmp2, int tmp3, int tmp4);
    int particleTmp(int id) const;
    // The players' keys (comm and pcomm of STKM and STKM2), as TPT's GameController sets them.
    void setStickmanKeys(int comm1, int pcomm1, int comm2, int pcomm2);

    // Renders with one of TPT's display presets (0..10, see presetName) into W*H pixels, 0xRRGGBB.
    // overlays: the gravity field display (1) and the gravity zones (2), as RenderParams::overlays
    void render(int preset, std::vector<uint32_t> &out, uint32_t overlays = 0, int decorationLevel = 1);
    void render(const ViewSettings &view, std::vector<uint32_t> &out, uint32_t overlays = 0);
    int foundParticles() const; // what the last render found (ctrl+F)
    bool heatRange(float &lo, float &hi) const; // the last render's hdispLimit, if valid
    // GameController's ResetAir, ResetSpark, InvertAirSim and ResetAHeat
    void resetAir();
    void resetSpark();
    void invertAir();
    void resetAmbientHeat();
    void clearRenderAccumulation(); // the fire buffer and the persistent display
    // Newtonian gravity's field (gravOut), (W/CELL) x (H/CELL) each.
    void gravityField(std::vector<float> &fx, std::vector<float> &fy) const;
    static int presetCount();
    // Every element slot (TPT's PT_NUM), and can_move[moving * maxElements + destination] for the first
    // maxElements. Works whether or not a ClassicSim exists.
    static std::vector<ClassicElement> exportElements(std::vector<uint8_t> &canMove, int maxElements);
    static std::string presetName(int preset);
    // TPT's renderer tables, laid out as RT_* in sim/GpuTypes.h.
    static std::vector<uint32_t> renderTables(); // ends with TPT's font (RenderTables.h)
    // A character's glyph: its offset in the font data (what FontReader::lookupChar finds, missing characters
    // shown as U+FFFD) and its width.
    static uint32_t fontGlyph(char32_t ch, int *width = nullptr);
    // The font's code point ranges (first, last) and each glyph's offset, in order
    static void exportFont(std::vector<std::array<uint32_t, 2>> &ranges, std::vector<uint32_t> &glyphs);
    static std::vector<std::string> golNames(); // SimulationData::builtinGol's names
    static std::vector<std::string> golDescriptions();
    // TPT's walls (wtypes) and menu sections (msections), for the interface
    struct Wall {
        std::string name, description, identifier;
        uint32_t colour = 0, eglow = 0;
        int drawstyle = 0;
    };
    static std::vector<Wall> exportWalls();
    struct MenuSection {
        char32_t icon = 0;
        std::string name;
        bool visible = true;
    };
    static std::vector<MenuSection> exportMenuSections();
    // What is at (x, y), as TPT's HUD reads it (Simulation::GetSample)
    struct Sample {
        bool inWorld = false, hasPart = false;
        int type = 0, ctype = 0, life = 0, tmp = 0, tmp2 = 0, tmp4 = 0, wall = 0, numParts = 0;
        float temp = 0.0f, pressure = 0.0f;
        int id = -1, x = 0, y = 0;
        float gravX = 0.0f, gravY = 0.0f, airTemp = 0.0f;
    };
    Sample sample(int x, int y) const;
    // The particle at (x, y): an energy particle there or the one in pmap, whichever comes first (the sample tool
    // looks at energy particles first, the property tool at pmap first)
    bool particleAt(int x, int y, bool photonsFirst, int &id, ClassicParticle &p) const;
    std::vector<float> gravityMass() const; // Newtonian gravity's input (gravIn.mass), cell by cell
    // The custom Game of Life types (SimulationData::SetCustomGOL): LIFE made with one of their rules takes its colours
    void setCustomGol(const std::vector<CustomGol> &types);
    // The simulation's signs (TPT keeps up to 16)
    std::vector<Sign> signs() const;
    void setSigns(const std::vector<Sign> &signs);
    SignSample signSample(int x, int y) const; // what a sign's {t}, {p}, {type}... read at (x, y)

    int particleCount() const;
    int elementCount(int type) const;
    // The particles in id order; with `ids`, also each one's id.
    std::vector<ClassicParticle> particles(std::vector<int> *ids = nullptr) const;
    // The size in cells (blocks) of the last save loaded.
    int loadedCW() const { return loadedCW_; }
    int loadedCH() const { return loadedCH_; }
    float pressure(int cx, int cy) const;     // air map cell
    // Air maps, (W/CELL) x (H/CELL) each: pressure, velocity x/y, ambient heat. Testing.
    void airMaps(std::vector<float> &pv, std::vector<float> &vx, std::vector<float> &vy, std::vector<float> &hv) const;
    void setAirMaps(const std::vector<float> &pv, const std::vector<float> &vx, const std::vector<float> &vy,
                    const std::vector<float> &hv);
    float ambientHeat(int cx, int cy) const;
    std::string elementName(int type) const;
    int elementByName(const std::string &name) const; // -1 if unknown
    // Walls: one TPT wall id per CELL block, (W/CELL) x (H/CELL). setWalls also prepares TPT's air blocking maps
    // as TPT does when it loads a save.
    std::vector<uint8_t> walls() const;
    void setWalls(const std::vector<uint8_t> &bmap);
    std::vector<float> fanVX() const;
    std::vector<float> fanVY() const;
    // Testing: change how an element couples to air (TPT's Advection and AirDrag properties).
    void setAirCoupling(int type, float advection, float airDrag);
    uint64_t tick() const { return tick_; }

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    ClassicOptions opts_;
    uint64_t tick_ = 0;
    int loadedCW_ = 0, loadedCH_ = 0;
};

} // namespace bp
