#include "app/App.h"
#include "classic/ClassicSim.h"
#include "sim/Elements.h"
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
        "  --mode MODE             viewer: classic (TPT's own engine), screen (GPU, 4K), huge [screen]\n"
        "  --load FILE             viewer: open a TPT save (.cps/.stm)\n"
        "  --export-tpt FILE       build --scene with TPT's engine (612x384) and write it as a TPT save\n"
        "  --export-ticks N        ...after running it N ticks                       [0]\n"
        "  --check-save FILE       paste a TPT save into a GPU world, check it against TPT's, run both\n"
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
        "  --edge N --gravity N    edge mode 0 void/1 solid/2 loop; gravity 0 vertical/1 off/2 radial\n"
        "  --aheat                 ambient heat on\n"
        "  --tpt-air N             compare: TPT air mode (0 on ... 4 no update)       [0]\n"
        "  --stickman-keys C,P,C2,P2  compare: keys held for STKM and STKM2 (comm, pcomm bits: 1 left,\n"
        "                          2 right, 4 jump, 8 use element)                  [0,0,0,0]\n"
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
        } else if (a == "--mode") {
            o.mode = next();
            if (o.mode != "classic" && o.mode != "screen" && o.mode != "huge") {
                std::fprintf(stderr, "--mode must be classic, screen or huge\n");
                return 2;
            }
        } else if (a == "--export-tpt") {
            o.exportPath = next();
        } else if (a == "--export-ticks") {
            o.exportTicks = std::stoi(next());
        } else if (a == "--check-save") {
            o.compare = true;
            o.checkSave = next();
        } else if (a == "--load") {
            o.loadPath = next();
        } else if (a == "--size") {
            o.sizeSet = true;
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
        } else if (a == "--stickman-keys") {
            o.stickmanKeys = splitList<int>(next(), [](const std::string &v) { return std::stoi(v); });
            o.stickmanKeys.resize(4, 0);
        } else if (a == "--edge") {
            o.edgeMode = std::stoi(next());
        } else if (a == "--gravity") {
            o.gravityMode = std::stoi(next());
        } else if (a == "--air-test") {
            o.compare = true;
            o.airTest = true;
        } else if (a == "--aheat") {
            o.ambientHeat = true;
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
        if (!o.exportPath.empty()) {
            ElementTable table = ElementTable::build();
            SceneData sd = makeScene(table, o.scenes.empty() ? "mixed" : o.scenes[0], ClassicSim::W, ClassicSim::H,
                                     o.seed, o.fill);
            ClassicSim classic;
            classic.seed(o.seed);
            if (!sd.walls.empty())
                classic.setWalls(sd.walls);
            for (size_t i = 0; i < sd.ptype.size(); i++) {
                const Particle &sp = sd.parts[i], &dp = table.defaults[sd.ptype[i]];
                int id = classic.createPart(int(sp.x), int(sp.y), int(sd.ptype[i]));
                if (id < 0 && sd.stacked)
                    id = classic.createPartStacked(int(sp.x), int(sp.y), int(sd.ptype[i]));
                if (sp.temp != dp.temp)
                    classic.setParticleTemp(id, sp.temp);
                if (sp.ctype != dp.ctype || sp.tmp != dp.tmp || sp.tmp2 != dp.tmp2 || sp.tmp3 != dp.tmp3 ||
                    sp.tmp4 != dp.tmp4)
                    classic.setParticleFields(id, sp.ctype, sd.ptype[i] == PT_FIGH ? classic.particleTmp(id) : sp.tmp,
                                              sp.tmp2, sp.tmp3, sp.tmp4);
            }
            for (int t = 0; t < o.exportTicks; t++)
                classic.step();
            std::string err;
            if (!classic.saveFile(o.exportPath, err)) {
                std::fprintf(stderr, "error: %s\n", err.c_str());
                return 1;
            }
            std::printf("wrote %s (%d particles)\n", o.exportPath.c_str(), classic.particleCount());
            return 0;
        }
        return o.compare ? runCompare(o) : o.bench ? runBench(o) : runViewer(o);
    } catch (const std::exception &e) {
        std::fprintf(stderr, "error: %s\n", e.what());
        return 1;
    }
}
