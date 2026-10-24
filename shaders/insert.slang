// TPT's Simulation::Load pasting a save, and clear_area, on the GPU. sim/GpuSave makes Load's decisions on the CPU, one
// particle after another as TPT does (what is placed, what is removed, which stick man slots); these passes carry
// them out:
//  - mode 0: what was there goes: every particle whose spot (int(x + 0.5), int(y + 0.5)) is set in `edit`, a bit per
//    pixel of the box at (brushX, brushY), brushR wide and brushType high;
//  - mode 3: clear_area's: every particle with camX <= x <= hdispMin and camY <= y <= hdispMax;
//  - modes 1 and 2: the pc.brushType inbox entries (the type in pad1) are placed, stacked ones too, with ids from the
//    free list, on top of their cell in the order RecalcFreeParticles leaves them: mode 1 all but INVIS and FILT,
//    then mode 2 those, which only take a cell that is empty. The id each one got is written back to its pad2
//    (0xFFFFFFFF if there was none left), for fixing SOAP's links. An entry's `pass` says which stick man it is
//    (PASTE_* below).
#include "common.slang"
#include "elements.slang"
#include "stickman_init.slang"

[[vk::binding(33, 0)]] RWStructuredBuffer<uint> edit;

// An inbox entry's pass: bits 0-1 what it is to the stick men (1 a player, 2 a player's spawn point, 3 a fighter in
// slot tmp), bit 2 which player, bit 3 a player whose Create makes its spawn point too, bits 4 and 5 rocket boots and
// fan, bit 6 a fighter whose ChangeType took the slot in bits 8-14 (which Load then leaves taken), bits 16-31 a
// fighter's element
static const uint PASTE_PLAYER = 1, PASTE_SPAWN = 2, PASTE_FIGHTER = 3;

[shader("compute")]
[numthreads(64, 1, 1)]
void main(uint3 tid: SV_DispatchThreadID)
{
    if (pc.mode == 0 || pc.mode == 3)
    {
        if (tid.x >= pc.capacity || ptype[tid.x] == PT_NONE)
            return;
        Particle q = parts[tid.x];
        if (pc.mode == 3)
        {
            if (q.x >= pc.camX && q.x <= pc.hdispMin && q.y >= pc.camY && q.y <= pc.hdispMax)
                killPartById(tid.x);
            return;
        }
        int x = int(q.x + 0.5) - pc.brushX, y = int(q.y + 0.5) - pc.brushY;
        if (x < 0 || y < 0 || x >= pc.brushR || y >= pc.brushType)
            return;
        uint bit = uint(y) * uint(pc.brushR) + uint(x);
        if ((edit[bit >> 5] & (1u << (bit & 31))) != 0)
            killPart(tid.x, x + pc.brushX, y + pc.brushY);
        return;
    }
    if (tid.x >= uint(pc.brushType))
        return;
    Particle p = inbox[tid.x];
    uint t = p.pad1;
    bool late = t == PT_INVIS || t == PT_FILT;
    if (late != (pc.mode == 2))
        return;
    inbox[tid.x].pad2 = 0xFFFFFFFFu;
    int x = int(floor(p.x + 0.5)), y = int(floor(p.y + 0.5));
    if (!inSim(x, y))
        return;
    uint slot;
    InterlockedAdd(counters[CTR_FREE_TAKEN], 1u, slot);
    if (slot >= counters[CTR_FREE_COUNT])
        return;
    uint id = freelist[slot];
    uint reg = p.pass;
    p.pass = 0xFFFFFFFFu;
    p.pad1 = 0;
    p.pad2 = 0;
    parts[id] = p;
    ptype[id] = t;
    uint c = cellIndex(x, y), orig;
    if (isEnergy(t))
        InterlockedMax(photons[c], pmapEnc(id), orig);
    else if (late)
        InterlockedCompareExchange(pmap[c], 0u, pmapEnc(id), orig);
    else
        InterlockedMax(pmap[c], pmapEnc(id), orig);
    inbox[tid.x].pad2 = id;

    // The stick men, as Load sets them up after create_part (Element_STKM_init_legs and so on); sim/GpuSave allows
    // at most one of each player and of each spawn point
    uint kind = reg & 3, s = (reg >> 2) & 1;
    if (kind == PASTE_PLAYER)
    {
        Stickman st = stickmen[s];
        initLegs(st, int(p.x + 0.5), int(p.y + 0.5));
        st.spwn = 1;
        st.elem = int(PT_DUST);
        st.rocketBoots = (reg >> 4) & 1;
        st.fan = (reg >> 5) & 1;
        st.partRef = id + 1;
        stickmen[s] = st;
        if ((reg & 8) != 0)
        {
            // STKM's Create, inside create_part: its spawn point where it is (sim/GpuSave has found there is none)
            stickmen[s].spawnRef = 0;
            uint old;
            InterlockedAnd(counters[CTR_STKM_FLAGS], ~(s == 0 ? 4u : 8u), old);
            int sp = createPartStacked(x, y, s == 0 ? PT_SPAWN : PT_SPAWN2);
            if (sp >= 0)
                stickmen[s].spawnRef = uint(sp) + 1;
        }
    }
    else if (kind == PASTE_SPAWN)
        stickmen[s].spawnRef = id + 1;
    else if (kind == PASTE_FIGHTER && p.tmp >= 0 && p.tmp < int(MAX_FIGHTERS))
    {
        if ((reg & 64) != 0)
        {
            // FIGH's ChangeType: Element_FIGH_Alloc and NewFighter, for a slot nothing uses after Load's own
            uint a = (reg >> 8) & 0x7F;
            Stickman lost = stickmen[2 + a];
            initLegs(lost, int(p.x + 0.5), int(p.y + 0.5));
            lost.elem = int(PT_DUST);
            lost.spwn = 1;
            lost.partRef = STKM_LEAKED;
            stickmen[2 + a] = lost;
        }
        Stickman st = stickmen[2 + uint(p.tmp)];
        initLegs(st, int(p.x + 0.5), int(p.y + 0.5));
        st.elem = int(reg >> 16);
        st.spwn = 1;
        st.rocketBoots = (reg >> 4) & 1;
        st.fan = (reg >> 5) & 1;
        st.partRef = id + 1;
        stickmen[2 + uint(p.tmp)] = st;
    }
}
