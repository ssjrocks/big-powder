// TPT's game screen (gui/game/GameView) over the zoomable world: the menu sections on the right, the tool palette and
// the buttons along the bottom, the quick options, tooltips and the text about the simulation, laid out and drawn
// as TPT does them, in TPT's pixels (the window divided by the interface scale).
#pragma once
#include "classic/TptTools.h"
#include "ui/Canvas.h"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace bp {

struct ElementTable;

// A tool in TPT's menus (gui/game/Tool and its kinds)
struct UiTool {
    // Elements, Game of Life rulesets (built in, or custom: id the rule), walls, the simulation tools (SimTool), the
    // property and sample tools, the decoration tools (DecorationTool) and the custom Game of Life tool (GOLTool);
    // None for a tool removed (GameModel::FreeTool leaves its place empty)
    enum class Kind { Element, Life, Wall, Sim, Property, Sample, Deco, AddLife, Sign, None };
    Kind kind = Kind::Element;
    int id = 0; // element id, Game of Life ruleset, wall id, SimToolId or DecoMode
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
    bool replaceMode = false, specificDelete = false;
    std::u32string saveName = U"[untitled simulation]";
    UiSample sample;
};

enum class UiAction {
    None, Open, Reload, Save, Clear, SimulationOptions, RenderOptions, Pause, GravityGrid, Decorations, Newtonian,
    AmbientHeat, ElementSearch, ColourPicker, RemoveCustomGol,
};
constexpr int SC_TOOL = 13, SC_DECO = 15; // TPT's menu sections (MenuSection.h)

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
    // The modifiers change what choosing a tool does (ctrl+alt: the element replace mode works on).
    void setModifiers(bool shift, bool ctrl, bool alt) { shift_ = shift, ctrl_ = ctrl, alt_ = alt; }
    bool mouseMove(int x, int y, bool anyButtonDown);
    bool mouseDown(int x, int y, int button);
    bool mouseUp(int x, int y, int button);
    UiAction takeAction();

    const std::vector<UiTool> &tools() const { return tools_; }
    int toolIndex(const std::string &identifier) const; // -1 if there is none
    // The tools on the left, right and middle buttons (slots 0-2) and the one replace mode works on (3)
    const UiTool *activeTool(int slot) const;
    int activeToolIndex(int slot) const { return slot >= 0 && slot < 4 ? slots()[slot] : -1; }
    void setActiveTool(int slot, const std::string &identifier);
    // GameController::SetActiveTool: the deco menu's middle button is the left one, the tool becomes the last tool, and
    // it is told it was chosen (takeChosen)
    void chooseTool(int slot, int index);
    // A tool chosen since last asked (its index and slot), for those that do something when chosen: the property
    // tool opens its window
    bool takeChosen(int &index, int &slot);
    // GameModel::SetActiveTool: only that (the sample tool picks tools this way)
    void setActiveToolIndex(int slot, int index) {
        if (slot >= 0 && slot < 4 && index >= 0 && index < int(tools_.size()))
            slots()[slot] = index;
    }
    // The tool's 26 x 14 texture, for those that have one (walls, NONE and the sample tool; the decoration tools in the
    // palette, where TPT makes theirs from the colour; others are their colour)
    bool hasTexture(const UiTool &t, bool palette = false) const;
    void drawToolTexture(Canvas &c, const UiTool &t, int x, int y) const;
    // Custom Game of Life types (GameModel::AllocCustomGolTool, RemoveCustomGol's FreeTool): a tool in the Life menu,
    // DEFAULT_PT_LIFECUST_ and its name
    void addCustomGolTool(const CustomGol &type);
    void removeTool(const std::string &identifier);
    // The custom type a ctrl+shift+right click asked to remove (with UiAction::RemoveCustomGol)
    const std::string &removeRequest() const { return removeRequest_; }
    // GameView::ToolTip from the bottom (the text above the bottom bar): a link sign's
    void showButtonTip(const std::u32string &text) {
        buttonTip_ = text;
        buttonTipIn_ = true;
    }
    // The menu section's name (what element search also matches), "" if none
    std::u32string sectionName(int section) const;
    // GameController::SetActiveMenu: the deco menu has its own tools on the mouse buttons and shows the colour
    // selector (the colour presets and the colour picker's button)
    void setActiveMenu(int section);
    int lastMenu() const { return lastMenu_; } // the last menu that isn't the deco menu
    bool colourSelectorVisible() const { return section_ == SC_DECO; }
    // The decoration tools' colour (0xAARRGGBB): GameModel::SetColourSelectorColour, and GameController::SetColour,
    // which also changes the colour preset chosen
    uint32_t decoColour() const { return decoColour_; }
    void setColourSelectorColour(uint32_t c) { decoColour_ = c; }
    void setColour(uint32_t c);
    // GameModel's last tool: the last one chosen or drawn with (it decides how the brush shows)
    const UiTool *lastTool() const { return last_ >= 0 ? &tools_[size_t(last_)] : nullptr; }
    void setLastTool(int slot) {
        if (slot >= 0 && slot < 4 && slots()[slot] >= 0)
            last_ = slots()[slot];
    }
    int activeSection() const { return section_; }
    // Where a menu section's button is (its middle), false if it isn't shown; and a palette button's for a tool
    bool sectionButton(int section, int &x, int &y) const;
    bool paletteButton(int tool, int &x, int &y) const;

    void tick(double ms); // tooltips fade in and out
    void draw(Canvas &c, const UiState &state);

private:
    struct Button {
        int x = 0, y = 0, w = 0, h = 0;
        std::u32string text, tip;
        Icon icon = Icon::None;
        int marginLeft = 4, marginRight = 4, marginTop = 1, marginBottom = 1;
        bool alignLeft = false, togglable = false, enabled = true;
        std::optional<uint32_t> colour; // its background, 0xAARRGGBB (the colour picker's button)
        UiAction action = UiAction::None;
        int section = -1; // a menu section's button
        bool contains(int px, int py) const { return px >= x && py >= y && px < x + w && py < y + h; }
    };
    void layout();
    void drawButton(Canvas &c, const Button &b, bool toggled) const;
    void drawToolButton(Canvas &c, const UiTool &t, int x, int y, int toolIndex) const;
    std::vector<int> sectionTools(int section) const;
    void updateScroll();
    int toolAt(int x, int y) const;
    void showTip(const Button &b);
    void showToolTip(const std::u32string &text, int y, bool fromBar);
    std::u32string sampleText(const UiSample &s) const;

    const ElementTable &elements_;
    std::vector<UiTool> tools_;
    int *slots() { return section_ == SC_DECO ? deco_ : regular_; }
    const int *slots() const { return section_ == SC_DECO ? deco_ : regular_; }
    void clickPreset(int preset); // GameController::SetActiveColourPreset, then SetColour
    int presetAt(int x, int y) const;
    bool overColourPicker(int x, int y) const;
    // GameModel's two toolsets: the regular one and the deco menu's
    int regular_[4] = {-1, -1, -1, -1}, deco_[4] = {-1, -1, -1, -1};
    int last_ = -1;
    int lastMenu_ = 8;
    uint32_t decoColour_ = 0xFFC86432; // (TPT's Decoration.Red, Green, Blue, Alpha: 200, 100, 50, 255)
    uint32_t presets_[8] = {0xFFFFFFFF, 0xFF00FFFF, 0xFFFF00FF, 0xFFFFFF00, 0xFFFF0000, 0xFF00FF00, 0xFF0000FF, 0xFF000000};
    int activePreset_ = 0; // (GameModel's activeColourPreset: the preset + 1, 0 for none)
    int pressedPreset_ = -1;
    std::string removeRequest_;
    void sanitizeToolsets(); // GameModel::SanitizeToolsets
    bool pressedPicker_ = false;
    int chosen_ = -1, chosenSlot_ = 0;
    bool shift_ = false, ctrl_ = false, alt_ = false;
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
