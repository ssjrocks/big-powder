// TPT's brushes and the uses of its tools, shared by Classic mode, which runs TPT's own tool code
// (ClassicSim::useTool), and the GPU engine, which does the same with sim/Editing. Like ClassicSim.h, this header
// includes nothing from TPT.
#pragma once
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace bp {

// TPT's brushes (gui/game/Brush and its kinds), made by TPT's own code: the circle (with TPT's "perfect circle"
// setting, on by default), the square and the triangle, then bitmap brushes from .ptb files, in the order of
// GameModel::BuildBrushList. TPT starts with the circle at radius 4.
class TptBrush {
public:
    explicit TptBrush(bool perfectCircle = true);
    ~TptBrush();
    TptBrush(const TptBrush &other);
    TptBrush &operator=(const TptBrush &other);

    // Adds the .ptb files in dir as TPT loads its Brushes folder (square bitmaps, a byte a pixel); returns how many
    int loadBitmaps(const std::string &dir);
    // The Brushes folder every brush made from now on loads (so the tools' own brushes match the interface's)
    static void setBitmapFolder(const std::string &dir);
    int count() const;
    int index() const { return index_; }
    void setIndex(int i); // GameModel::SetBrushID: wraps around and keeps the radius
    int rx() const;
    int ry() const;
    void setRadius(int rx, int ry);                                       // Brush::SetRadius: 0..200 each
    void adjustSize(int delta, bool logarithmic, bool keepX, bool keepY); // Brush::AdjustSize
    // The shape, (2 rx + 1) x (2 ry + 1) row by row, nonzero inside, and its outline (what TPT's cursor shows)
    const std::vector<uint8_t> &bitmap() const;
    const std::vector<uint8_t> &outline() const;

    struct Impl; // TPT's brush objects (classic/TptToolsImpl.h)
    Impl &impl() const { return *impl_; }

private:
    std::unique_ptr<Impl> impl_;
    int index_ = 0;
};

// Where Simulation::CreateLine (Parts), CreateWallLine (Walls) and ApplyDecorationLine (Deco) stamp along a line, in
// order, as x, y pairs: TPT's loop, built with TPT's settings like the rest of its code. CreateLine stamps the corners
// of its steps too only when the brush is a single pixel (radius 0, 0), and not past the line's end; the decoration
// line does the corners past the end too.
enum class LineKind { Parts, Walls, Deco };
void tptLinePoints(int x1, int y1, int x2, int y2, LineKind kind, bool singlePixel, std::vector<int> &out);

// TPT's decoration modes (SimulationData.h) and colour spaces (SimulationSettings.h)
enum DecoMode { DECO_DRAW, DECO_CLEAR, DECO_ADD, DECO_SUBTRACT, DECO_MULTIPLY, DECO_DIVIDE, DECO_SMUDGE };
enum DecoSpace { DECOSPACE_SRGB, DECOSPACE_LINEAR, DECOSPACE_GAMMA22, DECOSPACE_GAMMA18 };
// Simulation::ApplyDecoration on a particle's dcolour (0xAARRGGBB) with a colour (the same layout), for every mode but
// SMUDGE: TPT's arithmetic with TPT's settings. Each of the four channels comes out of its own old value alone.
uint32_t tptDecorate(uint32_t dcolour, uint32_t colour, int mode);
// ApplyDecoration's SMUDGE at a pixel: around[(dx + 2) * 5 + (dy + 2)] is the dcolour of the particle in pmap there (0
// if none); inside is whether the pixel is CELL or more from the world's edges. Sets `changed` as TPT changes it.
uint32_t tptSmudge(uint32_t dcolour, const uint32_t around[25], bool inside, int decoSpace, bool &changed);
// Misc.cpp's HSV_to_RGB and RGB_to_HSV (hue 0-359, the rest 0-255), and Gradient's two-colour gradient (0xRRGGBB)
void tptHsvToRgb(int h, int s, int v, int &r, int &g, int &b);
void tptRgbToHsv(int r, int g, int b, int &h, int &s, int &v);
std::vector<uint32_t> tptGradient(uint32_t from, uint32_t to, int resolution);
// Misc.h's clamp_flt: f from [min, max] onto 0-255 (the colour picker's hue across its square)
unsigned tptClampFlt(float f, float min, float max);
// The colour picker's label: the four channels as String::Build writes them in hex, uppercase, width 2
std::u32string tptColourHex(int a, int r, int g, int b);

// What SimTool CYCL adds to the air's velocity at (x, y) for a brush centred on (brushX, brushY): TPT's arithmetic
// with TPT's settings, from still air (as long as it isn't clamped, adding this is what CYCL does)
void tptCycloneDelta(float strength, int brushX, int brushY, int x, int y, float &dvx, float &dvy);

// A custom Game of Life type (CustomGOLData): its name, rule (GOLString's form) and colours (0xRRGGBB)
struct CustomGol {
    std::u32string name;
    int rule = 0;
    uint32_t colour1 = 0, colour2 = 0;
};
// GOLString.cpp: ValidateGOLName, ParseGOLString (-1 if it isn't a rule) and SerialiseGOLRule
bool tptValidGolName(const std::u32string &name);
int tptParseGolRule(const std::u32string &rule);
std::u32string tptGolRuleString(int rule);

// TPT's replace mode flags (Simulation::replaceModeFlags)
constexpr int REPLACE_MODE = 1, SPECIFIC_DELETE = 2;

// TPT's simulation tools (simulation/simtools, ToolNumbers' order)
enum SimToolId {
    TOOL_HEAT, TOOL_COOL, TOOL_AIR, TOOL_VAC, TOOL_PGRV, TOOL_NGRV, TOOL_MIX, TOOL_CYCL, TOOL_AMBM, TOOL_AMBP, TOOL_WIND,
    SIM_TOOL_COUNT
};
struct SimToolInfo {
    std::string identifier, name, description;
    uint32_t colour = 0; // 0xRRGGBB
};
std::vector<SimToolInfo> tptSimTools(); // from TPT's own definitions

// What TPT's property tool sets (AccessProperty): one of Particle::GetProperties, and the value's bits
struct ParticleProperty {
    enum Field { TYPE, LIFE, CTYPE, X, Y, VX, VY, TEMP, FLAGS, TMP, TMP2, TMP3, TMP4, DCOLOUR, COUNT };
    enum class Kind : uint8_t { Int, UInt, Float };
    int field = -1;
    Kind kind = Kind::Int;
    uint32_t bits = 0; // the int, unsigned int or float
};
// TPT's temperature scales (TempScale)
enum TemperatureScale { TEMPSCALE_KELVIN, TEMPSCALE_CELSIUS, TEMPSCALE_FAHRENHEIT };
const std::vector<std::string> &tptPropertyNames(); // Particle::GetProperties' names, in order
ParticleProperty::Kind tptPropertyKind(int field);
// AccessProperty::Parse: element and Game of Life names, 0x, # and 0b numbers, temperatures in the scale (or with a
// K, C or F after them). False where TPT shows the value in red.
bool tptParseProperty(int field, const std::u32string &value, int temperatureScale, ParticleProperty &out);
// The text PropertyWindow::TakePropertyFrom shows for a particle's value (a temperature in the scale)
std::u32string tptPropertyText(const ParticleProperty &p, int temperatureScale);

// One use of a tool, as TPT's GameController hands it to the tool (gui/game/tool/Tool)
struct ToolUse {
    enum class Action : uint8_t {
        Draw,     // Tool::Draw: the button pressed, drawing at one point
        DrawLine, // dragging (from the last point to this one), or a line drawn with shift
        DrawRect, // a box drawn with ctrl
        DrawFill, // a flood fill with ctrl+shift
        Click,    // the button let go: places a stick man (PlopTool)
        Drag,     // Tool::Drag: each frame while a line is being drawn (WIND blows along it)
    };
    // Element and wall tools, the simulation tools (SimTool), the property tool (PropertyTool) and the decoration
    // tools (DecorationTool)
    enum class Kind : uint8_t { Element, Wall, Sim, Property, Deco };
    Action action = Action::Draw;
    Kind kind = Kind::Element;
    int tool = 0; // TPT's ToolID: an element (with a Game of Life ruleset above PMAPBITS), a wall id or a SimToolId
    ParticleProperty property; // what the property tool sets
    uint32_t decoColour = 0;   // the decoration tools' colour (0xAARRGGBB, as dcolour) and Simulation::deco_space
    int decoSpace = DECOSPACE_SRGB;
    int x1 = 0, y1 = 0, x2 = 0, y2 = 0;
    bool dragging = false; // DrawLine: while the button is held, as opposed to a line drawn with shift
    float strength = 1.0f; // Tool::Strength
    // The brush in use (TptBrush::index) and its radius
    int brush = 0, rx = 4, ry = 4;
    // Simulation::replaceModeFlags and replaceModeSelected (the element tool slot 3 holds)
    int replaceFlags = 0, replaceSelected = 0;
};

} // namespace bp
