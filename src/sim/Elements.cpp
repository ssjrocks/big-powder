#include "sim/Elements.h"

#include "classic/ClassicSim.h"

#include <set>
#include <string>

namespace bp {

namespace {

// Elements whose whole behaviour the GPU engine has. Everything else stays disabled on the GPU (saves that use
// them still load in Classic mode, and GPU worlds report what they left out).
//
// No element code of their own (only properties), so the generic engine covers them: movement, heat,
// transitions, burning and explosions. Their Graphics functions (drawing only) are not ported yet.
// Not yet: SALT (dissolves into SLTW, in WATR's code) and THRM (burns into LAVA, in FIRE's code) wait for the
// elements they react into.
const char *const PROPERTY_ONLY[] = {
    "BGLA", "BRCK", "DESL", "DMND", "DRIC", "DUST", "DYST", "GAS", "GUNP", "INSL", "LNTG", "LO2", "LRBD",
    "METL", "MWAX", "NICE", "NITR", "OIL",  "PLEX", "PSTE", "PSTS", "RBDM", "SAND", "SAWD", "SMKE", "STNE",
    "WAX",
};
// Element code ported to shaders/elements.slang.
const char *const PORTED[] = {
    "WATR", "FIRE",
};

} // namespace

ElementTable ElementTable::build() {
    ElementTable t;
    std::vector<uint8_t> canMove;
    std::vector<ClassicElement> tpt = ClassicSim::exportElements(canMove, MAX_ELEMENTS);

    std::set<std::string> gpu;
    for (const char *n : PROPERTY_ONLY)
        gpu.insert(n);
    for (const char *n : PORTED)
        gpu.insert(n);

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
        p.enabled = (e.enabled && gpu.count(e.name)) ? 1 : 0;

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
