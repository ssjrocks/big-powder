// TPT's GameView mouse and keyboard handling for drawing (gui/game/GameView.cpp): turns presses, drags, ticks and
// modifiers into the uses of tools TPT's GameController would make, and keeps the brush and what its cursor shows.
// Positions are world pixels, already kept inside the world (GameController::PointTranslate).
#pragma once
#include "classic/TptTools.h"
#include "ui/BrushCursor.h"
#include "ui/GameUi.h"

#include <array>
#include <optional>
#include <unordered_map>
#include <vector>

namespace bp {

class GameInput {
public:
    explicit GameInput(GameUi &ui) : ui_(ui) {}

    TptBrush brush;
    // What the property tool sets (PropertyTool's configuration; none until its window has been OKed)
    std::optional<ParticleProperty> property;
    int decoSpace = DECOSPACE_SRGB; // Simulation::deco_space, the colour space SMUDGE averages in
    // The sample tool's presses (SampleTool::Draw): where, and whether shift was held (it then opens the property
    // window with the particle's value)
    struct Sample {
        int x, y;
        bool shift;
    };
    std::vector<Sample> takeSamples();
    // The sign tool's clicks (SignTool::Click, when the button is let go)
    std::vector<std::array<int, 2>> takeSignClicks();
    int replaceFlags() const { return replaceFlags_; }
    bool drawing() const { return isMouseDown_; }
    CursorState::Mode drawMode() const { return drawMode_; }

    // DoKeyPress and DoKeyRelease: each key event's modifiers. Shift and ctrl choose how the next press draws (not
    // while drawing) and the tools' strength; alt snaps lines and boxes.
    void setModifiers(bool shift, bool ctrl, bool alt);
    void blur(); // OnBlur
    // OnMouseDown: alt alone makes a press the middle button
    int pressButton(int button) const { return alt_ && !shift_ && !ctrl_ ? 2 : button; }
    void mouseDown(int x, int y, int button); // a press over the world with the left, right or middle button
    void mouseUp(int x, int y);
    void mouseMove(int x, int y);
    void tick(int x, int y); // OnTick, every frame, with the mouse
    // The brush: the wheel (OnMouseWheel), [ and ] (logarithmic unless alt), Tab
    void wheel(int d) { brush.adjustSize(d, false, ctrl_, shift_); }
    void bracket(int d, bool alt) { brush.adjustSize(d, !alt, ctrl_, shift_); }
    void changeBrush() { brush.setIndex(brush.index() + 1); }
    // ; and Insert toggle replace mode, with ctrl specific delete; Delete toggles specific delete
    void toggleReplace(bool ctrl) { setReplaceFlags(replaceFlags_ ^ (ctrl ? SPECIFIC_DELETE : REPLACE_MODE)); }
    void toggleSpecificDelete() { setReplaceFlags(replaceFlags_ ^ SPECIFIC_DELETE); }

    std::vector<ToolUse> takeUses();
    CursorState cursor(int x, int y) const; // the brush cursor with the mouse at (x, y)

private:
    void updateDrawMode();
    void setReplaceFlags(int flags); // GameController::SetReplaceModeFlags
    void use(ToolUse::Action a, int x1, int y1, int x2, int y2, bool dragging, float strength);
    void drawPoints(int ox, int oy, int nx, int ny, bool held); // GameController::DrawPoints

    GameUi &ui_;
    CursorState::Mode drawMode_ = CursorState::Mode::Points;
    bool isMouseDown_ = false, skipDraw_ = false, shift_ = false, ctrl_ = false, alt_ = false;
    bool decoBrush_ = false; // the last tool pressed with is a decoration tool (its fill shows no cursor)
    int toolIndex_ = 0, replaceFlags_ = 0;
    float toolStrength_ = 1.0f;
    int lastX_ = 0, lastY_ = 0, curX_ = 0, curY_ = 0, p1X_ = 0, p1Y_ = 0; // lastPoint, currentPoint, drawPoint1
    std::vector<ToolUse> uses_;
    std::vector<Sample> samples_;
    std::vector<std::array<int, 2>> signClicks_;
    std::unordered_map<std::string, float> strengths_; // each tool's Strength, as last set (Drag keeps it)
};

} // namespace bp
