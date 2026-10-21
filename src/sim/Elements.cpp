#include "sim/Elements.h"

#include "classic/ClassicSim.h"

#include <iterator>
#include <set>
#include <stdexcept>
#include <string>

namespace bp {

namespace {

// Elements whose whole behaviour the GPU engine has. Everything else stays disabled on the GPU (saves that use
// them still load in Classic mode, and GPU worlds report what they left out).
//
// No element code of their own (only properties), so the generic engine covers them: movement, heat,
// transitions, burning and explosions. Their Graphics functions (drawing only) are not ported yet.
const int PROPERTY_ONLY[] = {
    PT_BGLA, PT_BRCK, PT_CFLM, PT_DESL, PT_DMND, PT_DRIC, PT_DUST, PT_DYST, PT_GAS,  PT_GUNP, PT_INSL, PT_LNTG,
    PT_LO2,  PT_LRBD, PT_METL, PT_MWAX, PT_NICE, PT_NITR, PT_OIL,  PT_PLEX, PT_PSTE, PT_PSTS, PT_RBDM, PT_ROCK,
    PT_SALT, PT_SAND, PT_SAWD, PT_SMKE, PT_STNE, PT_WAX,  PT_THRM, PT_CNCT,
};
// Element code ported to shaders/elements.slang (Create functions in runElementCreate).
const int PORTED[] = {
    PT_WATR, PT_FIRE,
    // Water family
    PT_DSTW, PT_SLTW, PT_WTRV, PT_ICEI, PT_SNOW, PT_RIME, PT_FOG,
    // Plants and their partners
    PT_SEED, PT_PLNT, PT_VINE, PT_WOOD, PT_SPNG, PT_GOO, PT_O2, PT_CO2, PT_COAL, PT_BCOL,
    // Lava, melting and metals
    PT_LAVA, PT_GLAS, PT_BMTL, PT_BRMT, PT_IRON, PT_GOLD, PT_TTAN, PT_CLST, PT_CRMC,
    // Chemistry and gases (H2's and NBLE's fusion waits for NEUT and PHOT)
    PT_ACID, PT_CAUS, PT_CBNW, PT_BASE, PT_H2, PT_BOYL, PT_RFRG, PT_RFGL, PT_PLSM, PT_NBLE,
    // More liquids and powders
    PT_GEL, PT_MERC, PT_FRZW, PT_FRZZ, PT_YEST, PT_ANAR, PT_QRTZ, PT_PQRT, PT_SLCN, PT_GLOW, PT_RSST, PT_RSSS,
    PT_VIRS, PT_VRSS, PT_VRSG,
    // Electronics (conductors without code of their own are driven by SPRK)
    PT_SPRK, PT_PSCN, PT_NSCN, PT_INWR, PT_INST, PT_NTCT, PT_PTCT, PT_BTRY, PT_SWCH, PT_WIRE,
    // Sensors and explosives
    PT_DTEC, PT_TSNS, PT_PSNS, PT_LSNS, PT_VSNS, PT_LDTC, PT_FUSE, PT_FSEP, PT_IGNT, PT_BANG, PT_BOMB, PT_EMBR,
    PT_C5, PT_DEST, PT_FWRK, PT_FIRW, PT_THDR,
    // Cloners, voids, holes (NBHL/NWHL's gravity waits for Newtonian gravity), converter, shields, powered
    PT_CLNE, PT_BCLN, PT_PCLN, PT_PBCN, PT_VOID, PT_PVOD, PT_BHOL, PT_WHOL, PT_NBHL, PT_NWHL, PT_CONV,
    PT_SHLD1, PT_SHLD2, PT_SHLD3, PT_SHLD4, PT_PUMP, PT_HSWC, PT_DLAY,
    // Light and nuclear (GRVT's gravity waits for Newtonian gravity)
    PT_PHOT, PT_NEUT, PT_ELEC, PT_PROT, PT_GRVT, PT_PLUT, PT_URAN, PT_DEUT, PT_ISOZ, PT_ISZS, PT_POLO,
    // Exotic matter and more light (GPMP's gravity waits for Newtonian gravity)
    PT_AMTR, PT_SING, PT_WARP, PT_EXOT, PT_VIBR, PT_BVBR, PT_FILT, PT_BIZR, PT_BIZRG, PT_BIZRS, PT_INVIS, PT_LCRY,
    PT_GPMP, PT_TUNG,
    // Force elements and more (GBMB's gravity waits for Newtonian gravity)
    PT_ACEL, PT_DCEL, PT_RPEL, PT_FRAY, PT_GBMB, PT_DMG, PT_BREC, PT_HEAC, PT_STOR,
    // Beams, wireless and EMP (the beams are cast after the passes, rays.slang; EMP's effect, emp.slang)
    PT_ARAY, PT_BRAY, PT_CRAY, PT_DRAY, PT_WIFI, PT_EMP,
};

} // namespace

ElementTable ElementTable::build() {
    ElementTable t;
    std::vector<uint8_t> canMove;
    std::vector<ClassicElement> tpt = ClassicSim::exportElements(canMove, MAX_ELEMENTS);

    // The PT_* ids (generated from TPT's element list) must be the ids the TPT build uses.
    for (int id = 1; id < MAX_ELEMENTS; id++) {
        std::string ours = id < PT_COUNT ? kElementIdNames[id] : "";
        std::string theirs = id < int(tpt.size()) ? tpt[id].constName : "";
        if (ours != theirs)
            throw std::runtime_error("element id " + std::to_string(id) + " is " + (ours.empty() ? "unused" : ours) +
                                     " here but " + (theirs.empty() ? "unused" : theirs) +
                                     " in TPT: run tools/gen-element-ids.py");
    }

    std::set<int> gpu(std::begin(PROPERTY_ONLY), std::end(PROPERTY_ONLY));
    gpu.insert(std::begin(PORTED), std::end(PORTED));

    t.defaults.assign(MAX_ELEMENTS, Particle{});
    for (int id = 0; id < MAX_ELEMENTS; id++) {
        const ClassicElement &e = tpt[id];
        ElementInfo &info = t.elements[id];
        info.name = e.name;
        info.description = e.description;
        info.existsInTpt = e.enabled;
        info.menuSection = e.menuSection;
        info.menuVisible = e.menuVisible != 0;
        ElementProps &p = info.props;
        p = ElementProps{};
        p.advection = e.advection;
        p.airDrag = e.airDrag;
        p.airLoss = e.airLoss;
        p.loss = e.loss;
        p.collision = e.collision;
        p.gravity = e.gravity;
        p.diffusion = e.diffusion;
        p.hotAir = e.hotAir;
        p.falldown = e.falldown;
        p.flammable = e.flammable;
        p.explosive = e.explosive;
        p.meltable = e.meltable;
        p.hardness = e.hardness;
        p.weight = e.weight;
        p.heatConduct = e.heatConduct;
        p.properties = e.properties;
        p.lowPressure = e.lowPressure;
        p.highPressure = e.highPressure;
        p.lowTemperature = e.lowTemperature;
        p.highTemperature = e.highTemperature;
        p.lowPressureT = e.lowPressureT;
        p.highPressureT = e.highPressureT;
        p.lowTemperatureT = e.lowTemperatureT;
        p.highTemperatureT = e.highTemperatureT;
        p.colour = e.colour;
        p.defaultTemp = e.defaults.temp;
        p.heatCapacity = e.heatCapacity;
        p.newtonianGravity = e.newtonianGravity;
        p.photonReflectWavelengths = e.photonReflectWavelengths;
        p.carriesTypeIn = e.carriesTypeIn;
        p.enabled = (e.enabled && gpu.count(id)) ? 1 : 0;
        p.existsInTpt = e.enabled ? 1 : 0;

        Particle &d = t.defaults[id];
        d.life = e.defaults.life;
        d.ctype = e.defaults.ctype;
        d.temp = e.defaults.temp;
        d.tmp = e.defaults.tmp;
        d.tmp2 = e.defaults.tmp2;
        d.tmp3 = e.defaults.tmp3;
        d.tmp4 = e.defaults.tmp4;
        d.flags = e.defaults.flags;
        d.dcolour = e.defaults.dcolour;
    }

    // A transition into an element the GPU engine doesn't have yet would make a particle it can't simulate.
    // Those transitions are switched off on the GPU until the target element is ported.
    auto fix = [&](int32_t &target) {
        if (target >= 0 && !t.enabled(target))
            target = NT;
    };
    for (auto &e : t.elements) {
        fix(e.props.lowPressureT);
        fix(e.props.highPressureT);
        fix(e.props.lowTemperatureT);
        fix(e.props.highTemperatureT);
    }

    t.canMove.assign(canMove.begin(), canMove.end());
    return t;
}

int ElementTable::byName(const std::string &name) const {
    for (int id = 1; id < MAX_ELEMENTS; id++)
        if (elements[id].existsInTpt && elements[id].name == name)
            return id;
    return -1;
}

} // namespace bp
