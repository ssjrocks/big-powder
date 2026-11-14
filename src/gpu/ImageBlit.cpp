#include "gpu/ImageBlit.h"

#include <algorithm>
#include <array>
#include <cstring>

namespace bp {

namespace {
struct BlitConsts {
    float camX, camY, zoom;
    uint32_t screenW, screenH;
    uint32_t imageW, imageH;
};
constexpr VkBufferUsageFlags STORAGE = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT |
                                       VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
} // namespace

ImageBlit::ImageBlit(VkContext &ctx, uint32_t imageW, uint32_t imageH, const std::string &shaderDir)
    : ctx_(ctx), imageW_(imageW), imageH_(imageH) {
    VkDeviceSize bytes = VkDeviceSize(imageW) * imageH * 4;
    for (auto &s : staging_)
        s = ctx_.createBuffer(bytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, true);
    image_ = ctx_.createBuffer(bytes, STORAGE, false);
    frame_ = ctx_.createBuffer(4, STORAGE, false);

    VkDevice d = ctx_.device();
    std::array<VkDescriptorSetLayoutBinding, 2> b{};
    for (uint32_t i = 0; i < 2; i++) {
        b[i].binding = i;
        b[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        b[i].descriptorCount = 1;
        b[i].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    }
    VkDescriptorSetLayoutCreateInfo lci{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    lci.bindingCount = 2;
    lci.pBindings = b.data();
    VK_CHECK(vkCreateDescriptorSetLayout(d, &lci, nullptr, &setLayout_));
    VkPushConstantRange range{VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(BlitConsts)};
    VkPipelineLayoutCreateInfo plci{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    plci.setLayoutCount = 1;
    plci.pSetLayouts = &setLayout_;
    plci.pushConstantRangeCount = 1;
    plci.pPushConstantRanges = &range;
    VK_CHECK(vkCreatePipelineLayout(d, &plci, nullptr, &pipeLayout_));
    VkDescriptorPoolSize ps{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 2};
    VkDescriptorPoolCreateInfo dpci{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    dpci.maxSets = 1;
    dpci.poolSizeCount = 1;
    dpci.pPoolSizes = &ps;
    VK_CHECK(vkCreateDescriptorPool(d, &dpci, nullptr, &pool_));
    VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    ai.descriptorPool = pool_;
    ai.descriptorSetCount = 1;
    ai.pSetLayouts = &setLayout_;
    VK_CHECK(vkAllocateDescriptorSets(d, &ai, &set_));

    VkShaderModule m = ctx_.loadShader(shaderDir + "/blit.spv");
    VkComputePipelineCreateInfo ci{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
    ci.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    ci.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    ci.stage.module = m;
    ci.stage.pName = "main";
    ci.layout = pipeLayout_;
    pipeline_ = ctx_.createComputePipeline(ci, "blit");
    vkDestroyShaderModule(d, m, nullptr);
    writeDescriptors();
}

ImageBlit::~ImageBlit() {
    VkDevice d = ctx_.device();
    vkDeviceWaitIdle(d);
    vkDestroyPipeline(d, pipeline_, nullptr);
    vkDestroyPipelineLayout(d, pipeLayout_, nullptr);
    vkDestroyDescriptorPool(d, pool_, nullptr);
    vkDestroyDescriptorSetLayout(d, setLayout_, nullptr);
    for (auto &s : staging_)
        ctx_.destroyBuffer(s);
    ctx_.destroyBuffer(image_);
    ctx_.destroyBuffer(frame_);
}

void ImageBlit::writeDescriptors() {
    VkDescriptorBufferInfo infos[2] = {{image_.buffer, 0, VK_WHOLE_SIZE}, {frame_.buffer, 0, VK_WHOLE_SIZE}};
    VkWriteDescriptorSet w[2]{};
    for (uint32_t i = 0; i < 2; i++) {
        w[i] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        w[i].dstSet = set_;
        w[i].dstBinding = i;
        w[i].descriptorCount = 1;
        w[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        w[i].pBufferInfo = &infos[i];
    }
    vkUpdateDescriptorSets(ctx_.device(), 2, w, 0, nullptr);
}

void ImageBlit::ensureFrameBuffer(uint32_t w, uint32_t h) {
    VkDeviceSize need = VkDeviceSize(w) * h * 4;
    if (frame_.size >= need)
        return;
    vkDeviceWaitIdle(ctx_.device());
    ctx_.destroyBuffer(frame_);
    frame_ = ctx_.createBuffer(need, STORAGE, false);
    writeDescriptors();
}

void ImageBlit::record(VkCommandBuffer cmd, int slot, const std::vector<uint32_t> &pixels, const BlitParams &p) {
    Buffer &st = staging_[slot % FRAMES];
    std::memcpy(st.mapped, pixels.data(), std::min<size_t>(pixels.size() * 4, st.size));
    VkBufferCopy region{0, 0, st.size};
    computeBarrier(cmd);
    vkCmdCopyBuffer(cmd, st.buffer, image_.buffer, 1, &region);
    computeBarrier(cmd);

    BlitConsts bc{p.camX, p.camY, p.zoom, p.screenW, p.screenH, imageW_, imageH_};
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline_);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeLayout_, 0, 1, &set_, 0, nullptr);
    vkCmdPushConstants(cmd, pipeLayout_, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(bc), &bc);
    vkCmdDispatch(cmd, (p.screenW + 15) / 16, (p.screenH + 15) / 16, 1);
}

} // namespace bp
