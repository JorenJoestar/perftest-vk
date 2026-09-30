// bindless_layout.hpp - the single descriptor/pipeline layout of the bindless suite.
//
// A typical "bindless" engine layout, reduced to what the tests use:
//   set 0, every binding a runtime-sized array with UPDATE_AFTER_BIND | PARTIALLY_BOUND | UPDATE_UNUSED_WHILE_PENDING
//     binding 0  SAMPLED_IMAGE[]    Texture2D<float4>
//     binding 1  STORAGE_BUFFER[]   every buffer view: StructuredBuffer<T>, RWStructuredBuffer<T>, (RW)ByteAddressBuffer
//     binding 2  SAMPLER            immutable point/clamp sampler (not an array, no binding flags)
//   push constants: 128 bytes, VK_SHADER_STAGE_ALL
// One pipeline layout for every test: pipelines change, the set stays bound, per-dispatch data goes in push constants.
//
// What matters for the measurements is the descriptor type, the binding flags, the array sizes and whether indices are
// NonUniform; the capacities default to engine-sized arrays (thousands of slots), clamped to the device limits.
// Binding numbers must match shaders/bindless/bindless.hlsli.
#pragma once

#include <vulkan/vulkan.h>

#include <cstdint>

enum : uint32_t {
    k_bindless_binding_textures = 0,
    k_bindless_binding_buffers  = 1,
    k_bindless_binding_sampler  = 2,
    k_bindless_push_size        = 128,
};

struct BindlessLayoutDesc {
    uint32_t            texture_capacity = 16384;      // requested slots, clamped to the device limits
    uint32_t            buffer_capacity  = 65536;
};

struct BindlessLayout {
    VkDescriptorSetLayout set_layout       = VK_NULL_HANDLE;
    VkPipelineLayout      pipeline_layout  = VK_NULL_HANDLE;
    VkDescriptorPool      pool             = VK_NULL_HANDLE;
    VkDescriptorSet       set              = VK_NULL_HANDLE;
    VkSampler             sampler          = VK_NULL_HANDLE;
    uint32_t              texture_capacity = 0;     // actual, after clamping
    uint32_t              buffer_capacity  = 0;
};

// Checks the features the layout needs. On failure returns false and the name of the first missing feature.
bool        bindless_layout_check_device( VkPhysicalDevice physical, const char** missing );
// Turns on the needed features in the structs you chain into VkDeviceCreateInfo.
void        bindless_layout_enable_features( VkPhysicalDeviceFeatures& core, VkPhysicalDeviceVulkan12Features& v12 );

VkResult    bindless_layout_create( VkPhysicalDevice physical, VkDevice device, const BindlessLayoutDesc& desc, BindlessLayout& layout );
void        bindless_layout_destroy( VkDevice device, BindlessLayout& layout );

void        bindless_write_sampled_image( VkDevice device, const BindlessLayout& layout, uint32_t slot, VkImageView view );
void        bindless_write_buffer( VkDevice device, const BindlessLayout& layout, uint32_t slot, VkBuffer buffer,
                                   VkDeviceSize offset = 0, VkDeviceSize range = VK_WHOLE_SIZE );

void        cmd_bind_bindless_layout( VkCommandBuffer cb, const BindlessLayout& layout, VkPipelineBindPoint bind_point );
void        cmd_bindless_push_constants( VkCommandBuffer cb, const BindlessLayout& layout, const void* data, uint32_t size );
