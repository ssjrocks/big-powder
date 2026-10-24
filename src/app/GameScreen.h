// The game screen: TPT's GameView and GameController with their windows, between the input and the frames. It owns
// the world (Classic or GPU), the camera, TPT's interface over it (ui/GameUi, ui/GameInput, ui/Dialogs) and the
// simulation clock. The viewer feeds it SDL's events and shows what it draws; --check-screen drives it with made-up
// events and no window.
#pragma once
#include "app/App.h"
#include "app/History.h"
#include "app/Stamps.h"
#include "app/World.h"
#include "gpu/Overlay.h"
#include "ui/Canvas.h"
#include "ui/GameInput.h"
#include "ui/GameUi.h"
#include "ui/Widgets.h"

#include <SDL3/SDL_events.h>

#include <memory>
#include <optional>
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
    History &history() { return history_; }
    // TPT's select modes (GameView's): an area being chosen to copy, cut or make a stamp of, or a save being placed
    enum class Select { None, Copy, Cut, Stamp, Place };
    Select selectMode() const { return selectMode_; }
    const std::optional<SaveArea> &clipboard() const { return clipboard_; }
    const std::optional<SaveArea> &placing() const { return placed_; } // (as it is placed: transformed)
    Stamps &stamps() { return stamps_; }
    bool paused() const { return paused_; }
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
    void flushUses();  // the tools' uses so far, used now (before a snapshot of the world)
    void historySnapshot(); // GameController::HistorySnapshot
    void showWindow(std::unique_ptr<ui::Window> w);
    void openPropertyWindow(const ClassicParticle *from);
    void openElementSearch();
    // GOLTool::OpenWindow, then GOLWindow::validate and GameModel::AddCustomGol
    void openGolWindow(int slot, int rule, uint32_t colour1, uint32_t colour2);
    // Signs: SignTool::Click; GameController::GetSignAt (the last sign whose box holds the point); a link sign's click
    void signToolClick(int x, int y);
    int signAt(int x, int y);
    bool linkSignPress(int x, int y, int button);
    bool linkSignRelease(int x, int y, int button);
    void signTooltip();
    void drawSignLayer(VkCommandBuffer cmd, int slot); // SignWindow::DoDraw: every sign over the frozen picture
    void addCustomGol(int slot, std::u32string name, const std::u32string &rule, uint32_t colour1, uint32_t colour2);
    // Selections (app/GameSelect.cpp): GameView's BeginStampSelection and the like
    void beginSelect(Select mode, const std::u32string &tip);
    void setPlaceSave(std::optional<SaveArea> save); // GameModel::SetPlaceSave, then NotifyPlaceSaveChanged
    void applyTransformPlaceSave(); // ApplyTransformPlaceSave: TransformPlaceSave and NotifyTransformedPlaceSaveChanged
    void translateSave(int dx, int dy);
    void transformSave(const Mat2i &m);
    void placeSavePos(int &bx, int &by) const; // PlaceSavePos, in blocks
    bool placeKey(SDL_Keycode k, SDL_Scancode sc, bool repeat, bool shift, bool ctrl); // arrows and R while placing
    void selectPress(int button);
    void selectRelease(int button);
    void selectMove();
    void drawSelectLayer(VkCommandBuffer cmd, int slot);
    void copyRegion(int x1, int y1, int x2, int y2); // GameController's CopyRegion, CutRegion, StampRegion, PlaceSave
    void cutRegion(int x1, int y1, int x2, int y2);
    void stampRegion(int x1, int y1, int x2, int y2);
    void placeSave(int bx, int by);
    void loadLastStamp(); // L: the first stamp, to place
    void openStamps();    // K: the stamp browser (GameController::OpenStamps)
    void openChosenStamp(); // its callback, once it has gone

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
    std::vector<CustomGol> customGol_;                         // SimulationData's custom Game of Life types
    int foundSign_ = -1;                     // GameController's foundSignID: a sign pressed on
    std::vector<SignSample> windowSamples_;  // what the signs read, while the sign window is up
    Canvas signLayer_;
    std::vector<ToolUse> pendingUses_;       // what the interface uses besides the brush (a button sign's spark)
    History history_;                        // undo
    Select selectMode_ = Select::None;
    int selectX1_ = -1, selectY1_ = -1, selectX2_ = -1, selectY2_ = -1; // selectPoint1 and selectPoint2
    bool selectDown_ = false;                // (GameView's isMouseDown while selecting)
    std::optional<SaveArea> clipboard_;      // Clipboard's data
    std::optional<SaveArea> placeSave_, placed_; // GameModel's placeSave and transformedPlaceSave
    Mat2i placeTransform_ = MAT_IDENTITY;    // GameView's placeSaveTransform and placeSaveTranslate
    int placeTranslateX_ = 0, placeTranslateY_ = 0;
    std::vector<uint32_t> placeThumb_;       // placeSaveThumb
    uint64_t thumbVersion_ = 0;
    bool includePressure_ = true;            // Simulation.IncludePressure
    bool shift_ = false;                     // GameView's shiftBehaviour
    Stamps stamps_;
    std::optional<std::pair<std::string, bool>> chosenStamp_; // the browser's choice, and whether to move it first
    std::u32string lastGolName_ = U"CGOL", lastGolRule_ = U"B3/S23"; // (TPT's CustomGOL.Name and CustomGOL.Rule)
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
