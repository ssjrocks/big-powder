#include "app/App.h"
#include "sim/Scenes.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <filesystem>
#include <sstream>

using namespace bp;

static void usage() {
    std::puts(
        "usage: big-powder [options]\n"
        "  --scene NAME[,NAME...]  empty, water, sand, rain, mixed, or all (bench)   [mixed]\n"
        "  --size WxH              world size in cells, multiples of 4               [3840x2160]\n"
        "  --colours G[,G...]      colour grid N (N x N) or NXxNY, sides 1..16        [7]\n"
        "  --fill F                scale the amount of material in a scene          [1]\n"
        "  --seed N\n"
        "  --tps N                 viewer: simulation ticks per second             [60]\n"
        "  --present MODE          viewer: fifo (vsync), mailbox or immediate       [fifo]\n"
        "  --fullscreen\n"
        "  --bench                 run headless benchmarks instead of the viewer\n"
        "  --warmup N --ticks N --batch N   bench timing                           [60 300 10]\n"
        "  --compare               compare the GPU engine with TPT's own engine at 612x384\n"
        "  --interval N            compare: ticks between checkpoints                [50]\n"
        "  --pass-order N          0 random, 1/2 raster, 3/4 fixed random  [0]\n"
        "  --tpt-air N             compare: TPT air mode (0 on ... 4 no update)       [0]\n"
        "  --snapshot PREFIX       bench: save PREFIX-<scene>-<N>.ppm (whole world) and -zoom.ppm\n"
        "  --validate              enable Vulkan validation layers\n");
}

template <class T> static std::vector<T> splitList(const std::string &s, T (*conv)(const std::string &)) {
    std::vector<T> out;
    std::stringstream ss(s);
    std::string item;
    while (std::getline(ss, item, ','))
        if (!item.empty())
            out.push_back(conv(item));
    return out;
}

int main(int argc, char **argv) {
    AppOptions o;
    for (int i = 1; i < argc; i++) {
        std::string a = argv[i];
        auto next = [&]() -> std::string {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "missing value for %s\n", a.c_str());
                std::exit(2);
            }
            return argv[++i];
        };
        if (a == "--scene") {
            std::string v = next();
            o.scenes = v == "all" ? sceneNames()
                                  : splitList<std::string>(v, [](const std::string &s) { return s; });
        } else if (a == "--size") {
            std::string v = next();
            if (std::sscanf(v.c_str(), "%ux%u", &o.W, &o.H) != 2) {
                std::fprintf(stderr, "bad --size %s\n", v.c_str());
                return 2;
            }
        } else if (a == "--colours") {
            o.colours = splitList<AppOptions::Colours>(next(), [](const std::string &s) {
                unsigned nx = 0, ny = 0;
                if (std::sscanf(s.c_str(), "%ux%u", &nx, &ny) == 2)
                    return AppOptions::Colours{nx, ny};
                nx = unsigned(std::stoul(s));
                return AppOptions::Colours{nx, nx};
            });
        } else if (a == "--fill") {
            o.fill = std::stof(next());
        } else if (a == "--seed") {
            o.seed = uint32_t(std::stoul(next()));
        } else if (a == "--tps") {
            o.tps = std::stod(next());
        } else if (a == "--present") {
            o.present = next();
        } else if (a == "--fullscreen") {
            o.fullscreen = true;
        } else if (a == "--bench") {
            o.bench = true;
        } else if (a == "--compare") {
            o.compare = true;
        } else if (a == "--pass-order") {
            o.passOrder = std::stoi(next());
        } else if (a == "--no-air-coupling") {
            o.noAirCoupling = true;
        } else if (a == "--serial") {
            o.serial = true;
        } else if (a == "--tpt-air") {
            o.tptAirMode = std::stoi(next());
        } else if (a == "--interval") {
            o.interval = std::stoi(next());
        } else if (a == "--warmup") {
            o.warmup = std::stoi(next());
        } else if (a == "--ticks") {
            o.ticks = std::stoi(next());
        } else if (a == "--batch") {
            o.batch = std::stoi(next());
        } else if (a == "--snapshot") {
            o.snapshot = next();
        } else if (a == "--validate") {
            o.validate = true;
        } else if (a == "-h" || a == "--help") {
            usage();
            return 0;
        } else {
            std::fprintf(stderr, "unknown option %s\n", a.c_str());
            usage();
            return 2;
        }
    }

    std::error_code ec;
    auto exe = std::filesystem::canonical("/proc/self/exe", ec);
    o.shaderDir = (ec ? std::filesystem::path(".") : exe.parent_path()) / "shaders";

    try {
        return o.compare ? runCompare(o) : o.bench ? runBench(o) : runViewer(o);
    } catch (const std::exception &e) {
        std::fprintf(stderr, "error: %s\n", e.what());
        return 1;
    }
}
