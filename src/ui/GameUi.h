// TPT's game screen (gui/game/GameView) over the zoomable world: the menu sections on the right, the tool palette and
// the buttons along the bottom, the quick options, tooltips and the text about the simulation, laid out and drawn
// as TPT does them, in TPT's pixels (the window divided by the interface scale).
#pragma once
#include "ui/Canvas.h"

#include <cstdint>
#include <string>
#include <vector>

namespace bp {

struct ElementTable;

// A tool in TPT's menus (gui/game/Tool and its kinds)
struct UiTool {
    enum class Kind { Element, Life, Wall };
    Kind kind = Kind::Element;
    int id = 0; // element id, Game of Life ruleset, or wall id
    std::u32string name, description;
    uint32_t colour = 0; // 0xRRGGBB
    std::string identifier;
    int section = 0;
    // What a brush stroke creates: the element, with a Game of Life ruleset above PMAPBITS (as TPT's PMAP(v, t))
    int brushType() const { return kind == Kind::Life ? (id << 9) | 78 /* PT_LIFE */ : id; }
};

// What the world reports for the text at the top right (GameView's sample)
struct UiSample {
    bool inWorld = false, hasPart = false;
    int type = 0, ctype = 0, life = 0, tmp = 0, tmp2 = 0, tmp4 = 0, wall = 0;
    float temp = 0.0f, pressure = 0.0f;
};

struct UiState {
    double fps = 0.0;
    bool paused = false, gravityGrid = false, decorations = true, newtonian = false, ambientHeat = false;
    std::u32string saveName = U"[untitled simulation]";
    UiSample sample;
};

enum class UiAction {
    None, Open, Reload, Save, Clear, SimulationOptions, RenderOptions, Pause, GravityGrid, Decorations, Newtonian,
    AmbientHeat, ElementSearch,
};

class GameUi {
public:
    static constexpr int BARSIZE = 17, MENUSIZE = 40; // TPT's right bar and bottom area

    explicit GameUi(const ElementTable &elements);

    void setSize(int w, int h); // in interface pixels
    int width() const { return w_; }
    int height() const { return h_; }
    // The world shows in [0, viewW) x [0, viewH), TPT's XRES x YRES
    int viewW() const { return w_ - BARSIZE; }
    int viewH() const { return h_ - MENUSIZE; }
    bool overWorld(int x, int y) const { return x >= 0 && y >= 0 && x < viewW() && y < viewH(); }

    // Mouse input in interface pixels; button 0 left, 1 right, 2 middle. True when the interface took it.
    bool mouseMove(int x, int y, bool anyButtonDown);
    bool mouseDown(int x, int y, int button);
    bool mouseUp(int x, int y, int button);
    UiAction takeAction();

    const std::vector<UiTool> &tools() const { return tools_; }
    const UiTool *activeTool(int slot) const;
    void setActiveTool(int slot, const std::string &identifier);
    int activeSection() const { return section_; }

    void tick(double ms); // tooltips fade in and out
    void draw(Canvas &c, const UiState &state);

private:
    struct Button {
        int x = 0, y = 0, w = 0, h = 0;
        std::u32string text, tip;
        Icon icon = Icon::None;
        int marginLeft = 4, marginRight = 4, marginTop = 1, marginBottom = 1;
        bool alignLeft = false, togglable = false, enabled = true;
        UiAction action = UiAction::None;
        int section = -1; // a menu section's button
        bool contains(int px, int py) const { return px >= x && py >= y && px < x + w && py < y + h; }
    };
    void layout();
    void drawButton(Canvas &c, const Button &b, bool toggled) const;
    void drawToolButton(Canvas &c, const UiTool &t, int x, int y, int toolIndex) const;
    void drawToolTexture(Canvas &c, const UiTool &t, int x, int y) const;
    std::vector<int> sectionTools(int section) const;
    void updateScroll();
    int toolAt(int x, int y) const;
    void showTip(const Button &b);
    void showToolTip(const std::u32string &text, int y, bool fromBar);
    std::u32string sampleText(const UiSample &s) const;

    const ElementTable &elements_;
    std::vector<UiTool> tools_;
    int active_[3] = {-1, -1, -1};
    int section_ = 8; // SC_POWDERS, as TPT starts
    int w_ = 0, h_ = 0, mouseX_ = -1, mouseY_ = -1;
    std::vector<Button> buttons_;
    int pressed_ = -1, pressedTool_ = -1, pressedButton_ = -1;
    std::vector<int> paletteTools_;
    int paletteX0_ = 0; // the first (rightmost) tool button's x
    bool scrollVisible_ = false;
    int scrollX_ = 0, scrollW_ = 0;
    UiAction action_ = UiAction::None;
    // Tooltips: the element/section one (bottom right or by the bar) and the bottom buttons' one
    std::u32string toolTip_, buttonTip_;
    int toolTipX_ = -1, toolTipY_ = -1;
    bool toolTipIn_ = false, buttonTipIn_ = false;
    double toolTipPresence_ = 0, buttonTipShow_ = 0;
};

} // namespace bp
