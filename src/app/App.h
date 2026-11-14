#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace bp {

struct AppOptions {
    bool bench = false;
    bool compare = false; // accuracy check against TPT (Classic)
    int interval = 50;    // compare: ticks between checkpoints
    int passOrder = 0;    // GPU colour pass order, see SimConfig::passOrder
    bool serial = false;
    std::vector<int> stickmanKeys{0, 0, 0, 0}; // compare: STKM's comm and pcomm, then STKM2's, held all along
    bool noAirCoupling = false; // compare: set Advection and AirDrag to 0 for all elements in both engines  // compare: also run the GPU port serially in id order (TPT's order)
    int tptAirMode = 0;
    int edgeMode = 0;     // 0 void, 1 solid, 2 loop
    int gravityMode = 0;  // 0 vertical, 1 off, 2 radial
    bool ambientHeat = false;
    bool newtonian = false; // TPT's Newtonian gravity
    int uiScale = 0;        // viewer: TPT pixels per interface pixel (0: from the window's height)
    bool airTest = false;     // compare: air solver only, no particles   // compare: TPT's air mode (4 = no air update, like the GPU engine before its air solver)
    std::string mode = "screen";     // viewer: classic (TPT's engine, 612x384), screen (GPU), huge (GPU, 7680x4320)
    std::string loadPath;             // viewer: TPT save to open at start
    std::string exportPath;           // write --scene as a TPT save and exit
    int exportTicks = 0;              // ...after running TPT this many ticks
    std::string checkSave;            // compare: paste this TPT save into a GPU world and check it against TPT
    std::string checkWorld;           // compare: save each scene to this world file, load it back and check it
    std::string checkExport;          // compare: write an area of each scene as this TPT save and check it in TPT
    bool zoomCheck = false;           // compare: check zoomed-out views against box averages of the world image
    bool sleepStats = false;          // compare: how much of each scene changes not at all from tick to tick
    std::string uiSnapshot;           // compare: the viewer's picture with the interface, without a window
    bool toolCheck = false;           // compare: TPT's tools used the same way in TPT and on the GPU
    bool screenCheck = false;         // compare: the game screen driven by made-up input (app/ScreenCheck)
    bool undoCheck = false;           // compare: snapshots for undo, timed and checked
    bool clipboardCheck = false;      // compare: copy, transform, paste and cut, in TPT and on the GPU
    std::string dataDir;              // where stamps and saves go (~/.local/share/big-powder if empty)
    bool renderCheck = false;         // compare TPT's renderer with the GPU's on the same state
    std::vector<int> renderPresets;   // ...for these display presets (all if empty)
    int overlays = 0;                 // ...with the gravity field (1) and gravity zones (2) displays
    int renderGrid = 0;               // ...with the grid (G: its size in cells, 0-9)
    std::string renderFind;           // ...finding an element (ctrl+F), by name
    int renderFrames = 3;             // ...after drawing this many frames
    bool sizeSet = false;             // --size given
    std::vector<std::string> scenes{"mixed"};
    struct Colours { uint32_t nx, ny; };
    std::vector<Colours> colours{{7, 7}}; // colour grids to run (bench/compare use all, the viewer the first)
    uint32_t W = 3840, H = 2160;
    int warmup = 60;   // bench: ticks before timing
    int ticks = 300;   // bench: timed ticks
    int batch = 10;    // bench: ticks per submit
    float fill = 1.0f;
    uint32_t seed = 1;
    bool validate = false;
    double tps = 60.0; // viewer: simulation ticks per second
    std::string present = "fifo";
    bool fullscreen = false;
    std::string shaderDir;
    std::string snapshot; // bench: write PREFIX-<scene>-<N>[-zoom].ppm after each run
};

constexpr int VIEWER_RESTART = 75; // runViewer: an update was installed, start the new program
int runViewer(const AppOptions &o);
int runBench(const AppOptions &o);
int runCompare(const AppOptions &o);
int runScreenCheck(const AppOptions &o);

} // namespace bp
