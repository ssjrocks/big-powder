// Interactive viewer: SDL3 window and Vulkan swapchain around the game screen (app/GameScreen).
#include "app/App.h"
#include "app/GameScreen.h"
#include "app/Updater.h"
#include "app/appreports.h"

#include <SDL3/SDL.h>
#include <SDL3/SDL_vulkan.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <memory>
#include <thread>

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

} // namespace

int runViewer(const AppOptions &o) {
    bool restart = false;
    // Crash and problem reports (release builds; not the checks), and the update check
    appreports::init("io.github.ssjrocks.BigPowder", "Big Powder", BP_VERSION, dataDir());
    appreports::log(std::string("Big Powder ") + BP_VERSION + ", mode " + o.mode);
    const std::filesystem::path installDir = std::filesystem::path(o.shaderDir).parent_path();
    if (canUpdate(installDir))
        cleanUpUpdate(installDir);
    auto updateCheck = std::make_shared<UpdateCheck>();
    std::thread([updateCheck] {
        std::string error;
        std::optional<UpdateInfo> u = checkForUpdate(BP_VERSION, error);
        std::lock_guard g(updateCheck->m);
        updateCheck->info = u;
        updateCheck->error = error;
        updateCheck->ready = true;
    }).detach();
    if (!SDL_Init(SDL_INIT_VIDEO))
        throw std::runtime_error(std::string("SDL_Init: ") + SDL_GetError());
    SDL_Window *window = SDL_CreateWindow("Big Powder", 1600, 900,
                                          SDL_WINDOW_VULKAN | SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY);
    if (!window)
        throw std::runtime_error(std::string("SDL_CreateWindow: ") + SDL_GetError());
    if (o.fullscreen)
        SDL_SetWindowFullscreen(window, true);

    std::puts("Controls: TPT's (F1 in the game lists them). Big Powder's own: middle drag or alt+drag pans, Z+wheel\n"
              "  zooms, Home fits the view, M switches Classic (TPT's engine) / GPU, F6 next scene, . steps, Ctrl+S\n"
              "  saves, drop a .cps/.stm/.bpw to load, F11 fullscreen.");

    TptBrush::setBitmapFolder(dataDir() + "/Brushes"); // (TPT's brushes folder; the tools' brushes load it too)
    {
        Uint32 extCount = 0;
        const char *const *exts = SDL_Vulkan_GetInstanceExtensions(&extCount);
        VkContext ctx(std::vector<const char *>(exts, exts + extCount), o.validate);
        VkSurfaceKHR surface;
        if (!SDL_Vulkan_CreateSurface(window, ctx.instance(), nullptr, &surface))
            throw std::runtime_error(std::string("SDL_Vulkan_CreateSurface: ") + SDL_GetError());
        ctx.setSurface(surface);
        ctx.createDevice();
        appreports::log(std::string("GPU: ") + ctx.properties().deviceName);

        int pw, ph;
        SDL_GetWindowSizeInPixels(window, &pw, &ph);
        Swapchain sc(ctx, surface, parsePresentMode(o.present), uint32_t(pw), uint32_t(ph));
        auto screen = std::make_unique<GameScreen>(ctx, o);
        screen->resize(sc.extent.width, sc.extent.height);
        screen->setUpdateCheck(updateCheck, installDir);
        screen->setGpuName(ctx.properties().deviceName);

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

        bool needResize = false, textInput = false;
        uint64_t lastNs = SDL_GetTicksNS();
        uint64_t hudNs = lastNs, printNs = lastNs;
        int hudFrames = 0, hudTicks = 0;
        double gpuTickMs = 0;
        int frame = 0;
        uint64_t nextFrameNs = 0;

        while (screen->running()) {
            SDL_Event ev;
            while (SDL_PollEvent(&ev)) {
                if (ev.type == SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED)
                    needResize = true;
                screen->event(ev, SDL_GetWindowPixelDensity(window));
            }
            if (!screen->running())
                break;
            if (screen->takeFullscreenToggle())
                SDL_SetWindowFullscreen(window, !(SDL_GetWindowFlags(window) & SDL_WINDOW_FULLSCREEN));
            screen->setFullscreenState((SDL_GetWindowFlags(window) & SDL_WINDOW_FULLSCREEN) != 0);
            if (screen->wantsTextInput() != textInput) {
                textInput = !textInput;
                if (textInput)
                    SDL_StartTextInput(window);
                else
                    SDL_StopTextInput(window);
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
            screen->resize(sc.extent.width, sc.extent.height);

            uint64_t nowNs = SDL_GetTicksNS();
            double dt = double(nowNs - lastNs) * 1e-9;
            lastNs = nowNs;

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
            VkCommandBuffer cmd = cmds[f];
            VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
            bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
            VK_CHECK(vkBeginCommandBuffer(cmd, &bi));
            queryWritten[f] = screen->record(cmd, f, dt, queries[f]);
            hudTicks += screen->takeTicks();

            VkBufferMemoryBarrier bb{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};
            bb.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
            bb.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
            bb.srcQueueFamilyIndex = bb.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            bb.buffer = screen->frameBuffer().buffer;
            bb.size = VK_WHOLE_SIZE;
            vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0,
                                 nullptr, 1, &bb, 0, nullptr);
            VkImage img = sc.images[imageIndex];
            imageBarrier(cmd, img, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0,
                         VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
            VkBufferImageCopy region{};
            region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
            region.imageExtent = {sw, sh, 1};
            vkCmdCopyBufferToImage(cmd, screen->frameBuffer().buffer, img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1,
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
            // The settings' rendering framerate cap (0: the display's, through the present mode)
            if (int cap = screen->drawFps(); cap > 0) {
                const uint64_t period = 1'000'000'000ull / uint64_t(cap);
                const uint64_t t = SDL_GetTicksNS();
                nextFrameNs = nextFrameNs + period < t ? t : nextFrameNs + period;
                if (nextFrameNs > t)
                    SDL_DelayPrecise(nextFrameNs - t);
            } else
                nextFrameNs = 0;

            // Window title as a simple HUD.
            if (nowNs - hudNs > 500'000'000ull) {
                double secs = double(nowNs - hudNs) * 1e-9;
                screen->setFps(hudFrames / secs);
                std::string title =
                    screen->status(hudFrames / secs, hudTicks / secs, presentModeName(sc.presentMode), gpuTickMs);
                SDL_SetWindowTitle(window, title.c_str());
                if (nowNs - printNs > 2'000'000'000ull) {
                    std::printf("%s | %ux%u px\n", title.c_str(), sw, sh);
                    std::fflush(stdout);
                    printNs = nowNs;
                }
                hudNs = nowNs;
                hudFrames = hudTicks = 0;
            }
        }

        // The window goes at once. Destroying the device then waits for the GPU driver's background compile of the
        // shaders (minutes on a fresh install: NVIDIA keys its cache by program), which then lands in its cache.
        SDL_HideWindow(window);
        std::puts("closing (the GPU driver may finish compiling shaders first)");
        std::fflush(stdout);
        vkDeviceWaitIdle(dev);
        restart = screen->restartRequested();
        screen.reset();
        for (int f = 0; f < FRAMES_IN_FLIGHT; f++) {
            vkDestroyFence(dev, fences[f], nullptr);
            vkDestroySemaphore(dev, acquired[f], nullptr);
            vkDestroyQueryPool(dev, queries[f], nullptr);
        }
    }
    SDL_DestroyWindow(window);
    SDL_Quit();
    return restart ? VIEWER_RESTART : 0;
}

} // namespace bp
