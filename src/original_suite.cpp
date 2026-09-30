// original_suite.cpp - the original PerfTest tests (see original_suite.hpp).
#include "original_suite.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <map>

//////////////////////////////////////////////////////////////////////////
// Constant structs (must match shaders/loadConstantsGPU.h)
//////////////////////////////////////////////////////////////////////////

struct LoadConstants {
    uint32_t elementsMask;
    uint32_t writeIndex;
    uint32_t readStartAddress;
    uint32_t padding;
};

struct LoadConstantsWithArray {
    uint32_t elementsMask;
    uint32_t writeIndex;
    uint32_t readStartAddress;
    uint32_t padding;
    float    benchmarkArray[ 1024 ][ 4 ];     // 16 KB
};

static_assert( sizeof( LoadConstants ) == 16, "layout" );
static_assert( sizeof( LoadConstantsWithArray ) == 16 + 16384, "layout" );

//////////////////////////////////////////////////////////////////////////
// Test description
//////////////////////////////////////////////////////////////////////////

// What binding 1 holds for a test.
enum class SourceKind { TypedBuffer, StorageBuffer, SampledImage, None, Count };

enum class CbKind { Aligned, Unaligned, WithArray };

enum Requirement : uint32_t {
    Req_None                = 0,
    Req_BigUniformBuffer    = 1u << 0,     // maxUniformBufferRange >= sizeof(LoadConstantsWithArray)
    Req_LinearFilter        = 1u << 1,     // SAMPLED_IMAGE_FILTER_LINEAR for the image format
};

struct TestCase {
    std::string name;
    std::string shader;          // .spv base name
    SourceKind  kind;
    CbKind      cb;
    VkFormat    format   = VK_FORMAT_UNDEFINED;   // typed buffer view / texture format
    uint32_t    structured_stride = 0;             // StorageBuffer: 0 = raw input buffer, else structured buffer stride
    bool        bilinear = false;
    uint32_t    requirements = Req_None;
};

struct FormatInfo {
    VkFormat    format;
    const char* buffer_name;   // name used by the original for Buffer<...>
    const char* texture_name;  // name used by the original for Texture2D<...>
    uint32_t    size;
    int         width;         // 1d / 2d / 4d shader variant
};

// Order matches the original test list.
static const FormatInfo k_formats[] = {
    { VK_FORMAT_R8_UNORM,            "R8",      "R8",      1,  1 },
    { VK_FORMAT_R8G8_UNORM,          "RG8",     "RG8",     2,  2 },
    { VK_FORMAT_R8G8B8A8_UNORM,      "RGBA8",   "RGBA8",   4,  4 },
    { VK_FORMAT_R16_SFLOAT,          "R16f",    "R16F",    2,  1 },
    { VK_FORMAT_R16G16_SFLOAT,       "RG16f",   "RG16F",   4,  2 },
    { VK_FORMAT_R16G16B16A16_SFLOAT, "RGBA16f", "RGBA16F", 8,  4 },
    { VK_FORMAT_R32_SFLOAT,          "R32f",    "R32F",    4,  1 },
    { VK_FORMAT_R32G32_SFLOAT,       "RG32f",   "RG32F",   8,  2 },
    { VK_FORMAT_R32G32B32A32_SFLOAT, "RGBA32f", "RGBA32F", 16, 4 },
};

static const char* k_patterns[ 3 ][ 2 ] = { { "Invariant", "uniform" }, { "Linear", "linear" }, { "Random", "random" } };

static std::vector<TestCase> build_test_list() {
    std::vector<TestCase> tests;
    auto add = [ & ]( const std::string& name, const std::string& shader, SourceKind kind, CbKind cb ) -> TestCase& {
        TestCase t;
        t.name = name;
        t.shader = shader;
        t.kind = kind;
        t.cb = cb;
        tests.push_back( t );
        return tests.back();
    };

    // Typed buffers: Buffer<format>.Load
    for ( const FormatInfo& f : k_formats ) {
        for ( auto& p : k_patterns ) {
            add( std::string( "Buffer<" ) + f.buffer_name + ">.Load " + p[ 1 ],
                 "loadTyped" + std::to_string( f.width ) + "d" + p[ 0 ], SourceKind::TypedBuffer, CbKind::Aligned ).format = f.format;
        }
    }

    // Raw buffers: ByteAddressBuffer.Load/Load2/Load3/Load4 (aligned), then Load2/Load4 unaligned
    for ( int w = 1; w <= 4; ++w ) {
        for ( auto& p : k_patterns ) {
            add( std::string( "ByteAddressBuffer.Load" ) + ( w > 1 ? std::to_string( w ) : "" ) + " " + p[ 1 ],
                 "loadRaw" + std::to_string( w ) + "d" + p[ 0 ], SourceKind::StorageBuffer, CbKind::Aligned );
        }
    }

    for ( int w : { 2, 4 } ) {
        for ( auto& p : k_patterns ) {
            add( "ByteAddressBuffer.Load" + std::to_string( w ) + " unaligned " + p[ 1 ],
                 "loadRaw" + std::to_string( w ) + "d" + p[ 0 ], SourceKind::StorageBuffer, CbKind::Unaligned );
        }
    }

    // Structured buffers
    const struct { int width; const char* type; uint32_t stride; } structured[] = { { 1, "float", 4 }, { 2, "float2", 8 }, { 4, "float4", 16 } };
    for ( auto& s : structured ) {
        for ( auto& p : k_patterns ) {
            add( std::string( "StructuredBuffer<" ) + s.type + ">.Load " + p[ 1 ],
                 "loadStructured" + std::to_string( s.width ) + "d" + p[ 0 ], SourceKind::StorageBuffer, CbKind::Aligned ).structured_stride = s.stride;
        }
    }

    // Constant buffer float4 array
    for ( auto& p : k_patterns ) {
        add( std::string( "cbuffer{float4} load " ) + p[ 1 ], std::string( "loadConstant4d" ) + p[ 0 ], SourceKind::None, CbKind::WithArray ).requirements |= Req_BigUniformBuffer;
    }

    // Texture2D.Load
    for ( const FormatInfo& f : k_formats ) {
        for ( auto& p : k_patterns ) {
            add( std::string( "Texture2D<" ) + f.texture_name + ">.Load " + p[ 1 ],
                 "loadTex" + std::to_string( f.width ) + "d" + p[ 0 ], SourceKind::SampledImage, CbKind::Aligned ).format = f.format;
        }
    }

    // Texture2D.Sample nearest, then bilinear
    for ( int bilinear = 0; bilinear < 2; ++bilinear ) {
        for ( const FormatInfo& f : k_formats ) {
            for ( auto& p : k_patterns ) {
                TestCase& t = add( std::string( "Texture2D<" ) + f.texture_name + ">.Sample(" + ( bilinear ? "bilinear" : "nearest" ) + ") " + p[ 1 ],
                                   "sampleTex" + std::to_string( f.width ) + "d" + p[ 0 ], SourceKind::SampledImage, CbKind::Aligned );
                t.format = f.format;
                t.bilinear = bilinear != 0;
                if ( bilinear ) {
                    t.requirements |= Req_LinearFilter;
                }
            }
        }
    }

    return tests;
}

//////////////////////////////////////////////////////////////////////////
// Benchmark resources
//////////////////////////////////////////////////////////////////////////

struct Resources {
    GpuBuffer                           input;                  // 16 KB: typed views and raw loads
    GpuBuffer                           structured[ 3 ];        // strides 4, 8, 16 (1024 elements each)
    GpuBuffer                           output;                 // 2048 floats, RWBuffer<float>
    VkBufferView                        output_view = VK_NULL_HANDLE;
    std::map<VkFormat, VkBufferView>    typed_views;
    std::map<VkFormat, GpuImage>           textures;
    std::map<VkFormat, VkFormatFeatureFlags> texture_features;
    GpuBuffer                           cb_aligned, cb_unaligned, cb_with_array;
    VkSampler                           sampler_nearest  = VK_NULL_HANDLE;
    VkSampler                           sampler_bilinear = VK_NULL_HANDLE;

    VkDescriptorSetLayout               set_layouts[ ( int )SourceKind::Count ] = {};
    VkPipelineLayout                    pipeline_layouts[ ( int )SourceKind::Count ] = {};
    VkDescriptorPool                    descriptor_pool = VK_NULL_HANDLE;
    std::map<std::string, VkPipeline>   pipelines;               // key: shader + kind
    std::map<std::string, std::string>  pipeline_errors;
};

static VkDescriptorType source_descriptor_type( SourceKind k ) {
    switch ( k ) {
        case SourceKind::TypedBuffer:         return VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER;
        case SourceKind::StorageBuffer:       return VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        case SourceKind::SampledImage:        return VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
        default:                              return VK_DESCRIPTOR_TYPE_MAX_ENUM;
    }
}

static void create_resources( Gpu& gpu, Resources& r ) {
    r.input = gpu_create_buffer( gpu, 1024 * 16, VK_BUFFER_USAGE_UNIFORM_TEXEL_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT );
    
    const uint32_t strides[ 3 ] = { 4, 8, 16 };
    
    for ( int i = 0; i < 3; ++i ) {
        r.structured[ i ] = gpu_create_buffer( gpu, 1024 * strides[ i ], VK_BUFFER_USAGE_STORAGE_BUFFER_BIT );
    }
    
    r.output = gpu_create_buffer( gpu, 2048 * 4, VK_BUFFER_USAGE_STORAGE_TEXEL_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT );
    r.output_view = gpu_create_buffer_view( gpu, r.output, VK_FORMAT_R32_SFLOAT, VK_WHOLE_SIZE );

    for ( const FormatInfo& f : k_formats ) {
        VkFormatProperties fp;
        vkGetPhysicalDeviceFormatProperties( gpu.physical, f.format, &fp );

        if ( fp.bufferFeatures & VK_FORMAT_FEATURE_UNIFORM_TEXEL_BUFFER_BIT ) {
            r.typed_views[ f.format ] = gpu_create_buffer_view( gpu, r.input, f.format, 1024 * f.size );
        }

        r.texture_features[ f.format ] = fp.optimalTilingFeatures;

        if ( fp.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT ) {
            r.textures[ f.format ] = gpu_create_image( gpu, f.format, 32, 32, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT );
        }
    }

    r.cb_aligned    = gpu_create_buffer( gpu, 256, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT );
    r.cb_unaligned  = gpu_create_buffer( gpu, 256, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT );
    r.cb_with_array = gpu_create_buffer( gpu, sizeof( LoadConstantsWithArray ), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT );

    // Samplers
    VkSamplerCreateInfo si{ VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
    si.addressModeU = si.addressModeV = si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    si.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    si.magFilter = si.minFilter = VK_FILTER_NEAREST;
    VK_CHECK( vkCreateSampler( gpu.device, &si, nullptr, &r.sampler_nearest ) );
    
    si.magFilter = si.minFilter = VK_FILTER_LINEAR;
    VK_CHECK( vkCreateSampler( gpu.device, &si, nullptr, &r.sampler_bilinear ) );

    // Initialize contents on the GPU: zeros everywhere, constants via vkCmdUpdateBuffer.
    VkCommandBuffer cb = gpu_allocated_and_begin_cb( gpu );
    vkCmdFillBuffer( cb, r.input.buffer, 0, VK_WHOLE_SIZE, 0 );
    
    for ( auto& s : r.structured ) {
        vkCmdFillBuffer( cb, s.buffer, 0, VK_WHOLE_SIZE, 0 );
    }

    vkCmdFillBuffer( cb, r.output.buffer, 0, VK_WHOLE_SIZE, 0 );

    LoadConstants lc{ 0, 0xffffffffu, 0, 0 };
    vkCmdUpdateBuffer( cb, r.cb_aligned.buffer, 0, sizeof( lc ), &lc );
    lc.readStartAddress = 4;                                   // unaligned
    vkCmdUpdateBuffer( cb, r.cb_unaligned.buffer, 0, sizeof( lc ), &lc );

    static LoadConstantsWithArray lca;
    memset( &lca, 0, sizeof( lca ) );
    lca.writeIndex = 0xffffffffu;
    vkCmdUpdateBuffer( cb, r.cb_with_array.buffer, 0, sizeof( lca ), &lca );

    std::vector<VkImageMemoryBarrier> to_transfer, to_read;
    for ( auto& [ format, img ] : r.textures ) {
        VkImageMemoryBarrier b{ VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
        b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.image = img.image;
        b.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
        b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        b.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        b.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        to_transfer.push_back( b );

        b.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        b.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        b.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        b.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        to_read.push_back( b );
    }
    if ( !to_transfer.empty() ) {
        vkCmdPipelineBarrier( cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, ( uint32_t )to_transfer.size(), to_transfer.data() );
    }

    const VkClearColorValue zero{};
    const VkImageSubresourceRange range{ VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
    for ( auto& [format, img] : r.textures ) {
        vkCmdClearColorImage( cb, img.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &zero, 1, &range );
    }

    VkMemoryBarrier mb{ VK_STRUCTURE_TYPE_MEMORY_BARRIER };
    mb.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    mb.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_UNIFORM_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
    vkCmdPipelineBarrier( cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &mb, 0, nullptr,
                          ( uint32_t )to_read.size(), to_read.empty() ? nullptr : to_read.data() );

    gpu_end_cb_submit_and_wait( gpu, cb, VK_NULL_HANDLE );
    vkFreeCommandBuffers( gpu.device, gpu.command_pool, 1, &cb );

    // Descriptor set layouts: 0 = cbuffer, 1 = source (varies), 2 = RWBuffer<float> output, 3 = sampler.
    for ( int k = 0; k < ( int )SourceKind::Count; ++k ) {
        const SourceKind kind = ( SourceKind )k;
        std::vector<VkDescriptorSetLayoutBinding> bindings;
        bindings.push_back( { 0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr } );
        if ( kind != SourceKind::None ) {
            bindings.push_back( { 1, source_descriptor_type( kind ), 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr } );
        }
        bindings.push_back( { 2, VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr } );
        bindings.push_back( { 3, VK_DESCRIPTOR_TYPE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr } );

        VkDescriptorSetLayoutCreateInfo lci{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
        lci.bindingCount = ( uint32_t )bindings.size();
        lci.pBindings = bindings.data();
        VK_CHECK( vkCreateDescriptorSetLayout( gpu.device, &lci, nullptr, &r.set_layouts[ k ] ) );

        VkPipelineLayoutCreateInfo pci{ VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
        pci.setLayoutCount = 1;
        pci.pSetLayouts = &r.set_layouts[ k ];
        VK_CHECK( vkCreatePipelineLayout( gpu.device, &pci, nullptr, &r.pipeline_layouts[ k ] ) );
    }
}


//////////////////////////////////////////////////////////////////////////
// Per-test preparation
//////////////////////////////////////////////////////////////////////////

struct PreparedTest {
    const TestCase*     test = nullptr;
    VkPipeline          pipeline = VK_NULL_HANDLE;
    VkPipelineLayout    layout = VK_NULL_HANDLE;
    VkDescriptorSet     set = VK_NULL_HANDLE;
    std::string         skip_reason;
};

static VkPipeline get_pipeline( Gpu& gpu, Resources& r, const std::string& shader_dir, const TestCase& t, std::string& error ) {
    const std::string key = t.shader + "#" + std::to_string( ( int )t.kind );
    auto it = r.pipelines.find( key );
    if ( it != r.pipelines.end() ) {
        if ( !it->second ) {
            error = r.pipeline_errors[ key ];
        }
        return it->second;
    }

    VkPipeline pipeline = gpu_create_compute_pipeline( gpu, r.pipeline_layouts[ ( int )t.kind ], shader_dir + "/" + t.shader + ".spv", error );
    r.pipelines[ key ] = pipeline;
    r.pipeline_errors[ key ] = error;
    
    return pipeline;
}

static std::string check_requirements( const Gpu& gpu, const Resources& r, const TestCase& t ) {
    if ( ( t.requirements & Req_BigUniformBuffer ) && gpu.props.limits.maxUniformBufferRange < sizeof( LoadConstantsWithArray ) ) {
        return "maxUniformBufferRange < 16400";
    }

    if ( t.kind == SourceKind::TypedBuffer && !r.typed_views.count( t.format ) ) {
        return "format not supported for uniform texel buffers";
    }

    if ( t.kind == SourceKind::SampledImage ) {
        if ( !r.textures.count( t.format ) ) {
            return "format not supported for sampled images";
        }

        if ( ( t.requirements & Req_LinearFilter ) && !( r.texture_features.at( t.format ) & VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT ) ) {
            return "linear filtering not supported for this format";
        }
    }
    return {};
}

static void prepare_tests( Gpu& gpu, Resources& r, const std::string& shader_dir, std::vector<PreparedTest>& prepared ) {
    // Descriptor pool sized for the worst case.
    const uint32_t n = ( uint32_t )prepared.size();
    VkDescriptorPoolSize sizes[] = {
        { VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, n },
        { VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER, n },
        { VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, n },
        { VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, n },
        { VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER, n },
        { VK_DESCRIPTOR_TYPE_SAMPLER, n },
    };

    VkDescriptorPoolCreateInfo dpci{ VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
    dpci.maxSets = n;
    dpci.poolSizeCount = ( uint32_t )( sizeof( sizes ) / sizeof( sizes[ 0 ] ) );
    dpci.pPoolSizes = sizes;
    VK_CHECK( vkCreateDescriptorPool( gpu.device, &dpci, nullptr, &r.descriptor_pool ) );

    for ( PreparedTest& p : prepared ) {
        const TestCase& t = *p.test;
        p.skip_reason = check_requirements( gpu, r, t );
        if ( !p.skip_reason.empty() ) {
            continue;
        }

        p.pipeline = get_pipeline( gpu, r, shader_dir, t, p.skip_reason );
        if ( !p.pipeline ) {
            if ( p.skip_reason.empty() ) {
                p.skip_reason = "pipeline unavailable";
            }
            continue;
        }
        p.layout = r.pipeline_layouts[ ( int )t.kind ];

        VkDescriptorSetAllocateInfo ai{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
        ai.descriptorPool = r.descriptor_pool;
        ai.descriptorSetCount = 1;
        ai.pSetLayouts = &r.set_layouts[ ( int )t.kind ];
        VK_CHECK( vkAllocateDescriptorSets( gpu.device, &ai, &p.set ) );

        const GpuBuffer* cb = nullptr;
        VkDeviceSize cb_range = 0;
        switch ( t.cb ) {
            case CbKind::Aligned:
            {
                cb = &r.cb_aligned;
                cb_range = sizeof( LoadConstants );
                break;
            }
            case CbKind::Unaligned:
            {
                cb = &r.cb_unaligned;
                cb_range = sizeof( LoadConstants );
                break;
            }
            case CbKind::WithArray:
            {
                cb = &r.cb_with_array;
                cb_range = sizeof( LoadConstantsWithArray );
                break;
            }
        }

        VkDescriptorBufferInfo cb_info{ cb->buffer, 0, cb_range };
        VkDescriptorBufferInfo storage_info{};
        VkDescriptorImageInfo image_info{ VK_NULL_HANDLE, VK_NULL_HANDLE, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
        VkDescriptorImageInfo sampler_info{ t.bilinear ? r.sampler_bilinear : r.sampler_nearest, VK_NULL_HANDLE, VK_IMAGE_LAYOUT_UNDEFINED };
        VkBufferView source_view = VK_NULL_HANDLE;

        std::vector<VkWriteDescriptorSet> writes;

        auto write = [ & ]( uint32_t binding, VkDescriptorType type, uint32_t count ) -> VkWriteDescriptorSet& {
            VkWriteDescriptorSet w{ VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
            w.dstSet = p.set;
            w.dstBinding = binding;
            w.descriptorCount = count;
            w.descriptorType = type;
            writes.push_back( w );

            return writes.back();
        };

        write( 0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1 ).pBufferInfo = &cb_info;

        switch ( t.kind ) {
            case SourceKind::TypedBuffer:
                source_view = r.typed_views.at( t.format );
                write( 1, VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER, 1 ).pTexelBufferView = &source_view;
                break;
            case SourceKind::StorageBuffer: {
                const GpuBuffer& b = t.structured_stride == 4 ? r.structured[ 0 ] : t.structured_stride == 8 ? r.structured[ 1 ] : t.structured_stride == 16 ? r.structured[ 2 ] : r.input;
                storage_info = { b.buffer, 0, VK_WHOLE_SIZE };
                write( 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1 ).pBufferInfo = &storage_info;
                break;
            }
            case SourceKind::SampledImage:
                image_info.imageView = r.textures.at( t.format ).view;
                write( 1, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1 ).pImageInfo = &image_info;
                break;
            default:
                break;
        }

        write( 2, VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER, 1 ).pTexelBufferView = &r.output_view;
        write( 3, VK_DESCRIPTOR_TYPE_SAMPLER, 1 ).pImageInfo = &sampler_info;
        
        vkUpdateDescriptorSets( gpu.device, ( uint32_t )writes.size(), writes.data(), 0, nullptr );
    }
}

//////////////////////////////////////////////////////////////////////////
// Suite interface
//////////////////////////////////////////////////////////////////////////

struct OriginalSuite {
    Resources                 res;
    std::vector<TestCase>     tests;
    std::vector<PreparedTest> prepared;
    uint32_t                  groups_x = 0, groups_y = 0;
};

std::vector<std::string> original_test_names() {
    std::vector<std::string> names;
    for ( const TestCase& t : build_test_list() ) names.push_back( t.name );
    return names;
}

OriginalSuite* original_create( Gpu& gpu, const AppOptions& opt, const std::string& shader_dir ) {
    OriginalSuite* s = new OriginalSuite;
    s->groups_x = opt.groups_x;
    s->groups_y = opt.groups_y;

    for ( const TestCase& t : build_test_list() ) {
        if ( opt.filter.empty() || t.name.find( opt.filter ) != std::string::npos ) {
            s->tests.push_back( t );
        }
    }

    create_resources( gpu, s->res );

    s->prepared.resize( s->tests.size() );

    for ( size_t i = 0; i < s->tests.size(); ++i ) {
        s->prepared[ i ].test = &s->tests[ i ];
    }

    if ( !s->prepared.empty() ) {
        prepare_tests( gpu, s->res, shader_dir, s->prepared );
    }

    return s;
}

void original_append_tests( OriginalSuite* s, std::vector<AppRunnableTest>& out ) {
    for ( PreparedTest& p : s->prepared ) {

        AppRunnableTest t;
        t.suite = "original";
        t.name = p.test->name;
        t.skip_reason = p.pipeline ? std::string() : ( p.skip_reason.empty() ? std::string( "pipeline unavailable" ) : p.skip_reason );
        
        if ( p.pipeline ) {
            const VkPipeline pipeline = p.pipeline;
            const VkPipelineLayout layout = p.layout;
            const VkDescriptorSet set = p.set;
            const uint32_t gx = s->groups_x, gy = s->groups_y;

            t.record = [ = ]( VkCommandBuffer cmd ) {
                vkCmdBindPipeline( cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline );
                vkCmdBindDescriptorSets( cmd, VK_PIPELINE_BIND_POINT_COMPUTE, layout, 0, 1, &set, 0, nullptr );
                vkCmdDispatch( cmd, gx, gy, 1 );
            };
        }
        out.push_back( std::move( t ) );
    }
}

void original_destroy( Gpu& gpu, OriginalSuite* s ) {
    Resources& res = s->res;

    for ( auto& [key, pipeline] : res.pipelines ) {
        if ( pipeline ) vkDestroyPipeline( gpu.device, pipeline, nullptr );
    }

    if ( res.descriptor_pool ) {
        vkDestroyDescriptorPool( gpu.device, res.descriptor_pool, nullptr );
    }

    for ( int k = 0; k < ( int )SourceKind::Count; ++k ) {
        vkDestroyPipelineLayout( gpu.device, res.pipeline_layouts[ k ], nullptr );
        vkDestroyDescriptorSetLayout( gpu.device, res.set_layouts[ k ], nullptr );
    }

    vkDestroySampler( gpu.device, res.sampler_nearest, nullptr );
    vkDestroySampler( gpu.device, res.sampler_bilinear, nullptr );

    for ( auto& [format, view] : res.typed_views ) {
        vkDestroyBufferView( gpu.device, view, nullptr );
    }

    vkDestroyBufferView( gpu.device, res.output_view, nullptr );
    for ( auto& [format, img] : res.textures ) {
        gpu_destroy_image( gpu, img );
    }

    for ( GpuBuffer* b : { &res.input, &res.structured[ 0 ], &res.structured[ 1 ], &res.structured[ 2 ], &res.output,
                        &res.cb_aligned, &res.cb_unaligned, &res.cb_with_array } ) {
        gpu_destroy_buffer( gpu, *b );
    }
        
    delete s;
}
