// What TPT's tools share on the GPU (spawn.slang, sim_tool.slang): finding live particles, and the stick men's
// creation rules.
#pragma once
#include "common.slang"
#include "elements.slang"
#include "stickman_init.slang"

int liveId(uint e)
{
    int id = pmapId(e);
    return (id >= 0 && ptype[id] != PT_NONE) ? id : -1;
}

// The stick men's Create and ChangeType, when a tool makes one: a player gets its legs, and from Create (`created`) a
// spawn point where it appears if it has none; a fighter takes the first free slot. (Stick men made any other way
// are taken stock of in stickmen.slang, next tick.)
void registerStickman(uint id, int x, int y, uint t, bool created)
{
    if (t == PT_STKM || t == PT_STKM2)
    {
        uint s = t == PT_STKM ? 0u : 1u;
        if (created && stickmen[s].spawnRef == 0)
        {
            int sp = createPartStacked(x, y, s == 0 ? PT_SPAWN : PT_SPAWN2);
            if (sp >= 0)
                stickmen[s].spawnRef = uint(sp) + 1;
        }
        Stickman st = stickmen[s];
        initLegs(st, x, y);
        st.spwn = 1;
        st.partRef = id + 1;
        stickmen[s] = st;
    }
    else if (t == PT_FIGH)
    {
        int slot = -1;
        for (uint g = 0; g < MAX_FIGHTERS && slot < 0; g++)
            if (stickmen[2 + g].spwn == 0)
                slot = int(g);
        parts[id].tmp = slot;
        if (slot < 0)
            return;
        Stickman st = stickmen[2 + slot];
        initLegs(st, x, y);
        st.elem = int(PT_DUST);
        st.spwn = 1;
        st.partRef = id + 1;
        stickmen[2 + slot] = st;
    }
}

bool createAllowed(uint t)
{
    return !(isStickman(t) || t == PT_SPAWN || t == PT_SPAWN2) || stickmanCreateAllowed(t);
}

