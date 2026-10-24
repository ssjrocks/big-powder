#include "app/GameScreen.h"
#include "sim/WorldFile.h"
#include "ui/BrushCursor.h"
#include "ui/Dialogs.h"

#include <SDL3/SDL_clipboard.h>
#include <SDL3/SDL_stdinc.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <filesystem>

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

std::string savesDir() {
    std::filesystem::path dir = std::filesystem::path(dataDir()) / "saves";
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
    : ctx_(ctx), o_(o), mode_(o.mode), scenes_(sceneNames()), uiTable_(ElementTable::build()), canvas_(uiTable_),
      ui_(uiTable_), overlay_(ctx, o.shaderDir), input_(ui_), tps_(o.tps) {
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
    settings_.edgeMode = o.edgeMode;
    settings_.gravityMode = o.gravityMode;
    settings_.ambientHeat = o.ambientHeat;
    settings_.newtonianGravity = o.newtonian;
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
    vkDeviceWaitIdle(ctx_.device());
    for (Buffer &b : frozen_)
        ctx_.destroyBuffer(b);
    if (ui::host().font == &canvas_)
        ui::host().font = nullptr;
}

void GameScreen::report(const std::string &msg) {
    std::printf("%s\n", msg.c_str());
    std::fflush(stdout);
}

void GameScreen::loadScene() { world_->loadScene(scenes_[sceneIdx_]); }

void GameScreen::switchMode(const std::string &mode) {
    vkDeviceWaitIdle(ctx_.device());
    world_.reset(); // only one TPT engine may exist at a time
    mode_ = mode;
    world_ = makeWorld(ctx_, o_, mode_);
    world_->applySettings(settings_);
    loadScene();
    fitView();
    sinceStats_ = 1e9;
}

int GameScreen::uiScale() const {
    if (o_.uiScale > 0)
        return o_.uiScale;
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
                                                }));
}

void GameScreen::openElementSearch() {
    showWindow(std::make_unique<ElementSearchWindow>(ui_, [this](int slot, int tool) { ui_.chooseTool(slot, tool); }));
}

void GameScreen::afterInput() {
    int index, slot;
    while (ui_.takeChosen(index, slot))
        if (ui_.tools()[size_t(index)].kind == UiTool::Kind::Property)
            openPropertyWindow(nullptr);
    for (const GameInput::Sample &sm : input_.takeSamples()) {
        ClassicParticle p;
        vkDeviceWaitIdle(ctx_.device());
        if (!world_->particleAt(sm.x, sm.y, true, p))
            continue;
        if (sm.shift) {
            ui_.setActiveToolIndex(0, ui_.toolIndex("DEFAULT_UI_PROPERTY"));
            openPropertyWindow(&p);
        } else if (p.type == PT_LIFE) {
            // its ruleset's tool (custom rulesets come with their window)
            for (size_t i = 0; i < ui_.tools().size(); i++)
                if (ui_.tools()[i].kind == UiTool::Kind::Life && ui_.tools()[i].id == p.ctype)
                    ui_.setActiveToolIndex(0, int(i));
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
        running_ = false;
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
        else if (ev.wheel.integer_y)
            input_.wheel(ev.wheel.integer_y);
        break;
    case SDL_EVENT_WINDOW_FOCUS_LOST:
        input_.blur();
        panning_ = zoomKey_ = false;
        break;
    case SDL_EVENT_MOUSE_BUTTON_DOWN: {
        mouseX_ = ev.button.x * s;
        mouseY_ = ev.button.y * s;
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
        if (!inWorld(mouseX_, mouseY_))
            break;
        int px, py;
        worldPoint(mouseX_, mouseY_, px, py);
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
        int fx, fy;
        worldPoint(mouseX_, mouseY_, fx, fy);
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
        input_.mouseMove(px, py);
        break;
    }
    default:
        break;
    }
}

void GameScreen::keyUp(const SDL_Event &ev) {
    input_.setModifiers(ev.key.mod & SDL_KMOD_SHIFT, ev.key.mod & SDL_KMOD_CTRL, ev.key.mod & SDL_KMOD_ALT);
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
    if (ev.key.scancode == SDL_SCANCODE_Z && !ctrl)
        zoomKey_ = true;
    if (!repeat) {
        if (k == SDLK_RIGHT)
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
    // While STKM2 is out, W only steers it (with ctrl it still switches gravity), as in TPT.
    bool wFree = ctrl || !world_->stickman2Out();
    if (k == SDLK_ESCAPE)
        running_ = false;
    else if (k == SDLK_SPACE)
        paused_ = !paused_;
    else if (k == SDLK_PERIOD)
        stepOnce_ = true;
    else if (k == SDLK_V || k == SDLK_H)
        world_->cycleDisplay();
    else if (k == SDLK_G && ctrl)
        gravityGrid_ = !gravityGrid_;
    else if (k == SDLK_B && ctrl) {
        decorations_ = !decorations_;
        world_->setDecorations(decorations_);
    } else if (k == SDLK_C)
        world_->cyclePasses();
    else if (k == SDLK_M)
        switchMode(mode_ == "classic" ? (o_.mode == "huge" ? "huge" : "screen") : "classic");
    else if (k == SDLK_S && ctrl && shift) {
        // the TPT-sized area around the mouse, as a TPT save
        vkDeviceWaitIdle(dev);
        std::string msg;
        float wx, wy;
        toWorld(mouseX_, mouseY_, wx, wy);
        world_->exportTpt(savesDir() + "/" + mode_ + "-" + timestamp() + ".cps", int(wx), int(wy), msg);
        report(msg);
    } else if (k == SDLK_S && ctrl) {
        vkDeviceWaitIdle(dev);
        std::string msg;
        world_->save(savesDir() + "/" + mode_ + "-" + timestamp() + world_->saveExtension(), msg);
        report(msg);
    } else if (k == SDLK_E && !ctrl) {
        if (!repeat)
            openElementSearch();
    } else if ((k == SDLK_P || k == SDLK_F2) && ctrl) {
        // TPT's ctrl+P: the property tool (with shift, on the right button)
        if (!repeat)
            ui_.chooseTool(shift ? 1 : 0, ui_.toolIndex("DEFAULT_UI_PROPERTY"));
    } else if (k == SDLK_E || (k == SDLK_W && wFree) || k == SDLK_Y || k == SDLK_U || k == SDLK_N) {
        if (k == SDLK_E) // (TPT's ctrl+E)
            settings_.edgeMode = (settings_.edgeMode + 1) % 3;
        else if (k == SDLK_W)
            settings_.gravityMode = (settings_.gravityMode + 1) % 3;
        else if (k == SDLK_Y)
            settings_.airMode = (settings_.airMode + 1) % 5;
        else if (k == SDLK_U)
            settings_.ambientHeat = !settings_.ambientHeat;
        else // TPT's N: Newtonian gravity
            settings_.newtonianGravity = !settings_.newtonianGravity;
        vkDeviceWaitIdle(dev);
        world_->applySettings(settings_);
    } else if (k == SDLK_TAB) {
        if (!repeat)
            input_.changeBrush();
    } else if (k == SDLK_F6) {
        sceneIdx_ = (sceneIdx_ + 1) % scenes_.size();
        vkDeviceWaitIdle(dev);
        loadScene();
    } else if (k == SDLK_R) {
        vkDeviceWaitIdle(dev);
        loadScene();
    } else if (ev.key.scancode == SDL_SCANCODE_LEFTBRACKET && !repeat)
        input_.bracket(-1, alt);
    else if (ev.key.scancode == SDL_SCANCODE_RIGHTBRACKET && !repeat)
        input_.bracket(1, alt);
    else if ((ev.key.scancode == SDL_SCANCODE_SEMICOLON || k == SDLK_INSERT) && !repeat)
        input_.toggleReplace(ctrl);
    else if (k == SDLK_DELETE && !repeat)
        input_.toggleSpecificDelete();
    else if (k == SDLK_MINUS)
        tps_ = std::max(1.875, tps_ / 2);
    else if (k == SDLK_EQUALS)
        tps_ = std::min(960.0, tps_ * 2);
    else if (k == SDLK_HOME)
        fitView();
    else if (k == SDLK_F11)
        fullscreenToggle_ = true;
}

bool GameScreen::record(VkCommandBuffer cmd, int slot, double dt, VkQueryPool queries) {
    VkDevice dev = ctx_.device();
    nowMs_ += dt * 1000.0;
    ui::host().now = nowMs_;
    afterInput();
    // What the interface's buttons asked for
    switch (ui_.takeAction()) {
    case UiAction::ElementSearch:
        openElementSearch();
        break;
    case UiAction::Reload:
        vkDeviceWaitIdle(dev);
        loadScene();
        break;
    case UiAction::Save: {
        vkDeviceWaitIdle(dev);
        std::string msg;
        world_->save(savesDir() + "/" + mode_ + "-" + timestamp() + world_->saveExtension(), msg);
        report(msg);
        break;
    }
    case UiAction::Clear:
        vkDeviceWaitIdle(dev);
        world_->loadScene("empty");
        saveName_ = "[untitled simulation]";
        break;
    case UiAction::Pause:
        paused_ = !paused_;
        break;
    case UiAction::RenderOptions: // (TPT opens its renderer options; the presets for now)
        world_->cycleDisplay();
        break;
    case UiAction::GravityGrid:
        gravityGrid_ = !gravityGrid_;
        break;
    case UiAction::Decorations:
        decorations_ = !decorations_;
        world_->setDecorations(decorations_);
        break;
    case UiAction::Newtonian:
        settings_.newtonianGravity = !settings_.newtonianGravity;
        vkDeviceWaitIdle(dev);
        world_->applySettings(settings_);
        break;
    case UiAction::AmbientHeat:
        settings_.ambientHeat = !settings_.ambientHeat;
        vkDeviceWaitIdle(dev);
        world_->applySettings(settings_);
        break;
    default:
        break;
    }
    if (windows_.empty()) {
        int px, py;
        worldPoint(mouseX_, mouseY_, px, py);
        input_.tick(px, py);
    }

    // Fixed-rate simulation clock. If the engine can't keep up the sim slows down instead of skipping.
    int ticks = 0;
    if (!windows_.empty()) {
        accumulator_ = 0; // (the game stops while a window is up)
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

    const uint32_t sw = sw_, sh = sh_;
    world_->ensureFrameBuffer(sw, sh);
    world_->setStickmanKeys(comm1_, pcomm1_, comm2_, pcomm2_);
    FrameRequest req;
    req.ticks = ticks;
    req.uses = input_.takeUses();
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
        if (input_.drawing() || over) {
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
    if (windows_.empty()) {
        world_->record(cmd, req);
        ticksDone_ += ticks;
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

    // The interface over it
    const int us = uiScale();
    if (!windows_.empty()) {
        windows_.tick(dt * 1000.0);
        canvas_.resize(int(sw) / us, int(sh) / us);
        canvas_.blendFilledRect(0, 0, canvas_.width(), canvas_.height(), rgb(0x000000, windows_.fadeAlpha()));
        windows_.draw(canvas_);
    } else {
        ui_.setSize(int(sw) / us, int(sh) / us);
        ui_.tick(dt * 1000.0);
        UiState st;
        st.fps = uiFps_;
        st.paused = paused_;
        st.gravityGrid = gravityGrid_;
        st.decorations = decorations_;
        st.newtonian = settings_.newtonianGravity;
        st.ambientHeat = settings_.ambientHeat;
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
    static const char *gravNames[] = {"vertical", "off", "radial"};
    static const char *airNames[] = {"air on", "pressure off", "velocity off", "air off", "air frozen"};
    const UiTool *t0 = ui_.activeTool(0);
    std::string tool;
    if (t0)
        for (char32_t ch : t0->kind == UiTool::Kind::Wall ? t0->description.substr(0, 24) : t0->name)
            tool += char(ch < 128 ? ch : '?');
    char title[512];
    std::snprintf(title, sizeof title,
                  "Big Powder %s %ux%u | %.0f fps (%s) | %.1f tps of %.0f | %s %.2f ms/tick | %.2fM particles | %s "
                  "r=%d,%d | %s | edge %s, gravity %s, %s%s%s%s | %s%s",
                  mode_.c_str(), world_->width(), world_->height(), fps, present, ticksPerSec, tps_,
                  world_->gpuTimed() ? "GPU" : "CPU", tickMs, double(particleCount_) / 1e6, tool.c_str(),
                  input_.brush.rx(), input_.brush.ry(), world_->displayName().c_str(), edgeNames[settings_.edgeMode],
                  gravNames[settings_.gravityMode], airNames[settings_.airMode],
                  settings_.ambientHeat ? ", ambient heat" : "", passes.empty() ? "" : " | ", passes.c_str(),
                  scenes_[sceneIdx_].c_str(), paused_ ? " | PAUSED" : "");
    return title;
}

} // namespace bp
