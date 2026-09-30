// Material textures: every lane samples `iterations` texels of its texture, chosen among `distinct` textures per wave.
//   TEX_UNIFORM     one texture for the whole dispatch, no NonUniformResourceIndex (control)
//   TEX_NONUNIFORM  bindless_tex2d_nu(index): NonUniformResourceIndex, the driver handles the divergence
//   TEX_SCALARIZED  manual scalarization: loop over the distinct indices of the wave with WaveReadLaneFirst
// Output: xyz = sum of the sampled RGB bytes, w = texture index used (so --verify can check the right texture was read).
#include "bench.hlsli"

#define TEX_SIZE 64

uint3 sample_texels(Texture2D<float4> tex, uint tid)
{
    uint3 acc = 0;
    [loop]
    for (uint i = 0; i < g_push.iterations; ++i)
    {
        const uint h = bench_hash(tid * g_push.iterations + i);
        const float2 uv = (float2(h & (TEX_SIZE - 1), (h >> 6) & (TEX_SIZE - 1)) + 0.5) / TEX_SIZE;
        const float4 s = tex.SampleLevel(g_sampler_point_clamp, uv, 0.0);
        acc += uint3(round(s.rgb * 255.0));
    }
    return acc;
}

[numthreads(64, 1, 1)]
void main(uint3 tid : SV_DispatchThreadID)
{
#if defined(TEX_UNIFORM)
    const uint index = g_push.tex_base;
    const uint3 acc = sample_texels(bindless_tex2d(index), tid.x);
#else
    const uint lanes = WaveGetLaneCount();
    const uint distinct = min(g_push.distinct, lanes);
    const uint index = g_push.tex_base + WaveGetLaneIndex() * distinct / lanes;
    #if defined(TEX_NONUNIFORM)
    const uint3 acc = sample_texels(bindless_tex2d_nu(index), tid.x);
    #elif defined(TEX_SCALARIZED)
    uint3 acc = 0;
    [loop]
    for (;;)
    {
        const uint first = WaveReadLaneFirst(index);
        [branch]
        if (first == index)
        {
            // `first` is uniform across the active lanes; still marked NonUniform, which is what the spec requires
            // outside a full subgroup (the driver sees a readfirstlane result and needs no loop of its own).
            acc = sample_texels(bindless_tex2d_nu(first), tid.x);
            break;
        }
    }
    #endif
#endif
    bench_output(tid.x, uint4(acc, index - g_push.tex_base));
}
