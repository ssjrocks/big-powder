// Element definitions (ported from TPT's src/simulation/elements/*.cpp) and the can_move table.
#pragma once
#include "sim/ElementIds.h"
#include "sim/GpuTypes.h"
#include "sim/Signs.h"

#include <array>
#include <string>
#include <vector>

namespace bp {


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
    std::string identifier; // TPT's (DEFAULT_PT_WATR...), which saves use to name elements
    std::string description;
    bool existsInTpt = false; // a real TPT element (props.enabled says whether the GPU engine runs it)
    int menuSection = 0;
    bool menuVisible = false;
    ElementProps props;
};

// Every element, read from TPT's own tables at startup (ClassicSim::exportElements), so the properties are
// exactly TPT's. props.enabled marks the elements the GPU engine fully supports (see Elements.cpp).
struct ElementTable {
    std::array<ElementInfo, MAX_ELEMENTS> elements;
    std::vector<Particle> defaults; // TPT's DefaultProperties per element
    std::vector<uint32_t> canMove;  // TPT's can_move[moving * MAX_ELEMENTS + destination]
    std::vector<uint32_t> renderTables; // TPT's renderer tables (sim/RenderTables.h), ending with its font
    Font font;                          // the font's code points
    std::vector<std::string> golNames;  // the built-in Game of Life rulesets' names (by LIFE's ctype)
    std::vector<std::string> golDescriptions;
    struct Wall {
        std::string name, description, identifier;
        uint32_t colour = 0, eglow = 0;
        int drawstyle = 0;
    };
    std::vector<Wall> walls;             // TPT's wall types, by id
    struct MenuSection {
        char32_t icon = 0;
        std::string name;
        bool visible = true;
    };
    std::vector<MenuSection> menuSections; // TPT's SC_* menus

    static ElementTable build();
    bool enabled(int id) const { return id > 0 && id < MAX_ELEMENTS && elements[id].props.enabled; }
    int byName(const std::string &name) const; // -1 if unknown
};

} // namespace bp
