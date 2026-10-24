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

// Where Simulation::CreateLine (wallLine false) and CreateWallLine (true) call CreateParts or CreateWalls, in order,
// as x, y pairs: TPT's loop, built with TPT's settings like the rest of its code. CreateLine stamps the corners of its
// steps too only when the brush is a single pixel (radius 0, 0).
void tptLinePoints(int x1, int y1, int x2, int y2, bool wallLine, bool singlePixel, std::vector<int> &out);

// TPT's replace mode flags (Simulation::replaceModeFlags)
constexpr int REPLACE_MODE = 1, SPECIFIC_DELETE = 2;

// One use of a tool, as TPT's GameController hands it to the tool (gui/game/tool/Tool)
struct ToolUse {
    enum class Action : uint8_t {
        Draw,     // Tool::Draw: the button pressed, drawing at one point
        DrawLine, // dragging (from the last point to this one), or a line drawn with shift
        DrawRect, // a box drawn with ctrl
        DrawFill, // a flood fill with ctrl+shift
        Click,    // the button let go: places a stick man (PlopTool)
    };
    enum class Kind : uint8_t { Element, Wall };
    Action action = Action::Draw;
    Kind kind = Kind::Element;
    int tool = 0; // TPT's ToolID: an element (with a Game of Life ruleset above PMAPBITS), or a wall id
    int x1 = 0, y1 = 0, x2 = 0, y2 = 0;
    bool dragging = false; // DrawLine: while the button is held, as opposed to a line drawn with shift
    float strength = 1.0f; // Tool::Strength
    // The brush in use (TptBrush::index) and its radius
    int brush = 0, rx = 4, ry = 4;
    // Simulation::replaceModeFlags and replaceModeSelected (the element tool slot 3 holds)
    int replaceFlags = 0, replaceSelected = 0;
};

} // namespace bp
