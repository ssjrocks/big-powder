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

} // namespace bp
