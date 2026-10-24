// TPT's windows for the game's tools, made of ui/Widgets as TPT makes them: the property tool's (PropertyTool.cpp's
// PropertyWindow), element search (gui/elementsearch), the stamp browser (gui/localbrowser) and TPT's dialogues.
#pragma once
#include "classic/TptSave.h"
#include "classic/TptTools.h"
#include "ui/GameUi.h"
#include "ui/Widgets.h"
#include "sim/Signs.h"

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

// SignTool's window: a sign's text and where its pointer is; editing one (its text changes as it is typed), Move (the
// sign follows the mouse until a click) and Delete. The signs change as TPT changes Simulation::signs, through
// onChange; the game screen draws them over the frozen picture meanwhile (SignWindow::DoDraw).
class SignWindow : public ui::Window {
public:
    using Changed = std::function<void(const std::vector<Sign> &signs)>;
    // From the interface's pixels to the world's; false where the world doesn't show
    using ToWorld = std::function<bool(int x, int y, int &wx, int &wy)>;
    SignWindow(std::vector<Sign> signs, int signID, int x, int y, Changed onChange, ToWorld toWorld);
    const std::vector<Sign> &signs() const { return signs_; }
    bool moving() const { return moving_; }

    void onDraw(Canvas &c) override;
    void onTryExit(Exit) override { close(); }
    void doDraw(Canvas &c) override;
    void doMouseMove(int x, int y) override;
    void doMouseDown(int x, int y, int button) override;
    void doMouseUp(int x, int y, int button) override;
    void doMouseWheel(int x, int y, int d) override;
    void doKeyPress(int key, int scan, bool repeat, bool shift, bool ctrl, bool alt) override;
    void doKeyRelease(int key, int scan, bool repeat, bool shift, bool ctrl, bool alt) override;

private:
    std::vector<Sign> signs_;
    int signID_, x_, y_; // the sign (-1 for a new one) and where it goes
    bool moving_ = false;
    ui::DropDown *justification_;
    ui::Textbox *text_;
    Changed onChange_;
    ToWorld toWorld_;
};

// TextPrompt: a question and a line of text to answer it with
class TextPrompt : public ui::Window {
public:
    TextPrompt(const std::u32string &title, const std::u32string &message, const std::u32string &text,
               const std::u32string &placeholder, std::function<void(const std::u32string &)> onText);
    void onDraw(Canvas &c) override;
};

// The stamp browser's stamps (ui::SaveButton with a SaveFile): the picture of one, made when it is first drawn (one a
// tick), its name, and on hover a box in the corner to select it with
class StampButton : public ui::Component {
public:
    using Load = std::function<bool(const std::string &name, SaveArea &out, std::string &error)>;
    StampButton(ui::Point position, ui::Point size, const std::string &name, Load load);
    std::function<void()> action, selectionChange;
    const std::string &name() const { return name_; }
    bool selected = false;

    void tick() override;
    void draw(Canvas &c, ui::Point screenPos) override;
    void onMouseClick(int x, int y, int button) override;
    void onMouseDown(int x, int y, int button) override;
    void onMouseEnter(int, int) override { inside_ = true; }
    void onMouseLeave(int, int) override { inside_ = false; }
    static int rendering; // pictures made this tick (ThumbnailRendererTask's queue)

private:
    void thumbBox(int &w, int &h) const;
    std::string name_;
    std::u32string shown_; // (cut to fit)
    Load load_;
    std::vector<uint32_t> thumb_;
    int thumbW_ = 0, thumbH_ = 0;
    std::string error_;
    bool tried_ = false, wantsDraw_ = false, inside_ = false, buttonDown_ = false;
};

// LocalBrowserView with its controller and model: the stamps 20 a page (5 x 4), found by a part of their names;
// clicking one places it (and moves it to the front, unless ctrl is held), selected ones can be deleted or renamed
class StampBrowser : public ui::Window {
public:
    using Open = std::function<void(const std::string &name, bool moveToFront)>;
    // The stamps' names in order, and what to do with them
    struct Store {
        std::function<std::vector<std::string>()> names;
        StampButton::Load load;
        std::function<void(const std::string &)> remove;
        std::function<bool(const std::string &, const std::string &, std::string &)> rename;
        std::function<void()> rescan;
    };
    StampBrowser(int w, int h, Store store, Open onOpen);

    void onTick() override;
    void onMouseWheel(int x, int y, int d) override;
    void onKeyPress(int key, int scan, bool repeat, bool shift, bool ctrl, bool alt) override;
    void onKeyRelease(int key, int scan, bool repeat, bool shift, bool ctrl, bool alt) override;
    void onTryExit(Exit) override {}

private:
    void update(const std::u32string &query, int page); // LocalBrowserModel::UpdateSavesList
    void requestUpdate(const std::u32string &query, int page);
    void setPageRelative(int offset);
    void pageChanged(); // NotifyPageChanged
    void selectedChanged();
    void exit();
    int pageCount() const;
    Store store_;
    Open open_;
    std::vector<std::string> names_; // (stampIDs)
    std::vector<StampButton *> buttons_;
    std::vector<std::string> selected_;
    std::u32string query_;
    int page_ = 0;
    bool moveToFront_ = true, changed_ = false, pending_ = false;
    std::u32string pendingQuery_;
    int pendingPage_ = 0;
    double lastChanged_ = 0;
    ui::Button *next_, *previous_, *rescan_, *remove_, *rename_;
    ui::Label *pageLabel_, *pageCountLabel_;
    ui::Textbox *pageBox_, *search_;
};

} // namespace bp
