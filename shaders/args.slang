// Turns the colour list sizes into indirect dispatch arguments for the update passes (after BeforeSim's LOVE and
// Game of Life, which add the particles they make to the lists).
#include "common.slang"

[shader("compute")]
[numthreads(256, 1, 1)]
void main(uint3 tid: SV_DispatchThreadID)
{
    uint c = tid.x;
    if (c == 0)
    {
        // BeforeSim: the EMP flash fades (SPRK adds 3 when it sets off EMP, up to 40)
        int d = min(int(counters[CTR_EMP_DECOR]), 40);
        if (d > 0)
            d -= d / 25 + 2;
        counters[CTR_EMP_DECOR] = uint(max(d, 0));
    }
    if (c >= pc.colourNX * pc.colourNY)
        return;
    uint count = min(counters[CTR_COLOUR_COUNT + c], pc.listCap);
    counters[CTR_DISPATCH + 3 * c + 0] = (count + 63) / 64;
    counters[CTR_DISPATCH + 3 * c + 1] = 1;
    counters[CTR_DISPATCH + 3 * c + 2] = 1;
}
