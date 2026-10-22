// The heat display's range (Renderer::AdjustHdispLimit, at the start of each frame), kept in counters between
// frames. Stage 0 (colourIdx 0, only for the dynamic heat display) finds the lowest and highest temperature: of
// particles that conduct heat in the heat colour mode, and of the air's heat in the air heat display. Stage 1 (one
// thread) moves the range towards them slowly (it widens at once), or sets the preset's fixed range.
#include "render_common.slang"

[shader("compute")]
[numthreads(256, 1, 1)]
void main(uint3 tid: SV_DispatchThreadID)
{
    if (pc.colourIdx == 0)
    {
        uint i = tid.x;
        if ((pc.colorMode & COLOUR_HEAT) != 0 && i < pc.capacity)
        {
            uint t = ptype[i];
            if (t != PT_NONE && elems[t].heatConduct != 0)
            {
                int nx = int(parts[i].x + 0.5), ny = int(parts[i].y + 0.5);
                if (inVideo(nx, ny))
                {
                    uint v = asuint(max(parts[i].temp, 0.0)), o;
                    InterlockedMin(counters[CTR_HDISP_SCAN], v, o);
                    InterlockedMax(counters[CTR_HDISP_SCAN + 1], v, o);
                }
            }
        }
        if (params[0].ambientHeat != 0 && (pc.displayMode & DISPLAY_AIR) != 0 && (pc.displayMode & DISPLAY_AIRH) != 0 &&
            i < pc.CW * pc.CH)
        {
            uint v = asuint(max(airGet(AIR_HV, i), 0.0)), o;
            InterlockedMin(counters[CTR_HDISP_SCAN], v, o);
            InterlockedMax(counters[CTR_HDISP_SCAN + 1], v, o);
        }
        return;
    }
    if (tid.x != 0)
        return;
    float lo = MAX_TEMP, hi = MIN_TEMP;
    if (pc.hdispMin < 0.0)
    {
        lo = asfloat(counters[CTR_HDISP_SCAN]);
        hi = asfloat(counters[CTR_HDISP_SCAN + 1]);
        float sLo = asfloat(counters[CTR_HDISP]), sHi = asfloat(counters[CTR_HDISP + 1]);
        // shrink towards the new range slowly, to prevent rapid flashes (but widen at once)
        float maxGap = sHi - hi;
        hi = max(hi, sHi - maxGap * 0.05);
        float minGap = lo - sLo;
        lo = min(lo, sLo + minGap * 0.05);
        // at least 1 degree apart
        hi = min(MAX_TEMP, max(hi, lo + 1.0));
        lo = max(MIN_TEMP, min(lo, hi - 1.0));
    }
    else
    {
        lo = pc.hdispMin;
        hi = pc.hdispMax;
    }
    if (isnan(lo))
        lo = MIN_TEMP;
    if (isnan(hi))
        hi = MAX_TEMP;
    hi = clamp(hi, MIN_TEMP, MAX_TEMP);
    lo = clamp(lo, MIN_TEMP, hi);
    counters[CTR_HDISP] = asuint(lo);
    counters[CTR_HDISP + 1] = asuint(hi);
}
