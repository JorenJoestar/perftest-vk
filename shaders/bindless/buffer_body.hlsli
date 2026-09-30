// Buffer path and descriptor index tests: the loop of the original perftest bodies (256 loads per thread,
// 16 KB source, uniform / linear / random start address) through different ways of reaching the buffer.
//
// BUF_RAW_ALIGNED  BINDLESS_LOAD_RAW<T>: Slang LoadAligned<T>, DXC Load<T>
// BUF_BDA          buffer device address, alignment BDA_ALIGNMENT
// BUF_SLOT         ByteAddressBuffer through a slot index: dynamically uniform (block_base + block_index)
// BUF_SLOT_NU      ... or one of 8 slots per lane with NonUniformResourceIndex (all 8 point at the same buffer)
// LOAD_WIDTH 1 / 2 / 4, LOAD_INVARIANT / LOAD_LINEAR / LOAD_RANDOM
#include "bench.hlsli"

#define THREAD_GROUP_SIZE 256
groupshared float dummyLDS[THREAD_GROUP_SIZE];

#if LOAD_WIDTH == 1
    #define LOAD_TYPE float
    #define SWIZZLE xxxx
#elif LOAD_WIDTH == 2
    #define LOAD_TYPE float2
    #define SWIZZLE xyxy
#elif LOAD_WIDTH == 4
    #define LOAD_TYPE float4
    #define SWIZZLE xyzw
#endif

#if defined(BUF_BDA)
LOAD_TYPE bda_load(uint64_t address)
{
#if defined(__SLANG__)
    return loadAligned<BDA_ALIGNMENT>((LOAD_TYPE*)address);
#else
    return vk::RawBufferLoad<LOAD_TYPE>(address, BDA_ALIGNMENT);
#endif
}
#endif

[numthreads(THREAD_GROUP_SIZE, 1, 1)]
void main(uint3 tid : SV_DispatchThreadID, uint gix : SV_GroupIndex)
{
    float4 value = 0.0;

#if defined(LOAD_INVARIANT)
    uint htid = 0;
#elif defined(LOAD_LINEAR)
    uint htid = gix;
#elif defined(LOAD_RANDOM)
    uint htid = perftest_hash1(gix) & 0xf;
#endif
    htid = htid * (4 * LOAD_WIDTH) + g_push.read_start;

#if defined(BUF_BDA)
    const uint64_t base_address = (uint64_t(g_push.addr_hi) << 32) | uint64_t(g_push.addr_lo);
#elif defined(BUF_SLOT_NU)
    const uint slot = g_push.block_base + ((perftest_hash1(gix) >> 8) & 7);
#elif defined(BUF_SLOT)
    const uint slot = g_push.block_base + g_push.block_index;
#endif

    [loop]
    for (int i = 0; i < 256; ++i)
    {
        const uint address = (htid + i * (4 * LOAD_WIDTH)) | g_push.or_mask;
#if defined(BUF_RAW_ALIGNED)
        value += BINDLESS_LOAD_RAW(LOAD_TYPE, g_push.src_view, address).SWIZZLE;
#elif defined(BUF_BDA)
        value += bda_load(base_address + address).SWIZZLE;
#elif defined(BUF_SLOT_NU)
    #if LOAD_WIDTH == 1
        value += asfloat(bindless_raw_nu(slot).Load(address)).xxxx;
    #else
        value += asfloat(bindless_raw_nu(slot).Load4(address)).xyzw;
    #endif
#elif defined(BUF_SLOT)
    #if LOAD_WIDTH == 1
        value += asfloat(bindless_raw(slot).Load(address)).xxxx;
    #else
        value += asfloat(bindless_raw(slot).Load4(address)).xyzw;
    #endif
#endif
    }

    dummyLDS[gix] = value.x + value.y + value.z + value.w;
    GroupMemoryBarrierWithGroupSync();
    // Like the original, the result goes through LDS; w = the neighbour's sum, so the LDS round trip cannot be optimized away.
    bench_output(tid.x, uint4(asuint(value.xyz), asuint(dummyLDS[(gix + 1) & (THREAD_GROUP_SIZE - 1)])));
}
