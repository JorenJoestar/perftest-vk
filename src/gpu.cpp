// context.cpp - options, Vulkan instance/device, resource helpers.
#include "gpu.hpp"
#include "bindless_layout.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <numeric>
#include <random>

//////////////////////////////////////////////////////////////////////////
// Instance and device
//////////////////////////////////////////////////////////////////////////

uint32_t g_validation_messages = 0;

static VKAPI_ATTR VkBool32 VKAPI_CALL debug_callback( VkDebugUtilsMessageSeverityFlagBitsEXT severity, VkDebugUtilsMessageTypeFlagsEXT,
                                                      const VkDebugUtilsMessengerCallbackDataEXT* data, void* ) {
    if ( severity >= VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT ) {
        fprintf( stderr, "\n[validation] %s\n", data->pMessage );
        ++g_validation_messages;
    }
    return VK_FALSE;
}

const char* gpu_vendor_name( uint32_t id ) {
    switch ( id ) {
        case 0x1002: return "AMD";
        case 0x10DE: return "NVIDIA";
        case 0x8086: return "Intel";
        case 0x13B5: return "ARM";
        case 0x5143: return "Qualcomm";
        case 0x106B: return "Apple";
        case 0x10005: return "Mesa";
        default: return "unknown";
    }
}

std::string gpu_driver_version_string( const VkPhysicalDeviceProperties& p ) {
    char buf[ 64 ];
    const uint32_t v = p.driverVersion;
    if ( p.vendorID == 0x10DE ) {
        snprintf( buf, sizeof( buf ), "%u.%u.%u.%u", ( v >> 22 ) & 0x3ff, ( v >> 14 ) & 0xff, ( v >> 6 ) & 0xff, v & 0x3f );
    }        
    else if ( p.vendorID == 0x8086 ) {
        snprintf( buf, sizeof( buf ), "%u.%u", v >> 14, v & 0x3fff );
    }
    else {
        snprintf( buf, sizeof( buf ), "%u.%u.%u", VK_API_VERSION_MAJOR( v ), VK_API_VERSION_MINOR( v ), VK_API_VERSION_PATCH( v ) );
    }
    return buf;
}

std::string gpu_config_string( const Gpu& gpu, const AppOptions& opt ) {
    std::string s;
    
    if ( opt.robustness ) {
        s += gpu.features.robustness2 ? "robustness2" : "robustness";
    }

    if ( gpu.required_subgroup_size ) {
        s += ( s.empty() ? "" : "+" ) + std::string( "subgroup" ) + std::to_string( gpu.required_subgroup_size );
    }

    return s.empty() ? "default" : s;
}

void gpu_create_instance( Gpu& gpu, const AppOptions& opt ) {
    uint32_t loader_version = VK_API_VERSION_1_0;
    auto enumerate_version = ( PFN_vkEnumerateInstanceVersion )vkGetInstanceProcAddr( nullptr, "vkEnumerateInstanceVersion" );
    if ( enumerate_version ) {
        enumerate_version( &loader_version );
    }

    VkApplicationInfo app{ VK_STRUCTURE_TYPE_APPLICATION_INFO };
    app.pApplicationName = "perftest-vk";
    app.pEngineName = "perftest-vk";
    app.apiVersion = std::min<uint32_t>( loader_version, VK_API_VERSION_1_3 );
    if ( app.apiVersion < VK_API_VERSION_1_1 ) {
        app_fatal( "Vulkan 1.1 loader required" );
    }

    std::vector<const char*> layers, extensions;
    if ( opt.validation ) {
        layers.push_back( "VK_LAYER_KHRONOS_validation" );
        extensions.push_back( VK_EXT_DEBUG_UTILS_EXTENSION_NAME );
    }

    VkInstanceCreateInfo ci{ VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO };
    ci.pApplicationInfo = &app;
    ci.enabledLayerCount = ( uint32_t )layers.size();
    ci.ppEnabledLayerNames = layers.data();
    ci.enabledExtensionCount = ( uint32_t )extensions.size();
    ci.ppEnabledExtensionNames = extensions.data();
    VK_CHECK( vkCreateInstance( &ci, nullptr, &gpu.instance ) );

    if ( opt.validation ) {
        auto create_messenger = ( PFN_vkCreateDebugUtilsMessengerEXT )vkGetInstanceProcAddr( gpu.instance, "vkCreateDebugUtilsMessengerEXT" );
        VkDebugUtilsMessengerCreateInfoEXT mi{ VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT };
        mi.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
        mi.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT;
        mi.pfnUserCallback = debug_callback;
        if ( create_messenger ) {
            VK_CHECK( create_messenger( gpu.instance, &mi, nullptr, &gpu.messenger ) );
        }
    }
}

static bool has_extension( VkPhysicalDevice physical, const char* name ) {

    uint32_t count = 0;
    vkEnumerateDeviceExtensionProperties( physical, nullptr, &count, nullptr );

    std::vector<VkExtensionProperties> props( count );
    vkEnumerateDeviceExtensionProperties( physical, nullptr, &count, props.data() );

    for ( const VkExtensionProperties& p : props ) {
        if ( strcmp( p.extensionName, name ) == 0 ) {
            return true;
        }
    }

    return false;
}

void gpu_create_device( Gpu& gpu, const AppOptions& opt ) {

    uint32_t count = 0;
    vkEnumeratePhysicalDevices( gpu.instance, &count, nullptr );

    std::vector<VkPhysicalDevice> devices( count );
    vkEnumeratePhysicalDevices( gpu.instance, &count, devices.data() );

    if ( devices.empty() ) {
        app_fatal( "no Vulkan devices" );
    }

    printf( "Devices found:\n" );

    int selected = opt.device_index;

    for ( size_t i = 0; i < devices.size(); ++i ) {
        VkPhysicalDeviceProperties p;
        vkGetPhysicalDeviceProperties( devices[ i ], &p );
        printf( "%zu: %s\n", i, p.deviceName );
        if ( selected < 0 && p.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU ) {
            selected = ( int )i;
        }
    }

    if ( selected < 0 ) {
        selected = 0;
    }

    selected = std::min( std::max( selected, 0 ), ( int )devices.size() - 1 );

    gpu.physical = devices[ selected ];
    
    vkGetPhysicalDeviceProperties( gpu.physical, &gpu.props );
    vkGetPhysicalDeviceMemoryProperties( gpu.physical, &gpu.memory_props );
    printf( "Using device %d\n\n", selected );

    // Compute queue with timestamps
    uint32_t qcount = 0;
    vkGetPhysicalDeviceQueueFamilyProperties( gpu.physical, &qcount, nullptr );

    std::vector<VkQueueFamilyProperties> families( qcount );
    vkGetPhysicalDeviceQueueFamilyProperties( gpu.physical, &qcount, families.data() );

    gpu.queue_family = UINT32_MAX;
    // prefer the graphics+compute family, like the D3D11 original
    for ( uint32_t i = 0; i < qcount; ++i ) {
        if ( ( families[ i ].queueFlags & VK_QUEUE_GRAPHICS_BIT ) &&
             ( families[ i ].queueFlags & VK_QUEUE_COMPUTE_BIT ) &&
             families[ i ].timestampValidBits ) { 
            gpu.queue_family = i; break;
        }
    }
    if ( gpu.queue_family == UINT32_MAX ) {
        for ( uint32_t i = 0; i < qcount; ++i ) {
            if ( ( families[ i ].queueFlags & VK_QUEUE_COMPUTE_BIT ) && families[ i ].timestampValidBits ) {
                gpu.queue_family = i; break;
            }
        }
    }

    if ( gpu.queue_family == UINT32_MAX ) {
        app_fatal( "no compute queue with timestamp support" );
    }

    gpu.timestamp_valid_bits = families[ gpu.queue_family ].timestampValidBits;

    // Subgroup properties (Vulkan 1.1) and size control (1.3)
    const bool api12 = gpu.props.apiVersion >= VK_API_VERSION_1_2;
    const bool api13 = gpu.props.apiVersion >= VK_API_VERSION_1_3;

    VkPhysicalDeviceVulkan13Properties p13{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_PROPERTIES };
    VkPhysicalDeviceSubgroupProperties subgroup{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SUBGROUP_PROPERTIES };
    subgroup.pNext = api13 ? &p13 : nullptr;
    
    VkPhysicalDeviceProperties2 props2{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2 };
    props2.pNext = &subgroup;
    
    vkGetPhysicalDeviceProperties2( gpu.physical, &props2 );
    gpu.subgroup_size = subgroup.subgroupSize;
    gpu.features.subgroup_ballot = ( subgroup.supportedStages & VK_SHADER_STAGE_COMPUTE_BIT ) &&
                                   ( subgroup.supportedOperations & VK_SUBGROUP_FEATURE_BALLOT_BIT );

    // GpuFeatures: query what the device has, enable only what the tests can use.
    VkPhysicalDeviceVulkan13Features f13{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES };
    VkPhysicalDeviceVulkan12Features f12{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES };
    VkPhysicalDeviceFeatures2 f2{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2 };
    
    if ( api12 ) { 
        f2.pNext = &f12;
        f12.pNext = api13 ? &f13 : nullptr; 
    }
    vkGetPhysicalDeviceFeatures2( gpu.physical, &f2 );

    gpu.features.storage_write_without_format = f2.features.shaderStorageImageWriteWithoutFormat;
    gpu.features.storage_read_without_format  = f2.features.shaderStorageImageReadWithoutFormat;
    gpu.features.shader_int64                 = f2.features.shaderInt64;
    gpu.features.ssbo_dynamic_indexing        = f2.features.shaderStorageBufferArrayDynamicIndexing;
    gpu.features.ssbo_nonuniform_indexing     = api12 && f12.shaderStorageBufferArrayNonUniformIndexing;
    gpu.features.buffer_device_address        = api12 && f12.bufferDeviceAddress && f2.features.shaderInt64;
    gpu.features.buffer_int64_atomics         = api12 && f12.shaderBufferInt64Atomics && f2.features.shaderInt64;

    VkPhysicalDeviceVulkan13Features enable13{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES };
    VkPhysicalDeviceVulkan12Features enable12{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES };
    VkPhysicalDeviceRobustness2FeaturesEXT robust2{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ROBUSTNESS_2_FEATURES_EXT };
    
    VkPhysicalDeviceFeatures2 enable{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2 };
    enable.features.shaderStorageImageWriteWithoutFormat = gpu.features.storage_write_without_format;
    enable.features.shaderStorageImageReadWithoutFormat  = gpu.features.storage_read_without_format;
    enable.features.shaderInt64                          = gpu.features.shader_int64;
    enable.features.shaderStorageBufferArrayDynamicIndexing = gpu.features.ssbo_dynamic_indexing;
    
    std::vector<const char*> extensions;
    void** chain_tail = &enable.pNext;
    auto chain = [ & ]( void* s, void** next ) { *chain_tail = s; chain_tail = next; };

    if ( api12 ) {
        enable12.shaderStorageBufferArrayNonUniformIndexing = gpu.features.ssbo_nonuniform_indexing;
        enable12.bufferDeviceAddress = gpu.features.buffer_device_address;
        enable12.shaderBufferInt64Atomics = gpu.features.buffer_int64_atomics;
        // Bindless suite: everything the bindless layout needs (Vulkan 1.2 descriptor indexing).
        const char* missing = nullptr;
        if ( bindless_layout_check_device( gpu.physical, &missing ) ) {
            bindless_layout_enable_features( enable.features, enable12 );
            gpu.features.bindless = true;
        } else {
            gpu.features.bindless_missing = std::string( missing ) + " not supported";
        }
        chain( &enable12, &enable12.pNext );
    } else {
        gpu.features.bindless_missing = "Vulkan 1.2 required";
    }

    if ( opt.subgroup_size ) {

        if ( !api13 || !f13.subgroupSizeControl || !f13.computeFullSubgroups ) {
            app_fatal( "--subgroup-size needs Vulkan 1.3 subgroupSizeControl + computeFullSubgroups" );
        }

        if ( !( p13.requiredSubgroupSizeStages & VK_SHADER_STAGE_COMPUTE_BIT ) || opt.subgroup_size < p13.minSubgroupSize || opt.subgroup_size > p13.maxSubgroupSize ) {
            app_fatal( "--subgroup-size %u not supported for compute (range %u-%u)", opt.subgroup_size, p13.minSubgroupSize, p13.maxSubgroupSize );
        }

        enable13.subgroupSizeControl = VK_TRUE;
        enable13.computeFullSubgroups = VK_TRUE;
        chain( &enable13, &enable13.pNext );
        gpu.required_subgroup_size = opt.subgroup_size;
    }

    if ( opt.robustness ) {
        enable.features.robustBufferAccess = VK_TRUE;
        const char* ext = has_extension( gpu.physical, "VK_KHR_robustness2" ) ? "VK_KHR_robustness2"
                        : has_extension( gpu.physical, "VK_EXT_robustness2" ) ? "VK_EXT_robustness2" : nullptr;
        if ( ext ) {
            VkPhysicalDeviceRobustness2FeaturesEXT query{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ROBUSTNESS_2_FEATURES_EXT };
            VkPhysicalDeviceFeatures2 q2{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2 };
            q2.pNext = &query;
            
            vkGetPhysicalDeviceFeatures2( gpu.physical, &q2 );
            
            robust2.robustBufferAccess2 = query.robustBufferAccess2;
            robust2.robustImageAccess2 = query.robustImageAccess2;

            if ( robust2.robustBufferAccess2 ) {
                extensions.push_back( ext );
                chain( &robust2, &robust2.pNext );
                gpu.features.robustness2 = true;
            }
        }
    }

    const float priority = 1.0f;
    VkDeviceQueueCreateInfo qci{ VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO };
    qci.queueFamilyIndex = gpu.queue_family;
    qci.queueCount = 1;
    qci.pQueuePriorities = &priority;

    VkDeviceCreateInfo dci{ VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO };
    dci.pNext = &enable;
    dci.queueCreateInfoCount = 1;
    dci.pQueueCreateInfos = &qci;
    dci.enabledExtensionCount = ( uint32_t )extensions.size();
    dci.ppEnabledExtensionNames = extensions.data();
    VK_CHECK( vkCreateDevice( gpu.physical, &dci, nullptr, &gpu.device ) );
    vkGetDeviceQueue( gpu.device, gpu.queue_family, 0, &gpu.queue );

    if ( gpu.features.buffer_device_address ) {
        gpu.get_buffer_address = ( PFN_vkGetBufferDeviceAddress )vkGetDeviceProcAddr( gpu.device, "vkGetBufferDeviceAddress" );
    }
    if ( !gpu.get_buffer_address ) {
        gpu.features.buffer_device_address = false;
    }

    VkCommandPoolCreateInfo pci{ VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO };
    pci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    pci.queueFamilyIndex = gpu.queue_family;
    VK_CHECK( vkCreateCommandPool( gpu.device, &pci, nullptr, &gpu.command_pool ) );
}

void gpu_create( Gpu& gpu, const AppOptions& opt ) {
    gpu_create_instance( gpu, opt );
    gpu_create_device( gpu, opt );
}

void gpu_destroy( Gpu& gpu ) {
    if ( gpu.command_pool ) {
        vkDestroyCommandPool( gpu.device, gpu.command_pool, nullptr );
    }

    if ( gpu.device ) {
        vkDestroyDevice( gpu.device, nullptr );
    }

    if ( gpu.messenger ) {
        auto destroy_messenger = ( PFN_vkDestroyDebugUtilsMessengerEXT )vkGetInstanceProcAddr( gpu.instance, "vkDestroyDebugUtilsMessengerEXT" );
        if ( destroy_messenger ) {
            destroy_messenger( gpu.instance, gpu.messenger, nullptr );
        }
    }
    if ( gpu.instance ) {
        vkDestroyInstance( gpu.instance, nullptr );
    }

    gpu = Gpu{};
}

//////////////////////////////////////////////////////////////////////////
// Resources
//////////////////////////////////////////////////////////////////////////

uint32_t gpu_find_memory_type( const Gpu& gpu, uint32_t type_bits, VkMemoryPropertyFlags wanted ) {
    for ( uint32_t i = 0; i < gpu.memory_props.memoryTypeCount; ++i ) {
        if ( ( type_bits & ( 1u << i ) ) && ( gpu.memory_props.memoryTypes[ i ].propertyFlags & wanted ) == wanted ) {
            return i;
        }
    }

    // fallback: any compatible type
    for ( uint32_t i = 0; i < gpu.memory_props.memoryTypeCount; ++i ) {
        if ( type_bits & ( 1u << i ) ) {
            return i;
        }
    }

    app_fatal( "no compatible memory type" );
}

static GpuBuffer allocate_buffer( Gpu& gpu, VkDeviceSize size, VkBufferUsageFlags usage, VkMemoryPropertyFlags memory ) {
    GpuBuffer b;
    b.size = size;
    
    if ( !gpu.features.buffer_device_address ) {
        usage &= ~VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
    }

    VkBufferCreateInfo ci{ VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
    ci.size = size;
    ci.usage = usage | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    ci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    VK_CHECK( vkCreateBuffer( gpu.device, &ci, nullptr, &b.buffer ) );

    VkMemoryRequirements req;
    vkGetBufferMemoryRequirements( gpu.device, b.buffer, &req );
    
    VkMemoryAllocateFlagsInfo flags{ VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO };
    flags.flags = VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT;
    
    VkMemoryAllocateInfo ai{ VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
    ai.pNext = ( usage & VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT ) ? &flags : nullptr;
    ai.allocationSize = req.size;
    ai.memoryTypeIndex = gpu_find_memory_type( gpu, req.memoryTypeBits, memory );
   
    if ( ( gpu.memory_props.memoryTypes[ ai.memoryTypeIndex ].propertyFlags & memory ) != memory &&
         ( memory & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT ) ) {
        app_fatal( "no host-visible memory type" );
    }

    VK_CHECK( vkAllocateMemory( gpu.device, &ai, nullptr, &b.memory ) );
    VK_CHECK( vkBindBufferMemory( gpu.device, b.buffer, b.memory, 0 ) );

    if ( memory & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT ) {
        VK_CHECK( vkMapMemory( gpu.device, b.memory, 0, VK_WHOLE_SIZE, 0, &b.mapped ) );
    }

    return b;
}

GpuBuffer gpu_create_buffer( Gpu& gpu, VkDeviceSize size, VkBufferUsageFlags usage ) {
    return allocate_buffer( gpu, size, usage, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT );
}

GpuBuffer gpu_create_host_buffer( Gpu& gpu, VkDeviceSize size, VkBufferUsageFlags usage ) {
    return allocate_buffer( gpu, size, usage, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT );
}

void gpu_destroy_buffer( Gpu& gpu, GpuBuffer& b ) {
    if ( b.buffer ) {
        vkDestroyBuffer( gpu.device, b.buffer, nullptr );
    }

    if ( b.memory ) {
        vkFreeMemory( gpu.device, b.memory, nullptr );
    }

    b = GpuBuffer{};
}

GpuImage gpu_create_image( Gpu& gpu, VkFormat format, uint32_t width, uint32_t height, VkImageUsageFlags usage ) {
    GpuImage img;

    VkImageCreateInfo ci{ VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
    ci.imageType = VK_IMAGE_TYPE_2D;
    ci.format = format;
    ci.extent = { width, height, 1 };
    ci.mipLevels = 1;
    ci.arrayLayers = 1;
    ci.samples = VK_SAMPLE_COUNT_1_BIT;
    ci.tiling = VK_IMAGE_TILING_OPTIMAL;
    ci.usage = usage;
    ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    VK_CHECK( vkCreateImage( gpu.device, &ci, nullptr, &img.image ) );

    VkMemoryRequirements req;
    vkGetImageMemoryRequirements( gpu.device, img.image, &req );
    
    VkMemoryAllocateInfo ai{ VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
    ai.allocationSize = req.size;
    ai.memoryTypeIndex = gpu_find_memory_type( gpu, req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT );
    
    VK_CHECK( vkAllocateMemory( gpu.device, &ai, nullptr, &img.memory ) );
    VK_CHECK( vkBindImageMemory( gpu.device, img.image, img.memory, 0 ) );

    VkImageViewCreateInfo vi{ VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
    vi.image = img.image;
    vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vi.format = format;
    vi.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
    
    VK_CHECK( vkCreateImageView( gpu.device, &vi, nullptr, &img.view ) );
    return img;
}

void gpu_destroy_image( Gpu& gpu, GpuImage& img ) {
    if ( img.view ) {
        vkDestroyImageView( gpu.device, img.view, nullptr );
    }

    if ( img.image ) {
        vkDestroyImage( gpu.device, img.image, nullptr );
    }

    if ( img.memory ) {
        vkFreeMemory( gpu.device, img.memory, nullptr );
    }

    img = GpuImage{};
}

VkBufferView gpu_create_buffer_view( Gpu& gpu, const GpuBuffer& b, VkFormat format, VkDeviceSize range ) {
    VkBufferViewCreateInfo ci{ VK_STRUCTURE_TYPE_BUFFER_VIEW_CREATE_INFO };
    ci.buffer = b.buffer;
    ci.format = format;
    ci.offset = 0;
    ci.range = range;
    VkBufferView view;
    
    VK_CHECK( vkCreateBufferView( gpu.device, &ci, nullptr, &view ) );
    
    return view;
}

VkDeviceAddress gpu_buffer_address( Gpu& gpu, const GpuBuffer& b ) {
    if ( !gpu.features.buffer_device_address ) {
        return 0;
    }

    VkBufferDeviceAddressInfo ai{ VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO };
    ai.buffer = b.buffer;
    
    return gpu.get_buffer_address( gpu.device, &ai );
}

VkCommandBuffer gpu_allocated_and_begin_cb( Gpu& gpu ) {
    VkCommandBufferAllocateInfo ai{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
    ai.commandPool = gpu.command_pool;
    ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    ai.commandBufferCount = 1;

    VkCommandBuffer cb;
    VK_CHECK( vkAllocateCommandBuffers( gpu.device, &ai, &cb ) );
    
    VkCommandBufferBeginInfo bi{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    
    VK_CHECK( vkBeginCommandBuffer( cb, &bi ) );
    
    return cb;
}

void gpu_end_cb_submit_and_wait( Gpu& gpu, VkCommandBuffer cb, VkFence fence ) {
    VK_CHECK( vkEndCommandBuffer( cb ) );
    
    VkSubmitInfo si{ VK_STRUCTURE_TYPE_SUBMIT_INFO };
    si.commandBufferCount = 1;
    si.pCommandBuffers = &cb;
    
    VK_CHECK( vkQueueSubmit( gpu.queue, 1, &si, fence ) );
    if ( fence ) {
        VK_CHECK( vkWaitForFences( gpu.device, 1, &fence, VK_TRUE, UINT64_MAX ) );
        VK_CHECK( vkResetFences( gpu.device, 1, &fence ) );
    } else {
        VK_CHECK( vkQueueWaitIdle( gpu.queue ) );
    }
}

void gpu_end_cb_submit_wait_free_cb( Gpu& gpu, VkCommandBuffer cb ) {

    gpu_end_cb_submit_and_wait( gpu, cb, VK_NULL_HANDLE );

    vkFreeCommandBuffers( gpu.device, gpu.command_pool, 1, &cb );
}

VkPipeline gpu_create_compute_pipeline( Gpu& gpu, VkPipelineLayout layout, const std::string& spv_path, std::string& error ) {
    std::vector<uint32_t> code = app_read_spirv( spv_path );
    if ( code.empty() ) {
        error = "shader not compiled (" + spv_path.substr( spv_path.find_last_of( "/\\" ) + 1 ) + ")";
        return VK_NULL_HANDLE;
    }

    VkShaderModuleCreateInfo mci{ VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO };
    mci.codeSize = code.size() * 4;
    mci.pCode = code.data();
    
    VkShaderModule module;
    VK_CHECK( vkCreateShaderModule( gpu.device, &mci, nullptr, &module ) );

    VkPipelineShaderStageRequiredSubgroupSizeCreateInfo required{ VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_REQUIRED_SUBGROUP_SIZE_CREATE_INFO };
    required.requiredSubgroupSize = gpu.required_subgroup_size;
    
    VkComputePipelineCreateInfo pci{ VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO };
    pci.stage = { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_COMPUTE_BIT, module, "main", nullptr };
    
    if ( gpu.required_subgroup_size ) {
        pci.stage.pNext = &required;
        pci.stage.flags = VK_PIPELINE_SHADER_STAGE_CREATE_REQUIRE_FULL_SUBGROUPS_BIT;
    }
    
    pci.layout = layout;
    
    VkPipeline pipeline = VK_NULL_HANDLE;
    
    const VkResult res = vkCreateComputePipelines( gpu.device, VK_NULL_HANDLE, 1, &pci, nullptr, &pipeline );
    vkDestroyShaderModule( gpu.device, module, nullptr );
    
    if ( res != VK_SUCCESS ) {
        error = "pipeline creation failed (VkResult " + std::to_string( res ) + ")";
        return VK_NULL_HANDLE;
    }

    return pipeline;
}

// Benchmark /////////////////////////////////////////////////////////////
void gpu_run_benchmark( Gpu& gpu, const AppOptions& opt, const std::vector<AppRunnableTest*>& active ) {

    VkQueryPoolCreateInfo qci{ VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO };
    qci.queryType = VK_QUERY_TYPE_TIMESTAMP;
    qci.queryCount = ( uint32_t )active.size() * 2;

    VkQueryPool query_pool;
    VK_CHECK( vkCreateQueryPool( gpu.device, &qci, nullptr, &query_pool ) );

    VkCommandBufferAllocateInfo cai{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
    cai.commandPool = gpu.command_pool;
    cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cai.commandBufferCount = 1;

    VkCommandBuffer cmd;
    VK_CHECK( vkAllocateCommandBuffers( gpu.device, &cai, &cmd ) );

    VkFenceCreateInfo fci{ VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
    VkFence fence;
    VK_CHECK( vkCreateFence( gpu.device, &fci, nullptr, &fence ) );

    const uint64_t timestamp_mask = gpu.timestamp_valid_bits >= 64 ? ~0ull : ( ( 1ull << gpu.timestamp_valid_bits ) - 1 );
    const double ns_per_tick = ( double )gpu.props.limits.timestampPeriod;

    std::vector<uint64_t> timestamps( active.size() * 2 );
    std::vector<size_t> order( active.size() );
    std::iota( order.begin(), order.end(), 0 );
    std::mt19937 rng( 12345 );

    for ( AppRunnableTest* t : active ) {
        t->samples_ms.clear();
    }

    printf( "Running %d warm-up frames and %d benchmark frames%s:\n", opt.warmup_frames, opt.bench_frames, opt.shuffle ? " (shuffled order)" : "" );

    const int total_frames = opt.warmup_frames + opt.bench_frames;

    for ( int frame = 0; frame < total_frames; ++frame ) {
        if ( opt.shuffle ) {
            std::shuffle( order.begin(), order.end(), rng );
        }

        VkCommandBufferBeginInfo bi{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
        bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        VK_CHECK( vkBeginCommandBuffer( cmd, &bi ) );

        vkCmdResetQueryPool( cmd, query_pool, 0, qci.queryCount );

        for ( size_t i : order ) {
            // Serialize dispatches (D3D11 does this implicitly for the shared output UAV), so each timing covers one dispatch.
            VkMemoryBarrier mb{ VK_STRUCTURE_TYPE_MEMORY_BARRIER };
            mb.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
            mb.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;

            vkCmdPipelineBarrier( cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &mb, 0, nullptr, 0, nullptr );
            vkCmdWriteTimestamp( cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, query_pool, ( uint32_t )i * 2 );

            active[ i ]->record( cmd );
            vkCmdWriteTimestamp( cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, query_pool, ( uint32_t )i * 2 + 1 );
        }

        gpu_end_cb_submit_and_wait( gpu, cmd, fence );
        VK_CHECK( vkResetCommandBuffer( cmd, 0 ) );

        VK_CHECK( vkGetQueryPoolResults( gpu.device, query_pool, 0, qci.queryCount, timestamps.size() * sizeof( uint64_t ),
                                         timestamps.data(), sizeof( uint64_t ), VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT ) );
        if ( frame >= opt.warmup_frames ) {
            for ( size_t i = 0; i < active.size(); ++i ) {
                const uint64_t ticks = ( ( timestamps[ i * 2 + 1 ] & timestamp_mask ) - ( timestamps[ i * 2 ] & timestamp_mask ) ) & timestamp_mask;

                active[ i ]->samples_ms.push_back( ticks * ns_per_tick * 1e-6 );
            }
        }

        printf( "%c", frame < opt.warmup_frames ? '.' : 'X' );
        fflush( stdout );
    }

    printf( "\n" );

    vkDestroyFence( gpu.device, fence, nullptr );
    vkFreeCommandBuffers( gpu.device, gpu.command_pool, 1, &cmd );
    vkDestroyQueryPool( gpu.device, query_pool, nullptr );
}
