// bindless.hlsli - the bindless layout of the bindless suite, HLSL for DXC and Slang (SPIR-V).
//
// Must match src/bindless_layout.hpp:
//   set 0 (UPDATE_AFTER_BIND | PARTIALLY_BOUND | UPDATE_UNUSED_WHILE_PENDING on the arrays)
//     binding 0  SAMPLED_IMAGE[]   Texture2D<float4>
//     binding 1  STORAGE_BUFFER[]  every buffer view: StructuredBuffer<T>, RWStructuredBuffer<T>, (RW)ByteAddressBuffer, aliased
//     binding 2  SAMPLER           immutable point/clamp sampler
//   push constants: 128 bytes
//
// A buffer is addressed with a BufferRef: the view (slot in binding 1) plus the first element inside it.
// *_nu accessors apply NonUniformResourceIndex to the final index. Do not do arithmetic on the result of
// NonUniformResourceIndex: some Slang versions drop the decoration (slang#13072).
//
// Compile flags:
//   Slang: -target spirv -profile spirv_1_5 -warnings-disable 39001,41012   (39001 = intentional binding aliasing)
//   DXC:   -spirv -T cs_6_6 -fspv-target-env=vulkan1.2

#ifndef BINDLESS_HLSLI
#define BINDLESS_HLSLI

struct BufferRef
{
    uint view;
    uint first;
};

[[vk::binding(0, 0)]] Texture2D<float4>   g_textures[];
[[vk::binding(1, 0)]] ByteAddressBuffer   g_raw_buffers[];
[[vk::binding(1, 0)]] RWByteAddressBuffer g_rw_raw_buffers[];
[[vk::binding(2, 0)]] SamplerState        g_sampler_point_clamp;

#define BINDLESS_PUSH_CONSTANTS(T) [[vk::push_constant]] T g_push;

Texture2D<float4>   bindless_tex2d(uint i)       { return g_textures[i]; }
Texture2D<float4>   bindless_tex2d_nu(uint i)    { return g_textures[NonUniformResourceIndex(i)]; }
ByteAddressBuffer   bindless_raw(uint view)      { return g_raw_buffers[view]; }
ByteAddressBuffer   bindless_raw_nu(uint view)   { return g_raw_buffers[NonUniformResourceIndex(view)]; }
RWByteAddressBuffer bindless_rw_raw(uint view)   { return g_rw_raw_buffers[view]; }

// Typed views: declare once per struct type, at global scope.
//   BINDLESS_DECLARE_VIEW(Light)  ->  bindless_load_Light(ref, i), bindless_load_nu_Light(ref, i), bindless_store_Light(ref, i, v)
#define BINDLESS_DECLARE_VIEW(T)                                                                                              \
    [[vk::binding(1, 0)]] StructuredBuffer<T>   g_view_##T[];                                                                \
    [[vk::binding(1, 0)]] RWStructuredBuffer<T> g_rw_view_##T[];                                                             \
    T    bindless_load_##T(BufferRef r, uint i)       { return g_view_##T[r.view][r.first + i]; }                            \
    T    bindless_load_nu_##T(BufferRef r, uint i)    { return g_view_##T[NonUniformResourceIndex(r.view)][r.first + i]; }   \
    void bindless_store_##T(BufferRef r, uint i, T v) { g_rw_view_##T[r.view][r.first + i] = v; }

// Raw load of a T at a byte offset aligned to the natural alignment of T.
// Slang: LoadAligned<T> (one vector load). DXC: Load<T> (on SPIR-V it becomes 32-bit loads).
#if defined(__SLANG__)
    #define BINDLESS_LOAD_RAW(T, view, byte_offset) bindless_raw(view).LoadAligned<T>(byte_offset)
#else
    #define BINDLESS_LOAD_RAW(T, view, byte_offset) bindless_raw(view).Load<T>(byte_offset)
#endif

#endif // BINDLESS_HLSLI
