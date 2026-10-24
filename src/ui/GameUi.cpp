#include "ui/GameUi.h"
#include "sim/Elements.h"
#include "sim/RenderTables.h"

#include <algorithm>
#include <cstdio>

namespace bp {

namespace {

constexpr int SC_WALL = 0, SC_LIFE = 12, SC_DECO = 15;

std::u32string fixed2(float v) {
    char b[64];
    std::snprintf(b, sizeof b, "%.2f", double(v));
    return utf32(b);
}

} // namespace

GameUi::GameUi(const ElementTable &e) : elements_(e) {
    // GameModel::InitTools: the elements in id order, the built-in Game of Life rulesets, the walls
    for (int id = 0; id < MAX_ELEMENTS; id++) {
        const ElementInfo &info = e.elements[id];
        if (!info.existsInTpt || (id != 0 && !e.enabled(id)))
            continue;
        UiTool t;
        t.kind = UiTool::Kind::Element;
        t.id = id;
        t.name = utf32(info.name);
        t.description = utf32(info.description);
        t.colour = info.props.colour & 0xFFFFFF;
        t.identifier = info.identifier;
        t.section = info.menuVisible ? info.menuSection : -1;
        tools_.push_back(t);
    }
    for (size_t i = 0; i < e.golNames.size(); i++) {
        UiTool t;
        t.kind = UiTool::Kind::Life;
        t.id = int(i);
        t.name = utf32(e.golNames[i]);
        t.description = i < e.golDescriptions.size() ? utf32(e.golDescriptions[i]) : U"";
        t.colour = e.renderTables[RT_GOL_COLOUR + i] & 0xFFFFFF;
        t.identifier = "DEFAULT_PT_LIFE_" + e.golNames[i];
        t.section = SC_LIFE;
        tools_.push_back(t);
    }
    for (size_t i = 0; i < e.walls.size(); i++) {
        UiTool t;
        t.kind = UiTool::Kind::Wall;
        t.id = int(i);
        t.description = utf32(e.walls[i].description);
        t.colour = e.walls[i].colour;
        t.identifier = e.walls[i].identifier;
        t.section = SC_WALL;
        tools_.push_back(t);
    }
    // TPT starts with DUST on the left button and NONE (the eraser) on the right
    setActiveTool(0, "DEFAULT_PT_DUST");
    setActiveTool(1, "DEFAULT_PT_NONE");
}

void GameUi::setActiveTool(int slot, const std::string &identifier) {
    for (size_t i = 0; i < tools_.size(); i++)
        if (tools_[i].identifier == identifier)
            active_[slot] = int(i);
}

const UiTool *GameUi::activeTool(int slot) const {
    return slot >= 0 && slot < 3 && active_[slot] >= 0 ? &tools_[size_t(active_[slot])] : nullptr;
}

std::vector<int> GameUi::sectionTools(int section) const {
    std::vector<int> out;
    for (size_t i = 0; i < tools_.size(); i++)
        if (tools_[i].section == section)
            out.push_back(int(i));
    return out;
}

void GameUi::setSize(int w, int h) {
    if (w == w_ && h == h_)
        return;
    w_ = w;
    h_ = h;
    layout();
}

void GameUi::layout() {
    buttons_.clear();
    const int W = w_, H = h_;
    auto add = [&](int x, int y, int w, int h, Icon icon, const char32_t *tip, UiAction a) -> Button & {
        Button b;
        b.x = x, b.y = y, b.w = w, b.h = h, b.icon = icon, b.tip = tip, b.action = a;
        buttons_.push_back(b);
        return buttons_.back();
    };
    // The bottom bar (GameView's constructor); the server's buttons (votes, tags, sign in) are left out
    add(1, H - 16, 17, 15, Icon::Open, U"Find & open a simulation. Hold Ctrl to load offline saves.", UiAction::Open)
        .enabled = false; // (the save browser comes later)
    add(19, H - 16, 17, 15, Icon::Reload, U"Reload the simulation", UiAction::Reload).marginLeft += 2;
    {
        Button &s = add(37, H - 16, 150, 15, Icon::Save, U"Save the simulation", UiAction::Save);
        s.alignLeft = true;
    }
    add(W - 159, H - 16, 17, 15, Icon::New, U"Erase everything", UiAction::Clear).marginLeft += 2;
    {
        Button &o = add(W - 48, H - 16, 15, 15, Icon::SimulationSettings, U"Settings", UiAction::SimulationOptions);
        o.marginLeft += 2;
        o.enabled = false; // (the options dialog comes later)
    }
    add(W - 32, H - 16, 15, 15, Icon::RenderSettings, U"Renderer options", UiAction::RenderOptions).marginLeft += 2;
    add(W - 16, H - 16, 15, 15, Icon::Pause, U"Pause/Resume the simulation", UiAction::Pause).togglable = true;
    // Quick options at the top of the bar (the sand effect and the console aren't there yet)
    {
        int y = 1;
        auto quick = [&](const char32_t *letter, const char32_t *tip, UiAction a) {
            Button &b = add(W - 16, y, 15, 15, Icon::None, tip, a);
            b.text = letter;
            b.togglable = true;
            y += 16;
        };
        quick(U"G", U"Draw gravity field \bg(ctrl+g)", UiAction::GravityGrid);
        quick(U"D", U"Draw decorations \bg(ctrl+b)", UiAction::Decorations);
        quick(U"N", U"Newtonian Gravity \bg(n)", UiAction::Newtonian);
        quick(U"A", U"Ambient heat \bg(u)", UiAction::AmbientHeat);
    }
    // The menu sections, from the bottom up (GameView::NotifyMenuListChanged); the ones with no tools yet are left out
    {
        int y = H - 48;
        for (int i = int(elements_.menuSections.size()) - 1; i >= 0; i--) {
            const auto &m = elements_.menuSections[size_t(i)];
            if (!m.visible || sectionTools(i).empty())
                continue;
            Button b;
            b.x = W - 16, b.y = y, b.w = 15, b.h = 15;
            b.text = std::u32string(1, m.icon);
            b.tip = utf32(m.name);
            b.marginLeft = 2, b.marginRight = 2, b.marginTop = 0, b.marginBottom = 3; // ui::Border(0, 2, 3, 2)
            b.togglable = true;
            b.section = i;
            buttons_.push_back(b);
            y -= 16;
        }
    }
    paletteTools_ = sectionTools(section_);
    updateScroll();
}

// GameView::updateToolButtonScroll: the palette runs right to left from WINDOWW - 56; when it is wider than the
// view, the mouse's x along the view scrolls it
void GameUi::updateScroll() {
    const int XRES = viewW();
    paletteX0_ = w_ - 56;
    int total = 31 * int(paletteTools_.size());
    int scrollSize = int(float(XRES - BARSIZE) / float(std::max(total, 1)) * float(XRES - BARSIZE));
    scrollSize = std::min(scrollSize, XRES - 1);
    scrollVisible_ = total > XRES - 15;
    if (scrollVisible_) {
        int mx = std::min(mouseX_, XRES);
        scrollX_ = int(float(mx) / float(XRES) * float(XRES - scrollSize)) + 1;
        float overflow = float(total - (XRES - BARSIZE));
        float location = float(XRES - 3) / float((XRES - 2) - mx);
        paletteX0_ += int(overflow / location);
    }
    scrollW_ = scrollSize - 1;
}

int GameUi::toolAt(int x, int y) const {
    const int top = viewH() + 1;
    if (y < top || y >= top + 18 || x < 1 || x >= viewW())
        return -1;
    for (size_t k = 0; k < paletteTools_.size(); k++) {
        int bx = paletteX0_ - 31 * int(k);
        if (x >= bx && x < bx + 30)
            return paletteTools_[k];
    }
    return -1;
}

void GameUi::showTip(const Button &b) {
    if (b.y > h_ - 17) {
        buttonTip_ = b.tip;
        buttonTipIn_ = true;
    } else
        showToolTip(b.tip, b.y, true);
}

// GameView::ToolTip
void GameUi::showToolTip(const std::u32string &text, int y, bool fromBar) {
    toolTip_ = text;
    Canvas measure(elements_);
    int tw, th;
    measure.textSize(text, tw, th);
    toolTipX_ = w_ - 27 - (tw - 1);
    toolTipY_ = fromBar ? y + 3 : h_ - MENUSIZE - 10;
    if (toolTipY_ + 10 > h_ - MENUSIZE)
        toolTipY_ = h_ - MENUSIZE - 10;
    toolTipIn_ = true;
}

bool GameUi::mouseMove(int x, int y, bool anyButtonDown) {
    mouseX_ = x;
    mouseY_ = y;
    if (scrollVisible_ || !paletteTools_.empty())
        updateScroll();
    for (const Button &b : buttons_)
        if (b.contains(x, y)) {
            showTip(b);
            // hovering over a section opens it, except the decoration tools (and not while drawing)
            if (b.section >= 0 && b.section != SC_DECO && !anyButtonDown && b.section != section_) {
                section_ = b.section;
                paletteTools_ = sectionTools(section_);
                updateScroll();
            }
            return true;
        }
    int t = toolAt(x, y);
    if (t >= 0)
        showToolTip(tools_[size_t(t)].description, 0, false);
    return !overWorld(x, y);
}

bool GameUi::mouseDown(int x, int y, int button) {
    pressed_ = button;
    pressedTool_ = toolAt(x, y);
    pressedButton_ = -1;
    for (size_t i = 0; i < buttons_.size(); i++)
        if (buttons_[i].contains(x, y))
            pressedButton_ = int(i);
    return !overWorld(x, y);
}

bool GameUi::mouseUp(int x, int y, int button) {
    bool inside = !overWorld(x, y);
    if (pressed_ != button)
        return inside;
    // ToolButton::OnMouseClick: the button released over the tool it was pressed on chooses it for that mouse button
    int t = toolAt(x, y);
    if (t >= 0 && t == pressedTool_ && button >= 0 && button < 3)
        active_[button] = t;
    if (pressedButton_ >= 0 && buttons_[size_t(pressedButton_)].contains(x, y)) {
        const Button &b = buttons_[size_t(pressedButton_)];
        if (b.section >= 0) {
            section_ = b.section;
            paletteTools_ = sectionTools(section_);
            updateScroll();
        } else if (b.enabled && button == 0)
            action_ = b.action;
    }
    pressed_ = pressedTool_ = pressedButton_ = -1;
    return inside;
}

UiAction GameUi::takeAction() {
    UiAction a = action_;
    action_ = UiAction::None;
    return a;
}

void GameUi::tick(double ms) {
    double step = ms * 1.0; // (TPT's tooltips reach full strength in a few frames)
    toolTipPresence_ = toolTipIn_ ? std::min(120.0, toolTipPresence_ + step) : std::max(0.0, toolTipPresence_ - step);
    buttonTipShow_ = buttonTipIn_ ? std::min(120.0, buttonTipShow_ + step) : std::max(0.0, buttonTipShow_ - step);
    toolTipIn_ = buttonTipIn_ = false;
}

// ui::Button::Draw with ui::Appearance's colours
void GameUi::drawButton(Canvas &c, const Button &b, bool toggled) const {
    bool inside = b.contains(mouseX_, mouseY_);
    bool down = pressed_ >= 0 && pressedButton_ >= 0 && &buttons_[size_t(pressedButton_)] == &b && inside;
    uint32_t text = 0xFFFFFF, border = 0xC8C8C8, bg = 0x000000;
    if (!b.enabled) {
        text = 0x646464, border = 0x646464, bg = 0x0A0A0A;
    } else if (down || (b.togglable && toggled)) {
        text = 0x000000, border = 0xEBEBEB, bg = 0xFFFFFF;
    } else if (inside) {
        text = 0xFFFFFF, border = 0xFFFFFF, bg = 0x141414;
    }
    c.blendFilledRect(b.x + 1, b.y + 1, b.w - 2, b.h - 2, rgb(bg));
    c.blendRect(b.x, b.y, b.w, b.h, rgb(border));
    // Component::TextPosition
    int tw, th;
    c.textSize(b.text, tw, th);
    th -= 3;
    tw -= 1;
    if (b.icon != Icon::None)
        tw += 13;
    int areaW = b.w - (b.marginRight + b.marginLeft), areaH = b.h - (b.marginTop + b.marginBottom);
    int ty = b.marginTop + (areaH - th) / 2;
    int tx = b.alignLeft ? b.marginLeft : b.marginLeft + (areaW - tw) / 2;
    int ix = tx, iy = ty - 1;
    if (b.icon != Icon::None)
        tx += 15;
    c.blendText(b.x + tx, b.y + ty, b.text, rgb(text));
    bool invert = (int(bg & 0xFF) + 3 * int((bg >> 8) & 0xFF) + 2 * int(bg >> 16)) > 544;
    if (b.icon != Icon::None)
        c.drawIcon(b.x + ix, b.y + iy, b.icon, b.enabled ? 255 : 180, invert);
}

// The tools' textures (Renderer::WallIcon; NONE's icon)
void GameUi::drawToolTexture(Canvas &c, const UiTool &t, int x, int y) const {
    const int W = 26, H = 14;
    c.drawFilledRect(x, y, W, H, 0x000000); // (a texture starts black)
    if (t.kind == UiTool::Kind::Element) { // only NONE has one
        c.blendChar(x + W / 2 - 4, y + H / 2 - 2, 0xE06C, rgb(0xFF0000));
        return;
    }
    const auto &wt = elements_.walls[size_t(t.id)];
    uint32_t primary = wt.colour, secondary = wt.eglow;
    auto px = [&](int i, int j, uint32_t col) { c.drawPixel(x + i, y + j, col); };
    switch (wt.drawstyle) {
    case 1:
        for (int j = 0; j < H; j++)
            for (int i = 0; i < W; i++)
                if (~j & ~(i ^ (j >> 1)) & 1)
                    px(i, j, primary);
        break;
    case 2:
        for (int j = 0; j < H; j++)
            for (int i = 0; i < W; i++)
                if (~j & ~i & 1)
                    px(i, j, primary);
        break;
    case 3:
        for (int j = 0; j < H; j++)
            for (int i = 0; i < W; i++)
                px(i, j, primary);
        break;
    case 4:
        for (int j = 0; j < H; j++)
            for (int i = 0; i < W; i++) {
                int m = ((i - j) % 4 + 4) % 4;
                px(i, j, m == 0 ? primary : m == 1 ? secondary : 0x202020);
            }
        break;
    }
    switch (t.id) {
    case WL_EWALL:
        for (int j = 0; j < H; j++)
            for (int i = 0; i < W; i++)
                if ((i < W / 4 + j) != bool(i & j & 1))
                    px(i, j, primary);
        break;
    case WL_WALLELEC:
        for (int j = 0; j < H; j++)
            for (int i = 0; i < W; i++)
                px(i, j, (~j & ~i & 1) ? primary : 0x808080);
        break;
    case WL_EHOLE:
    case WL_STASIS:
        for (int j = 0; j < H; j++)
            for (int i = 0; i < W; i++)
                if ((i < W / 4 + j) == bool(i & j & 1))
                    px(i, j, primary);
        break;
    case WL_ERASE:
        for (int j = 0; j < H; j++)
            for (int i = 0; i < W; i++)
                if ((i < W / 2) ? (~j & ~(i ^ (j >> 1)) & 1) : 1)
                    px(i, j, primary);
        c.blendChar(x + W / 2 - 4, y + H / 2 - 2, 0xE06C, rgb(0xFF0000));
        break;
    case WL_ERASEALL: {
        int r = 100, g = 150, b = 50, rd = 1, gd = -1, bd = -1;
        for (int i = 0; i < W; i++) {
            r += 15 * rd;
            g += 15 * gd;
            b += 15 * bd;
            if (r > 200) rd = -1;
            if (g > 200) gd = -1;
            if (b > 200) bd = -1;
            if (r < 15) rd = 1;
            if (g < 15) gd = 1;
            if (b < 15) bd = 1;
            uint32_t col = (uint32_t(std::clamp(r, 0, 150)) << 16) | (uint32_t(std::clamp(g, 0, 200)) << 8) |
                           uint32_t(std::clamp(b, 0, 200));
            c.drawLine(x + i, y, x + i, y + H - 1, col);
        }
        c.blendChar(x + W / 2 - 10, y + H / 2 - 2, 0xE06C, rgb(0xFF0000));
        c.blendChar(x + W / 2 + 1, y + H / 2 - 2, 0xE06C, rgb(0xFF0000));
        break;
    }
    case WL_STREAM:
        c.drawRect(x, y, W, H, 0xA0A0A0);
        c.addChar(x + 4, y + 2, 0xE00D, rgb(0xFFFFFF));
        c.addChar(x + 8, y + 2, 0xE06D, rgb(0xFFFFFF));
        break;
    }
}

// ToolButton::Draw
void GameUi::drawToolButton(Canvas &c, const UiTool &t, int x, int y, int index) const {
    const int W = 30, H = 18;
    bool textured = t.kind == UiTool::Kind::Wall || (t.kind == UiTool::Kind::Element && t.id == 0);
    if (textured)
        drawToolTexture(c, t, x + 2, y + 2);
    else
        c.blendFilledRect(x + 2, y + 2, W - 4, H - 4, rgb(t.colour));
    int selection = index == active_[0] ? 0 : index == active_[1] ? 1 : index == active_[2] ? 2 : -1;
    static const uint32_t selected[3] = {0xFF0000, 0x0000FF, 0x00FF00};
    bool inside = mouseX_ >= x && mouseY_ >= y && mouseX_ < x + W && mouseY_ < y + H;
    uint32_t border = selection >= 0 ? selected[selection] : 0x000000;
    if (inside && selection == -1)
        border = 0xFF0000; // (Appearance.BorderActive)
    c.blendRect(x, y, W, H, rgb(border));
    if (textured)
        return;
    std::u32string text = t.name.substr(0, 7);
    int tw, th;
    c.textSize(text, tw, th);
    th -= 3;
    tw -= 1;
    int tx = 4 + ((W - 8) - tw) / 2, ty = 1 + ((H - 2) - th) / 2; // centred, Margin(1, 4)
    int total = int(t.colour & 0xFF) + 3 * int((t.colour >> 8) & 0xFF) + 2 * int(t.colour >> 16);
    c.blendText(x + tx, y + ty, text, rgb(total < 544 ? 0xFFFFFF : 0x000000));
}

// GameView's sample text: BasicParticleInfo, Temp, Pressure (the debug text comes with the debug HUD)
std::u32string GameUi::sampleText(const UiSample &s) const {
    auto name = [&](int t) { return t >= 0 && t < MAX_ELEMENTS ? utf32(elements_.elements[t].name) : U"Empty"; };
    auto isElement = [&](int t) { return t > 0 && t < MAX_ELEMENTS && elements_.elements[t].existsInTpt; };
    if (s.hasPart) {
        std::u32string info;
        if (s.type == PT_LAVA && isElement(s.ctype))
            info = U"Molten " + name(s.ctype);
        else if ((s.type == PT_PIPE || s.type == PT_PPIP) && isElement(s.ctype))
            info = s.ctype == PT_LAVA && isElement(s.tmp4) ? name(s.type) + U" with molten " + name(s.tmp4)
                                                             : name(s.type) + U" with " + name(s.ctype);
        else if (s.type == PT_LIFE)
            info = s.ctype >= 0 && s.ctype < int(elements_.golNames.size()) ? utf32(elements_.golNames[size_t(s.ctype)])
                                                                           : name(s.type);
        else
            info = name(s.type);
        return info + U", Temp: " + fixed2(s.temp - 273.15f) + U"C, Pressure: " + fixed2(s.pressure);
    }
    if (s.wall > 0 && s.wall < int(elements_.walls.size()))
        return utf32(elements_.walls[size_t(s.wall)].name) + U", Pressure: " + fixed2(s.pressure);
    if (s.inWorld)
        return U"Empty, Pressure: " + fixed2(s.pressure);
    return U"Empty";
}

void GameUi::draw(Canvas &c, const UiState &st) {
    c.resize(w_, h_);
    const int XRES = viewW(), YRES = viewH();
    // the bars' background (TPT's window is black where the simulation isn't)
    c.drawFilledRect(XRES, 0, w_ - XRES, h_, 0x000000);
    c.drawFilledRect(0, YRES, XRES, h_ - YRES, 0x000000);

    // GameView::OnDraw: the text about the simulation
    {
        int alpha = 255;
        if (toolTipY_ < 120)
            alpha -= int(toolTipPresence_) * 3;
        alpha = std::max(alpha, 0);
        std::u32string info = sampleText(st.sample);
        int tw, th;
        c.textSize(info, tw, th);
        tw -= 1;
        c.blendFilledRect(XRES - 20 - tw, 12, tw + 8, 15, rgb(0x000000, uint8_t(alpha * 0.5f)));
        c.blendText(XRES - 16 - tw, 16, info, rgb(0xFFFFFF, uint8_t(alpha * 0.75f)));
    }
    {
        std::u32string fps = U"FPS: " + fixed2(float(st.fps));
        int tw, th;
        c.textSize(fps, tw, th);
        c.blendFilledRect(12, 12, tw - 1 + 8, th + 5, rgb(0x000000, 127));
        c.blendText(16, 16, fps, rgb(0x20D8FF, 191));
    }
    if (toolTipPresence_ > 0 && toolTipX_ != -1 && !toolTip_.empty()) {
        uint8_t a = uint8_t(toolTipPresence_ > 51 ? 255 : int(toolTipPresence_) * 5);
        if (toolTipY_ == h_ - MENUSIZE - 10) { // BlendTextOutline
            for (int oy : {-1, 1})
                for (int ox : {-1, 1})
                    c.blendText(toolTipX_ + ox, toolTipY_ + oy, toolTip_, rgb(0x000000, 0x78));
        }
        c.blendText(toolTipX_, toolTipY_, toolTip_, rgb(0xFFFFFF, a));
    }
    if (buttonTipShow_ > 0)
        c.blendText(16, h_ - MENUSIZE - 24, buttonTip_, rgb(0xFFFFFF, uint8_t(buttonTipShow_ > 51 ? 255 : int(buttonTipShow_) * 5)));

    // The palette, clipped to (1, YRES + 1, XRES - 1, 18), and its scroll bar
    c.setClip(1, YRES + 1, XRES - 1, 18);
    for (size_t k = 0; k < paletteTools_.size(); k++) {
        int x = paletteX0_ - 31 * int(k);
        if (x + 30 < 1 || x > XRES)
            continue;
        drawToolButton(c, tools_[size_t(paletteTools_[k])], x, YRES + 1, paletteTools_[k]);
    }
    c.resetClip();
    if (scrollVisible_) {
        Button sb;
        sb.x = scrollX_, sb.y = YRES + 21, sb.w = scrollW_, sb.h = 2;
        drawButton(c, sb, false);
    }

    for (const Button &b : buttons_) {
        bool toggled = (b.section >= 0 && b.section == section_) ||
                       (b.action == UiAction::Pause && st.paused) ||
                       (b.action == UiAction::GravityGrid && st.gravityGrid) ||
                       (b.action == UiAction::Decorations && st.decorations) ||
                       (b.action == UiAction::Newtonian && st.newtonian) ||
                       (b.action == UiAction::AmbientHeat && st.ambientHeat);
        Button shown = b;
        if (b.action == UiAction::Save)
            shown.text = st.saveName;
        drawButton(c, shown, toggled);
    }
}

} // namespace bp
