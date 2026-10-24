// A stick man's state when it appears (STKM.cpp's Element_STKM_init_legs), for stickmen.slang and the tools
// (spawn.slang).
#pragma once
#include "common.slang"

// Element_STKM_init_legs
void initLegs(inout Stickman st, int x, int y)
{
    st.legs[0] = float(x - 1);
    st.legs[1] = float(y + 6);
    st.legs[2] = float(x - 1);
    st.legs[3] = float(y + 6);
    st.legs[4] = float(x - 3);
    st.legs[5] = float(y + 12);
    st.legs[6] = float(x - 3);
    st.legs[7] = float(y + 12);
    st.legs[8] = float(x + 1);
    st.legs[9] = float(y + 6);
    st.legs[10] = float(x + 1);
    st.legs[11] = float(y + 6);
    st.legs[12] = float(x + 3);
    st.legs[13] = float(y + 12);
    st.legs[14] = float(x + 3);
    st.legs[15] = float(y + 12);
    for (int k = 0; k < 8; k++)
        st.accs[k] = 0.0;
    st.comm = 0;
    st.pcomm = 0;
    st.frames = 0;
    st.spwn = 0;
    st.fan = 0;
    st.rocketBoots = 0;
}
