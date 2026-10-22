// Interactive viewer: SDL3 window, Vulkan swapchain, camera, brush and a fixed-rate simulation clock.
#include "sim/WorldFile.h"
#include "app/App.h"
#include "app/World.h"
#include "sim/GpuSim.h"

#include <SDL3/SDL.h>
#include <SDL3/SDL_vulkan.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <filesystem>

namespace bp {

namespace {

constexpr int FRAMES_IN_FLIGHT = 2;

struct Swapchain {
    VkContext &ctx;
    VkSwapchainKHR swapchain = VK_NULL_HANDLE;
    VkFormat format = VK_FORMAT_UNDEFINED;
    VkExtent2D extent{};
    std::vector<VkImage> images;
    std::vector<VkSemaphore> renderDone; // one per image
    VkPresentModeKHR presentMode = VK_PRESENT_MODE_FIFO_KHR;

    Swapchain(VkContext &c, VkSurfaceKHR surface, VkPresentModeKHR wanted, uint32_t w, uint32_t h) : ctx(c) {
        create(surface, wanted, w, h);
    }
    ~Swapchain() { destroy(); }

    void create(VkSurfaceKHR surface, VkPresentModeKHR wanted, uint32_t w, uint32_t h) {
        VkPhysicalDevice pd = ctx.physicalDevice();
        VkSurfaceCapabilitiesKHR caps;
        VK_CHECK(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(pd, surface, &caps));
        if (!(caps.supportedUsageFlags & VK_IMAGE_USAGE_TRANSFER_DST_BIT))
            throw std::runtime_error("swapchain images can't be copied to on this system");

        uint32_t n = 0;
        vkGetPhysicalDeviceSurfaceFormatsKHR(pd, surface, &n, nullptr);
        std::vector<VkSurfaceFormatKHR> formats(n);
        vkGetPhysicalDeviceSurfaceFormatsKHR(pd, surface, &n, formats.data());
        VkSurfaceFormatKHR chosen{VK_FORMAT_UNDEFINED, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR};
        for (auto f : formats)
            if ((f.format == VK_FORMAT_B8G8R8A8_UNORM || f.format == VK_FORMAT_B8G8R8A8_SRGB) &&
                f.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) {
                chosen = f;
                if (f.format == VK_FORMAT_B8G8R8A8_UNORM)
                    break;
            }
        if (chosen.format == VK_FORMAT_UNDEFINED)
            throw std::runtime_error("no BGRA8 swapchain format");
        format = chosen.format;

        vkGetPhysicalDeviceSurfacePresentModesKHR(pd, surface, &n, nullptr);
        std::vector<VkPresentModeKHR> modes(n);
        vkGetPhysicalDeviceSurfacePresentModesKHR(pd, surface, &n, modes.data());
        presentMode = std::find(modes.begin(), modes.end(), wanted) != modes.end() ? wanted : VK_PRESENT_MODE_FIFO_KHR;

        extent = caps.currentExtent;
        if (extent.width == UINT32_MAX) {
            extent.width = std::clamp(w, caps.minImageExtent.width, caps.maxImageExtent.width);
            extent.height = std::clamp(h, caps.minImageExtent.height, caps.maxImageExtent.height);
        }
        uint32_t count = std::max(caps.minImageCount + 1, 3u);
        if (caps.maxImageCount)
            count = std::min(count, caps.maxImageCount);

        VkSwapchainCreateInfoKHR ci{VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
        ci.surface = surface;
        ci.minImageCount = count;
        ci.imageFormat = chosen.format;
        ci.imageColorSpace = chosen.colorSpace;
        ci.imageExtent = extent;
        ci.imageArrayLayers = 1;
        ci.imageUsage = VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        ci.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
        ci.preTransform = caps.currentTransform;
        ci.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
        ci.presentMode = presentMode;
        ci.clipped = VK_TRUE;
        ci.oldSwapchain = swapchain;
        VkSwapchainKHR newSwapchain;
        VK_CHECK(vkCreateSwapchainKHR(ctx.device(), &ci, nullptr, &newSwapchain));
        destroy();
        swapchain = newSwapchain;

        vkGetSwapchainImagesKHR(ctx.device(), swapchain, &n, nullptr);
        images.resize(n);
        vkGetSwapchainImagesKHR(ctx.device(), swapchain, &n, images.data());
        renderDone.resize(n);
        VkSemaphoreCreateInfo sci{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        for (auto &s : renderDone)
            VK_CHECK(vkCreateSemaphore(ctx.device(), &sci, nullptr, &s));
    }

    void destroy() {
        for (auto s : renderDone)
            vkDestroySemaphore(ctx.device(), s, nullptr);
        renderDone.clear();
        if (swapchain)
            vkDestroySwapchainKHR(ctx.device(), swapchain, nullptr);
        swapchain = VK_NULL_HANDLE;
    }
};

void imageBarrier(VkCommandBuffer cmd, VkImage image, VkImageLayout from, VkImageLayout to, VkAccessFlags srcAccess,
                  VkAccessFlags dstAccess, VkPipelineStageFlags srcStage, VkPipelineStageFlags dstStage) {
    VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    b.srcAccessMask = srcAccess;
    b.dstAccessMask = dstAccess;
    b.oldLayout = from;
    b.newLayout = to;
    b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = image;
    b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCmdPipelineBarrier(cmd, srcStage, dstStage, 0, 0, nullptr, 0, nullptr, 1, &b);
}

struct Camera {
    float x = 0, y = 0, zoom = 1; // world position of the top-left screen pixel; screen px per cell

    void fit(uint32_t sw, uint32_t sh, uint32_t W, uint32_t H) {
        zoom = std::min(float(sw) / float(W), float(sh) / float(H));
        x = (float(W) - float(sw) / zoom) * 0.5f;
        y = (float(H) - float(sh) / zoom) * 0.5f;
    }
    void zoomAt(float sx, float sy, float factor) {
        float wx = x + sx / zoom, wy = y + sy / zoom;
        zoom = std::clamp(zoom * factor, 1.0f / 16.0f, 256.0f);
        x = wx - sx / zoom;
        y = wy - sy / zoom;
    }
};

struct ElementKey {
    SDL_Keycode key;
    int type;
};
// TPT's wall names, by wall id (SimulationData.cpp LoadWalls).
const char *const WALL_NAMES[] = {"erase walls", "CONDUCTIVE WALL", "EWALL", "DETECTOR", "STREAMLINE", "FAN",
                                  "LIQUID WALL", "ABSORB WALL", "WALL", "AIRONLY WALL", "POWDER WALL", "CONDUCTOR",
                                  "EHOLE", "GAS WALL", "GRAVITY WALL", "ENERGY WALL", "AIRBLOCK WALL", "ERASEALL",
                                  "STASIS WALL"};

const ElementKey ELEMENT_KEYS[] = {{SDLK_1, PT_DUST}, {SDLK_2, PT_WATR}, {SDLK_3, PT_OIL}, {SDLK_4, PT_FIRE},
                                   {SDLK_5, PT_STNE}, {SDLK_6, PT_METL}, {SDLK_0, PT_NONE}};

VkPresentModeKHR parsePresentMode(const std::string &s) {
    if (s == "mailbox")
        return VK_PRESENT_MODE_MAILBOX_KHR;
    if (s == "immediate")
        return VK_PRESENT_MODE_IMMEDIATE_KHR;
    return VK_PRESENT_MODE_FIFO_KHR;
}

const char *presentModeName(VkPresentModeKHR m) {
    switch (m) {
    case VK_PRESENT_MODE_MAILBOX_KHR: return "mailbox";
    case VK_PRESENT_MODE_IMMEDIATE_KHR: return "immediate";
    default: return "vsync";
    }
}

std::string savesDir() {
    const char *home = std::getenv("HOME");
    std::filesystem::path dir = std::filesystem::path(home ? home : ".") / ".local/share/big-powder/saves";
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

} // namespace

int runViewer(const AppOptions &o) {
    if (!SDL_Init(SDL_INIT_VIDEO))
        throw std::runtime_error(std::string("SDL_Init: ") + SDL_GetError());
    SDL_Window *window = SDL_CreateWindow("Big Powder", 1600, 900,
                                          SDL_WINDOW_VULKAN | SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY);
    if (!window)
        throw std::runtime_error(std::string("SDL_CreateWindow: ") + SDL_GetError());
    if (o.fullscreen)
        SDL_SetWindowFullscreen(window, true);

    std::puts("Controls: left drag = draw, right/middle drag = pan, wheel = zoom, shift+wheel or [ ] = brush size\n"
              "  1 DUST  2 WATR  3 OIL  4 FIRE  5 STNE  6 METL  0 erase, PageUp/PageDown = every element\n"
              "  space pause, . step, V display mode, C colour passes (GPU), Tab next scene, R reload,\n"
              "  M switch Classic (TPT's engine) / Screen (GPU), Ctrl+S save (Classic), drop a .cps/.stm to load,\n"
              "  L wall brush (PageUp/PageDown wall type, 0 erase walls), E edge mode, W gravity, Y air, U ambient heat\n"
              "  - / = sim speed, Home fit view, F11 fullscreen, Esc quit");

    {
        Uint32 extCount = 0;
        const char *const *exts = SDL_Vulkan_GetInstanceExtensions(&extCount);
        VkContext ctx(std::vector<const char *>(exts, exts + extCount), o.validate);
        VkSurfaceKHR surface;
        if (!SDL_Vulkan_CreateSurface(window, ctx.instance(), nullptr, &surface))
            throw std::runtime_error(std::string("SDL_Vulkan_CreateSurface: ") + SDL_GetError());
        ctx.setSurface(surface);
        ctx.createDevice();

        std::string mode = o.mode;
        // A world file opens as a GPU world of its own size
        AppOptions startOptions = o;
        if (o.loadPath.size() > 4 && o.loadPath.compare(o.loadPath.size() - 4, 4, ".bpw") == 0) {
            WorldFileInfo info;
            std::string err;
            if (readWorldInfo(o.loadPath, info, err)) {
                startOptions.W = info.W;
                startOptions.H = info.H;
                startOptions.sizeSet = true;
                if (mode == "classic")
                    mode = uint64_t(info.W) * info.H > uint64_t(3840) * 2160 ? "huge" : "screen";
            }
        }
        std::unique_ptr<World> world = makeWorld(ctx, startOptions, mode);
        const auto &scenes = sceneNames();
        size_t sceneIdx = 0;
        if (!o.scenes.empty()) {
            auto it = std::find(scenes.begin(), scenes.end(), o.scenes[0]);
            sceneIdx = it == scenes.end() ? 0 : size_t(it - scenes.begin());
        }
        auto loadScene = [&] { world->loadScene(scenes[sceneIdx]); };
        auto report = [&](const std::string &msg) {
            std::printf("%s\n", msg.c_str());
            std::fflush(stdout);
        };
        if (o.loadPath.empty()) {
            loadScene();
        } else {
            std::string msg;
            world->loadSave(o.loadPath, int(world->width() / 2), int(world->height() / 2), msg);
            report(msg);
        }

        int pw, ph;
        SDL_GetWindowSizeInPixels(window, &pw, &ph);
        Swapchain sc(ctx, surface, parsePresentMode(o.present), uint32_t(pw), uint32_t(ph));
        Camera cam;
        cam.fit(sc.extent.width, sc.extent.height, world->width(), world->height());

        VkDevice dev = ctx.device();
        VkCommandBufferAllocateInfo ai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        ai.commandPool = ctx.commandPool();
        ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        ai.commandBufferCount = FRAMES_IN_FLIGHT;
        VkCommandBuffer cmds[FRAMES_IN_FLIGHT];
        VK_CHECK(vkAllocateCommandBuffers(dev, &ai, cmds));
        VkFence fences[FRAMES_IN_FLIGHT];
        VkSemaphore acquired[FRAMES_IN_FLIGHT];
        VkQueryPool queries[FRAMES_IN_FLIGHT];
        bool queryWritten[FRAMES_IN_FLIGHT] = {};
        for (int f = 0; f < FRAMES_IN_FLIGHT; f++) {
            VkFenceCreateInfo fci{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
            fci.flags = VK_FENCE_CREATE_SIGNALED_BIT;
            VK_CHECK(vkCreateFence(dev, &fci, nullptr, &fences[f]));
            VkSemaphoreCreateInfo sci{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
            VK_CHECK(vkCreateSemaphore(dev, &sci, nullptr, &acquired[f]));
            VkQueryPoolCreateInfo qci{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
            qci.queryType = VK_QUERY_TYPE_TIMESTAMP;
            qci.queryCount = 4; // recordTick: start, after the lists, end, after the passes
            VK_CHECK(vkCreateQueryPool(dev, &qci, nullptr, &queries[f]));
        }
        const double tsPeriodMs = double(ctx.properties().limits.timestampPeriod) * 1e-6;

        // Input and simulation state.
        bool running = true, paused = false, stepOnce = false, needResize = false;
        bool wallBrush = false;
        bool gravityGrid = false; // TPT's ctrl+G: the gravity field display
        int wallType = WL_WALL;
        std::vector<WallOp> wallOps;
        WorldSettings settings;
        settings.edgeMode = o.edgeMode;
        settings.gravityMode = o.gravityMode;
        settings.ambientHeat = o.ambientHeat;
        settings.newtonianGravity = o.newtonian;
        world->applySettings(settings);
        double tps = o.tps;
        int brushType = PT_WATR, brushR = 8;
        bool drawing = false, panning = false;
        float lastWX = 0, lastWY = 0;
        float mouseX = 0, mouseY = 0; // in pixels
        std::vector<BrushOp> brushOps;
        // The players' keys (TPT's GameController): arrows steer STKM, WASD STKM2.
        uint8_t comm1 = 0, pcomm1 = 0, comm2 = 0, pcomm2 = 0;
        double accumulator = 0;
        uint64_t lastNs = SDL_GetTicksNS();
        uint64_t hudNs = lastNs, statsNs = 0, printNs = lastNs;
        int hudFrames = 0, hudTicks = 0;
        double gpuTickMs = 0;
        uint64_t particleCount = 0;
        int frame = 0;

        auto pixelScale = [&] { return SDL_GetWindowPixelDensity(window); };
        auto toWorld = [&](float sx, float sy, float &wx, float &wy) {
            wx = cam.x + sx / cam.zoom;
            wy = cam.y + sy / cam.zoom;
        };
        auto addStroke = [&](float x0, float y0, float x1, float y1) {
            float dx = x1 - x0, dy = y1 - y0;
            float len = std::sqrt(dx * dx + dy * dy);
            float step = std::max(1.0f, float(brushR) * 0.5f);
            int n = std::max(1, int(len / step));
            for (int k = 0; k <= n && brushOps.size() < 512 && wallOps.size() < 512; k++) {
                float t = float(k) / float(n);
                int px = int(std::floor(x0 + dx * t)), py = int(std::floor(y0 + dy * t));
                if (wallBrush) {
                    WallOp w{px / CELL, py / CELL, brushR / CELL, uint8_t(wallType)};
                    if (wallOps.empty() || wallOps.back().cx != w.cx || wallOps.back().cy != w.cy)
                        wallOps.push_back(w);
                } else {
                    brushOps.push_back({px, py, brushR, brushType});
                }
            }
        };
        auto cycleWall = [&](int dir) {
            do {
                wallType = (wallType + dir + 19) % 19;
            } while (wallType == WL_ERASEALL); // a tool in TPT, not a wall
        };
        auto cycleElement = [&](int dir) {
            std::vector<int> els = world->elements();
            if (els.empty())
                return;
            auto it = std::find(els.begin(), els.end(), brushType);
            int i = it == els.end() ? 0 : int(it - els.begin());
            i = (i + dir + int(els.size())) % int(els.size());
            brushType = els[size_t(i)];
        };
        auto switchMode = [&](const std::string &newMode) {
            vkDeviceWaitIdle(dev);
            for (bool &q : queryWritten)
                q = false;
            world.reset(); // only one TPT engine may exist at a time
            mode = newMode;
            world = makeWorld(ctx, o, mode);
            world->applySettings(settings);
            loadScene();
            cam.fit(sc.extent.width, sc.extent.height, world->width(), world->height());
            statsNs = 0;
        };

        while (running) {
            SDL_Event ev;
            while (SDL_PollEvent(&ev)) {
                switch (ev.type) {
                case SDL_EVENT_QUIT:
                    running = false;
                    break;
                case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED:
                    needResize = true;
                    break;
                case SDL_EVENT_DROP_FILE: {
                    float s = pixelScale();
                    float wx, wy;
                    toWorld(ev.drop.x * s, ev.drop.y * s, wx, wy);
                    vkDeviceWaitIdle(dev);
                    std::string msg;
                    world->loadSave(ev.drop.data, int(wx), int(wy), msg);
                    report(msg);
                    break;
                }
                case SDL_EVENT_KEY_UP: {
                    if (ev.key.repeat)
                        break;
                    SDL_Keycode k = ev.key.key;
                    SDL_Scancode sc2 = ev.key.scancode;
                    if (k == SDLK_RIGHT || k == SDLK_LEFT) {
                        pcomm1 = comm1; // the last direction
                        comm1 &= 12;
                    } else if (k == SDLK_UP)
                        comm1 &= 11;
                    else if (k == SDLK_DOWN)
                        comm1 &= 7;
                    if (sc2 == SDL_SCANCODE_D || sc2 == SDL_SCANCODE_A) {
                        pcomm2 = comm2;
                        comm2 &= 12;
                    } else if (sc2 == SDL_SCANCODE_W)
                        comm2 &= 11;
                    else if (sc2 == SDL_SCANCODE_S)
                        comm2 &= 7;
                    break;
                }
                case SDL_EVENT_KEY_DOWN: {
                    SDL_Keycode k = ev.key.key;
                    bool ctrl = (SDL_GetModState() & SDL_KMOD_CTRL) != 0;
                    if (!ev.key.repeat) {
                        if (k == SDLK_RIGHT)
                            comm1 |= 2;
                        else if (k == SDLK_LEFT)
                            comm1 |= 1;
                        else if (k == SDLK_DOWN)
                            comm1 |= 8;
                        else if (k == SDLK_UP)
                            comm1 |= 4;
                        SDL_Scancode sc2 = ev.key.scancode;
                        if (sc2 == SDL_SCANCODE_D)
                            comm2 |= 2;
                        else if (sc2 == SDL_SCANCODE_A)
                            comm2 |= 1;
                        else if (sc2 == SDL_SCANCODE_S)
                            comm2 |= 8;
                        else if (sc2 == SDL_SCANCODE_W)
                            comm2 |= 4;
                    }
                    // While STKM2 is out, W only steers it (with ctrl it still switches gravity), as in TPT.
                    bool wFree = ctrl || !world->stickman2Out();
                    for (const auto &ek : ELEMENT_KEYS)
                        if (k == ek.key) {
                            if (wallBrush && ek.type == PT_NONE)
                                wallType = WL_ERASE;
                            else {
                                brushType = ek.type;
                                wallBrush = false;
                            }
                        }
                    if (k == SDLK_ESCAPE)
                        running = false;
                    else if (k == SDLK_SPACE)
                        paused = !paused;
                    else if (k == SDLK_PERIOD)
                        stepOnce = true;
                    else if (k == SDLK_V || k == SDLK_H)
                        world->cycleDisplay();
                    else if (k == SDLK_G && ctrl)
                        gravityGrid = !gravityGrid;
                    else if (k == SDLK_C)
                        world->cyclePasses();
                    else if (k == SDLK_M)
                        switchMode(mode == "classic" ? (o.mode == "huge" ? "huge" : "screen") : "classic");
                    else if (k == SDLK_S && ctrl && (SDL_GetModState() & SDL_KMOD_SHIFT)) {
                        // the TPT-sized area around the mouse, as a TPT save
                        vkDeviceWaitIdle(dev);
                        std::string msg;
                        float wx, wy;
                        toWorld(mouseX, mouseY, wx, wy);
                        world->exportTpt(savesDir() + "/" + mode + "-" + timestamp() + ".cps", int(wx), int(wy), msg);
                        report(msg);
                    } else if (k == SDLK_S && ctrl) {
                        vkDeviceWaitIdle(dev);
                        std::string msg;
                        world->save(savesDir() + "/" + mode + "-" + timestamp() + world->saveExtension(), msg);
                        report(msg);
                    } else if (k == SDLK_PAGEUP)
                        wallBrush ? cycleWall(-1) : cycleElement(-1);
                    else if (k == SDLK_PAGEDOWN)
                        wallBrush ? cycleWall(+1) : cycleElement(+1);
                    else if (k == SDLK_L)
                        wallBrush = !wallBrush;
                    else if (k == SDLK_E || (k == SDLK_W && wFree) || k == SDLK_Y || k == SDLK_U || k == SDLK_N) {
                        if (k == SDLK_E)
                            settings.edgeMode = (settings.edgeMode + 1) % 3;
                        else if (k == SDLK_W)
                            settings.gravityMode = (settings.gravityMode + 1) % 3;
                        else if (k == SDLK_Y)
                            settings.airMode = (settings.airMode + 1) % 5;
                        else if (k == SDLK_U)
                            settings.ambientHeat = !settings.ambientHeat;
                        else // TPT's N: Newtonian gravity
                            settings.newtonianGravity = !settings.newtonianGravity;
                        vkDeviceWaitIdle(dev);
                        world->applySettings(settings);
                    }
                    else if (k == SDLK_TAB) {
                        sceneIdx = (sceneIdx + 1) % scenes.size();
                        vkDeviceWaitIdle(dev);
                        loadScene();
                    } else if (k == SDLK_R) {
                        vkDeviceWaitIdle(dev);
                        loadScene();
                    } else if (k == SDLK_LEFTBRACKET)
                        brushR = std::max(0, brushR - std::max(1, brushR / 5));
                    else if (k == SDLK_RIGHTBRACKET)
                        brushR = std::min(400, brushR + std::max(1, brushR / 5));
                    else if (k == SDLK_MINUS)
                        tps = std::max(1.875, tps / 2);
                    else if (k == SDLK_EQUALS)
                        tps = std::min(960.0, tps * 2);
                    else if (k == SDLK_HOME)
                        cam.fit(sc.extent.width, sc.extent.height, world->width(), world->height());
                    else if (k == SDLK_F11)
                        SDL_SetWindowFullscreen(window, !(SDL_GetWindowFlags(window) & SDL_WINDOW_FULLSCREEN));
                    break;
                }
                case SDL_EVENT_MOUSE_WHEEL: {
                    float s = pixelScale();
                    if (SDL_GetModState() & SDL_KMOD_SHIFT) {
                        int d = ev.wheel.y > 0 ? 1 : -1;
                        brushR = std::clamp(brushR + d * std::max(1, brushR / 5), 0, 400);
                    } else {
                        cam.zoomAt(ev.wheel.mouse_x * s, ev.wheel.mouse_y * s, std::pow(1.25f, ev.wheel.y));
                    }
                    break;
                }
                case SDL_EVENT_MOUSE_BUTTON_DOWN: {
                    float s = pixelScale();
                    mouseX = ev.button.x * s;
                    mouseY = ev.button.y * s;
                    if (ev.button.button == SDL_BUTTON_LEFT) {
                        toWorld(mouseX, mouseY, lastWX, lastWY);
                        if (!wallBrush && (brushType == PT_STKM || brushType == PT_STKM2 || brushType == PT_FIGH)) {
                            // Stick men are placed one per click (TPT's PlopTool)
                            brushOps.push_back({int(std::floor(lastWX)), int(std::floor(lastWY)), 0, brushType});
                            break;
                        }
                        drawing = true;
                        addStroke(lastWX, lastWY, lastWX, lastWY);
                    } else {
                        panning = true;
                    }
                    break;
                }
                case SDL_EVENT_MOUSE_BUTTON_UP:
                    if (ev.button.button == SDL_BUTTON_LEFT)
                        drawing = false;
                    else
                        panning = false;
                    break;
                case SDL_EVENT_MOUSE_MOTION: {
                    float s = pixelScale();
                    mouseX = ev.motion.x * s;
                    mouseY = ev.motion.y * s;
                    if (panning) {
                        cam.x -= ev.motion.xrel * s / cam.zoom;
                        cam.y -= ev.motion.yrel * s / cam.zoom;
                    }
                    if (drawing) {
                        float wx, wy;
                        toWorld(mouseX, mouseY, wx, wy);
                        addStroke(lastWX, lastWY, wx, wy);
                        lastWX = wx;
                        lastWY = wy;
                    }
                    break;
                }
                default:
                    break;
                }
            }
            if (!running)
                break;
            // Holding the button still keeps drawing, like TPT.
            if (drawing && brushOps.empty() && !wallBrush) {
                float wx, wy;
                toWorld(mouseX, mouseY, wx, wy);
                addStroke(wx, wy, wx, wy);
            }

            if (needResize) {
                vkDeviceWaitIdle(dev);
                SDL_GetWindowSizeInPixels(window, &pw, &ph);
                if (pw > 0 && ph > 0)
                    sc.create(surface, parsePresentMode(o.present), uint32_t(pw), uint32_t(ph));
                needResize = false;
            }
            if (sc.extent.width == 0 || sc.extent.height == 0) {
                SDL_Delay(10);
                continue;
            }

            // Fixed-rate simulation clock. If the engine can't keep up the sim slows down instead of skipping.
            uint64_t nowNs = SDL_GetTicksNS();
            double dt = double(nowNs - lastNs) * 1e-9;
            lastNs = nowNs;
            int ticks = 0;
            if (!paused) {
                accumulator += dt;
                double step = 1.0 / tps;
                while (accumulator >= step && ticks < 8) {
                    accumulator -= step;
                    ticks++;
                }
                if (ticks == 8)
                    accumulator = 0;
            } else if (stepOnce) {
                ticks = 1;
            }
            stepOnce = false;

            int f = frame % FRAMES_IN_FLIGHT;
            VK_CHECK(vkWaitForFences(dev, 1, &fences[f], VK_TRUE, UINT64_MAX));
            if (queryWritten[f]) {
                uint64_t ts[3];
                if (vkGetQueryPoolResults(dev, queries[f], 0, 3, sizeof(ts), ts, 8, VK_QUERY_RESULT_64_BIT) ==
                    VK_SUCCESS)
                    gpuTickMs = gpuTickMs * 0.9 + 0.1 * double(ts[2] - ts[0]) * tsPeriodMs;
                queryWritten[f] = false;
            }

            uint32_t imageIndex;
            VkResult acq = vkAcquireNextImageKHR(dev, sc.swapchain, UINT64_MAX, acquired[f], VK_NULL_HANDLE,
                                                 &imageIndex);
            if (acq == VK_ERROR_OUT_OF_DATE_KHR) {
                needResize = true;
                continue;
            }
            if (acq != VK_SUCCESS && acq != VK_SUBOPTIMAL_KHR)
                throw std::runtime_error("vkAcquireNextImageKHR failed");
            VK_CHECK(vkResetFences(dev, 1, &fences[f]));

            const uint32_t sw = sc.extent.width, sh = sc.extent.height;
            world->ensureFrameBuffer(sw, sh);

            VkCommandBuffer cmd = cmds[f];
            VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
            bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
            VK_CHECK(vkBeginCommandBuffer(cmd, &bi));

            world->setStickmanKeys(comm1, pcomm1, comm2, pcomm2);
            FrameRequest req;
            req.ticks = ticks;
            req.brush = std::move(brushOps);
            brushOps.clear();
            req.walls = std::move(wallOps);
            wallOps.clear();
            req.camX = cam.x;
            req.camY = cam.y;
            req.zoom = cam.zoom;
            req.screenW = sw;
            req.screenH = sh;
            float bwx, bwy;
            toWorld(mouseX, mouseY, bwx, bwy);
            req.brushX = int(std::floor(bwx));
            req.brushY = int(std::floor(bwy));
            req.brushR = brushR;
            // TPT shows the gravity zones while the GRAV wall tool is selected
            req.overlays = (gravityGrid ? 1u : 0u) | (wallBrush && wallType == WL_GRAV ? 2u : 0u);
            req.slot = f;
            if (ticks > 0 && world->gpuTimed()) {
                vkCmdResetQueryPool(cmd, queries[f], 0, 4);
                queryWritten[f] = true;
                req.queries = queries[f];
            }
            world->record(cmd, req);
            hudTicks += ticks;

            VkBufferMemoryBarrier bb{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};
            bb.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
            bb.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
            bb.srcQueueFamilyIndex = bb.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            bb.buffer = world->frameBuffer().buffer;
            bb.size = VK_WHOLE_SIZE;
            vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0,
                                 nullptr, 1, &bb, 0, nullptr);
            VkImage img = sc.images[imageIndex];
            imageBarrier(cmd, img, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0,
                         VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
            VkBufferImageCopy region{};
            region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
            region.imageExtent = {sw, sh, 1};
            vkCmdCopyBufferToImage(cmd, world->frameBuffer().buffer, img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1,
                                   &region);
            imageBarrier(cmd, img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
                         VK_ACCESS_TRANSFER_WRITE_BIT, 0, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT);
            VK_CHECK(vkEndCommandBuffer(cmd));

            VkPipelineStageFlags waitStage = VK_PIPELINE_STAGE_TRANSFER_BIT;
            VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
            si.waitSemaphoreCount = 1;
            si.pWaitSemaphores = &acquired[f];
            si.pWaitDstStageMask = &waitStage;
            si.commandBufferCount = 1;
            si.pCommandBuffers = &cmd;
            si.signalSemaphoreCount = 1;
            si.pSignalSemaphores = &sc.renderDone[imageIndex];
            VK_CHECK(vkQueueSubmit(ctx.queue(), 1, &si, fences[f]));

            VkPresentInfoKHR pi{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
            pi.waitSemaphoreCount = 1;
            pi.pWaitSemaphores = &sc.renderDone[imageIndex];
            pi.swapchainCount = 1;
            pi.pSwapchains = &sc.swapchain;
            pi.pImageIndices = &imageIndex;
            VkResult pres = vkQueuePresentKHR(ctx.queue(), &pi);
            if (pres == VK_ERROR_OUT_OF_DATE_KHR || pres == VK_SUBOPTIMAL_KHR)
                needResize = true;
            else if (pres != VK_SUCCESS)
                throw std::runtime_error("vkQueuePresentKHR failed");
            frame++;
            hudFrames++;

            // Window title as a simple HUD.
            if (nowNs - statsNs > 2'000'000'000ull) {
                particleCount = world->particleCount();
                statsNs = SDL_GetTicksNS();
            }
            if (nowNs - hudNs > 500'000'000ull) {
                double secs = double(nowNs - hudNs) * 1e-9;
                double tickMs = world->gpuTimed() ? gpuTickMs : world->cpuTickMs();
                std::string passes = world->passesName();
                char title[512];
                static const char *edgeNames[] = {"void", "solid", "loop"};
                static const char *gravNames[] = {"vertical", "off", "radial"};
                static const char *airNames[] = {"air on", "pressure off", "velocity off", "air off", "air frozen"};
                std::string tool = wallBrush ? std::string(WALL_NAMES[wallType]) : world->elementName(brushType);
                std::snprintf(title, sizeof title,
                              "Big Powder %s %ux%u | %.0f fps (%s) | %.1f tps of %.0f | %s %.2f ms/tick | %.2fM "
                              "particles | %s r=%d | %s | edge %s, gravity %s, %s%s%s%s | %s%s",
                              mode.c_str(), world->width(), world->height(), hudFrames / secs,
                              presentModeName(sc.presentMode), hudTicks / secs, tps,
                              world->gpuTimed() ? "GPU" : "CPU", tickMs, double(particleCount) / 1e6, tool.c_str(),
                              brushR, world->displayName().c_str(), edgeNames[settings.edgeMode],
                              gravNames[settings.gravityMode], airNames[settings.airMode],
                              settings.ambientHeat ? ", ambient heat" : "", passes.empty() ? "" : " | ",
                              passes.c_str(), scenes[sceneIdx].c_str(), paused ? " | PAUSED" : "");
                SDL_SetWindowTitle(window, title);
                if (nowNs - printNs > 2'000'000'000ull) {
                    std::printf("%s | %ux%u px\n", title, sw, sh);
                    std::fflush(stdout);
                    printNs = nowNs;
                }
                hudNs = nowNs;
                hudFrames = hudTicks = 0;
            }
        }

        vkDeviceWaitIdle(dev);
        world.reset();
        for (int f = 0; f < FRAMES_IN_FLIGHT; f++) {
            vkDestroyFence(dev, fences[f], nullptr);
            vkDestroySemaphore(dev, acquired[f], nullptr);
            vkDestroyQueryPool(dev, queries[f], nullptr);
        }
    }
    SDL_DestroyWindow(window);
    SDL_Quit();
    return 0;
}

} // namespace bp
