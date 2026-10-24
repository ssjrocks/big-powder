#include "ui/Dialogs.h"

#include <SDL3/SDL_keycode.h>

#include <algorithm>
#include <cwctype>
#include <map>
#include <random>
#include <tuple>

namespace bp {

namespace {

std::u32string lower(std::u32string s) {
    for (char32_t &c : s)
        if (c < 0x80)
            c = char32_t(std::towlower(wint_t(c)));
    return s;
}

} // namespace

// ---- PropertyWindow ----

PropertyWindow::PropertyWindow(int field, const std::u32string &text, int temperatureScale, Set onSet)
    : ui::Window(ui::Point{-1, -1}, ui::Point{200, 87}), scale_(temperatureScale), onSet_(std::move(onSet)) {
    auto title = std::make_unique<ui::Label>(ui::Point{4, 5}, ui::Point{size.x - 8, 14}, U"Edit property");
    title->setTextColour(ui::style::InformationTitle);
    title->appearance.hAlign = ui::Appearance::HAlign::Left;
    title->appearance.vAlign = ui::Appearance::VAlign::Top;
    title->setText(U"Edit property");
    add(std::move(title));

    auto ok = std::make_unique<ui::Button>(ui::Point{0, size.y - 17}, ui::Point{size.x, 17}, U"OK");
    ok->appearance.hAlign = ui::Appearance::HAlign::Left;
    ok->appearance.vAlign = ui::Appearance::VAlign::Middle;
    ok->appearance.borderInactive = rgb(0xC8C8C8);
    ok->setText(U"OK");
    ok->action = [this] {
        close();
        if (valid_ && onSet_)
            onSet_(parsed_, property_->option().second, value_->text());
    };
    okayButton = add(std::move(ok));

    auto prop = std::make_unique<ui::DropDown>(ui::Point{8, 25}, ui::Point{size.x - 16, 16});
    prop->change = [this] {
        focus(value_);
        update();
    };
    const auto &names = tptPropertyNames();
    for (size_t i = 0; i < names.size(); i++)
        prop->addOption(std::u32string(names[i].begin(), names[i].end()), int(i));
    property_ = add(std::move(prop));

    auto value = std::make_unique<ui::Textbox>(ui::Point{8, 46}, ui::Point{size.x - 16, 16}, U"", U"[value]");
    value->appearance.hAlign = ui::Appearance::HAlign::Left;
    value->appearance.vAlign = ui::Appearance::VAlign::Middle;
    value->change = [this] { update(); };
    value_ = add(std::move(value));

    property_->setOption(field);
    value_->setText(text);
    focus(value_);
    update();
}

void PropertyWindow::update() {
    valid_ = tptParseProperty(property_->option().second, value_->text(), scale_, parsed_);
    okayButton->enabled = valid_;
    value_->setTextColour(valid_ ? 0xFFFFFF : ui::style::ErrorTitle);
}

void PropertyWindow::onDraw(Canvas &c) {
    c.drawFilledRect(position.x - 1, position.y - 1, size.x + 2, size.y + 2, 0x000000);
    c.drawRect(position.x, position.y, size.x, size.y, 0xC8C8C8);
}

void PropertyWindow::onKeyPress(int key, int, bool, bool, bool, bool) {
    if (key == SDLK_UP || key == SDLK_DOWN) {
        property_->setOption(property_->option().second + (key == SDLK_UP ? -1 : 1));
        focus(value_);
        update();
    }
}

// ---- ElementSearchWindow ----

ElementSearchWindow::ElementSearchWindow(const GameUi &ui, Choose onChoose)
    : ui::Window(ui::Point{-1, -1}, ui::Point{236, 302}), ui_(ui), choose_(std::move(onChoose)) {
    auto title = std::make_unique<ui::Label>(ui::Point{4, 5}, ui::Point{size.x - 8, 15}, U"Element Search");
    title->setTextColour(ui::style::InformationTitle);
    title->appearance.hAlign = ui::Appearance::HAlign::Left;
    title->setText(U"Element Search");
    add(std::move(title));

    auto field = std::make_unique<ui::Textbox>(ui::Point{8, 23}, ui::Point{size.x - 16, 17}, U"");
    field->change = [this] { search(field_->text()); };
    field->appearance.hAlign = ui::Appearance::HAlign::Left;
    field->setText(U"");
    field_ = add(std::move(field));
    focus(field_);

    auto closeButton = std::make_unique<ui::Button>(ui::Point{0, size.y - 15}, ui::Point{size.x / 2 + 1, 15}, U"Close");
    closeButton->action = [this] { exit_ = true; };
    auto okButton = std::make_unique<ui::Button>(ui::Point{size.x / 2, size.y - 15}, ui::Point{size.x / 2, 15}, U"OK");
    okButton->action = [this] {
        if (first_ >= 0)
            choose(0, first_);
    };
    add(std::move(okButton));
    add(std::move(closeButton));

    const ui::Point fp = field_->position, fs = field_->size;
    results_ = add(std::make_unique<ui::ScrollPanel>(ui::Point{fp.x + 1, fp.y + fs.y + 9},
                                                     ui::Point{fs.x - 2, size.y - (fp.y + fs.y + 6) - 23}));
    search(U"");
}

void ElementSearchWindow::search(const std::u32string &query) {
    first_ = -1;
    results_->clear();
    const std::u32string q = lower(query);
    struct Match {
        int tool;     // relevance by the tool's place in the list
        int haystack; // name, description, menu
        int needle;   // the whole of it, its start, anywhere in it
        bool operator<(const Match &o) const {
            return std::tie(haystack, needle, tool) < std::tie(o.haystack, o.needle, o.tool);
        }
    };
    std::map<int, Match> matches;
    auto push = [&](Match m) {
        auto it = matches.find(m.tool);
        if (it == matches.end())
            matches.emplace(m.tool, m);
        else if (m < it->second)
            it->second = m;
    };
    auto pushIfMatches = [&](const std::u32string &info, int tool, int haystack) {
        if (info == q)
            push({tool, haystack, 0});
        if (info.compare(0, q.size(), q) == 0)
            push({tool, haystack, 1});
        if (info.find(q) != std::u32string::npos)
            push({tool, haystack, 2});
    };
    const auto &tools = ui_.tools();
    for (int i = 0; i < int(tools.size()); i++) {
        const UiTool &t = tools[size_t(i)];
        if (t.kind == UiTool::Kind::None)
            continue;
        pushIfMatches(lower(t.name), i, 0);
        pushIfMatches(lower(t.description), i, 1);
        if (t.section >= 0) // (a tool in a menu)
            pushIfMatches(lower(ui_.sectionName(t.section)), i, 2);
    }
    std::vector<Match> sorted;
    for (const auto &m : matches)
        sorted.push_back(m.second);
    std::sort(sorted.begin(), sorted.end());
    ui::Point current;
    const ui::Point view{1, 1};
    for (const Match &m : sorted) {
        const UiTool &t = tools[size_t(m.tool)];
        if (first_ < 0)
            first_ = m.tool;
        std::function<void(Canvas &, int, int)> texture;
        if (ui_.hasTexture(t))
            texture = [this, &t](Canvas &c, int x, int y) { ui_.drawToolTexture(c, t, x, y); };
        auto b = std::make_unique<ui::ToolButton>(ui::Point{current.x + view.x, current.y + view.y}, ui::Point{30, 18},
                                                  texture ? U"" : t.name, t.description, t.colour, texture);
        ui::ToolButton *bp = b.get();
        const int tool = m.tool;
        b->action = [this, bp, tool] {
            if (bp->selection() >= 0 && bp->selection() <= 2)
                choose(bp->selection(), tool);
        };
        for (int slot = 0; slot < 3; slot++)
            if (ui_.activeToolIndex(slot) == m.tool) {
                b->setSelection(slot);
                break;
            }
        results_->add(std::move(b));
        current.x += 31;
        if (current.x + 30 > field_->size.x) {
            current.x = 0;
            current.y += 19;
        }
    }
    if (current.x == 0)
        current.y -= 19;
    results_->innerSize = ui::Point{results_->size.x, current.y + 20};
}

void ElementSearchWindow::choose(int slot, int tool) {
    const UiTool &t = ui_.tools()[size_t(tool)];
    if (ctrl_ && shift_ && !alt_) {
        // (favourites come with the menu of them)
    } else if (ctrl_ && alt_ && !shift_ && t.identifier.rfind("DEFAULT_PT_", 0) == 0)
        choose_(3, tool);
    else
        choose_(slot, tool);
    exit_ = true;
}

void ElementSearchWindow::onDraw(Canvas &c) {
    c.drawFilledRect(position.x - 1, position.y - 1, size.x + 2, size.y + 2, 0x000000);
    c.drawRect(position.x, position.y, size.x, size.y, 0xFFFFFF);
    c.blendRect(position.x + results_->position.x - 1, position.y + results_->position.y - 1, results_->size.x + 2,
                results_->size.y + 2, rgb(0xFFFFFF, 180));
    if (toolTipPresence_ > 0 && !toolTip_.empty()) // (TPT puts it below the window, at the screen's left)
        c.blendText(10, position.y + size.y + 9, toolTip_,
                    rgb(0xFFFFFF, uint8_t(toolTipPresence_ > 51 ? 255 : int(toolTipPresence_) * 5)));
}

void ElementSearchWindow::onTick() {
    if (exit_)
        close();
    // ui::Fade's linear profile: 120 a second up, 60 down
    double now = ui::host().now, ms = lastTick_ < 0 ? 0 : now - lastTick_;
    lastTick_ = now;
    if (toolTipIn_)
        toolTipPresence_ = std::min(120.0, toolTipPresence_ + ms * 0.12);
    else
        toolTipPresence_ = std::max(0.0, toolTipPresence_ - ms * 0.06);
    toolTipIn_ = false;
}

void ElementSearchWindow::onKeyPress(int key, int, bool repeat, bool, bool, bool) {
    if (repeat)
        return;
    switch (key) {
    case SDLK_KP_ENTER:
    case SDLK_RETURN:
        if (first_ >= 0)
            choose_(0, first_);
        [[fallthrough]];
    case SDLK_ESCAPE:
    case SDLK_AC_BACK:
        exit_ = true;
        break;
    case SDLK_LSHIFT:
    case SDLK_RSHIFT:
        shift_ = true;
        break;
    case SDLK_LCTRL:
    case SDLK_RCTRL:
        ctrl_ = true;
        break;
    case SDLK_LALT:
    case SDLK_RALT:
        alt_ = true;
        break;
    }
}

void ElementSearchWindow::onKeyRelease(int key, int, bool repeat, bool, bool, bool) {
    if (repeat)
        return;
    switch (key) {
    case SDLK_LSHIFT:
    case SDLK_RSHIFT:
        shift_ = false;
        break;
    case SDLK_LCTRL:
    case SDLK_RCTRL:
        ctrl_ = false;
        break;
    case SDLK_LALT:
    case SDLK_RALT:
        alt_ = false;
        break;
    }
}

void ElementSearchWindow::toolTip(ui::Point, const std::u32string &text) {
    toolTip_ = text;
    toolTipIn_ = true;
}

// ---- ColourPickerWindow ----

namespace {

int number(const std::u32string &s) { // String::ToNumber<int>(true): 0 when it isn't one
    int v = 0;
    for (char32_t c : s) {
        if (c < U'0' || c > U'9')
            return 0;
        v = v * 10 + int(c - U'0');
        if (v > 100000)
            return v;
    }
    return v;
}

std::u32string decimal(int v) { return utf32(std::to_string(v)); }

} // namespace

ColourPickerWindow::ColourPickerWindow(uint32_t initial, Picked onPicked)
    : ui::Window(ui::Point{-1, -1}, ui::Point{266, 215}), onPicked_(std::move(onPicked)) {
    auto colourChange = [this] {
        int r = std::min(number(r_->text()), 255), g = std::min(number(g_->text()), 255);
        int b = std::min(number(b_->text()), 255), alpha = std::min(number(a_->text()), 255);
        tptRgbToHsv(r, g, b, hue_, saturation_, value_);
        alpha_ = alpha;
        updateTextboxes(r, g, b, alpha);
        updateSliders();
    };
    auto colourChangeSlider = [this] {
        int r, g, b;
        hue_ = hSlider_->value();
        saturation_ = sSlider_->value();
        value_ = vSlider_->value();
        tptHsvToRgb(hue_, saturation_, value_, r, g, b);
        updateTextboxes(r, g, b, alpha_);
        updateSliders();
    };
    hSlider_ = add(std::make_unique<ui::Slider>(ui::Point{0, 134}, ui::Point{size.x, 17}, 359));
    hSlider_->change = colourChangeSlider;
    sSlider_ = add(std::make_unique<ui::Slider>(ui::Point{0, 134 + 17}, ui::Point{size.x, 17}, 255));
    sSlider_->change = colourChangeSlider;
    vSlider_ = add(std::make_unique<ui::Slider>(ui::Point{0, 134 + 34}, ui::Point{size.x, 17}, 255));
    vSlider_->change = colourChangeSlider;
    ui::Textbox **boxes[4] = {&r_, &g_, &b_, &a_};
    for (int k = 0; k < 4; k++) {
        auto t = std::make_unique<ui::Textbox>(ui::Point{5 + 35 * k, size.y - 23}, ui::Point{30, 17}, U"255");
        t->change = colourChange;
        t->setLimit(3);
        t->setInputType(ui::Textbox::Input::Number);
        *boxes[k] = add(std::move(t));
    }
    hex_ = add(std::make_unique<ui::Label>(ui::Point{150, size.y - 23}, ui::Point{53, 17}, U"0xFFFFFFFF"));
    auto done = std::make_unique<ui::Button>(ui::Point{size.x - 45, size.y - 23}, ui::Point{40, 17}, U"Done");
    done->action = [this] {
        uint32_t r = uint32_t(number(r_->text())) & 0xFF, g = uint32_t(number(g_->text())) & 0xFF;
        uint32_t b = uint32_t(number(b_->text())) & 0xFF;
        if (onPicked_)
            onPicked_(uint32_t(alpha_) << 24 | r << 16 | g << 8 | b);
        close();
    };
    okayButton = add(std::move(done));
    const int ir = int((initial >> 16) & 0xFF), ig = int((initial >> 8) & 0xFF), ib = int(initial & 0xFF);
    tptRgbToHsv(ir, ig, ib, hue_, saturation_, value_);
    alpha_ = int(initial >> 24);
    updateTextboxes(ir, ig, ib, alpha_);
    updateSliders();
}

void ColourPickerWindow::updateTextboxes(int r, int g, int b, int a) {
    r_->setText(decimal(r));
    g_->setText(decimal(g));
    b_->setText(decimal(b));
    a_->setText(decimal(a));
    hex_->setText(tptColourHex(a, r, g, b));
}

void ColourPickerWindow::updateSliders() {
    hSlider_->setValue(hue_);
    sSlider_->setValue(saturation_);
    vSlider_->setValue(value_);
    int r, g, b;
    tptHsvToRgb(hue_, saturation_, 255, r, g, b); // the value's gradient
    vSlider_->setColour(0x000000, uint32_t(r) << 16 | uint32_t(g) << 8 | uint32_t(b));
    if (value_ != 0) { // the saturation's
        tptHsvToRgb(hue_, 255, value_, r, g, b);
        sSlider_->setColour(uint32_t(value_) * 0x010101u, uint32_t(r) << 16 | uint32_t(g) << 8 | uint32_t(b));
    }
}

void ColourPickerWindow::pickAt(int x, int y) {
    x -= position.x + 5;
    y -= position.y + 5;
    hue_ = std::clamp(int((float(x) / float(255)) * 359.0f), 0, 359);
    saturation_ = std::clamp(255 - (y * 2), 0, 255);
}

void ColourPickerWindow::onMouseMove(int x, int y) {
    if (!mouseDown_)
        return;
    pickAt(x, y);
    int r, g, b;
    tptHsvToRgb(hue_, saturation_, value_, r, g, b);
    updateTextboxes(r, g, b, alpha_);
    updateSliders();
}

void ColourPickerWindow::onMouseDown(int x, int y, int) {
    int lx = x - (position.x + 5), ly = y - (position.y + 5);
    if (lx >= 0 && lx < 256 && ly >= 0 && ly <= 128) {
        mouseDown_ = true;
        pickAt(x, y);
    }
    if (mouseDown_) {
        int r, g, b;
        tptHsvToRgb(hue_, saturation_, value_, r, g, b);
        updateTextboxes(r, g, b, alpha_);
        updateSliders();
    }
}

void ColourPickerWindow::onMouseUp(int x, int y, int) {
    if (!mouseDown_)
        return;
    int r, g, b;
    tptHsvToRgb(hue_, saturation_, value_, r, g, b);
    updateTextboxes(r, g, b, alpha_);
    updateSliders();
    mouseDown_ = false;
    pickAt(x, y); // (TPT takes the point again after showing it)
}

void ColourPickerWindow::onKeyPress(int key, int, bool repeat, bool, bool, bool) {
    if (repeat || key != SDLK_TAB)
        return;
    if (r_->isFocused())
        g_->tabFocus();
    else if (g_->isFocused())
        b_->tabFocus();
    else if (b_->isFocused())
        a_->tabFocus();
    else if (a_->isFocused())
        r_->tabFocus();
}

void ColourPickerWindow::onDraw(Canvas &c) {
    c.blendFilledRect(position.x - 1, position.y - 1, size.x + 2, size.y + 2, rgb(0x000000, uint8_t(alpha_)));
    c.drawRect(position.x, position.y, size.x, size.y, 0xFFFFFF);
    c.drawRect(position.x + 4, position.y + 4, 258, 130, 0xB4B4B4);
    const int ox = position.x + 5, oy = position.y + 5;
    // the colour square: hue across, saturation down
    int lastx = -1;
    for (int saturation = 0; saturation <= 255; saturation += 2)
        for (int hue = 0; hue <= 359; hue++) {
            int currx = int(tptClampFlt(float(hue), 0, 359)) + ox;
            if (currx == lastx)
                continue;
            lastx = currx;
            int r, g, b;
            tptHsvToRgb(hue, 255 - saturation, value_, r, g, b);
            c.blendPixel(currx, saturation / 2 + oy, rgb(uint32_t(r) << 16 | uint32_t(g) << 8 | uint32_t(b), uint8_t(alpha_)));
        }
    // the hue's gradient over its slider
    const int gradientWidth = hSlider_->size.x - 10;
    for (int rx = 0; rx < gradientWidth; rx++) {
        int r, g, b;
        tptHsvToRgb(rx * 360 / gradientWidth, saturation_, value_, r, g, b);
        for (int ry = 0; ry < hSlider_->size.y / 2 - 1; ry++)
            c.blendPixel(ox + hSlider_->position.x + rx, oy + hSlider_->position.y + ry,
                         rgb(uint32_t(r) << 16 | uint32_t(g) << 8 | uint32_t(b), uint8_t(alpha_)));
    }
    // the square's pointer
    int hx = int(tptClampFlt(float(hue_), 0, 359)), sy = (255 - saturation_) / 2;
    c.xorLine(ox + hx, oy + sy - 5, ox + hx, oy + sy - 1);
    c.xorLine(ox + hx, oy + sy + 1, ox + hx, oy + sy + 5);
    c.xorLine(ox + hx - 5, oy + sy, ox + hx - 1, oy + sy);
    c.xorLine(ox + hx + 1, oy + sy, ox + hx + 5, oy + sy);
}

// ---- GolWindow ----

GolWindow::GolWindow(int rule, uint32_t colour1, uint32_t colour2, const std::u32string &lastName,
                     const std::u32string &lastRule, Add onAdd)
    : ui::Window(ui::Point{-1, -1}, ui::Point{200, 108}), highColour_(colour1 & 0xFFFFFF), lowColour_(colour2 & 0xFFFFFF),
      onAdd_(std::move(onAdd)) {
    auto title = std::make_unique<ui::Label>(ui::Point{4, 5}, ui::Point{size.x - 8, 14}, U"Edit custom GOL type");
    title->setTextColour(ui::style::InformationTitle);
    title->appearance.hAlign = ui::Appearance::HAlign::Left;
    title->appearance.vAlign = ui::Appearance::VAlign::Top;
    add(std::move(title));

    auto ok = std::make_unique<ui::Button>(ui::Point{0, size.y - 17}, ui::Point{size.x, 17}, U"OK");
    ok->appearance.hAlign = ui::Appearance::HAlign::Left;
    ok->appearance.vAlign = ui::Appearance::VAlign::Middle;
    ok->appearance.borderInactive = rgb(0xC8C8C8);
    ok->action = [this] {
        if (!name_->text().empty() && !rule_->text().empty()) {
            close();
            if (onAdd_)
                onAdd_(name_->text(), rule_->text(), highColour_, lowColour_);
        }
    };
    okayButton = add(std::move(ok));

    auto name = std::make_unique<ui::Textbox>(ui::Point{8, 25}, ui::Point{size.x - 16, 16}, U"", U"[name]");
    name->appearance.hAlign = ui::Appearance::HAlign::Left;
    name->appearance.vAlign = ui::Appearance::VAlign::Middle;
    name->setLimit(7);
    name_ = add(std::move(name));
    focus(name_);
    auto ruleBox = std::make_unique<ui::Textbox>(ui::Point{8, 46}, ui::Point{size.x - 16, 16}, U"", U"[rule]");
    ruleBox->appearance.hAlign = ui::Appearance::HAlign::Left;
    ruleBox->appearance.vAlign = ui::Appearance::VAlign::Middle;
    rule_ = add(std::move(ruleBox));
    focus(rule_);

    auto picker = [this](uint32_t *colour) {
        return [this, colour] {
            openPending = std::make_unique<ColourPickerWindow>(0xFF000000u | *colour, [this, colour](uint32_t c) {
                *colour = c & 0xFFFFFF;
                updateGradient();
            });
        };
    };
    high_ = add(std::make_unique<ui::Button>(ui::Point{8, 67}, ui::Point{16, 16}, U""));
    high_->action = picker(&highColour_);
    low_ = add(std::make_unique<ui::Button>(ui::Point{size.x - 24, 67}, ui::Point{16, 16}, U""));
    low_->action = picker(&lowColour_);

    if (rule) {
        rule_->setText(tptGolRuleString(rule));
        name_->setText(U"");
    } else {
        rule_->setText(lastRule);
        name_->setText(lastName);
        // (TPT's interfaceRng: light, then dark)
        static std::mt19937 rng(std::random_device{}());
        auto between = [](int a, int b) { return uint32_t(std::uniform_int_distribution<int>(a, b)(rng)); };
        highColour_ = between(0x80, 0xFF) << 16 | between(0x80, 0xFF) << 8 | between(0x80, 0xFF);
        lowColour_ = between(0x00, 0x7F) << 16 | between(0x00, 0x7F) << 8 | between(0x00, 0x7F);
    }
    updateGradient();
}

void GolWindow::updateGradient() {
    high_->appearance.backgroundInactive = high_->appearance.backgroundHover = rgb(highColour_);
    low_->appearance.backgroundInactive = low_->appearance.backgroundHover = rgb(lowColour_);
}

void GolWindow::onDraw(Canvas &c) {
    c.drawFilledRect(position.x - 1, position.y - 1, size.x + 2, size.y + 2, 0x000000);
    c.drawRect(position.x, position.y, size.x, size.y, 0xC8C8C8);
    const int width = size.x - 60;
    for (int xx = 0; xx < width; ++xx) {
        auto f = float(xx) / float(width);
        auto mix = [&](int shift) {
            return uint32_t(int(float((highColour_ >> shift) & 0xFF) * (1.f - f) + float((lowColour_ >> shift) & 0xFF) * f));
        };
        uint32_t col = mix(16) << 16 | mix(8) << 8 | mix(0);
        for (int yy = 0; yy < 16; ++yy)
            c.drawPixel(position.x + xx + 30, position.y + yy + 67, col);
    }
}

// ---- ConfirmPrompt, ErrorMessage ----

ConfirmPrompt::ConfirmPrompt(const std::u32string &title, const std::u32string &message, std::function<void()> onOkay,
                             const std::u32string &buttonText)
    : ui::Window(ui::Point{-1, -1}, ui::Point{250, 50}) {
    auto t = std::make_unique<ui::Label>(ui::Point{4, 5}, ui::Point{size.x - 8, 15}, title);
    t->setTextColour(ui::style::WarningTitle);
    t->appearance.hAlign = ui::Appearance::HAlign::Left;
    add(std::move(t));
    auto panel = std::make_unique<ui::ScrollPanel>(ui::Point{4, 24}, ui::Point{size.x - 8, 206});
    auto m = std::make_unique<ui::Label>(ui::Point{4, 0}, ui::Point{size.x - 28, -1}, U"");
    m->appearance.hAlign = ui::Appearance::HAlign::Left;
    m->appearance.vAlign = ui::Appearance::VAlign::Top;
    m->setMultiline(true);
    m->setText(message);
    const int labelH = m->size.y;
    panel->add(std::move(m));
    panel->innerSize = ui::Point{panel->size.x, labelH + 4};
    if (labelH < panel->size.y)
        panel->size.y = labelH + 4;
    size.y += panel->size.y + 12;
    add(std::move(panel));
    auto cancel = std::make_unique<ui::Button>(ui::Point{0, size.y - 16}, ui::Point{size.x - 75, 16}, U"Cancel");
    cancel->appearance.hAlign = ui::Appearance::HAlign::Left;
    cancel->appearance.vAlign = ui::Appearance::VAlign::Middle;
    cancel->appearance.borderInactive = rgb(0xC8C8C8);
    cancel->action = [this] { close(); };
    cancelButton = add(std::move(cancel));
    auto ok = std::make_unique<ui::Button>(ui::Point{size.x - 76, size.y - 16}, ui::Point{76, 16}, buttonText);
    ok->appearance.hAlign = ui::Appearance::HAlign::Left;
    ok->appearance.vAlign = ui::Appearance::VAlign::Middle;
    ok->appearance.textInactive = rgb(ui::style::WarningTitle);
    ok->action = [this, onOkay] {
        close();
        if (onOkay)
            onOkay();
    };
    okayButton = add(std::move(ok));
}

void ConfirmPrompt::onDraw(Canvas &c) {
    c.drawFilledRect(position.x - 1, position.y - 1, size.x + 2, size.y + 2, 0x000000);
    c.drawRect(position.x, position.y, size.x, size.y, 0xC8C8C8);
}

ErrorMessage::ErrorMessage(const std::u32string &title, const std::u32string &message)
    : ui::Window(ui::Point{-1, -1}, ui::Point{200, 35}) {
    auto t = std::make_unique<ui::Label>(ui::Point{4, 5}, ui::Point{size.x - 8, 16}, title);
    t->setTextColour(ui::style::ErrorTitle);
    t->appearance.hAlign = ui::Appearance::HAlign::Left;
    add(std::move(t));
    auto m = std::make_unique<ui::Label>(ui::Point{4, 24}, ui::Point{size.x - 8, -1}, U"");
    m->setMultiline(true);
    m->appearance.hAlign = ui::Appearance::HAlign::Left;
    m->appearance.vAlign = ui::Appearance::VAlign::Top;
    m->setText(message);
    size.y += m->size.y + 12;
    add(std::move(m));
    auto ok = std::make_unique<ui::Button>(ui::Point{0, size.y - 16}, ui::Point{size.x, 16}, U"Dismiss");
    ok->appearance.hAlign = ui::Appearance::HAlign::Left;
    ok->appearance.vAlign = ui::Appearance::VAlign::Middle;
    ok->appearance.borderInactive = rgb(0xC8C8C8);
    ok->action = [this] { close(); };
    okayButton = cancelButton = add(std::move(ok));
}

void ErrorMessage::onDraw(Canvas &c) {
    c.drawFilledRect(position.x - 1, position.y - 1, size.x + 2, size.y + 2, 0x000000);
    c.drawRect(position.x, position.y, size.x, size.y, 0xC8C8C8);
}

// ---- SignWindow ----

SignWindow::SignWindow(std::vector<Sign> signs, int signID, int x, int y, Changed onChange, ToWorld toWorld)
    : ui::Window(ui::Point{-1, -1}, ui::Point{250, 87}), signs_(std::move(signs)), signID_(signID), x_(x), y_(y),
      onChange_(std::move(onChange)), toWorld_(std::move(toWorld)) {
    auto title = std::make_unique<ui::Label>(ui::Point{4, 5}, ui::Point{size.x - 8, 15}, U"New sign");
    title->setTextColour(ui::style::InformationTitle);
    title->appearance.hAlign = ui::Appearance::HAlign::Left;
    title->appearance.vAlign = ui::Appearance::VAlign::Middle;
    ui::Label *titleLabel = add(std::move(title));

    auto ok = std::make_unique<ui::Button>(ui::Point{0, size.y - 16}, ui::Point{size.x, 16}, U"OK");
    ok->appearance.hAlign = ui::Appearance::HAlign::Left;
    ok->appearance.vAlign = ui::Appearance::VAlign::Middle;
    ok->appearance.borderInactive = rgb(0xC8C8C8);
    ok->action = [this] {
        close();
        const std::u32string text = text_->text();
        const int ju = justification_->option().second;
        if (signID_ == -1 && !text.empty())
            signs_.push_back(Sign{x_, y_, ju, text});
        else if (signID_ != -1 && !text.empty())
            signs_[size_t(signID_)] = Sign{x_, y_, ju, text};
        if (onChange_)
            onChange_(signs_);
    };
    okayButton = add(std::move(ok));

    add(std::make_unique<ui::Label>(ui::Point{8, 48}, ui::Point{40, 15}, U"Pointer:"));
    auto ju = std::make_unique<ui::DropDown>(ui::Point{52, 48}, ui::Point{55, 16});
    ju->addOption(U"\U0000E020 Left", 0);
    ju->addOption(U"\U0000E01E Middle", 1);
    ju->addOption(U"\U0000E01F Right", 2);
    ju->addOption(U"\U0000E01D None", 3);
    ju->setOption(1);
    ju->appearance.hAlign = ui::Appearance::HAlign::Left;
    justification_ = add(std::move(ju));

    auto text = std::make_unique<ui::Textbox>(ui::Point{8, 25}, ui::Point{size.x - 16, 17}, U"", U"[message]");
    text->appearance.hAlign = ui::Appearance::HAlign::Left;
    text->appearance.vAlign = ui::Appearance::VAlign::Middle;
    text->setLimit(45);
    text->change = [this] {
        if (signID_ != -1) {
            signs_[size_t(signID_)].text = text_->text();
            signs_[size_t(signID_)].ju = justification_->option().second;
            if (onChange_)
                onChange_(signs_);
        }
    };
    text_ = add(std::move(text));
    focus(text_);

    if (signID_ != -1) {
        titleLabel->setText(U"Edit sign");
        text_->setText(signs_[size_t(signID_)].text);
        justification_->setOption(signs_[size_t(signID_)].ju);
        ui::Point p{justification_->position.x + justification_->size.x + 3, 48};
        auto move = std::make_unique<ui::Button>(p, ui::Point{((size.x - p.x - 8) / 2) - 2, 16}, U"Move");
        move->action = [this] {
            signs_[size_t(signID_)].ju = justification_->option().second;
            moving_ = true;
        };
        ui::Button *moveButton = add(std::move(move));
        p.x += moveButton->size.x + 3;
        auto del = std::make_unique<ui::Button>(p, ui::Point{(size.x - p.x - 8) - 1, 16}, U"Delete");
        del->action = [this] {
            close();
            signs_.erase(signs_.begin() + signID_);
            if (onChange_)
                onChange_(signs_);
        };
        add(std::move(del));
        x_ = signs_[size_t(signID_)].x;
        y_ = signs_[size_t(signID_)].y;
    }
}

void SignWindow::onDraw(Canvas &c) {
    c.drawFilledRect(position.x - 1, position.y - 1, size.x + 2, size.y + 2, 0x000000);
    c.drawRect(position.x, position.y, size.x, size.y, 0xC8C8C8);
}

void SignWindow::doDraw(Canvas &c) {
    if (!moving_) // (the signs themselves go over the picture first, by the game screen)
        ui::Window::doDraw(c);
}

void SignWindow::doMouseMove(int x, int y) {
    if (!moving_) {
        ui::Window::doMouseMove(x, y);
        return;
    }
    int wx, wy;
    if (toWorld_ && toWorld_(x, y, wx, wy)) {
        signs_[size_t(signID_)].x = x_ = wx;
        signs_[size_t(signID_)].y = y_ = wy;
        if (onChange_)
            onChange_(signs_);
    }
}

void SignWindow::doMouseDown(int x, int y, int button) {
    if (!moving_)
        ui::Window::doMouseDown(x, y, button);
    else
        moving_ = false;
}

void SignWindow::doMouseUp(int x, int y, int button) {
    if (!moving_)
        ui::Window::doMouseUp(x, y, button);
}

void SignWindow::doMouseWheel(int x, int y, int d) {
    if (!moving_)
        ui::Window::doMouseWheel(x, y, d);
}

void SignWindow::doKeyPress(int key, int scan, bool repeat, bool shift, bool ctrl, bool alt) {
    if (!moving_)
        ui::Window::doKeyPress(key, scan, repeat, shift, ctrl, alt);
}

void SignWindow::doKeyRelease(int key, int scan, bool repeat, bool shift, bool ctrl, bool alt) {
    if (!moving_)
        ui::Window::doKeyRelease(key, scan, repeat, shift, ctrl, alt);
}

} // namespace bp
