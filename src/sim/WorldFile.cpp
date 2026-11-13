#include "sim/WorldFile.h"
#include "sim/GpuSave.h"
#include "sim/Elements.h"
#include "sim/GpuSim.h"
#include "sim/Scenes.h"

#include <zstd.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <map>
#include <memory>
#include <thread>

namespace bp {

namespace {

const char MAGIC[8] = {'B', 'I', 'G', 'P', 'O', 'W', 'D', 'R'};
constexpr uint32_t VERSION = 2; // (2: the convection mode as TPT numbers it)
constexpr uint32_t PMAPBITS = 9; // TPT's: how types are packed with other bits in particle fields
constexpr uint64_t BLOCKS = ~uint64_t(0); // a section made of blocks that end with an empty one
constexpr uint32_t PARTICLE_BLOCK = 1u << 20;

constexpr uint32_t tag(const char (&s)[5]) {
    return uint32_t(uint8_t(s[0])) | (uint32_t(uint8_t(s[1])) << 8) | (uint32_t(uint8_t(s[2])) << 16) |
           (uint32_t(uint8_t(s[3])) << 24);
}
constexpr uint32_t T_HEAD = tag("HEAD"), T_SETT = tag("SETT"), T_PALT = tag("PALT"), T_PART = tag("PART"),
                   T_WALL = tag("WALL"), T_AIR = tag("AIR_"), T_GRAV = tag("GRAV"), T_SIGN = tag("SIGN"),
                   T_STKM = tag("STKM"), T_ENGN = tag("ENGN"), T_END = tag("END_");

struct FileCloser {
    void operator()(FILE *f) const {
        if (f)
            std::fclose(f);
    }
};

// zstd-compressed output, several threads
class Writer {
public:
    explicit Writer(FILE *f) : f_(f), cctx_(ZSTD_createCCtx()), out_(ZSTD_CStreamOutSize()) {
        ZSTD_CCtx_setParameter(cctx_, ZSTD_c_compressionLevel, 3);
        ZSTD_CCtx_setParameter(cctx_, ZSTD_c_nbWorkers, int(std::min(8u, std::max(1u, std::thread::hardware_concurrency() / 3))));
    }
    ~Writer() { ZSTD_freeCCtx(cctx_); }
    void write(const void *data, size_t n) { pump(data, n, ZSTD_e_continue); }
    template <class T> void put(T v) { write(&v, sizeof v); }
    template <class T> void array(const std::vector<T> &v) { write(v.data(), v.size() * sizeof(T)); }
    void section(uint32_t t, uint64_t length) {
        put(t);
        put(length);
    }
    bool finish() {
        pump(nullptr, 0, ZSTD_e_end);
        return ok_;
    }

private:
    void pump(const void *data, size_t n, ZSTD_EndDirective mode) {
        ZSTD_inBuffer in{data, n, 0};
        for (;;) {
            ZSTD_outBuffer out{out_.data(), out_.size(), 0};
            size_t left = ZSTD_compressStream2(cctx_, &out, &in, mode);
            if (ZSTD_isError(left)) {
                ok_ = false;
                return;
            }
            if (out.pos && std::fwrite(out_.data(), 1, out.pos, f_) != out.pos)
                ok_ = false;
            bool done = mode == ZSTD_e_end ? left == 0 : in.pos == in.size;
            if (done)
                return;
        }
    }
    FILE *f_;
    ZSTD_CCtx *cctx_;
    std::vector<char> out_;
    bool ok_ = true;
};

class Reader {
public:
    explicit Reader(FILE *f) : f_(f), dctx_(ZSTD_createDCtx()), in_(ZSTD_DStreamInSize()), out_(ZSTD_DStreamOutSize()) {}
    ~Reader() { ZSTD_freeDCtx(dctx_); }
    bool read(void *data, size_t n) {
        char *d = static_cast<char *>(data);
        while (n) {
            if (outPos_ == outSize_ && !refill())
                return false;
            size_t k = std::min(n, outSize_ - outPos_);
            std::memcpy(d, out_.data() + outPos_, k);
            outPos_ += k;
            d += k;
            n -= k;
        }
        return true;
    }
    template <class T> bool get(T &v) { return read(&v, sizeof v); }
    template <class T> bool array(std::vector<T> &v, size_t n) {
        v.resize(n);
        return read(v.data(), n * sizeof(T));
    }
    bool skip(uint64_t n) {
        char buf[4096];
        while (n) {
            size_t k = size_t(std::min<uint64_t>(n, sizeof buf));
            if (!read(buf, k))
                return false;
            n -= k;
        }
        return true;
    }

private:
    bool refill() {
        for (;;) {
            if (inPos_ == inSize_) {
                inSize_ = std::fread(in_.data(), 1, in_.size(), f_);
                inPos_ = 0;
                if (!inSize_)
                    return false;
            }
            ZSTD_inBuffer in{in_.data(), inSize_, inPos_};
            ZSTD_outBuffer out{out_.data(), out_.size(), 0};
            size_t r = ZSTD_decompressStream(dctx_, &out, &in);
            inPos_ = in.pos;
            if (ZSTD_isError(r))
                return false;
            if (out.pos) {
                outPos_ = 0;
                outSize_ = out.pos;
                return true;
            }
        }
    }
    FILE *f_;
    ZSTD_DCtx *dctx_;
    std::vector<char> in_, out_;
    size_t inPos_ = 0, inSize_ = 0, outPos_ = 0, outSize_ = 0;
};

std::unique_ptr<FILE, FileCloser> openFile(const std::string &path, const char *mode) {
    return std::unique_ptr<FILE, FileCloser>(std::fopen(path.c_str(), mode));
}

struct Header {
    uint32_t W = 0, H = 0, CW = 0, CH = 0;
    uint64_t tick = 0, particles = 0;
    uint32_t seed = 0, pmapbits = PMAPBITS;
    uint32_t version = VERSION;
};

bool readStart(FILE *f, Reader &r, Header &h, std::string &error) {
    char magic[8];
    uint32_t version = 0;
    if (std::fread(magic, 1, 8, f) != 8 || std::memcmp(magic, MAGIC, 8) != 0 || std::fread(&version, 4, 1, f) != 1) {
        error = "not a Big Powder world file";
        return false;
    }
    if (version > VERSION) {
        error = "made by a newer Big Powder (format " + std::to_string(version) + ")";
        return false;
    }
    h.version = version;
    uint32_t t;
    uint64_t len;
    if (!r.get(t) || !r.get(len) || t != T_HEAD || !r.get(h.W) || !r.get(h.H) || !r.get(h.CW) || !r.get(h.CH) ||
        !r.get(h.tick) || !r.get(h.particles) || !r.get(h.seed) || !r.get(h.pmapbits)) {
        error = "the file is damaged (header)";
        return false;
    }
    return true;
}

// The fields that can carry an element (Particle::PossiblyCarriesType): life, ctype, tmp..tmp4, by TPT's field
// numbers (CarriesTypeIn's bits)
int32_t *carried(Particle &p, int field) {
    switch (field) {
    case 1: return &p.life;
    case 2: return &p.ctype;
    case 9: return &p.tmp;
    case 10: return &p.tmp2;
    case 11: return &p.tmp3;
    case 12: return &p.tmp4;
    }
    return nullptr;
}

} // namespace

bool readWorldInfo(const std::string &path, WorldFileInfo &info, std::string &error) {
    auto f = openFile(path, "rb");
    if (!f) {
        error = "cannot open " + path;
        return false;
    }
    Reader r(f.get());
    Header h;
    if (!readStart(f.get(), r, h, error))
        return false;
    info.W = h.W;
    info.H = h.H;
    info.particles = h.particles;
    return true;
}

bool saveWorld(GpuSim &sim, const std::string &path, std::string &error) {
    const ElementTable &elements = sim.elementTable();
    const uint32_t W = sim.width(), H = sim.height(), CW = W / CELL, CH = H / CELL;
    const size_t cells = size_t(CW) * CH;
    SimStats stats = sim.readStats();
    std::string tmp = path + ".part";
    auto f = openFile(tmp, "wb");
    if (!f) {
        error = "cannot write " + path;
        return false;
    }
    std::fwrite(MAGIC, 1, 8, f.get());
    std::fwrite(&VERSION, 4, 1, f.get());
    Writer w(f.get());

    w.section(T_HEAD, 4 * 4 + 8 + 8 + 4 + 4);
    w.put(W), w.put(H), w.put(CW), w.put(CH);
    w.put(uint64_t(sim.tick())), w.put(uint64_t(stats.total));
    w.put(sim.seed()), w.put(PMAPBITS);

    const SimSettings &s = sim.settings();
    const AirSettings &a = sim.air();
    w.section(T_SETT, 4 * 4 + 3 + 4 + 1 + 4 + 5 * 4);
    w.put(s.edgeMode), w.put(s.gravityMode), w.put(s.customGravX), w.put(s.customGravY);
    w.put(uint8_t(s.ambientHeat)), w.put(uint8_t(s.heat)), w.put(uint8_t(s.waterEqualisation));
    w.put(s.convectionMode), w.put(uint8_t(s.newtonianGravity));
    w.put(a.mode), w.put(a.vorticity), w.put(a.edgePressure), w.put(a.edgeVX), w.put(a.edgeVY), w.put(a.ambientTemp);

    {
        std::vector<std::pair<uint32_t, std::string>> palette;
        uint64_t len = 4;
        for (uint32_t id = 1; id < MAX_ELEMENTS; id++)
            if (elements.elements[id].existsInTpt) {
                palette.push_back({id, elements.elements[id].identifier});
                len += 4 + 2 + palette.back().second.size();
            }
        w.section(T_PALT, len);
        w.put(uint32_t(palette.size()));
        for (const auto &[id, name] : palette) {
            w.put(id);
            w.put(uint16_t(name.size()));
            w.write(name.data(), name.size());
        }
    }

    // Particles, a block at a time, each field as one array (they compress far better that way)
    w.section(T_PART, BLOCKS);
    {
        std::vector<uint32_t> ids, flags, dcolour;
        std::vector<uint16_t> types;
        std::vector<int32_t> life, ctype, t1, t2, t3, t4;
        std::vector<float> x, y, vx, vy, temp;
        uint32_t prev = 0;
        auto flush = [&] {
            if (ids.empty())
                return;
            w.put(uint32_t(ids.size()));
            w.array(ids), w.array(types), w.array(life), w.array(ctype), w.array(x), w.array(y), w.array(vx);
            w.array(vy), w.array(temp), w.array(t1), w.array(t2), w.array(t3), w.array(t4), w.array(flags);
            w.array(dcolour);
            for (auto *v : {&ids, &flags, &dcolour})
                v->clear();
            types.clear();
            for (auto *v : {&life, &ctype, &t1, &t2, &t3, &t4})
                v->clear();
            for (auto *v : {&x, &y, &vx, &vy, &temp})
                v->clear();
        };
        sim.forEachParticle([&](uint32_t id, uint32_t type, const Particle &p) {
            ids.push_back(id - prev); // (ids rise, so the gaps compress to almost nothing)
            prev = id;
            types.push_back(uint16_t(type));
            life.push_back(p.life), ctype.push_back(p.ctype), x.push_back(p.x), y.push_back(p.y);
            vx.push_back(p.vx), vy.push_back(p.vy), temp.push_back(p.temp), t1.push_back(p.tmp);
            t2.push_back(p.tmp2), t3.push_back(p.tmp3), t4.push_back(p.tmp4), flags.push_back(p.flags);
            dcolour.push_back(p.dcolour);
            if (ids.size() == PARTICLE_BLOCK)
                flush();
        });
        flush();
        w.put(uint32_t(0));
    }

    {
        std::vector<float> fvx, fvy;
        sim.readFans(fvx, fvy);
        w.section(T_WALL, cells * (1 + 4 + 4));
        w.array(sim.walls()), w.array(fvx), w.array(fvy);
    }
    {
        std::vector<float> pv, vx, vy, hv;
        sim.readAir(pv, vx, vy, hv);
        w.section(T_AIR, cells * 4 * 4);
        w.array(pv), w.array(vx), w.array(vy), w.array(hv);
    }
    {
        std::vector<uint32_t> g = sim.readGravityCells();
        w.section(T_GRAV, g.size() * 4);
        w.array(g);
    }
    {
        uint64_t len = 4;
        for (const Sign &sg : sim.signs())
            len += 4 * 4 + sg.text.size() * 4;
        w.section(T_SIGN, len);
        w.put(uint32_t(sim.signs().size()));
        for (const Sign &sg : sim.signs()) {
            w.put(int32_t(sg.x)), w.put(int32_t(sg.y)), w.put(int32_t(sg.ju)), w.put(uint32_t(sg.text.size()));
            w.write(sg.text.data(), sg.text.size() * 4);
        }
    }
    {
        std::vector<uint8_t> st = sim.readStickmen();
        w.section(T_STKM, st.size());
        w.array(st);
    }
    {
        // The engine's own state, beyond what TPT's saves keep: the world continues exactly where it was
        GpuSim::EngineState e = sim.readEngineState();
        w.section(T_ENGN, 4 + 3 * 8 + e.counters.size() * 4 + e.walls.size() * 4 + e.portals.size());
        w.put(GpuSim::ENGINE_STATE_VERSION);
        w.put(uint64_t(e.counters.size())), w.put(uint64_t(e.walls.size())), w.put(uint64_t(e.portals.size()));
        w.array(e.counters), w.array(e.walls), w.array(e.portals);
    }
    w.section(T_END, 0);
    if (!w.finish() || std::fflush(f.get()) != 0) {
        f.reset();
        std::remove(tmp.c_str());
        error = "could not write " + path;
        return false;
    }
    f.reset();
    if (std::rename(tmp.c_str(), path.c_str()) != 0) {
        error = "could not write " + path;
        return false;
    }
    return true;
}

bool exportTptArea(GpuSim &sim, int x, int y, int w, int h, const std::string &path, std::string &message) {
    // Simulation::Save of the area, as a stamp is made, written by TPT's GameSave
    SaveArea a = gpuSaveArea(sim, x, y, x + w - 1, y + h - 1, true);
    if (w <= 0 || h <= 0 || a.cw > ClassicSim::W / CELL || a.ch > ClassicSim::H / CELL) {
        message = "the area must be inside the world and at most 612x384";
        return false;
    }
    std::vector<char> data;
    std::string err;
    if (!tptWriteSave(a, data, err)) {
        message = err;
        return false;
    }
    auto f = openFile(path, "wb");
    if (!f || std::fwrite(data.data(), 1, data.size(), f.get()) != data.size() || std::fflush(f.get()) != 0) {
        message = "cannot write " + path;
        return false;
    }
    message = "saved " + std::to_string(a.particles.size()) + " particles (" + std::to_string(a.cw * CELL) + "x" +
              std::to_string(a.ch * CELL) + ") as " + path;
    return true;
}

bool loadWorld(GpuSim &sim, const std::string &path, std::string &message) {
    const ElementTable &elements = sim.elementTable();
    auto f = openFile(path, "rb");
    if (!f) {
        message = "cannot open " + path;
        return false;
    }
    Reader r(f.get());
    Header h;
    if (!readStart(f.get(), r, h, message))
        return false;
    if (h.W != sim.width() || h.H != sim.height()) {
        message = path + " is a " + std::to_string(h.W) + "x" + std::to_string(h.H) + " world; this one is " +
                  std::to_string(sim.width()) + "x" + std::to_string(sim.height());
        return false;
    }
    const size_t cells = size_t(h.CW) * h.CH;
    const uint32_t mask = (1u << h.pmapbits) - 1;
    SimSettings settings = sim.settings();
    AirSettings air = sim.air();
    std::vector<int> map(size_t(1) << h.pmapbits, 0); // the file's element numbers to ours
    std::map<std::string, int> missing;
    std::vector<std::string> fileNames(map.size());
    SceneData scene;
    scene.W = h.W;
    scene.H = h.H;
    std::vector<uint8_t> bmap;
    std::vector<float> fvx, fvy, pv, vx, vy, hv;
    std::vector<uint32_t> grav;
    std::vector<uint8_t> stickmen;
    GpuSim::EngineState engine;
    bool haveEngine = false;
    uint64_t dropped = 0, kept = 0;
    bool haveAir = false, haveWalls = false;
    auto damaged = [&](const char *what) {
        message = path + " is damaged (" + what + ")";
        return false;
    };
    for (;;) {
        uint32_t t;
        uint64_t len;
        if (!r.get(t) || !r.get(len))
            return damaged("sections");
        if (t == T_END)
            break;
        if (t == T_SETT) {
            uint8_t ah, heat, we, nw;
            if (!r.get(settings.edgeMode) || !r.get(settings.gravityMode) || !r.get(settings.customGravX) ||
                !r.get(settings.customGravY) || !r.get(ah) || !r.get(heat) || !r.get(we) ||
                !r.get(settings.convectionMode) || !r.get(nw) || !r.get(air.mode) || !r.get(air.vorticity) ||
                !r.get(air.edgePressure) || !r.get(air.edgeVX) || !r.get(air.edgeVY) || !r.get(air.ambientTemp))
                return damaged("settings");
            if (h.version < 2)
                settings.convectionMode += 1; // (format 1: 0 legacy, 1 Boussinesq)
            settings.ambientHeat = ah;
            settings.heat = heat;
            settings.waterEqualisation = we;
            settings.newtonianGravity = nw;
        } else if (t == T_PALT) {
            uint32_t n;
            if (!r.get(n))
                return damaged("palette");
            for (uint32_t k = 0; k < n; k++) {
                uint32_t id;
                uint16_t sz;
                std::string name;
                if (!r.get(id) || !r.get(sz))
                    return damaged("palette");
                name.resize(sz);
                if (!r.read(name.data(), sz))
                    return damaged("palette");
                if (id >= map.size())
                    continue;
                fileNames[id] = name;
                for (int ours = 1; ours < MAX_ELEMENTS; ours++)
                    if (elements.elements[ours].existsInTpt && elements.elements[ours].identifier == name)
                        map[id] = ours;
            }
        } else if (t == T_PART) {
            uint32_t prev = 0;
            for (;;) {
                uint32_t n;
                if (!r.get(n))
                    return damaged("particles");
                if (!n)
                    break;
                std::vector<uint32_t> ids, flags, dcolour;
                std::vector<uint16_t> types;
                std::vector<int32_t> life, ctype, t1, t2, t3, t4;
                std::vector<float> x, y, pvx, pvy, temp;
                if (!r.array(ids, n) || !r.array(types, n) || !r.array(life, n) || !r.array(ctype, n) ||
                    !r.array(x, n) || !r.array(y, n) || !r.array(pvx, n) || !r.array(pvy, n) || !r.array(temp, n) ||
                    !r.array(t1, n) || !r.array(t2, n) || !r.array(t3, n) || !r.array(t4, n) ||
                    !r.array(flags, n) || !r.array(dcolour, n))
                    return damaged("particles");
                for (uint32_t k = 0; k < n; k++) {
                    uint32_t id = prev + ids[k];
                    prev = id;
                    int type = types[k] < map.size() ? map[types[k]] : 0;
                    if (!type || !elements.enabled(type)) {
                        dropped++;
                        if (types[k] < map.size())
                            missing[fileNames[types[k]].empty() ? "#" + std::to_string(types[k]) : fileNames[types[k]]]++;
                        continue;
                    }
                    if (id >= sim.capacity())
                        return damaged("particle ids");
                    if (scene.ptype.size() <= id) {
                        scene.ptype.resize(size_t(id) + 1, 0);
                        scene.parts.resize(size_t(id) + 1, Particle{});
                    }
                    Particle p{};
                    p.life = life[k], p.ctype = ctype[k], p.x = x[k], p.y = y[k], p.vx = pvx[k], p.vy = pvy[k];
                    p.temp = temp[k], p.tmp = t1[k], p.tmp2 = t2[k], p.tmp3 = t3[k], p.tmp4 = t4[k];
                    p.flags = flags[k], p.dcolour = dcolour[k];
                    // GameSave::MapPalette: the fields that carry an element are renumbered too
                    uint32_t carries = elements.elements[type].props.carriesTypeIn;
                    for (int field : {1, 2, 9, 10, 11, 12})
                        if (carries & (1u << field)) {
                            int32_t *v = carried(p, field);
                            uint32_t c = uint32_t(*v) & mask;
                            if (c > 0 && c < map.size())
                                *v = int32_t((uint32_t(*v) & ~mask) | uint32_t(map[c]));
                        }
                    scene.ptype[id] = uint32_t(type);
                    scene.parts[id] = p;
                    kept++;
                }
            }
        } else if (t == T_WALL) {
            if (!r.array(bmap, cells) || !r.array(fvx, cells) || !r.array(fvy, cells))
                return damaged("walls");
            haveWalls = true;
        } else if (t == T_AIR) {
            if (!r.array(pv, cells) || !r.array(vx, cells) || !r.array(vy, cells) || !r.array(hv, cells))
                return damaged("air");
            haveAir = true;
        } else if (t == T_GRAV) {
            if (!r.array(grav, size_t(len / 4)))
                return damaged("gravity");
        } else if (t == T_SIGN) {
            uint32_t n;
            if (!r.get(n))
                return damaged("signs");
            for (uint32_t k = 0; k < n; k++) {
                int32_t sx, sy, ju;
                uint32_t sz;
                if (!r.get(sx) || !r.get(sy) || !r.get(ju) || !r.get(sz) || sz > 4096)
                    return damaged("signs");
                Sign sg{sx, sy, ju, std::u32string(sz, U' ')};
                if (!r.read(sg.text.data(), size_t(sz) * 4))
                    return damaged("signs");
                scene.signs.push_back(sg);
            }
        } else if (t == T_STKM) {
            if (!r.array(stickmen, size_t(len)))
                return damaged("stick men");
        } else if (t == T_ENGN) {
            uint32_t version;
            uint64_t nc, nw, np;
            if (!r.get(version) || !r.get(nc) || !r.get(nw) || !r.get(np))
                return damaged("engine state");
            if (version != GpuSim::ENGINE_STATE_VERSION) {
                if (!r.skip(len - 4 - 3 * 8))
                    return damaged("engine state");
            } else {
                if (!r.array(engine.counters, size_t(nc)) || !r.array(engine.walls, size_t(nw)) ||
                    !r.array(engine.portals, size_t(np)))
                    return damaged("engine state");
                haveEngine = true;
            }
        } else if (len == BLOCKS || !r.skip(len))
            return damaged("an unknown section");
    }

    sim.setAir(air);
    sim.setSettings(settings);
    if (haveWalls)
        scene.walls = bmap;
    sim.load(scene);
    if (haveWalls)
        sim.setWalls(bmap, &fvx, &fvy);
    if (haveAir)
        sim.writeAir(pv, vx, vy, hv);
    sim.writeGravityCells(grav);
    sim.writeStickmen(stickmen);
    if (haveEngine)
        sim.writeEngineState(engine);
    sim.setTick(h.tick);
    sim.setSeed(h.seed);
    message = "loaded " + path + ": " + std::to_string(kept) + " particles";
    if (dropped) {
        message += "; left out " + std::to_string(dropped) + " of elements this build doesn't run:";
        for (const auto &[name, n] : missing)
            message += " " + name + " " + std::to_string(n);
    }
    return true;
}

} // namespace bp
