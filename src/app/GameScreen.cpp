#include "app/GameScreen.h"
#include "app/appreports.h"
#include "app/OnlineSaves.h"
#include "classic/TptSave.h"
#include "sim/WorldFile.h"
#include "ui/BrushCursor.h"
#include "ui/Dialogs.h"
#include "sim/RenderTables.h"

#include <SDL3/SDL_clipboard.h>
#include <SDL3/SDL_misc.h>
#include <SDL3/SDL_stdinc.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <thread>

namespace bp {

void Camera::fit(uint32_t sw, uint32_t sh, uint32_t W, uint32_t H) {
    zoom = std::min(float(sw) / float(W), float(sh) / float(H));
    x = (float(W) - float(sw) / zoom) * 0.5f;
    y = (float(H) - float(sh) / zoom) * 0.5f;
}

void Camera::zoomAt(float sx, float sy, float factor) {
    float wx = x + sx / zoom, wy = y + sy / zoom;
    zoom = std::clamp(zoom * factor, 1.0f / 16.0f, 256.0f);
    x = wx - sx / zoom;
    y = wy - sy / zoom;
}

std::string dataDir() {
    const char *home = std::getenv("HOME");
    return (std::filesystem::path(home ? home : ".") / ".local/share/big-powder").string();
}

namespace {

std::string savesDir(const AppOptions &o) {
    std::filesystem::path dir = std::filesystem::path(o.dataDir.empty() ? dataDir() : o.dataDir) / "saves";
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    return dir.string();
}

std::string timestamp() {
    std::time_t t = std::time(nullptr);
    char buf[32];
    std::strftime(buf, sizeof buf, "%Y%m%d-%H%M%S", std::localtime(&t));
    return buf;
}

std::unique_ptr<World> makeWorld(VkContext &ctx, const AppOptions &o, const std::string &mode) {
    if (mode == "classic")
        return makeClassicWorld(ctx, o);
    if (mode == "huge")
        return makeGpuWorld(ctx, o, o.sizeSet ? o.W : 7680, o.sizeSet ? o.H : 4320, "huge");
    return makeGpuWorld(ctx, o, o.W, o.H, "screen");
}

bool isBpw(const std::string &path) { return path.size() > 4 && path.compare(path.size() - 4, 4, ".bpw") == 0; }

} // namespace

GameScreen::GameScreen(VkContext &ctx, const AppOptions &o)
    : ctx_(ctx), o_(o), dataRoot_(o.dataDir.empty() ? dataDir() : o.dataDir), prefs_(dataRoot_ + "/powder.pref"),
      mode_(o.mode), scenes_(sceneNames()), uiTable_(ElementTable::build()), canvas_(uiTable_),
      ui_(uiTable_), overlay_(ctx, o.shaderDir), input_(ui_), signLayer_(uiTable_),
      stamps_((o.dataDir.empty() ? dataDir() : o.dataDir) + "/stamps"), tps_(o.tps) {
    {
        std::error_code ec;
        std::filesystem::create_directories(dataRoot_, ec);
    }
    // A world file opens as a GPU world of its own size
    AppOptions start = o;
    if (isBpw(o.loadPath)) {
        WorldFileInfo info;
        std::string err;
        if (readWorldInfo(o.loadPath, info, err)) {
            start.W = info.W;
            start.H = info.H;
            start.sizeSet = true;
            if (mode_ == "classic")
                mode_ = uint64_t(info.W) * info.H > uint64_t(3840) * 2160 ? "huge" : "screen";
        }
    }
    world_ = makeWorld(ctx_, start, mode_);
    if (!o.scenes.empty()) {
        auto it = std::find(scenes_.begin(), scenes_.end(), o.scenes[0]);
        sceneIdx_ = it == scenes_.end() ? 0 : size_t(it - scenes_.begin());
    }
    if (o.loadPath.empty())
        loadScene();
    else {
        std::string msg;
        world_->loadSave(o.loadPath, int(world_->width() / 2), int(world_->height() / 2), msg);
        report(msg);
    }
    saveName_ = o.loadPath.empty() ? scenes_[sceneIdx_] : std::filesystem::path(o.loadPath).filename().string();
    loadPrefs();
    // (the command line's settings over the prefs)
    if (o.edgeMode)
        settings_.edgeMode = o.edgeMode;
    if (o.gravityMode)
        settings_.gravityMode = o.gravityMode;
    if (o.ambientHeat)
        settings_.ambientHeat = true;
    if (o.newtonian)
        settings_.newtonianGravity = true;
    if (o.uiScale > 0)
        uiScale_ = o.uiScale;
    if (o.tps != 60.0)
        tps_ = o.tps, simUncapped_ = false;
    world_->applySettings(settings_);

    ui::host().font = &canvas_;
    ui::host().clipboardGet = [] {
        char *t = SDL_GetClipboardText();
        std::u32string text = utf32(t ? t : "");
        SDL_free(t);
        return text;
    };
    ui::host().clipboardSet = [](const std::u32string &text) { SDL_SetClipboardText(utf8(text).c_str()); };
}

GameScreen::~GameScreen() {
    savePrefs();
    vkDeviceWaitIdle(ctx_.device());
    for (Buffer &b : frozen_)
        ctx_.destroyBuffer(b);
    if (ui::host().font == &canvas_)
        ui::host().font = nullptr;
}

void GameScreen::report(const std::string &msg) {
    appreports::log(msg);
    std::printf("%s\n", msg.c_str());
    std::fflush(stdout);
}

void GameScreen::loadScene() { world_->loadScene(scenes_[sceneIdx_]); }

void GameScreen::switchMode(const std::string &mode) {
    vkDeviceWaitIdle(ctx_.device());
    history_.clear(); // (its snapshots are the old world's)
    selectMode_ = Select::None;
    placeSave_.reset(), placed_.reset();
    world_.reset();   // only one TPT engine may exist at a time
    mode_ = mode;
    world_ = makeWorld(ctx_, o_, mode_);
    world_->applySettings(settings_);
    world_->setCustomGol(customGol_);
    loadScene();
    fitView();
    sinceStats_ = 1e9;
}

int GameScreen::uiScale() const {
    if (uiScale_ > 0)
        return uiScale_;
    return std::max(1, int(std::lround(double(sh_) / 700.0)));
}

void GameScreen::resize(uint32_t w, uint32_t h) {
    bool first = sw_ == 0;
    sw_ = w;
    sh_ = h;
    if (first)
        fitView();
}

// The world shows left of the bar and above the bottom area
void GameScreen::fitView() {
    int s = uiScale();
    uint32_t vw = uint32_t(std::max(1, int(sw_) - GameUi::BARSIZE * s));
    uint32_t vh = uint32_t(std::max(1, int(sh_) - GameUi::MENUSIZE * s));
    cam_.fit(vw, vh, world_->width(), world_->height());
}

void GameScreen::toWorld(float sx, float sy, float &wx, float &wy) const {
    wx = cam_.x + sx / cam_.zoom;
    wy = cam_.y + sy / cam_.zoom;
}

bool GameScreen::inWorld(float sx, float sy) const {
    float wx, wy;
    toWorld(sx, sy, wx, wy);
    return wx >= 0 && wy >= 0 && wx < float(world_->width()) && wy < float(world_->height());
}

void GameScreen::worldPoint(float sx, float sy, int &x, int &y) const {
    float wx, wy;
    toWorld(sx, sy, wx, wy);
    x = std::clamp(int(std::floor(wx)), 0, int(world_->width()) - 1);
    y = std::clamp(int(std::floor(wy)), 0, int(world_->height()) - 1);
}

void GameScreen::showWindow(std::unique_ptr<ui::Window> w) {
    windows_.show(std::move(w), ui_.width(), ui_.height());
}

// PropertyTool::OpenWindow: from what was set last, or with a particle's value of the property being set
void GameScreen::openPropertyWindow(const ClassicParticle *from) {
    int field = propertyField_;
    std::u32string text = propertyText_;
    if (from && input_.property) {
        field = input_.property->field;
        text = tptPropertyText(particleProperty(*from, field), temperatureScale_);
    }
    showWindow(std::make_unique<PropertyWindow>(field, text, temperatureScale_,
                                                [this](const ParticleProperty &p, int f, const std::u32string &t) {
                                                    input_.property = p;
                                                    propertyField_ = f;
                                                    propertyText_ = t;
                                                    Prefs::Defer defer(prefs_);
                                                    prefs_.setInt("Prop.Type", f);
                                                    prefs_.setString("Prop.Value", utf8(t));
                                                }));
}

void GameScreen::openElementSearch() {
    showWindow(std::make_unique<ElementSearchWindow>(ui_, [this](int slot, int tool) { ui_.chooseTool(slot, tool); }));
}

void GameScreen::openGolWindow(int slot, int rule, uint32_t colour1, uint32_t colour2) {
    showWindow(std::make_unique<GolWindow>(
        rule, colour1, colour2, lastGolName_, lastGolRule_,
        [this, slot](const std::u32string &name, const std::u32string &ruleText, uint32_t c1, uint32_t c2) {
            addCustomGol(slot, name, ruleText, c1, c2);
        }));
}

void GameScreen::addCustomGol(int slot, std::u32string name, const std::u32string &ruleText, uint32_t colour1,
                              uint32_t colour2) {
    auto error = [this](const char32_t *message) {
        showWindow(std::make_unique<ErrorMessage>(U"Could not add GOL type", message));
    };
    if (!tptValidGolName(name))
        return error(U"Invalid name provided");
    for (char32_t &c : name)
        if (c >= U'a' && c <= U'z')
            c = c - U'a' + U'A';
    int rule = tptParseGolRule(ruleText);
    if (rule == -1)
        return error(U"Invalid rule provided");
    for (const CustomGol &g : customGol_)
        if (g.rule == rule)
            return error(U"This Custom GoL rule already exists");
    lastGolName_ = name;
    lastGolRule_ = tptGolRuleString(rule); // (made canonical)
    {
        Prefs::Defer defer(prefs_);
        prefs_.setString("CustomGOL.Name", utf8(lastGolName_));
        prefs_.setString("CustomGOL.Rule", utf8(lastGolRule_));
    }
    for (const CustomGol &g : customGol_) // (GameModel::CheckCustomGol)
        if (g.name == name)
            return error(U"Name already taken");
    CustomGol type{name, rule, colour1, colour2};
    customGol_.push_back(type);
    world_->setCustomGol(customGol_);
    ui_.addCustomGolTool(type);
    saveCustomGol();
    // (SelectNextIdentifier: GameModel::SetActiveTool on the button the window was opened for)
    ui_.setActiveToolIndex(slot, ui_.toolIndex("DEFAULT_PT_LIFECUST_" + utf8(name)));
}

// GameController::GetSignAt: the last sign whose box (as it is drawn) holds the point
int GameScreen::signAt(int x, int y) {
    std::vector<Sign> signs = world_->signs();
    if (signs.empty())
        return -1;
    std::vector<SignSample> samples = world_->signSamples(false);
    for (int i = int(signs.size()) - 1; i >= 0; i--) {
        int sx, sy, sw, sh;
        signDisplayText(uiTable_, signs[size_t(i)], size_t(i) < samples.size() ? samples[size_t(i)] : SignSample{}, sx,
                        sy, sw, sh);
        if (x >= sx && x <= sx + sw && y >= sy && y <= sy + sh)
            return i;
    }
    return -1;
}

// GameController::MouseDown: a press on a link sign (unless it is the sign tool's left button) is the sign's, not the
// tool's
bool GameScreen::linkSignPress(int x, int y, int button) {
    foundSign_ = -1;
    const UiTool *t = ui_.activeTool(0);
    if (t && t->kind == UiTool::Kind::Sign && button == 0)
        return false;
    foundSign_ = signAt(x, y);
    if (foundSign_ < 0)
        return false;
    std::vector<Sign> signs = world_->signs();
    return signSplit(signs[size_t(foundSign_)].text).first != 0;
}

// GameController::MouseUp: letting go on a link sign follows it (a button sign sparks its spot)
bool GameScreen::linkSignRelease(int x, int y, int button) {
    if (foundSign_ < 0)
        return false;
    foundSign_ = -1;
    const UiTool *t = ui_.activeTool(0);
    if (t && t->kind == UiTool::Kind::Sign && button == 0)
        return false;
    int id = signAt(x, y);
    if (id < 0)
        return false;
    const Sign s = world_->signs()[size_t(id)];
    auto [at, type] = signSplit(s.text);
    if (!at)
        return false;
    const std::u32string arg = s.text.substr(3, at - 3);
    switch (type) {
    case BUTTON: {
        ToolUse u;
        u.action = ToolUse::Action::Create;
        u.tool = PT_SPRK;
        u.x1 = u.x2 = s.x, u.y1 = u.y2 = s.y;
        pendingUses_.push_back(u);
        break;
    }
    case THREAD: // (Platform::OpenURI)
        SDL_OpenURL(("https://powdertoy.co.uk/Discussions/Thread/View.html?Thread=" + utf8(arg)).c_str());
        break;
    case SAVE:
    case SEARCH:
        report("online saves aren't here yet: " + utf8(s.text));
        break;
    default:
        break;
    }
    return true;
}

// GameView::OnTick: a link sign under the mouse says where it goes
void GameScreen::signTooltip() {
    if (!inWorld(mouseX_, mouseY_))
        return;
    int x, y;
    worldPoint(mouseX_, mouseY_, x, y);
    int id = signAt(x, y);
    if (id < 0)
        return;
    const std::u32string text = world_->signs()[size_t(id)].text;
    auto [at, type] = signSplit(text);
    if (!at)
        return;
    const std::u32string arg = text.substr(3, at - 3);
    if (type == SAVE)
        ui_.showButtonTip(U"Go to save ID:" + arg);
    else if (type == THREAD)
        ui_.showButtonTip(U"Open forum thread " + arg + U" in browser");
    else if (type == SEARCH)
        ui_.showButtonTip(U"Search for " + arg);
}

// SignTool::Click: the first sign whose box holds the point is edited; anywhere else a new one is made (16 at most)
void GameScreen::signToolClick(int x, int y) {
    std::vector<Sign> signs = world_->signs();
    windowSamples_ = world_->signSamples(true);
    int index = -1;
    for (size_t i = 0; i < signs.size(); i++) {
        int sx, sy, sw, sh;
        signDisplayText(uiTable_, signs[i], i < windowSamples_.size() ? windowSamples_[i] : SignSample{}, sx, sy, sw, sh);
        if (x > sx && x < sx + sw && y > sy && y < sy + sh) {
            index = int(i);
            break;
        }
    }
    if (index == -1 && signs.size() >= 16) // (MAXSIGNS)
        return;
    showWindow(std::make_unique<SignWindow>(
        signs, index, x, y,
        [this](const std::vector<Sign> &changed) {
            world_->setSigns(changed);
            windowSamples_ = world_->signSamples(true);
        },
        [this](int ux, int uy, int &wx, int &wy) {
            // GameModel::AdjustZoomCoords, and only over the simulation
            if (!ui_.overWorld(ux, uy))
                return false;
            const float us = float(uiScale());
            float sx = (float(ux) + 0.5f) * us, sy = (float(uy) + 0.5f) * us;
            if (!inWorld(sx, sy))
                return false;
            worldPoint(sx, sy, wx, wy);
            return true;
        }));
}

void GameScreen::drawSignLayer(VkCommandBuffer cmd, int slot) {
    const std::vector<Sign> signs = world_->signs();
    struct Drawn {
        std::u32string text;
        int x, y, w, h;
    };
    std::vector<Drawn> drawn;
    int x0 = 1 << 30, y0 = 1 << 30, x1 = -(1 << 30), y1 = -(1 << 30);
    for (size_t i = 0; i < signs.size(); i++) {
        Drawn d;
        d.text = signDisplayText(uiTable_, signs[i], i < windowSamples_.size() ? windowSamples_[i] : SignSample{}, d.x, d.y,
                                 d.w, d.h);
        x0 = std::min({x0, d.x, signs[i].x - 4}), y0 = std::min({y0, d.y, signs[i].y - 4});
        x1 = std::max({x1, d.x + d.w + 1 + FONT_H, signs[i].x + 4}), y1 = std::max({y1, d.y + d.h + 2, signs[i].y + 4});
        drawn.push_back(d);
    }
    Overlay::Layer layer;
    layer.fade = windows_.fadeAlpha();
    if (drawn.empty()) {
        overlay_.recordLayer(cmd, slot, layer, cam_.x, cam_.y, cam_.zoom, world_->frameBuffer(), sw_, sh_);
        return;
    }
    signLayer_.resize(x1 - x0, y1 - y0);
    for (size_t i = 0; i < signs.size(); i++) {
        const Drawn &d = drawn[i];
        const int x = d.x - x0, y = d.y - y0;
        signLayer_.drawFilledRect(x + 1, y + 1, d.w, d.h - 1, 0x000000);
        signLayer_.drawRect(x, y, d.w + 1, d.h, 0xC0C0C0);
        signLayer_.blendText(x + 3, y + 4, d.text, rgb(0xFFFFFF));
        if (signs[i].ju != 3) {
            int px = signs[i].x - x0, py = signs[i].y - y0, dx = 1 - signs[i].ju, dy = signs[i].y > 18 ? -1 : 1;
            for (int j = 0; j < 4; j++) {
                signLayer_.drawPixel(px, py, 0xC0C0C0);
                px += dx;
                py += dy;
            }
        }
    }
    layer.pixels = &signLayer_.pixels();
    layer.x0 = x0, layer.y0 = y0;
    layer.w = uint32_t(signLayer_.width()), layer.h = uint32_t(signLayer_.height());
    overlay_.recordLayer(cmd, slot, layer, cam_.x, cam_.y, cam_.zoom, world_->frameBuffer(), sw_, sh_);
}

void GameScreen::flushUses() {
    std::vector<ToolUse> uses = input_.takeUses();
    uses.insert(uses.end(), pendingUses_.begin(), pendingUses_.end());
    pendingUses_.clear();
    world_->apply(uses);
}

void GameScreen::historySnapshot() {
    flushUses();
    history_.snapshot(*world_);
}

void GameScreen::afterInput() {
    if (!later_.empty()) {
        windows_.collect(); // (the window that asked has gone first)
        auto later = std::move(later_);
        later_.clear();
        for (auto &open : later)
            open();
    }
    if (chosenStamp_)
        openChosenStamp();
    for (auto [x, y] : input_.takeSignClicks())
        signToolClick(x, y);
    int index, slot;
    while (ui_.takeChosen(index, slot)) {
        const UiTool &t = ui_.tools()[size_t(index)];
        if (t.kind == UiTool::Kind::Property)
            openPropertyWindow(nullptr);
        else if (t.kind == UiTool::Kind::AddLife)
            openGolWindow(slot, 0, 0, 0);
    }
    for (const GameInput::Sample &sm : input_.takeSamples()) {
        vkDeviceWaitIdle(ctx_.device());
        if (ui_.colourSelectorVisible()) {
            // in the deco menu it takes the picture's colour there (GameView::GetPixelUnderMouse)
            ui_.setColourSelectorColour(0xFF000000u | world_->picturePixel(sm.x, sm.y));
            continue;
        }
        ClassicParticle p;
        if (!world_->particleAt(sm.x, sm.y, true, p))
            continue;
        if (sm.shift) {
            ui_.setActiveToolIndex(0, ui_.toolIndex("DEFAULT_UI_PROPERTY"));
            openPropertyWindow(&p);
        } else if (p.type == PT_LIFE) {
            // its ruleset's tool, or the custom type window with its rule and colours
            bool found = false;
            for (size_t i = 0; i < ui_.tools().size() && !found; i++)
                if (ui_.tools()[i].kind == UiTool::Kind::Life && ui_.tools()[i].id == p.ctype) {
                    ui_.setActiveToolIndex(0, int(i));
                    found = true;
                }
            if (!found)
                openGolWindow(0, p.ctype, p.dcolour, uint32_t(p.tmp));
        } else if (p.type > 0 && p.type < MAX_ELEMENTS)
            ui_.setActiveToolIndex(0, ui_.toolIndex(uiTable_.elements[size_t(p.type)].identifier));
    }
}

// While a window is up it takes the input (TPT's engine gives it to the top window only)
void GameScreen::windowEvent(const SDL_Event &ev, float s) {
    const int us = uiScale();
    auto mods = [&](bool &shift, bool &ctrl, bool &alt) {
        shift = (ev.key.mod & SDL_KMOD_SHIFT) != 0;
        ctrl = (ev.key.mod & SDL_KMOD_CTRL) != 0;
        alt = (ev.key.mod & SDL_KMOD_ALT) != 0;
    };
    bool shift, ctrl, alt;
    switch (ev.type) {
    case SDL_EVENT_MOUSE_MOTION:
        mouseX_ = ev.motion.x * s;
        mouseY_ = ev.motion.y * s;
        ui::host().mouseX = int(mouseX_) / us;
        ui::host().mouseY = int(mouseY_) / us;
        windows_.mouseMove(ui::host().mouseX, ui::host().mouseY);
        break;
    case SDL_EVENT_MOUSE_BUTTON_DOWN:
        windows_.mouseDown(int(ev.button.x * s) / us, int(ev.button.y * s) / us, ev.button.button);
        break;
    case SDL_EVENT_MOUSE_BUTTON_UP:
        windows_.mouseUp(int(ev.button.x * s) / us, int(ev.button.y * s) / us, ev.button.button);
        break;
    case SDL_EVENT_MOUSE_WHEEL:
        if (ev.wheel.integer_y)
            windows_.mouseWheel(ui::host().mouseX, ui::host().mouseY, ev.wheel.integer_y);
        break;
    case SDL_EVENT_KEY_DOWN:
        mods(shift, ctrl, alt);
        windows_.keyPress(int(ev.key.key), int(ev.key.scancode), ev.key.repeat, shift, ctrl, alt);
        break;
    case SDL_EVENT_KEY_UP:
        mods(shift, ctrl, alt);
        windows_.keyRelease(int(ev.key.key), int(ev.key.scancode), ev.key.repeat, shift, ctrl, alt);
        break;
    case SDL_EVENT_TEXT_INPUT:
        windows_.textInput(utf32(ev.text.text));
        break;
    default:
        break;
    }
    afterInput();
}

void GameScreen::event(const SDL_Event &ev, float s) {
    if (ev.type == SDL_EVENT_QUIT) {
        // EventProcess: with fast quit, or from the game screen, closing the window exits; otherwise it closes the
        // window on top (Engine::CloseWindow)
        if (fastQuit_ || windows_.empty())
            running_ = false;
        else
            windows_.top()->close();
        return;
    }
    if (!windows_.empty()) {
        windowEvent(ev, s);
        return;
    }
    switch (ev.type) {
    case SDL_EVENT_DROP_FILE: {
        float wx, wy;
        toWorld(ev.drop.x * s, ev.drop.y * s, wx, wy);
        historySnapshot();
        vkDeviceWaitIdle(ctx_.device());
        std::string msg;
        world_->loadSave(ev.drop.data, int(wx), int(wy), msg);
        report(msg);
        break;
    }
    case SDL_EVENT_KEY_UP:
        keyUp(ev);
        break;
    case SDL_EVENT_KEY_DOWN:
        keyDown(ev);
        break;
    case SDL_EVENT_MOUSE_WHEEL:
        if (zoomKey_)
            cam_.zoomAt(ev.wheel.mouse_x * s, ev.wheel.mouse_y * s, std::pow(1.25f, ev.wheel.y));
        else if (ev.wheel.integer_y && selectMode_ == Select::None)
            input_.wheel(ev.wheel.integer_y);
        break;
    case SDL_EVENT_WINDOW_FOCUS_LOST:
        input_.blur();
        panning_ = zoomKey_ = false;
        break;
    case SDL_EVENT_MOUSE_BUTTON_DOWN: {
        mouseX_ = ev.button.x * s;
        mouseY_ = ev.button.y * s;
        if (introText_ > 50) // (GameView::DoMouseDown)
            introText_ = 50;
        int button = ev.button.button == SDL_BUTTON_LEFT     ? 0
                     : ev.button.button == SDL_BUTTON_RIGHT  ? 1
                     : ev.button.button == SDL_BUTTON_MIDDLE ? 2
                                                             : -1;
        int us = uiScale();
        if (button < 0 || ui_.mouseDown(int(mouseX_) / us, int(mouseY_) / us, button))
            break; // (the menus took it)
        // GameView::OnMouseDown; the middle button (or alt alone) pans here
        button = input_.pressButton(button);
        if (button == 2) {
            panning_ = true;
            panButton_ = ev.button.button;
            break;
        }
        if (selectMode_ != Select::None) {
            selectPress(button);
            break;
        }
        if (!inWorld(mouseX_, mouseY_))
            break;
        int px, py;
        worldPoint(mouseX_, mouseY_, px, py);
        if (linkSignPress(px, py, button))
            break; // (the sign's)
        historySnapshot();
        input_.mouseDown(px, py, button);
        break;
    }
    case SDL_EVENT_MOUSE_BUTTON_UP: {
        mouseX_ = ev.button.x * s;
        mouseY_ = ev.button.y * s;
        int button = ev.button.button == SDL_BUTTON_LEFT ? 0 : ev.button.button == SDL_BUTTON_RIGHT ? 1 : 2;
        int us = uiScale();
        ui_.mouseUp(int(mouseX_) / us, int(mouseY_) / us, button);
        if (panning_ && ev.button.button == panButton_) {
            panning_ = false;
            break;
        }
        if (selectMode_ != Select::None) {
            selectRelease(button);
            break;
        }
        int fx, fy;
        worldPoint(mouseX_, mouseY_, fx, fy);
        if (inWorld(mouseX_, mouseY_) ? linkSignRelease(fx, fy, button) : (foundSign_ = -1, false))
            break;
        input_.mouseUp(fx, fy);
        break;
    }
    case SDL_EVENT_MOUSE_MOTION: {
        mouseX_ = ev.motion.x * s;
        mouseY_ = ev.motion.y * s;
        int us = uiScale();
        ui::host().mouseX = int(mouseX_) / us;
        ui::host().mouseY = int(mouseY_) / us;
        ui_.mouseMove(int(mouseX_) / us, int(mouseY_) / us, input_.drawing() || panning_);
        if (panning_) {
            cam_.x -= ev.motion.xrel * s / cam_.zoom;
            cam_.y -= ev.motion.yrel * s / cam_.zoom;
        }
        int px, py;
        worldPoint(mouseX_, mouseY_, px, py);
        if (selectMode_ != Select::None)
            selectMove();
        else
            input_.mouseMove(px, py);
        break;
    }
    default:
        break;
    }
}

void GameScreen::keyUp(const SDL_Event &ev) {
    input_.setModifiers(ev.key.mod & SDL_KMOD_SHIFT, ev.key.mod & SDL_KMOD_CTRL, ev.key.mod & SDL_KMOD_ALT);
    shift_ = (ev.key.mod & SDL_KMOD_SHIFT) != 0;
    if (ev.key.repeat)
        return;
    if (ev.key.scancode == SDL_SCANCODE_Z)
        zoomKey_ = false;
    SDL_Keycode k = ev.key.key;
    SDL_Scancode sc = ev.key.scancode;
    if (k == SDLK_RIGHT || k == SDLK_LEFT) {
        pcomm1_ = comm1_; // the last direction
        comm1_ &= 12;
    } else if (k == SDLK_UP)
        comm1_ &= 11;
    else if (k == SDLK_DOWN)
        comm1_ &= 7;
    if (sc == SDL_SCANCODE_D || sc == SDL_SCANCODE_A) {
        pcomm2_ = comm2_;
        comm2_ &= 12;
    } else if (sc == SDL_SCANCODE_W)
        comm2_ &= 11;
    else if (sc == SDL_SCANCODE_S)
        comm2_ &= 7;
}

void GameScreen::keyDown(const SDL_Event &ev) {
    input_.setModifiers(ev.key.mod & SDL_KMOD_SHIFT, ev.key.mod & SDL_KMOD_CTRL, ev.key.mod & SDL_KMOD_ALT);
    const SDL_Keycode k = ev.key.key;
    const bool shift = (ev.key.mod & SDL_KMOD_SHIFT) != 0, ctrl = (ev.key.mod & SDL_KMOD_CTRL) != 0,
               alt = (ev.key.mod & SDL_KMOD_ALT) != 0, repeat = ev.key.repeat;
    VkDevice dev = ctx_.device();
    shift_ = shift;
    if (introText_ > 50)
        introText_ = 50;
    if (ev.key.scancode == SDL_SCANCODE_Z && !ctrl)
        zoomKey_ = true;
    // While a save is being placed the arrows move it and R turns it
    if (selectMode_ == Select::Place && placeKey(k, ev.key.scancode, repeat, shift, ctrl))
        return;
    // ctrl+Z (with shift forward, and not while drawing) and ctrl+Y: undo and redo
    if (!repeat && ctrl && ev.key.scancode == SDL_SCANCODE_Y) {
        history_.forward(*world_);
        return;
    }
    if (!repeat && ctrl && ev.key.scancode == SDL_SCANCODE_Z && !input_.drawing() &&
        !(selectMode_ != Select::None && selectDown_)) {
        if (shift)
            history_.forward(*world_);
        else
            history_.restore(*world_);
        return;
    }
    if (!repeat) {
        if (selectMode_ != Select::None) // (GameController::KeyPress: not while placing)
            ;
        else if (k == SDLK_RIGHT)
            comm1_ |= 2;
        else if (k == SDLK_LEFT)
            comm1_ |= 1;
        else if (k == SDLK_DOWN)
            comm1_ |= 8;
        else if (k == SDLK_UP)
            comm1_ |= 4;
        SDL_Scancode sc = ev.key.scancode;
        if (sc == SDL_SCANCODE_D)
            comm2_ |= 2;
        else if (sc == SDL_SCANCODE_A)
            comm2_ |= 1;
        else if (sc == SDL_SCANCODE_S)
            comm2_ |= 8;
        else if (sc == SDL_SCANCODE_W)
            comm2_ |= 4;
    }
    if (repeat)
        return;
    const SDL_Scancode sc = ev.key.scancode;
    // GameController::KeyPress: while STKM2 is out, W, D and S only steer it (with ctrl they still do the rest)
    if (ctrl || !world_->stickman2Out()) {
        if (sc == SDL_SCANCODE_W) {
            switchGravity();
            return;
        }
        if (sc == SDL_SCANCODE_D) {
            showDebug_ = !showDebug_;
            return;
        }
        if (sc == SDL_SCANCODE_S) { // GameView::BeginStampSelection
            beginSelect(Select::Stamp,
                        U"\x0F\xEF\xEF\x10" U"Click-and-drag to specify an area to create a stamp (right click = cancel)");
            return;
        }
    }
    // GameView::OnKeyPress
    bool shortcut = true;
    switch (sc) {
    case SDL_SCANCODE_SPACE:
        paused_ = !paused_;
        break;
    case SDL_SCANCODE_P:
    case SDL_SCANCODE_F2:
        if (ctrl) // the property tool (with shift, on the right button)
            ui_.chooseTool(shift ? 1 : 0, ui_.toolIndex("DEFAULT_UI_PROPERTY"));
        else
            screenshot_ = true;
        break;
    case SDL_SCANCODE_F3:
        showDebug_ = !showDebug_;
        break;
    case SDL_SCANCODE_F5: // GameController::ReloadSim
        historySnapshot();
        vkDeviceWaitIdle(dev);
        loadScene();
        break;
    case SDL_SCANCODE_R:
        if (ctrl) {
            historySnapshot();
            vkDeviceWaitIdle(dev);
            loadScene();
        }
        break;
    case SDL_SCANCODE_E:
        if (ctrl)
            setEdgeMode(settings_.edgeMode + 1);
        else
            openElementSearch();
        break;
    case SDL_SCANCODE_F:
        if (ctrl) {
            ViewSettings candidate = view_;
            bool any = findCandidate(candidate);
            const bool same = view_.findKind != 0 && any && candidate.findKind == view_.findKind &&
                              candidate.findField == view_.findField && candidate.findValue == view_.findValue;
            if (same || !any)
                view_.findKind = 0;
            else
                view_ = candidate;
            findTool_ = ui_.activeToolIndex(0);
        } else { // GameController::FrameStep
            stepOnce_ = true;
            paused_ = true;
        }
        break;
    case SDL_SCANCODE_G:
        if (ctrl)
            showGravityGrid(!gravityGrid_);
        else
            view_.gridSize = (view_.gridSize + (shift ? 9 : 1)) % 10; // GameController::AdjustGridSize
        break;
    case SDL_SCANCODE_F1:
        introText_ = introText_ ? 0 : 8047;
        break;
    case SDL_SCANCODE_F11:
        fullscreenToggle_ = true;
        break;
    case SDL_SCANCODE_H:
        if (ctrl)
            introText_ = introText_ ? 0 : 8047;
        else
            showHud_ = !showHud_;
        break;
    case SDL_SCANCODE_B:
        if (ctrl)
            setDecoration(view_.decorationLevel == 0);
        else if (ui_.colourSelectorVisible()) // the deco menu, or back to the last menu
            ui_.setActiveMenu(ui_.lastMenu());
        else {
            setDecoration(true);
            paused_ = true;
            ui_.setActiveMenu(SC_DECO);
        }
        break;
    case SDL_SCANCODE_Y:
        if (!ctrl) // (ctrl+Y is handled above)
            switchAir();
        break;
    case SDL_SCANCODE_ESCAPE:
    case SDL_SCANCODE_Q:
        confirmExit();
        break;
    case SDL_SCANCODE_U:
        if (ctrl) { // GameModel::ResetAHeat
            vkDeviceWaitIdle(dev);
            world_->resetAmbientHeat();
        } else
            setAmbientHeat(!settings_.ambientHeat);
        break;
    case SDL_SCANCODE_N:
        setNewtonianGravity(!settings_.newtonianGravity);
        break;
    case SDL_SCANCODE_EQUALS:
        vkDeviceWaitIdle(dev);
        if (ctrl)
            world_->resetSpark();
        else
            world_->resetAir();
        break;
    case SDL_SCANCODE_C:
        if (ctrl)
            beginSelect(Select::Copy, U"\x0F\xEF\xEF\x10" U"Click-and-drag to specify an area to copy (right click = cancel)");
        break;
    case SDL_SCANCODE_X:
        if (ctrl)
            beginSelect(Select::Cut,
                        U"\x0F\xEF\xEF\x10" U"Click-and-drag to specify an area to copy then cut (right click = cancel)");
        break;
    case SDL_SCANCODE_V:
        if (ctrl && clipboard_) { // GameController::LoadClipboard
            setPlaceSave(*clipboard_);
            worldPoint(mouseX_, mouseY_, selectX1_, selectY1_);
            selectX2_ = selectX1_, selectY2_ = selectY1_;
            selectDown_ = false;
            input_.cancel();
        }
        break;
    case SDL_SCANCODE_L:
        if (!stamps_.names().empty()) {
            loadLastStamp();
            break;
        }
        [[fallthrough]]; // (with no stamps, L does what K does)
    case SDL_SCANCODE_K:
        selectMode_ = Select::None;
        selectX1_ = selectY1_ = selectX2_ = selectY2_ = -1;
        openStamps();
        break;
    case SDL_SCANCODE_RIGHTBRACKET:
        input_.bracket(1, alt);
        break;
    case SDL_SCANCODE_LEFTBRACKET:
        input_.bracket(-1, alt);
        break;
    case SDL_SCANCODE_I:
        if (!ctrl) { // (ctrl+I installs TPT on the system; not here)
            vkDeviceWaitIdle(dev);
            world_->invertAir();
        }
        break;
    case SDL_SCANCODE_SEMICOLON:
        input_.toggleReplace(ctrl);
        break;
    default:
        shortcut = false;
    }
    if (!shortcut) {
        if (k == SDLK_TAB)
            input_.changeBrush();
        else if (k == SDLK_INSERT)
            input_.toggleReplace(ctrl);
        else if (k == SDLK_DELETE)
            input_.toggleSpecificDelete();
        // Big Powder's own keys
        else if (k == SDLK_PERIOD)
            stepOnce_ = true;
        else if (k == SDLK_M)
            switchMode(mode_ == "classic" ? (o_.mode == "huge" ? "huge" : "screen") : "classic");
        else if (k == SDLK_F6) {
            historySnapshot();
            sceneIdx_ = (sceneIdx_ + 1) % scenes_.size();
            vkDeviceWaitIdle(dev);
            loadScene();
        } else if (k == SDLK_HOME)
            fitView();
    }
    // The display presets: 0-9, shift+1 (with the debug HUD), shift+6 and shift+0
    if (shift && showDebug_ && k == SDLK_1)
        loadRenderPreset(10);
    else if (shift && k == SDLK_6)
        loadRenderPreset(11);
    else if (shift && k == SDLK_0)
        loadRenderPreset(12);
    else if (k >= SDLK_0 && k <= SDLK_9)
        loadRenderPreset(int(k - SDLK_0));
}

void GameScreen::setInfoTip(const std::u32string &tip) {
    infoTip_ = tip;
    infoTipPresence_ = 120;
}

void GameScreen::loadRenderPreset(int preset) {
    const ViewSettings p = tptPresetView(preset);
    view_.renderMode = p.renderMode, view_.displayMode = p.displayMode, view_.colorMode = p.colorMode;
    view_.autoHeat = p.autoHeat, view_.hdispMin = p.hdispMin, view_.hdispMax = p.hdispMax;
    setInfoTip(utf32(ClassicSim::presetName(preset)));
}

void GameScreen::setDecoration(bool on) {
    const int level = on ? 1 : 0;
    if (view_.decorationLevel == level)
        return;
    view_.decorationLevel = level;
    setInfoTip(on ? U"Decorations Layer: On" : U"Decorations Layer: Off");
}

void GameScreen::showGravityGrid(bool on) {
    gravityGrid_ = on;
    setInfoTip(on ? U"Gravity Grid: On" : U"Gravity Grid: Off");
}

void GameScreen::applySettings() {
    vkDeviceWaitIdle(ctx_.device());
    world_->applySettings(settings_);
}

void GameScreen::setAmbientHeat(bool on) {
    settings_.ambientHeat = on;
    applySettings();
    setInfoTip(on ? U"Ambient Heat: On" : U"Ambient Heat: Off");
}

void GameScreen::setNewtonianGravity(bool on) {
    settings_.newtonianGravity = on;
    applySettings();
    setInfoTip(on ? U"Newtonian Gravity: On" : U"Newtonian Gravity: Off");
}

void GameScreen::setEdgeMode(int mode) {
    settings_.edgeMode = mode < 0 || mode >= 3 ? 0 : mode;
    applySettings();
    static const char32_t *names[] = {U"Edge Mode: Void", U"Edge Mode: Solid", U"Edge Mode: Loop"};
    setInfoTip(names[settings_.edgeMode]);
}

void GameScreen::switchGravity() {
    settings_.gravityMode = (settings_.gravityMode + 1) % 4;
    applySettings();
    static const char32_t *names[] = {U"Gravity: Vertical", U"Gravity: Off", U"Gravity: Radial", U"Gravity: Custom"};
    setInfoTip(names[settings_.gravityMode]);
}

void GameScreen::switchAir() {
    settings_.airMode = (settings_.airMode + 1) % 5;
    applySettings();
    static const char32_t *names[] = {U"Air: On", U"Air: Pressure Off", U"Air: Velocity Off", U"Air: Off",
                                      U"Air: No Update"};
    setInfoTip(names[settings_.airMode]);
}

// GameView::FindingElementCandidate: the left button's element (a Game of Life type with its rule), or what the
// property tool sets
bool GameScreen::findCandidate(ViewSettings &v) const {
    const UiTool *t = ui_.activeTool(0);
    if (!t)
        return false;
    if (t->identifier.find("_PT_") != std::string::npos) {
        v.findKind = 1, v.findField = ParticleProperty::TYPE;
        v.findValue = uint32_t(t->brushType());
        return true;
    }
    if (t->identifier == "DEFAULT_UI_PROPERTY" && input_.property) {
        const ParticleProperty &p = *input_.property;
        v.findKind = p.field == ParticleProperty::TYPE ? 1 : p.kind == ParticleProperty::Kind::Float ? 3 : 2;
        v.findField = p.field;
        v.findValue = p.bits;
        return true;
    }
    return false;
}

void GameScreen::confirmExit() {
    if (confirmingExit_)
        return;
    confirmingExit_ = true;
    showWindow(std::make_unique<ConfirmPrompt>(U"You are about to quit", U"Are you sure you want to exit the game?",
                                               [this] { running_ = false; }, U"Confirm",
                                               [this] { confirmingExit_ = false; }));
}

// GameView::TakeScreenshot(0, 0): the renderer's frame as a PNG, named by the time (with a number when there are
// more in the same second)
void GameScreen::takeScreenshot() {
    vkDeviceWaitIdle(ctx_.device());
    const std::vector<uint32_t> px = world_->picture();
    const int w = int(world_->width()), h = int(world_->height());
    if (px.size() < size_t(w) * size_t(h))
        return;
    const int64_t now = int64_t(std::time(nullptr));
    std::string suffix;
    if (now == lastShotTime_)
        suffix = " (" + std::to_string(++shotIndex_) + ")";
    else
        shotIndex_ = 1;
    lastShotTime_ = now;
    const std::string path = dataRoot_ + "/screenshot " + tptDate(now, "%Y-%m-%d %H.%M.%S") + suffix + ".png";
    const std::vector<char> png = tptPng(px.data(), w, h);
    std::ofstream out(path, std::ios::binary);
    if (png.empty() || !out.write(png.data(), std::streamsize(png.size()))) {
        report("Could not write " + path);
        return;
    }
    report("Screenshot: " + path);
}

void GameScreen::openRenderOptions() {
    const int us = uiScale();
    ui_.setSize(int(sw_) / us, int(sh_) / us);
    showWindow(std::make_unique<RenderOptionsWindow>(ui_.viewW(), ui_.viewH(), ui_.height(), view_));
}

void GameScreen::openOptions() {
    std::function<void()> reportProblem;
    if (appreports::enabled()) // (a build with a report URL)
        reportProblem = [this] { later_.push_back([this] { openReportWindow(); }); };
    showWindow(std::make_unique<OptionsWindow>(
        optionValues(), [this](const OptionValues &v) { applyOptions(v); },
        [this] { SDL_OpenURL(("file://" + dataRoot_).c_str()); }, reportProblem));
}

OptionValues GameScreen::optionValues() const {
    OptionValues v;
    v.sim = settings_;
    v.temperatureScale = temperatureScale_;
    v.simUncapped = simUncapped_;
    v.simFps = float(tps_);
    v.drawFollowDisplay = drawFollowDisplay_;
    v.drawFps = drawFps_;
    v.scale = pendingScale_ >= 0 ? pendingScale_ : uiScale_;
    v.maxScale = std::max(1, int(std::min(sw_ / 629, sh_ / 424))); // (TPT's WINDOWW x WINDOWH, whole)
    v.fullscreen = fullscreen_;
    v.fastQuit = fastQuit_;
    v.stickyCategories = ui_.mouseClickRequired();
    v.includePressure = includePressure_;
    v.perfectCircle = input_.brush.perfectCircle();
    v.decoSpace = input_.decoSpace;
    return v;
}

// OptionsModel's setters: each setting that changed, and the prefs it is kept in
void GameScreen::applyOptions(const OptionValues &v) {
    Prefs::Defer defer(prefs_);
    const ClassicOptions &o = v.sim;
    if (o.edgeMode != settings_.edgeMode)
        prefs_.setInt("Simulation.EdgeMode", o.edgeMode);
    if (o.ambientTemp != settings_.ambientTemp)
        prefs_.setFloat("Simulation.AmbientAirTemp", o.ambientTemp);
    if (o.edgePressure != settings_.edgePressure)
        prefs_.setFloat("Simulation.EdgePressure", o.edgePressure);
    if (o.edgeVX != settings_.edgeVX)
        prefs_.setFloat("Simulation.EdgeVelocityX", o.edgeVX);
    if (o.edgeVY != settings_.edgeVY)
        prefs_.setFloat("Simulation.EdgeVelocityY", o.edgeVY);
    if (o.vorticity != settings_.vorticity)
        prefs_.setFloat("Simulation.VorticityCoeff", o.vorticity);
    if (o.convectionMode != settings_.convectionMode)
        prefs_.setInt("Simulation.ConvectionMode", o.convectionMode);
    const bool simChanged =
        o.heat != settings_.heat || o.ambientHeat != settings_.ambientHeat ||
        o.newtonianGravity != settings_.newtonianGravity || o.waterEqualisation != settings_.waterEqualisation ||
        o.edgeMode != settings_.edgeMode || o.gravityMode != settings_.gravityMode || o.airMode != settings_.airMode ||
        o.customGravX != settings_.customGravX || o.customGravY != settings_.customGravY ||
        o.ambientTemp != settings_.ambientTemp || o.edgePressure != settings_.edgePressure ||
        o.edgeVX != settings_.edgeVX || o.edgeVY != settings_.edgeVY || o.vorticity != settings_.vorticity ||
        o.convectionMode != settings_.convectionMode;
    if (simChanged) {
        settings_ = o;
        applySettings();
    }
    if (v.temperatureScale != temperatureScale_) {
        temperatureScale_ = v.temperatureScale;
        prefs_.setInt("Renderer.TemperatureScale", temperatureScale_);
    }
    if (v.simUncapped != simUncapped_ || float(tps_) != v.simFps) {
        simUncapped_ = v.simUncapped;
        tps_ = v.simFps;
        prefs_.setFloat("FpsLimit", simUncapped_ ? 2.0f : v.simFps); // (2: uncapped)
    }
    if (v.drawFollowDisplay != drawFollowDisplay_ || v.drawFps != drawFps_) {
        drawFollowDisplay_ = v.drawFollowDisplay;
        drawFps_ = v.drawFps;
        prefs_.setInt("DrawLimit", drawFollowDisplay_ ? -1 : drawFps_); // (-1: the display's)
    }
    if (v.scale != (pendingScale_ >= 0 ? pendingScale_ : uiScale_)) {
        pendingScale_ = v.scale;
        prefs_.setInt("Scale", v.scale);
    }
    if (v.fullscreen != fullscreen_) {
        fullscreen_ = v.fullscreen;
        fullscreenToggle_ = !fullscreenToggle_;
        prefs_.setBool("Fullscreen", v.fullscreen);
    }
    if (v.fastQuit != fastQuit_) {
        fastQuit_ = v.fastQuit;
        prefs_.setBool("FastQuit", fastQuit_);
    }
    if (v.stickyCategories != ui_.mouseClickRequired()) {
        ui_.setMouseClickRequired(v.stickyCategories);
        prefs_.setBool("MouseClickRequired", v.stickyCategories);
    }
    if (v.includePressure != includePressure_) {
        includePressure_ = v.includePressure;
        prefs_.setBool("Simulation.IncludePressure", includePressure_);
    }
    if (v.perfectCircle != input_.brush.perfectCircle()) {
        input_.brush.setPerfectCircle(v.perfectCircle);
        prefs_.setBool("PerfectCircleBrush", v.perfectCircle);
    }
    if (v.decoSpace != input_.decoSpace) {
        input_.decoSpace = v.decoSpace;
        prefs_.setInt("Simulation.DecoSpace", v.decoSpace);
    }
}

// GameModel's and GameController's constructors: what TPT keeps between runs, with TPT's defaults and ranges
void GameScreen::loadPrefs() {
    const Prefs &p = prefs_;
    view_.renderMode = p.getUInt("Renderer.RenderMode", view_.renderMode);
    view_.displayMode = p.getUInt("Renderer.DisplayMode", view_.displayMode);
    view_.colorMode = p.getUInt("Renderer.ColourMode", view_.colorMode);
    gravityGrid_ = p.getBool("Renderer.GravityField", false);
    view_.decorationLevel = p.getBool("Renderer.Decorations", true) ? 1 : 0;
    view_.gridCheckerboard = p.getBool("Renderer.GridCheckerboard", false);
    showDebug_ = p.getBool("Renderer.DebugMode", false);

    settings_.edgeMode = p.getEnum("Simulation.EdgeMode", 3, 0);
    auto inRange = [&](const char *key, float lo, float hi, float &value) {
        float f = p.getFloat(key, value);
        if (f >= lo && f <= hi)
            value = f;
    };
    inRange("Simulation.AmbientAirTemp", 0.0f, 9999.0f, settings_.ambientTemp); // (MIN_TEMP, MAX_TEMP)
    inRange("Simulation.EdgePressure", -256.0f, 256.0f, settings_.edgePressure);
    inRange("Simulation.EdgeVelocityX", -1e4f, 1e4f, settings_.edgeVX); // (MAX_VELOCITY)
    inRange("Simulation.EdgeVelocityY", -1e4f, 1e4f, settings_.edgeVY);
    inRange("Simulation.VorticityCoeff", 0.0f, 1.0f, settings_.vorticity);
    settings_.convectionMode = p.getEnum("Simulation.ConvectionMode", 3, 2);
    input_.decoSpace = p.getEnum("Simulation.DecoSpace", 4, DECOSPACE_SRGB);
    settings_.newtonianGravity = p.getBool("Simulation.NewtonianGravity", false);
    settings_.ambientHeat = p.getInt("Simulation.AmbientHeat", 0) != 0;
    settings_.prettyPowder = p.getInt("Simulation.PrettyPowder", 0) != 0;
    input_.brush.setPerfectCircle(p.getBool("PerfectCircleBrush", true));
    {
        auto channel = [&](const char *key, int def) { return uint32_t(std::clamp(p.getInt(key, def), 0, 255)); };
        ui_.setColourSelectorColour(channel("Decoration.Alpha", 255) << 24 | channel("Decoration.Red", 200) << 16 |
                                    channel("Decoration.Green", 100) << 8 | channel("Decoration.Blue", 50));
    }
    history_.limit = std::min(p.getUInt("Simulation.UndoHistoryLimit", 5), 200u);
    ui_.setMouseClickRequired(p.getBool("MouseClickRequired", false));
    includePressure_ = p.getBool("Simulation.IncludePressure", true);
    temperatureScale_ = p.getEnum("Renderer.TemperatureScale", 3, TEMPSCALE_CELSIUS);
    ui_.setFavourites(p.getStrings("Favorites"));
    propertyField_ = p.getInt("Prop.Type", 0);
    propertyText_ = utf32(p.getString("Prop.Value", ""));
    lastGolName_ = utf32(p.getString("CustomGOL.Name", "CGOL"));
    lastGolRule_ = utf32(p.getString("CustomGOL.Rule", "B3/S23"));
    fastQuit_ = p.getBool("FastQuit", true);

    // GameModel::LoadCustomGol: "name rule colour1 colour2", the ones that don't check out dropped
    bool removedAny = false;
    for (const std::string &line : p.getStrings("CustomGOL.Types")) {
        std::vector<std::string> parts;
        for (size_t at = 0; at <= line.size();) {
            size_t sp = line.find(' ', at);
            if (sp == std::string::npos)
                sp = line.size();
            parts.push_back(line.substr(at, sp - at));
            at = sp + 1;
        }
        CustomGol g;
        bool ok = parts.size() == 4;
        if (ok) {
            g.name = utf32(parts[0]);
            g.rule = tptParseGolRule(utf32(parts[1]));
            float c1 = 0, c2 = 0;
            ok = tptParseNumber(utf32(parts[2]), c1) && tptParseNumber(utf32(parts[3]), c2);
            g.colour1 = uint32_t(int64_t(c1)) & 0xFFFFFF, g.colour2 = uint32_t(int64_t(c2)) & 0xFFFFFF;
        }
        ok = ok && tptValidGolName(g.name) && g.rule != -1; // (CheckCustomGol)
        for (const CustomGol &other : customGol_)
            ok = ok && other.name != g.name && other.rule != g.rule;
        if (!ok) {
            removedAny = true;
            continue;
        }
        customGol_.push_back(g);
        ui_.addCustomGolTool(g);
    }
    world_->setCustomGol(customGol_);
    if (removedAny)
        saveCustomGol();

    // The settings window's: the framerate caps (FpsLimit 2: uncapped), the scale and the window
    const float fps = p.getFloat("FpsLimit", 60.0f);
    simUncapped_ = fps == 2.0f;
    tps_ = fps >= 3.0f && fps <= 1000.0f ? fps : 60.0f;
    const int draw = p.getInt("DrawLimit", -1);
    drawFollowDisplay_ = draw == -1 || draw < 1 || draw >= 1000; // (Big Powder follows the display by default)
    drawFps_ = drawFollowDisplay_ ? 60 : draw;
    uiScale_ = std::max(0, p.getInt("Scale", 0));
    fullscreen_ = o_.fullscreen;
    if (p.getBool("Fullscreen", false) && !o_.fullscreen)
        fullscreenToggle_ = true;
}

// GameModel's destructor: what TPT saves as it quits
void GameScreen::savePrefs() {
    Prefs::Defer defer(prefs_);
    prefs_.setUInt("Renderer.ColourMode", view_.colorMode);
    prefs_.setUInt("Renderer.DisplayMode", view_.displayMode);
    prefs_.setUInt("Renderer.RenderMode", view_.renderMode);
    prefs_.setBool("Renderer.GravityField", gravityGrid_);
    prefs_.setBool("Renderer.GridCheckerboard", view_.gridCheckerboard);
    prefs_.setBool("Renderer.Decorations", view_.decorationLevel != 0);
    prefs_.setBool("Renderer.DebugMode", showDebug_);
    prefs_.setBool("Simulation.NewtonianGravity", settings_.newtonianGravity);
    prefs_.setInt("Simulation.AmbientHeat", settings_.ambientHeat ? 1 : 0);
    prefs_.setInt("Simulation.PrettyPowder", settings_.prettyPowder ? 1 : 0);
    const uint32_t c = ui_.decoColour();
    prefs_.setInt("Decoration.Red", int(c >> 16 & 0xFF));
    prefs_.setInt("Decoration.Green", int(c >> 8 & 0xFF));
    prefs_.setInt("Decoration.Blue", int(c & 0xFF));
    prefs_.setInt("Decoration.Alpha", int(c >> 24));
}

void GameScreen::saveCustomGol() {
    std::vector<std::string> types;
    for (const CustomGol &g : customGol_)
        types.push_back(utf8(g.name) + " " + utf8(tptGolRuleString(g.rule)) + " " + std::to_string(g.colour1 & 0xFFFFFF) +
                        " " + std::to_string(g.colour2 & 0xFFFFFF));
    prefs_.setStrings("CustomGOL.Types", types);
}

// IntroText, with Big Powder's controls where they differ from TPT's
std::u32string GameScreen::introMessage() const {
    return utf32(
        "\bl\bUBig Powder\bU - The Powder Toy 100.1's simulation on a huge, zoomable world - https://powdertoy.co.uk\n"
        "\n"
        "\n"
        "\bgPress \bo'F1'\bg to show or hide this text.\n"
        "\n"
        "\bgTo choose a material, hover over one of the icons on the right, it will show a selection of elements in that "
        "group.\n"
        "Pick your material from the menu using \bomouse left/right\bg buttons.\n"
        "Draw freeform lines by dragging your mouse left/right button across the drawing area.\n"
        "\n"
        "Use the \bomouse scroll wheel\bg, or \bo'['\bg and \bo']'\bg, to change the tool size for particles. Press "
        "\boTab\bg to change the brush shape.\n"
        "\boMiddle drag\bg or \boAlt+drag\bg moves the view, \bo'Z'+wheel\bg zooms it and \boHome\bg fits the world. The "
        "sample tool (in the tools menu) \"samples\" the particles.\n"
        "\boCtrl+C/V/X\bg are copy, paste and cut respectively.\n"
        "When pasting, use \bo'R'\bg to rotate, \boShift+R\bg and \boShift+Ctrl+R\bg to mirror vertically or "
        "horizontally.\n"
        "\boShift+drag\bg will create straight lines of particles. \boShift+Alt+drag\bg for horizontal, vertical and "
        "diagonal lines.\n"
        "\boCtrl+drag\bg will result in filled rectangles. \boCtrl+Alt+drag\bg for filled squares. \boCtrl+Shift+click\bg "
        "will flood-fill a closed area.\n"
        "\n"
        "\boSpacebar\bg can be used to pause physics. Use \bo'F'\bg to step ahead by one frame, \bo'F5'\bg to reload "
        "simulation.\n"
        "\boCtrl+Z\bg will act as undo, \boCtrl+Y\bg or \boCtrl+Shift+Z\bg as redo.\n"
        "Use \bo'S'\bg to save parts of the window as 'stamps'. \bo'L'\bg loads the most recent stamp, \bo'K'\bg shows a "
        "library of stamps you saved.\n"
        "\n"
        "Use \bo0-9\bg to select a view mode.\n"
        "Use \bo'H'\bg to toggle the HUD. Use \bo'D'\bg to toggle debug mode in the HUD.\n"
        "Use \boCtrl+F\bg to highlight a selected element on the screen.\n"
        "\bo'M'\bg switches between Classic (TPT's own engine) and the GPU world, \bo'F6'\bg loads the next scene, "
        "\boCtrl+S\bg saves. Drop a save on the window to load it.\n"
        "\n"
        "\bgThe online features (the save browser, accounts) aren't part of Big Powder.\n"
        "\n"
        "\btBig Powder " BP_VERSION ", from The Powder Toy 100.1.400");
}

// GameController::NotifyUpdateAvailable: the notification, once the viewer's check finds a newer release
void GameScreen::updateNotification() {
    if (!updateCheck_)
        return;
    std::lock_guard g(updateCheck_->m);
    if (!updateCheck_->ready)
        return;
    if (updateCheck_->info) {
        update_ = updateCheck_->info;
        ui_.setNotification(U"A new version is available - click here to update");
        report("A new version is available: " + update_->version);
    } else if (!updateCheck_->error.empty())
        report("Couldn't check for updates: " + updateCheck_->error);
    updateCheck_.reset();
}

// UpdateNotification::Action: what is installed, what would be, and the release's notes; installed in place when
// this is a release's folder that can be written, or else the releases page to download it from
void GameScreen::openUpdatePrompt() {
    if (!update_)
        return;
    const bool here = canUpdate(installDir_);
    std::u32string message =
        here ? U"Are you sure you want to run the updater? Please save any changes before updating.\n\nCurrent version:\n "
             : U"Click \"Confirm\" to download the latest version from the releases page.\n\nCurrent version:\n ";
    message += utf32(BP_VERSION) + U"\nNew version:\n " + utf32(update_->version);
    if (!update_->notes.empty()) {
        std::string notes = update_->notes;
        notes.erase(std::remove(notes.begin(), notes.end(), '\r'), notes.end());
        message += U"\n\nChangelog:\n" + utf32(notes);
    }
    const std::string page = update_->releaseUrl.empty() ? RELEASES_PAGE : update_->releaseUrl;
    showWindow(std::make_unique<ConfirmPrompt>(U"Run Updater", message, [this, here, page] {
        if (here)
            later_.push_back([this] { startUpdate(); });
        else
            SDL_OpenURL(page.c_str());
    }));
}

// GameController::RunUpdater: the download, check and install on a thread of its own under TPT's TaskWindow; then
// the game starts again as the new version
void GameScreen::startUpdate() {
    if (!update_)
        return;
    report("Updating to " + update_->version);
    auto progress = std::make_shared<UpdateProgress>();
    updateProgress_ = progress;
    std::thread([progress, u = *update_, dir = installDir_] { installUpdate(u, dir, *progress); }).detach();
    showWindow(std::make_unique<TaskWindow>(
        U"Downloading update...",
        [progress] {
            std::lock_guard g(progress->m);
            TaskWindow::State s;
            s.status = utf32(progress->status);
            s.progress = progress->progress;
            s.done = progress->done;
            return s;
        },
        [this, progress] {
            std::lock_guard g(progress->m);
            if (progress->ok) {
                report("Update installed: starting again");
                restart_ = true;
                running_ = false;
            } else {
                appreports::reportError("update", progress->error);
                const std::u32string error = utf32(progress->error);
                later_.push_back([this, error] { showWindow(std::make_unique<ErrorMessage>(U"Error", error)); });
            }
        }));
}

void GameScreen::openSaveBrowser() {
    if (!savesSource_)
        savesSource_ = std::make_unique<OnlineSaves>();
    auto browser = std::make_unique<SaveBrowserWindow>(
        ui_.width(), ui_.height(), *savesSource_, [this](const OnlineSave &save, bool instant) {
            later_.push_back([this, save, instant] { openSavePreview(save, instant); });
        });
    saveBrowser_ = browser.get();
    showWindow(std::move(browser));
}

void GameScreen::openSavePreview(const OnlineSave &save, bool instant) {
    auto render = [](const std::vector<char> &data, int &w, int &h) {
        SaveArea area;
        std::string error;
        if (!tptReadSave(data, area, error)) {
            w = h = 0;
            return std::vector<uint32_t>{};
        }
        return tptRenderPreview(area, 612 / 2, 384 / 2, w, h); // (RES / 2)
    };
    showWindow(std::make_unique<SavePreviewWindow>(
        save, *savesSource_, render,
        [this](const OnlineSave &s, const std::vector<char> &data) {
            later_.push_back([this, s, data] { loadOnlineSave(s, data); });
        },
        instant, [](const std::string &url) { SDL_OpenURL(url.c_str()); }));
}

void GameScreen::loadOnlineSave(const OnlineSave &save, const std::vector<char> &data) {
    // (SearchController::OpenSaveDone: the browser goes too)
    if (saveBrowser_ && windows_.top() == saveBrowser_) {
        saveBrowser_->close();
        windows_.collect();
    }
    saveBrowser_ = nullptr;
    report("Opening save " + std::to_string(save.id) + " \"" + utf8(save.name) + "\" by " + save.username);
    if (mode_ == "classic") {
        // GameModel::SetSave: TPT's own loader, in place of the world
        const std::filesystem::path file =
            std::filesystem::temp_directory_path() / ("big-powder-" + std::to_string(save.id) + ".cps");
        {
            std::ofstream out(file, std::ios::binary | std::ios::trunc);
            out.write(data.data(), std::streamsize(data.size()));
        }
        historySnapshot();
        vkDeviceWaitIdle(ctx_.device());
        std::string msg;
        const bool ok = world_->loadSave(file.string(), 0, 0, msg);
        std::error_code ec;
        std::filesystem::remove(file, ec);
        report(msg);
        if (!ok) {
            showWindow(std::make_unique<ErrorMessage>(U"Error loading save", utf32(msg)));
            return;
        }
    } else {
        SaveArea area;
        std::string error;
        if (!tptReadSave(data, area, error)) {
            showWindow(std::make_unique<ErrorMessage>(U"Error loading save", utf32(error)));
            return;
        }
        setPlaceSave(std::move(area)); // (as a stamp: it follows the mouse until a click places it)
        worldPoint(mouseX_, mouseY_, selectX1_, selectY1_);
        selectX2_ = selectX1_, selectY2_ = selectY1_;
        selectDown_ = false;
        input_.cancel();
    }
    saveName_ = utf8(save.name);
}

void GameScreen::openReportWindow() {
    showWindow(std::make_unique<ReportWindow>([this](const std::u32string &note) {
        auto result = std::make_shared<std::atomic<int>>(0);
        reportResult_ = result;
        appreports::sendReport(utf8(note), reportState(), [result](bool ok) { *result = ok ? 1 : 2; });
    }));
}

// What a problem report says about the game: the mode, the world, the settings and the display
std::map<std::string, std::string> GameScreen::reportState() const {
    char b[512];
    std::snprintf(b, sizeof b,
                  "edge %d, gravity %d, air %d, heat %d, ambient heat %d, newtonian %d, water %d, convection %d, "
                  "sand effect %d, ambient %.2f K, pressure %.2f, vorticity %.2f",
                  settings_.edgeMode, settings_.gravityMode, settings_.airMode, int(settings_.heat),
                  int(settings_.ambientHeat), int(settings_.newtonianGravity), int(settings_.waterEqualisation),
                  settings_.convectionMode, int(settings_.prettyPowder), double(settings_.ambientTemp),
                  double(settings_.edgePressure), double(settings_.vorticity));
    char v[160];
    std::snprintf(v, sizeof v, "render %08X display %08X colour %08X deco %d grid %d", view_.renderMode,
                  view_.displayMode, view_.colorMode, view_.decorationLevel, view_.gridSize);
    return {{"mode", mode_},
            {"world", std::to_string(world_->width()) + "x" + std::to_string(world_->height())},
            {"scene", saveName_},
            {"gpu", gpuName_},
            {"screen", std::to_string(sw_) + "x" + std::to_string(sh_) + ", interface scale " +
                           std::to_string(uiScale())},
            {"fps", std::to_string(int(uiFps_)) + ", " + std::to_string(int(tps_)) + " ticks/s cap" +
                        (simUncapped_ ? " (uncapped)" : "")},
            {"settings", b},
            {"view", v}};
}

bool GameScreen::record(VkCommandBuffer cmd, int slot, double dt, VkQueryPool queries) {
    VkDevice dev = ctx_.device();
    nowMs_ += dt * 1000.0;
    ui::host().now = nowMs_;
    afterInput();
    if (screenshot_) {
        screenshot_ = false;
        takeScreenshot();
    }
    if (ui_.takeFavouritesChanged()) // (Favorite::SaveFavoritesToPrefs)
        prefs_.setStrings("Favorites", ui_.favourites());
    // What the interface's buttons asked for
    switch (ui_.takeAction()) {
    case UiAction::ElementSearch:
        openElementSearch();
        break;
    case UiAction::RemoveCustomGol: {
        std::string id = ui_.removeRequest();
        showWindow(std::make_unique<ConfirmPrompt>(
            U"Remove custom GOL type", U"Are you sure you want to remove " + utf32(id.substr(20)) + U"?", [this, id] {
                // GameModel::RemoveCustomGol
                std::erase_if(customGol_, [&](const CustomGol &g) { return "DEFAULT_PT_LIFECUST_" + utf8(g.name) == id; });
                world_->setCustomGol(customGol_);
                ui_.removeTool(id);
                saveCustomGol();
            }));
        break;
    }
    case UiAction::Open:
        openSaveBrowser();
        break;
    case UiAction::Notification:
        openUpdatePrompt();
        break;
    case UiAction::DismissNotification: // GameController::RemoveNotification
        ui_.setNotification(U"");
        break;
    case UiAction::ColourPicker: // GameController::OpenColourPicker
        showWindow(std::make_unique<ColourPickerWindow>(ui_.decoColour(), [this](uint32_t c) { ui_.setColour(c); }));
        break;
    case UiAction::Reload:
        historySnapshot();
        vkDeviceWaitIdle(dev);
        loadScene();
        break;
    case UiAction::Save: {
        vkDeviceWaitIdle(dev);
        std::string msg;
        world_->save(savesDir(o_) + "/" + mode_ + "-" + timestamp() + world_->saveExtension(), msg);
        report(msg);
        break;
    }
    case UiAction::Clear: // GameController::ClearSim
        historySnapshot();
        vkDeviceWaitIdle(dev);
        world_->loadScene("empty");
        saveName_ = "[untitled simulation]";
        break;
    case UiAction::Pause:
        paused_ = !paused_;
        break;
    case UiAction::RenderOptions:
        openRenderOptions();
        break;
    case UiAction::SimulationOptions:
        openOptions();
        break;
    case UiAction::SandEffect:
        settings_.prettyPowder = !settings_.prettyPowder;
        applySettings();
        break;
    case UiAction::GravityGrid:
        showGravityGrid(!gravityGrid_);
        break;
    case UiAction::Decorations:
        setDecoration(view_.decorationLevel == 0);
        break;
    case UiAction::Newtonian:
        setNewtonianGravity(!settings_.newtonianGravity);
        break;
    case UiAction::AmbientHeat:
        setAmbientHeat(!settings_.ambientHeat);
        break;
    default:
        break;
    }
    updateNotification();
    if (windows_.empty())
        saveBrowser_ = nullptr;
    if (reportResult_ && *reportResult_) {
        setInfoTip(*reportResult_ == 1 ? U"Problem report sent. Thank you"
                                       : U"The report couldn't be sent now: it will go when Big Powder next starts");
        reportResult_.reset();
    }
    // GameView's fades: the introduction and the info tip, 60 a second
    introText_ = std::max(0.0, introText_ - dt * 60.0);
    infoTipPresence_ = std::max(0.0, infoTipPresence_ - dt * 60.0);
    // NotifyActiveToolsChanged: what is being found follows the left button's tool
    if (view_.findKind && ui_.activeToolIndex(0) != findTool_) {
        findTool_ = ui_.activeToolIndex(0);
        if (!findCandidate(view_))
            view_.findKind = 0;
    }
    if (selectMode_ == Select::Place && placeThumb_.empty())
        selectMode_ = Select::None;
    ui_.holdButtonTip(selectMode_ != Select::None && selectMode_ != Select::Place);
    if (windows_.empty()) {
        int px, py;
        worldPoint(mouseX_, mouseY_, px, py);
        if (selectMode_ == Select::None)
            input_.tick(px, py);
        signTooltip();
    }

    // Fixed-rate simulation clock. If the engine can't keep up the sim slows down instead of skipping.
    int ticks = 0;
    if (!windows_.empty()) {
        accumulator_ = 0; // (the game stops while a window is up)
    } else if (!paused_ && simUncapped_) {
        ticks = 8; // (the settings' uncapped simulation: as many ticks as a frame may take)
        accumulator_ = 0;
    } else if (!paused_) {
        accumulator_ += dt;
        double step = 1.0 / tps_;
        while (accumulator_ >= step && ticks < 8) {
            accumulator_ -= step;
            ticks++;
        }
        if (ticks == 8)
            accumulator_ = 0;
    } else if (stepOnce_) {
        ticks = 1;
    }
    stepOnce_ = false;
    // TPT's BeforeSim runs every frame, whether or not a tick does (60 a second)
    int idleFrames = 0;
    if (ticks == 0 && windows_.empty()) {
        sandClock_ += dt * 60.0;
        idleFrames = std::min(int(sandClock_), 8);
        sandClock_ -= std::floor(sandClock_);
    } else
        sandClock_ = 0;

    const uint32_t sw = sw_, sh = sh_;
    world_->ensureFrameBuffer(sw, sh);
    world_->setStickmanKeys(comm1_, pcomm1_, comm2_, pcomm2_);
    FrameRequest req;
    req.ticks = ticks;
    req.view = view_;
    req.idleFrames = idleFrames;
    req.uses = input_.takeUses();
    req.uses.insert(req.uses.end(), pendingUses_.begin(), pendingUses_.end());
    pendingUses_.clear();
    req.camX = cam_.x;
    req.camY = cam_.y;
    req.zoom = cam_.zoom;
    req.screenW = sw;
    req.screenH = sh;
    float bwx, bwy;
    toWorld(mouseX_, mouseY_, bwx, bwy);
    req.brushX = int(std::floor(bwx));
    req.brushY = int(std::floor(bwy));
    // TPT shows the brush while the mouse is over the simulation or drawing
    {
        int us = uiScale();
        bool over = ui_.overWorld(int(mouseX_) / us, int(mouseY_) / us) && inWorld(mouseX_, mouseY_);
        cursor_.clear();
        if (selectMode_ == Select::None && (input_.drawing() || over)) {
            int px, py;
            worldPoint(mouseX_, mouseY_, px, py);
            brushCursor(input_.cursor(px, py), input_.brush, cursor_);
        }
        req.cursor = cursor_;
    }
    // TPT shows the gravity zones while a GRAV wall tool is chosen
    bool gravTool = false;
    for (int s = 0; s < 3; s++)
        if (const UiTool *t = ui_.activeTool(s))
            gravTool |= t->kind == UiTool::Kind::Wall && t->id == WL_GRAV;
    req.overlays = (gravityGrid_ ? 1u : 0u) | (gravTool ? 2u : 0u);
    req.slot = slot;
    bool timed = false;
    if (ticks > 0 && world_->gpuTimed() && queries) {
        vkCmdResetQueryPool(cmd, queries, 0, 4);
        timed = true;
        req.queries = queries;
    }

    // A window's frozen screen: what the last frame showed, kept while it is up
    while (frozen_.size() > windows_.depth()) {
        vkDeviceWaitIdle(dev); // (an earlier frame may still read it)
        ctx_.destroyBuffer(frozen_.back());
        frozen_.pop_back();
    }
    const VkDeviceSize frameBytes = VkDeviceSize(sw) * sh * 4;
    while (frozen_.size() < windows_.depth()) {
        frozen_.push_back(
            ctx_.createBuffer(frameBytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT, false));
        computeBarrier(cmd);
        VkBufferCopy copy{0, 0, frameBytes};
        vkCmdCopyBuffer(cmd, world_->frameBuffer().buffer, frozen_.back().buffer, 1, &copy);
    }
    // A window that shows the world (the renderer options) has it drawn live under it, not ticking
    const bool live = !windows_.empty() && windows_.top()->showsWorld();
    if (live) {
        req.cursor.clear();
        world_->record(cmd, req);
    } else if (windows_.empty()) {
        world_->record(cmd, req);
        ticksDone_ += ticks;
        if (selectMode_ != Select::None)
            drawSelectLayer(cmd, slot);
    } else {
        timed = false;
        computeBarrier(cmd);
        if (frozen_.back().size >= frameBytes) {
            VkBufferCopy copy{0, 0, frameBytes};
            vkCmdCopyBuffer(cmd, frozen_.back().buffer, world_->frameBuffer().buffer, 1, &copy);
        } else
            vkCmdFillBuffer(cmd, world_->frameBuffer().buffer, 0, frameBytes, 0xFF000000u);
        computeBarrier(cmd);
    }

    // The interface over it (a new scale from the settings once their window has gone)
    if (pendingScale_ >= 0 && windows_.empty()) {
        uiScale_ = pendingScale_;
        pendingScale_ = -1;
    }
    const int us = uiScale();
    if (!windows_.empty()) {
        windows_.tick(dt * 1000.0);
        canvas_.resize(int(sw) / us, int(sh) / us);
        if (dynamic_cast<SignWindow *>(windows_.top()))
            drawSignLayer(cmd, slot); // (the fade, then the signs over it)
        else if (!live)
            canvas_.blendFilledRect(0, 0, canvas_.width(), canvas_.height(), rgb(0x000000, windows_.fadeAlpha()));
        windows_.draw(canvas_);
    } else {
        ui_.setSize(int(sw) / us, int(sh) / us);
        ui_.tick(dt * 1000.0);
        UiState st;
        st.fps = uiFps_;
        st.paused = paused_;
        st.gravityGrid = gravityGrid_;
        st.decorations = view_.decorationLevel != 0;
        st.prettyPowder = settings_.prettyPowder;
        st.newtonian = settings_.newtonianGravity;
        st.ambientHeat = settings_.ambientHeat;
        st.showHud = showHud_, st.showDebug = showDebug_;
        st.introText = introText_;
        if (introText_ > 0) {
            static const std::u32string intro = introMessage();
            st.introMessage = intro;
        }
        st.infoTip = infoTip_, st.infoTipPresence = infoTipPresence_;
        st.temperatureScale = temperatureScale_;
        st.gridSize = view_.gridSize;
        st.finding = view_.findKind != 0;
        st.foundParticles = st.finding ? world_->foundParticles() : 0;
        // (the debug HUD's particle count, read twice a second: it waits for the GPU)
        sinceCount_ += dt;
        if (showDebug_ && sinceCount_ > 0.5) {
            particleCount_ = world_->particleCount();
            sinceCount_ = 0;
        }
        st.numParts = uint32_t(particleCount_);
        // (the heat display's range shows when it follows what is shown: TEMP L and H)
        if (view_.autoHeat)
            st.autoHeat = world_->heatRange(st.hdispMin, st.hdispMax);
        st.customGol = &customGol_;
        st.replaceMode = (input_.replaceFlags() & REPLACE_MODE) != 0;
        st.specificDelete = (input_.replaceFlags() & SPECIFIC_DELETE) != 0;
        st.saveName = utf32(saveName_);
        st.sample = world_->cursorSample();
        ui_.draw(canvas_, st);
    }
    overlay_.record(cmd, slot, canvas_.pixels(), uint32_t(canvas_.width()), uint32_t(canvas_.height()), uint32_t(us),
                    world_->frameBuffer(), sw, sh);
    sinceStats_ += dt;
    return timed;
}

std::string GameScreen::status(double fps, double ticksPerSec, const char *present, double gpuTickMs) {
    if (sinceStats_ > 2.0) {
        particleCount_ = world_->particleCount();
        sinceStats_ = 0;
    }
    double tickMs = world_->gpuTimed() ? gpuTickMs : world_->cpuTickMs();
    std::string passes = world_->passesName();
    static const char *edgeNames[] = {"void", "solid", "loop"};
    static const char *gravNames[] = {"vertical", "off", "radial", "custom"};
    static const char *airNames[] = {"air on", "pressure off", "velocity off", "air off", "air frozen"};
    const UiTool *t0 = ui_.activeTool(0);
    std::string tool;
    if (t0)
        for (char32_t ch : t0->kind == UiTool::Kind::Wall ? t0->description.substr(0, 24) : t0->name)
            tool += char(ch < 128 ? ch : '?');
    char title[512];
    std::snprintf(title, sizeof title,
                  "Big Powder " BP_VERSION " %s %ux%u | %.0f fps (%s) | %.1f tps of %.0f | %s %.2f ms/tick | %.2fM particles | %s "
                  "r=%d,%d | edge %s, gravity %s, %s%s%s%s | %s%s",
                  mode_.c_str(), world_->width(), world_->height(), fps, present, ticksPerSec, tps_,
                  world_->gpuTimed() ? "GPU" : "CPU", tickMs, double(particleCount_) / 1e6, tool.c_str(),
                  input_.brush.rx(), input_.brush.ry(), edgeNames[settings_.edgeMode],
                  gravNames[settings_.gravityMode], airNames[settings_.airMode],
                  settings_.ambientHeat ? ", ambient heat" : "", passes.empty() ? "" : " | ", passes.c_str(),
                  scenes_[sceneIdx_].c_str(), paused_ ? " | PAUSED" : "");
    return title;
}

} // namespace bp
