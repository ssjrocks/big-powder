// Turns the colour list sizes into indirect dispatch arguments for the update passes.
#include "common.slang"

[shader("compute")]
[numthreads(32, 1, 1)]
void main(uint3 tid: SV_DispatchThreadID)
{
    uint c = tid.x;
    if (c >= pc.colourN * pc.colourN)
        return;
    uint count = min(counters[CTR_COLOUR_COUNT + c], pc.listCap);
    counters[CTR_DISPATCH + 3 * c + 0] = (count + 63) / 64;
    counters[CTR_DISPATCH + 3 * c + 1] = 1;
    counters[CTR_DISPATCH + 3 * c + 2] = 1;
}
