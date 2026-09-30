// Atomic tests. `iterations` operations per thread; operation id = tid * iterations + i, h = bench_hash(id).
//   ATOMIC_APPEND_LANE  append/compaction: a lane is active if (h & 1023) < pattern; InterlockedAdd on a counter
//                       (element 0 of src_view) returns its slot, then it writes id into the list (block_base view,
//                       index masked by stride). The usual GPU-driven culling / particle-list pattern.
//   ATOMIC_APPEND_WAVE  the same with one InterlockedAdd per wave (WaveActiveCountBits + WavePrefixCountBits).
//   ATOMIC_ADD          InterlockedAdd(counters[h & mask], 1), result not used (binning, histogram)
//   ATOMIC_ADD_RETURN   the same, returned value used (allocation inside a bin)
//   ATOMIC_OR           InterlockedOr(counters[h & mask], 1 << (id & 31)) (light / tile bitmasks)
//   ATOMIC_HIST_LDS     histogram in groupshared memory, then one global InterlockedAdd per non-empty bin per group
//   ATOMIC_MAX32        InterlockedMax(targets[h & mask], bench_hash(id ^ 0x5bd1e995))
//   ATOMIC_MAX64        InterlockedMax on uint64: (bench_hash(id ^ 0x5bd1e995) << 32) | id, the visibility-buffer
//                       "depth | payload" pattern of software rasterizers
#include "bench.hlsli"

BINDLESS_DECLARE_VIEW(uint)
#if defined(ATOMIC_MAX64)
BINDLESS_DECLARE_VIEW(uint64_t)
#endif

#define ATOMIC_GROUP_SIZE 256
groupshared uint g_lds[ATOMIC_GROUP_SIZE];

[numthreads(ATOMIC_GROUP_SIZE, 1, 1)]
void main(uint3 tid : SV_DispatchThreadID, uint gix : SV_GroupIndex)
{
    uint acc = 0;
#if defined(ATOMIC_HIST_LDS)
    g_lds[gix] = 0;
    GroupMemoryBarrierWithGroupSync();
#endif

    [loop]
    for (uint i = 0; i < g_push.iterations; ++i)
    {
        const uint id = tid.x * g_push.iterations + i;
        const uint h = bench_hash(id);
#if defined(ATOMIC_APPEND_LANE)
        if ((h & 1023) < g_push.pattern)
        {
            uint slot;
            InterlockedAdd(g_rw_view_uint[g_push.src_view][0], 1, slot);
            g_rw_view_uint[g_push.block_base][slot & g_push.stride] = id;
        }
#elif defined(ATOMIC_APPEND_WAVE)
        const bool active = (h & 1023) < g_push.pattern;
        const uint count = WaveActiveCountBits(active);
        if (count != 0)
        {
            uint base = 0;
            if (WaveIsFirstLane())
                InterlockedAdd(g_rw_view_uint[g_push.src_view][0], count, base);
            base = WaveReadLaneFirst(base);
            if (active)
                g_rw_view_uint[g_push.block_base][(base + WavePrefixCountBits(active)) & g_push.stride] = id;
        }
#elif defined(ATOMIC_ADD)
        InterlockedAdd(g_rw_view_uint[g_push.src_view][h & g_push.mask], 1);
#elif defined(ATOMIC_ADD_RETURN)
        uint old;
        InterlockedAdd(g_rw_view_uint[g_push.src_view][h & g_push.mask], 1, old);
        acc += old;
#elif defined(ATOMIC_OR)
        InterlockedOr(g_rw_view_uint[g_push.src_view][h & g_push.mask], 1u << (id & 31));
#elif defined(ATOMIC_HIST_LDS)
        InterlockedAdd(g_lds[h & g_push.mask], 1);
#elif defined(ATOMIC_MAX32)
        InterlockedMax(g_rw_view_uint[g_push.src_view][h & g_push.mask], bench_hash(id ^ 0x5bd1e995u));
#elif defined(ATOMIC_MAX64)
        const uint64_t v = (uint64_t(bench_hash(id ^ 0x5bd1e995u)) << 32) | uint64_t(id);
        InterlockedMax(g_rw_view_uint64_t[g_push.src_view][h & g_push.mask], v);
#endif
    }

#if defined(ATOMIC_HIST_LDS)
    GroupMemoryBarrierWithGroupSync();
    if (gix <= g_push.mask && g_lds[gix] != 0)
        InterlockedAdd(g_rw_view_uint[g_push.src_view][gix], g_lds[gix]);
#endif
    bench_output(tid.x, uint4(acc, 0, 0, 0));      // keeps the returned values alive (ATOMIC_ADD_RETURN)
}
