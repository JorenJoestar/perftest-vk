// Fills the data buffer with bench_pattern(e).
#include "bench.hlsli"

[numthreads(256, 1, 1)]
void main(uint3 tid : SV_DispatchThreadID)
{
    BufferRef r;
    r.view = g_push.src_view;
    r.first = g_push.src_first;
    const uint e = tid.x + tid.y * g_push.stride;
    if (e <= g_push.mask)
        bindless_store_uint4(r, e, bench_pattern(e));
}
