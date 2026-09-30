// Typed alias tests: two typed views (32-byte structs) aliased on the buffer binding,
// the GLSL pattern `buffer A { Light data[]; } a[]; buffer B { Material data[]; } b[];` on one binding.
// 128 iterations x 2 loads, 16 KB source, same addresses as the original.
// INDEX_NONUNIFORM: every lane picks one of 8 slots (all pointing at the same buffer).
#include "bench.hlsli"

struct Light    { float4 positionRadius; float4 colorIntensity; };
struct Material { float4 baseColor;      float4 roughnessMetalness; };
BINDLESS_DECLARE_VIEW(Light)
BINDLESS_DECLARE_VIEW(Material)

#define THREAD_GROUP_SIZE 256
groupshared float dummyLDS[THREAD_GROUP_SIZE];

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

    BufferRef ref;
#if defined(INDEX_NONUNIFORM)
    ref.view = g_push.block_base + ((perftest_hash1(gix) >> 8) & 7);
#else
    ref.view = g_push.block_base + g_push.block_index;
#endif
    ref.first = 0;

    [loop]
    for (int i = 0; i < 128; ++i)
    {
        const uint elem = (htid + i) | g_push.or_mask;
#if defined(INDEX_NONUNIFORM)
        value += bindless_load_nu_Light(ref, elem).colorIntensity;
        value += bindless_load_nu_Material(ref, elem).baseColor;
#else
        value += bindless_load_Light(ref, elem).colorIntensity;
        value += bindless_load_Material(ref, elem).baseColor;
#endif
    }

    dummyLDS[gix] = value.x + value.y + value.z + value.w;
    GroupMemoryBarrierWithGroupSync();
    bench_output(tid.x, uint4(asuint(value.xyz), asuint(dummyLDS[(gix + 1) & (THREAD_GROUP_SIZE - 1)])));
}
