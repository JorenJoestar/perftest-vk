// Store tests: `iterations` stores per thread of STORE_WIDTH uints (1, 2, 4 = 4, 8, 16 bytes) into a working set of
// (mask + 1) elements. The value stored at element e depends only on e (store_value), so threads that collide on
// the same element write the same value and the final contents are deterministic (checked by --verify).
//   pattern 0  linear  element (tid + i * stride): consecutive lanes, consecutive elements (full cache lines)
//   pattern 1  random  element bench_hash(tid * iterations + i)
//   pattern 2  sparse  like linear, but one element per 64-byte line (partial cache-line writes)
// One path per define:
//   STORE_STRUCTURED  RWStructuredBuffer<uint / uint2 / uint4> typed view
//   STORE_RAW         RWByteAddressBuffer.Store / Store2 / Store4
//   STORE_BDA         buffer device address, aligned to the store size (Slang storeAligned / DXC vk::RawBufferStore)
#include "bench.hlsli"

#if STORE_WIDTH == 1
    #define STORE_TYPE uint
    BINDLESS_DECLARE_VIEW(uint)
#elif STORE_WIDTH == 2
    #define STORE_TYPE uint2
    BINDLESS_DECLARE_VIEW(uint2)
#else
    #define STORE_TYPE uint4
#endif
#define STORE_BYTES (4 * STORE_WIDTH)

STORE_TYPE store_value(uint e)
{
    const uint4 v = bench_pattern(e);
#if STORE_WIDTH == 1
    return v.x;
#elif STORE_WIDTH == 2
    return v.xy;
#else
    return v;
#endif
}

void store_element(uint e)
{
    const STORE_TYPE v = store_value(e);
#if defined(STORE_STRUCTURED)
    BufferRef r;
    r.view = g_push.src_view;
    r.first = 0;
    #if STORE_WIDTH == 1
    bindless_store_uint(r, e, v);
    #elif STORE_WIDTH == 2
    bindless_store_uint2(r, e, v);
    #else
    bindless_store_uint4(r, e, v);
    #endif
#elif defined(STORE_RAW)
    #if STORE_WIDTH == 1
    bindless_rw_raw(g_push.src_view).Store(e * STORE_BYTES, v);
    #elif STORE_WIDTH == 2
    bindless_rw_raw(g_push.src_view).Store2(e * STORE_BYTES, v);
    #else
    bindless_rw_raw(g_push.src_view).Store4(e * STORE_BYTES, v);
    #endif
#elif defined(STORE_BDA)
    const uint64_t address = ((uint64_t(g_push.addr_hi) << 32) | uint64_t(g_push.addr_lo)) + uint64_t(e) * STORE_BYTES;
    #if defined(__SLANG__)
    storeAligned<STORE_BYTES>((STORE_TYPE*)address, v);
    #else
    vk::RawBufferStore<STORE_TYPE>(address, v, STORE_BYTES);
    #endif
#endif
}

[numthreads(256, 1, 1)]
void main(uint3 tid : SV_DispatchThreadID)
{
    [branch]
    if (g_push.pattern == 1)
    {
        [loop]
        for (uint i = 0; i < g_push.iterations; ++i)
            store_element(bench_hash(tid.x * g_push.iterations + i) & g_push.mask);
    }
    else
    {
        const uint spread = g_push.pattern == 2 ? 64 / STORE_BYTES : 1;
        [loop]
        for (uint i = 0; i < g_push.iterations; ++i)
            store_element(((tid.x + i * g_push.stride) * spread) & g_push.mask);
    }
}
