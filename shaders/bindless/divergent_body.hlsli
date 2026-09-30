// "8 buffers" tests: divergent access to 8 buffers.
// One 128 KB buffer split in 8 regions of 16 KB (region r holds float(r + 1)), 256 float4 loads per thread.
//   MODE_SLOTS   8 slots, one per region (block_base + region); INDEX_DIVERGENT -> NonUniformResourceIndex
//   MODE_BDA     one pointer per lane: base + region * 16 KB
//   MODE_OFFSET  one slot for the whole 128 KB, region selected by element offset (uniform view, divergent index)
// Without INDEX_DIVERGENT the region is block_index (dynamically uniform): the control case.
#include "bench.hlsli"

BINDLESS_DECLARE_VIEW(float4)

#define THREAD_GROUP_SIZE 256
#define REGION_BYTES 16384
groupshared float dummyLDS[THREAD_GROUP_SIZE];

[numthreads(THREAD_GROUP_SIZE, 1, 1)]
void main(uint3 tid : SV_DispatchThreadID, uint gix : SV_GroupIndex)
{
    float4 value = 0.0;

#if defined(LOAD_LINEAR)
    uint htid = gix;
#elif defined(LOAD_RANDOM)
    uint htid = perftest_hash1(gix) & 0xf;
#endif

#if defined(INDEX_DIVERGENT)
    const uint region = (perftest_hash1(gix) >> 8) & 7;
#else
    const uint region = g_push.block_index;
#endif

#if defined(MODE_BDA)
    const uint64_t base_address = ((uint64_t(g_push.addr_hi) << 32) | uint64_t(g_push.addr_lo)) + uint64_t(region * REGION_BYTES);
#else
    BufferRef ref;
    #if defined(MODE_SLOTS)
    ref.view = g_push.block_base + region;
    ref.first = 0;
    #else
    ref.view = g_push.src_view;
    ref.first = region * (REGION_BYTES / 16);
    #endif
#endif

    [loop]
    for (int i = 0; i < 256; ++i)
    {
        const uint elem = (htid + i) | g_push.or_mask;
#if defined(MODE_BDA)
    #if defined(__SLANG__)
        value += loadAligned<16>((float4*)(base_address + uint64_t(elem * 16)));
    #else
        value += vk::RawBufferLoad<float4>(base_address + uint64_t(elem * 16), 16);
    #endif
#elif defined(MODE_SLOTS) && defined(INDEX_DIVERGENT)
        value += bindless_load_nu_float4(ref, elem);
#else
        value += bindless_load_float4(ref, elem);
#endif
    }

    dummyLDS[gix] = value.x + value.y + value.z + value.w;
    GroupMemoryBarrierWithGroupSync();
    bench_output(tid.x, uint4(asuint(value.xyz), asuint(dummyLDS[(gix + 1) & (THREAD_GROUP_SIZE - 1)])));
}
