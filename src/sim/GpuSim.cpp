#include "sim/GpuSim.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <numeric>

namespace bp {

namespace {
constexpr uint32_t BINDINGS = 16;
constexpr VkBufferUsageFlags STORAGE = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT |
                                       VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
uint32_t ceilDiv(uint32_t a, uint32_t b) { return (a + b - 1) / b; }
} // namespace

GpuSim::GpuSim(VkContext &ctx, const SimConfig &cfg, const ElementTable &elements)
    : ctx_(ctx), elements_(elements), shaderDir_(cfg.shaderDir), W_(cfg.W), H_(cfg.H), CW_(cfg.W / CELL),
      CH_(cfg.H / CELL), capacity_(cfg.W * cfg.H), colourNX_(cfg.colourNX), colourNY_(cfg.colourNY), seed_(cfg.seed), air_(cfg.air), settings_(cfg.settings), passOrder_(cfg.passOrder), rng_(cfg.seed) {
    if (W_ % CELL || H_ % CELL)
        throw std::runtime_error("world size must be a multiple of 4");
    setColours(colourNX_, colourNY_);

    // Enough for any colour grid up to MAX_COLOUR_SIDE x MAX_COLOUR_SIDE (see listCapFor).
    uint64_t listsSize = uint64_t(W_ + MAX_COLOUR_SIDE) * (H_ + MAX_COLOUR_SIDE) * 17 / 16 + 1024ull * MAX_COLOURS;

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
    airBuf_ = ctx_.createBuffer(uint64_t(CW_) * CH_ * AIR_PLANES * 4, STORAGE, false);
    freelist_ = ctx_.createBuffer(uint64_t(capacity_) * 4, STORAGE, false);
    stats_ = ctx_.createBuffer(STAT_SIZE * 4, STORAGE, false);
    frame_ = ctx_.createBuffer(4, STORAGE, false);
    inbox_ = ctx_.createBuffer(sizeof(Particle), STORAGE, false);
    walls_ = ctx_.createBuffer(uint64_t(CW_) * CH_ * 4, STORAGE, false);
    params_ = ctx_.createBuffer(sizeof(SimParams), STORAGE, false);
    defaults_ = ctx_.createBuffer(sizeof(Particle) * MAX_ELEMENTS, STORAGE, false);
    // Deferred work, CW*CH entries each: set_emap requests, FloodINST, BANG flood_prop and BOMB explosions.
    emapReqs_ = ctx_.createBuffer(uint64_t(CW_) * CH_ * 4 * 4, STORAGE, false);
    bmap_.assign(size_t(CW_) * CH_, 0);

    std::vector<ElementProps> props(MAX_ELEMENTS);
    for (int i = 0; i < MAX_ELEMENTS; i++)
        props[i] = elements_.elements[i].props;
    ctx_.upload(elems_, props.data(), props.size() * sizeof(ElementProps));
    ctx_.upload(canMove_, elements_.canMove.data(), elements_.canMove.size() * 4);
    ctx_.upload(defaults_, elements_.defaults.data(), elements_.defaults.size() * sizeof(Particle));
    ctx_.immediate([&](VkCommandBuffer cmd) {
        for (const Buffer *b : {&ptype_, &pmap_, &counters_, &airBuf_, &stats_, &walls_})
            vkCmdFillBuffer(cmd, b->buffer, 0, VK_WHOLE_SIZE, 0);
    });
    applyEdgeMode();

    createPipelines();
    updateDescriptors();
}

GpuSim::~GpuSim() {
    VkDevice d = ctx_.device();
    vkDeviceWaitIdle(d);
    for (VkPipeline p : {pWallsPre_, pEmapFill_, pDeferred_, pInsert_, pAir_, pPrepare_, pArgs_, pUpdate_, pFreelist_, pSpawn_, pRender_, pStatsParts_, pStatsCells_})
        vkDestroyPipeline(d, p, nullptr);
    vkDestroyPipelineLayout(d, pipeLayout_, nullptr);
    vkDestroyDescriptorPool(d, descPool_, nullptr);
    vkDestroyDescriptorSetLayout(d, setLayout_, nullptr);
    for (Buffer *b : {&parts_, &ptype_, &pmap_, &elems_, &canMove_, &lists_, &counters_, &airBuf_, &frame_, &freelist_,
                      &stats_, &inbox_, &walls_, &params_, &emapReqs_, &defaults_})
        ctx_.destroyBuffer(*b);
}

uint32_t GpuSim::listCapFor(uint32_t nx, uint32_t ny) const {
    // One colour covers about 1/(nx*ny) of the cells; leave room for stacked particles.
    uint32_t cells = ceilDiv(W_, nx) * ceilDiv(H_, ny);
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
    pAir_ = make("air");
    pInsert_ = make("insert");
    pWallsPre_ = make("walls_pre");
    pEmapFill_ = make("emap_fill");
    pDeferred_ = make("deferred");
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
                                    &counters_, &airBuf_, &frame_, &freelist_, &stats_, &inbox_,
                                    &walls_, &params_, &emapReqs_, &defaults_};
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
        for (const Buffer *b : {&ptype_, &pmap_, &airBuf_, &counters_})
            vkCmdFillBuffer(cmd, b->buffer, 0, VK_WHOLE_SIZE, 0);
    });
    if (!scene.ptype.empty()) {
        ctx_.upload(parts_, scene.parts.data(), scene.parts.size() * sizeof(Particle));
        ctx_.upload(ptype_, scene.ptype.data(), scene.ptype.size() * 4);
    }
    ctx_.upload(pmap_, scene.pmap.data(), scene.pmap.size() * 4);
    // clear_sim: ambient heat starts at the ambient air temperature.
    uint32_t ambientBits;
    std::memcpy(&ambientBits, &air_.ambientTemp, 4);
    VkDeviceSize plane = VkDeviceSize(CW_) * CH_ * 4;
    ctx_.immediate([&](VkCommandBuffer cmd) {
        vkCmdFillBuffer(cmd, airBuf_.buffer, 6 * plane, plane, ambientBits); // AIR_HV
    });
    if (scene.walls.size() == bmap_.size())
        bmap_ = scene.walls;
    else
        std::fill(bmap_.begin(), bmap_.end(), 0);
    applyEdgeMode();
    tick_ = 0;
}

SimParams GpuSim::currentParams() const {
    SimParams p{};
    p.airMode = air_.mode;
    p.vorticity = air_.vorticity;
    p.edgePressure = air_.edgePressure;
    p.edgeVX = air_.edgeVX;
    p.edgeVY = air_.edgeVY;
    p.ambientTemp = air_.ambientTemp;
    p.edgeMode = settings_.edgeMode;
    p.gravityMode = settings_.gravityMode;
    p.customGravX = settings_.customGravX;
    p.customGravY = settings_.customGravY;
    p.ambientHeat = settings_.ambientHeat ? 1 : 0;
    p.legacyHeat = settings_.heat ? 0 : 1;
    p.waterEqualisation = settings_.waterEqualisation ? 1 : 0;
    p.convectionMode = settings_.convectionMode;
    return p;
}

void GpuSim::setSettings(const SimSettings &s) {
    bool edgeChanged = s.edgeMode != settings_.edgeMode;
    settings_ = s;
    paramsDirty_ = true;
    if (edgeChanged) {
        vkDeviceWaitIdle(ctx_.device());
        applyEdgeMode();
    }
}

void GpuSim::applyEdgeMode() {
    // Simulation::SetEdgeMode: "solid" is a ring of walls around the border; void and loop clear that ring.
    uint8_t w = settings_.edgeMode == 1 ? WL_WALL : 0;
    for (uint32_t x = 0; x < CW_; x++) {
        bmap_[x] = w;
        bmap_[size_t(CH_ - 1) * CW_ + x] = w;
    }
    for (uint32_t y = 1; y + 1 < CH_; y++) {
        bmap_[size_t(y) * CW_] = w;
        bmap_[size_t(y) * CW_ + CW_ - 1] = w;
    }
    uploadWalls();
}

static uint32_t wallWord(uint8_t b);

void GpuSim::uploadWalls() {
    // Wall words as walls_pre.slang computes them, so air is blocked from the first tick (like TPT's
    // Air::ApproximateBlockAirMaps on load). Electricity starts off.
    std::vector<uint32_t> words(bmap_.size());
    for (size_t i = 0; i < bmap_.size(); i++)
        words[i] = wallWord(bmap_[i]);
    vkDeviceWaitIdle(ctx_.device());
    ctx_.upload(walls_, words.data(), words.size() * 4);
}

void GpuSim::setWalls(const std::vector<uint8_t> &bmap, const std::vector<float> *fanVX,
                      const std::vector<float> *fanVY) {
    if (bmap.size() != bmap_.size())
        throw std::runtime_error("wall map size does not match the simulation");
    bmap_ = bmap;
    applyEdgeMode();
    VkDeviceSize plane = VkDeviceSize(CW_) * CH_ * 4;
    if (fanVX && fanVX->size() == bmap_.size())
        ctx_.upload(airBuf_, fanVX->data(), plane, 10 * plane); // AIR_FVX
    if (fanVY && fanVY->size() == bmap_.size())
        ctx_.upload(airBuf_, fanVY->data(), plane, 11 * plane); // AIR_FVY
}

static uint32_t wallWord(uint8_t b) {
    bool blockair = b == WL_WALL || b == WL_WALLELEC || b == WL_BLOCKAIR || b == WL_EWALL;
    uint32_t blockairh = (blockair || b == WL_GRAV) ? 8u : 0u;
    return uint32_t(b) | (blockairh << 16) | (blockair ? (1u << 24) : 0u);
}

void GpuSim::recordPaintWalls(VkCommandBuffer cmd, int cx, int cy, int r, uint8_t wall) {
    bool any = false;
    for (int y = std::max(cy - r, 1); y <= std::min(cy + r, int(CH_) - 2); y++) {
        int x0 = -1;
        std::vector<uint32_t> words;
        for (int x = std::max(cx - r, 1); x <= std::min(cx + r, int(CW_) - 2); x++) {
            if ((x - cx) * (x - cx) + (y - cy) * (y - cy) > r * r)
                continue;
            if (x0 < 0)
                x0 = x;
            bmap_[size_t(y) * CW_ + x] = wall;
            words.push_back(wallWord(wall));
        }
        if (x0 < 0)
            continue;
        if (!any)
            computeBarrier(cmd);
        any = true;
        vkCmdUpdateBuffer(cmd, walls_.buffer, (VkDeviceSize(y) * CW_ + VkDeviceSize(x0)) * 4, words.size() * 4,
                          words.data());
    }
    if (any)
        computeBarrier(cmd);
}

void GpuSim::stampWalls(const std::vector<uint8_t> &src, int srcCW, int srcCH, int cx, int cy) {
    bool any = false;
    for (int y = 0; y < srcCH; y++)
        for (int x = 0; x < srcCW; x++) {
            uint8_t w = src[size_t(y) * srcCW + x];
            int tx = cx + x, ty = cy + y;
            if (!w || tx < 0 || ty < 0 || tx >= int(CW_) || ty >= int(CH_))
                continue;
            bmap_[size_t(ty) * CW_ + tx] = w;
            any = true;
        }
    if (any)
        applyEdgeMode();
}

void GpuSim::setColours(uint32_t nx, uint32_t ny) {
    if (nx < 1 || ny < 1 || nx > MAX_COLOUR_SIDE || ny > MAX_COLOUR_SIDE)
        throw std::runtime_error("colour grid sides must be 1..16");
    colourNX_ = nx;
    colourNY_ = ny;
}

PushConsts GpuSim::basePush() const {
    PushConsts p{};
    p.W = W_;
    p.H = H_;
    p.CW = CW_;
    p.CH = CH_;
    p.tick = uint32_t(tick_);
    p.seed = seed_;
    p.colourNX = colourNX_;
    p.colourNY = colourNY_;
    p.listCap = listCapFor(colourNX_, colourNY_);
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
    // The grid offset moves randomly each tick so no tile edge is favoured. Colour index = cy * N + cx, so
    // ascending order is raster order within the N*N pattern.
    uint32_t ox = rng_() % colourNX_, oy = rng_() % colourNY_;
    bool fixedOffset = passOrder_ == 2 || passOrder_ == 3;
    pcs.colourOX = fixedOffset ? 0 : ox;
    pcs.colourOY = fixedOffset ? 0 : oy;
    uint32_t passes = colourNX_ * colourNY_;
    std::array<uint32_t, MAX_COLOURS> order{};
    std::iota(order.begin(), order.begin() + passes, 0u);
    if (passOrder_ == 0)
        std::shuffle(order.begin(), order.begin() + passes, rng_);
    else if (passOrder_ >= 3) {
        // One random order, kept for every tick (like TPT, where particles keep their ids).
        if (fixedOrder_.size() != passes) {
            fixedOrder_.assign(order.begin(), order.begin() + passes);
            std::shuffle(fixedOrder_.begin(), fixedOrder_.end(), rng_);
        }
        std::copy(fixedOrder_.begin(), fixedOrder_.end(), order.begin());
    }

    computeBarrier(cmd);
    if (paramsDirty_) {
        SimParams sp = currentParams();
        vkCmdUpdateBuffer(cmd, params_.buffer, 0, sizeof(SimParams), &sp);
        computeBarrier(cmd);
        paramsDirty_ = false;
    }
    // BeforeSim: the air update comes first (Air::update_air), unless air is set to "no update", then
    // ambient heat (Air::update_airh) if it is on.
    if (air_.mode != 4) {
        PushConsts ap = pcs;
        for (uint32_t stage = 0; stage < 4; stage++) {
            ap.mode = stage;
            dispatch(cmd, pAir_, ap, ceilDiv(CW_, 16), ceilDiv(CH_, 16));
            computeBarrier(cmd);
        }
        // o-planes (OVX, OVY, OPV) back to VX, VY, PV.
        VkDeviceSize plane = VkDeviceSize(CW_) * CH_ * 4;
        VkBufferCopy region{3 * plane, 0, 3 * plane};
        vkCmdCopyBuffer(cmd, airBuf_.buffer, airBuf_.buffer, 1, &region);
        computeBarrier(cmd);
    }
    if (settings_.ambientHeat) {
        PushConsts ap = pcs;
        for (uint32_t stage = 4; stage <= 6; stage++) {
            ap.mode = stage;
            dispatch(cmd, pAir_, ap, ceilDiv(CW_, 16), ceilDiv(CH_, 16));
            computeBarrier(cmd);
        }
    }
    vkCmdFillBuffer(cmd, counters_.buffer, CTR_EMAP_REQ * 4, 4 * 4, 0); // the deferred work queues
    vkCmdFillBuffer(cmd, counters_.buffer, CTR_COLOUR_COUNT * 4, MAX_COLOURS * 4, 0);
    vkCmdFillBuffer(cmd, counters_.buffer, CTR_FREE_COUNT * 4, 2 * 4, 0); // free list, refilled by prepare
    vkCmdFillBuffer(cmd, pmap_.buffer, 0, VK_WHOLE_SIZE, 0);              // rebuilt by prepare
    computeBarrier(cmd);
    dispatch(cmd, pPrepare_, pcs, ceilDiv(capacity_, 256));
    computeBarrier(cmd);
    if (serial_) {
        // Serial mode allocates like TPT instead: ids freed this tick first, then the lowest unused id
        // (allocPart in elements.slang). The counters become that stack's size and the scan position.
        vkCmdFillBuffer(cmd, counters_.buffer, CTR_FREE_COUNT * 4, 2 * 4, 0);
        computeBarrier(cmd);
    }
    // After RecalcFreeParticles, BeforeSim counts electrified walls down and recomputes air blocking.
    dispatch(cmd, pWallsPre_, pcs, ceilDiv(CW_, 16), ceilDiv(CH_, 16));
    computeBarrier(cmd);
    dispatch(cmd, pArgs_, pcs, 1);
    computeBarrier(cmd);
    if (pool)
        vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, pool, firstQuery + 1);

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pUpdate_);
    pcs.mode = debugCounters_ ? 1 : 0;
    if (serial_) {
        pcs.mode |= 2;
        constexpr uint32_t chunk = 2048; // SERIAL_CHUNK in update.slang
        for (uint32_t start = 0; start < capacity_; start += chunk) {
            pcs.colourIdx = start;
            vkCmdPushConstants(cmd, pipeLayout_, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(PushConsts), &pcs);
            vkCmdDispatch(cmd, 1, 1, 1);
            computeBarrier(cmd);
        }
        passes = 0;
    }
    for (uint32_t k = 0; k < passes; k++) {
        pcs.colourIdx = order[k];
        vkCmdPushConstants(cmd, pipeLayout_, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(PushConsts), &pcs);
        vkCmdDispatchIndirect(cmd, counters_.buffer, (CTR_DISPATCH + 3 * order[k]) * 4);
        computeBarrier(cmd);
    }
    // Electrify the wall networks that detectors and sparks asked for this tick, then the other deferred
    // work: INST networks sparks reached, BANG blobs set off, BOMB explosions.
    dispatch(cmd, pEmapFill_, pcs, 1);
    computeBarrier(cmd);
    dispatch(cmd, pDeferred_, pcs, 1);
    computeBarrier(cmd);
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
    vkCmdFillBuffer(cmd, counters_.buffer, CTR_INST_REQ * 4, 3 * 4, 0);
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
    // A SPRK brush on INST sparks the whole INST network (FloodINST).
    dispatch(cmd, pDeferred_, basePush(), 1);
    computeBarrier(cmd);
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
    s.moved = ctr[CTR_DBG_MOVED];
    s.lockedTarget = ctr[CTR_DBG_LOCKED];
    s.ownLost = ctr[CTR_DBG_OWN_LOST];
    s.casLost = ctr[CTR_DBG_CAS_LOST];
    s.samePass = ctr[CTR_DBG_SAME_PASS];
    return s;
}

std::vector<ClassicParticle> GpuSim::readParticles() {
    vkDeviceWaitIdle(ctx_.device());
    std::vector<uint32_t> types(capacity_);
    std::vector<Particle> ps(capacity_);
    ctx_.download(ptype_, types.data(), types.size() * 4);
    ctx_.download(parts_, ps.data(), ps.size() * sizeof(Particle));
    std::vector<ClassicParticle> out;
    for (uint32_t i = 0; i < capacity_; i++) {
        if (!types[i])
            continue;
        const Particle &p = ps[i];
        out.push_back({int(types[i]), p.life, p.ctype, p.x, p.y, p.vx, p.vy, p.temp, p.tmp, p.tmp2, p.tmp3, p.tmp4,
                       p.flags, p.dcolour});
    }
    return out;
}

void GpuSim::insertParticles(const std::vector<ClassicParticle> &particles, int offsetX, int offsetY) {
    if (particles.empty())
        return;
    VkDeviceSize need = VkDeviceSize(particles.size()) * sizeof(Particle);
    vkDeviceWaitIdle(ctx_.device());
    if (inbox_.size < need) {
        ctx_.destroyBuffer(inbox_);
        inbox_ = ctx_.createBuffer(need, STORAGE, false);
        updateDescriptors();
    }
    std::vector<Particle> in(particles.size());
    for (size_t i = 0; i < particles.size(); i++) {
        const ClassicParticle &cp = particles[i];
        Particle &p = in[i];
        p = Particle{};
        p.life = cp.life;
        p.ctype = cp.ctype;
        p.x = cp.x;
        p.y = cp.y;
        p.vx = cp.vx;
        p.vy = cp.vy;
        p.temp = cp.temp;
        p.tmp = cp.tmp;
        p.tmp2 = cp.tmp2;
        p.tmp3 = cp.tmp3;
        p.tmp4 = cp.tmp4;
        p.flags = cp.flags;
        p.dcolour = cp.dcolour;
        p.pad1 = uint32_t(cp.type); // insert.slang reads the type from here
    }
    ctx_.upload(inbox_, in.data(), need);
    PushConsts pcs = basePush();
    ctx_.immediate([&](VkCommandBuffer cmd) {
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeLayout_, 0, 1, &set_, 0, nullptr);
        vkCmdFillBuffer(cmd, counters_.buffer, CTR_FREE_COUNT * 4, 2 * 4, 0);
        computeBarrier(cmd);
        dispatch(cmd, pFreelist_, pcs, ceilDiv(capacity_, 256));
        computeBarrier(cmd);
        PushConsts ip = pcs;
        ip.brushType = int32_t(particles.size());
        ip.brushX = offsetX;
        ip.brushY = offsetY;
        dispatch(cmd, pInsert_, ip, ceilDiv(uint32_t(particles.size()), 64));
        computeBarrier(cmd);
    });
}

void GpuSim::readAir(std::vector<float> &pv, std::vector<float> &vx, std::vector<float> &vy,
                     std::vector<float> &hv) {
    vkDeviceWaitIdle(ctx_.device());
    size_t n = size_t(CW_) * CH_;
    VkDeviceSize plane = n * 4;
    vx.resize(n);
    vy.resize(n);
    pv.resize(n);
    hv.resize(n);
    ctx_.download(airBuf_, vx.data(), plane, 0 * plane);
    ctx_.download(airBuf_, vy.data(), plane, 1 * plane);
    ctx_.download(airBuf_, pv.data(), plane, 2 * plane);
    ctx_.download(airBuf_, hv.data(), plane, 6 * plane);
}

void GpuSim::writeAir(const std::vector<float> &pv, const std::vector<float> &vx, const std::vector<float> &vy,
                      const std::vector<float> &hv) {
    vkDeviceWaitIdle(ctx_.device());
    VkDeviceSize plane = VkDeviceSize(CW_) * CH_ * 4;
    ctx_.upload(airBuf_, vx.data(), plane, 0 * plane);
    ctx_.upload(airBuf_, vy.data(), plane, 1 * plane);
    ctx_.upload(airBuf_, pv.data(), plane, 2 * plane);
    ctx_.upload(airBuf_, hv.data(), plane, 6 * plane);
}

} // namespace bp
