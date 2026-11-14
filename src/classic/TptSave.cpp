// Compiled with TPT's include paths and flags (see CMakeLists.txt), as ClassicSim.cpp.
#include "classic/TptSaveImpl.h"

#include "common/platform/Platform.h"
#include "graphics/Renderer.h"
#include "graphics/VideoBuffer.h"
#include "simulation/ElementClasses.h"
#include "simulation/Simulation.h"
#include "simulation/elements/PIPE.h"

#include <zstd.h>

#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstring>
#include <mutex>
#include <shared_mutex>
#include <stdexcept>

namespace bp {

void SaveArea::setSize(int w, int h) {
    cw = w, ch = h;
    contentX = contentY = 0, contentW = w, contentH = h;
    particles.clear();
    const size_t n = cells();
    walls.assign(n, 0);
    blockAir.assign(n, 0);
    blockAirH.assign(n, 0);
    for (auto *v : {&fanVX, &fanVY, &pv, &vx, &vy, &hv, &gravMass, &gravForceX, &gravForceY})
        v->assign(n, 0.0f);
    gravMask.assign(n, 0xFFFFFFFFu);
}

bool tptPressureInTmp3(int type) { return GameSave::PressureInTmp3(type); }

namespace {

ClassicParticle fromTpt(const Particle &p) {
    return ClassicParticle{p.type, p.life, p.ctype, p.x, p.y, p.vx, p.vy, p.temp,
                           p.tmp, p.tmp2, p.tmp3, p.tmp4, uint32_t(p.flags), p.dcolour};
}

Particle toTpt(const ClassicParticle &c) {
    Particle p{};
    p.type = c.type, p.life = c.life, p.ctype = c.ctype, p.x = c.x, p.y = c.y, p.vx = c.vx, p.vy = c.vy;
    p.temp = c.temp, p.tmp = c.tmp, p.tmp2 = c.tmp2, p.tmp3 = c.tmp3, p.tmp4 = c.tmp4;
    p.flags = int(c.flags), p.dcolour = c.dcolour;
    return p;
}

template <class P, class V>
void planeOut(const P &plane, int cw, int ch, std::vector<V> &out) {
    out.resize(size_t(cw) * ch);
    for (int y = 0; y < ch; y++)
        for (int x = 0; x < cw; x++)
            out[size_t(y) * cw + x] = plane[Vec2<int>{x, y}];
}

template <class P, class V>
void planeIn(P &plane, int cw, int ch, const std::vector<V> &in) {
    if (in.size() != size_t(cw) * ch)
        return;
    for (int y = 0; y < ch; y++)
        for (int x = 0; x < cw; x++)
            plane[Vec2<int>{x, y}] = in[size_t(y) * cw + x];
}

} // namespace

void toSaveArea(const GameSave &g, SaveArea &a) {
    a = SaveArea{};
    a.cw = g.blockSize.X, a.ch = g.blockSize.Y;
    a.contentX = g.blockContent.pos.X, a.contentY = g.blockContent.pos.Y;
    a.contentW = g.blockContent.size.X, a.contentH = g.blockContent.size.Y;
    a.particles.reserve(size_t(g.particlesCount));
    for (int i = 0; i < g.particlesCount; i++)
        a.particles.push_back(fromTpt(g.particles[i]));
    planeOut(g.blockMap, a.cw, a.ch, a.walls);
    planeOut(g.blockAir, a.cw, a.ch, a.blockAir);
    planeOut(g.blockAirh, a.cw, a.ch, a.blockAirH);
    planeOut(g.fanVelX, a.cw, a.ch, a.fanVX);
    planeOut(g.fanVelY, a.cw, a.ch, a.fanVY);
    planeOut(g.pressure, a.cw, a.ch, a.pv);
    planeOut(g.velocityX, a.cw, a.ch, a.vx);
    planeOut(g.velocityY, a.cw, a.ch, a.vy);
    planeOut(g.ambientHeat, a.cw, a.ch, a.hv);
    planeOut(g.gravMass, a.cw, a.ch, a.gravMass);
    planeOut(g.gravMask, a.cw, a.ch, a.gravMask);
    planeOut(g.gravForceX, a.cw, a.ch, a.gravForceX);
    planeOut(g.gravForceY, a.cw, a.ch, a.gravForceY);
    a.hasPressure = g.hasPressure, a.hasAmbientHeat = g.hasAmbientHeat;
    a.hasBlockAirMaps = g.hasBlockAirMaps, a.hasGravityMaps = g.hasGravityMaps;
    for (const sign &s : g.signs)
        a.signs.push_back(Sign{s.x, s.y, int(s.ju), std::u32string(s.text.begin(), s.text.end())});
    a.rocketBoots1 = g.stkm.rocketBoots1, a.rocketBoots2 = g.stkm.rocketBoots2;
    a.fan1 = g.stkm.fan1, a.fan2 = g.stkm.fan2;
    a.rocketBootsFigh.assign(g.stkm.rocketBootsFigh.begin(), g.stkm.rocketBootsFigh.end());
    a.fanFigh.assign(g.stkm.fanFigh.begin(), g.stkm.fanFigh.end());
    a.version = {int(g.version[0]), int(g.version[1])};
    a.paused = g.paused;
    a.missingElements = bool(g.missingElements);
    a.options.heat = !g.legacyEnable;
    a.options.ambientHeat = g.aheatEnable;
    a.options.newtonianGravity = g.gravityEnable;
    a.options.waterEqualisation = g.waterEEnabled;
    a.options.edgeMode = g.edgeMode;
    a.options.gravityMode = g.gravityMode;
    a.options.airMode = g.airMode;
    a.options.customGravX = g.customGravityX, a.options.customGravY = g.customGravityY;
    a.options.ambientTemp = g.ambientAirTemp, a.options.vorticity = g.vorticityCoeff;
    a.options.edgePressure = g.edgePressure, a.options.edgeVX = g.edgeVelocityX, a.options.edgeVY = g.edgeVelocityY;
    a.options.convectionMode = g.convectionMode;
}

std::unique_ptr<GameSave> toGameSave(const SaveArea &a) {
    auto g = std::make_unique<GameSave>(Vec2<int>{a.cw, a.ch});
    g->blockContent = RectSized(Vec2<int>{a.contentX, a.contentY}, Vec2<int>{a.contentW, a.contentH});
    // (as GameSave's own readers and Transform leave it: particles in order, none-typed ones kept)
    g->particlesCount = int(std::min(a.particles.size(), size_t(NPART)));
    for (int i = 0; i < g->particlesCount; i++)
        g->particles[i] = toTpt(a.particles[size_t(i)]);
    planeIn(g->blockMap, a.cw, a.ch, a.walls);
    planeIn(g->blockAir, a.cw, a.ch, a.blockAir);
    planeIn(g->blockAirh, a.cw, a.ch, a.blockAirH);
    planeIn(g->fanVelX, a.cw, a.ch, a.fanVX);
    planeIn(g->fanVelY, a.cw, a.ch, a.fanVY);
    planeIn(g->pressure, a.cw, a.ch, a.pv);
    planeIn(g->velocityX, a.cw, a.ch, a.vx);
    planeIn(g->velocityY, a.cw, a.ch, a.vy);
    planeIn(g->ambientHeat, a.cw, a.ch, a.hv);
    planeIn(g->gravMass, a.cw, a.ch, a.gravMass);
    planeIn(g->gravMask, a.cw, a.ch, a.gravMask);
    planeIn(g->gravForceX, a.cw, a.ch, a.gravForceX);
    planeIn(g->gravForceY, a.cw, a.ch, a.gravForceY);
    g->hasPressure = a.hasPressure, g->hasAmbientHeat = a.hasAmbientHeat;
    g->hasBlockAirMaps = a.hasBlockAirMaps, g->hasGravityMaps = a.hasGravityMaps;
    for (const Sign &s : a.signs) // (TPT holds MAXSIGNS)
        if (g->signs.size() < MAXSIGNS)
            g->signs.emplace_back(String(s.text.data(), s.text.size()), s.x, s.y, sign::Justification(s.ju));
    g->stkm.rocketBoots1 = a.rocketBoots1, g->stkm.rocketBoots2 = a.rocketBoots2;
    g->stkm.fan1 = a.fan1, g->stkm.fan2 = a.fan2;
    g->stkm.rocketBootsFigh.assign(a.rocketBootsFigh.begin(), a.rocketBootsFigh.end());
    g->stkm.fanFigh.assign(a.fanFigh.begin(), a.fanFigh.end());
    g->version = Version<2>(a.version[0], a.version[1]);
    g->paused = a.paused;
    g->legacyEnable = !a.options.heat;
    g->aheatEnable = a.options.ambientHeat;
    g->gravityEnable = a.options.newtonianGravity;
    g->waterEEnabled = a.options.waterEqualisation;
    g->edgeMode = a.options.edgeMode;
    g->gravityMode = a.options.gravityMode;
    g->airMode = a.options.airMode;
    g->customGravityX = a.options.customGravX, g->customGravityY = a.options.customGravY;
    g->ambientAirTemp = a.options.ambientTemp, g->vorticityCoeff = a.options.vorticity;
    g->edgePressure = a.options.edgePressure, g->edgeVelocityX = a.options.edgeVX;
    g->edgeVelocityY = a.options.edgeVY;
    g->convectionMode = a.options.convectionMode;
    g->pmapbits = PMAPBITS;
    return g;
}

// GameSave::Transform, line for line, on a SaveArea and with the world's size in place of TPT's CELLS
void tptTransform(SaveArea &s, const Mat2i &m, int nudgeX, int nudgeY, int cellsW, int cellsH) {
    const Mat2<int> transform(m[0], m[1], m[2], m[3]);
    const Vec2<int> nudge{nudgeX, nudgeY}, blockSize{s.cw, s.ch}, worldCells{cellsW, cellsH};
    // undo translation by rotation
    auto br = transform * (blockSize * CELL - Vec2{1, 1});
    auto bbr = transform * (blockSize - Vec2{1, 1});
    auto translate = Vec2{std::max(0, -br.X), std::max(0, -br.Y)};
    auto btranslate = Vec2{std::max(0, -bbr.X), std::max(0, -bbr.Y)};
    auto newBlockS = transform * blockSize;
    newBlockS.X = std::abs(newBlockS.X);
    newBlockS.Y = std::abs(newBlockS.Y);
    translate += nudge;

    // Grow as needed.
    assert((Vec2{CELL, CELL}.OriginRect().Contains(nudge)));
    auto blockContent = newBlockS.OriginRect();
    if (nudge.X)
        newBlockS.X += 1;
    if (nudge.Y)
        newBlockS.Y += 1;
    if (nudge.X >= CELL / 2)
        blockContent.pos.X += 1;
    if (nudge.Y >= CELL / 2)
        blockContent.pos.Y += 1;
    btranslate += blockContent.pos;

    newBlockS = newBlockS.Clamp(RectBetween({0, 0}, worldCells));
    blockContent &= worldCells.OriginRect();
    auto newPartS = newBlockS * CELL;

    // Prepare to patch pipes.
    std::array<int, 8> pipeOffsetMap;
    std::transform(Element_PIPE_offsets.begin(), Element_PIPE_offsets.end(), pipeOffsetMap.begin(),
                   [transform](auto offset) {
                       auto it = std::find(Element_PIPE_offsets.begin(), Element_PIPE_offsets.end(), transform * offset);
                       assert(it != Element_PIPE_offsets.end());
                       return int(it - Element_PIPE_offsets.begin());
                   });

    // Translate signs.
    for (Sign &sg : s.signs) {
        auto newPos = transform * Vec2{sg.x, sg.y} + translate;
        if (!newPartS.OriginRect().Contains(newPos)) {
            sg.text.clear();
            continue;
        }
        sg.x = newPos.X;
        sg.y = newPos.Y;
    }

    // Translate particles.
    for (ClassicParticle &c : s.particles) {
        if (!c.type)
            continue;
        Particle p = toTpt(c);
        {
            // * We want particles to retain their distance from the centre of the particle grid cell
            //   they are in, but more importantly we also don't want them to change grid cells,
            //   so we just get rid of the edge cases.
            constexpr auto threshold = 0.001f;
            auto boundaryDiffX = p.x - (floor(p.x) + 0.5f);
            if (fabs(boundaryDiffX) < threshold)
                p.x += copysign(threshold, boundaryDiffX);
            auto boundaryDiffY = p.y - (floor(p.y) + 0.5f);
            if (fabs(boundaryDiffY) < threshold)
                p.y += copysign(threshold, boundaryDiffY);
        }
        if (p.x - floor(p.x) == 0.5f)
            p.x += 0.001f;
        if (p.y - floor(p.y) == 0.5f)
            p.y += 0.001f;
        auto newPos = transform * Vec2{p.x, p.y} + translate;
        if (!newPartS.OriginRect().Contains(Vec2{int(floor(newPos.X + 0.5f)), int(floor(newPos.Y + 0.5f))})) {
            c.type = PT_NONE;
            continue;
        }
        p.x = newPos.X;
        p.y = newPos.Y;
        auto newVel = transform * Vec2{p.vx, p.vy};
        p.vx = newVel.X;
        p.vy = newVel.Y;
        if (p.type == PT_PIPE || p.type == PT_PPIP)
            Element_PIPE_transformPatchOffsets(p, pipeOffsetMap);
        c = fromTpt(p);
    }

    // Translate blocky stuff.
    SaveArea n;
    n.setSize(newBlockS.X, newBlockS.Y);
    auto at = [](const SaveArea &a, Vec2<int> b) { return size_t(b.Y) * size_t(a.cw) + size_t(b.X); };
    for (auto bpos : blockSize.OriginRect()) {
        auto newBpos = transform * bpos + btranslate;
        if (!newBlockS.OriginRect().Contains(newBpos))
            continue;
        const size_t from = at(s, bpos), to = at(n, newBpos);
        if (s.walls[from]) {
            n.walls[to] = s.walls[from];
            if (s.walls[from] == WL_FAN) {
                auto newVel = transform * Vec2{s.fanVX[from], s.fanVY[from]};
                n.fanVX[to] = newVel.X;
                n.fanVY[to] = newVel.Y;
            }
        }
        n.pv[to] = s.pv[from];
        {
            auto transformed = transform * Vec2{s.vx[from], s.vy[from]};
            n.vx[to] = transformed.X;
            n.vy[to] = transformed.Y;
        }
        n.hv[to] = s.hv[from];
        n.blockAir[to] = s.blockAir[from];
        n.blockAirH[to] = s.blockAirH[from];
        n.gravMass[to] = s.gravMass[from];
        n.gravMask[to] = s.gravMask[from];
        {
            auto transformed = transform * Vec2{s.gravForceX[from], s.gravForceY[from]};
            n.gravForceX[to] = transformed.X;
            n.gravForceY[to] = transformed.Y;
        }
    }
    s.walls = std::move(n.walls);
    s.fanVX = std::move(n.fanVX);
    s.fanVY = std::move(n.fanVY);
    s.pv = std::move(n.pv);
    s.vx = std::move(n.vx);
    s.vy = std::move(n.vy);
    s.hv = std::move(n.hv);
    s.blockAir = std::move(n.blockAir);
    s.blockAirH = std::move(n.blockAirH);
    s.gravMass = std::move(n.gravMass);
    s.gravMask = std::move(n.gravMask);
    s.gravForceX = std::move(n.gravForceX);
    s.gravForceY = std::move(n.gravForceY);

    s.cw = newBlockS.X, s.ch = newBlockS.Y;
    s.contentX = blockContent.pos.X, s.contentY = blockContent.pos.Y;
    s.contentW = blockContent.size.X, s.contentH = blockContent.size.Y;
}

void tptTransformTpt(SaveArea &s, const Mat2i &m, int nudgeX, int nudgeY) {
    TptTables tables;
    auto g = toGameSave(s);
    g->Transform(Mat2<int>(m[0], m[1], m[2], m[3]), Vec2<int>{nudgeX, nudgeY});
    SaveArea out;
    toSaveArea(*g, out);
    out.particles.resize(s.particles.size()); // (GameSave holds NPART; the rest are none)
    out.paused = s.paused;
    s = std::move(out);
}

// Our own file for what is bigger than TPT reads: "BPSA", a format version, then everything zstd-compressed
namespace {

constexpr char BIG_MAGIC[4] = {'B', 'P', 'S', 'A'};
constexpr uint32_t BIG_VERSION = 1;

struct Out {
    std::vector<uint8_t> b;
    void bytes(const void *p, size_t n) {
        const uint8_t *c = static_cast<const uint8_t *>(p);
        b.insert(b.end(), c, c + n);
    }
    template <class T>
    void put(const T &v) { bytes(&v, sizeof v); }
    template <class T>
    void vec(const std::vector<T> &v) {
        put(uint64_t(v.size()));
        bytes(v.data(), v.size() * sizeof(T));
    }
};

struct In {
    const uint8_t *p, *end;
    void bytes(void *d, size_t n) {
        if (size_t(end - p) < n)
            throw std::runtime_error("the stamp is cut short");
        std::memcpy(d, p, n);
        p += n;
    }
    template <class T>
    T get() {
        T v;
        bytes(&v, sizeof v);
        return v;
    }
    template <class T>
    void vec(std::vector<T> &v) {
        uint64_t n = get<uint64_t>();
        if (n > uint64_t(end - p) / sizeof(T))
            throw std::runtime_error("the stamp is damaged");
        v.resize(size_t(n));
        bytes(v.data(), v.size() * sizeof(T));
    }
};

bool writeBig(const SaveArea &s, std::vector<char> &out, std::string &error) {
    Out o;
    o.put(s.cw), o.put(s.ch), o.put(s.contentX), o.put(s.contentY), o.put(s.contentW), o.put(s.contentH);
    o.vec(s.particles);
    for (auto *v : {&s.walls, &s.blockAir, &s.blockAirH})
        o.vec(*v);
    for (auto *v : {&s.fanVX, &s.fanVY, &s.pv, &s.vx, &s.vy, &s.hv, &s.gravMass, &s.gravForceX, &s.gravForceY})
        o.vec(*v);
    o.vec(s.gravMask);
    for (bool f : {s.hasPressure, s.hasAmbientHeat, s.hasBlockAirMaps, s.hasGravityMaps, s.rocketBoots1,
                   s.rocketBoots2, s.fan1, s.fan2, s.paused, s.missingElements})
        o.put(uint8_t(f));
    o.vec(s.rocketBootsFigh);
    o.vec(s.fanFigh);
    o.put(s.version[0]), o.put(s.version[1]);
    o.put(uint32_t(s.signs.size()));
    for (const Sign &sg : s.signs) {
        o.put(int32_t(sg.x)), o.put(int32_t(sg.y)), o.put(int32_t(sg.ju));
        o.put(uint32_t(sg.text.size()));
        o.bytes(sg.text.data(), sg.text.size() * sizeof(char32_t));
    }
    const ClassicOptions &op = s.options;
    for (bool f : {op.heat, op.ambientHeat, op.newtonianGravity, op.waterEqualisation})
        o.put(uint8_t(f));
    o.put(int32_t(op.edgeMode)), o.put(int32_t(op.gravityMode)), o.put(int32_t(op.airMode));
    for (float f : {op.customGravX, op.customGravY, op.ambientTemp, op.vorticity, op.edgePressure, op.edgeVX, op.edgeVY})
        o.put(f);
    o.put(int32_t(op.convectionMode));

    const size_t bound = ZSTD_compressBound(o.b.size());
    out.resize(8 + 8 + bound);
    std::memcpy(out.data(), BIG_MAGIC, 4);
    std::memcpy(out.data() + 4, &BIG_VERSION, 4);
    const uint64_t raw = o.b.size();
    std::memcpy(out.data() + 8, &raw, 8);
    size_t n = ZSTD_compress(out.data() + 16, bound, o.b.data(), o.b.size(), 3);
    if (ZSTD_isError(n)) {
        error = "could not compress the stamp";
        return false;
    }
    out.resize(16 + n);
    return true;
}

bool readBig(const std::vector<char> &data, SaveArea &s, std::string &error) {
    uint32_t version;
    uint64_t raw;
    std::memcpy(&version, data.data() + 4, 4);
    std::memcpy(&raw, data.data() + 8, 8);
    if (version != BIG_VERSION) {
        error = "the stamp is from another version of Big Powder";
        return false;
    }
    if (raw > (uint64_t(1) << 36)) {
        error = "the stamp is damaged";
        return false;
    }
    std::vector<uint8_t> b(raw);
    size_t n = ZSTD_decompress(b.data(), b.size(), data.data() + 16, data.size() - 16);
    if (ZSTD_isError(n) || n != b.size()) {
        error = "the stamp is damaged";
        return false;
    }
    try {
        In in{b.data(), b.data() + b.size()};
        s = SaveArea{};
        s.cw = in.get<int>(), s.ch = in.get<int>();
        s.contentX = in.get<int>(), s.contentY = in.get<int>(), s.contentW = in.get<int>(), s.contentH = in.get<int>();
        in.vec(s.particles);
        for (auto *v : {&s.walls, &s.blockAir, &s.blockAirH})
            in.vec(*v);
        for (auto *v : {&s.fanVX, &s.fanVY, &s.pv, &s.vx, &s.vy, &s.hv, &s.gravMass, &s.gravForceX, &s.gravForceY})
            in.vec(*v);
        in.vec(s.gravMask);
        for (bool *f : {&s.hasPressure, &s.hasAmbientHeat, &s.hasBlockAirMaps, &s.hasGravityMaps, &s.rocketBoots1,
                        &s.rocketBoots2, &s.fan1, &s.fan2, &s.paused, &s.missingElements})
            *f = in.get<uint8_t>() != 0;
        in.vec(s.rocketBootsFigh);
        in.vec(s.fanFigh);
        s.version[0] = in.get<int>(), s.version[1] = in.get<int>();
        s.signs.resize(in.get<uint32_t>());
        for (Sign &sg : s.signs) {
            sg.x = in.get<int32_t>(), sg.y = in.get<int32_t>(), sg.ju = in.get<int32_t>();
            sg.text.resize(in.get<uint32_t>());
            in.bytes(sg.text.data(), sg.text.size() * sizeof(char32_t));
        }
        ClassicOptions &op = s.options;
        for (bool *f : {&op.heat, &op.ambientHeat, &op.newtonianGravity, &op.waterEqualisation})
            *f = in.get<uint8_t>() != 0;
        op.edgeMode = in.get<int32_t>(), op.gravityMode = in.get<int32_t>(), op.airMode = in.get<int32_t>();
        for (float *f : {&op.customGravX, &op.customGravY, &op.ambientTemp, &op.vorticity, &op.edgePressure, &op.edgeVX,
                         &op.edgeVY})
            *f = in.get<float>();
        op.convectionMode = in.get<int32_t>();
        if (s.cw <= 0 || s.ch <= 0 || s.walls.size() != s.cells() || s.pv.size() != s.cells() ||
            s.gravMask.size() != s.cells())
            throw std::runtime_error("the stamp is damaged");
    } catch (const std::exception &e) {
        error = e.what();
        return false;
    }
    return true;
}

} // namespace

bool tptReadSave(const std::vector<char> &data, SaveArea &out, std::string &error) {
    if (data.size() >= 16 && std::memcmp(data.data(), BIG_MAGIC, 4) == 0)
        return readBig(data, out, error);
    TptTables tables;
    try {
        GameSave g(data, false);
        toSaveArea(g, out);
        return true;
    } catch (const std::exception &e) {
        error = e.what();
        return false;
    }
}

bool tptWriteSave(const SaveArea &s, std::vector<char> &out, std::string &error) {
    size_t live = 0;
    for (const ClassicParticle &p : s.particles)
        live += p.type != 0;
    if (s.cw > XCELLS || s.ch > YCELLS || live > size_t(NPART))
        return writeBig(s, out, error);
    TptTables tables;
    try {
        SaveArea packed = s; // (GameSave's << leaves out particles of no type)
        std::erase_if(packed.particles, [](const ClassicParticle &p) { return p.type == 0; });
        auto g = toGameSave(packed);
        auto [fakeNewer, bytes] = g->Serialise();
        (void)fakeNewer;
        if (bytes.empty()) {
            error = "TPT could not serialise the stamp";
            return false;
        }
        out = std::move(bytes);
        return true;
    } catch (const std::exception &e) {
        error = e.what();
        return false;
    }
}

namespace {

std::vector<uint32_t> renderSave(const SaveArea &s, const RendererSettings &settings, bool fire) {
    const int W = s.cw * CELL, H = s.ch * CELL;
    std::vector<uint32_t> out(size_t(std::max(W, 0)) * size_t(std::max(H, 0)), 0);
    if (W <= 0 || H <= 0)
        return out;
    auto sim = Simulation::Factory();
    Renderer ren;
    ren.sim = sim.get();
    ren.ApplySettings(settings);
    // SaveRenderer::Render, of the save placed at TPT's top-left corner
    auto draw = [&](const GameSave &save) {
        auto &sd = SimulationData::CRef();
        std::shared_lock lk(sd.elementGraphicsMx);
        sim->clear_sim();
        sim->Load(&save, true, {0, 0});
        ren.ClearAccumulation();
        ren.Clear();
        if (fire)
            ren.ApproximateAccumulation();
        ren.RenderSimulation();
        return &ren.GetVideo();
    };
    if (s.cw <= XCELLS && s.ch <= YCELLS) {
        const auto &video = *draw(*toGameSave(s));
        for (int y = 0; y < H; y++)
            for (int x = 0; x < W; x++)
                out[size_t(y) * W + size_t(x)] = video[{x, y}] & 0xFFFFFF;
        return out;
    }
    // A bigger one in pieces of TPT's size, each drawn with a margin around it for what spreads past a particle
    // (glow, blur)
    constexpr int margin = 8;
    constexpr int stepX = XCELLS - 2 * margin, stepY = YCELLS - 2 * margin;
    const int tilesX = (s.cw + stepX - 1) / stepX, tilesY = (s.ch + stepY - 1) / stepY;
    auto floorDiv = [](int a, int b) { return a >= 0 ? a / b : -((-a + b - 1) / b); };
    std::vector<std::vector<uint32_t>> inTile(size_t(tilesX) * tilesY);
    for (size_t i = 0; i < s.particles.size(); i++) {
        const ClassicParticle &p = s.particles[i];
        if (!p.type)
            continue;
        const int cx = floorDiv(int(std::floor(p.x + 0.5f)), CELL), cy = floorDiv(int(std::floor(p.y + 0.5f)), CELL);
        // tile t holds cells [t * step - margin, t * step + step + margin)
        const int txLo = std::max(0, floorDiv(cx - stepX - margin, stepX) + 1);
        const int txHi = std::min(tilesX - 1, floorDiv(cx + margin, stepX));
        const int tyLo = std::max(0, floorDiv(cy - stepY - margin, stepY) + 1);
        const int tyHi = std::min(tilesY - 1, floorDiv(cy + margin, stepY));
        for (int ty = tyLo; ty <= tyHi; ty++)
            for (int tx = txLo; tx <= txHi; tx++)
                inTile[size_t(ty) * tilesX + tx].push_back(uint32_t(i));
    }
    for (int ty = 0; ty < tilesY; ty++)
        for (int tx = 0; tx < tilesX; tx++) {
            const int bx = tx * stepX - margin, by = ty * stepY - margin; // the piece's corner, in cells
            SaveArea piece;
            piece.setSize(XCELLS, YCELLS);
            const int c0x = std::max(0, s.contentX - bx), c0y = std::max(0, s.contentY - by);
            const int c1x = std::min(XCELLS, s.contentX + s.contentW - bx);
            const int c1y = std::min(YCELLS, s.contentY + s.contentH - by);
            piece.contentX = c0x, piece.contentY = c0y;
            piece.contentW = std::max(0, c1x - c0x), piece.contentH = std::max(0, c1y - c0y);
            for (uint32_t i : inTile[size_t(ty) * tilesX + tx]) {
                ClassicParticle p = s.particles[i];
                p.x -= float(bx * CELL);
                p.y -= float(by * CELL);
                piece.particles.push_back(p);
            }
            for (int y = 0; y < YCELLS; y++)
                for (int x = 0; x < XCELLS; x++) {
                    const int sx = bx + x, sy = by + y;
                    if (sx < 0 || sy < 0 || sx >= s.cw || sy >= s.ch)
                        continue;
                    const size_t from = size_t(sy) * s.cw + size_t(sx), to = size_t(y) * XCELLS + size_t(x);
                    piece.walls[to] = s.walls[from];
                    piece.fanVX[to] = s.fanVX[from], piece.fanVY[to] = s.fanVY[from];
                    piece.pv[to] = s.pv[from], piece.vx[to] = s.vx[from], piece.vy[to] = s.vy[from];
                    piece.hv[to] = s.hv[from];
                    piece.blockAir[to] = s.blockAir[from], piece.blockAirH[to] = s.blockAirH[from];
                    piece.gravMass[to] = s.gravMass[from], piece.gravMask[to] = s.gravMask[from];
                    piece.gravForceX[to] = s.gravForceX[from], piece.gravForceY[to] = s.gravForceY[from];
                }
            for (const Sign &sg : s.signs)
                piece.signs.push_back(Sign{sg.x - bx * CELL, sg.y - by * CELL, sg.ju, sg.text});
            piece.hasPressure = s.hasPressure, piece.hasAmbientHeat = s.hasAmbientHeat;
            piece.hasBlockAirMaps = s.hasBlockAirMaps, piece.hasGravityMaps = s.hasGravityMaps;
            piece.rocketBoots1 = s.rocketBoots1, piece.rocketBoots2 = s.rocketBoots2;
            piece.fan1 = s.fan1, piece.fan2 = s.fan2, piece.version = s.version;
            const auto &video = *draw(*toGameSave(piece));
            for (int y = margin * CELL; y < (margin + stepY) * CELL; y++) {
                const int oy = by * CELL + y;
                if (oy < 0 || oy >= H)
                    continue;
                for (int x = margin * CELL; x < (margin + stepX) * CELL; x++) {
                    const int ox = bx * CELL + x;
                    if (ox >= 0 && ox < W)
                        out[size_t(oy) * W + size_t(ox)] = video[{x, y}] & 0xFFFFFF;
                }
            }
        }
    return out;
}

} // namespace

std::vector<uint32_t> tptRenderSave(const SaveArea &s, const ViewSettings &view, uint32_t overlays) {
    TptTables tables;
    return renderSave(s, viewRendererSettings(view, overlays), true);
}

std::vector<uint32_t> tptRenderThumbnail(const SaveArea &s, int boxW, int boxH, int &w, int &h) {
    TptTables tables;
    RendererSettings settings;
    settings.decorationLevel = RendererSettings::decorationEnabled;
    std::vector<uint32_t> full = renderSave(s, settings, false);
    w = s.cw * CELL, h = s.ch * CELL;
    if (full.empty())
        return full;
    VideoBuffer video(Vec2<int>{w, h});
    std::copy(full.begin(), full.end(), video.Data());
    video.ResizeToFit(Vec2<int>{boxW, boxH}, true);
    w = video.Size().X, h = video.Size().Y;
    std::vector<uint32_t> out(video.Data(), video.Data() + size_t(w) * size_t(h));
    for (uint32_t &p : out)
        p &= 0xFFFFFF;
    return out;
}

std::vector<uint32_t> tptRenderPreview(const SaveArea &s, int boxW, int boxH, int &w, int &h) {
    TptTables tables;
    RendererSettings settings;
    settings.decorationLevel = RendererSettings::decorationAntiClickbait;
    std::vector<uint32_t> full = renderSave(s, settings, true);
    w = s.cw * CELL, h = s.ch * CELL;
    if (full.empty())
        return full;
    VideoBuffer video(Vec2<int>{w, h});
    std::copy(full.begin(), full.end(), video.Data());
    video.ResizeToFit(Vec2<int>{boxW, boxH}, true);
    w = video.Size().X, h = video.Size().Y;
    std::vector<uint32_t> out(video.Data(), video.Data() + size_t(w) * size_t(h));
    for (uint32_t &p : out)
        p &= 0xFFFFFF;
    return out;
}

} // namespace bp
