#include "sim/GpuSave.h"
#include "sim/ElementIds.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <unordered_map>

namespace bp {

namespace {

// TPT's playerst as the GPU keeps it (Stickman in shaders/common.slang)
struct Stickman {
    uint32_t comm, pcomm;
    int32_t elem;
    uint32_t spwn, frames, rocketBoots, fan, spawnRef, partRef, pad0, pad1, pad2;
    float legs[16], accs[8];
};
static_assert(sizeof(Stickman) == STICKMAN_BYTES);
constexpr uint32_t STKM_IN_PORTAL = 0xFFFFFFFFu, STKM_LEAKED = 0xFFFFFFFEu;
constexpr uint32_t FIGHTERS = 100;      // MAX_FIGHTERS
constexpr int SPC_AIR = 256, OLD_SPC_AIR = 236; // (SimulationData.h)
// insert.slang's PASTE_*
constexpr uint32_t PASTE_PLAYER = 1, PASTE_SPAWN = 2, PASTE_FIGHTER = 3;

std::vector<Stickman> stickmenOf(GpuSim &sim) {
    std::vector<uint8_t> raw = sim.readStickmen();
    std::vector<Stickman> out(STICKMEN);
    std::memcpy(out.data(), raw.data(), std::min(raw.size(), out.size() * sizeof(Stickman)));
    return out;
}

} // namespace

SaveArea gpuSaveArea(GpuSim &sim, int x1, int y1, int x2, int y2, bool includePressure) {
    const int W = int(sim.width()), H = int(sim.height()), CW = W / CELL;
    // SaneSaveRect, then Save's blocks: the cells the area touches
    x1 = std::clamp(x1, 0, W - 1), x2 = std::clamp(x2, 0, W - 1);
    y1 = std::clamp(y1, 0, H - 1), y2 = std::clamp(y2, 0, H - 1);
    const int tlx = std::min(x1, x2), tly = std::min(y1, y2), brx = std::max(x1, x2), bry = std::max(y1, y2);
    const int bx0 = tlx / CELL, by0 = tly / CELL, bx1 = brx / CELL, by1 = bry / CELL;
    SaveArea a;
    a.setSize(bx1 - bx0 + 1, by1 - by0 + 1);
    const float ox = float(bx0 * CELL), oy = float(by0 * CELL);

    // The particles in id order, and which id each was (Save's particleMap, for SOAP)
    std::unordered_map<uint32_t, uint32_t> particleMap;
    bool soap = false;
    sim.forEachParticle([&](uint32_t id, uint32_t type, const Particle &p) {
        const int x = int(p.x + 0.5f), y = int(p.y + 0.5f);
        if (x < tlx || x > brx || y < tly || y > bry)
            return;
        particleMap.emplace(id, uint32_t(a.particles.size()));
        a.particles.push_back({int(type), p.life, p.ctype, p.x - ox, p.y - oy, p.vx, p.vy, p.temp, p.tmp, p.tmp2,
                               p.tmp3, p.tmp4, p.flags, p.dcolour});
        soap = soap || type == PT_SOAP;
    });
    if (soap)
        for (ClassicParticle &p : a.particles) {
            if (p.type != PT_SOAP)
                continue;
            if ((p.ctype & 2) == 2) {
                auto n = particleMap.find(uint32_t(p.tmp));
                if (n != particleMap.end())
                    p.tmp = int(n->second);
                else
                    p.tmp = 0, p.ctype ^= 2;
            }
            if ((p.ctype & 4) == 4) {
                auto n = particleMap.find(uint32_t(p.tmp2));
                if (n != particleMap.end())
                    p.tmp2 = int(n->second);
                else
                    p.tmp2 = 0, p.ctype ^= 4;
            }
        }

    for (const Sign &s : sim.signs())
        if (!s.text.empty() && s.x >= tlx && s.x <= brx && s.y >= tly && s.y <= bry)
            a.signs.push_back(Sign{s.x - int(ox), s.y - int(oy), s.ju, s.text});

    const bool grav = sim.settings().newtonianGravity;
    const std::vector<uint32_t> words = sim.readWallWords();
    std::vector<float> fvx, fvy, pv, vx, vy, hv;
    sim.readFans(fvx, fvy);
    if (includePressure)
        sim.readAir(pv, vx, vy, hv);
    std::vector<uint32_t> gravCells;
    if (grav)
        gravCells = sim.readGravityCells();
    const size_t worldCells = size_t(CW) * (H / CELL);
    auto gravPlane = [&](int plane, size_t c) { return gravCells[size_t(plane) * worldCells + c]; };
    for (int y = 0; y < a.ch; y++)
        for (int x = 0; x < a.cw; x++) {
            const size_t from = size_t(by0 + y) * CW + size_t(bx0 + x), to = size_t(y) * a.cw + size_t(x);
            const uint32_t w = words[from];
            if (w & 0xFF) {
                a.walls[to] = uint8_t(w & 0xFF);
                a.fanVX[to] = fvx[from];
                a.fanVY[to] = fvy[from];
            }
            if (includePressure) {
                a.pv[to] = pv[from], a.vx[to] = vx[from], a.vy[to] = vy[from], a.hv[to] = hv[from];
                a.blockAir[to] = uint8_t((w >> 24) & 1);
                a.blockAirH[to] = uint8_t((w >> 16) & 0xFF);
            }
            if (grav) { // (gravity.slang's planes: mass, mask, force x, force y)
                float m, fx, fy;
                uint32_t mb = gravPlane(0, from), xb = gravPlane(2, from), yb = gravPlane(3, from);
                std::memcpy(&m, &mb, 4), std::memcpy(&fx, &xb, 4), std::memcpy(&fy, &yb, 4);
                a.gravMass[to] = m;
                a.gravMask[to] = gravPlane(1, from) ? 0xFFFFFFFFu : 0u;
                a.gravForceX[to] = fx, a.gravForceY[to] = fy;
            }
        }
    a.hasBlockAirMaps = includePressure;
    a.hasGravityMaps = grav;
    a.hasPressure = a.hasAmbientHeat = includePressure;

    const std::vector<Stickman> st = stickmenOf(sim);
    a.rocketBoots1 = st[0].rocketBoots != 0, a.rocketBoots2 = st[1].rocketBoots != 0;
    a.fan1 = st[0].fan != 0, a.fan2 = st[1].fan != 0;
    for (uint32_t i = 0; i < FIGHTERS; i++) {
        if (st[2 + i].rocketBoots)
            a.rocketBootsFigh.push_back(i);
        if (st[2 + i].fan)
            a.fanFigh.push_back(i);
    }

    // SaveSimOptions
    const SimSettings &ss = sim.settings();
    const AirSettings &air = sim.air();
    a.options.heat = ss.heat;
    a.options.ambientHeat = ss.ambientHeat;
    a.options.newtonianGravity = ss.newtonianGravity;
    a.options.waterEqualisation = ss.waterEqualisation;
    a.options.edgeMode = int(ss.edgeMode);
    a.options.gravityMode = int(ss.gravityMode);
    a.options.airMode = int(air.mode);
    a.customGravX = ss.customGravX, a.customGravY = ss.customGravY;
    a.ambientTemp = air.ambientTemp, a.vorticity = air.vorticity;
    a.edgePressure = air.edgePressure, a.edgeVX = air.edgeVX, a.edgeVY = air.edgeVY;
    a.convectionMode = int(ss.convectionMode);
    return a;
}

PasteResult gpuLoadArea(GpuSim &sim, const SaveArea &s, bool includePressure, int bx, int by) {
    PasteResult result;
    const ElementTable &elements = sim.elementTable();
    const int W = int(sim.width()), H = int(sim.height()), CW = W / CELL, CH = H / CELL;
    const int px = bx * CELL, py = by * CELL;
    // The paste's box (what was there is removed only inside it)
    const int ax0 = std::max(0, px), ay0 = std::max(0, py);
    const int ax1 = std::min(W, px + s.cw * CELL), ay1 = std::min(H, py + s.ch * CELL);
    const int aw = std::max(0, ax1 - ax0), ah = std::max(0, ay1 - ay0);
    std::vector<uint32_t> spots((size_t(aw) * ah + 31) / 32, 0);

    // The stick men as Load finds them: which players and spawn points are out, which fighter slots are taken, and
    // where each one's particle is (so that pasting over it removes it)
    std::vector<Stickman> st = stickmenOf(sim);
    bool playerOut[2] = {false, false}, spawnOut[2] = {false, false};
    bool fighterUsed[FIGHTERS] = {};
    uint32_t fighcount = 0;
    struct Special {
        uint32_t kind, slot;
    };
    std::unordered_map<uint64_t, std::vector<Special>> specials; // by spot
    auto spotKey = [](int x, int y) { return uint64_t(uint32_t(x)) << 32 | uint32_t(y); };
    auto note = [&](uint32_t ref, uint32_t type, uint32_t kind, uint32_t slot, int fighterTmp) {
        if (ref == 0 || ref == STKM_IN_PORTAL || ref == STKM_LEAKED)
            return ref != 0;
        Particle p;
        if (sim.readParticleById(ref - 1, p) != type || (fighterTmp >= 0 && p.tmp != fighterTmp))
            return false;
        specials[spotKey(int(p.x + 0.5f), int(p.y + 0.5f))].push_back({kind, slot});
        return true;
    };
    for (uint32_t k = 0; k < 2; k++) {
        if (st[k].spwn)
            playerOut[k] = note(st[k].partRef, k == 0 ? PT_STKM : PT_STKM2, PASTE_PLAYER, k, -1);
        spawnOut[k] = note(st[k].spawnRef, k == 0 ? PT_SPAWN : PT_SPAWN2, PASTE_SPAWN, k, -1);
    }
    for (uint32_t f = 0; f < FIGHTERS; f++)
        if (st[2 + f].spwn && note(st[2 + f].partRef, PT_FIGH, PASTE_FIGHTER, f, int(f)))
            fighterUsed[f] = true, fighcount++;
    auto allocFighter = [&]() -> int { // Element_FIGH_Alloc
        if (fighcount >= FIGHTERS)
            return -1;
        for (uint32_t i = 0; i < FIGHTERS; i++)
            if (!fighterUsed[i]) {
                fighterUsed[i] = true;
                fighcount++;
                return int(i);
            }
        return -1;
    };
    const bool oldStickmen = s.version[0] < 93, olderStickmen = s.version[0] < 88; // (Version(93, 0), (88, 0))
    auto airStickman = [&](int ctype) {
        return (oldStickmen && ctype == SPC_AIR) || (olderStickmen && ctype == OLD_SPC_AIR);
    };

    std::vector<Particle> place;
    std::vector<uint32_t> fromIndex; // each placed one's index in the save (soapList)
    for (size_t n = 0; n < s.particles.size(); n++) {
        const ClassicParticle &c = s.particles[n];
        const int t = c.type;
        if (t <= 0 || t >= MAX_ELEMENTS)
            continue;
        const float fx = c.x + float(px), fy = c.y + float(py);
        const int x = int(std::floor(fx + 0.5f)), y = int(std::floor(fy + 0.5f));
        if (x < 0 || y < 0 || x >= W || y >= H)
            continue;
        // Ensure we can spawn this element (and the stick men's CreateAllowed)
        if ((playerOut[0] && t == PT_STKM) || (playerOut[1] && t == PT_STKM2))
            continue;
        if ((t == PT_SPAWN && spawnOut[0]) || (t == PT_SPAWN2 && spawnOut[1]))
            continue;
        if (t == PT_FIGH && fighcount >= FIGHTERS)
            continue;
        if (!elements.elements[size_t(t)].existsInTpt)
            continue;
        if (!elements.enabled(t)) {
            result.leftOut[t]++;
            continue;
        }
        // removeExistingParticles
        {
            const uint32_t bit = uint32_t(y - ay0) * uint32_t(aw) + uint32_t(x - ax0);
            if (!(spots[bit >> 5] & (1u << (bit & 31)))) {
                spots[bit >> 5] |= 1u << (bit & 31);
                auto it = specials.find(spotKey(x, y));
                if (it != specials.end()) {
                    for (const Special &sp : it->second) { // (their ChangeType to nothing, from kill_part)
                        if (sp.kind == PASTE_PLAYER)
                            playerOut[sp.slot] = false;
                        else if (sp.kind == PASTE_SPAWN)
                            spawnOut[sp.slot] = false;
                        else if (fighterUsed[sp.slot])
                            fighterUsed[sp.slot] = false, fighcount--;
                    }
                    specials.erase(it);
                }
            }
        }
        Particle e{};
        e.life = c.life, e.ctype = c.ctype, e.x = fx, e.y = fy, e.vx = c.vx, e.vy = c.vy, e.temp = c.temp;
        e.tmp = c.tmp, e.tmp2 = c.tmp2, e.tmp3 = c.tmp3, e.tmp4 = c.tmp4, e.flags = c.flags, e.dcolour = c.dcolour;
        e.pad1 = uint32_t(t);
        e.pass = 0;
        // create_part(-3, ...) with its Create and ChangeType, then what Load does for each kind
        if (t == PT_STKM || t == PT_STKM2) {
            const uint32_t k = t == PT_STKM ? 0 : 1;
            const bool makeSpawn = !spawnOut[k]; // STKM's Create (SPAWN's CreateAllowed: no spawn point yet)
            spawnOut[k] = true;
            playerOut[k] = true;
            const bool fan = airStickman(c.ctype) || (k == 0 ? s.fan1 : s.fan2);
            const bool boots = k == 0 ? s.rocketBoots1 : s.rocketBoots2;
            e.pass = PASTE_PLAYER | k << 2 | uint32_t(makeSpawn) << 3 | uint32_t(boots) << 4 | uint32_t(fan) << 5;
        } else if (t == PT_SPAWN || t == PT_SPAWN2) {
            const uint32_t k = t == PT_SPAWN ? 0 : 1;
            spawnOut[k] = true;
            e.pass = PASTE_SPAWN | k << 2;
        } else if (t == PT_FIGH) {
            const int lost = allocFighter(); // FIGH's ChangeType takes a slot, which Load's own then replaces and
            const int slot = allocFighter(); // which stays taken
            if (slot < 0)
                continue; // (Load sets the particle's type to none)
            bool fan = false;
            if (airStickman(e.ctype)) {
                fan = true;
                e.ctype = 0;
            }
            const uint32_t oldTmp = uint32_t(c.tmp);
            bool boots = std::find(s.rocketBootsFigh.begin(), s.rocketBootsFigh.end(), oldTmp) != s.rocketBootsFigh.end();
            fan = fan || std::find(s.fanFigh.begin(), s.fanFigh.end(), oldTmp) != s.fanFigh.end();
            const int elem = e.ctype > 0 && e.ctype < MAX_ELEMENTS ? e.ctype : int(PT_DUST); // NewFighter
            e.tmp = slot;
            e.pass = PASTE_FIGHTER | uint32_t(boots) << 4 | uint32_t(fan) << 5 | uint32_t(elem) << 16;
            if (lost >= 0)
                e.pass |= 64u | uint32_t(lost) << 8;
        }
        if (tptPressureInTmp3(t) && !includePressure)
            e.tmp3 = 0;
        place.push_back(e);
        fromIndex.push_back(uint32_t(n));
    }

    sim.killAtSpots(ax0, ay0, aw, ah, spots);
    const std::vector<uint32_t> ids = sim.placeParticles(place);
    for (uint32_t id : ids)
        result.placed += id != 0xFFFFFFFFu;

    // SOAP's links: from a particle's index in the save to its new id (a link not found is left as it is)
    {
        std::unordered_map<uint32_t, uint32_t> soapList;
        for (size_t i = 0; i < place.size(); i++)
            if (place[i].pad1 == PT_SOAP && ids[i] != 0xFFFFFFFFu)
                soapList.emplace(fromIndex[i], ids[i]);
        std::vector<std::array<int32_t, 3>> links;
        for (size_t i = 0; i < place.size(); i++) {
            if (place[i].pad1 != PT_SOAP || ids[i] == 0xFFFFFFFFu)
                continue;
            int32_t tmp = place[i].tmp, tmp2 = place[i].tmp2;
            if ((place[i].ctype & 0x2) == 2)
                if (auto it = soapList.find(uint32_t(tmp)); it != soapList.end())
                    tmp = int32_t(it->second);
            if ((place[i].ctype & 0x4) == 4)
                if (auto it = soapList.find(uint32_t(tmp2)); it != soapList.end())
                    tmp2 = int32_t(it->second);
            if (tmp != place[i].tmp || tmp2 != place[i].tmp2)
                links.push_back({int32_t(ids[i]), tmp, tmp2});
        }
        sim.setLinks(links);
    }

    // Signs, moved with it, while there is room
    {
        std::vector<Sign> signs = sim.signs();
        for (size_t i = 0; i < s.signs.size() && signs.size() < GpuSim::SIGN_MAX; i++) {
            Sign sg = s.signs[i];
            if (sg.text.empty())
                continue;
            sg.x += px, sg.y += py;
            if (sg.x < 0 || sg.y < 0 || sg.x >= W || sg.y >= H)
                continue;
            signs.push_back(sg);
        }
        if (signs.size() != sim.signs().size())
            sim.setSigns(signs);
    }

    // The cells it brings: walls (where it has one) with their fans, the air with pressure, gravity's maps with
    // Newtonian gravity on
    const int tx0 = std::max(0, bx + s.contentX), ty0 = std::max(0, by + s.contentY);
    const int tx1 = std::min(CW, bx + s.contentX + s.contentW), ty1 = std::min(CH, by + s.contentY + s.contentH);
    if (tx0 >= tx1 || ty0 >= ty1) {
        sim.pasted();
        return result;
    }
    const bool useGravityMaps = s.hasGravityMaps && sim.settings().newtonianGravity;
    std::vector<uint32_t> words = sim.readWallWords();
    std::vector<float> fvx, fvy, pv, vx, vy, hv;
    sim.readFans(fvx, fvy);
    if (includePressure)
        sim.readAir(pv, vx, vy, hv);
    std::vector<uint32_t> gravCells;
    if (useGravityMaps)
        gravCells = sim.readGravityCells();
    const size_t worldCells = size_t(CW) * CH;
    for (int y = ty0; y < ty1; y++)
        for (int x = tx0; x < tx1; x++) {
            const size_t to = size_t(y) * CW + size_t(x), from = size_t(y - by) * s.cw + size_t(x - bx);
            uint32_t &w = words[to];
            if (s.walls[from]) {
                w = (w & ~0xFFu) | s.walls[from];
                fvx[to] = s.fanVX[from];
                fvy[to] = s.fanVY[from];
            }
            if (includePressure) {
                if (s.hasPressure)
                    pv[to] = s.pv[from], vx[to] = s.vx[from], vy[to] = s.vy[from];
                if (s.hasAmbientHeat)
                    hv[to] = s.hv[from];
                if (s.hasBlockAirMaps)
                    w = (w & ~(0x01FF0000u)) | uint32_t(s.blockAirH[from]) << 16 | uint32_t(s.blockAir[from] & 1) << 24;
            }
            if (!includePressure || !s.hasBlockAirMaps) {
                // Air::ApproximateBlockAirMaps, its walls' part (the particles' is worked out again next tick)
                const uint32_t b = w & 0xFF, e = (w >> 8) & 0xFF;
                const bool blockair = b == WL_WALL || b == WL_WALLELEC || b == WL_BLOCKAIR || (b == WL_EWALL && !e);
                const uint32_t blockairh = (blockair || b == WL_GRAV) ? 8u : 0u;
                w = (w & ~(0x01FF0000u)) | blockairh << 16 | uint32_t(blockair) << 24;
            }
            if (useGravityMaps) { // (the field it brings; the mass and mask are worked out again next tick)
                uint32_t fx, fy;
                std::memcpy(&fx, &s.gravForceX[from], 4);
                std::memcpy(&fy, &s.gravForceY[from], 4);
                gravCells[2 * worldCells + to] = fx;
                gravCells[3 * worldCells + to] = fy;
            }
        }
    sim.writeWallWords(words);
    sim.writeFans(fvx, fvy);
    if (includePressure)
        sim.writeAir(pv, vx, vy, hv);
    if (useGravityMaps)
        sim.writeGravityCells(gravCells);
    sim.pasted();
    return result;
}

void gpuClearArea(GpuSim &sim, int x, int y, int w, int h) {
    const int W = int(sim.width()), H = int(sim.height()), CW = W / CELL;
    // (the area inside the world)
    const int x0 = std::max(0, x), y0 = std::max(0, y), x1 = std::min(W, x + w), y1 = std::min(H, y + h);
    if (x1 <= x0 || y1 <= y0)
        return;
    x = x0, y = y0, w = x1 - x0, h = y1 - y0;
    const float fx = float(x) - .5f, fy = float(y) - .5f;
    sim.killInBox(fx, fy, fx + float(w) + 1, fy + float(h) + 1);
    std::vector<uint32_t> words = sim.readWallWords();
    const int cx1 = x / CELL, cy1 = y / CELL, cx2 = (x + w) / CELL, cy2 = (y + h) / CELL;
    for (int cy = cy1; cy <= cy2 && cy < H / CELL; cy++)
        for (int cx = cx1; cx <= cx2 && cx < CW; cx++)
            words[size_t(cy) * CW + size_t(cx)] &= ~0xFFFFu; // the wall and its electricity
    sim.writeWallWords(words);
    std::vector<Sign> signs = sim.signs();
    std::erase_if(signs, [&](const Sign &s) {
        return !s.text.empty() && s.x >= x && s.y >= y && s.x <= x + w && s.y <= y + h;
    });
    if (signs.size() != sim.signs().size())
        sim.setSigns(signs);
}

} // namespace bp
