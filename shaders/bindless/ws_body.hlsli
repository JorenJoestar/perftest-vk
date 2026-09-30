// Working-set sweep: `iterations` uint4 loads per thread over a working set of (mask + 1) elements,
// linear (coalesced), random per lane, wave-uniform or wave-coherent (see main). One path per define:
//   WS_STRUCTURED   StructuredBuffer<uint4> typed view (the usual engine path)
//   WS_RAW_LOAD4    ByteAddressBuffer.Load4
//   WS_RAW_ALIGNED  BINDLESS_LOAD_RAW: Slang LoadAligned<uint4> / DXC Load<uint4>
//   WS_BDA          buffer device address, 16-byte aligned (Slang pointer / DXC vk::RawBufferLoad)
#include "bench.hlsli"

uint4 ws_load(uint e)
{
#if defined(WS_STRUCTURED)
    BufferRef r;
    r.view = g_push.src_view;
    r.first = g_push.src_first;
    return bindless_load_uint4(r, e);
#elif defined(WS_RAW_LOAD4)
    return bindless_raw(g_push.src_view).Load4((g_push.src_first + e) * 16);
#elif defined(WS_RAW_ALIGNED)
    return BINDLESS_LOAD_RAW(uint4, g_push.src_view, (g_push.src_first + e) * 16);
#elif defined(WS_BDA)
    const uint64_t address = ((uint64_t(g_push.addr_hi) << 32) | uint64_t(g_push.addr_lo)) + uint64_t(g_push.src_first + e) * 16;
    #if defined(__SLANG__)
    return loadAligned<16>((uint4*)address);
    #else
    return vk::RawBufferLoad<uint4>(address, 16);
    #endif
#endif
}

[numthreads(256, 1, 1)]
void main(uint3 tid : SV_DispatchThreadID)
{
    uint4 acc = 0;
    [branch]
    if (g_push.pattern == 0)
    {
        // Linear: consecutive lanes read consecutive elements.
        [loop]
        for (uint i = 0; i < g_push.iterations; ++i)
            acc += ws_load((tid.x + i * g_push.stride) & g_push.mask);
    }
    else if (g_push.pattern == 1)
    {
        // Random per lane.
        [loop]
        for (uint i = 0; i < g_push.iterations; ++i)
            acc += ws_load(bench_hash(tid.x * g_push.iterations + i) & g_push.mask);
    }
    else
    {
        // Per wave: the wave index comes from WaveReadLaneFirst, so the compiler knows it is uniform (scalar loads where
        // the hardware has them). Pattern 2: every lane reads the same random element; pattern 3: the wave reads a
        // contiguous block of WaveGetLaneCount() elements at a random place.
        const uint wave = WaveReadLaneFirst(tid.x) / WaveGetLaneCount();
        const uint lane = g_push.pattern == 3 ? WaveGetLaneIndex() : 0;
        const uint lanes = g_push.pattern == 3 ? WaveGetLaneCount() : 1;
        [loop]
        for (uint i = 0; i < g_push.iterations; ++i)
            acc += ws_load((bench_hash(wave * g_push.iterations + i) * lanes + lane) & g_push.mask);
    }
    bench_output(tid.x, acc);
}
