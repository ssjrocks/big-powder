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
    bool noAirCoupling = false; // compare: set Advection and AirDrag to 0 for all elements in both engines  // compare: also run the GPU port serially in id order (TPT's order)
    int tptAirMode = 0;
    int edgeMode = 0;     // 0 void, 1 solid, 2 loop
    int gravityMode = 0;  // 0 vertical, 1 off, 2 radial
    bool ambientHeat = false;
    bool airTest = false;     // compare: air solver only, no particles   // compare: TPT's air mode (4 = no air update, like the GPU engine before its air solver)
    std::string mode = "screen";     // viewer: classic (TPT's engine, 612x384), screen (GPU), huge (GPU, 7680x4320)
    std::string loadPath;             // viewer: TPT save to open at start
    std::string exportPath;           // write --scene as a TPT save and exit
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

int runViewer(const AppOptions &o);
int runBench(const AppOptions &o);
int runCompare(const AppOptions &o);

} // namespace bp
