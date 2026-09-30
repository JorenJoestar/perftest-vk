// bindless_layout.cpp - see bindless_layout.hpp.
#include "bindless_layout.hpp"

#include <algorithm>
#include <cassert>

namespace {

// One table drives both the check and the enable, so they cannot drift apart.
#define BINDLESS_CORE_FEATURES( X )                    \
    X( shaderSampledImageArrayDynamicIndexing )        \
    X( shaderStorageBufferArrayDynamicIndexing )

#define BINDLESS_V12_FEATURES( X )                     \
    X( runtimeDescriptorArray )                        \
    X( descriptorBindingPartiallyBound )               \
    X( descriptorBindingUpdateUnusedWhilePending )     \
    X( descriptorBindingSampledImageUpdateAfterBind )  \
    X( descriptorBindingStorageBufferUpdateAfterBind ) \
    X( shaderSampledImageArrayNonUniformIndexing )     \
    X( shaderStorageBufferArrayNonUniformIndexing )

void write( VkDevice device, const BindlessLayout& layout, uint32_t binding, uint32_t slot, VkDescriptorType type,
            const VkDescriptorImageInfo* image, const VkDescriptorBufferInfo* buffer ) {
    VkWriteDescriptorSet w{ VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
    w.dstSet = layout.set;
    w.dstBinding = binding;
    w.dstArrayElement = slot;
    w.descriptorCount = 1;
    w.descriptorType = type;
    w.pImageInfo = image;
    w.pBufferInfo = buffer;
    vkUpdateDescriptorSets( device, 1, &w, 0, nullptr );
}

} // namespace

bool bindless_layout_check_device( VkPhysicalDevice physical, const char** missing ) {
    VkPhysicalDeviceVulkan12Features v12{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES };
    VkPhysicalDeviceFeatures2 f2{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2 };
    f2.pNext = &v12;
    vkGetPhysicalDeviceFeatures2( physical, &f2 );
    const char* first_missing = nullptr;
#define BINDLESS_CHECK_CORE( name ) if ( !first_missing && !f2.features.name ) first_missing = #name;
#define BINDLESS_CHECK_V12( name )  if ( !first_missing && !v12.name ) first_missing = #name;
    BINDLESS_CORE_FEATURES( BINDLESS_CHECK_CORE )
    BINDLESS_V12_FEATURES( BINDLESS_CHECK_V12 )
#undef BINDLESS_CHECK_V12
#undef BINDLESS_CHECK_CORE
    if ( missing ) *missing = first_missing;
    return first_missing == nullptr;
}

void bindless_layout_enable_features( VkPhysicalDeviceFeatures& core, VkPhysicalDeviceVulkan12Features& v12 ) {
#define BINDLESS_ENABLE_CORE( name ) core.name = VK_TRUE;
#define BINDLESS_ENABLE_V12( name )  v12.name = VK_TRUE;
    BINDLESS_CORE_FEATURES( BINDLESS_ENABLE_CORE )
    BINDLESS_V12_FEATURES( BINDLESS_ENABLE_V12 )
#undef BINDLESS_ENABLE_V12
#undef BINDLESS_ENABLE_CORE
}

VkResult bindless_layout_create( VkPhysicalDevice physical, VkDevice device, const BindlessLayoutDesc& desc, BindlessLayout& layout ) {
    layout = BindlessLayout{};

    // Clamp the capacities to the device limits
    VkPhysicalDeviceVulkan12Properties v12{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_PROPERTIES };
    VkPhysicalDeviceProperties2 props{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2 };
    props.pNext = &v12;
    
    vkGetPhysicalDeviceProperties2( physical, &props );
    
    layout.texture_capacity = std::min( { desc.texture_capacity, v12.maxDescriptorSetUpdateAfterBindSampledImages,
                                          v12.maxPerStageDescriptorUpdateAfterBindSampledImages } );
    layout.buffer_capacity  = std::min( { desc.buffer_capacity, v12.maxDescriptorSetUpdateAfterBindStorageBuffers,
                                          v12.maxPerStageDescriptorUpdateAfterBindStorageBuffers } );
    // Textures and buffers share maxPerStageUpdateAfterBindResources: shrink buffers, then textures, if needed.
    for ( uint32_t* capacity : { &layout.buffer_capacity, &layout.texture_capacity } ) {

        const uint64_t resources = ( uint64_t )layout.texture_capacity + layout.buffer_capacity;

        if ( resources > v12.maxPerStageUpdateAfterBindResources ) {
            const uint64_t excess = resources - v12.maxPerStageUpdateAfterBindResources;
            *capacity -= ( uint32_t )std::min<uint64_t>( excess, *capacity / 2 );
        }
    }

    // Immutable sampler
    VkSamplerCreateInfo sampler_ci{ VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
    sampler_ci.magFilter = sampler_ci.minFilter = VK_FILTER_NEAREST;
    sampler_ci.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    sampler_ci.addressModeU = sampler_ci.addressModeV = sampler_ci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sampler_ci.maxLod = VK_LOD_CLAMP_NONE;
    
    VkResult result = vkCreateSampler( device, &sampler_ci, nullptr, &layout.sampler );
    
    if ( result != VK_SUCCESS ) { 
        bindless_layout_destroy( device, layout ); return result;
    }

    // Set layout
    const VkDescriptorBindingFlags array_flags = VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT
                                               | VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT
                                               | VK_DESCRIPTOR_BINDING_UPDATE_UNUSED_WHILE_PENDING_BIT;
    const VkDescriptorSetLayoutBinding bindings[] = {
        { k_bindless_binding_textures, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,  layout.texture_capacity, VK_SHADER_STAGE_ALL, nullptr },
        { k_bindless_binding_buffers,  VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, layout.buffer_capacity,  VK_SHADER_STAGE_ALL, nullptr },
        { k_bindless_binding_sampler,  VK_DESCRIPTOR_TYPE_SAMPLER,        1,                       VK_SHADER_STAGE_ALL, &layout.sampler },
    };

    const VkDescriptorBindingFlags flags[] = { array_flags, array_flags, 0 };
    VkDescriptorSetLayoutBindingFlagsCreateInfo flags_ci{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO };
    flags_ci.bindingCount = 3;
    flags_ci.pBindingFlags = flags;
    
    VkDescriptorSetLayoutCreateInfo set_ci{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
    set_ci.pNext = &flags_ci;
    set_ci.flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_UPDATE_AFTER_BIND_POOL_BIT;
    set_ci.bindingCount = 3;
    set_ci.pBindings = bindings;
    result = vkCreateDescriptorSetLayout( device, &set_ci, nullptr, &layout.set_layout );
    
    if ( result != VK_SUCCESS ) {
        bindless_layout_destroy( device, layout ); return result;
    }

    // Bindless test pipeline layout
    const VkPushConstantRange push{ VK_SHADER_STAGE_ALL, 0, k_bindless_push_size };
    VkPipelineLayoutCreateInfo pl_ci{ VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
    pl_ci.setLayoutCount = 1;
    pl_ci.pSetLayouts = &layout.set_layout;
    pl_ci.pushConstantRangeCount = 1;
    pl_ci.pPushConstantRanges = &push;
    
    result = vkCreatePipelineLayout( device, &pl_ci, nullptr, &layout.pipeline_layout );
    
    if ( result != VK_SUCCESS ) {
        bindless_layout_destroy( device, layout ); return result;
    }

    // Pool and set
    const VkDescriptorPoolSize sizes[] = {
        { VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,  layout.texture_capacity },
        { VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, layout.buffer_capacity },
        { VK_DESCRIPTOR_TYPE_SAMPLER,        1 },
    };

    VkDescriptorPoolCreateInfo pool_ci{ VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
    pool_ci.flags = VK_DESCRIPTOR_POOL_CREATE_UPDATE_AFTER_BIND_BIT;
    pool_ci.maxSets = 1;
    pool_ci.poolSizeCount = 3;
    pool_ci.pPoolSizes = sizes;
    
    result = vkCreateDescriptorPool( device, &pool_ci, nullptr, &layout.pool );
    
    if ( result != VK_SUCCESS ) {
        bindless_layout_destroy( device, layout ); return result;
    }

    VkDescriptorSetAllocateInfo alloc{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
    alloc.descriptorPool = layout.pool;
    alloc.descriptorSetCount = 1;
    alloc.pSetLayouts = &layout.set_layout;
    result = vkAllocateDescriptorSets( device, &alloc, &layout.set );
    if ( result != VK_SUCCESS ) {
        bindless_layout_destroy( device, layout ); return result;
    }
    return VK_SUCCESS;
}

void bindless_layout_destroy( VkDevice device, BindlessLayout& layout ) {
    if ( layout.pool ) {
        vkDestroyDescriptorPool( device, layout.pool, nullptr );
    }

    if ( layout.pipeline_layout ) {
        vkDestroyPipelineLayout( device, layout.pipeline_layout, nullptr );
    }

    if ( layout.set_layout ) {
        vkDestroyDescriptorSetLayout( device, layout.set_layout, nullptr );
    }

    if ( layout.sampler ) {
        vkDestroySampler( device, layout.sampler, nullptr );
    }

    layout = BindlessLayout{};
}

void bindless_write_sampled_image( VkDevice device, const BindlessLayout& layout, uint32_t slot, VkImageView view ) {
    assert( slot < layout.texture_capacity );
    const VkDescriptorImageInfo info{ VK_NULL_HANDLE, view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
    write( device, layout, k_bindless_binding_textures, slot, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, &info, nullptr );
}

void bindless_write_buffer( VkDevice device, const BindlessLayout& layout, uint32_t slot, VkBuffer buffer, VkDeviceSize offset, VkDeviceSize range ) {
    assert( slot < layout.buffer_capacity );
    const VkDescriptorBufferInfo info{ buffer, offset, range };
    write( device, layout, k_bindless_binding_buffers, slot, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, nullptr, &info );
}

void cmd_bind_bindless_layout( VkCommandBuffer cb, const BindlessLayout& layout, VkPipelineBindPoint bind_point ) {
    vkCmdBindDescriptorSets( cb, bind_point, layout.pipeline_layout, 0, 1, &layout.set, 0, nullptr );
}

void cmd_bindless_push_constants( VkCommandBuffer cb, const BindlessLayout& layout, const void* data, uint32_t size ) {
    assert( size <= k_bindless_push_size );
    vkCmdPushConstants( cb, layout.pipeline_layout, VK_SHADER_STAGE_ALL, 0, size, data );
}
