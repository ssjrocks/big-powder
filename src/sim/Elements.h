// Element definitions (ported from TPT's src/simulation/elements/*.cpp) and the can_move table.
#pragma once
#include "sim/GpuTypes.h"

#include <array>
#include <string>
#include <vector>

namespace bp {

// TPT element ids.
enum ElementId : int {
    PT_NONE = 0,
    PT_DUST = 1,
    PT_WATR = 2,
    PT_OIL = 3,
    PT_FIRE = 4,
    PT_STNE = 5,
    PT_LAVA = 6,
    PT_GAS = 10,
    PT_ICEI = 13,
    PT_METL = 14,
    PT_WTRV = 23,
    PT_PLSM = 49,
};

constexpr int NT = -1;
constexpr int ST = -2;

constexpr uint32_t TYPE_PART = 0x1;
constexpr uint32_t TYPE_LIQUID = 0x2;
constexpr uint32_t TYPE_SOLID = 0x4;
constexpr uint32_t TYPE_GAS = 0x8;
constexpr uint32_t TYPE_ENERGY = 0x10;
constexpr uint32_t PROP_CONDUCTS = 0x20;
constexpr uint32_t PROP_PHOTPASS = 0x40;
constexpr uint32_t PROP_NEUTPASS = 0x200;
constexpr uint32_t PROP_HOT_GLOW = 0x800;
constexpr uint32_t PROP_LIFE_DEC = 0x4000;
constexpr uint32_t PROP_LIFE_KILL = 0x8000;

struct ElementInfo {
    std::string name;
    ElementProps props;
};

struct ElementTable {
    std::array<ElementInfo, MAX_ELEMENTS> elements;
    std::vector<uint32_t> canMove; // [moving * MAX_ELEMENTS + destination]

    static ElementTable build();
    bool enabled(int id) const { return id > 0 && id < MAX_ELEMENTS && elements[id].props.enabled; }
};

} // namespace bp
