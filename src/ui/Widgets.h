// TPT's interface components (gui/interface): windows over the game screen, with labels, buttons, text boxes, drop
// downs, scrolling panels and tool buttons, laid out, drawn and driven as TPT's are, in TPT's pixels on the interface's
// Canvas. Mouse buttons are SDL's (1 left, 2 middle, 3 right) and keys SDL's keycodes and scancodes, as in TPT.
#pragma once
#include "ui/Canvas.h"

#include <algorithm>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace bp::ui {

struct Point {
    int x = 0, y = 0;
};

// What the components need from the app (TPT's ui::Engine and its platform calls): the mouse, the time, the font
// for measuring text, the clipboard
struct Host {
    int mouseX = 0, mouseY = 0;
    double now = 0; // ms
    const Canvas *font = nullptr;
    std::function<std::u32string()> clipboardGet;
    std::function<void(const std::u32string &)> clipboardSet;
};
Host &host();

struct Border {
    int top = 0, right = 0, bottom = 0, left = 0;
    Border(int all) : top(all), right(all), bottom(all), left(all) {}
    Border(int v, int h) : top(v), right(h), bottom(v), left(h) {}
    Border(int t, int r, int b, int l) : top(t), right(r), bottom(b), left(l) {}
    bool all(int v) const { return top == v && right == v && bottom == v && left == v; }
};

// ui::Appearance
struct Appearance {
    enum class HAlign { Left, Centre, Right };
    enum class VAlign { Top, Middle, Bottom };
    HAlign hAlign = HAlign::Centre;
    VAlign vAlign = VAlign::Middle;
    Rgba backgroundHover = rgb(0x141414), backgroundInactive = rgb(0x000000), backgroundActive = rgb(0xFFFFFF),
         backgroundDisabled = rgb(0x0A0A0A);
    Rgba textHover = rgb(0xFFFFFF), textInactive = rgb(0xFFFFFF), textActive = rgb(0x000000),
         textDisabled = rgb(0x646464);
    Rgba borderHover = rgb(0xFFFFFF), borderInactive = rgb(0xC8C8C8), borderActive = rgb(0xEBEBEB),
         borderFavourite = rgb(0xFFFF00), borderDisabled = rgb(0x646464);
    Border margin{1, 4};
    Border border{1};
    Icon icon = Icon::None;
};

// gui/Style's colours
namespace style {
constexpr uint32_t InformationTitle = 0x8CB4FF, WarningTitle = 0xFFD820, ErrorTitle = 0xFF4B4B;
constexpr uint32_t ConfirmButton = 0x7484FF, ActiveBorder = 0xFFFFFF, InactiveBorder = 0x646464;
} // namespace style

class Window;
class Panel;

class Component {
public:
    Component(Point position, Point size) : position(position), size(size) {}
    virtual ~Component() = default;
    Component(const Component &) = delete;
    Component &operator=(const Component &) = delete;

    Point position, size;
    Appearance appearance;
    bool enabled = true, visible = true, doesTextInput = false;
    bool mouseInside = false, mouseDownInside = false;

    Window *parentWindow() const { return window_; }
    Panel *parent() const { return panel_; }
    bool isFocused() const;
    Point containerPos() const;
    Point screenPos() const;

    virtual void draw(Canvas &, Point) {}
    virtual void tick() {}
    virtual void onKeyPress(int, int, bool, bool, bool, bool) {}
    virtual void onKeyRelease(int, int, bool, bool, bool, bool) {}
    virtual void onTextInput(const std::u32string &) {}
    virtual void onMouseClick(int, int, int) {}
    virtual void onMouseDown(int, int, int) {}
    virtual void onMouseHover(int, int) {}
    virtual void onMouseMoved(int, int) {}
    virtual void onMouseEnter(int, int) {}
    virtual void onMouseLeave(int, int) {}
    virtual void onMouseUp(int, int, int) {}
    virtual void onMouseWheel(int, int, int) {}
    virtual void onMouseWheelInside(int, int, int) {}
    virtual void onFocus() {}
    virtual void onDefocus() {}

protected:
    void placeText(const std::u32string &text); // Component::TextPosition
    Point textPos_, textSize_, iconPos_;
    bool drawn_ = false;

private:
    Window *window_ = nullptr;
    Panel *panel_ = nullptr;
    friend class Window;
    friend class Panel;
};

// ui::TextWrapper: lays text out (wrapping it at spaces when asked) and maps between where characters are in the
// text, in the laid-out text (with its added line breaks), in the text without formatting codes, and on the screen
class TextWrapper {
public:
    struct Index {
        int raw = 0, wrapped = 0, clear = 0;
    };
    int update(const std::u32string &text, bool wrap, int maxWidth);
    Index clearToIndex(int clear) const;
    Index pointToIndex(int x, int y) const;
    int indexToPoint(Index index, int &x, int &y) const;
    const std::u32string &wrappedText() const { return wrapped_; }
    int wrappedLines() const { return lines_; }
    Index begin() const { return {0, 0, 0}; }
    Index end() const { return {rawSize_, int(wrapped_.size()), clearSize_}; }
    int wrappedWidth() const { return wrappedWidth_; }

private:
    struct Region {
        int x, y, width, line;
        Index index;
    };
    int rawSize_ = 0, clearSize_ = 0, lines_ = 0, wrappedWidth_ = 0;
    std::u32string wrapped_;
    std::vector<Region> regions_;
};

// ui::Label: text, selectable with the mouse and copied with ctrl+C
class Label : public Component {
public:
    Label(Point position, Point size, const std::u32string &text);
    void setMultiline(bool on);
    virtual void setText(const std::u32string &text);
    virtual std::u32string text() const { return text_; }
    void setTextColour(uint32_t c) { textColour_ = c; }
    bool hasSelection() const { return selH_.raw > selL_.raw; }
    void clearSelection();
    void selectAll();
    void autoHeight();

    void onMouseDown(int x, int y, int button) override;
    void onMouseUp(int x, int y, int button) override;
    void onMouseMoved(int x, int y) override;
    void onKeyPress(int key, int scan, bool repeat, bool shift, bool ctrl, bool alt) override;
    void draw(Canvas &c, Point screenPos) override;
    void tick() override;

protected:
    void updateWrapper();
    void updateSelection();
    void copySelection();
    std::u32string text_, displayText_, displayWithSelection_;
    TextWrapper wrapper_, displayWrapper_;
    uint32_t textColour_ = 0xFFFFFF;
    TextWrapper::Index sel0_, sel1_, selL_, selH_;
    bool multiline_ = false, selecting_ = false, autoHeight_ = false;
    int scrollX_ = 0;
};

// ui::Button
class Button : public Component {
public:
    Button(Point position, Point size, const std::u32string &text = U"", const std::u32string &toolTip = U"");
    std::function<void()> action, altAction, mouseEnterAction;
    void setText(const std::u32string &text);
    const std::u32string &text() const { return text_; }
    void setIcon(Icon icon);
    void setTogglable(bool on) { toggle_ = false, togglable_ = on; }
    bool toggleState() const { return toggle_; }
    void setToggleState(bool on) { toggle_ = on; }
    void doAction();

    void draw(Canvas &c, Point screenPos) override;
    void onMouseClick(int x, int y, int button) override;
    void onMouseUp(int x, int y, int button) override;
    void onMouseDown(int x, int y, int button) override;
    void onMouseEnter(int x, int y) override;
    void onMouseHover(int x, int y) override;
    void onMouseLeave(int x, int y) override;

protected:
    void placeButtonText();
    std::u32string text_, displayText_, toolTip_;
    bool buttonDown_ = false, altButtonDown_ = false, togglable_ = false, toggle_ = false;
};

// ui::Textbox
class Textbox : public Label {
public:
    enum class Input { All, Multiline, Numeric, Number };
    Textbox(Point position, Point size, const std::u32string &text = U"", const std::u32string &placeholder = U"");
    std::function<void()> change, defocus;
    void setText(const std::u32string &text) override;
    std::u32string text() const override { return backing_; }
    void setPlaceholder(const std::u32string &p) { placeholder_ = p; }
    void setBorder(bool on) { border_ = on; }
    void setHidden(bool hidden) { masked_ = hidden; }
    void setLimit(size_t limit) { limit_ = limit; }
    void setInputType(Input t) { input_ = t; }
    bool readOnly = false;
    void tabFocus();

    void tick() override;
    void onMouseDown(int x, int y, int button) override;
    void onMouseUp(int x, int y, int button) override;
    void onMouseMoved(int x, int y) override;
    void onKeyPress(int key, int scan, bool repeat, bool shift, bool ctrl, bool alt) override;
    void onTextInput(const std::u32string &text) override;
    void onDefocus() override;
    void draw(Canvas &c, Point screenPos) override;

private:
    bool characterValid(char32_t c) const;
    bool stringValid(const std::u32string &s) const;
    void insertText(const std::u32string &s);
    void afterTextChange(bool changed);
    void resetCursorPosition();
    void cutSelection();
    void pasteIntoSelection();
    void showText();
    Input input_ = Input::All;
    size_t limit_ = std::u32string::npos;
    bool mouseDown_ = false, masked_ = false, border_ = true;
    int cursor_ = 0, cursorX_ = 0, cursorY_ = 0;
    std::u32string backing_, placeholder_;
};

// ui::DropDown: shows the chosen option, and a list of them to choose from when clicked
class DropDown : public Component {
public:
    DropDown(Point position, Point size) : Component(position, size) {}
    std::function<void()> change;
    void addOption(const std::u32string &text, int value);
    void setOption(int value);
    void setOptionText(const std::u32string &text);
    std::pair<std::u32string, int> option() const;
    int optionIndex() const { return index_; }
    const std::vector<std::pair<std::u32string, int>> &options() const { return options_; }

    void onMouseClick(int x, int y, int button) override;
    void onMouseEnter(int, int) override { inside_ = true; }
    void onMouseLeave(int, int) override { inside_ = false; }
    void draw(Canvas &c, Point screenPos) override;

private:
    std::vector<std::pair<std::u32string, int>> options_;
    int index_ = -1;
    bool inside_ = false;
};

// ui::Slider: a value from 0 to its steps, dragged along it, over a gradient between two colours
class Slider : public Component {
public:
    Slider(Point position, Point size, int steps) : Component(position, size), steps_(steps) {}
    std::function<void()> change;
    int value() const { return value_; }
    void setValue(int v) { value_ = std::clamp(v, 0, steps_); }
    void setColour(uint32_t from, uint32_t to); // 0xRRGGBB

    void onMouseMoved(int x, int y) override;
    void onMouseDown(int x, int y, int button) override;
    void onMouseUp(int x, int y, int button) override;
    void draw(Canvas &c, Point screenPos) override;

private:
    void updatePosition(int position);
    int steps_, value_ = 0;
    bool down_ = false;
    std::vector<uint32_t> gradient_;
};

// ui::Panel: components of its own, drawn and clicked through a viewport
class Panel : public Component {
public:
    Panel(Point position, Point size) : Component(position, size) {}
    ~Panel() override;
    Point innerSize, viewport; // viewport: where the inside is drawn from (negative when scrolled)
    Component *add(std::unique_ptr<Component> c);
    template <class T>
    T *add(std::unique_ptr<T> c) {
        T *p = c.get();
        add(std::unique_ptr<Component>(std::move(c)));
        return p;
    }
    void clear();
    const std::vector<std::unique_ptr<Component>> &children() const { return children_; }

    void draw(Canvas &c, Point screenPos) override;
    void tick() override;
    void onMouseClick(int x, int y, int button) override;
    void onMouseDown(int x, int y, int button) override;
    void onMouseHover(int x, int y) override;
    void onMouseMoved(int x, int y) override;
    void onMouseLeave(int x, int y) override;
    void onMouseUp(int x, int y, int button) override;
    void onMouseWheel(int x, int y, int d) override;
    void onMouseWheelInside(int x, int y, int d) override;

protected:
    virtual void xDraw(Canvas &, Point) {}
    virtual void xTick() {}
    virtual void xOnMouseDown(int, int, int) {}
    virtual void xOnMouseUp(int, int, int) {}
    virtual void xOnMouseMoved(int, int) {}
    virtual void xOnMouseWheelInside(int, int, int) {}
    void propagateMouseMove();
    std::vector<std::unique_ptr<Component>> children_;
    friend class Window;
};

// ui::ScrollPanel: a panel scrolled with the wheel or its bar
class ScrollPanel : public Panel {
public:
    ScrollPanel(Point position, Point size) : Panel(position, size) {}

protected:
    void xDraw(Canvas &c, Point screenPos) override;
    void xTick() override;
    void xOnMouseDown(int x, int y, int button) override;
    void xOnMouseUp(int x, int y, int button) override;
    void xOnMouseMoved(int x, int y) override;
    void xOnMouseWheelInside(int x, int y, int d) override;

private:
    int barWidth_ = 0, maxOffsetY_ = 0;
    float offsetY_ = 0, velocity_ = 0;
    bool insideBar_ = false, insideBarArea_ = false, barSelected_ = false;
    int barInitialOffset_ = 0, barInitialClick_ = 0, barClickLocation_ = 0;
    double lastTick_ = -1;
};

// gui/game/ToolButton: a tool's button in the menus and element search (its colour or texture, its name), with the
// border of the mouse button it is on
class ToolButton : public Button {
public:
    // drawTexture draws the tool's 26 x 14 texture at the given point, or is empty for a plain coloured button
    ToolButton(Point position, Point size, const std::u32string &text, const std::u32string &toolTip, uint32_t colour,
               std::function<void(Canvas &, int, int)> drawTexture = nullptr);
    void setSelection(int slot); // 0-2 the mouse buttons (red, blue, green), 3 replace mode's (cyan), -1 none
    int selection() const { return selection_; }
    bool favourite = false;

    void onMouseDown(int x, int y, int button) override;
    void onMouseClick(int x, int y, int button) override;
    void onMouseUp(int x, int y, int button) override;
    void draw(Canvas &c, Point screenPos) override;

private:
    std::function<void(Canvas &, int, int)> drawTexture_;
    int selection_ = -1;
};

// ui::Window. Positions of -1 centre it when it is shown.
class Window {
public:
    Window(Point position, Point size) : position(position), size(size) {}
    virtual ~Window() = default;
    Window(const Window &) = delete;
    Window &operator=(const Window &) = delete;

    Point position, size;
    bool doesTextInput = false;
    Button *okayButton = nullptr, *cancelButton = nullptr;
    std::unique_ptr<Window> openPending; // a window to open over this one (a drop down's list)

    template <class T>
    T *add(std::unique_ptr<T> c) {
        T *p = c.get();
        addComponent(std::unique_ptr<Component>(std::move(c)));
        return p;
    }
    void addComponent(std::unique_ptr<Component> c);
    void removeComponent(Component *c); // (not from inside the component's own event)
    void focus(Component *c);
    bool isFocused(const Component *c) const { return c == focused_; }
    Component *focused() const { return focused_; }
    bool wantsTextInput() const;
    void close() { closing_ = true; } // (TPT's CloseActiveWindow and SelfDestruct)
    bool closing() const { return closing_; }
    virtual void toolTip(Point, const std::u32string &) {}

    enum class Exit { MouseOutside, Escape };
    virtual void onTryExit(Exit how);
    virtual void onTryOkay();
    virtual void onDraw(Canvas &) {}
    virtual void onTick() {}
    virtual void onKeyPress(int, int, bool, bool, bool, bool) {}
    virtual void onKeyRelease(int, int, bool, bool, bool, bool) {}
    virtual void onMouseDown(int, int, int) {}
    virtual void onMouseUp(int, int, int) {}
    virtual void onMouseMove(int, int) {}
    virtual void onMouseWheel(int, int, int) {}

    // Window::Do*: what TPT's engine calls
    virtual void doDraw(Canvas &c);
    void doTick();
    virtual void doKeyPress(int key, int scan, bool repeat, bool shift, bool ctrl, bool alt);
    virtual void doKeyRelease(int key, int scan, bool repeat, bool shift, bool ctrl, bool alt);
    void doTextInput(const std::u32string &text);
    virtual void doMouseDown(int x, int y, int button);
    virtual void doMouseMove(int x, int y);
    virtual void doMouseUp(int x, int y, int button);
    virtual void doMouseWheel(int x, int y, int d);

private:
    std::vector<std::unique_ptr<Component>> components_;
    Component *focused_ = nullptr, *hover_ = nullptr;
    bool closing_ = false;
};

// ui::Engine's windows: each shown over the frozen picture of what was on screen when it opened (darkening over its
// first 20 frames), the top one taking the input
class Engine {
public:
    void show(std::unique_ptr<Window> w, int screenW, int screenH);
    bool empty() const { return windows_.empty(); }
    size_t depth() const { return windows_.size(); }
    Window *top() const { return windows_.empty() ? nullptr : windows_.back().get(); }
    // Removes the windows that closed (and everything above them); returns how many went
    int collect();
    bool wantsTextInput() const { return top() && top()->wantsTextInput(); }
    // How dark the frozen picture under the top window is (TPT's fade, by frames at 60 a second)
    uint8_t fadeAlpha() const;

    void tick(double ms);
    void draw(Canvas &c);
    void mouseDown(int x, int y, int button);
    void mouseUp(int x, int y, int button);
    void mouseMove(int x, int y);
    void mouseWheel(int x, int y, int d);
    void keyPress(int key, int scan, bool repeat, bool shift, bool ctrl, bool alt);
    void keyRelease(int key, int scan, bool repeat, bool shift, bool ctrl, bool alt);
    void textInput(const std::u32string &text);

private:
    std::vector<std::unique_ptr<Window>> windows_;
    std::vector<double> shownMs_; // how long each has been up
    bool ignoreEvents_ = false;   // the rest of the frame a window opened or closed in
};

} // namespace bp::ui
