// MIT License. Original PerfTest (c) 2016-2017 Sebastian Aaltonen. Vulkan port (c) 2026 Gabriel Sassone.
#pragma once

#include <vulkan/vulkan.h>

#include <cstdint>

#include "app.hpp"

// Gpu ///////////////////////////////////////////////////////////////

//
//
struct GpuBuffer {
    VkBuffer        buffer = VK_NULL_HANDLE;
    VkDeviceMemory  memory = VK_NULL_HANDLE;
    VkDeviceSize    size   = 0;
    void*           mapped = nullptr;        // host-visible buffers only
}; // struct GpuBuffer

//
//
struct GpuImage {
    VkImage         image  = VK_NULL_HANDLE;
    VkImageView     view   = VK_NULL_HANDLE;
    VkDeviceMemory  memory = VK_NULL_HANDLE;
}; // struct GpuImage

//
//
struct GpuFeatures {
    bool        storage_write_without_format   = false;
    bool        storage_read_without_format    = false;
    bool        shader_int64                   = false;
    bool        ssbo_dynamic_indexing          = false;
    bool        ssbo_nonuniform_indexing       = false;
    bool        buffer_device_address          = false;
    bool        buffer_int64_atomics           = false;   // shaderBufferInt64Atomics (64-bit atomic max test)
    bool        bindless                       = false;   // everything the bindless layout needs (bindless suite)
    bool        subgroup_ballot                = false;   // WaveReadLaneFirst in compute
    bool        robustness2                    = false;   // robustBufferAccess2 enabled (with --robustness)
    std::string bindless_missing;                  // why the bindless suite cannot run
}; // struct GpuFeatures

//
//
struct Gpu {
    VkInstance                      instance        = VK_NULL_HANDLE;
    VkDebugUtilsMessengerEXT        messenger       = VK_NULL_HANDLE;
    VkPhysicalDevice                physical        = VK_NULL_HANDLE;
    VkPhysicalDeviceProperties      props{};
    VkPhysicalDeviceMemoryProperties memory_props{};
    VkDevice                        device          = VK_NULL_HANDLE;
    VkQueue                         queue           = VK_NULL_HANDLE;
    uint32_t                        queue_family    = 0;
    uint32_t                        timestamp_valid_bits = 0;
    uint32_t                        subgroup_size   = 0;        // reported default subgroup size
    uint32_t                        required_subgroup_size = 0; // from --subgroup-size (0 = none)
    GpuFeatures                     features;
    PFN_vkGetBufferDeviceAddress    get_buffer_address = nullptr;
    VkCommandPool                   command_pool    = VK_NULL_HANDLE;
}; // struct Gpu

extern uint32_t g_validation_messages;

void            gpu_create( Gpu& gpu, const AppOptions& opt );
void            gpu_destroy( Gpu& gpu );

const char*     gpu_vendor_name( uint32_t id );
std::string     gpu_driver_version_string( const VkPhysicalDeviceProperties& p );
std::string     gpu_config_string( const Gpu& gpu, const AppOptions& opt );   // "default", "robustness", "subgroup32"...

uint32_t        gpu_find_memory_type( const Gpu& gpu, uint32_t type_bits, VkMemoryPropertyFlags wanted );
GpuBuffer       gpu_create_buffer( Gpu& gpu, VkDeviceSize size, VkBufferUsageFlags usage );            // device local
GpuBuffer       gpu_create_host_buffer( Gpu& gpu, VkDeviceSize size, VkBufferUsageFlags usage );       // host visible + coherent, mapped
void            gpu_destroy_buffer( Gpu& gpu, GpuBuffer& b );
GpuImage        gpu_create_image( Gpu& gpu, VkFormat format, uint32_t width, uint32_t height, VkImageUsageFlags usage );
void            gpu_destroy_image( Gpu& gpu, GpuImage& img );
VkBufferView    gpu_create_buffer_view( Gpu& gpu, const GpuBuffer& b, VkFormat format, VkDeviceSize range );
VkDeviceAddress gpu_buffer_address( Gpu& gpu, const GpuBuffer& b );
VkCommandBuffer gpu_allocated_and_begin_cb( Gpu& gpu );
void            gpu_end_cb_submit_and_wait( Gpu& gpu, VkCommandBuffer cb, VkFence fence );
void            gpu_end_cb_submit_wait_free_cb( Gpu& gpu, VkCommandBuffer cb );   // submit, wait, free
// Compute pipeline from a .spv file; applies --subgroup-size. Returns VK_NULL_HANDLE and sets `error` on failure.
VkPipeline      gpu_create_compute_pipeline( Gpu& gpu, VkPipelineLayout layout, const std::string& spv_path, std::string& error );



// Records every test once per frame (compute->compute barrier, timestamp, dispatch, timestamp) and collects
// one sample per benchmark frame into AppRunnableTest::samples_ms.
void            gpu_run_benchmark( Gpu& gpu, const AppOptions& opt, const std::vector<AppRunnableTest*>& active );

