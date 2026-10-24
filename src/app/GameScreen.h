// The game screen: TPT's GameView and GameController with their windows, between the input and the frames. It owns
// the world (Classic or GPU), the camera, TPT's interface over it (ui/GameUi, ui/GameInput, ui/Dialogs) and the
// simulation clock. The viewer feeds it SDL's events and shows what it draws; --check-screen drives it with made-up
// events and no window.
#pragma once
#include "app/App.h"
#include "app/World.h"
#include "gpu/Overlay.h"
#include "ui/Canvas.h"
#include "ui/GameInput.h"
#include "ui/GameUi.h"
#include "ui/Widgets.h"

#include <SDL3/SDL_events.h>

#include <memory>
#include <string>
#include <vector>

namespace bp {

struct Camera {
    float x = 0, y = 0, zoom = 1; // world position of the top-left screen pixel; screen px per cell

    void fit(uint32_t sw, uint32_t sh, uint32_t W, uint32_t H);
    void zoomAt(float sx, float sy, float factor);
};

std::string dataDir(); // ~/.local/share/big-powder

class GameScreen {
public:
    GameScreen(VkContext &ctx, const AppOptions &o);
    ~GameScreen();

    // The screen's size in pixels; the interface's scale follows its height unless the options set it
    void resize(uint32_t w, uint32_t h);
    int uiScale() const;
    // One of SDL's events, its positions in window points (pixelScale pixels each)
    void event(const SDL_Event &ev, float pixelScale = 1.0f);
    // A frame: what the input does every frame, the simulation's ticks for dt seconds of its clock, then the world and
    // the interface drawn into frameBuffer(). With a query pool, the first tick writes its timestamps there; true if
    // it did.
    bool record(VkCommandBuffer cmd, int slot, double dt, VkQueryPool queries);
    const Buffer &frameBuffer() const { return world_->frameBuffer(); }

    bool running() const { return running_; }
    bool wantsTextInput() const { return windows_.wantsTextInput(); }
    bool takeFullscreenToggle() {
        bool t = fullscreenToggle_;
        fullscreenToggle_ = false;
        return t;
    }
    void setFps(double fps) { uiFps_ = fps; }
    // For the window's title: the mode, the clock, the tool, the settings
    std::string status(double fps, double ticksPerSec, const char *present, double gpuTickMs);
    int takeTicks() {
        int t = ticksDone_;
        ticksDone_ = 0;
        return t;
    }
    bool gpuTimed() const { return world_->gpuTimed(); }

    // What the tests look at
    World &world() { return *world_; }
    GameUi &ui() { return ui_; }
    GameInput &input() { return input_; }
    ui::Engine &windows() { return windows_; }
    const Camera &camera() const { return cam_; }
    // The world pixel under a screen pixel
    void toWorld(float sx, float sy, float &wx, float &wy) const;

private:
    void loadScene();
    void switchMode(const std::string &mode);
    void fitView();
    bool inWorld(float sx, float sy) const;
    void worldPoint(float sx, float sy, int &x, int &y) const; // GameController::PointTranslate
    void report(const std::string &msg);
    void windowEvent(const SDL_Event &ev, float s);
    void keyDown(const SDL_Event &ev);
    void keyUp(const SDL_Event &ev);
    void afterInput(); // Tool::Select and SampleTool::Draw: what choosing a tool or sampling does beyond that
    void showWindow(std::unique_ptr<ui::Window> w);
    void openPropertyWindow(const ClassicParticle *from);
    void openElementSearch();

    VkContext &ctx_;
    AppOptions o_;
    std::string mode_;
    std::unique_ptr<World> world_;
    const std::vector<std::string> &scenes_;
    size_t sceneIdx_ = 0;
    std::string saveName_; // the save button's text: the scene or file loaded

    // TPT's game screen over the world (ui/GameUi), drawn in TPT's pixels times the interface scale
    ElementTable uiTable_;
    Canvas canvas_;
    GameUi ui_;
    Overlay overlay_;
    // TPT's windows over the game screen (ui/Dialogs). While one is up the game stops, as in TPT: the screen as it was
    // when it opened shows under it (`frozen_`, one per window), darkening.
    ui::Engine windows_;
    std::vector<Buffer> frozen_;
    // TPT's drawing, as its GameView handles the mouse (ui/GameInput). Middle drag (or alt+drag) pans instead of TPT's
    // middle-button tool, and Z with the wheel zooms.
    GameInput input_;
    int propertyField_ = 0; // the property window's last property and value (TPT's Prop.Type and Prop.Value)
    std::u32string propertyText_;
    int temperatureScale_ = TEMPSCALE_CELSIUS;
    Camera cam_;
    uint32_t sw_ = 0, sh_ = 0;

    bool running_ = true, paused_ = false, stepOnce_ = false, fullscreenToggle_ = false;
    bool gravityGrid_ = false; // TPT's ctrl+G: the gravity field display
    bool decorations_ = true;  // ctrl+B
    WorldSettings settings_;
    double tps_;
    bool panning_ = false, zoomKey_ = false;
    uint8_t panButton_ = 0;
    float mouseX_ = 0, mouseY_ = 0; // in pixels
    std::vector<int> cursor_;
    double uiFps_ = 0, accumulator_ = 0, nowMs_ = 0;
    // The players' keys (TPT's GameController): arrows steer STKM, WASD STKM2.
    uint8_t comm1_ = 0, pcomm1_ = 0, comm2_ = 0, pcomm2_ = 0;
    int ticksDone_ = 0;
    uint64_t particleCount_ = 0;
    double sinceStats_ = 1e9;
};

} // namespace bp
