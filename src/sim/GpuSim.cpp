#include "sim/GpuSim.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstddef>
#include <numeric>
#include <unordered_map>

namespace bp {

namespace {
constexpr uint32_t BINDINGS = 35;
constexpr size_t SIGN_OPS = 16384; // signs' drawing operations a frame (signs.slang)
constexpr VkBufferUsageFlags STORAGE = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT |
                                       VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
uint32_t ceilDiv(uint32_t a, uint32_t b) { return (a + b - 1) / b; }
} // namespace

GpuSim::GpuSim(VkContext &ctx, const SimConfig &cfg, const ElementTable &elements)
    : ctx_(ctx), elements_(elements), shaderDir_(cfg.shaderDir), W_(cfg.W), H_(cfg.H), CW_(cfg.W / CELL),
      CH_(cfg.H / CELL), capacity_(cfg.W * cfg.H), colourNX_(cfg.colourNX), colourNY_(cfg.colourNY), seed_(cfg.seed), air_(cfg.air), settings_(cfg.settings), passOrder_(cfg.passOrder), rng_(cfg.seed), stackRng_(cfg.seed + 1) {
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
    photons_ = ctx_.createBuffer(uint64_t(W_) * H_ * 4, STORAGE, false); // energy particles' map
    portals_ = ctx_.createBuffer(uint64_t(PORTAL_SLOTS) * sizeof(Particle), STORAGE, false); // inside PRTI/PRTO
    stickmen_ = ctx_.createBuffer(uint64_t(STICKMEN) * STICKMAN_BYTES, STORAGE, false);     // legs and so on
    // pmap_count where particles stack, then the list of such cells (stacking.slang)
    stacking_ = ctx_.createBuffer((uint64_t(W_) * H_ + 1 + 3 * STACK_CAP) * 4, STORAGE, false);
    // The renderer (gfx, fire and resolve.slang): TPT's tables, the world image, each particle's graphics,
    // fragment lists (a head per pixel and per fire cell) and the fire buffer
    renderTables_ = ctx_.createBuffer(uint64_t(elements_.renderTables.size()) * 4, STORAGE, false);
    video_ = ctx_.createBuffer(uint64_t(W_) * H_ * 4, STORAGE, false);
    gfx_ = ctx_.createBuffer(uint64_t(capacity_) * 12, STORAGE, false);
    fragHeads_ = ctx_.createBuffer((uint64_t(W_) * H_ + uint64_t(CW_) * CH_) * 4, STORAGE, false);
    frags_ = ctx_.createBuffer(uint64_t(FRAG_CAP) * 16, STORAGE, false);
    fireBuf_ = ctx_.createBuffer(uint64_t(CW_) * CH_ * 4, STORAGE, false);
    persistVideo_ = ctx_.createBuffer(uint64_t(W_) * H_ * 4, STORAGE, false); // the persistent display's last frame
    gfxFlags_ = ctx_.createBuffer(uint64_t(CW_) * CH_ * 4, STORAGE, false);
    // Newtonian gravity: per cell mass, mask, field and next field; the FFT arrays come when it is switched on
    gravCells_ = ctx_.createBuffer(uint64_t(CW_) * CH_ * 4 * 6, STORAGE, false);
    gravFft_ = ctx_.createBuffer(16, STORAGE, false);
    gravDraw_ = ctx_.createBuffer(16, STORAGE, false);
    // Signs: spots, what is there, drawing operations (signs.slang); read-backs per frame in flight
    signData_ = ctx_.createBuffer((4 * SIGN_MAX + SIGN_OPS) * 16, STORAGE, false);
    // The world image's pyramid for zooming out (mip.slang): levels 1 to 4
    {
        uint64_t texels = 0;
        for (uint32_t l = 1; l <= MIP_LEVELS; l++)
            texels += uint64_t((W_ + (1u << l) - 1) >> l) * ((H_ + (1u << l) - 1) >> l);
        mips_ = ctx_.createBuffer(texels * 4, STORAGE, false);
    }
    for (Buffer &b : signProbe_)
        b = ctx_.createBuffer(3 * SIGN_MAX * 16, VK_BUFFER_USAGE_TRANSFER_DST_BIT, true);
    // TPT's tools (spawn, wall_edit and fill_map.slang; grows when needed) and the brush cursor (render.slang)
    edit_ = ctx_.createBuffer(1 << 20, STORAGE, false);
    cursor_ = ctx_.createBuffer(uint64_t(W_) * H_ / 4 + 64, STORAGE, false); // up to two bits a world pixel
    auto pow2 = [](uint32_t v) { uint32_t p = 1; while (p < v) p <<= 1; return p; };
    gravNX_ = pow2(2 * CW_ - 1);
    gravNY_ = pow2(2 * CH_ - 1);
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
    // Deferred work, CW*CH entries each: set_emap requests, FloodINST, BANG flood_prop, BOMB explosions, beams
    // and lightning (three words each, in two regions), the ETRD particles (for ETRD's arcs), PPIP floods,
    // piston moves.
    emapReqs_ = ctx_.createBuffer(uint64_t(CW_) * CH_ * 4 * 9, STORAGE, false);
    bmap_.assign(size_t(CW_) * CH_, 0);

    std::vector<ElementProps> props(MAX_ELEMENTS);
    for (int i = 0; i < MAX_ELEMENTS; i++)
        props[i] = elements_.elements[i].props;
    ctx_.upload(elems_, props.data(), props.size() * sizeof(ElementProps));
    ctx_.upload(canMove_, elements_.canMove.data(), elements_.canMove.size() * 4);
    ctx_.upload(renderTables_, elements_.renderTables.data(), elements_.renderTables.size() * 4);
    ctx_.upload(defaults_, elements_.defaults.data(), elements_.defaults.size() * sizeof(Particle));
    ctx_.immediate([&](VkCommandBuffer cmd) {
        for (const Buffer *b : {&ptype_, &pmap_, &photons_, &counters_, &airBuf_, &stats_, &walls_, &portals_, &stickmen_,
                                &stacking_, &video_, &gfx_, &fireBuf_, &persistVideo_, &gravCells_})
            vkCmdFillBuffer(cmd, b->buffer, 0, VK_WHOLE_SIZE, 0);
    });
    applyEdgeMode();

    createPipelines();
    updateDescriptors();
    if (settings_.newtonianGravity)
        ensureGravity();
}

GpuSim::~GpuSim() {
    VkDevice d = ctx_.device();
    vkDeviceWaitIdle(d);
    for (VkPipeline p : {pLove_, pGol_, pWallsPre_, pEmapFill_, pDeferred_, pRays_, pEmp_, pInsert_, pAir_, pPrepare_, pArgs_, pUpdate_, pFreelist_, pSpawn_, pRender_, pStatsParts_, pStatsCells_, pStickmen_, pStacking_, pGfx_, pFire_, pResolve_, pHdisp_, pResolveHeavy_, pFireSmooth_, pGravity_, pLens_, pStreams_, pSigns_, pMip_, pWallEdit_, pFillMap_})
        vkDestroyPipeline(d, p, nullptr);
    vkDestroyPipelineLayout(d, pipeLayout_, nullptr);
    vkDestroyDescriptorPool(d, descPool_, nullptr);
    vkDestroyDescriptorSetLayout(d, setLayout_, nullptr);
    for (Buffer *b : {&parts_, &ptype_, &pmap_, &elems_, &canMove_, &lists_, &counters_, &airBuf_, &frame_, &freelist_,
                      &stats_, &inbox_, &walls_, &params_, &emapReqs_, &defaults_, &photons_, &portals_, &stickmen_, &stacking_,
                      &renderTables_, &video_, &gfx_, &fragHeads_, &frags_, &fireBuf_, &persistVideo_,
                      &gfxFlags_, &gravCells_, &gravFft_, &gravDraw_, &signData_, &mips_, &edit_, &cursor_})
        ctx_.destroyBuffer(*b);
    for (Buffer &b : signProbe_)
        ctx_.destroyBuffer(b);
}

uint32_t GpuSim::listCapFor(uint32_t nx, uint32_t ny) const {
    // One colour covers about 1/(nx*ny) of the cells; leave room for stacked particles.
    uint32_t cells = ceilDiv(W_, nx) * ceilDiv(H_, ny);
    return cells + cells / 16 + 1024;
}

VkPipeline GpuSim::makePipeline(const char *name) {
    VkDevice d = ctx_.device();
    VkShaderModule m = ctx_.loadShader(shaderDir_ + "/" + name + ".spv");
    VkComputePipelineCreateInfo ci{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
    ci.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    ci.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    ci.stage.module = m;
    ci.stage.pName = "main";
    ci.layout = pipeLayout_;
    VkPipeline p;
    auto t0 = std::chrono::steady_clock::now();
    VK_CHECK(vkCreateComputePipelines(d, VK_NULL_HANDLE, 1, &ci, nullptr, &p));
    double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    if (secs > 2.0) // the driver compiles new shaders once and caches them
        std::fprintf(stderr, "compiling the %s kernel took %.0f s\n", name, secs);
    vkDestroyShaderModule(d, m, nullptr);
    return p;
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

    pAir_ = makePipeline("air");
    pInsert_ = makePipeline("insert");
    pWallsPre_ = makePipeline("walls_pre");
    pEmapFill_ = makePipeline("emap_fill");
    pDeferred_ = makePipeline("deferred");
    pRays_ = makePipeline("rays");
    pLove_ = makePipeline("love");
    pGol_ = makePipeline("gol");
    pEmp_ = makePipeline("emp");
    pPrepare_ = makePipeline("prepare");
    pArgs_ = makePipeline("args");
    pUpdate_ = makePipeline("update");
    pFreelist_ = makePipeline("freelist");
    pSpawn_ = makePipeline("spawn");
    pWallEdit_ = makePipeline("wall_edit");
    pFillMap_ = makePipeline("fill_map");
    pRender_ = makePipeline("render");
    pStickmen_ = makePipeline("stickmen");
    pGfx_ = makePipeline("gfx");
    pHdisp_ = makePipeline("hdisp");
    pFire_ = makePipeline("fire");
    pResolve_ = makePipeline("resolve");
    pResolveHeavy_ = makePipeline("resolve_heavy");
    pFireSmooth_ = makePipeline("fire_smooth");
    pGravity_ = makePipeline("gravity");
    pLens_ = makePipeline("lens");
    pStreams_ = makePipeline("streams");
    pSigns_ = makePipeline("signs");
    pMip_ = makePipeline("mip");
    pStacking_ = makePipeline("stacking");
    pStatsParts_ = makePipeline("stats_parts");
    pStatsCells_ = makePipeline("stats_cells");
}

void GpuSim::updateDescriptors() {
    const Buffer *bufs[BINDINGS] = {&parts_, &ptype_, &pmap_, &elems_, &canMove_, &lists_,
                                    &counters_, &airBuf_, &frame_, &freelist_, &stats_, &inbox_,
                                    &walls_, &params_, &emapReqs_, &defaults_, &photons_, &portals_, &stickmen_,
                                    &stacking_, &renderTables_, &video_, &gfx_, &fragHeads_, &frags_, &fireBuf_,
                                    &persistVideo_, &gfxFlags_, &gravCells_, &gravFft_, &gravDraw_, &signData_, &mips_,
                                    &edit_, &cursor_};
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
    setSigns(scene.signs);
    ctx_.immediate([&](VkCommandBuffer cmd) {
        for (const Buffer *b : {&ptype_, &pmap_, &photons_, &airBuf_, &counters_, &portals_, &stickmen_, &stacking_,
                                &fireBuf_, &persistVideo_, &gravCells_})
            vkCmdFillBuffer(cmd, b->buffer, 0, VK_WHOLE_SIZE, 0);
    });
    gravMaskDirty_ = true;
    if (!scene.ptype.empty()) {
        ctx_.upload(parts_, scene.parts.data(), scene.parts.size() * sizeof(Particle));
        ctx_.upload(ptype_, scene.ptype.data(), scene.ptype.size() * 4);
    }
    // pmap and photons as RecalcFreeParticles builds them: in id order each particle goes on top (energy particles in
    // photons), except that INVIS and FILT only take an empty cell.
    {
        std::vector<uint32_t> pm(size_t(W_) * H_, 0), ph(size_t(W_) * H_, 0);
        for (size_t i = 0; i < scene.ptype.size(); i++) {
            uint32_t t = scene.ptype[i];
            if (!t)
                continue;
            int x = int(scene.parts[i].x + 0.5f), y = int(scene.parts[i].y + 0.5f);
            if (x < 0 || y < 0 || x >= int(W_) || y >= int(H_))
                continue;
            size_t c = size_t(y) * W_ + size_t(x);
            if (elements_.elements[t].props.properties & 0x10) // TYPE_ENERGY
                ph[c] = uint32_t(i) + 1;
            else if (!pm[c] || (t != PT_INVIS && t != PT_FILT))
                pm[c] = uint32_t(i) + 1;
        }
        ctx_.upload(pmap_, pm.data(), pm.size() * 4);
        ctx_.upload(photons_, ph.data(), ph.size() * 4);
    }
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
    p.stkmComm = stkmKeys_;
    p.newtonian = settings_.newtonianGravity ? 1 : 0;
    return p;
}

void GpuSim::setSettings(const SimSettings &s) {
    bool edgeChanged = s.edgeMode != settings_.edgeMode;
    bool gravityOff = settings_.newtonianGravity && !s.newtonianGravity;
    settings_ = s;
    paramsDirty_ = true;
    if (edgeChanged) {
        vkDeviceWaitIdle(ctx_.device());
        applyEdgeMode();
    }
    if (settings_.newtonianGravity)
        ensureGravity();
    if (gravityOff) {
        // EnableNewtonianGravity(false): the field goes (planes 2 to 5)
        vkDeviceWaitIdle(ctx_.device());
        VkDeviceSize plane = VkDeviceSize(CW_) * CH_ * 4;
        ctx_.immediate([&](VkCommandBuffer cmd) { vkCmdFillBuffer(cmd, gravCells_.buffer, 2 * plane, 4 * plane, 0); });
    }
}

// UpdateGravityMask: the field reaches the cells connected to the world's edge without crossing GRAV walls.
std::vector<uint32_t> GpuSim::gravityMask() const {
    std::vector<uint32_t> mask(size_t(CW_) * CH_, 0);
    std::vector<std::pair<int, int>> stack;
    auto check = [&](int x, int y) {
        size_t c = size_t(y) * CW_ + size_t(x);
        if (bmap_[c] == WL_GRAV || mask[c])
            return;
        mask[c] = 0xFFFFFFFFu;
        const int dx[4] = {0, -1, 1, 0}, dy[4] = {-1, 0, 0, 1};
        for (int k = 0; k < 4; k++) {
            int qx = x + dx[k], qy = y + dy[k];
            if (qx >= 0 && qy >= 0 && qx < int(CW_) && qy < int(CH_))
                stack.push_back({qx, qy});
        }
    };
    for (int x = 0; x < int(CW_); x++) {
        check(x, 0);
        check(x, int(CH_) - 1);
    }
    for (int y = 1; y < int(CH_) - 1; y++) {
        check(0, y);
        check(int(CW_) - 1, y);
    }
    while (!stack.empty()) {
        auto [x, y] = stack.back();
        stack.pop_back();
        check(x, y);
    }
    return mask;
}

// The FFT arrays and the kernel's spectra, made the first time Newtonian gravity is switched on: K(d) =
// -M_GRAV * d / |d|^3 for every offset between two cells, with the inverse transform's 1/(NX*NY) folded in.
void GpuSim::ensureGravity() {
    if (gravReady_)
        return;
    if (gravNX_ > 4096 || gravNY_ > 4096)
        throw std::runtime_error("Newtonian gravity supports worlds of up to 8192x8192 cells' worth of FFT (4096 a side)");
    vkDeviceWaitIdle(ctx_.device());
    uint64_t n = uint64_t(gravNX_) * gravNY_;
    ctx_.destroyBuffer(gravFft_);
    gravFft_ = ctx_.createBuffer(n * 8 * 5, STORAGE, false);
    ctx_.destroyBuffer(gravDraw_);
    gravDraw_ = ctx_.createBuffer(uint64_t(W_) * H_ * 4, STORAGE, false);
    updateDescriptors();
    std::vector<float> kx(n * 2, 0.0f), ky(n * 2, 0.0f);
    const float mGrav = 6.67300e-1f; // TPT's M_GRAV
    const float scale = -mGrav / float(n);
    for (int dy = -int(CH_) + 1; dy < int(CH_); dy++)
        for (int dx = -int(CW_) + 1; dx < int(CW_); dx++) {
            if (dx == 0 && dy == 0)
                continue;
            float distance = std::hypot(float(dx), float(dy));
            size_t k = size_t((dy + int(gravNY_)) % int(gravNY_)) * gravNX_ + size_t((dx + int(gravNX_)) % int(gravNX_));
            kx[2 * k] = scale * float(dx) / std::pow(distance, 3.f);
            ky[2 * k] = scale * float(dy) / std::pow(distance, 3.f);
        }
    ctx_.upload(gravFft_, kx.data(), n * 8, 0);
    ctx_.upload(gravFft_, ky.data(), n * 8, n * 8);
    ctx_.immediate([&](VkCommandBuffer cmd) {
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeLayout_, 0, 1, &set_, 0, nullptr);
        dispatchGravity(cmd, 8, 2 * gravNY_);
        dispatchGravity(cmd, 9, 2 * gravNX_);
    });
    gravReady_ = true;
}

void GpuSim::setSigns(const std::vector<Sign> &signs) {
    signs_.clear();
    for (const Sign &s : signs)
        if (!s.text.empty() && signs_.size() < SIGN_MAX)
            signs_.push_back(s);
    signSamples_.assign(signs_.size(), SignSample{});
    signsVersion_++;
}

// Simulation::Load's signs: moved with the save, kept if their spot is in the world
void GpuSim::addSigns(const std::vector<Sign> &signs, int offX, int offY) {
    std::vector<Sign> all = signs_;
    for (Sign s : signs) {
        s.x += offX;
        s.y += offY;
        if (s.x >= 0 && s.y >= 0 && s.x < int(W_) && s.y < int(H_))
            all.push_back(s);
    }
    setSigns(all);
}

void GpuSim::sampleSigns() {
    if (signs_.empty())
        return;
    vkDeviceWaitIdle(ctx_.device());
    ctx_.immediate([&](VkCommandBuffer cmd) {
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeLayout_, 0, 1, &set_, 0, nullptr);
        recordSignProbe(cmd, 0);
    });
    readSignProbe(0);
}

// What the signs show at their spots: written by signs.slang's stage 0, copied where the host can read it once the
// frame is done (the next time this frame slot comes round)
void GpuSim::recordSignProbe(VkCommandBuffer cmd, uint32_t slot) {
    uint32_t s = slot % PROBE_SLOTS, signs = uint32_t(std::min(signs_.size(), size_t(SIGN_MAX - 1)));
    uint32_t n = signs + 1; // and the cursor
    std::vector<uint32_t> spots(size_t(n) * 4, 0);
    for (uint32_t k = 0; k < signs; k++) {
        spots[4 * k] = uint32_t(signs_[k].x);
        spots[4 * k + 1] = uint32_t(signs_[k].y);
    }
    spots[4 * signs] = uint32_t(cursorX_);
    spots[4 * signs + 1] = uint32_t(cursorY_);
    vkCmdUpdateBuffer(cmd, signData_.buffer, 0, spots.size() * 4, spots.data());
    computeBarrier(cmd);
    PushConsts sp = basePush();
    sp.colourIdx = 0;
    sp.colourOX = n;
    dispatch(cmd, pSigns_, sp, 1, 1);
    computeBarrier(cmd);
    VkBufferCopy region{SIGN_MAX * 16, 0, VkDeviceSize(n) * 3 * 16};
    vkCmdCopyBuffer(cmd, signData_.buffer, signProbe_[s].buffer, 1, &region);
    VkMemoryBarrier mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    mb.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    mb.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &mb, 0, nullptr, 0,
                         nullptr);
    probeVersion_[s] = signsVersion_;
    probeSigns_[s] = signs;
    probeCursor_[s] = true;
}

void GpuSim::readSignProbe(uint32_t slot) {
    uint32_t s = slot % PROBE_SLOTS;
    const uint32_t *d = static_cast<const uint32_t *>(signProbe_[s].mapped);
    auto unpack = [](const uint32_t *a, SignSample &out) {
        out.hasPart = a[0] != 0;
        out.type = int(a[1]);
        out.ctype = int(a[2]);
        out.life = int(a[3]);
        out.tmp = int(a[4]);
        out.tmp2 = int(a[5]);
        out.tmp4 = int(a[6]);
        std::memcpy(&out.temp, &a[7], 4);
        std::memcpy(&out.pressure, &a[8], 4);
        std::memcpy(&out.aheat, &a[9], 4);
    };
    if (probeCursor_[s])
        unpack(d + 12 * probeSigns_[s], cursorSample_);
    if (probeVersion_[s] != signsVersion_)
        return; // (the signs have changed since; they show what they last did)
    signSamples_.resize(signs_.size());
    for (size_t k = 0; k < signs_.size() && k < probeSigns_[s]; k++)
        unpack(d + 12 * k, signSamples_[k]);
}

// Renderer::DrawSigns: each sign's operations (Signs.cpp), drawn one sign after another over its area
void GpuSim::recordSigns(VkCommandBuffer cmd, uint32_t slot) {
    (void)slot;
    std::vector<std::array<uint32_t, 4>> ops;
    struct Area { uint32_t first, count; int x0, y0, x1, y1; };
    std::vector<Area> areas;
    for (size_t k = 0; k < signs_.size(); k++) {
        SignDraw d = signDraw(elements_, signs_[k], k < signSamples_.size() ? signSamples_[k] : SignSample{});
        if (ops.size() + d.ops.size() > SIGN_OPS)
            break;
        areas.push_back({uint32_t(ops.size()), uint32_t(d.ops.size()), std::max(d.x0, 0), std::max(d.y0, 0),
                         std::min(d.x1, int(W_)), std::min(d.y1, int(H_))});
        ops.insert(ops.end(), d.ops.begin(), d.ops.end());
    }
    const VkDeviceSize base = 4 * SIGN_MAX * 16, bytes = ops.size() * 16;
    for (VkDeviceSize off = 0; off < bytes; off += 65536)
        vkCmdUpdateBuffer(cmd, signData_.buffer, base + off, std::min<VkDeviceSize>(65536, bytes - off),
                          reinterpret_cast<const char *>(ops.data()) + off);
    computeBarrier(cmd);
    PushConsts sp = basePush();
    sp.colourIdx = 1;
    for (const Area &a : areas) {
        if (a.x1 <= a.x0 || a.y1 <= a.y0)
            continue;
        sp.colourOX = a.first;
        sp.colourOY = a.count;
        sp.brushX = a.x0;
        sp.brushY = a.y0;
        sp.screenW = uint32_t(a.x1 - a.x0);
        sp.screenH = uint32_t(a.y1 - a.y0);
        dispatch(cmd, pSigns_, sp, ceilDiv(sp.screenW, 16), ceilDiv(sp.screenH, 16));
        computeBarrier(cmd);
    }
}

// UpdateGravityMask, when the walls have changed. TPT does it every frame, ticking or not (BeforeSim), so drawing
// does it too: the gravity zones show the walls as they are.
void GpuSim::recordGravityMask(VkCommandBuffer cmd) {
    if (!gravMaskDirty_)
        return;
    std::vector<uint32_t> mask = gravityMask();
    VkDeviceSize plane = VkDeviceSize(CW_) * CH_ * 4;
    for (VkDeviceSize off = 0; off < plane; off += 65536)
        vkCmdUpdateBuffer(cmd, gravCells_.buffer, plane + off, std::min<VkDeviceSize>(65536, plane - off),
                          reinterpret_cast<const char *>(mask.data()) + off);
    computeBarrier(cmd);
    gravMaskDirty_ = false;
}

void GpuSim::dispatchGravity(VkCommandBuffer cmd, uint32_t stage, uint32_t groups) {
    PushConsts gp = basePush();
    gp.colourIdx = stage;
    dispatch(cmd, pGravity_, gp, groups);
    computeBarrier(cmd);
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
    gravMaskDirty_ = true;
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
    // Each colour's place in this tick's order, for elements that ask whether a neighbour is updated later in
    // the tick (as TPT asks whether its id is higher)
    std::array<uint32_t, MAX_COLOURS> rank{};
    for (uint32_t k = 0; k < passes; k++)
        rank[order[k]] = k;

    computeBarrier(cmd);
    vkCmdUpdateBuffer(cmd, counters_.buffer, CTR_PASS_RANK * 4, sizeof(rank), rank.data());
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
    // ...Newtonian gravity: the field computed last tick comes into use, and the next one is computed from the mass
    // gathered during the last tick (DispatchNewtonianGravity); the mass is cleared for this tick either way...
    if (settings_.newtonianGravity && gravReady_) {
        uint32_t cells = CW_ * CH_, n = gravNX_ * gravNY_;
        dispatchGravity(cmd, 0, ceilDiv(cells, 256));
        dispatchGravity(cmd, 1, ceilDiv(n, 256));
        dispatchGravity(cmd, 2, CH_);
        dispatchGravity(cmd, 3, gravNX_);
        dispatchGravity(cmd, 4, ceilDiv(n, 256));
        dispatchGravity(cmd, 5, 2 * gravNX_);
        dispatchGravity(cmd, 6, 2 * CH_);
        dispatchGravity(cmd, 7, ceilDiv(cells, 256));
    } else {
        vkCmdFillBuffer(cmd, gravCells_.buffer, 0, VkDeviceSize(CW_) * CH_ * 4, 0); // gravIn.mass
        computeBarrier(cmd);
    }
    // ...and, as TPT updates the mask after starting that computation, a changed mask applies from the next tick
    recordGravityMask(cmd);
    vkCmdFillBuffer(cmd, counters_.buffer, CTR_PSTN_REQ * 4, 12 * 4, 0); // piston and PPIP queues, LIFE/LOVE flags, deferred work queues, EMP and ETRD counts
    vkCmdFillBuffer(cmd, counters_.buffer, CTR_COLOUR_COUNT * 4, MAX_COLOURS * 4, 0);
    vkCmdFillBuffer(cmd, counters_.buffer, CTR_FREE_COUNT * 4, 2 * 4, 0); // free list, refilled by prepare
    vkCmdFillBuffer(cmd, counters_.buffer, CTR_STKM_COUNT * 4, 4, 0);     // stick men, listed by prepare
    vkCmdFillBuffer(cmd, counters_.buffer, CTR_STKM_LEFT * 4, 4, 0);      // cells their heads leave
    vkCmdFillBuffer(cmd, stacking_.buffer, VkDeviceSize(W_) * H_ * 4, 4, 0); // cells where particles stack
    vkCmdFillBuffer(cmd, pmap_.buffer, 0, VK_WHOLE_SIZE, 0);              // rebuilt by prepare
    vkCmdFillBuffer(cmd, photons_.buffer, 0, VK_WHOLE_SIZE, 0);           // likewise
    computeBarrier(cmd);
    dispatch(cmd, pPrepare_, pcs, ceilDiv(capacity_, 256));
    computeBarrier(cmd);
    // After RecalcFreeParticles, BeforeSim counts electrified walls down and recomputes air blocking...
    dispatch(cmd, pWallsPre_, pcs, ceilDiv(CW_, 16), ceilDiv(CH_, 16));
    computeBarrier(cmd);
    // ...checks for stacking on 1 tick in 10, or after a save was pasted (the cells' counts are cleared every tick)...
    {
        bool check = forceStackingCheck_ || stackRng_() % 10 == 0;
        forceStackingCheck_ = false;
        PushConsts sp = pcs;
        sp.mode = check ? 1 : 0;
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pStacking_);
        for (uint32_t stage = 0; stage < 5; stage++) {
            sp.colourIdx = stage;
            vkCmdPushConstants(cmd, pipeLayout_, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(PushConsts), &sp);
            if (stage == 1 || stage == 2)
                vkCmdDispatchIndirect(cmd, counters_.buffer, CTR_STACK_ARGS * 4);
            else if (stage == 4)
                vkCmdDispatchIndirect(cmd, counters_.buffer, CTR_STACK_CLEAR_ARGS * 4);
            else
                vkCmdDispatch(cmd, 1, 1, 1);
            computeBarrier(cmd);
        }
    }
    // ...redraws LOVE and LOLZ, and steps the Game of Life (both empty dispatches unless prepare saw them; what they
    // make joins the colour lists, so the passes are sized afterwards)
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pLove_);
    vkCmdPushConstants(cmd, pipeLayout_, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(PushConsts), &pcs);
    vkCmdDispatchIndirect(cmd, counters_.buffer, CTR_LOVE_ARGS * 4);
    computeBarrier(cmd);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pGol_);
    for (uint32_t stage = 0; stage < 3; stage++) {
        PushConsts gp = pcs;
        gp.colourIdx = stage;
        vkCmdPushConstants(cmd, pipeLayout_, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(PushConsts), &gp);
        vkCmdDispatchIndirect(cmd, counters_.buffer, (stage == 1 ? CTR_GOLC_ARGS : CTR_GOLP_ARGS) * 4);
        computeBarrier(cmd);
    }
    // ...respawns STKM and STKM2 at their spawn points. The stick men's own update (legs, keys, fighters' aim) is done
    // here too, one thread for all of them; their heads then move in the colour passes (stickmen.slang).
    dispatch(cmd, pStickmen_, pcs, 1);
    computeBarrier(cmd);
    dispatch(cmd, pArgs_, pcs, 1);
    computeBarrier(cmd);
    if (serial_) {
        // Serial mode allocates like TPT instead: ids freed this tick first, then the lowest unused id
        // (allocPart in elements.slang). The counters become that stack's size and the scan position.
        vkCmdFillBuffer(cmd, counters_.buffer, CTR_FREE_COUNT * 4, 2 * 4, 0);
        computeBarrier(cmd);
    }
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
    if (pool)
        vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, pool, firstQuery + 3);
    // Electrify the wall networks that detectors and sparks asked for this tick, then the other deferred
    // work: INST networks sparks reached, BANG blobs set off, BOMB explosions.
    dispatch(cmd, pEmapFill_, pcs, 1);
    computeBarrier(cmd);
    dispatch(cmd, pDeferred_, pcs, 1);
    computeBarrier(cmd);
    // Beams from ARAY, CRAY and DRAY (one thread each), then EMP's effect on electronics (AfterSim). The
    // deferred pass wrote both dispatch sizes; they are empty on most ticks.
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pRays_);
    vkCmdPushConstants(cmd, pipeLayout_, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(PushConsts), &pcs);
    vkCmdDispatchIndirect(cmd, counters_.buffer, CTR_RAY_ARGS * 4);
    computeBarrier(cmd);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pEmp_);
    for (uint32_t phase = 0; phase < 2; phase++) { // mark the centres, then apply (emp.slang)
        pcs.colourIdx = phase;
        vkCmdPushConstants(cmd, pipeLayout_, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(PushConsts), &pcs);
        vkCmdDispatchIndirect(cmd, counters_.buffer, CTR_EMP_ARGS * 4);
        computeBarrier(cmd);
    }
    if (pool)
        vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, pool, firstQuery + 2);
    tick_++;
}

VkDeviceSize GpuSim::editBytes(const std::vector<PixelEdit> &edits) {
    VkDeviceSize words = 0;
    for (const PixelEdit &e : edits)
        words += 8 + e.counts.size();
    return words * 4;
}

void GpuSim::reserveEditSpace(VkDeviceSize bytes) {
    if (edit_.size >= bytes)
        return;
    VkDeviceSize size = std::max<VkDeviceSize>(edit_.size, 4096);
    while (size < bytes)
        size *= 2;
    vkDeviceWaitIdle(ctx_.device());
    ctx_.destroyBuffer(edit_);
    edit_ = ctx_.createBuffer(size, STORAGE, false);
    updateDescriptors();
}

// vkCmdUpdateBuffer takes up to 64 KiB at a time
void GpuSim::updateBuffer(VkCommandBuffer cmd, const Buffer &b, const std::vector<uint32_t> &words) {
    const VkDeviceSize bytes = words.size() * 4;
    for (VkDeviceSize off = 0; off < bytes; off += 65536)
        vkCmdUpdateBuffer(cmd, b.buffer, off, std::min<VkDeviceSize>(65536, bytes - off),
                          reinterpret_cast<const char *>(words.data()) + off);
}

void GpuSim::recordEdits(VkCommandBuffer cmd, const std::vector<PixelEdit> &edits) {
    if (edits.empty())
        return;
    std::vector<uint32_t> data;
    std::vector<uint32_t> at;
    for (const PixelEdit &e : edits) {
        at.push_back(uint32_t(data.size()));
        data.insert(data.end(), {uint32_t(e.x0), uint32_t(e.y0), uint32_t(e.w), uint32_t(e.h), uint32_t(e.type),
                                 e.flags, uint32_t(e.selected), editSalt_ += 0x9E3779B9u});
        data.insert(data.end(), e.counts.begin(), e.counts.end());
    }
    if (data.size() * 4 > edit_.size)
        throw std::logic_error("GpuSim::recordEdits: reserveEditSpace first");
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeLayout_, 0, 1, &set_, 0, nullptr);
    PushConsts pcs = basePush();
    computeBarrier(cmd);
    updateBuffer(cmd, edit_, data);
    vkCmdFillBuffer(cmd, counters_.buffer, CTR_FREE_COUNT * 4, 2 * 4, 0);
    vkCmdFillBuffer(cmd, counters_.buffer, CTR_INST_REQ * 4, 3 * 4, 0);
    computeBarrier(cmd);
    dispatch(cmd, pFreelist_, pcs, ceilDiv(capacity_, 256));
    computeBarrier(cmd);
    for (size_t k = 0; k < edits.size(); k++) {
        pcs.colourIdx = at[k];
        dispatch(cmd, pSpawn_, pcs, ceilDiv(uint32_t(edits[k].w), 16), ceilDiv(uint32_t(edits[k].h), 16));
        computeBarrier(cmd);
    }
    // A SPRK brush on INST sparks the whole INST network (FloodINST).
    dispatch(cmd, pDeferred_, basePush(), 1);
    computeBarrier(cmd);
}

void GpuSim::recordWallEdits(VkCommandBuffer cmd, const std::vector<WallEdit> &edits, bool gravityWalls) {
    if (edits.empty())
        return;
    std::vector<uint32_t> data{uint32_t(edits.size())};
    for (const WallEdit &e : edits) {
        uint32_t f[2];
        std::memcpy(f, &e.fvx, 4);
        std::memcpy(f + 1, &e.fvy, 4);
        data.insert(data.end(), {e.cell | uint32_t(bmap_[e.cell]) << 24, (e.type ? 1u : 0u) | (e.fan ? 2u : 0u), f[0], f[1]});
    }
    if (data.size() * 4 > edit_.size)
        throw std::logic_error("GpuSim::recordWallEdits: reserveEditSpace first");
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeLayout_, 0, 1, &set_, 0, nullptr);
    computeBarrier(cmd);
    updateBuffer(cmd, edit_, data);
    computeBarrier(cmd);
    PushConsts pcs = basePush();
    pcs.colourIdx = 0;
    dispatch(cmd, pWallEdit_, pcs, ceilDiv(uint32_t(edits.size()), 64));
    computeBarrier(cmd);
    if (gravityWalls)
        gravMaskDirty_ = true; // (TPT's gravWallChanged)
}

std::vector<uint32_t> GpuSim::readFillMap(int x, int y, int c, int &cm) {
    const uint32_t words = ceilDiv(W_ * H_, 32);
    reserveEditSpace(VkDeviceSize(words + 1) * 4);
    vkDeviceWaitIdle(ctx_.device());
    ctx_.immediate([&](VkCommandBuffer cmd) {
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeLayout_, 0, 1, &set_, 0, nullptr);
        PushConsts pcs = basePush();
        pcs.brushX = x;
        pcs.brushY = y;
        pcs.brushType = c;
        computeBarrier(cmd);
        dispatch(cmd, pFillMap_, pcs, ceilDiv(words, 256));
        computeBarrier(cmd);
    });
    std::vector<uint32_t> out(words + 1);
    ctx_.download(edit_, out.data(), out.size() * 4);
    cm = out[0] == 0xFFFFFFFFu ? -1 : int(out[0]);
    out.erase(out.begin());
    return out;
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

void GpuSim::recordWorldImage(VkCommandBuffer cmd, const RenderParams &rp, VkQueryPool pool, uint32_t firstQuery) {
    auto stamp = [&](uint32_t k) {
        if (pool)
            vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, pool, firstQuery + k);
    };
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeLayout_, 0, 1, &set_, 0, nullptr);
    PushConsts pcs = basePush();
    bool autoHeat;
    ClassicSim::presetModes(rp.preset, pcs.renderMode, pcs.displayMode, pcs.colorMode, autoHeat);
    pcs.decoLevel = rp.decoLevel;
    pcs.hdispMin = autoHeat ? -1.0f : rp.hdispMin; // (-1: the range hdisp.slang adjusts)
    pcs.hdispMax = rp.hdispMax;
    // (the field display draws nothing while there is no field)
    pcs.overlays = rp.overlays & (gravReady_ ? 3u : 2u);
    stamp(0);
    // TPT's frame (GameView::RenderSimulation): each particle's graphics and fragments, the fire buffer, then
    // each pixel, then the fire buffer's smoothing for the next frame.
    computeBarrier(cmd);
    recordGravityMask(cmd);
    vkCmdFillBuffer(cmd, fragHeads_.buffer, 0, VK_WHOLE_SIZE, 0);
    vkCmdFillBuffer(cmd, gfxFlags_.buffer, 0, VK_WHOLE_SIZE, 0);
    vkCmdFillBuffer(cmd, counters_.buffer, CTR_FRAG_COUNT * 4, 4, 0);
    vkCmdFillBuffer(cmd, counters_.buffer, CTR_FIRE_BOX * 4, 8, 0xFFFFFFFFu); // lowest x, y
    vkCmdFillBuffer(cmd, counters_.buffer, (CTR_FIRE_BOX + 2) * 4, 8, 0);     // highest x, y
    if (autoHeat) {
        const float scan[2] = {9999.0f, 0.0f}; // (lowest, highest): MAX_TEMP and MIN_TEMP to start with
        vkCmdUpdateBuffer(cmd, counters_.buffer, CTR_HDISP_SCAN * 4, sizeof(scan), scan);
    }
    computeBarrier(cmd);
    // Renderer::Clear's AdjustHdispLimit: the heat display's range for this frame
    PushConsts hp = pcs;
    if (autoHeat) {
        hp.colourIdx = 0;
        dispatch(cmd, pHdisp_, hp, ceilDiv(std::max(capacity_, CW_ * CH_), 256));
        computeBarrier(cmd);
    }
    hp.colourIdx = 1;
    dispatch(cmd, pHdisp_, hp, 1);
    computeBarrier(cmd);
    dispatch(cmd, pGfx_, pcs, ceilDiv(capacity_, 256));
    dispatch(cmd, pStreams_, pcs, ceilDiv(CW_ * CH_, 64)); // DrawWalls' streamlines
    computeBarrier(cmd);
    if (pcs.overlays & 1) { // Renderer::draw_grav
        vkCmdFillBuffer(cmd, gravDraw_.buffer, 0, VK_WHOLE_SIZE, 0);
        computeBarrier(cmd);
        PushConsts gp = pcs;
        gp.colourIdx = 10;
        dispatch(cmd, pGravity_, gp, ceilDiv(CW_ * CH_, 256));
        computeBarrier(cmd);
    }
    stamp(1);
    PushConsts fp = pcs;
    fp.colourIdx = 0;
    dispatch(cmd, pFire_, fp, ceilDiv(CW_ * CH_, 1024));
    computeBarrier(cmd);
    stamp(2);
    dispatch(cmd, pResolve_, pcs, ceilDiv(W_, 16), ceilDiv(H_, 16)); // the common case; marks the others
    computeBarrier(cmd);
    dispatch(cmd, pResolveHeavy_, pcs, ceilDiv(W_, 16), ceilDiv(H_, 16));
    computeBarrier(cmd);
    readSignProbe(rp.slot); // (what the signs and the cursor showed, from this slot's last frame)
    if (!signs_.empty()) // DrawSigns
        recordSigns(cmd, rp.slot);
    cursorX_ = rp.brushX;
    cursorY_ = rp.brushY;
    recordSignProbe(cmd, rp.slot);
    if (pcs.displayMode & 0x10) { // DISPLAY_WARP: gravitational lensing, from a copy of the frame
        VkBufferCopy region{0, 0, VkDeviceSize(W_) * H_ * 4};
        vkCmdCopyBuffer(cmd, video_.buffer, fragHeads_.buffer, 1, &region);
        computeBarrier(cmd);
        dispatch(cmd, pLens_, pcs, ceilDiv(W_, 16), ceilDiv(H_, 16));
        computeBarrier(cmd);
    }
    stamp(3);
    if (pcs.renderMode & 0x00FF0000) { // FIREMODE: render_fire smooths the buffer (and only then)
        dispatch(cmd, pFireSmooth_, fp, 1);
        computeBarrier(cmd);
    }
    stamp(4);
}

void GpuSim::recordRender(VkCommandBuffer cmd, const RenderParams &rp, bool drawWorld) {
    if (drawWorld)
        recordWorldImage(cmd, rp);
    else
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeLayout_, 0, 1, &set_, 0, nullptr);
    PushConsts pcs = basePush();
    pcs.camX = rp.camX;
    pcs.camY = rp.camY;
    pcs.zoom = rp.zoom;
    pcs.screenW = rp.screenW;
    pcs.screenH = rp.screenH;
    recordCursor(cmd, rp, pcs);
    // Zoomed out past 1:4, the screen samples the pyramid (render.slang): make the levels it needs
    float inv = 1.0f / rp.zoom;
    uint32_t levels = 0;
    while (levels < MIP_LEVELS && inv / float(1u << levels) > 4.0f)
        levels++;
    if (levels) {
        computeBarrier(cmd);
        uint32_t srcW = W_, srcH = H_, srcOffset = 0, dstOffset = 0;
        for (uint32_t l = 1; l <= levels; l++) {
            uint32_t w = (W_ + (1u << l) - 1) >> l, h = (H_ + (1u << l) - 1) >> l;
            PushConsts mp = basePush();
            mp.colourIdx = l;
            mp.colourOX = srcOffset;
            mp.colourOY = dstOffset;
            mp.brushX = int(srcW);
            mp.brushY = int(srcH);
            mp.screenW = w;
            mp.screenH = h;
            dispatch(cmd, pMip_, mp, ceilDiv(w, 16), ceilDiv(h, 16));
            computeBarrier(cmd);
            srcW = w, srcH = h, srcOffset = dstOffset;
            dstOffset += w * h;
        }
    }
    dispatch(cmd, pRender_, pcs, ceilDiv(rp.screenW, 16), ceilDiv(rp.screenH, 16));
}

// TPT's brush cursor for render.slang: how many times each pixel is inverted, over the box of the pixels in view.
// Inverting twice differs from once, but three times is the same as once.
void GpuSim::recordCursor(VkCommandBuffer cmd, const RenderParams &rp, PushConsts &pcs) {
    pcs.brushR = 0;
    if (!rp.cursor || rp.cursor->size() < 2)
        return;
    const std::vector<int> &px = *rp.cursor;
    const float inv = 1.0f / rp.zoom;
    const int vx0 = std::max(0, int(std::floor(rp.camX))), vy0 = std::max(0, int(std::floor(rp.camY)));
    const int vx1 = std::min(int(W_) - 1, int(std::floor(rp.camX + float(rp.screenW) * inv)));
    const int vy1 = std::min(int(H_) - 1, int(std::floor(rp.camY + float(rp.screenH) * inv)));
    int x0 = INT32_MAX, y0 = INT32_MAX, x1 = INT32_MIN, y1 = INT32_MIN;
    for (size_t k = 0; k + 1 < px.size(); k += 2)
        if (px[k] >= vx0 && px[k] <= vx1 && px[k + 1] >= vy0 && px[k + 1] <= vy1) {
            x0 = std::min(x0, px[k]), x1 = std::max(x1, px[k]);
            y0 = std::min(y0, px[k + 1]), y1 = std::max(y1, px[k + 1]);
        }
    if (x0 > x1)
        return;
    const int w = x1 - x0 + 1, h = y1 - y0 + 1;
    std::vector<uint32_t> words((size_t(w) * h + 15) / 16, 0);
    for (size_t k = 0; k + 1 < px.size(); k += 2) {
        int x = px[k] - x0, y = px[k + 1] - y0;
        if (x < 0 || y < 0 || x >= w || y >= h)
            continue;
        size_t i = size_t(y) * w + x;
        uint32_t shift = uint32_t(i & 15) * 2, n = (words[i >> 4] >> shift) & 3;
        n = n == 1 ? 2 : 1;
        words[i >> 4] = (words[i >> 4] & ~(3u << shift)) | (n << shift);
    }
    computeBarrier(cmd);
    updateBuffer(cmd, cursor_, words);
    computeBarrier(cmd);
    pcs.brushX = x0;
    pcs.brushY = y0;
    pcs.brushR = w;
    pcs.brushType = h;
}

void GpuSim::clearRenderAccumulation() {
    ctx_.immediate([&](VkCommandBuffer cmd) {
        vkCmdFillBuffer(cmd, fireBuf_.buffer, 0, VK_WHOLE_SIZE, 0);
        vkCmdFillBuffer(cmd, persistVideo_.buffer, 0, VK_WHOLE_SIZE, 0);
    });
}

void GpuSim::setGravityField(const std::vector<float> &fx, const std::vector<float> &fy) {
    vkDeviceWaitIdle(ctx_.device());
    VkDeviceSize plane = VkDeviceSize(CW_) * CH_ * 4;
    ctx_.upload(gravCells_, fx.data(), plane, 2 * plane);
    ctx_.upload(gravCells_, fy.data(), plane, 3 * plane);
}

void GpuSim::readGravityField(std::vector<float> &fx, std::vector<float> &fy) {
    vkDeviceWaitIdle(ctx_.device());
    size_t cells = size_t(CW_) * CH_;
    std::vector<float> all(cells * 6);
    ctx_.download(gravCells_, all.data(), all.size() * 4);
    fx.assign(all.begin() + 2 * cells, all.begin() + 3 * cells);
    fy.assign(all.begin() + 3 * cells, all.begin() + 4 * cells);
}

std::vector<uint32_t> GpuSim::readWorldImage() {
    std::vector<uint32_t> px(size_t(W_) * H_);
    vkDeviceWaitIdle(ctx_.device());
    ctx_.download(video_, px.data(), px.size() * 4);
    return px;
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

std::vector<ClassicParticle> GpuSim::readParticles(std::vector<int> *ids) {
    vkDeviceWaitIdle(ctx_.device());
    std::vector<uint32_t> types(capacity_);
    std::vector<Particle> ps(capacity_);
    ctx_.download(ptype_, types.data(), types.size() * 4);
    ctx_.download(parts_, ps.data(), ps.size() * sizeof(Particle));
    std::vector<ClassicParticle> out;
    if (ids)
        ids->clear();
    for (uint32_t i = 0; i < capacity_; i++) {
        if (!types[i])
            continue;
        if (ids)
            ids->push_back(int(i));
        const Particle &p = ps[i];
        out.push_back({int(types[i]), p.life, p.ctype, p.x, p.y, p.vx, p.vy, p.temp, p.tmp, p.tmp2, p.tmp3, p.tmp4,
                       p.flags, p.dcolour});
    }
    return out;
}

void GpuSim::forEachParticle(const std::function<void(uint32_t, uint32_t, const Particle &)> &f) {
    vkDeviceWaitIdle(ctx_.device());
    const uint32_t block = 1u << 20;
    std::vector<uint32_t> types(block);
    std::vector<Particle> ps(block);
    for (uint32_t start = 0; start < capacity_; start += block) {
        uint32_t n = std::min(block, capacity_ - start);
        ctx_.download(ptype_, types.data(), VkDeviceSize(n) * 4, VkDeviceSize(start) * 4);
        ctx_.download(parts_, ps.data(), VkDeviceSize(n) * sizeof(Particle), VkDeviceSize(start) * sizeof(Particle));
        for (uint32_t i = 0; i < n; i++)
            if (types[i])
                f(start + i, types[i], ps[i]);
    }
}

void GpuSim::readFans(std::vector<float> &fvx, std::vector<float> &fvy) {
    vkDeviceWaitIdle(ctx_.device());
    size_t cells = size_t(CW_) * CH_;
    fvx.resize(cells);
    fvy.resize(cells);
    ctx_.download(airBuf_, fvx.data(), cells * 4, 10 * cells * 4); // AIR_FVX
    ctx_.download(airBuf_, fvy.data(), cells * 4, 11 * cells * 4); // AIR_FVY
}

std::vector<uint32_t> GpuSim::readGravityCells() {
    vkDeviceWaitIdle(ctx_.device());
    std::vector<uint32_t> cells(size_t(CW_) * CH_ * 6);
    ctx_.download(gravCells_, cells.data(), cells.size() * 4);
    return cells;
}

void GpuSim::writeGravityCells(const std::vector<uint32_t> &cells) {
    if (cells.size() != size_t(CW_) * CH_ * 6)
        return;
    vkDeviceWaitIdle(ctx_.device());
    ctx_.upload(gravCells_, cells.data(), cells.size() * 4);
}

GpuSim::EngineState GpuSim::readEngineState() {
    vkDeviceWaitIdle(ctx_.device());
    EngineState s;
    s.counters.resize(CTR_SIZE);
    s.walls.resize(size_t(CW_) * CH_);
    s.portals.resize(size_t(PORTAL_SLOTS) * sizeof(Particle));
    ctx_.download(counters_, s.counters.data(), s.counters.size() * 4);
    ctx_.download(walls_, s.walls.data(), s.walls.size() * 4);
    ctx_.download(portals_, s.portals.data(), s.portals.size());
    return s;
}

void GpuSim::writeEngineState(const EngineState &s) {
    if (s.counters.size() != CTR_SIZE || s.walls.size() != size_t(CW_) * CH_ ||
        s.portals.size() != size_t(PORTAL_SLOTS) * sizeof(Particle))
        return;
    vkDeviceWaitIdle(ctx_.device());
    ctx_.upload(counters_, s.counters.data(), s.counters.size() * 4);
    ctx_.upload(walls_, s.walls.data(), s.walls.size() * 4);
    ctx_.upload(portals_, s.portals.data(), s.portals.size());
}

std::vector<uint8_t> GpuSim::readStickmen() {
    vkDeviceWaitIdle(ctx_.device());
    std::vector<uint8_t> s(size_t(STICKMEN) * STICKMAN_BYTES);
    ctx_.download(stickmen_, s.data(), s.size());
    return s;
}

void GpuSim::writeStickmen(const std::vector<uint8_t> &state) {
    if (state.size() != size_t(STICKMEN) * STICKMAN_BYTES)
        return;
    vkDeviceWaitIdle(ctx_.device());
    ctx_.upload(stickmen_, state.data(), state.size());
}

void GpuSim::insertParticles(const std::vector<ClassicParticle> &particles, int offsetX, int offsetY,
                             const std::vector<int> *ids) {
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
        for (uint32_t stage = 0; stage < 2; stage++) { // remove what is in the way, then place
            ip.mode = stage;
            dispatch(cmd, pInsert_, ip, ceilDiv(uint32_t(particles.size()), 64));
            computeBarrier(cmd);
        }
    });
    forceStackingCheck_ = true;

    // SOAP links point at particle ids where they came from: renumber them (Simulation::Load's soapList; a link to
    // a particle that wasn't placed is left as it was, as in TPT).
    if (!ids || ids->size() != particles.size())
        return;
    bool soap = false;
    for (const ClassicParticle &cp : particles)
        soap = soap || cp.type == PT_SOAP;
    if (!soap)
        return;
    ctx_.download(inbox_, in.data(), need);
    std::unordered_map<int, uint32_t> placed; // old id -> new id
    for (size_t i = 0; i < particles.size(); i++)
        if (particles[i].type == PT_SOAP && in[i].pad2 != 0xFFFFFFFFu)
            placed[(*ids)[i]] = in[i].pad2;
    ctx_.immediate([&](VkCommandBuffer cmd) {
        for (size_t i = 0; i < particles.size(); i++) {
            const ClassicParticle &cp = particles[i];
            if (cp.type != PT_SOAP || in[i].pad2 == 0xFFFFFFFFu)
                continue;
            int32_t links[2] = {cp.tmp, cp.tmp2};
            if (cp.ctype & 2)
                if (auto it = placed.find(cp.tmp); it != placed.end())
                    links[0] = int32_t(it->second);
            if (cp.ctype & 4)
                if (auto it = placed.find(cp.tmp2); it != placed.end())
                    links[1] = int32_t(it->second);
            vkCmdUpdateBuffer(cmd, parts_.buffer, VkDeviceSize(in[i].pad2) * sizeof(Particle) + offsetof(Particle, tmp),
                              sizeof(links), links);
        }
        computeBarrier(cmd);
    });
}

void GpuSim::stampAir(const std::vector<float> &pv, const std::vector<float> &vx, const std::vector<float> &vy,
                      const std::vector<float> &hv, int srcCW, int sx, int sy, int w, int h, int cx, int cy) {
    std::vector<float> dpv, dvx, dvy, dhv;
    readAir(dpv, dvx, dvy, dhv);
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            int tx = cx + x, ty = cy + y;
            if (tx < 0 || ty < 0 || tx >= int(CW_) || ty >= int(CH_))
                continue;
            size_t s = size_t(sy + y) * srcCW + size_t(sx + x), d = size_t(ty) * CW_ + size_t(tx);
            dpv[d] = pv[s];
            dvx[d] = vx[s];
            dvy[d] = vy[s];
            dhv[d] = hv[s];
        }
    writeAir(dpv, dvx, dvy, dhv);
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
