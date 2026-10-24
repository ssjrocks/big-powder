// Composites the interface (ui/Canvas, premultiplied RGBA in interface pixels) over a frame buffer
// (shaders/overlay.slang). The canvas is written straight into a host-visible buffer per frame in flight.
#pragma once
#include "gpu/VkContext.h"

#include <cstdint>
#include <string>
#include <vector>

namespace bp {

class Overlay {
public:
    static constexpr int FRAMES = 2;

    Overlay(VkContext &ctx, const std::string &shaderDir);
    ~Overlay();
    Overlay(const Overlay &) = delete;
    Overlay &operator=(const Overlay &) = delete;

    // Records: the canvas (uiW x uiH pixels, each covering scale x scale screen pixels) over `frame`
    void record(VkCommandBuffer cmd, int slot, const std::vector<uint32_t> &canvas, uint32_t uiW, uint32_t uiH,
                uint32_t scale, const Buffer &frame, uint32_t screenW, uint32_t screenH);
    // What recordLayer draws over the world through the camera (shaders/world_layer.slang), in this order
    struct Layer {
        uint32_t fade = 0; // the frame darkened first (0-255)
        // TPT's selection: the world (worldW x worldH) darkened by `dim` outside [rx0, rx1] x [ry0, ry1] (all of it
        // if rx1 < rx0)
        uint32_t worldW = 0, worldH = 0, dim = 0;
        int rx0 = 0, ry0 = 0, rx1 = -1, ry1 = -1;
        // a picture of w x h world pixels at (x0, y0): ui/Canvas pixels with their coverage, or opaque ones blended at
        // `alpha`; the same non-zero version as last time in this frame slot means it hasn't changed
        const std::vector<uint32_t> *pixels = nullptr;
        int x0 = 0, y0 = 0;
        uint32_t w = 0, h = 0;
        bool opaque = false;
        uint32_t alpha = 255;
        uint64_t version = 0;
        // a dotted rectangle XORed around [dx0, dx1] x [dy0, dy1]
        bool dotted = false;
        int dx0 = 0, dy0 = 0, dx1 = 0, dy1 = 0;
    };
    void recordLayer(VkCommandBuffer cmd, int slot, const Layer &layer, float camX, float camY, float zoom,
                     const Buffer &frame, uint32_t screenW, uint32_t screenH);

private:
    void bind(int set, const Buffer &input, const Buffer &frame);
    VkContext &ctx_;
    Buffer canvas_[FRAMES], layer_[FRAMES];
    uint64_t layerVersion_[FRAMES] = {};
    VkDescriptorSetLayout setLayout_ = VK_NULL_HANDLE;
    VkPipelineLayout pipeLayout_ = VK_NULL_HANDLE;
    VkDescriptorPool pool_ = VK_NULL_HANDLE;
    VkDescriptorSet sets_[2 * FRAMES] = {}; // the canvas's per frame, then the layer's
    VkPipeline pipeline_ = VK_NULL_HANDLE, layerPipeline_ = VK_NULL_HANDLE;
};

} // namespace bp
