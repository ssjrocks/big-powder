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
    // Records: `frame` darkened by fade (0-255), then a picture of w x h world pixels at (x0, y0) over it through the
    // camera (shaders/world_layer.slang)
    void recordLayer(VkCommandBuffer cmd, int slot, const std::vector<uint32_t> &layer, int x0, int y0, uint32_t w,
                     uint32_t h, float camX, float camY, float zoom, uint32_t fade, const Buffer &frame, uint32_t screenW,
                     uint32_t screenH);

private:
    void bind(int set, const Buffer &input, const Buffer &frame);
    VkContext &ctx_;
    Buffer canvas_[FRAMES], layer_[FRAMES];
    VkDescriptorSetLayout setLayout_ = VK_NULL_HANDLE;
    VkPipelineLayout pipeLayout_ = VK_NULL_HANDLE;
    VkDescriptorPool pool_ = VK_NULL_HANDLE;
    VkDescriptorSet sets_[2 * FRAMES] = {}; // the canvas's per frame, then the layer's
    VkPipeline pipeline_ = VK_NULL_HANDLE, layerPipeline_ = VK_NULL_HANDLE;
};

} // namespace bp
