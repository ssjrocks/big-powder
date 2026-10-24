#include "ui/GameUi.h"
#include "sim/Elements.h"
#include "sim/RenderTables.h"
#include "classic/TptTools.h"

#include <algorithm>
#include <cstdio>
#include <optional>

namespace bp {

namespace {

constexpr int SC_WALL = 0, SC_LIFE = 12;

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
    // The simulation tools, then the property and sample tools (the decoration and sign tools come with their own)
    {
        std::vector<SimToolInfo> sims = tptSimTools();
        for (size_t i = 0; i < sims.size(); i++) {
            UiTool t;
            t.kind = UiTool::Kind::Sim;
            t.id = int(i);
            t.name = utf32(sims[i].name);
            t.description = utf32(sims[i].description);
            t.colour = sims[i].colour;
            t.identifier = sims[i].identifier;
            t.section = SC_TOOL;
            tools_.push_back(t);
        }
        // GameModel::InitTools' decoration tools, in its order
        static const struct {
            int mode;
            const char32_t *name, *description;
            const char *identifier;
        } decos[] = {
            {DECO_ADD, U"ADD", U"Colour blending: Add.", "DEFAULT_DECOR_ADD"},
            {DECO_SUBTRACT, U"SUB", U"Colour blending: Subtract.", "DEFAULT_DECOR_SUB"},
            {DECO_MULTIPLY, U"MUL", U"Colour blending: Multiply.", "DEFAULT_DECOR_MUL"},
            {DECO_DIVIDE, U"DIV", U"Colour blending: Divide.", "DEFAULT_DECOR_DIV"},
            {DECO_SMUDGE, U"SMDG", U"Smudge tool, blends surrounding deco together.", "DEFAULT_DECOR_SMDG"},
            {DECO_CLEAR, U"CLR", U"Erase any set decoration.", "DEFAULT_DECOR_CLR"},
            {DECO_DRAW, U"SET", U"Draw decoration (No blending).", "DEFAULT_DECOR_SET"},
        };
        for (const auto &d : decos) {
            UiTool t;
            t.kind = UiTool::Kind::Deco;
            t.id = d.mode;
            t.name = d.name;
            t.description = d.description;
            t.identifier = d.identifier;
            t.section = SC_DECO;
            tools_.push_back(t);
        }
        UiTool prop;
        prop.kind = UiTool::Kind::Property;
        prop.name = U"PROP";
        prop.description = U"Property Drawing Tool. Use to alter the properties of elements in the field.";
        prop.colour = 0xFEA900;
        prop.identifier = "DEFAULT_UI_PROPERTY";
        prop.section = SC_TOOL;
        tools_.push_back(prop);
        UiTool sign;
        sign.kind = UiTool::Kind::Sign;
        sign.name = U"SIGN";
        sign.description = U"Sign. Displays text. Click on a sign to edit it or anywhere else to place a new one.";
        sign.identifier = "DEFAULT_UI_SIGN";
        sign.section = SC_TOOL;
        tools_.push_back(sign);
        UiTool sample;
        sample.kind = UiTool::Kind::Sample;
        sample.name = U"SMPL";
        sample.description = U"Sample an element on the screen.";
        sample.identifier = "DEFAULT_UI_SAMPLE";
        sample.section = SC_TOOL;
        tools_.push_back(sample);
        UiTool gol;
        gol.kind = UiTool::Kind::AddLife;
        gol.name = U"CUST";
        gol.description = U"Add a new custom GOL type. (Use ctrl+shift+rightclick to remove them)";
        gol.colour = 0xFEA900;
        gol.identifier = "DEFAULT_UI_ADDLIFE";
        gol.section = SC_LIFE;
        tools_.push_back(gol);
    }
    sanitizeToolsets();
    last_ = regular_[0];
}

// GameModel::SanitizeToolsets: empty slots get DUST on the left button, NONE (the eraser) on the right, SMPL on the
// middle one and NONE for replace mode; in the deco menu SET, CLR, SMPL and NONE
void GameUi::sanitizeToolsets() {
    static const char *regular[4] = {"DEFAULT_PT_DUST", "DEFAULT_PT_NONE", "DEFAULT_UI_SAMPLE", "DEFAULT_PT_NONE"};
    static const char *deco[4] = {"DEFAULT_DECOR_SET", "DEFAULT_DECOR_CLR", "DEFAULT_UI_SAMPLE", "DEFAULT_PT_NONE"};
    for (int k = 0; k < 4; k++) {
        if (regular_[k] < 0)
            regular_[k] = toolIndex(regular[k]);
        if (deco_[k] < 0)
            deco_[k] = toolIndex(deco[k]);
    }
    if (last_ < 0)
        last_ = slots()[0];
}

void GameUi::addCustomGolTool(const CustomGol &type) {
    UiTool t;
    t.kind = UiTool::Kind::Life;
    t.id = type.rule;
    t.name = type.name;
    t.description = U"Custom GOL type: " + tptGolRuleString(type.rule);
    t.colour = type.colour1 & 0xFFFFFF;
    t.identifier = "DEFAULT_PT_LIFECUST_" + utf8(type.name);
    t.section = SC_LIFE;
    // GameModel::AllocTool: the first empty place, else a new one
    auto it = std::find_if(tools_.begin(), tools_.end(), [](const UiTool &u) { return u.kind == UiTool::Kind::None; });
    if (it != tools_.end())
        *it = t;
    else
        tools_.push_back(t);
    paletteTools_ = sectionTools(section_);
    layout();
}

void GameUi::removeTool(const std::string &identifier) {
    int i = toolIndex(identifier);
    if (i < 0)
        return;
    // GameModel::DeselectTool, then an empty place
    for (int k = 0; k < 4; k++) {
        if (regular_[k] == i)
            regular_[k] = -1;
        if (deco_[k] == i)
            deco_[k] = -1;
    }
    if (last_ == i)
        last_ = -1;
    tools_[size_t(i)] = UiTool{};
    tools_[size_t(i)].kind = UiTool::Kind::None;
    tools_[size_t(i)].section = -1;
    sanitizeToolsets();
    paletteTools_ = sectionTools(section_);
    layout();
}

int GameUi::toolIndex(const std::string &identifier) const {
    for (size_t i = 0; i < tools_.size(); i++)
        if (tools_[i].identifier == identifier)
            return int(i);
    return -1;
}

void GameUi::setActiveTool(int slot, const std::string &identifier) {
    int i = toolIndex(identifier);
    if (i >= 0 && slot >= 0 && slot < 4)
        slots()[slot] = i;
}

void GameUi::setActiveMenu(int section) {
    bool wasDeco = section_ == SC_DECO;
    section_ = section;
    if (section != SC_DECO)
        lastMenu_ = section;
    paletteTools_ = sectionTools(section_);
    updateScroll();
    if (section == SC_DECO && !wasDeco) {
        // GameView::NotifyColourSelectorVisibilityChanged's SetActiveColourPreset(-1): no preset chosen, or, if none
        // was, SET on the left button (TPT's size_t arithmetic)
        if (activePreset_ == 0)
            deco_[0] = toolIndex("DEFAULT_DECOR_SET");
        activePreset_ = 0;
    }
}

bool GameUi::sectionButton(int section, int &x, int &y) const {
    for (const Button &b : buttons_)
        if (b.section == section) {
            x = b.x + b.w / 2, y = b.y + b.h / 2;
            return true;
        }
    return false;
}

bool GameUi::paletteButton(int tool, int &x, int &y) const {
    for (size_t k = 0; k < paletteTools_.size(); k++)
        if (paletteTools_[k] == tool) {
            x = paletteX0_ - 31 * int(k) + 15, y = viewH() + 1 + 9;
            return true;
        }
    return false;
}

void GameUi::setColour(uint32_t c) {
    decoColour_ = c;
    if (activePreset_ > 0 && activePreset_ <= 8) // (GameModel::SetPresetColour)
        presets_[activePreset_ - 1] = c;
}

void GameUi::clickPreset(int preset) {
    if (activePreset_ - 1 != preset)
        activePreset_ = preset + 1;
    else
        deco_[0] = toolIndex("DEFAULT_DECOR_SET");
    setColour(presets_[preset]);
}

// The colour presets along the bottom left and the colour picker's button in the middle (GameView), in the deco menu
int GameUi::presetAt(int x, int y) const {
    if (!colourSelectorVisible() || y < viewH() + 1 || y >= viewH() + 19)
        return -1;
    for (int i = 0; i < 8; i++)
        if (x >= 5 + 31 * i && x < 5 + 31 * i + 30)
            return i;
    return -1;
}

bool GameUi::overColourPicker(int x, int y) const {
    int px = viewW() / 2 - 8, py = viewH() + 1;
    return colourSelectorVisible() && x >= px && y >= py && x < px + 16 && y < py + 16;
}

void GameUi::chooseTool(int slot, int index) {
    if (index < 0 || index >= int(tools_.size()))
        return;
    if (section_ == SC_DECO && slot == 2)
        slot = 0;
    if (slot < 0 || slot > 3)
        return;
    slots()[slot] = index;
    last_ = index;
    chosen_ = index;
    chosenSlot_ = slot;
}

bool GameUi::takeChosen(int &index, int &slot) {
    if (chosen_ < 0)
        return false;
    index = chosen_;
    slot = chosenSlot_;
    chosen_ = -1;
    return true;
}

std::u32string GameUi::sectionName(int section) const {
    if (section < 0 || section >= int(elements_.menuSections.size()))
        return U"";
    return utf32(elements_.menuSections[size_t(section)].name);
}

const UiTool *GameUi::activeTool(int slot) const {
    return slot >= 0 && slot < 4 && slots()[slot] >= 0 ? &tools_[size_t(slots()[slot])] : nullptr;
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
    {
        Button &search = add(W - 16, H - 32, 15, 15, Icon::None, U"Search for elements", UiAction::ElementSearch);
        search.text = U"\U0000E065";
        search.marginLeft = 2, search.marginRight = 2, search.marginTop = 0, search.marginBottom = 3;
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
            if (b.section >= 0 && b.section != SC_DECO && !anyButtonDown && b.section != section_)
                setActiveMenu(b.section);
            return true;
        }
    // (the colour presets and the colour picker's button lie over the palette)
    if (presetAt(x, y) >= 0)
        showToolTip(U"Decoration Presets.", 0, false);
    else if (overColourPicker(x, y))
        showToolTip(U"Pick Colour", 0, false);
    else if (int t = toolAt(x, y); t >= 0)
        showToolTip(tools_[size_t(t)].description, 0, false);
    return !overWorld(x, y);
}

bool GameUi::mouseDown(int x, int y, int button) {
    pressed_ = button;
    pressedPreset_ = presetAt(x, y);
    pressedPicker_ = overColourPicker(x, y);
    pressedTool_ = pressedPreset_ >= 0 || pressedPicker_ ? -1 : toolAt(x, y);
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
    // ToolButton::OnMouseClick: the button released over the tool it was pressed on chooses it for that mouse button;
    // with ctrl+alt an element becomes what replace mode works on (GameView's tool button action). (Shift+ctrl adds
    // favourites, which come later.)
    // a colour preset (ToolButton: any button), the colour picker's button (the left one)
    if (pressedPreset_ >= 0 && presetAt(x, y) == pressedPreset_ && button >= 0 && button < 3)
        clickPreset(pressedPreset_);
    if (pressedPicker_ && overColourPicker(x, y) && button == 0)
        action_ = UiAction::ColourPicker;
    int t = pressedPreset_ >= 0 || pressedPicker_ ? -1 : toolAt(x, y);
    // ctrl+shift+right click on a custom Game of Life type asks to remove it (with the left button, favourites come
    // with their menu)
    if (t >= 0 && t == pressedTool_ && button == 1 && shift_ && ctrl_ && !alt_ &&
        tools_[size_t(t)].identifier.rfind("DEFAULT_PT_LIFECUST_", 0) == 0) {
        removeRequest_ = tools_[size_t(t)].identifier;
        action_ = UiAction::RemoveCustomGol;
    }
    if (t >= 0 && t == pressedTool_ && button >= 0 && button < 3 && !(shift_ && ctrl_ && !alt_)) {
        int slot = button;
        if (ctrl_ && alt_ && !shift_ && tools_[size_t(t)].identifier.find("_PT_") != std::string::npos)
            slot = 3;
        chooseTool(slot, t);
    }
    if (pressedButton_ >= 0 && buttons_[size_t(pressedButton_)].contains(x, y)) {
        const Button &b = buttons_[size_t(pressedButton_)];
        if (b.section >= 0)
            setActiveMenu(b.section);
        else if (b.enabled && button == 0)
            action_ = b.action;
    }
    pressed_ = pressedTool_ = pressedButton_ = pressedPreset_ = -1;
    pressedPicker_ = false;
    return inside;
}

UiAction GameUi::takeAction() {
    UiAction a = action_;
    action_ = UiAction::None;
    return a;
}

void GameUi::tick(double ms) {
    // ui::Fade's linear profile: 120 a second up, 60 down
    toolTipPresence_ = toolTipIn_ ? std::min(120.0, toolTipPresence_ + ms * 0.12) : std::max(0.0, toolTipPresence_ - ms * 0.06);
    buttonTipShow_ = buttonTipIn_ ? std::min(120.0, buttonTipShow_ + ms * 0.12) : std::max(0.0, buttonTipShow_ - ms * 0.06);
    toolTipIn_ = buttonTipIn_ = false;
}

// ui::Button::Draw with ui::Appearance's colours
void GameUi::drawButton(Canvas &c, const Button &b, bool toggled) const {
    bool inside = b.contains(mouseX_, mouseY_);
    bool down = pressed_ >= 0 && pressedButton_ >= 0 && &buttons_[size_t(pressedButton_)] == &b && inside;
    uint32_t text = 0xFFFFFF, border = 0xC8C8C8, bg = 0x000000;
    uint8_t bgAlpha = 255;
    if (!b.enabled) {
        text = 0x646464, border = 0x646464, bg = 0x0A0A0A;
    } else if (down || (b.togglable && toggled)) {
        text = 0x000000, border = 0xEBEBEB, bg = 0xFFFFFF;
    } else if (inside) {
        text = 0xFFFFFF, border = 0xFFFFFF, bg = 0x141414;
        if (b.colour)
            bg = *b.colour & 0xFFFFFF, bgAlpha = uint8_t(*b.colour >> 24); // (its BackgroundHover is the colour too)
    } else if (b.colour)
        bg = *b.colour & 0xFFFFFF, bgAlpha = uint8_t(*b.colour >> 24);
    c.blendFilledRect(b.x + 1, b.y + 1, b.w - 2, b.h - 2, rgb(bg, bgAlpha));
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

bool GameUi::hasTexture(const UiTool &t, bool palette) const {
    return t.kind == UiTool::Kind::Wall || t.kind == UiTool::Kind::Sample || t.kind == UiTool::Kind::Sign ||
           (t.kind == UiTool::Kind::Element && t.id == 0) || (palette && t.kind == UiTool::Kind::Deco);
}

// The tools' textures (Renderer::WallIcon; NONE's icon; SampleTool::GetIcon; DecorationTool::GetIcon)
void GameUi::drawToolTexture(Canvas &c, const UiTool &t, int x, int y) const {
    const int W = 26, H = 14;
    c.drawFilledRect(x, y, W, H, 0x000000); // (a texture starts black)
    if (t.kind == UiTool::Kind::Deco) {
        const uint32_t colour = decoColour_ & 0xFFFFFF;
        const uint8_t alpha = uint8_t(decoColour_ >> 24);
        if (t.id == DECO_SMUDGE) {
            for (int j = 0; j < H; j++)
                for (int i = 0; i < W; i++)
                    c.drawPixel(x + i, y + j, (uint32_t(0xFF - 5 * i) << 8) | uint32_t(5 * i));
        } else if (t.id == DECO_DRAW || t.id == DECO_CLEAR)
            c.blendFilledRect(x, y, W, H, rgb(colour, alpha));
        else
            c.drawFilledRect(x, y, W, H, 0x323232);
        if (t.id == DECO_CLEAR) {
            uint32_t reverse = (((colour >> 16) + 127) & 0xFF) << 16 | ((((colour >> 8) & 0xFF) + 127) & 0xFF) << 8 |
                               (((colour & 0xFF) + 127) & 0xFF);
            c.blendChar(x + W / 2 - 4, y + H / 2 - 2, 0xE06C, rgb(reverse));
        } else if (t.id == DECO_ADD)
            c.addChar(x + 11, y + 4, U'+', rgb(colour));
        else if (t.id == DECO_SUBTRACT)
            c.addChar(x + 11, y + 4, U'-', rgb(colour));
        else if (t.id == DECO_MULTIPLY)
            c.addChar(x + 11, y + 3, U'x', rgb(colour));
        else if (t.id == DECO_DIVIDE)
            c.addChar(x + 11, y + 4, U'/', rgb(colour));
        return;
    }
    if (t.kind == UiTool::Kind::Sign) { // SignTool::GetIcon
        c.drawRect(x, y, W, H, 0xA0A0A0);
        c.blendChar(x + W / 2 - 5, y + H / 2 - 5, 0xE021, rgb(0x204080));
        c.blendChar(x + W / 2 - 5, y + H / 2 - 5, 0xE020, rgb(0xFFFFFF));
        return;
    }
    if (t.kind == UiTool::Kind::Sample) {
        c.drawRect(x, y, W, H, 0xA0A0A0);
        c.blendChar(x + W / 2 - 5, y + H / 2 - 5, 0xE066, rgb(0xFFFFFF));
        c.blendChar(x + W / 2 - 5, y + H / 2 - 5, 0xE06B, rgb(0x64B4FF));
        return;
    }
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
    bool textured = hasTexture(t, true);
    if (textured)
        drawToolTexture(c, t, x + 2, y + 2);
    else
        c.blendFilledRect(x + 2, y + 2, W - 4, H - 4, rgb(t.colour));
    const int *active = slots();
    int selection = index == active[0]   ? 0
                    : index == active[1] ? 1
                    : index == active[2] ? 2
                    : index == active[3] ? 3
                                          : -1;
    static const uint32_t selected[4] = {0xFF0000, 0x0000FF, 0x00FF00, 0x00FFFF};
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
        if (st.replaceMode)
            fps += U" [REPLACE MODE]";
        if (st.specificDelete)
            fps += U" [SPECIFIC DELETE]";
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
    // The deco menu's colour presets (ToolButtons) and the colour picker's button, over the palette
    if (colourSelectorVisible()) {
        for (int i = 0; i < 8; i++) {
            const int x = 5 + 31 * i, y = YRES + 1, W = 30, H = 18;
            c.blendFilledRect(x + 2, y + 2, W - 4, H - 4, rgb(presets_[i] & 0xFFFFFF, uint8_t(presets_[i] >> 24)));
            bool selected = activePreset_ - 1 == i, inside = presetAt(mouseX_, mouseY_) == i;
            c.blendRect(x, y, W, H, rgb(selected || inside ? 0xFF0000 : 0x000000));
        }
        Button picker;
        picker.x = XRES / 2 - 8, picker.y = YRES + 1, picker.w = 16, picker.h = 16;
        picker.colour = decoColour_;
        drawButton(c, picker, false);
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
