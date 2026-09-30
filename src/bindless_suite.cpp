// bindless_suite.cpp - see bindless_suite.hpp.
//
// Tests:
//   texture NonUniform   32 material textures, 1..32 distinct textures per wave:
//                        uniform index (control), NonUniformResourceIndex, manual scalarization (WaveReadLaneFirst)
//   working set          16 KB .. 64 MB, linear and random, through a typed view, raw Load4, raw aligned load and BDA;
//                        wave-uniform and wave-coherent addresses through the typed view and raw Load4
//   buffer path / descriptor index / typed alias / 8 buffers
//                        perftest-style loops (256 loads per thread, 16 KB source, uniform/linear/random addresses)
//                        through different buffer paths: raw aligned load, BDA, slot index (uniform or NonUniform),
//                        aliased typed views, divergent access to 8 buffers
#include "bindless_suite.hpp"
#include "bindless_layout.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <map>
#include <vector>

namespace {

struct BenchPush {                 // must match shaders/bindless/bench.hlsli
    uint32_t src_view, src_first, mask, iterations;
    uint32_t pattern, stride, tex_base, distinct;
    uint32_t out_view, verify, addr_lo, addr_hi;
    uint32_t or_mask, read_start, block_base, block_index;
};
static_assert( sizeof( BenchPush ) <= k_bindless_push_size, "push constants" );

enum class Kind { 
    WorkingSet,
    Texture,
    BufferLoad,
    TypedAlias,
    Divergent,
    Store,
    Atomic
};

enum class AtomicOp {
    AppendLane,
    AppendWave,
    Add,
    AddReturn,
    Or,
    HistLds,
    Max32,
    Max64
};

enum class Pattern {
    Uniform,
    Linear,
    Random
};

enum : uint32_t {
    Need_Bda = 1u << 0,
    Need_Ballot = 1u << 1,
    Need_Int64Atomics = 1u << 2
};

constexpr uint32_t k_data_slot = 0, k_output_slot = 1, k_texture_base = 0;
// Buffer tests (buffer path, descriptor index, typed alias, 8 buffers): 16 KB input, 8 slots on the input (timing), 8 slots on the 8 regions of the 128 KB divergent buffer
// (the "8 buffers" tests, and --verify of the other slot tests, so a wrong descriptor reads a wrong value), whole 128 KB.
constexpr uint32_t k_input_slot = 2, k_input_block_base = 3, k_region_block_base = 11, k_divergent_slot = 19, k_store_slot = 20, k_atomic_slot = 21, k_list_slot = 22, k_buffer_slots = 23;
constexpr uint32_t k_input_bytes = 16384, k_region_bytes = 16384;
// 64 MB of uint4
constexpr uint32_t k_data_elements = 64u * 1024 * 1024 / 16;
constexpr uint32_t k_store_bytes = 64u * 1024 * 1024;
// Atomic tests: 1M counters / 64-bit targets (8 MB), a 2M-entry append list (8 MB), 32 operations per thread
constexpr uint32_t k_atomic_targets = 1u << 20, k_list_entries = 1u << 21, k_atomic_iterations = 32;
constexpr uint32_t k_ws_groups = 1024, k_ws_group_size = 256;
// Loads/stores per thread in the working-set and store tests: more when the working set fits in a cache, so that no
// dispatch is too short to time reliably (the count is part of the test name).
constexpr uint32_t k_ws_iterations_dram = 32, k_ws_iterations_cache = 256;
uint32_t ws_iterations( uint32_t ws_bytes ) { return ws_bytes >= ( 64u << 20 ) ? k_ws_iterations_dram : k_ws_iterations_cache; }
constexpr uint32_t k_tex_groups = 4096, k_tex_group_size = 64, k_tex_iterations = 32;
constexpr uint32_t k_tex_count = 32, k_tex_size = 64;
// output buffer: one uint4 per thread
constexpr uint32_t k_max_threads = 1u << 20;
constexpr uint32_t k_verify_threads = 4096;

//
//
struct Test {

    std::string name, shader;
    Kind        kind;
    Pattern     buf_pattern = Pattern::Uniform; // buffer tests
    uint32_t    buf_width = 4;                  // BufferLoad: floats per load (1, 2, 4)
    bool        buf_divergent = false;          // NonUniform / divergent index
    bool        verify_on_regions = false;      // --verify points the slot index at the 8 distinct regions
    uint32_t    store_bytes = 0;                // Store: bytes per store (4, 8, 16)
    AtomicOp    atomic_op = AtomicOp::Add;      // Atomic: operation
    uint32_t    needs = 0;
    BenchPush   push{};
    uint32_t    groups = 0, group_size = 0;
    VkPipeline  pipeline = VK_NULL_HANDLE;
    std::string skip_reason;
}; // struct Test

// shaders/bindless/bench.hlsli
uint32_t bench_hash( uint32_t x ) {
    x ^= x >> 16;
    x *= 0x7feb352du;
    x ^= x >> 15;
    x *= 0x846ca68bu;
    x ^= x >> 16;
    return x;
}

void bench_pattern( uint32_t e, uint32_t out[ 4 ] ) {
    out[ 0 ] = e * 0x9E3779B1u;
    out[ 1 ] = e ^ 0xA5A5A5A5u;
    out[ 2 ] = e + 7u;
    out[ 3 ] = ~e;
}

float       buf_input_word( uint32_t k ) { return ( float )( ( k * 3u ) & 7u ); }
uint32_t    perftest_hash1( uint32_t c ) { return c * 0x3504f333u; }    // shaders/hash.hlsli

uint32_t    texel( uint32_t t, uint32_t x, uint32_t y ) { return bench_hash( t * 4096u + y * k_tex_size + x ) | 0xFF000000u; }   // RGBA8, alpha 255

std::vector<Test> build_tests( uint32_t buf_groups ) {
    std::vector<Test> tests;

    // Texture NonUniform
    auto tex = [ & ]( const std::string& name, const char* shader, uint32_t distinct, uint32_t needs ) {
        Test t;
        t.name = "texture NonUniform: " + name;
        t.shader = shader;
        t.kind = Kind::Texture;
        t.needs = needs;
        t.push.iterations = k_tex_iterations;
        t.push.tex_base = k_texture_base;
        t.push.distinct = distinct;
        t.push.out_view = k_output_slot;
        t.groups = k_tex_groups;
        t.group_size = k_tex_group_size;
        tests.push_back( t );
    };

    tex( "1 texture, uniform index (control)", "bindless_tex_uniform", 1, 0 );
    for ( uint32_t d : { 1u, 2u, 4u, 8u, 16u, 32u } ) {
        tex( std::to_string( d ) + " texture" + ( d > 1 ? "s" : "" ) + "/wave, NonUniformResourceIndex", "bindless_tex_nonuniform", d, 0 );
    }

    for ( uint32_t d : { 1u, 2u, 4u, 8u, 16u, 32u } ) {
        tex( std::to_string( d ) + " texture" + ( d > 1 ? "s" : "" ) + "/wave, scalarized (WaveReadLaneFirst loop)", "bindless_tex_scalarized", d, Need_Ballot );
    }

    // Working set
    const struct { const char* label; uint32_t bytes; } sizes[] = {
        { "16 KB", 16u << 10 }, { "256 KB", 256u << 10 }, { "4 MB", 4u << 20 }, { "64 MB", 64u << 20 } };

    const struct { const char* label; const char* shader; uint32_t needs; } paths[] = {
        { "StructuredBuffer<uint4>", "bindless_ws_structured", 0 },
        { "ByteAddressBuffer.Load4", "bindless_ws_raw_load4", 0 },
        { "raw aligned load (Slang LoadAligned / DXC Load<uint4>)", "bindless_ws_raw_aligned", 0 },
        { "BDA align16", "bindless_ws_bda", Need_Bda } };

    // Shader pattern 0 linear, 1 random (per lane), 2 wave-uniform (one random address per wave, like per-instance or
    // per-material data), 3 wave-coherent (each wave reads a contiguous block at a random place). 2 and 3 only for the
    // typed view and raw Load4.
    const char* ws_patterns[] = { "linear", "random", "wave-uniform", "wave-coherent" };

    for ( auto& size : sizes ) {
        for ( uint32_t pattern = 0; pattern < 4; ++pattern ) {
            for ( auto& path : paths ) {
                if ( pattern >= 2 && path.shader != std::string( "bindless_ws_structured" ) && path.shader != std::string( "bindless_ws_raw_load4" ) ) {
                    continue;
                }
                const uint32_t iterations = ws_iterations( size.bytes );
                Test t;
                t.name = std::string( "working set " ) + size.label + " " + ws_patterns[ pattern ] + ", " + std::to_string( iterations ) + " loads: " + path.label;
                t.shader = path.shader;
                t.kind = Kind::WorkingSet;
                t.needs = path.needs | Need_Ballot;          // WaveReadLaneFirst in ws_body.hlsli
                t.push.src_view = k_data_slot;
                t.push.mask = size.bytes / 16 - 1;
                t.push.iterations = iterations;
                t.push.pattern = pattern;
                t.push.stride = k_ws_groups * k_ws_group_size;
                t.push.out_view = k_output_slot;
                t.groups = k_ws_groups;
                t.group_size = k_ws_group_size;
                
                tests.push_back( t );
            }
        }
    }

    // GpuBuffer path / descriptor index / typed alias / 8 buffers
    const struct { Pattern p; const char* label; const char* suffix; } patterns[] = {
        { Pattern::Uniform, "uniform", "uniform" }, { Pattern::Linear, "linear", "linear" }, { Pattern::Random, "random", "random" } };

    auto buf = [ & ]( Kind kind, const std::string& name, const std::string& shader, Pattern p, uint32_t width, bool divergent,
                       uint32_t needs, bool verify_on_regions ) {
        Test t;
        t.name = name;
        t.shader = shader;
        t.kind = kind;
        t.needs = needs;
        t.buf_pattern = p;
        t.buf_width = width;
        t.buf_divergent = divergent;
        t.verify_on_regions = verify_on_regions;
        t.push.src_view = kind == Kind::Divergent ? k_divergent_slot : k_input_slot;
        t.push.block_base = kind == Kind::Divergent ? k_region_block_base : k_input_block_base;
        t.push.out_view = k_output_slot;
        t.groups = buf_groups;
        t.group_size = 256;
        
        tests.push_back( t );
    };

    for ( uint32_t w : { 2u, 4u } ) {
        for ( auto& p : patterns ) {
            buf( Kind::BufferLoad, "buffer path: raw load<float" + std::to_string( w ) + "> (Slang LoadAligned / DXC Load<T>) " + p.label,
                 "bindless_buf_raw_aligned" + std::to_string( w ) + "d_" + p.suffix, p.p, w, false, 0, false );
        }
    }

    const struct { uint32_t width, align; } bda[] = { { 1, 4 }, { 4, 4 }, { 4, 16 } };

    for ( auto& b : bda ) {
        for ( auto& p : patterns ) {
            buf( Kind::BufferLoad, std::string( "buffer path: BDA float" ) + ( b.width > 1 ? std::to_string( b.width ) : "" ) + " align" + std::to_string( b.align ) + " " + p.label,
                 "bindless_buf_bda" + std::to_string( b.width ) + "d_align" + std::to_string( b.align ) + "_" + p.suffix, p.p, b.width, false, Need_Bda, false );
        }
    }

    for ( uint32_t w : { 1u, 4u } ) {
        for ( auto& p : patterns ) {
            buf( Kind::BufferLoad, std::string( "descriptor index: ByteAddressBuffer slot, uniform index .Load" ) + ( w > 1 ? "4 " : " " ) + p.label,
                 "bindless_buf_slot" + std::to_string( w ) + "d_" + p.suffix, p.p, w, false, 0, true );
        }
    }

    buf( Kind::BufferLoad, "descriptor index: ByteAddressBuffer 8 slots, NonUniform index .Load4 random", "bindless_buf_slot4d_nonuniform_random",
          Pattern::Random, 4, true, 0, true );

    for ( auto& p : patterns ) {
        buf( Kind::TypedAlias, std::string( "typed alias: StructuredBuffer<Light|Material>, uniform index " ) + p.label,
             std::string( "bindless_buf_alias_" ) + p.suffix, p.p, 4, false, 0, true );
    }

    buf( Kind::TypedAlias, "typed alias: StructuredBuffer<Light|Material>, NonUniform index random", "bindless_buf_alias_nonuniform_random",
          Pattern::Random, 4, true, 0, true );
    
    const struct { const char* name; const char* shader; Pattern p; bool divergent; uint32_t needs; } div[] = {
        { "8 slots, uniform index random (control)", "slots_uniform_random", Pattern::Random, false, 0 },
        { "8 slots, NonUniform index linear", "slots_nonuniform_linear", Pattern::Linear, true, 0 },
        { "8 slots, NonUniform index random", "slots_nonuniform_random", Pattern::Random, true, 0 },
        { "BDA align16, uniform base random (control)", "bda_uniform_random", Pattern::Random, false, Need_Bda },
        { "BDA align16, divergent base linear", "bda_divergent_linear", Pattern::Linear, true, Need_Bda },
        { "BDA align16, divergent base random", "bda_divergent_random", Pattern::Random, true, Need_Bda },
        { "one 128 KB slot, uniform offset random (control)", "offset_uniform_random", Pattern::Random, false, 0 },
        { "one 128 KB slot, divergent offset linear", "offset_divergent_linear", Pattern::Linear, true, 0 },
        { "one 128 KB slot, divergent offset random", "offset_divergent_random", Pattern::Random, true, 0 } };

    for ( auto& d : div ) {
        buf( Kind::Divergent, std::string( "8 buffers: " ) + d.name, std::string( "bindless_buf_div_" ) + d.shader, d.p, 4, d.divergent, d.needs, false );
    }
    
    // Store
    auto store = [ & ]( const std::string& name, const std::string& shader, uint32_t bytes, uint32_t ws_bytes, uint32_t pattern, uint32_t needs ) {
        Test t;
        t.name = name;
        t.shader = shader;
        t.kind = Kind::Store;
        t.needs = needs;
        t.store_bytes = bytes;
        t.push.src_view = k_store_slot;
        t.push.mask = ws_bytes / bytes - 1;
        t.push.iterations = ws_iterations( ws_bytes );
        t.push.pattern = pattern;
        t.push.stride = k_ws_groups * k_ws_group_size;
        t.groups = k_ws_groups;
        t.group_size = k_ws_group_size;

        tests.push_back( t );
        };

    const struct { const char* label; const char* shader; uint32_t bytes; uint32_t needs; } store_paths[] = {
        { "RWStructuredBuffer<uint>", "bindless_store_structured_4b", 4, 0 },
        { "RWStructuredBuffer<uint2>", "bindless_store_structured_8b", 8, 0 },
        { "RWStructuredBuffer<uint4>", "bindless_store_structured_16b", 16, 0 },
        { "RWByteAddressBuffer.Store", "bindless_store_raw_4b", 4, 0 },
        { "RWByteAddressBuffer.Store2", "bindless_store_raw_8b", 8, 0 },
        { "RWByteAddressBuffer.Store4", "bindless_store_raw_16b", 16, 0 },
        { "BDA 4 B", "bindless_store_bda_4b", 4, Need_Bda },
        { "BDA 8 B", "bindless_store_bda_8b", 8, Need_Bda },
        { "BDA 16 B", "bindless_store_bda_16b", 16, Need_Bda } };
    const char* store_patterns[] = { "linear", "random", "sparse" };      // shader pattern 0, 1, 2

    // Every path and width into 64 MB (DRAM), linear / sparse / random
    for ( uint32_t pattern : { 0u, 2u, 1u } ) {
        for ( auto& path : store_paths ) {
            store( std::string( "store 64 MB " ) + store_patterns[ pattern ] + ", " + std::to_string( ws_iterations( 64u << 20 ) ) + " stores: " + path.label,
                   path.shader, path.bytes, 64u << 20, pattern, path.needs );
        }
    }

    // Smaller working sets (L1, L2, last-level cache): 16-byte stores, typed view against raw Store4
    for ( auto& size : sizes ) {
        if ( size.bytes == ( 64u << 20 ) ) {
            continue;
        }
        for ( uint32_t pattern : { 0u, 1u } ) {
            const std::string prefix = std::string( "store " ) + size.label + " " + store_patterns[ pattern ] + ", " + std::to_string( ws_iterations( size.bytes ) ) + " stores: ";
            store( prefix + "RWStructuredBuffer<uint4>", "bindless_store_structured_16b", 16, size.bytes, pattern, 0 );
            store( prefix + "RWByteAddressBuffer.Store4", "bindless_store_raw_16b", 16, size.bytes, pattern, 0 );
        }
    }

    // Atomics
    auto atomic = [ & ]( const std::string& name, const char* shader, AtomicOp op, uint32_t targets, uint32_t active, uint32_t needs ) {
        Test t;
        t.name = "atomic " + name;
        t.shader = shader;
        t.kind = Kind::Atomic;
        t.atomic_op = op;
        t.needs = needs;
        t.push.src_view = k_atomic_slot;
        t.push.block_base = k_list_slot;
        t.push.stride = k_list_entries - 1;         // append: list index mask
        t.push.mask = targets - 1;
        t.push.pattern = active;                    // append: an operation is active if (hash & 1023) < pattern
        t.push.iterations = k_atomic_iterations;
        t.push.out_view = k_output_slot;
        t.groups = k_ws_groups;
        t.group_size = k_ws_group_size;

        tests.push_back( t );
    };

    // Append / compaction (GPU-driven culling, particle and light lists): one atomic per lane against one per wave
    for ( uint32_t percent : { 100u, 50u, 10u } ) {
        const std::string label = "append, " + std::to_string( percent ) + "% of lanes: ";
        const uint32_t active = percent * 1024 / 100;
        atomic( label + "InterlockedAdd per lane", "bindless_atomic_append_lane", AtomicOp::AppendLane, 1, active, 0 );
        atomic( label + "one InterlockedAdd per wave", "bindless_atomic_append_wave", AtomicOp::AppendWave, 1, active, Need_Ballot );
    }

    // Histogram (luminance, sort): global atomics against groupshared + one global add per bin per group,
    // uniform data and skewed data (every value in 16 of the 256 bins)
    for ( uint32_t bins : { 256u, 16u } ) {
        const std::string label = bins == 256 ? "histogram 256 bins, uniform: " : "histogram 256 bins, skewed (16 hot bins): ";
        atomic( label + "global InterlockedAdd", "bindless_atomic_add", AtomicOp::Add, bins, 0, 0 );
        atomic( label + "groupshared, then global", "bindless_atomic_hist_lds", AtomicOp::HistLds, bins, 0, 0 );
    }

    // Contention: the same random operations spread over 1, 64, 4096 or 1M addresses
    for ( uint32_t targets : { 1u, 64u, 4096u, 1u << 20 } ) {
        const std::string label = "counters " + ( targets == ( 1u << 20 ) ? std::string( "1M" ) : std::to_string( targets ) ) + ": ";
        atomic( label + "InterlockedAdd", "bindless_atomic_add", AtomicOp::Add, targets, 0, 0 );
        atomic( label + "InterlockedAdd, result used", "bindless_atomic_add_return", AtomicOp::AddReturn, targets, 0, 0 );
        atomic( label + "InterlockedOr", "bindless_atomic_or", AtomicOp::Or, targets, 0, 0 );
    }

    // Visibility-buffer style max: few targets (heavy overlap) and many targets
    for ( uint32_t targets : { 4096u, 1u << 20 } ) {
        const std::string label = "max, " + ( targets == ( 1u << 20 ) ? std::string( "1M" ) : std::to_string( targets ) ) + " targets: ";
        atomic( label + "InterlockedMax 32-bit", "bindless_atomic_max32", AtomicOp::Max32, targets, 0, 0 );
        atomic( label + "InterlockedMax 64-bit (depth << 32 | id)", "bindless_atomic_max64", AtomicOp::Max64, targets, 0, Need_Int64Atomics );
    }

    return tests;
}

// Element written by thread `tid` at iteration `i` of a store test (shaders/bindless/store_body.hlsli).
uint32_t store_element( const Test& t, uint32_t tid, uint32_t i ) {
    if ( t.push.pattern == 1 ) {
        return bench_hash( tid * t.push.iterations + i ) & t.push.mask;
    }
    const uint32_t spread = t.push.pattern == 2 ? 64 / t.store_bytes : 1;
    return ( ( tid + i * t.push.stride ) * spread ) & t.push.mask;
}

void transition( VkCommandBuffer cb, VkImage image, VkImageLayout from, VkImageLayout to, VkAccessFlags src, VkAccessFlags dst,
                 VkPipelineStageFlags src_stage, VkPipelineStageFlags dst_stage ) {
    
    VkImageMemoryBarrier b{ VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
    b.srcAccessMask = src;
    b.dstAccessMask = dst;
    b.oldLayout = from;
    b.newLayout = to;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = image;
    b.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
    
    vkCmdPipelineBarrier( cb, src_stage, dst_stage, 0, 0, nullptr, 0, nullptr, 1, &b );
}

void memory_barrier( VkCommandBuffer cb, VkAccessFlags src, VkAccessFlags dst, VkPipelineStageFlags src_stage, VkPipelineStageFlags dst_stage ) {
    VkMemoryBarrier b{ VK_STRUCTURE_TYPE_MEMORY_BARRIER };
    b.srcAccessMask = src;
    b.dstAccessMask = dst;
    
    vkCmdPipelineBarrier( cb, src_stage, dst_stage, 0, 1, &b, 0, nullptr, 0, nullptr );
}

} // namespace

struct BindlessSuite {
    std::vector<Test>                   tests;
    BindlessLayout                      layout;
    bool                                ready = false;
    GpuBuffer                           data, output, staging, store_target;
    GpuBuffer                           input, divergent;          // buffer tests: 16 KB input, 128 KB = 8 regions
    GpuBuffer                           atomic_target, atomic_list; // atomic tests: 8 MB counters / targets, 8 MB append list
    GpuImage                            textures[ k_tex_count ];
    std::map<std::string, VkPipeline>   pipelines;
    VkDeviceAddress                     data_address = 0, input_address = 0, divergent_address = 0, store_address = 0;
};

std::vector<std::string> bindless_test_names() {

    std::vector<std::string> names;

    for ( const Test& t : build_tests( 4096 ) ) {
        names.push_back( t.name );
    }
    return names;
}

BindlessSuite* bindless_create( Gpu& gpu, const AppOptions& opt, const std::string& shader_dir ) {
    BindlessSuite* s = new BindlessSuite;

    for ( const Test& t : build_tests( opt.groups_x* opt.groups_y ) ) {
        if ( opt.filter.empty() || t.name.find( opt.filter ) != std::string::npos ) {
            s->tests.push_back( t );
        }
    }

    if ( s->tests.empty() ) {
        return s;
    }

    auto skip_all = [ & ]( const std::string& reason ) {
        for ( Test& t : s->tests ) {
            t.skip_reason = reason;
        }
        return s;
    };

    if ( !gpu.features.bindless ) {
        return skip_all( gpu.features.bindless_missing );
    }

    // Layout, buffers, textures
    VK_CHECK( bindless_layout_create( gpu.physical, gpu.device, BindlessLayoutDesc{}, s->layout ) );
    if ( s->layout.texture_capacity < k_tex_count || s->layout.buffer_capacity < k_buffer_slots ) {
        return skip_all( "bindless layout: device limits too low" );
    }

    s->data = gpu_create_buffer( gpu, ( VkDeviceSize )k_data_elements * 16, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT );
    s->output = gpu_create_buffer( gpu, ( VkDeviceSize )k_max_threads * 16, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT );
    s->staging = gpu_create_host_buffer( gpu, ( VkDeviceSize )k_tex_count * k_tex_size * k_tex_size * 4,
                                     VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT );
    s->data_address = gpu_buffer_address( gpu, s->data );
    s->input = gpu_create_buffer( gpu, k_input_bytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT );
    s->divergent = gpu_create_buffer( gpu, 8 * k_region_bytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT );
    s->input_address = gpu_buffer_address( gpu, s->input );
    s->divergent_address = gpu_buffer_address( gpu, s->divergent );
    s->store_target = gpu_create_buffer( gpu, k_store_bytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT |
                                         VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT );
    s->store_address = gpu_buffer_address( gpu, s->store_target );

    bindless_write_buffer( gpu.device, s->layout, k_data_slot, s->data.buffer );
    bindless_write_buffer( gpu.device, s->layout, k_output_slot, s->output.buffer );
    bindless_write_buffer( gpu.device, s->layout, k_input_slot, s->input.buffer );

    for ( uint32_t k = 0; k < 8; ++k ) {
        // 8 slots, same 16 KB
        bindless_write_buffer( gpu.device, s->layout, k_input_block_base + k, s->input.buffer );
        // 8 regions
        bindless_write_buffer( gpu.device, s->layout, k_region_block_base + k, s->divergent.buffer, k * k_region_bytes, k_region_bytes );
    }
    bindless_write_buffer( gpu.device, s->layout, k_divergent_slot, s->divergent.buffer );
    bindless_write_buffer( gpu.device, s->layout, k_store_slot, s->store_target.buffer );

    const VkBufferUsageFlags atomic_usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    s->atomic_target = gpu_create_buffer( gpu, ( VkDeviceSize )k_atomic_targets * 8, atomic_usage );
    s->atomic_list = gpu_create_buffer( gpu, ( VkDeviceSize )k_list_entries * 4, atomic_usage );
    bindless_write_buffer( gpu.device, s->layout, k_atomic_slot, s->atomic_target.buffer );
    bindless_write_buffer( gpu.device, s->layout, k_list_slot, s->atomic_list.buffer );

    uint32_t* texels = ( uint32_t* )s->staging.mapped;
    for ( uint32_t t = 0; t < k_tex_count; ++t ) {
        for ( uint32_t y = 0; y < k_tex_size; ++y ) {
            for ( uint32_t x = 0; x < k_tex_size; ++x ) {
                *texels++ = texel( t, x, y );
            }
        }
    }

    for ( uint32_t t = 0; t < k_tex_count; ++t ) {
        s->textures[ t ] = gpu_create_image( gpu, VK_FORMAT_R8G8B8A8_UNORM, k_tex_size, k_tex_size, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT );
        bindless_write_sampled_image( gpu.device, s->layout, k_texture_base + t, s->textures[ t ].view );
    }

    // Pipelines
    auto pipeline = [ & ]( const std::string& shader, std::string& error ) -> VkPipeline {
        auto it = s->pipelines.find( shader );
        if ( it != s->pipelines.end() ) {
            if ( !it->second ) error = "pipeline unavailable (" + shader + ")";
            return it->second;
        }

        VkPipeline p = gpu_create_compute_pipeline( gpu, s->layout.pipeline_layout, shader_dir + "/" + shader + ".spv", error );
        s->pipelines[ shader ] = p;
        
        return p;
    };

    std::string fill_error;
    VkPipeline fill = pipeline( "bindless_fill", fill_error );

    if ( !fill ) {
        return skip_all( "bindless shaders not available: " + fill_error );
    }

    for ( Test& t : s->tests ) {

        if ( ( t.needs & Need_Bda ) && !gpu.features.buffer_device_address ) {
            t.skip_reason = "bufferDeviceAddress + shaderInt64 not supported";
            continue;
        }

        if ( ( t.needs & Need_Int64Atomics ) && !gpu.features.buffer_int64_atomics ) {
            t.skip_reason = "shaderBufferInt64Atomics not supported";
            continue;
        }

        if ( ( t.needs & Need_Ballot ) && !gpu.features.subgroup_ballot ) {
            t.skip_reason = "subgroup ballot not supported in compute";
            continue;
        }

        const VkDeviceAddress address = t.kind == Kind::BufferLoad ? s->input_address : t.kind == Kind::Divergent ? s->divergent_address :
                                        t.kind == Kind::Store ? s->store_address : s->data_address;
        t.push.addr_lo = ( uint32_t )( address & 0xffffffffu );
        t.push.addr_hi = ( uint32_t )( address >> 32 );
        t.pipeline = pipeline( t.shader, t.skip_reason );
    }

    // Initial contents: texture upload + data fill
    VkCommandBuffer cb = gpu_allocated_and_begin_cb( gpu );

    for ( uint32_t t = 0; t < k_tex_count; ++t ) {
        transition( cb, s->textures[ t ].image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, VK_ACCESS_TRANSFER_WRITE_BIT,
                    VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT );

        VkBufferImageCopy copy{};
        copy.bufferOffset = ( VkDeviceSize )t * k_tex_size * k_tex_size * 4;
        copy.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
        copy.imageExtent = { k_tex_size, k_tex_size, 1 };
        
        vkCmdCopyBufferToImage( cb, s->staging.buffer, s->textures[ t ].image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy );
        
        transition( cb, s->textures[ t ].image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                    VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT );
    }
    
    vkCmdFillBuffer( cb, s->output.buffer, 0, VK_WHOLE_SIZE, 0 );
    vkCmdFillBuffer( cb, s->store_target.buffer, 0, VK_WHOLE_SIZE, 0 );
    vkCmdFillBuffer( cb, s->atomic_target.buffer, 0, VK_WHOLE_SIZE, 0 );
    vkCmdFillBuffer( cb, s->atomic_list.buffer, 0, VK_WHOLE_SIZE, 0 );
    
    // GpuBuffer tests (buffer path, descriptor index, typed alias, 8 buffers): input word k = float((k * 3) & 7); region r of the divergent buffer = float(r + 1).
    std::vector<float> input_words( k_input_bytes / 4 );

    for ( uint32_t k = 0; k < input_words.size(); ++k ) {
        input_words[ k ] = buf_input_word( k );
    }

    vkCmdUpdateBuffer( cb, s->input.buffer, 0, k_input_bytes, input_words.data() );

    for ( uint32_t r = 0; r < 8; ++r ) {
        const float v = ( float )( r + 1 );
        uint32_t bits;
        memcpy( &bits, &v, 4 );
        
        vkCmdFillBuffer( cb, s->divergent.buffer, r * k_region_bytes, k_region_bytes, bits );
    }
    memory_barrier( cb, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT,
                    VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT );

    BenchPush push{};
    push.src_view = k_data_slot;
    push.mask = k_data_elements - 1;
    
    vkCmdBindPipeline( cb, VK_PIPELINE_BIND_POINT_COMPUTE, fill );
    
    cmd_bind_bindless_layout( cb, s->layout, VK_PIPELINE_BIND_POINT_COMPUTE );
    cmd_bindless_push_constants( cb, s->layout, &push, sizeof( push ) );
    
    vkCmdDispatch( cb, k_data_elements / 256, 1, 1 );
    
    memory_barrier( cb, VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT );
    
    gpu_end_cb_submit_wait_free_cb( gpu, cb );
    
    s->ready = true;
    
    return s;
}

void bindless_append_tests( BindlessSuite* s, std::vector<AppRunnableTest>& out ) {
    for ( const Test& t : s->tests ) {
        AppRunnableTest r;
        r.suite = "bindless";
        r.name = t.name;
        r.skip_reason = t.pipeline ? std::string() : ( t.skip_reason.empty() ? std::string( "pipeline unavailable" ) : t.skip_reason );
        
        if ( t.pipeline ) {
            const VkPipeline pipeline = t.pipeline;
            const BindlessLayout* layout = &s->layout;
            const BenchPush push = t.push;
            const uint32_t groups = t.groups;
            
            r.record = [ = ]( VkCommandBuffer cmd ) {
                vkCmdBindPipeline( cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline );
                cmd_bind_bindless_layout( cmd, *layout, VK_PIPELINE_BIND_POINT_COMPUTE );
                cmd_bindless_push_constants( cmd, *layout, &push, sizeof( push ) );
                vkCmdDispatch( cmd, groups, 1, 1 );
            };
        }
        out.push_back( std::move( r ) );
    }
}

// --verify

// Value accumulated by lane `gix` of a buffer test in --verify mode (block_index = 3, slot tests on the 8 regions).
static void buf_value( const Test& t, uint32_t gix, float v[ 4 ] ) {

    v[ 0 ] = v[ 1 ] = v[ 2 ] = v[ 3 ] = 0.0f;

    const uint32_t region = t.buf_divergent ? ( perftest_hash1( gix ) >> 8 ) & 7 : 3;
    if ( t.kind != Kind::BufferLoad || t.verify_on_regions ) {
        // Every load returns (region + 1) in every component: 256 loads (the alias tests do 128 x 2).
        for ( int c = 0; c < 4; ++c ) {
            v[ c ] = 256.0f * ( float )( region + 1 );
        }
        return;
    }

    const uint32_t w = t.buf_width;
    uint32_t htid = t.buf_pattern == Pattern::Uniform ? 0 : t.buf_pattern == Pattern::Linear ? gix : perftest_hash1( gix ) & 0xf;
    htid *= 4 * w;
    
    for ( uint32_t i = 0; i < 256; ++i ) {
        const uint32_t k = ( htid + i * 4 * w ) / 4;

        for ( int c = 0; c < 4; ++c ) {
            v[ c ] += buf_input_word( k + ( w == 1 ? 0 : w == 2 ? ( c & 1 ) : c ) );
        }
    }
}

// Element read by thread `tid` at iteration `i` of a working-set test (shaders/bindless/ws_body.hlsli). The wave
// patterns assume that wave w holds threads w * wave_size .. w * wave_size + wave_size - 1.
static uint32_t ws_element( const Test& t, uint32_t tid, uint32_t i, uint32_t wave_size ) {
    const uint32_t wave = tid / wave_size, lane = tid % wave_size;
    switch ( t.push.pattern ) {
        case 0:  return ( tid + i * t.push.stride ) & t.push.mask;
        case 1:  return bench_hash( tid * t.push.iterations + i ) & t.push.mask;
        case 2:  return bench_hash( wave * t.push.iterations + i ) & t.push.mask;
        default: return ( bench_hash( wave * t.push.iterations + i ) * wave_size + lane ) & t.push.mask;
    }
}

static bool check_thread( const Test& t, uint32_t tid, const uint32_t got[ 4 ], uint32_t wave_size, std::string& why ) {
    uint32_t expected[ 4 ] = { 0, 0, 0, 0 };

    if ( t.kind == Kind::BufferLoad || t.kind == Kind::TypedAlias || t.kind == Kind::Divergent ) {
        const uint32_t gix = tid % 256;
        float v[ 4 ], n[ 4 ];
        
        buf_value( t, gix, v );
        buf_value( t, ( gix + 1 ) & 255, n );         // w = the neighbour's sum, read back through LDS
        
        const float neighbour_sum = ( ( n[ 0 ] + n[ 1 ] ) + n[ 2 ] ) + n[ 3 ];
        
        memcpy( expected, v, 12 );
        memcpy( &expected[ 3 ], &neighbour_sum, 4 );
    }
    else if ( t.kind == Kind::WorkingSet ) {
        for ( uint32_t i = 0; i < t.push.iterations; ++i ) {
            const uint32_t e = ws_element( t, tid, i, wave_size );

            uint32_t v[ 4 ];
            bench_pattern( t.push.src_first + e, v );
            for ( int c = 0; c < 4; ++c ) {
                expected[ c ] += v[ c ];
            }
        }
    }
    else {
        const uint32_t index = got[ 3 ];     // texture the shader says it used
        const uint32_t limit = t.shader == "bindless_tex_uniform" ? 1 : t.push.distinct;
        if ( index >= limit ) {
            why = "texture index " + std::to_string( index ) + " outside the " + std::to_string( limit ) + " allowed";
            return false;
        }
        expected[ 3 ] = index;
        for ( uint32_t i = 0; i < t.push.iterations; ++i ) {
            const uint32_t h = bench_hash( tid * t.push.iterations + i );
            const uint32_t v = texel( t.push.tex_base + index, h & ( k_tex_size - 1 ), ( h >> 6 ) & ( k_tex_size - 1 ) );
            for ( int c = 0; c < 3; ++c ) {
                expected[ c ] += ( v >> ( 8 * c ) ) & 0xFF;
            }
        }
    }

    if ( memcmp( expected, got, 16 ) == 0 ) {
        return true;
    }

    char buf[ 160 ];
    snprintf( buf, sizeof( buf ), "got (%08x %08x %08x %08x), expected (%08x %08x %08x %08x)", got[ 0 ], got[ 1 ], got[ 2 ], got[ 3 ],
              expected[ 0 ], expected[ 1 ], expected[ 2 ], expected[ 3 ] );
    why = buf;
    return false;
}

// Store tests: clear the working set, run k_verify_threads threads, read the working set back and check every element:
// written elements hold store_value(e) (bench_pattern(e), first 1, 2 or 4 words), the others are still zero.
static bool verify_store( Gpu& gpu, BindlessSuite* s, const Test& t, GpuBuffer& readback, std::string& why ) {
    
    const uint32_t words = t.store_bytes / 4;
    const uint32_t elements = t.push.mask + 1;
    const VkDeviceSize bytes = ( VkDeviceSize )elements * t.store_bytes;

    VkCommandBuffer cb = gpu_allocated_and_begin_cb( gpu );
    vkCmdFillBuffer( cb, s->store_target.buffer, 0, bytes, 0 );
    memory_barrier( cb, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_WRITE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT );

    vkCmdBindPipeline( cb, VK_PIPELINE_BIND_POINT_COMPUTE, t.pipeline );
    cmd_bind_bindless_layout( cb, s->layout, VK_PIPELINE_BIND_POINT_COMPUTE );
    cmd_bindless_push_constants( cb, s->layout, &t.push, sizeof( t.push ) );
    vkCmdDispatch( cb, k_verify_threads / t.group_size, 1, 1 );
    memory_barrier( cb, VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT );

    const VkBufferCopy copy{ 0, 0, bytes };
    vkCmdCopyBuffer( cb, s->store_target.buffer, readback.buffer, 1, &copy );
    memory_barrier( cb, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_HOST_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT );
    gpu_end_cb_submit_wait_free_cb( gpu, cb );

    std::vector<bool> written( elements, false );

    for ( uint32_t tid = 0; tid < k_verify_threads; ++tid ) {
        for ( uint32_t i = 0; i < t.push.iterations; ++i ) {
            written[ store_element( t, tid, i ) ] = true;
        }
    }

    const uint32_t* got = ( const uint32_t* )readback.mapped;
    uint32_t wrong = 0, first = 0;

    for ( uint32_t e = 0; e < elements; ++e ) {
        uint32_t expected[ 4 ] = { 0, 0, 0, 0 };
        if ( written[ e ] ) {
            bench_pattern( e, expected );
        }
        if ( memcmp( got + ( size_t )e * words, expected, words * 4 ) != 0 && wrong++ == 0 ) {
            first = e;
        }
    }

    if ( wrong == 0 ) {
        return true;
    }

    uint32_t expected[ 4 ] = { 0, 0, 0, 0 };
    if ( written[ first ] ) {
        bench_pattern( first, expected );
    }

    char buf[ 200 ];
    snprintf( buf, sizeof( buf ), "%u/%u elements wrong (element %u: got %08x..., expected %08x...%s)", wrong, elements, first,
              got[ ( size_t )first * words ], expected[ 0 ], written[ first ] ? "" : ", never written" );
    
    why = buf;
    
    return false;
}

// Atomic tests: clear targets and list, run k_verify_threads threads, read back and compare with the CPU. The results
// do not depend on the order of the operations: counts, OR masks and maxima are exact; for append, the counter must be
// the number of active operations and the list must hold every active id exactly once.
static bool verify_atomic( Gpu& gpu, BindlessSuite* s, const Test& t, GpuBuffer& readback, std::string& why ) {

    const bool is64 = t.atomic_op == AtomicOp::Max64;
    const bool append = t.atomic_op == AtomicOp::AppendLane || t.atomic_op == AtomicOp::AppendWave;
    const uint32_t targets = t.push.mask + 1;
    const uint32_t operations = k_verify_threads * t.push.iterations;
    const VkDeviceSize target_bytes = ( VkDeviceSize )targets * ( is64 ? 8 : 4 );

    VkCommandBuffer cb = gpu_allocated_and_begin_cb( gpu );
    vkCmdFillBuffer( cb, s->atomic_target.buffer, 0, VK_WHOLE_SIZE, 0 );
    vkCmdFillBuffer( cb, s->atomic_list.buffer, 0, VK_WHOLE_SIZE, 0 );
    memory_barrier( cb, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT,
                    VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT );

    vkCmdBindPipeline( cb, VK_PIPELINE_BIND_POINT_COMPUTE, t.pipeline );
    cmd_bind_bindless_layout( cb, s->layout, VK_PIPELINE_BIND_POINT_COMPUTE );
    cmd_bindless_push_constants( cb, s->layout, &t.push, sizeof( t.push ) );
    vkCmdDispatch( cb, k_verify_threads / t.group_size, 1, 1 );
    memory_barrier( cb, VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT );

    const VkBufferCopy target_copy{ 0, 0, target_bytes };
    vkCmdCopyBuffer( cb, s->atomic_target.buffer, readback.buffer, 1, &target_copy );
    if ( append ) {
        const VkBufferCopy list_copy{ 0, target_bytes, ( VkDeviceSize )operations * 4 };
        vkCmdCopyBuffer( cb, s->atomic_list.buffer, readback.buffer, 1, &list_copy );
    }
    memory_barrier( cb, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_HOST_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT );
    gpu_end_cb_submit_wait_free_cb( gpu, cb );

    const uint32_t* got32 = ( const uint32_t* )readback.mapped;
    const uint64_t* got64 = ( const uint64_t* )readback.mapped;
    char buf[ 200 ];

    if ( append ) {
        std::vector<uint32_t> expected;
        for ( uint32_t id = 0; id < operations; ++id ) {
            if ( ( bench_hash( id ) & 1023 ) < t.push.pattern ) {
                expected.push_back( id );
            }
        }
        if ( got32[ 0 ] != expected.size() ) {
            snprintf( buf, sizeof( buf ), "counter %u, expected %u", got32[ 0 ], ( uint32_t )expected.size() );
            why = buf;
            return false;
        }
        const uint32_t* list = got32 + target_bytes / 4;
        std::vector<uint32_t> sorted( list, list + expected.size() );
        std::sort( sorted.begin(), sorted.end() );
        if ( sorted != expected ) {
            why = "the list does not hold every active id exactly once";
            return false;
        }
        return true;
    }

    std::vector<uint64_t> expected( targets, 0 );
    for ( uint32_t id = 0; id < operations; ++id ) {
        uint64_t& e = expected[ bench_hash( id ) & t.push.mask ];
        const uint32_t value = bench_hash( id ^ 0x5bd1e995u );
        switch ( t.atomic_op ) {
            case AtomicOp::Or:    e |= 1u << ( id & 31 ); break;
            case AtomicOp::Max32: e = std::max<uint64_t>( e, value ); break;
            case AtomicOp::Max64: e = std::max<uint64_t>( e, ( ( uint64_t )value << 32 ) | id ); break;
            default:              e += 1; break;      // Add, AddReturn, HistLds
        }
    }

    uint32_t wrong = 0, first = 0;
    for ( uint32_t k = 0; k < targets; ++k ) {
        const uint64_t g = is64 ? got64[ k ] : got32[ k ];
        if ( g != expected[ k ] && wrong++ == 0 ) {
            first = k;
        }
    }
    if ( wrong == 0 ) {
        return true;
    }

    snprintf( buf, sizeof( buf ), "%u/%u targets wrong (target %u: got %llx, expected %llx)", wrong, targets, first,
              ( unsigned long long )( is64 ? got64[ first ] : got32[ first ] ), ( unsigned long long )expected[ first ] );
    why = buf;
    return false;
}

bool bindless_verify( Gpu& gpu, BindlessSuite* s ) {
    bool all_ok = true;

    if ( s->tests.empty() ) {
        return true;
    }

    if ( !s->ready ) {
        printf( "SKIP  bindless suite (%s)\n", s->tests[ 0 ].skip_reason.c_str() );
        return true;
    }

    const uint32_t wave_size = gpu.required_subgroup_size ? gpu.required_subgroup_size : gpu.subgroup_size;
    GpuBuffer readback = gpu_create_host_buffer( gpu, ( VkDeviceSize )k_verify_threads * 16, VK_BUFFER_USAGE_TRANSFER_DST_BIT );
    GpuBuffer store_readback;      // created on the first store test
    GpuBuffer atomic_readback;     // created on the first atomic test

    for ( const Test& t : s->tests ) {
        if ( !t.pipeline ) {
            printf( "SKIP  %s (%s)\n", t.name.c_str(), t.skip_reason.c_str() );
            continue;
        }

        if ( t.kind == Kind::Atomic ) {
            if ( !atomic_readback.buffer ) {
                atomic_readback = gpu_create_host_buffer( gpu, ( VkDeviceSize )k_atomic_targets * 8 + ( VkDeviceSize )k_list_entries * 4,
                                                          VK_BUFFER_USAGE_TRANSFER_DST_BIT );
            }

            std::string why;

            if ( verify_atomic( gpu, s, t, atomic_readback, why ) ) {
                printf( "PASS  %s\n", t.name.c_str() );
            }
            else {
                all_ok = false;
                printf( "FAIL  %s: %s\n", t.name.c_str(), why.c_str() );
            }
            continue;
        }

        if ( t.kind == Kind::Store ) {
            if ( !store_readback.buffer ) {
                store_readback = gpu_create_host_buffer( gpu, k_store_bytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT );
            }

            std::string why;

            if ( verify_store( gpu, s, t, store_readback, why ) ) {
                printf( "PASS  %s\n", t.name.c_str() );
            }
            else {
                all_ok = false;
                printf( "FAIL  %s: %s\n", t.name.c_str(), why.c_str() );
            }
            continue;
        }

        BenchPush push = t.push;
        push.verify = 1;
        if ( t.verify_on_regions ) {
            push.block_base = k_region_block_base;     // distinct content per slot: a wrong slot reads a wrong value
        }

        if ( t.kind == Kind::BufferLoad || t.kind == Kind::TypedAlias || t.kind == Kind::Divergent ) {
            push.block_index = 3;
        }

        VkCommandBuffer cb = gpu_allocated_and_begin_cb( gpu );
        vkCmdFillBuffer( cb, s->output.buffer, 0, VK_WHOLE_SIZE, 0 );
        memory_barrier( cb, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_WRITE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT );
        
        vkCmdBindPipeline( cb, VK_PIPELINE_BIND_POINT_COMPUTE, t.pipeline );
        cmd_bind_bindless_layout( cb, s->layout, VK_PIPELINE_BIND_POINT_COMPUTE );
        cmd_bindless_push_constants( cb, s->layout, &push, sizeof( push ) );
        vkCmdDispatch( cb, k_verify_threads / t.group_size, 1, 1 );
        memory_barrier( cb, VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT );
        
        const VkBufferCopy copy{ 0, 0, ( VkDeviceSize )k_verify_threads * 16 };
        vkCmdCopyBuffer( cb, s->output.buffer, readback.buffer, 1, &copy );
        memory_barrier( cb, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_HOST_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT );
        gpu_end_cb_submit_wait_free_cb( gpu, cb );

        const uint32_t* got = ( const uint32_t* )readback.mapped;
        int wrong = 0;
        std::string first_why;
        uint32_t first_tid = 0;

        for ( uint32_t tid = 0; tid < k_verify_threads; ++tid ) {
            std::string why;
            if ( !check_thread( t, tid, got + tid * 4, wave_size, why ) && wrong++ == 0 ) {
                first_why = why;
                first_tid = tid;
            }
        }
        if ( wrong == 0 ) {
            printf( "PASS  %s\n", t.name.c_str() );
        } else {
            all_ok = false;
            printf( "FAIL  %s: %d/%u threads wrong (thread %u: %s)\n", t.name.c_str(), wrong, k_verify_threads, first_tid, first_why.c_str() );
        }
    }

    gpu_destroy_buffer( gpu, readback );
    gpu_destroy_buffer( gpu, store_readback );
    gpu_destroy_buffer( gpu, atomic_readback );

    return all_ok;
}

void bindless_destroy( Gpu& gpu, BindlessSuite* s ) {
    for ( auto& [name, p] : s->pipelines ) {
        if ( p ) {
            vkDestroyPipeline( gpu.device, p, nullptr );
        }
    }

    if ( s->layout.pipeline_layout ) {
        bindless_layout_destroy( gpu.device, s->layout );
    }

    for ( GpuImage& img : s->textures ) {
        gpu_destroy_image( gpu, img );
    }

    for ( GpuBuffer* b : { &s->data, &s->output, &s->staging, &s->input, &s->divergent, &s->store_target, &s->atomic_target, &s->atomic_list } ) {
        gpu_destroy_buffer( gpu, *b );
    }

    delete s;
}
