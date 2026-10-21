// Uploads a CPU-rendered image each frame and draws it through the camera into a frame buffer.
#pragma once
#include "gpu/VkContext.h"

#include <cstdint>
#include <string>
#include <vector>

namespace bp {

struct BlitParams {
    float camX = 0, camY = 0, zoom = 1;
    uint32_t screenW = 0, screenH = 0;
    int brushX = 0, brushY = 0, brushR = -1;
};

class ImageBlit {
public:
    static constexpr int FRAMES = 2; // staging buffers, one per frame in flight

    ImageBlit(VkContext &ctx, uint32_t imageW, uint32_t imageH, const std::string &shaderDir);
    ~ImageBlit();
    ImageBlit(const ImageBlit &) = delete;
    ImageBlit &operator=(const ImageBlit &) = delete;

    // Records: copy `pixels` (imageW*imageH, 0xRRGGBB) to the GPU through staging slot `slot`, then draw.
    void record(VkCommandBuffer cmd, int slot, const std::vector<uint32_t> &pixels, const BlitParams &p);
    void ensureFrameBuffer(uint32_t w, uint32_t h);
    const Buffer &frameBuffer() const { return frame_; }

private:
    void writeDescriptors();

    VkContext &ctx_;
    uint32_t imageW_, imageH_;
    Buffer staging_[FRAMES], image_, frame_;
    VkDescriptorSetLayout setLayout_ = VK_NULL_HANDLE;
    VkPipelineLayout pipeLayout_ = VK_NULL_HANDLE;
    VkDescriptorPool pool_ = VK_NULL_HANDLE;
    VkDescriptorSet set_ = VK_NULL_HANDLE;
    VkPipeline pipeline_ = VK_NULL_HANDLE;
};

} // namespace bp
