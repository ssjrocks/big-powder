#include "gpu/Overlay.h"

#include <algorithm>
#include <array>
#include <cstring>

namespace bp {

namespace {
struct OverlayConsts {
    uint32_t screenW, screenH;
    uint32_t uiW, uiH, scale;
};
struct LayerConsts {
    uint32_t screenW, screenH;
    int32_t x0, y0;
    uint32_t w, h;
    float camX, camY, zoom;
    uint32_t fade;
};
} // namespace

Overlay::Overlay(VkContext &ctx, const std::string &shaderDir) : ctx_(ctx) {
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
    VkPushConstantRange range{VK_SHADER_STAGE_COMPUTE_BIT, 0, uint32_t(std::max(sizeof(OverlayConsts), sizeof(LayerConsts)))};
    VkPipelineLayoutCreateInfo plci{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    plci.setLayoutCount = 1;
    plci.pSetLayouts = &setLayout_;
    plci.pushConstantRangeCount = 1;
    plci.pPushConstantRanges = &range;
    VK_CHECK(vkCreatePipelineLayout(d, &plci, nullptr, &pipeLayout_));
    VkDescriptorPoolSize ps{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 4 * FRAMES};
    VkDescriptorPoolCreateInfo dpci{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    dpci.maxSets = 2 * FRAMES;
    dpci.poolSizeCount = 1;
    dpci.pPoolSizes = &ps;
    VK_CHECK(vkCreateDescriptorPool(d, &dpci, nullptr, &pool_));
    std::array<VkDescriptorSetLayout, 2 * FRAMES> layouts;
    layouts.fill(setLayout_);
    VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    ai.descriptorPool = pool_;
    ai.descriptorSetCount = 2 * FRAMES;
    ai.pSetLayouts = layouts.data();
    VK_CHECK(vkAllocateDescriptorSets(d, &ai, sets_));

    for (VkPipeline *p : {&pipeline_, &layerPipeline_}) {
        VkShaderModule m = ctx_.loadShader(shaderDir + (p == &pipeline_ ? "/overlay.spv" : "/world_layer.spv"));
        VkComputePipelineCreateInfo ci{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
        ci.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        ci.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
        ci.stage.module = m;
        ci.stage.pName = "main";
        ci.layout = pipeLayout_;
        VK_CHECK(vkCreateComputePipelines(d, VK_NULL_HANDLE, 1, &ci, nullptr, p));
        vkDestroyShaderModule(d, m, nullptr);
    }
}

Overlay::~Overlay() {
    VkDevice d = ctx_.device();
    vkDeviceWaitIdle(d);
    vkDestroyPipeline(d, pipeline_, nullptr);
    vkDestroyPipeline(d, layerPipeline_, nullptr);
    vkDestroyPipelineLayout(d, pipeLayout_, nullptr);
    vkDestroyDescriptorPool(d, pool_, nullptr);
    vkDestroyDescriptorSetLayout(d, setLayout_, nullptr);
    for (auto &c : canvas_)
        ctx_.destroyBuffer(c);
    for (auto &c : layer_)
        ctx_.destroyBuffer(c);
}

// Written every time: a buffer made again can get the handle of the one it replaced, so a handle says nothing about
// what a set points at (and this slot's last frame is done by now)
void Overlay::bind(int set, const Buffer &input, const Buffer &frame) {
    VkDescriptorBufferInfo infos[2] = {{input.buffer, 0, VK_WHOLE_SIZE}, {frame.buffer, 0, VK_WHOLE_SIZE}};
    VkWriteDescriptorSet w[2]{};
    for (uint32_t i = 0; i < 2; i++) {
        w[i] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        w[i].dstSet = sets_[set];
        w[i].dstBinding = i;
        w[i].descriptorCount = 1;
        w[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        w[i].pBufferInfo = &infos[i];
    }
    vkUpdateDescriptorSets(ctx_.device(), 2, w, 0, nullptr);
}

void Overlay::record(VkCommandBuffer cmd, int slot, const std::vector<uint32_t> &canvas, uint32_t uiW, uint32_t uiH,
                     uint32_t scale, const Buffer &frame, uint32_t screenW, uint32_t screenH) {
    if (canvas.empty() || !uiW || !uiH)
        return;
    int s = slot % FRAMES;
    VkDeviceSize bytes = VkDeviceSize(canvas.size()) * 4;
    if (canvas_[s].size < bytes) {
        // (this slot's last frame is done: the viewer waits for it before recording again)
        ctx_.destroyBuffer(canvas_[s]);
        canvas_[s] = ctx_.createBuffer(bytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, true);
    }
    std::memcpy(canvas_[s].mapped, canvas.data(), bytes);
    bind(s, canvas_[s], frame);
    OverlayConsts oc{screenW, screenH, uiW, uiH, scale};
    computeBarrier(cmd);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline_);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeLayout_, 0, 1, &sets_[s], 0, nullptr);
    vkCmdPushConstants(cmd, pipeLayout_, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(oc), &oc);
    vkCmdDispatch(cmd, (screenW + 15) / 16, (screenH + 15) / 16, 1);
}

void Overlay::recordLayer(VkCommandBuffer cmd, int slot, const std::vector<uint32_t> &layer, int x0, int y0, uint32_t w,
                          uint32_t h, float camX, float camY, float zoom, uint32_t fade, const Buffer &frame,
                          uint32_t screenW, uint32_t screenH) {
    int s = slot % FRAMES;
    VkDeviceSize bytes = std::max<VkDeviceSize>(VkDeviceSize(layer.size()) * 4, 4);
    if (layer_[s].size < bytes) {
        ctx_.destroyBuffer(layer_[s]);
        layer_[s] = ctx_.createBuffer(bytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, true);
    }
    if (!layer.empty())
        std::memcpy(layer_[s].mapped, layer.data(), layer.size() * 4);
    else
        w = h = 0;
    bind(FRAMES + s, layer_[s], frame);
    LayerConsts lc{screenW, screenH, x0, y0, w, h, camX, camY, zoom, fade};
    computeBarrier(cmd);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, layerPipeline_);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeLayout_, 0, 1, &sets_[FRAMES + s], 0, nullptr);
    vkCmdPushConstants(cmd, pipeLayout_, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(lc), &lc);
    vkCmdDispatch(cmd, (screenW + 15) / 16, (screenH + 15) / 16, 1);
}

} // namespace bp
