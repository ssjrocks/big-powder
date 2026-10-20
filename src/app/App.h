#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace bp {

struct AppOptions {
    bool bench = false;
    std::vector<std::string> scenes{"mixed"};
    std::vector<uint32_t> colours{3};
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

} // namespace bp
