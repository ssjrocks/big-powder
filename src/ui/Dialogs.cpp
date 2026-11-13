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
                             const std::u32string &buttonText, std::function<void()> onCancel)
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
    cancel->action = [this, onCancel] {
        close();
        if (onCancel)
            onCancel();
    };
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

namespace bp {

TextPrompt::TextPrompt(const std::u32string &title, const std::u32string &message, const std::u32string &text,
                       const std::u32string &placeholder, std::function<void(const std::u32string &)> onText)
    : ui::Window(ui::Point{-1, -1}, ui::Point{200, 65}) {
    auto t = std::make_unique<ui::Label>(ui::Point{4, 5}, ui::Point{size.x - 8, 18}, title);
    t->setTextColour(ui::style::WarningTitle);
    t->appearance.hAlign = ui::Appearance::HAlign::Left;
    t->appearance.vAlign = ui::Appearance::VAlign::Middle;
    add(std::move(t));
    auto m = std::make_unique<ui::Label>(ui::Point{4, 25}, ui::Point{size.x - 8, -1}, U"");
    m->setMultiline(true);
    m->setText(message);
    m->appearance.hAlign = ui::Appearance::HAlign::Left;
    m->appearance.vAlign = ui::Appearance::VAlign::Top;
    ui::Label *message_ = add(std::move(m));
    size.y += message_->size.y + 4;
    auto field = std::make_unique<ui::Textbox>(ui::Point{4, message_->position.y + message_->size.y + 7},
                                               ui::Point{size.x - 8, 16}, text, placeholder);
    field->appearance.vAlign = ui::Appearance::VAlign::Middle;
    field->appearance.hAlign = ui::Appearance::HAlign::Left;
    ui::Textbox *f = add(std::move(field));
    focus(f);
    auto cancel = std::make_unique<ui::Button>(ui::Point{0, size.y - 16}, ui::Point{size.x / 2 + 1, 16}, U"Cancel");
    cancel->appearance.hAlign = ui::Appearance::HAlign::Left;
    cancel->appearance.vAlign = ui::Appearance::VAlign::Middle;
    cancel->appearance.borderInactive = rgb(0xC8C8C8);
    cancel->action = [this] { close(); };
    cancelButton = add(std::move(cancel));
    auto ok = std::make_unique<ui::Button>(ui::Point{size.x / 2, size.y - 16}, ui::Point{size.x / 2, 16}, U"Okay");
    ok->appearance.hAlign = ui::Appearance::HAlign::Right;
    ok->appearance.vAlign = ui::Appearance::VAlign::Middle;
    ok->appearance.textInactive = rgb(ui::style::WarningTitle);
    ok->action = [this, f, onText] {
        close();
        if (onText)
            onText(f->text());
    };
    okayButton = add(std::move(ok));
}

void TextPrompt::onDraw(Canvas &c) {
    c.drawFilledRect(position.x - 1, position.y - 1, size.x + 2, size.y + 2, 0x000000);
    c.drawRect(position.x, position.y, size.x, size.y, 0xC8C8C8);
}

int StampButton::rendering = 0;

StampButton::StampButton(ui::Point position, ui::Point size, const std::string &name, Load load)
    : ui::Component(position, size), name_(name), load_(std::move(load)) {
    shown_ = utf32(name);
    if (const Canvas *f = ui::host().font) {
        int w, h;
        f->textSize(shown_, w, h);
        if (w - 1 > size.x) {
            shown_.erase(f->textFit(shown_, size.x - 22));
            shown_ += U"...";
        }
    }
}

// (Size.Y - 25) / YRES of TPT's world, in its proportions
void StampButton::thumbBox(int &w, int &h) const {
    const float scale = float(size.y - 25) / float(ClassicSim::H);
    w = int(float(ClassicSim::W) * scale), h = int(float(ClassicSim::H) * scale);
}

void StampButton::tick() {
    if (thumb_.empty() && !tried_ && wantsDraw_ && rendering < 1) {
        tried_ = true;
        rendering++;
        SaveArea s;
        if (!load_(name_, s, error_)) {
            if (error_.empty())
                error_ = "could not be read";
        } else {
            int bw, bh;
            thumbBox(bw, bh);
            thumb_ = tptRenderThumbnail(s, bw, bh, thumbW_, thumbH_);
        }
    }
    wantsDraw_ = false;
}

void StampButton::draw(Canvas &c, ui::Point p) {
    wantsDraw_ = true;
    int bw, bh;
    thumbBox(bw, bh);
    if (selected)
        c.blendFilledRect(p.x, p.y, size.x, size.y, rgb(0x64AAFF, 100));
    if (!thumb_.empty())
        c.drawImage(p.x + (size.x - thumbW_) / 2, p.y + (size.y - 21 - thumbH_) / 2, thumbW_, thumbH_, thumb_);
    else if (!error_.empty()) {
        int w, h;
        c.textSize(U"Error loading save", w, h);
        c.blendText(p.x + (size.x - (w - 1)) / 2, p.y + (size.y - 28) / 2, U"Error loading save", rgb(0xB4B4B4));
    }
    c.drawRect(p.x + (size.x - bw) / 2, p.y + (size.y - bh - 21) / 2, bw, bh, inside_ ? 0xD2E6FF : 0xB4B4B4);
    if (thumbW_ && !thumb_.empty())
        c.xorDottedRect(p.x + (size.x - thumbW_) / 2, p.y + (size.y - thumbH_ - 21) / 2, thumbW_, thumbH_);
    int w, h;
    c.textSize(shown_, w, h);
    c.blendText(p.x + (size.x - (w - 1)) / 2, p.y + size.y - 21, shown_, rgb(inside_ ? 0xFFFFFF : 0xB4B4B4));
    if (inside_) {
        c.drawFilledRect(p.x + size.x - 19, p.y + 7, 13, 13, 0x000000);
        c.drawRect(p.x + size.x - 20, p.y + 6, 14, 14, 0xFFFFFF);
        if (selected)
            c.drawFilledRect(p.x + size.x - 18, p.y + 8, 10, 10, 0xFFFFFF);
    }
}

void StampButton::onMouseClick(int x, int y, int button) {
    if (button != 1)
        return; // left click only
    if (x >= size.x - 20 && y >= 6 && y <= 20 && x <= size.x - 6) {
        selected = !selected;
        if (selectionChange)
            selectionChange();
        return;
    }
    if (!error_.empty()) {
        if (ui::Window *w = parentWindow())
            w->openPending = std::make_unique<ErrorMessage>(U"Error loading save", utf32(error_));
        return;
    }
    if (buttonDown_) {
        buttonDown_ = false;
        if (action)
            action();
    }
}

void StampButton::onMouseDown(int, int, int button) {
    if (!mouseDownInside || button == 3) // (the right button would open a menu; stamps have none)
        return;
    buttonDown_ = true;
    if (button != 1) {
        selected = !selected;
        if (selectionChange)
            selectionChange();
    }
}

namespace {
constexpr int STAMPS_PAGE = 20, STAMPS_X = 5, STAMPS_Y = 4;
}

StampBrowser::StampBrowser(int w, int h, Store store, Open onOpen)
    : ui::Window(ui::Point{0, 0}, ui::Point{w, h}), store_(std::move(store)), open_(std::move(onOpen)) {
    auto button = [&](int x, int y, int bw, const std::u32string &text, ui::Appearance::HAlign align) {
        auto b = std::make_unique<ui::Button>(ui::Point{x, y}, ui::Point{bw, 16}, text);
        b->appearance.hAlign = align;
        b->appearance.vAlign = ui::Appearance::VAlign::Middle;
        return add(std::move(b));
    };
    next_ = button(w - 52, h - 18, 50, U"Next ", ui::Appearance::HAlign::Right);
    previous_ = button(2, h - 18, 50, U" Prev", ui::Appearance::HAlign::Left);
    rescan_ = button(w - 122, h - 18, 60, U"Rescan", ui::Appearance::HAlign::Centre);
    next_->action = [this] { setPageRelative(1); };
    previous_->action = [this] { setPageRelative(-1); };
    rescan_->action = [this] { // LocalBrowserController::RescanStamps
        store_.rescan();
        requestUpdate(query_, page_);
    };
    auto field = std::make_unique<ui::Textbox>(ui::Point{30, 25}, ui::Point{w - 60, 16}, U"", U"[search]");
    field->appearance.vAlign = ui::Appearance::VAlign::Middle;
    field->appearance.hAlign = ui::Appearance::HAlign::Left;
    field->change = [this] { // searchChanged
        pageBox_->setText(U"1");
        changed_ = true;
        lastChanged_ = ui::host().now + 600;
    };
    search_ = add(std::move(field));
    focus(search_);
    auto pageBox = std::make_unique<ui::Textbox>(ui::Point{283, h - 18}, ui::Point{41, 16}, U"");
    pageBox->setInputType(ui::Textbox::Input::Number);
    pageBox->change = [this] { // pageChanged
        int num = 0;
        for (char32_t ch : pageBox_->text())
            if (ch >= U'0' && ch <= U'9')
                num = std::min(num * 10 + int(ch - U'0'), 1 << 24);
        if (num > pageCount())
            pageBox_->setText(utf32(std::to_string(pageCount())));
        changed_ = true;
        lastChanged_ = ui::host().now + 600;
    };
    pageBox_ = add(std::move(pageBox));
    auto label = [&](int x, int lw, const std::u32string &text, ui::Appearance::HAlign align) {
        auto l = std::make_unique<ui::Label>(ui::Point{x, h - 18}, ui::Point{lw, 16}, text);
        l->appearance.hAlign = align;
        return add(std::move(l));
    };
    pageLabel_ = label(0, 30, U"Page", ui::Appearance::HAlign::Right);
    pageCountLabel_ = label(w / 2 + 6, 50, U"", ui::Appearance::HAlign::Left);
    remove_ = button((w - 100) / 2, h - 18, 100, U"Delete", ui::Appearance::HAlign::Centre);
    remove_->visible = false;
    remove_->action = [this] { // RemoveSelected
        std::u32string desc = U"Are you sure you want to delete " + utf32(std::to_string(selected_.size())) + U" stamp";
        if (selected_.size() > 1)
            desc += U"s";
        desc += U"?";
        openPending = std::make_unique<ConfirmPrompt>(U"Delete stamps", desc, [this] {
            for (const std::string &n : selected_)
                store_.remove(n);
            selected_.clear();
            requestUpdate(query_, page_);
        });
    };
    rename_ = button((w - 100) / 2 + 52, h - 18, 100, U"Rename", ui::Appearance::HAlign::Centre);
    rename_->visible = false;
    rename_->action = [this] { // RenameSelected
        const std::string save = selected_.empty() ? "" : selected_[0];
        openPending = std::make_unique<TextPrompt>(
            U"Rename stamp", U"Enter a new name for the stamp:", U"", U"[new name]", [this, save](const std::u32string &n) {
                std::string err;
                if (n.empty())
                    openPending = std::make_unique<ErrorMessage>(U"Error renaming stamp",
                                                                 U"You have to specify the filename.");
                else if (!store_.rename(save, utf8(n), err))
                    openPending = std::make_unique<ErrorMessage>(U"Error renaming stamp", utf32(err));
                selected_.clear(); // RefreshSavesList
                requestUpdate(query_, page_);
            });
    };
    update(U"", 0);
}

int StampBrowser::pageCount() const {
    const int n = int(names_.size());
    const int count = n / STAMPS_PAGE + (n % STAMPS_PAGE ? 1 : 0);
    return count ? count : 1; // (always at least one page, even with nothing on it)
}

void StampBrowser::update(const std::u32string &query, int page) {
    selected_.clear();
    query_ = query;
    page_ = page;
    names_ = store_.names();
    for (StampButton *b : buttons_)
        removeComponent(b);
    buttons_.clear();
    // NotifySavesListChanged: the page's stamps in a grid below the search box
    const std::string q = utf8(query);
    std::vector<std::string> shown;
    int found = 0;
    for (const std::string &id : names_) {
        if (!q.empty() && id.find(q) == std::string::npos)
            continue;
        if (found >= page_ * STAMPS_PAGE && found < (page_ + 1) * STAMPS_PAGE)
            shown.push_back(id);
        found++;
    }
    const int padding = 2, yOffset = 50, areaW = size.x, areaH = size.y - yOffset - 18;
    const int bw = areaW / STAMPS_X - padding * 2, bh = areaH / STAMPS_Y - padding * 2;
    int x = 0, y = 0;
    for (size_t i = 0; i < shown.size(); i++) {
        if (x == STAMPS_X) {
            if (y == STAMPS_Y - 1)
                break;
            x = 0;
            y++;
        }
        auto b = std::make_unique<StampButton>(
            ui::Point{padding + x * (bw + padding * 2), yOffset + padding + y * (bh + padding * 2)}, ui::Point{bw, bh},
            shown[i], store_.load);
        StampButton *p = b.get();
        b->action = [this, p] {
            const std::string name = p->name();
            const bool front = moveToFront_;
            close(); // (LocalBrowserController::Exit, then the game takes the stamp)
            if (open_)
                open_(name, front);
        };
        b->selectionChange = [this, p] {
            if (p->selected) {
                if (std::find(selected_.begin(), selected_.end(), p->name()) == selected_.end())
                    selected_.push_back(p->name());
            } else
                std::erase(selected_, p->name());
            selectedChanged();
        };
        buttons_.push_back(add(std::move(b)));
        x++;
    }
    pageChanged();
    selectedChanged();
}

void StampBrowser::pageChanged() {
    if (names_.empty())
        pageLabel_->visible = pageCountLabel_->visible = pageBox_->visible = false;
    else {
        const std::u32string info = U"of " + utf32(std::to_string(pageCount()));
        pageCountLabel_->setText(info);
        int w = 0, h = 0;
        if (const Canvas *f = ui::host().font)
            f->textSize(info, w, h);
        w -= 1;
        pageLabel_->position.x = size.x / 2 - w - 20;
        pageBox_->position.x = size.x / 2 - w + 11;
        pageBox_->size.x = w - 4;
        pageLabel_->visible = pageCountLabel_->visible = pageBox_->visible = true;
        pageBox_->setText(utf32(std::to_string(page_ + 1)));
    }
    previous_->visible = page_ != 0;
    next_->visible = page_ != pageCount() - 1;
}

void StampBrowser::selectedChanged() {
    for (StampButton *b : buttons_)
        b->selected = std::find(selected_.begin(), selected_.end(), b->name()) != selected_.end();
    remove_->visible = !selected_.empty();
    rename_->visible = selected_.size() == 1;
    remove_->position.x = (size.x - 100) / 2 - (rename_->visible ? 52 : 0);
    const bool pages = !remove_->visible;
    pageLabel_->visible = pageCountLabel_->visible = pageBox_->visible = pages && !names_.empty();
}

void StampBrowser::setPageRelative(int offset) {
    int page = std::max(std::min(page_ + offset, pageCount() - 1), 0);
    if (page != page_)
        requestUpdate(query_, page);
}

void StampBrowser::requestUpdate(const std::u32string &query, int page) {
    pending_ = true;
    pendingQuery_ = query;
    pendingPage_ = page;
}

void StampBrowser::onTick() {
    StampButton::rendering = 0;
    if (pending_) { // (the list changes here, not inside a button's own click)
        pending_ = false;
        update(pendingQuery_, pendingPage_);
    }
    if (changed_ && lastChanged_ < ui::host().now) {
        changed_ = false;
        int num = 0;
        for (char32_t ch : pageBox_->text())
            if (ch >= U'0' && ch <= U'9')
                num = std::min(num * 10 + int(ch - U'0'), 1 << 24);
        const int page = std::max(num - 1, 0);
        // LocalBrowserController::SetSearch
        if ((search_->text() != query_ || page != page_) && page >= 0 && page < pageCount())
            update(search_->text(), page);
    }
}

void StampBrowser::onMouseWheel(int, int, int d) {
    if (d)
        setPageRelative(-d);
}

void StampBrowser::onKeyPress(int key, int, bool repeat, bool, bool, bool) {
    if (repeat)
        return;
    if (key == SDLK_ESCAPE)
        close();
    else if (key == SDLK_LCTRL || key == SDLK_RCTRL)
        moveToFront_ = false;
}

void StampBrowser::onKeyRelease(int key, int, bool repeat, bool, bool, bool) {
    if (repeat)
        return;
    if (key == SDLK_LCTRL || key == SDLK_RCTRL)
        moveToFront_ = true;
}

RenderOptionsWindow::RenderOptionsWindow(int viewW, int viewH, int h, ViewSettings &view)
    : ui::Window(ui::Point{0, 0}, ui::Point{viewW, h}), view_(view), xres_(viewW), yres_(viewH) {
    const TptModes &m = tptModes();
    auto preset = [&](int index, Icon icon, int dx, int dy, const char32_t *tip) {
        auto b = std::make_unique<ui::Button>(ui::Point{xres_ + dx, yres_ + dy}, ui::Point{30, 13}, U"", tip);
        b->setIcon(icon);
        b->action = [this, index] { loadPreset(index); };
        add(std::move(b));
    };
    preset(1, Icon::Velocity, -37, 6, U"Velocity display mode preset");
    preset(2, Icon::Pressure, -37, 24, U"Pressure display mode preset");
    preset(3, Icon::Persistant, -76, 6, U"Persistent display mode preset");
    preset(4, Icon::Fire, -76, 24, U"Fire display mode preset");
    preset(5, Icon::Blob, -115, 6, U"Blob display mode preset");
    preset(6, Icon::Heat, -115, 24, U"Heat display mode preset");
    preset(7, Icon::Blur, -154, 6, U"Fancy display mode preset");
    preset(8, Icon::Basic, -154, 24, U"Nothing display mode preset");
    preset(9, Icon::Gradient, -193, 6, U"Heat gradient display mode preset");
    preset(0, Icon::Alternate, -193, 24, U"Alternative Velocity display mode preset");
    preset(10, Icon::Life, -232, 6, U"Life display mode preset");
    auto box = [&](Kind kind, uint32_t mode, Icon icon, int dx, int dy, const char32_t *tip) {
        auto b = std::make_unique<ui::Checkbox>(ui::Point{dx, yres_ + dy}, ui::Point{30, 16}, U"", tip);
        b->setIcon(icon);
        ui::Checkbox *p = b.get();
        b->action = [this, kind, mode, p] {
            if (kind == Kind::Render) { // CalculateRenderMode
                uint32_t r = 0;
                for (const ModeBox &mb : boxes_)
                    if (mb.kind == Kind::Render && mb.box->checked)
                        r |= mb.mode;
                view_.renderMode = r;
            } else if (kind == Kind::Display) {
                uint32_t d = view_.displayMode;
                if (mode & tptModes().air) // (the air displays are one at a time)
                    d &= ~tptModes().air;
                d = p->checked ? d | mode : d & ~mode;
                view_.displayMode = d;
            } else // (one colour mode at a time, or none)
                view_.colorMode = p->checked ? mode : 0;
            sync();
        };
        boxes_.push_back({add(std::move(b)), mode, kind});
    };
    box(Kind::Render, m.effe, Icon::Effect, 1, 4, U"Adds Special flare effects to some elements");
    box(Kind::Render, m.fire, Icon::Fire, 1, 22, U"Fire effect for gasses");
    box(Kind::Render, m.glow, Icon::Glow, 33, 4, U"Glow effect on some elements");
    box(Kind::Render, m.blur, Icon::Blur, 33, 22, U"Blur effect for liquids");
    box(Kind::Render, m.blob, Icon::Blob, 65, 4, U"Makes everything be drawn like a blob");
    box(Kind::Render, m.basc, Icon::Basic, 65, 22, U"Basic rendering, without this, most things will be invisible");
    box(Kind::Render, m.sprk, Icon::Effect, 97, 4, U"Glow effect on sparks");
    box(Kind::Display, m.airc, Icon::Alternate, 135, 4, U"Displays pressure as red and blue, and velocity as white");
    box(Kind::Display, m.airp, Icon::Pressure, 135, 22, U"Displays pressure, red is positive and blue is negative");
    box(Kind::Display, m.airv, Icon::Velocity, 167, 4,
        U"Displays velocity and positive pressure: up/down adds blue, right/left adds red, still pressure adds green");
    box(Kind::Display, m.airh, Icon::Heat, 167, 22, U"Displays the temperature of the air like heat display does");
    box(Kind::Display, m.airw, Icon::Vort, 199, 4, U"Displays vorticity, red is clockwise and blue is anticlockwise");
    box(Kind::Display, m.warp, Icon::Warp, 237, 22, U"Gravity lensing, Newtonian Gravity bends light with this on");
    box(Kind::Display, m.deffe, Icon::Effect, 237, 4, U"Enables moving solids, stickmen guns, and premium(tm) graphics");
    box(Kind::Display, m.pers, Icon::Persistant, 269, 4, U"Element paths persist on the screen for a while");
    box(Kind::Colour, m.heat, Icon::Heat, 307, 4,
        U"Displays temperatures of the elements, dark blue is coldest, pink is hottest");
    box(Kind::Colour, m.life, Icon::Life, 307, 22, U"Displays the life value of elements in greyscale gradients");
    box(Kind::Colour, m.grad, Icon::Gradient, 339, 22,
        U"Changes colors of elements slightly to show heat diffusing through them");
    box(Kind::Colour, m.cbasc, Icon::Basic, 339, 4,
        U"No special effects at all for anything, overrides all other options and deco");
    sync();
}

void RenderOptionsWindow::sync() {
    for (ModeBox &mb : boxes_) {
        if (mb.kind == Kind::Render) // (compares bit masks: modes that others include show as on with them)
            mb.box->checked = (view_.renderMode & mb.mode) == mb.mode;
        else if (mb.kind == Kind::Display)
            mb.box->checked = (view_.displayMode & mb.mode) == mb.mode;
        else
            mb.box->checked = view_.colorMode == mb.mode;
    }
}

void RenderOptionsWindow::loadPreset(int preset) {
    const ViewSettings p = tptPresetView(preset);
    view_.renderMode = p.renderMode, view_.displayMode = p.displayMode, view_.colorMode = p.colorMode;
    view_.autoHeat = p.autoHeat, view_.hdispMin = p.hdispMin, view_.hdispMax = p.hdispMax;
    sync();
}

// (the world shows where the canvas is left clear)
void RenderOptionsWindow::onDraw(Canvas &c) {
    c.drawFilledRect(xres_, 0, c.width() - xres_, c.height(), 0x000000);
    c.drawFilledRect(0, yres_, xres_, c.height() - yres_, 0x000000);
    c.drawLine(0, yres_, xres_ - 1, yres_, 0xC8C8C8);
    for (int x : lines_)
        c.drawLine(x, yres_, x, size.y, 0xC8C8C8);
    c.drawLine(xres_, 0, xres_, size.y, 0xFFFFFF);
    if (tipPresence_ > 0 && !tip_.empty()) {
        const int alpha = tipPresence_ > 51 ? 255 : int(tipPresence_) * 5;
        const int tx = 6, ty = size.y - 40 - 12; // (Size.Y - MENUSIZE - 12)
        int tw, th;
        c.textSize(tip_, tw, th);
        c.blendFilledRect(tx - 4, ty - 4, tw + 8, 15, rgb(0x000000, uint8_t(alpha / 2)));
        c.blendText(tx, ty, tip_, rgb(0xFFFFFF, uint8_t(alpha)));
    }
}

void RenderOptionsWindow::onTick() {
    const double now = ui::host().now, ms = lastTick_ < 0 ? 0 : now - lastTick_;
    lastTick_ = now;
    tipPresence_ = tipIn_ ? std::min(120.0, tipPresence_ + ms * 0.12) : std::max(0.0, tipPresence_ - ms * 0.06);
    tipIn_ = false;
}

void RenderOptionsWindow::toolTip(ui::Point, const std::u32string &text) {
    tip_ = text;
    tipIn_ = true;
}

void RenderOptionsWindow::onMouseDown(int x, int y, int) {
    if (x > xres_ || y < yres_)
        close();
}

void RenderOptionsWindow::onKeyPress(int key, int, bool repeat, bool shift, bool, bool) {
    if (repeat)
        return;
    if (shift && key == int(SDLK_1))
        loadPreset(10);
    else if (key >= int(SDLK_0) && key <= int(SDLK_9))
        loadPreset(key - int(SDLK_0));
}

DirectionWindow::DirectionWindow(float scale, int radius, float x, float y, const std::u32string &label,
                                 std::function<void(float, float)> onDone)
    : ui::Window(ui::Point{-1, -1}, ui::Point{radius * 5 / 2 + 20, radius * 5 / 2 + 75}) {
    auto title = std::make_unique<ui::Label>(ui::Point{4, 1}, ui::Point{size.x - 8, 22}, label);
    title->setTextColour(ui::style::InformationTitle);
    title->appearance.hAlign = ui::Appearance::HAlign::Left;
    title->appearance.vAlign = ui::Appearance::VAlign::Middle;
    add(std::move(title));
    add(std::make_unique<ui::Separator>(ui::Point{0, 22}, ui::Point{size.x, 1}));
    auto text = [](float vx, float vy) {
        char b[96];
        std::snprintf(b, sizeof b, "X:%.1f Y:%.1f Total:%.1f", double(vx), double(vy), double(std::hypot(vx, vy)));
        return utf32(b);
    };
    auto values = std::make_unique<ui::Label>(ui::Point{0, radius * 5 / 2 + 37}, ui::Point{size.x, 16}, text(x, y));
    values->appearance.hAlign = ui::Appearance::HAlign::Centre;
    values->appearance.vAlign = ui::Appearance::VAlign::Middle;
    values_ = add(std::move(values));
    auto dir = std::make_unique<ui::DirectionSelector>(ui::Point{10, 32}, scale, radius, radius / 4, 2, 5);
    dir->setValues(x, y);
    dir->update = [this, text](float vx, float vy) { values_->setText(text(vx, vy)); };
    dir->setSnapPoints(5, 5, 2);
    ui::DirectionSelector *d = add(std::move(dir));
    auto ok = std::make_unique<ui::Button>(ui::Point{0, size.y - 17}, ui::Point{size.x, 17}, U"OK");
    ok->appearance.borderInactive = rgb(0xC8C8C8);
    ok->action = [this, d, onDone] {
        onDone(d->xValue(), d->yValue());
        close();
    };
    okayButton = add(std::move(ok));
}

void DirectionWindow::onDraw(Canvas &c) {
    c.drawFilledRect(position.x - 1, position.y - 1, size.x + 2, size.y + 2, 0x000000);
    c.drawRect(position.x, position.y, size.x, size.y, 0xC8C8C8);
}

namespace {
constexpr float MIN_TEMP_ = 0.0f, MAX_TEMP_ = 9999.0f, MIN_PRESSURE_ = -256.0f, MAX_PRESSURE_ = 256.0f;
} // namespace

OptionsWindow::OptionsWindow(const OptionValues &values, std::function<void(const OptionValues &)> onChange,
                             std::function<void()> openDataFolder)
    : ui::Window(ui::Point{-1, -1}, ui::Point{320, 340}), v_(values), onChange_(std::move(onChange)) {
    auto autoWidth = [this](ui::Component *c, int extra) { c->size.x = size.x - c->position.x - 12 - extra; };
    {
        auto t = std::make_unique<ui::Label>(ui::Point{4, 1}, ui::Point{size.x - 8, 22}, U"Settings");
        t->setTextColour(ui::style::InformationTitle);
        t->appearance.hAlign = ui::Appearance::HAlign::Left;
        t->appearance.vAlign = ui::Appearance::VAlign::Middle;
        autoWidth(t.get(), 0);
        add(std::move(t));
    }
    add(std::make_unique<ui::Separator>(ui::Point{0, 22}, ui::Point{size.x, 1}));
    panel_ = add(std::make_unique<ui::ScrollPanel>(ui::Point{1, 23}, ui::Point{size.x - 2, size.y - 39}));

    int y = 8;
    auto addLabel = [&](int indent, const std::u32string &text) {
        auto l = std::make_unique<ui::Label>(ui::Point{22 + indent * 15, y}, ui::Point{1, 16}, U"");
        autoWidth(l.get(), 0);
        l->appearance.hAlign = ui::Appearance::HAlign::Left;
        l->appearance.vAlign = ui::Appearance::VAlign::Middle;
        l->setMultiline(true);
        l->setText(U"\bg" + text);
        l->autoHeight();
        ui::Label *p = panel_->add(std::move(l));
        y += p->size.y - 1;
        return p;
    };
    auto addCheckbox = [&](int indent, const std::u32string &text, const std::u32string &info, bool checked,
                           std::function<void(bool)> act) {
        auto b = std::make_unique<ui::Checkbox>(ui::Point{8 + indent * 15, y}, ui::Point{1, 16}, text);
        autoWidth(b.get(), 0);
        b->checked = checked;
        ui::Checkbox *p = b.get();
        b->action = [this, p, act] {
            act(p->checked);
            changed();
        };
        y += 14;
        if (!info.empty())
            addLabel(indent, info);
        y += 4;
        panel_->add(std::move(b));
        return p;
    };
    auto addDropDown = [&](const std::u32string &info, std::vector<std::pair<std::u32string, int>> options, int value,
                           std::function<void(int)> act) {
        auto d = std::make_unique<ui::DropDown>(ui::Point{size.x - 95, y}, ui::Point{80, 16});
        for (auto &[text, v] : options)
            d->addOption(text, v);
        d->setOption(value);
        ui::DropDown *p = d.get();
        d->change = [this, p, act] {
            act(p->option().second);
            changed();
        };
        panel_->add(std::move(d));
        auto l = std::make_unique<ui::Label>(ui::Point{8, y}, ui::Point{size.x - 96, 16}, info);
        l->appearance.hAlign = ui::Appearance::HAlign::Left;
        l->appearance.vAlign = ui::Appearance::VAlign::Middle;
        autoWidth(l.get(), 85);
        panel_->add(std::move(l));
        y += 20;
        return p;
    };
    auto addTextbox = [&](const std::u32string &info, bool preview, std::function<void(bool)> update,
                          ui::Button **previewButton) {
        auto t = std::make_unique<ui::Textbox>(ui::Point{size.x - 95, y}, ui::Point{80, 16});
        t->setLimit(9);
        t->change = [update] { update(false); };
        t->defocus = [update] { update(true); };
        if (preview) {
            t->size.x -= 20;
            auto b = std::make_unique<ui::Button>(ui::Point{size.x - 31, y}, ui::Point{16, 16}, U"", U"Preview");
            *previewButton = panel_->add(std::move(b));
        }
        ui::Textbox *p = panel_->add(std::move(t));
        auto l = std::make_unique<ui::Label>(ui::Point{8, y}, ui::Point{size.x - 105, 16}, info);
        l->appearance.hAlign = ui::Appearance::HAlign::Left;
        l->appearance.vAlign = ui::Appearance::VAlign::Middle;
        panel_->add(std::move(l));
        y += 20;
        return p;
    };
    auto addLimit = [&](const std::u32string &info, std::vector<std::pair<std::u32string, int>> options,
                        std::function<void(bool)> act, ui::DropDown **drop) {
        auto d = std::make_unique<ui::DropDown>(ui::Point{size.x - 155, y}, ui::Point{100, 16});
        for (auto &[text, v] : options)
            d->addOption(text, v);
        d->change = [act] { act(true); };
        *drop = panel_->add(std::move(d));
        auto t = std::make_unique<ui::Textbox>(ui::Point{size.x - 51, y}, ui::Point{36, 16});
        t->change = [act] { act(false); };
        t->defocus = [act] { act(true); };
        t->setLimit(4);
        ui::Textbox *p = panel_->add(std::move(t));
        auto l = std::make_unique<ui::Label>(ui::Point{8, y}, ui::Point{size.x - 96, 16}, info);
        l->appearance.hAlign = ui::Appearance::HAlign::Left;
        l->appearance.vAlign = ui::Appearance::VAlign::Middle;
        autoWidth(l.get(), 105);
        panel_->add(std::move(l));
        y += 20;
        return p;
    };
    auto addSeparator = [&] {
        y += 6;
        panel_->add(std::make_unique<ui::Separator>(ui::Point{0, y}, ui::Point{size.x, 1}));
        y += 11;
    };
    auto addButtonWithLabel = [&](const std::u32string &text, const std::u32string &label, std::function<void()> act) {
        auto b = std::make_unique<ui::Button>(ui::Point{10, y}, ui::Point{90, 16}, text);
        b->action = act;
        panel_->add(std::move(b));
        ui::Label *l = addLabel(5, label);
        y += 13;
        return l;
    };

    addCheckbox(0, U"Heat simulation \bgIntroduced in version 34", U"Can cause odd behaviour when disabled", v_.sim.heat,
                [this](bool on) { v_.sim.heat = on; });
    addCheckbox(0, U"Newtonian gravity \bgIntroduced in version 48", U"May cause poor performance on older computers",
                v_.sim.newtonianGravity, [this](bool on) { v_.sim.newtonianGravity = on; });
    addCheckbox(0, U"Ambient heat simulation \bgIntroduced in version 50",
                U"Can cause odd / broken behaviour with many saves", v_.sim.ambientHeat,
                [this](bool on) { v_.sim.ambientHeat = on; });
    addCheckbox(0, U"Water equalisation \bgIntroduced in version 61", U"May cause poor performance with a lot of water",
                v_.sim.waterEqualisation, [this](bool on) { v_.sim.waterEqualisation = on; });
    addDropDown(U"Air simulation mode",
                {{U"On", 0}, {U"Pressure off", 1}, {U"Velocity off", 2}, {U"Off", 3}, {U"No update", 4}}, v_.sim.airMode,
                [this](int m) { v_.sim.airMode = m; });
    airTemp_ = addTextbox(U"Ambient air temperature", true, [this](bool d) { updateAirTemp(d); }, &airTempPreview_);
    pressure_ = addTextbox(U"Ambient air pressure", true, [this](bool d) { updatePressure(d); }, &pressurePreview_);
    {
        auto b = std::make_unique<ui::Button>(ui::Point{size.x - 95, y}, ui::Point{80, 16}, U"Change");
        b->action = [this] {
            openPending = std::make_unique<DirectionWindow>(0.05f, 40, v_.sim.edgeVX, v_.sim.edgeVY,
                                                            U"Ambient air velocity", [this](float x, float vy) {
                                                                v_.sim.edgeVX = x, v_.sim.edgeVY = vy;
                                                                changed();
                                                            });
        };
        panel_->add(std::move(b));
        auto l = std::make_unique<ui::Label>(ui::Point{8, y}, ui::Point{size.x - 96, 16}, U"Ambient air velocity");
        l->appearance.hAlign = ui::Appearance::HAlign::Left;
        l->appearance.vAlign = ui::Appearance::VAlign::Middle;
        panel_->add(std::move(l));
        y += 20;
    }
    vorticity_ = addTextbox(U"Vorticity confinement", false, [this](bool d) { updateVorticity(d); }, nullptr);
    addDropDown(U"Air heat convection mode", {{U"None", 0}, {U"Legacy", 1}, {U"Boussinesq", 2}}, v_.sim.convectionMode,
                [this](int m) { v_.sim.convectionMode = m; });
    addDropDown(U"Gravity simulation mode", {{U"Vertical", 0}, {U"Off", 1}, {U"Radial", 2}, {U"Custom", 3}},
                v_.sim.gravityMode, [this](int m) {
                    v_.sim.gravityMode = m;
                    if (m == 3)
                        openPending = std::make_unique<DirectionWindow>(
                            0.05f, 40, v_.sim.customGravX, v_.sim.customGravY, U"Custom Gravity", [this](float x, float gy) {
                                v_.sim.customGravX = x, v_.sim.customGravY = gy;
                                changed();
                            });
                });
    addDropDown(U"Edge mode", {{U"Void", 0}, {U"Solid", 1}, {U"Loop", 2}}, v_.sim.edgeMode,
                [this](int m) { v_.sim.edgeMode = m; });
    addDropDown(U"Temperature scale", {{U"Kelvin", 0}, {U"Celsius", 1}, {U"Fahrenheit", 2}}, v_.temperatureScale,
                [this](int s) {
                    v_.temperatureScale = s;
                    refresh();
                });
    addSeparator();
    fpsText_ = addLimit(U"Simulation framerate cap", {{U"Exact", 0}, {U"Uncapped", 1}}, [this](bool d) { updateFps(d); },
                        &fps_);
    drawText_ = addLimit(U"Rendering framerate cap", {{U"Exact", 0}, {U"Follow display", 1}},
                         [this](bool d) { updateDraw(d); }, &draw_);
    addButtonWithLabel(U"Reset", U" - Set both limits to sane defaults", [this] {
        v_.simUncapped = false, v_.simFps = 60.0f;
        v_.drawFollowDisplay = false, v_.drawFps = 60;
        refresh();
        changed();
    });
    y -= 6;
    addSeparator();
    {
        // (Big Powder's own first: from the window's height)
        std::vector<std::pair<std::u32string, int>> options{{U"Auto", 0}};
        for (int k = 1; k <= std::max(1, v_.maxScale); k++)
            options.push_back({utf32(std::to_string(k)), k});
        if (v_.scale > std::max(1, v_.maxScale))
            options.push_back({U"current", v_.scale});
        scaleBox_ = addDropDown(U"Window scale factor for larger screens", options, v_.scale,
                                [this](int s) { v_.scale = s; });
    }
    addCheckbox(0, U"Fullscreen \bg- fill the entire screen", U"", v_.fullscreen, [this](bool on) { v_.fullscreen = on; });
    addSeparator();
    addCheckbox(0, U"Fast quit", U"Always exit completely when hitting close", v_.fastQuit,
                [this](bool on) { v_.fastQuit = on; });
    addCheckbox(0, U"Sticky categories", U"Switch between categories by clicking", v_.stickyCategories,
                [this](bool on) { v_.stickyCategories = on; });
    addCheckbox(0, U"Include pressure", U"When saving, copying, stamping, etc.", v_.includePressure,
                [this](bool on) { v_.includePressure = on; });
    addCheckbox(0, U"Perfect circle brush", U"Better circle brush, without incorrect points on edges", v_.perfectCircle,
                [this](bool on) { v_.perfectCircle = on; });
    addDropDown(U"Colour space used by decoration tools",
                {{U"sRGB", 0}, {U"Linear", 1}, {U"Gamma 2.2", 2}, {U"Gamma 1.8", 3}}, v_.decoSpace,
                [this](int s) { v_.decoSpace = s; });
    y += 4;
    {
        auto b = std::make_unique<ui::Button>(ui::Point{10, y}, ui::Point{90, 16}, U"Open data folder");
        b->action = openDataFolder;
        panel_->add(std::move(b));
        y += 26;
    }

    auto ok = std::make_unique<ui::Button>(ui::Point{0, size.y - 16}, ui::Point{size.x, 16}, U"OK");
    ok->action = [this] { close(); };
    ui::Button *okp = add(std::move(ok));
    cancelButton = okayButton = okp;
    panel_->innerSize = ui::Point{size.x, y};
    refresh();
}

// NotifySettingsChanged: the text boxes and previews from the settings (not one that is being typed into)
void OptionsWindow::refresh() {
    if (!isFocused(airTemp_)) {
        airTemp_->setText(tptTemperatureText(v_.sim.ambientTemp, v_.temperatureScale));
        airTempPreview_->appearance.backgroundInactive = airTempPreview_->appearance.backgroundHover =
            rgb(tptHeatColour(v_.sim.ambientTemp));
        airTempPreview_->setText(U"");
    }
    auto fixed2 = [](float f) {
        char b[48];
        std::snprintf(b, sizeof b, "%.2f", double(f));
        return utf32(b);
    };
    if (!isFocused(pressure_)) {
        pressure_->setText(fixed2(v_.sim.edgePressure));
        pressurePreview_->appearance.backgroundInactive = pressurePreview_->appearance.backgroundHover =
            rgb(tptPressureColour(v_.sim.edgePressure));
        pressurePreview_->setText(U"");
    }
    if (!isFocused(vorticity_))
        vorticity_->setText(fixed2(v_.sim.vorticity));
    if (!isFocused(fpsText_)) {
        fps_->setOption(v_.simUncapped ? 1 : 0);
        fpsText_->enabled = !v_.simUncapped;
        char b[32];
        std::snprintf(b, sizeof b, "%g", double(v_.simFps));
        fpsText_->setText(v_.simUncapped ? U"" : utf32(b));
    }
    if (!isFocused(drawText_)) {
        draw_->setOption(v_.drawFollowDisplay ? 1 : 0);
        drawText_->enabled = !v_.drawFollowDisplay;
        drawText_->setText(v_.drawFollowDisplay ? U"" : utf32(std::to_string(v_.drawFps)));
    }
}

// UpdateSettingFromString's way: while typing, a value in range applies and out of range ones go unpreviewed; on
// leaving the box, an empty one takes the default and others are brought into range
void OptionsWindow::updateAirTemp(bool defocus) {
    float t = 0;
    bool valid = tptParseTemperature(airTemp_->text(), v_.temperatureScale, t);
    if (defocus) {
        if (airTemp_->text().empty())
            valid = true, t = 22.0f + 273.15f;
        else if (!valid)
            return;
        t = std::clamp(t, MIN_TEMP_, MAX_TEMP_);
        airTemp_->setText(tptTemperatureText(t, v_.temperatureScale));
    } else if (valid && (t < MIN_TEMP_ || t > MAX_TEMP_))
        valid = false;
    if (valid) {
        v_.sim.ambientTemp = t;
        changed();
    }
    airTempPreview_->appearance.backgroundInactive = airTempPreview_->appearance.backgroundHover =
        rgb(valid ? tptHeatColour(t) : 0x000000);
    airTempPreview_->setText(valid ? U"" : U"?");
}

void OptionsWindow::updatePressure(bool defocus) {
    float p = 0;
    bool valid = tptParseNumber(pressure_->text(), p);
    if (defocus) {
        if (pressure_->text().empty())
            valid = true, p = 0;
        else if (!valid)
            return;
        p = std::clamp(p, MIN_PRESSURE_, MAX_PRESSURE_);
        char b[48];
        std::snprintf(b, sizeof b, "%.2f", double(p));
        pressure_->setText(utf32(b));
    } else if (valid && (p < MIN_PRESSURE_ || p > MAX_PRESSURE_))
        valid = false;
    if (valid) {
        v_.sim.edgePressure = p;
        changed();
    }
    pressurePreview_->appearance.backgroundInactive = pressurePreview_->appearance.backgroundHover =
        rgb(valid ? tptPressureColour(p) : 0x000000);
    pressurePreview_->setText(valid ? U"" : U"?");
}

void OptionsWindow::updateVorticity(bool defocus) {
    float v = 0;
    bool valid = tptParseNumber(vorticity_->text(), v);
    if (defocus) {
        if (vorticity_->text().empty())
            valid = true, v = 0.1f;
        else if (!valid)
            return;
        v = std::clamp(v, 0.0f, 1.0f);
        char b[48];
        std::snprintf(b, sizeof b, "%.2f", double(v));
        vorticity_->setText(utf32(b));
    } else if (valid && (v < 0.0f || v > 1.0f))
        valid = false;
    if (valid) {
        v_.sim.vorticity = v;
        changed();
    }
}

void OptionsWindow::updateFps(bool defocus) {
    if (fps_->option().second != 0) {
        fpsText_->enabled = false;
        fpsText_->setText(U"");
        v_.simUncapped = true;
        changed();
        return;
    }
    v_.simUncapped = false;
    fpsText_->enabled = true;
    float f = 0;
    bool valid = tptParseNumber(fpsText_->text(), f);
    if (defocus) {
        if (fpsText_->text().empty())
            valid = true, f = 60.0f;
        else if (!valid)
            return;
        f = std::clamp(f, 3.0f, 1000.0f); // (FpsLimitExplicit's sane range)
        char b[32];
        std::snprintf(b, sizeof b, "%g", double(f));
        fpsText_->setText(utf32(b));
    } else if (valid && (f < 3.0f || f > 1000.0f))
        valid = false;
    if (valid) {
        v_.simFps = f;
        changed();
    }
}

void OptionsWindow::updateDraw(bool defocus) {
    if (draw_->option().second != 0) {
        drawText_->enabled = false;
        drawText_->setText(U"");
        v_.drawFollowDisplay = true;
        changed();
        return;
    }
    v_.drawFollowDisplay = false;
    drawText_->enabled = true;
    int f = 0;
    bool valid = tptParseNumber(drawText_->text(), f);
    if (defocus) {
        if (drawText_->text().empty())
            valid = true, f = 60;
        else if (!valid)
            return;
        f = std::clamp(f, 1, 1000);
        drawText_->setText(utf32(std::to_string(f)));
    } else if (valid && (f < 1 || f > 1000))
        valid = false;
    if (valid) {
        v_.drawFps = f;
        changed();
    }
}

void OptionsWindow::onDraw(Canvas &c) {
    c.drawFilledRect(position.x - 1, position.y - 1, size.x + 2, size.y + 2, 0x000000);
    c.drawRect(position.x, position.y, size.x, size.y, 0xFFFFFF);
}

} // namespace bp
