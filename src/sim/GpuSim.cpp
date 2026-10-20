#include "sim/GpuSim.h"

#include <algorithm>
#include <cstdio>
#include <numeric>

namespace bp {

namespace {
constexpr uint32_t BINDINGS = 11;
constexpr VkBufferUsageFlags STORAGE = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT |
                                       VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
uint32_t ceilDiv(uint32_t a, uint32_t b) { return (a + b - 1) / b; }
} // namespace

GpuSim::GpuSim(VkContext &ctx, const SimConfig &cfg, const ElementTable &elements)
    : ctx_(ctx), elements_(elements), shaderDir_(cfg.shaderDir), W_(cfg.W), H_(cfg.H), CW_(cfg.W / CELL),
      CH_(cfg.H / CELL), capacity_(cfg.W * cfg.H), colourN_(cfg.colourN), seed_(cfg.seed), rng_(cfg.seed) {
    if (W_ % CELL || H_ % CELL)
        throw std::runtime_error("world size must be a multiple of 4");
    if (colourN_ < 1 || colourN_ > 5)
        throw std::runtime_error("colourN must be 1..5");

    uint64_t listsSize = 0;
    for (uint32_t n = 1; n <= 5; n++)
        listsSize = std::max<uint64_t>(listsSize, uint64_t(n) * n * listCapFor(n));

    uint64_t limit = ctx_.properties().limits.maxStorageBufferRange;
    if (uint64_t(capacity_) * sizeof(Particle) > limit)
        throw std::runtime_error("world too large for one storage buffer on this GPU");

    parts_ = ctx_.createBuffer(uint64_t(capacity_) * sizeof(Particle), STORAGE, false);
    ptype_ = ctx_.createBuffer(uint64_t(capacity_) * 4, STORAGE, false);
    pmap_ = ctx_.createBuffer(uint64_t(W_) * H_ * 4, STORAGE, false);
    elems_ = ctx_.createBuffer(sizeof(ElementProps) * MAX_ELEMENTS, STORAGE, false);
    canMove_ = ctx_.createBuffer(uint64_t(MAX_ELEMENTS) * MAX_ELEMENTS * 4, STORAGE, false);
    lists_ = ctx_.createBuffer(listsSize * 4, STORAGE, false);
    counters_ = ctx_.createBuffer(CTR_SIZE * 4, STORAGE | VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT, false);
    air_ = ctx_.createBuffer(uint64_t(CW_) * CH_ * 3 * 4, STORAGE, false);
    freelist_ = ctx_.createBuffer(uint64_t(capacity_) * 4, STORAGE, false);
    stats_ = ctx_.createBuffer(STAT_SIZE * 4, STORAGE, false);
    frame_ = ctx_.createBuffer(4, STORAGE, false);

    std::vector<ElementProps> props(MAX_ELEMENTS);
    for (int i = 0; i < MAX_ELEMENTS; i++)
        props[i] = elements_.elements[i].props;
    ctx_.upload(elems_, props.data(), props.size() * sizeof(ElementProps));
    ctx_.upload(canMove_, elements_.canMove.data(), elements_.canMove.size() * 4);
    ctx_.immediate([&](VkCommandBuffer cmd) {
        for (const Buffer *b : {&ptype_, &pmap_, &counters_, &air_, &stats_})
            vkCmdFillBuffer(cmd, b->buffer, 0, VK_WHOLE_SIZE, 0);
    });

    createPipelines();
    updateDescriptors();
}

GpuSim::~GpuSim() {
    VkDevice d = ctx_.device();
    vkDeviceWaitIdle(d);
    for (VkPipeline p : {pPrepare_, pArgs_, pUpdate_, pFreelist_, pSpawn_, pRender_, pStatsParts_, pStatsCells_})
        vkDestroyPipeline(d, p, nullptr);
    vkDestroyPipelineLayout(d, pipeLayout_, nullptr);
    vkDestroyDescriptorPool(d, descPool_, nullptr);
    vkDestroyDescriptorSetLayout(d, setLayout_, nullptr);
    for (Buffer *b : {&parts_, &ptype_, &pmap_, &elems_, &canMove_, &lists_, &counters_, &air_, &frame_, &freelist_,
                      &stats_})
        ctx_.destroyBuffer(*b);
}

uint32_t GpuSim::listCapFor(uint32_t n) const {
    // One colour covers about 1/n^2 of the cells; leave room for stacked particles.
    uint32_t cells = ceilDiv(W_, n) * ceilDiv(H_, n);
    return cells + cells / 16 + 1024;
}

void GpuSim::createPipelines() {
    VkDevice d = ctx_.device();
    std::array<VkDescriptorSetLayoutBinding, BINDINGS> b{};
    for (uint32_t i = 0; i < BINDINGS; i++) {
        b[i].binding = i;
        b[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        b[i].descriptorCount = 1;
        b[i].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    }
    VkDescriptorSetLayoutCreateInfo lci{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    lci.bindingCount = BINDINGS;
    lci.pBindings = b.data();
    VK_CHECK(vkCreateDescriptorSetLayout(d, &lci, nullptr, &setLayout_));

    VkPushConstantRange range{VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(PushConsts)};
    VkPipelineLayoutCreateInfo plci{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    plci.setLayoutCount = 1;
    plci.pSetLayouts = &setLayout_;
    plci.pushConstantRangeCount = 1;
    plci.pPushConstantRanges = &range;
    VK_CHECK(vkCreatePipelineLayout(d, &plci, nullptr, &pipeLayout_));

    VkDescriptorPoolSize ps{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, BINDINGS};
    VkDescriptorPoolCreateInfo dpci{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    dpci.maxSets = 1;
    dpci.poolSizeCount = 1;
    dpci.pPoolSizes = &ps;
    VK_CHECK(vkCreateDescriptorPool(d, &dpci, nullptr, &descPool_));
    VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    ai.descriptorPool = descPool_;
    ai.descriptorSetCount = 1;
    ai.pSetLayouts = &setLayout_;
    VK_CHECK(vkAllocateDescriptorSets(d, &ai, &set_));

    auto make = [&](const char *name) {
        VkShaderModule m = ctx_.loadShader(shaderDir_ + "/" + name + ".spv");
        VkComputePipelineCreateInfo ci{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
        ci.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        ci.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
        ci.stage.module = m;
        ci.stage.pName = "main";
        ci.layout = pipeLayout_;
        VkPipeline p;
        VK_CHECK(vkCreateComputePipelines(d, VK_NULL_HANDLE, 1, &ci, nullptr, &p));
        vkDestroyShaderModule(d, m, nullptr);
        return p;
    };
    pPrepare_ = make("prepare");
    pArgs_ = make("args");
    pUpdate_ = make("update");
    pFreelist_ = make("freelist");
    pSpawn_ = make("spawn");
    pRender_ = make("render");
    pStatsParts_ = make("stats_parts");
    pStatsCells_ = make("stats_cells");
}

void GpuSim::updateDescriptors() {
    const Buffer *bufs[BINDINGS] = {&parts_, &ptype_, &pmap_, &elems_, &canMove_, &lists_,
                                    &counters_, &air_, &frame_, &freelist_, &stats_};
    std::array<VkDescriptorBufferInfo, BINDINGS> infos{};
    std::array<VkWriteDescriptorSet, BINDINGS> writes{};
    for (uint32_t i = 0; i < BINDINGS; i++) {
        infos[i] = {bufs[i]->buffer, 0, VK_WHOLE_SIZE};
        writes[i] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        writes[i].dstSet = set_;
        writes[i].dstBinding = i;
        writes[i].descriptorCount = 1;
        writes[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        writes[i].pBufferInfo = &infos[i];
    }
    vkUpdateDescriptorSets(ctx_.device(), BINDINGS, writes.data(), 0, nullptr);
}

void GpuSim::load(const SceneData &scene) {
    if (scene.W != W_ || scene.H != H_)
        throw std::runtime_error("scene size does not match the simulation");
    if (scene.ptype.size() > capacity_)
        throw std::runtime_error("scene has too many particles");
    vkDeviceWaitIdle(ctx_.device());
    ctx_.immediate([&](VkCommandBuffer cmd) {
        for (const Buffer *b : {&ptype_, &pmap_, &air_, &counters_})
            vkCmdFillBuffer(cmd, b->buffer, 0, VK_WHOLE_SIZE, 0);
    });
    if (!scene.ptype.empty()) {
        ctx_.upload(parts_, scene.parts.data(), scene.parts.size() * sizeof(Particle));
        ctx_.upload(ptype_, scene.ptype.data(), scene.ptype.size() * 4);
    }
    ctx_.upload(pmap_, scene.pmap.data(), scene.pmap.size() * 4);
    tick_ = 0;
}

void GpuSim::setColourN(uint32_t n) { colourN_ = std::clamp<uint32_t>(n, 1, 5); }

PushConsts GpuSim::basePush() const {
    PushConsts p{};
    p.W = W_;
    p.H = H_;
    p.CW = CW_;
    p.CH = CH_;
    p.tick = uint32_t(tick_);
    p.seed = seed_;
    p.colourN = colourN_;
    p.listCap = listCapFor(colourN_);
    p.capacity = capacity_;
    p.brushR = -1;
    return p;
}

void GpuSim::dispatch(VkCommandBuffer cmd, VkPipeline pipe, const PushConsts &pcs, uint32_t gx, uint32_t gy) {
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipe);
    vkCmdPushConstants(cmd, pipeLayout_, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(PushConsts), &pcs);
    vkCmdDispatch(cmd, gx, gy, 1);
}

void GpuSim::recordTick(VkCommandBuffer cmd, VkQueryPool pool, uint32_t firstQuery) {
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeLayout_, 0, 1, &set_, 0, nullptr);
    if (pool)
        vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, pool, firstQuery);

    PushConsts pcs = basePush();
    // A random grid offset and pass order each tick, so no direction or tile edge is favoured.
    pcs.colourOX = rng_() % colourN_;
    pcs.colourOY = rng_() % colourN_;
    uint32_t passes = colourN_ * colourN_;
    std::array<uint32_t, 32> order{};
    std::iota(order.begin(), order.begin() + passes, 0u);
    std::shuffle(order.begin(), order.begin() + passes, rng_);

    computeBarrier(cmd);
    vkCmdFillBuffer(cmd, counters_.buffer, CTR_COLOUR_COUNT * 4, MAX_COLOURS * 4, 0);
    vkCmdFillBuffer(cmd, pmap_.buffer, 0, VK_WHOLE_SIZE, 0); // rebuilt by prepare
    computeBarrier(cmd);
    dispatch(cmd, pPrepare_, pcs, ceilDiv(capacity_, 256));
    computeBarrier(cmd);
    dispatch(cmd, pArgs_, pcs, 1);
    computeBarrier(cmd);
    if (pool)
        vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, pool, firstQuery + 1);

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pUpdate_);
    for (uint32_t k = 0; k < passes; k++) {
        pcs.colourIdx = order[k];
        vkCmdPushConstants(cmd, pipeLayout_, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(PushConsts), &pcs);
        vkCmdDispatchIndirect(cmd, counters_.buffer, (CTR_DISPATCH + 3 * order[k]) * 4);
        computeBarrier(cmd);
    }
    if (pool)
        vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, pool, firstQuery + 2);
    tick_++;
}

void GpuSim::recordBrush(VkCommandBuffer cmd, const std::vector<BrushOp> &ops) {
    if (ops.empty())
        return;
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeLayout_, 0, 1, &set_, 0, nullptr);
    PushConsts pcs = basePush();
    computeBarrier(cmd);
    vkCmdFillBuffer(cmd, counters_.buffer, CTR_FREE_COUNT * 4, 2 * 4, 0);
    computeBarrier(cmd);
    dispatch(cmd, pFreelist_, pcs, ceilDiv(capacity_, 256));
    computeBarrier(cmd);
    for (const BrushOp &op : ops) {
        pcs.brushX = op.x;
        pcs.brushY = op.y;
        pcs.brushR = std::max(op.r, 0);
        pcs.brushType = op.type;
        uint32_t side = uint32_t(2 * pcs.brushR + 1);
        dispatch(cmd, pSpawn_, pcs, ceilDiv(side * side, 64));
        computeBarrier(cmd);
    }
}

void GpuSim::ensureFrameBuffer(uint32_t w, uint32_t h) {
    uint64_t need = uint64_t(w) * h * 4;
    if (frame_.size >= need)
        return;
    vkDeviceWaitIdle(ctx_.device());
    ctx_.destroyBuffer(frame_);
    frame_ = ctx_.createBuffer(need, STORAGE, false);
    updateDescriptors();
}

void GpuSim::recordRender(VkCommandBuffer cmd, const RenderParams &rp) {
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeLayout_, 0, 1, &set_, 0, nullptr);
    PushConsts pcs = basePush();
    pcs.camX = rp.camX;
    pcs.camY = rp.camY;
    pcs.zoom = rp.zoom;
    pcs.screenW = rp.screenW;
    pcs.screenH = rp.screenH;
    pcs.mode = rp.mode;
    pcs.brushX = rp.brushX;
    pcs.brushY = rp.brushY;
    pcs.brushR = rp.brushR;
    computeBarrier(cmd);
    dispatch(cmd, pRender_, pcs, ceilDiv(rp.screenW, 16), ceilDiv(rp.screenH, 16));
}

SimStats GpuSim::readStats() {
    PushConsts pcs = basePush();
    ctx_.immediate([&](VkCommandBuffer cmd) {
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeLayout_, 0, 1, &set_, 0, nullptr);
        vkCmdFillBuffer(cmd, stats_.buffer, 0, VK_WHOLE_SIZE, 0);
        computeBarrier(cmd);
        dispatch(cmd, pStatsParts_, pcs, ceilDiv(capacity_, 256));
        dispatch(cmd, pStatsCells_, pcs, ceilDiv(W_ * H_, 256));
        computeBarrier(cmd);
    });
    std::vector<uint32_t> st(STAT_SIZE), ctr(CTR_SIZE);
    ctx_.download(stats_, st.data(), st.size() * 4);
    ctx_.download(counters_, ctr.data(), ctr.size() * 4);
    SimStats s;
    for (int i = 0; i < MAX_ELEMENTS; i++) {
        s.typeCount[i] = st[STAT_TYPE_COUNT + i];
        s.total += st[STAT_TYPE_COUNT + i];
    }
    s.notInPmap = st[STAT_PART_NOT_IN_PMAP];
    s.dangling = st[STAT_PMAP_DANGLING];
    s.wrongPos = st[STAT_PMAP_WRONG_POS];
    s.locked = st[STAT_PMAP_LOCKED];
    s.listOverflow = ctr[CTR_LIST_OVERFLOW];
    s.stacked = ctr[CTR_STACKED];
    return s;
}

} // namespace bp
