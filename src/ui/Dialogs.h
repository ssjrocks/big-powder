// TPT's windows for the game's tools, made of ui/Widgets as TPT makes them: the property tool's (PropertyTool.cpp's
// PropertyWindow) and element search (gui/elementsearch).
#pragma once
#include "classic/TptTools.h"
#include "ui/GameUi.h"
#include "ui/Widgets.h"

#include <functional>

namespace bp {

// PropertyWindow: a property and its value, which turns red while it can't be read; OK gives what the property tool
// is to set, with the property and text to open with next time (TPT's Prop.Type and Prop.Value)
class PropertyWindow : public ui::Window {
public:
    using Set = std::function<void(const ParticleProperty &p, int field, const std::u32string &text)>;
    PropertyWindow(int field, const std::u32string &text, int temperatureScale, Set onSet);

    void onDraw(Canvas &c) override;
    void onKeyPress(int key, int scan, bool repeat, bool shift, bool ctrl, bool alt) override;
    void onTryExit(Exit) override { close(); }

private:
    void update();
    ui::DropDown *property_;
    ui::Textbox *value_;
    int scale_;
    bool valid_ = false;
    ParticleProperty parsed_;
    Set onSet_;
};

// ElementSearchActivity: every tool whose name, description or menu matches what is typed, best first; clicking one
// (or Enter for the first) chooses it for that mouse button. Ctrl+alt chooses what replace mode works on.
class ElementSearchWindow : public ui::Window {
public:
    using Choose = std::function<void(int slot, int tool)>;
    ElementSearchWindow(const GameUi &ui, Choose onChoose);

    void onDraw(Canvas &c) override;
    void onTick() override;
    void onKeyPress(int key, int scan, bool repeat, bool shift, bool ctrl, bool alt) override;
    void onKeyRelease(int key, int scan, bool repeat, bool shift, bool ctrl, bool alt) override;
    void toolTip(ui::Point, const std::u32string &text) override;

private:
    void search(const std::u32string &query);
    void choose(int slot, int tool);
    const GameUi &ui_;
    Choose choose_;
    ui::Textbox *field_;
    ui::ScrollPanel *results_;
    int first_ = -1;
    bool exit_ = false, shift_ = false, ctrl_ = false, alt_ = false;
    std::u32string toolTip_;
    bool toolTipIn_ = false;
    double toolTipPresence_ = 0, lastTick_ = -1;
};

// ColourPickerActivity: hue and saturation from the square, value from its slider, or the four channels typed in;
// Done gives the colour (0xAARRGGBB)
class ColourPickerWindow : public ui::Window {
public:
    using Picked = std::function<void(uint32_t colour)>;
    ColourPickerWindow(uint32_t initial, Picked onPicked);

    void onDraw(Canvas &c) override;
    void onKeyPress(int key, int scan, bool repeat, bool shift, bool ctrl, bool alt) override;
    void onMouseMove(int x, int y) override;
    void onMouseDown(int x, int y, int button) override;
    void onMouseUp(int x, int y, int button) override;
    void onTryExit(Exit) override { close(); }

private:
    void updateTextboxes(int r, int g, int b, int a);
    void updateSliders();
    void pickAt(int x, int y);
    ui::Slider *hSlider_, *sSlider_, *vSlider_;
    ui::Textbox *r_, *g_, *b_, *a_;
    ui::Label *hex_;
    int hue_ = 0, saturation_ = 0, value_ = 0, alpha_ = 255;
    bool mouseDown_ = false;
    Picked onPicked_;
};

// GOLTool's window: a custom Game of Life type's name, rule and two colours (each button opens the colour picker); OK
// hands them on to be checked (GOLWindow::validate)
class GolWindow : public ui::Window {
public:
    using Add = std::function<void(const std::u32string &name, const std::u32string &rule, uint32_t colour1,
                                   uint32_t colour2)>;
    // With a rule (the sample tool's), its text and the colours given; otherwise the last name and rule (TPT's
    // CustomGOL.Name and CustomGOL.Rule) and random colours, light then dark
    GolWindow(int rule, uint32_t colour1, uint32_t colour2, const std::u32string &lastName, const std::u32string &lastRule,
              Add onAdd);

    void onDraw(Canvas &c) override;
    void onTryExit(Exit) override { close(); }

private:
    void updateGradient();
    ui::Button *high_, *low_;
    ui::Textbox *name_, *rule_;
    uint32_t highColour_, lowColour_; // 0xRRGGBB
    Add onAdd_;
};

// ConfirmPrompt: a question with Cancel and a button to go ahead
class ConfirmPrompt : public ui::Window {
public:
    ConfirmPrompt(const std::u32string &title, const std::u32string &message, std::function<void()> onOkay,
                  const std::u32string &buttonText = U"Confirm");
    void onDraw(Canvas &c) override;
};

// ErrorMessage: a red title, the message, Dismiss
class ErrorMessage : public ui::Window {
public:
    ErrorMessage(const std::u32string &title, const std::u32string &message);
    void onDraw(Canvas &c) override;
};

} // namespace bp
