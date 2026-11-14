#include "app/App.h"
#include "app/GameScreen.h"
#include "app/Updater.h"
#include "app/appreports.h"
#include "classic/ClassicSim.h"
#include "sim/Elements.h"
#include "sim/Scenes.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <filesystem>
#include <optional>
#include <sstream>
#include <thread>

#include <unistd.h>

using namespace bp;

static void usage() {
    std::puts(
        "usage: big-powder [options]\n"
        "  --version               print the version\n"
        "  --mode MODE             viewer: classic (TPT's own engine), screen (GPU, 4K), huge [screen]\n"
        "  --load FILE             viewer: open a TPT save (.cps/.stm)\n"
        "  --export-tpt FILE       build --scene with TPT's engine (612x384) and write it as a TPT save\n"
        "  --export-ticks N        ...after running it N ticks                       [0]\n"
        "  --check-save FILE       paste a TPT save into a GPU world, check it against TPT's, run both\n"
        "  --render-check          run TPT --ticks, then draw that state with both renderers and compare\n"
        "  --check-tools           (with --compare) use TPT's tools the same way in TPT and on the GPU, compare\n"
        "  --presets N[,N...] --frames N   render check: display presets (all) and frames drawn  [3]\n"
        "  --check-screen          (with --compare) drive the game screen with made-up keys and clicks (its\n"
        "                          windows and tools) in Classic mode and on the GPU; --ui-snapshot P for pictures\n"
        "  --check-undo            (with --compare) time undo's snapshots of --scene and check they restore exactly\n"
        "  --check-clipboard       (with --compare) copy, transform, paste and cut in TPT and on the GPU, compared\n"
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
        "  --newtonian             Newtonian gravity on\n"
        "  --ui-scale N            viewer: size of the interface's pixels (0: from the window)  [0]\n"
        "  --overlays N            render check: gravity field (1) and gravity zones (2) displays  [0]\n"
        "  --grid N                render check: the grid (G), N cells apart                [0]\n"
        "  --find NAME             render check: finding an element (ctrl+F)\n"
        "  --tpt-air N             compare: TPT air mode (0 on ... 4 no update)       [0]\n"
        "  --stickman-keys C,P,C2,P2  compare: keys held for STKM and STKM2 (comm, pcomm bits: 1 left,\n"
        "                          2 right, 4 jump, 8 use element)                  [0,0,0,0]\n"
        "  --snapshot PREFIX       bench: save PREFIX-<scene>-<N>.ppm (whole world) and -zoom.ppm\n"
        "  --validate              enable Vulkan validation layers\n"
        "  --check-update [--as V] look for a newer release (as version V)\n"
        "  --install-update DIR    ...and install it into DIR (a copy of a release)\n"
        "  --send-test-report NOTE   send a problem report now (release builds)\n"
        "  --crash-test            crash (SIGSEGV) with the crash reporter on; --flush-reports then sends it\n");
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

// The release checks: --check-update, --install-update, --send-test-report, --crash-test, --flush-reports
static int runReleaseCheck(const std::string &what, const std::string &arg, const std::string &as) {
    if (what == "--check-update" || what == "--install-update") {
        std::string error;
        std::optional<UpdateInfo> u = checkForUpdate(as, error);
        if (!u) {
            std::printf("%s\n", error.empty() ? ("no release newer than " + as).c_str() : ("error: " + error).c_str());
            return error.empty() ? 0 : 1;
        }
        std::printf("newer: %s (%s, %llu bytes, sha256 %s)\n", u->version.c_str(), u->file.c_str(),
                    (unsigned long long)u->size, u->sha256.c_str());
        if (what == "--check-update")
            return 0;
        if (!canUpdate(arg)) {
            std::printf("error: %s isn't a release install that can be written\n", arg.c_str());
            return 1;
        }
        UpdateProgress p;
        installUpdate(*u, arg, p);
        std::printf("%s\n", p.ok ? "installed" : ("error: " + p.error).c_str());
        return p.ok ? 0 : 1;
    }
    appreports::init("io.github.ssjrocks.BigPowder", "Big Powder", BP_VERSION, dataDir());
    if (!appreports::enabled()) {
        std::printf("error: this build has no report URL (configure with -DAPP_REPORTS_URL=...)\n");
        return 1;
    }
    appreports::log("release check " + what);
    if (what == "--crash-test") {
        std::fflush(stdout);
        volatile int *nothing = nullptr;
        *nothing = 1; // (a real SIGSEGV through the reporter's handler)
        return 1;
    }
    if (what == "--send-test-report") {
        std::atomic<int> result{0};
        appreports::sendReport(arg, {{"check", "--send-test-report"}}, [&](bool ok) { result = ok ? 1 : 2; });
        for (int k = 0; k < 600 && !result; k++)
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        std::printf("%s\n", result == 1 ? "sent" : "not sent (queued for the next start)");
        return result == 1 ? 0 : 1;
    }
    appreports::flush(); // --flush-reports
    std::printf("flushed\n");
    return 0;
}

int main(int argc, char **argv) {
    std::setvbuf(stdout, nullptr, _IOLBF, 0); // (each line as it comes, also when the log is a file or the journal)
    AppOptions o;
    std::string releaseCheck, releaseArg, as = BP_VERSION;
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
        } else if (a == "--render-check") {
            o.compare = true;
            o.renderCheck = true;
        } else if (a == "--presets") {
            o.renderPresets = splitList<int>(next(), [](const std::string &v) { return std::stoi(v); });
        } else if (a == "--frames") {
            o.renderFrames = std::stoi(next());
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
        } else if (a == "--newtonian") {
            o.newtonian = true;
        } else if (a == "--ui-scale") {
            o.uiScale = std::stoi(next());
        } else if (a == "--check-tools" || a == "--check-brush") {
            o.toolCheck = true;
        } else if (a == "--check-screen") {
            o.screenCheck = true;
        } else if (a == "--check-undo") {
            o.undoCheck = true;
        } else if (a == "--check-clipboard") {
            o.clipboardCheck = true;
        } else if (a == "--ui-snapshot") {
            o.uiSnapshot = next();
        } else if (a == "--sleep-stats") {
            o.sleepStats = true;
        } else if (a == "--zoom-check") {
            o.zoomCheck = true;
        } else if (a == "--check-export") {
            o.checkExport = next();
        } else if (a == "--check-world") {
            o.checkWorld = next();
        } else if (a == "--overlays") {
            o.overlays = std::stoi(next());
        } else if (a == "--grid") {
            o.renderGrid = std::stoi(next());
        } else if (a == "--find") {
            o.renderFind = next();
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
        } else if (a == "--version") {
            std::printf("Big Powder %s\n", BP_VERSION);
            return 0;
        } else if (a == "--check-update" || a == "--crash-test" || a == "--flush-reports") {
            releaseCheck = a;
        } else if (a == "--install-update" || a == "--send-test-report") {
            releaseCheck = a;
            releaseArg = next();
        } else if (a == "--as") {
            as = next();
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
    if (!releaseCheck.empty())
        return runReleaseCheck(releaseCheck, releaseArg, as);

    try {
        if (!o.exportPath.empty()) {
            ElementTable table = ElementTable::build();
            SceneData sd = makeScene(table, o.scenes.empty() ? "mixed" : o.scenes[0], ClassicSim::W, ClassicSim::H,
                                     o.seed, o.fill);
            ClassicSim classic;
            classic.seed(o.seed);
            if (!sd.walls.empty())
                classic.setWalls(sd.walls);
            classic.setSigns(sd.signs);
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
        if (o.compare || o.bench)
            return o.compare ? runCompare(o) : runBench(o);
        int r = runViewer(o);
        if (r == VIEWER_RESTART && !ec) { // (an update was installed: the new program, as this one was started)
            execv(exe.c_str(), argv);
            std::fprintf(stderr, "error: could not restart %s\n", exe.c_str());
            return 1;
        }
        return r;
    } catch (const std::exception &e) {
        std::fprintf(stderr, "error: %s\n", e.what());
        appreports::reportError("std::exception", e.what(), "the game stopped");
        return 1;
    }
}
