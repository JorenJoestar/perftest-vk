// bench.hlsli - common code of the bindless suite (see src/bindless_suite.cpp).
// Resources come from the bindless layout (bindless.hlsli + src/bindless_layout.cpp):
// every test only needs push constants.
#include "bindless.hlsli"

struct BenchPush                   // 64 of the 128 bytes; must match BenchPush in src/bindless_suite.cpp
{
    uint src_view;                 // buffer slot of the data buffer
    uint src_first;                // first element (uint4) of the data inside the view
    uint mask;                     // working set in elements - 1 (power of two)
    uint iterations;
    uint pattern;                  // 0 = linear (coalesced sweep), 1 = random
    uint stride;                   // linear pattern: elements advanced per iteration (= total threads)
    uint tex_base;                 // first texture slot
    uint distinct;                 // textures per wave
    uint out_view;                 // buffer slot of the output
    uint verify;                   // 1 = every thread writes its result
    uint addr_lo;                  // BDA of the data buffer
    uint addr_hi;
    // Buffer tests (buffer_body, alias_body, divergent_body):
    uint or_mask;                  // always 0, OR-ed into addresses so the compiler cannot merge loads (like elementsMask)
    uint read_start;               // start byte address (like readStartAddress)
    uint block_base;               // first of 8 consecutive buffer slots
    uint block_index;              // dynamically uniform slot offset (0 when timing, 3 in --verify)
};
BINDLESS_PUSH_CONSTANTS(BenchPush)

BINDLESS_DECLARE_VIEW(uint4)

// Integer hash (same on the CPU): good enough to spread random accesses over the whole working set.
uint bench_hash(uint x)
{
    x ^= x >> 16;
    x *= 0x7feb352du;
    x ^= x >> 15;
    x *= 0x846ca68bu;
    x ^= x >> 16;
    return x;
}

// The original perftest hash (shaders/hash.hlsli), used by the buffer tests to pick addresses and descriptors.
uint perftest_hash1(uint c)
{
    return c * 0x3504f333u;
}

// Content of element e of the data buffer (written by bindless_fill, recomputed on the CPU by --verify).
uint4 bench_pattern(uint e)
{
    return uint4(e * 0x9E3779B1u, e ^ 0xA5A5A5A5u, e + 7u, ~e);
}

// Keeps the loads alive without paying for a store: the store runs only in --verify mode,
// or if the result hits a value the data never produces.
void bench_output(uint tid, uint4 value)
{
    [branch]
    if (g_push.verify != 0 || (value.x == 0xFFFFFFFFu && value.y == 0x12345678u))
    {
        BufferRef out_ref;
        out_ref.view = g_push.out_view;
        out_ref.first = 0;
        bindless_store_uint4(out_ref, tid, value);
    }
}
