#include "ui/GameInput.h"

namespace bp {

using Mode = CursorState::Mode;

void GameInput::setModifiers(bool shift, bool ctrl, bool alt) {
    if (shift != shift_ || ctrl != ctrl_) {
        shift_ = shift;
        ctrl_ = ctrl;
        if (!isMouseDown_)
            updateDrawMode();
        toolStrength_ = shift_ ? 10.0f : ctrl_ ? 0.1f : 1.0f; // UpdateToolStrength
    }
    alt_ = alt;
    ui_.setModifiers(shift_, ctrl_, alt_);
}

void GameInput::blur() {
    shift_ = ctrl_ = alt_ = false;
    ui_.setModifiers(false, false, false);
    toolStrength_ = 1.0f;
    isMouseDown_ = false;
    drawMode_ = Mode::Points;
}

void GameInput::updateDrawMode() {
    const UiTool *last = ui_.lastTool();
    bool toolBrush = last && last->identifier.rfind("DEFAULT_TOOL_", 0) == 0;
    if (ctrl_ && shift_)
        drawMode_ = toolBrush ? Mode::Points : Mode::Fill;
    else if (ctrl_)
        drawMode_ = Mode::Rect;
    else if (shift_)
        drawMode_ = Mode::Line;
    else
        drawMode_ = Mode::Points;
    if (last && last->identifier == "DEFAULT_UI_SAMPLE")
        drawMode_ = Mode::Points;
}

void GameInput::setReplaceFlags(int flags) {
    if (!(replaceFlags_ & REPLACE_MODE) && (flags & REPLACE_MODE))
        flags &= ~SPECIFIC_DELETE; // replace mode has just been switched on
    if (!(replaceFlags_ & SPECIFIC_DELETE) && (flags & SPECIFIC_DELETE))
        flags &= ~REPLACE_MODE;
    if ((flags & SPECIFIC_DELETE) && (flags & REPLACE_MODE))
        flags &= ~SPECIFIC_DELETE;
    replaceFlags_ = flags;
}

void GameInput::use(ToolUse::Action a, int x1, int y1, int x2, int y2, bool dragging, float strength) {
    const UiTool *t = ui_.activeTool(toolIndex_);
    if (!t)
        return;
    ToolUse u;
    u.action = a;
    u.kind = t->kind == UiTool::Kind::Wall ? ToolUse::Kind::Wall : ToolUse::Kind::Element;
    u.tool = t->kind == UiTool::Kind::Wall ? t->id : t->brushType();
    u.x1 = x1, u.y1 = y1, u.x2 = x2, u.y2 = y2;
    u.dragging = dragging;
    u.strength = strength;
    u.brush = brush.index(), u.rx = brush.rx(), u.ry = brush.ry();
    u.replaceFlags = replaceFlags_;
    if (const UiTool *r = ui_.activeTool(3)) // (Simulation::replaceModeSelected: the tool's ToolID)
        u.replaceSelected = r->kind == UiTool::Kind::Wall ? r->id : r->brushType();
    uses_.push_back(u);
}

void GameInput::drawPoints(int ox, int oy, int nx, int ny, bool held) {
    if (held)
        use(ToolUse::Action::DrawLine, ox, oy, nx, ny, true, toolStrength_);
    else
        use(ToolUse::Action::Draw, nx, ny, nx, ny, false, toolStrength_);
}

void GameInput::mouseDown(int x, int y, int button) {
    toolIndex_ = button;
    ui_.setLastTool(toolIndex_);
    updateDrawMode();
    isMouseDown_ = true;
    if (drawMode_ == Mode::Rect || drawMode_ == Mode::Line) {
        p1X_ = x, p1Y_ = y;
    } else if (drawMode_ == Mode::Points) {
        lastX_ = curX_ = x, lastY_ = curY_ = y;
        drawPoints(lastX_, lastY_, curX_, curY_, false);
    } else if (drawMode_ == Mode::Fill) {
        use(ToolUse::Action::DrawFill, x, y, x, y, false, 1.0f);
    }
}

// OnMouseUp: a line or box is drawn now; points end with a line to here, and the tool's click (a stick man's tool
// places one). Whichever button is let go ends the drawing.
void GameInput::mouseUp(int x, int y) {
    if (isMouseDown_) {
        isMouseDown_ = false;
        if (drawMode_ == Mode::Rect || drawMode_ == Mode::Line) {
            if (alt_ && drawMode_ == Mode::Line)
                lineSnap(p1X_, p1Y_, x, y);
            if (alt_ && drawMode_ == Mode::Rect)
                rectSnap(p1X_, p1Y_, x, y);
            use(drawMode_ == Mode::Rect ? ToolUse::Action::DrawRect : ToolUse::Action::DrawLine, p1X_, p1Y_, x, y,
                false, 1.0f);
        } else if (drawMode_ == Mode::Points) {
            drawPoints(lastX_, lastY_, x, y, true);
            use(ToolUse::Action::Click, x, y, x, y, false, toolStrength_);
        } else if (drawMode_ == Mode::Fill) {
            use(ToolUse::Action::DrawFill, x, y, x, y, false, 1.0f);
        }
    }
    updateDrawMode(); // (ctrl or shift may have changed while drawing)
}

void GameInput::mouseMove(int x, int y) {
    if (!isMouseDown_)
        return;
    if (drawMode_ == Mode::Points) {
        curX_ = x, curY_ = y;
        drawPoints(lastX_, lastY_, curX_, curY_, true);
        lastX_ = curX_, lastY_ = curY_;
        skipDraw_ = true;
    } else if (drawMode_ == Mode::Fill) {
        use(ToolUse::Action::DrawFill, x, y, x, y, false, 1.0f);
        skipDraw_ = true;
    }
}

// OnTick: holding the button draws again each frame (from the last point), or fills again, unless the mouse
// moved and drew since the last frame
void GameInput::tick(int x, int y) {
    if (skipDraw_) {
        skipDraw_ = false;
    } else if (isMouseDown_) {
        if (drawMode_ == Mode::Points) {
            drawPoints(lastX_, lastY_, curX_, curY_, true);
            lastX_ = curX_, lastY_ = curY_;
        } else if (drawMode_ == Mode::Fill) {
            use(ToolUse::Action::DrawFill, x, y, x, y, false, 1.0f);
        }
    }
}

std::vector<ToolUse> GameInput::takeUses() {
    std::vector<ToolUse> u;
    u.swap(uses_);
    return u;
}

CursorState GameInput::cursor(int x, int y) const {
    CursorState cs;
    cs.mode = drawMode_;
    cs.mouseDown = isMouseDown_;
    cs.snap = alt_;
    const UiTool *last = ui_.lastTool();
    cs.wallBrush = last && last->kind == UiTool::Kind::Wall;
    cs.x1 = p1X_, cs.y1 = p1Y_;
    cs.x = x, cs.y = y;
    return cs;
}

} // namespace bp
