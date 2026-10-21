#include "sim/Elements.h"

namespace bp {

namespace {

constexpr float CFDS = 4.0f / CELL;
constexpr float R_TEMP = 22.0f;
constexpr float MIN_TEMP = 0.0f;
constexpr float MAX_TEMP = 9999.0f;
constexpr float MIN_PRESSURE = -256.0f;
constexpr float MAX_PRESSURE = 256.0f;
constexpr float IPL = MIN_PRESSURE - 1;
constexpr float IPH = MAX_PRESSURE + 1;
constexpr float ITL = MIN_TEMP - 1;
constexpr float ITH = MAX_TEMP + 1;

// The defaults of TPT's Element constructor.
ElementProps defaults(uint32_t colour) {
    ElementProps p{};
    p.colour = colour;
    p.lowPressure = IPL;
    p.highPressure = IPH;
    p.lowTemperature = ITL;
    p.highTemperature = ITH;
    p.lowPressureT = p.highPressureT = p.lowTemperatureT = p.highTemperatureT = NT;
    p.defaultTemp = R_TEMP + 273.15f;
    p.heatCapacity = 1.0f;
    p.enabled = 1;
    return p;
}

} // namespace

ElementTable ElementTable::build() {
    ElementTable t;
    for (auto &e : t.elements) {
        e.props = defaults(0);
        e.props.enabled = 0;
    }
    auto def = [&](int id, const char *name, ElementProps p) { t.elements[id] = {name, p}; };

    // Values copied from TPT v100.1.400 src/simulation/elements/<NAME>.cpp.
    {
        ElementProps p = defaults(0xFFE0A0);
        p.advection = 0.7f; p.airDrag = 0.02f * CFDS; p.airLoss = 0.96f; p.loss = 0.80f;
        p.collision = 0.0f; p.gravity = 0.1f; p.diffusion = 0.0f; p.hotAir = 0.0f * CFDS;
        p.falldown = 1; p.flammable = 10; p.explosive = 0; p.meltable = 0; p.hardness = 30;
        p.weight = 85; p.heatConduct = 70;
        p.properties = TYPE_PART;
        def(PT_DUST, "DUST", p);
    }
    {
        ElementProps p = defaults(0x2030D0);
        p.advection = 0.6f; p.airDrag = 0.01f * CFDS; p.airLoss = 0.98f; p.loss = 0.95f;
        p.collision = 0.0f; p.gravity = 0.1f; p.diffusion = 0.0f; p.hotAir = 0.0f * CFDS;
        p.falldown = 2; p.flammable = 0; p.explosive = 0; p.meltable = 0; p.hardness = 20;
        p.weight = 30; p.defaultTemp = R_TEMP - 2.0f + 273.15f; p.heatConduct = 29;
        p.properties = TYPE_LIQUID | PROP_CONDUCTS | PROP_LIFE_DEC | PROP_NEUTPASS | PROP_PHOTPASS;
        p.lowTemperature = 273.15f; p.lowTemperatureT = PT_ICEI;
        p.highTemperature = 373.0f; p.highTemperatureT = PT_WTRV;
        def(PT_WATR, "WATR", p);
    }
    {
        ElementProps p = defaults(0x404010);
        p.advection = 0.6f; p.airDrag = 0.01f * CFDS; p.airLoss = 0.98f; p.loss = 0.95f;
        p.collision = 0.0f; p.gravity = 0.1f; p.diffusion = 0.0f; p.hotAir = 0.0f * CFDS;
        p.falldown = 2; p.flammable = 20; p.explosive = 0; p.meltable = 0; p.hardness = 5;
        p.weight = 20; p.heatConduct = 42;
        p.properties = TYPE_LIQUID | PROP_NEUTPASS;
        p.highTemperature = 333.0f; p.highTemperatureT = PT_GAS;
        def(PT_OIL, "OIL", p);
    }
    {
        ElementProps p = defaults(0xFF1000);
        p.advection = 0.9f; p.airDrag = 0.04f * CFDS; p.airLoss = 0.97f; p.loss = 0.20f;
        p.collision = 0.0f; p.gravity = -0.1f; p.diffusion = 0.0f; p.hotAir = 0.001f * CFDS;
        p.falldown = 1; p.flammable = 0; p.explosive = 0; p.meltable = 0; p.hardness = 1;
        p.weight = 2; p.defaultTemp = R_TEMP + 400.0f + 273.15f; p.heatConduct = 88;
        p.properties = TYPE_GAS | PROP_LIFE_DEC | PROP_LIFE_KILL;
        p.highTemperature = 2773.0f; p.highTemperatureT = PT_PLSM;
        def(PT_FIRE, "FIRE", p);
    }
    {
        ElementProps p = defaults(0xA0A0A0);
        p.advection = 0.4f; p.airDrag = 0.04f * CFDS; p.airLoss = 0.94f; p.loss = 0.95f;
        p.collision = -0.1f; p.gravity = 0.3f; p.diffusion = 0.0f; p.hotAir = 0.0f * CFDS;
        p.falldown = 1; p.flammable = 0; p.explosive = 0; p.meltable = 5; p.hardness = 1;
        p.weight = 90; p.heatConduct = 150;
        p.properties = TYPE_PART;
        p.highTemperature = 983.0f; p.highTemperatureT = PT_LAVA;
        def(PT_STNE, "STNE", p);
    }
    {
        ElementProps p = defaults(0x404060);
        p.advection = 0.0f; p.airDrag = 0.00f * CFDS; p.airLoss = 0.90f; p.loss = 0.00f;
        p.collision = 0.0f; p.gravity = 0.0f; p.diffusion = 0.0f; p.hotAir = 0.0f * CFDS;
        p.falldown = 0; p.flammable = 0; p.explosive = 0; p.meltable = 1; p.hardness = 1;
        p.weight = 100; p.heatConduct = 251;
        p.properties = TYPE_SOLID | PROP_CONDUCTS | PROP_LIFE_DEC | PROP_HOT_GLOW;
        p.highTemperature = 1273.0f; p.highTemperatureT = PT_LAVA;
        def(PT_METL, "METL", p);
    }

    // Transitions into elements that aren't ported yet are switched off.
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

    // can_move, following SimulationData::init_can_move (only the parts that apply so far).
    // 0 = blocked, 1 = swap, 2 = share the cell.
    t.canMove.assign(MAX_ELEMENTS * MAX_ELEMENTS, 1);
    for (int d = 0; d < MAX_ELEMENTS; d++)
        t.canMove[0 * MAX_ELEMENTS + d] = 0;
    for (int m = 1; m < MAX_ELEMENTS; m++) {
        for (int d = 1; d < MAX_ELEMENTS; d++) {
            const auto &em = t.elements[m].props;
            const auto &ed = t.elements[d].props;
            // Weight check; also stops particles of the same type displacing each other.
            if (em.weight <= ed.weight)
                t.canMove[m * MAX_ELEMENTS + d] = 0;
            if ((em.properties & TYPE_ENERGY) && (ed.properties & TYPE_ENERGY))
                t.canMove[m * MAX_ELEMENTS + d] = 2;
        }
    }
    return t;
}

} // namespace bp
