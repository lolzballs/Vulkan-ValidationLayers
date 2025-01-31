/* Copyright (c) 2020-2024 The Khronos Group Inc.
 * Copyright (c) 2020-2024 Valve Corporation
 * Copyright (c) 2020-2024 LunarG, Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "gpu/instrumentation/gpuav_shader_instrumentor.h"
#include <vulkan/vulkan_core.h>
#include <spirv/unified1/NonSemanticShaderDebugInfo100.h>
#include <spirv/unified1/spirv.hpp>

#include "generated/vk_extension_helper.h"
#include "gpu/shaders/gpuav_shaders_constants.h"
#include "gpu/spirv/module.h"
#include "chassis/chassis_modification_state.h"
#include "gpu/shaders/gpuav_error_codes.h"
#include "spirv-tools/optimizer.hpp"
#include "utils/vk_layer_utils.h"
#include "sync/sync_utils.h"
#include "state_tracker/pipeline_state.h"
#include "error_message/spirv_logging.h"

#include "state_tracker/descriptor_sets.h"
#include "state_tracker/shader_object_state.h"
#include "state_tracker/shader_instruction.h"

#include <cassert>
#include <fstream>
#include <string>

namespace gpuav {

void SpirvCache::Add(uint32_t hash, std::vector<uint32_t> spirv) { spirv_shaders_.emplace(hash, std::move(spirv)); }

std::vector<uint32_t> *SpirvCache::Get(uint32_t spirv_hash) {
    auto it = spirv_shaders_.find(spirv_hash);
    if (it != spirv_shaders_.end()) {
        return &it->second;
    }
    return nullptr;
}

ReadLockGuard GpuShaderInstrumentor::ReadLock() const {
    if (global_settings.fine_grained_locking) {
        return ReadLockGuard(validation_object_mutex, std::defer_lock);
    } else {
        return ReadLockGuard(validation_object_mutex);
    }
}

WriteLockGuard GpuShaderInstrumentor::WriteLock() {
    if (global_settings.fine_grained_locking) {
        return WriteLockGuard(validation_object_mutex, std::defer_lock);
    } else {
        return WriteLockGuard(validation_object_mutex);
    }
}

// In charge of getting things for shader instrumentation that both GPU-AV and DebugPrintF will need
void GpuShaderInstrumentor::PostCreateDevice(const VkDeviceCreateInfo *pCreateInfo, const Location &loc) {
    BaseClass::PostCreateDevice(pCreateInfo, loc);

    VkPhysicalDeviceFeatures supported_features{};
    DispatchGetPhysicalDeviceFeatures(physical_device, &supported_features);
    if (!supported_features.fragmentStoresAndAtomics) {
        InternalError(
            device, loc,
            "GPU Shader Instrumentation requires fragmentStoresAndAtomics to allow writting out data inside the fragment shader.");
        return;
    }
    if (!supported_features.vertexPipelineStoresAndAtomics) {
        InternalError(device, loc,
                      "GPU Shader Instrumentation requires vertexPipelineStoresAndAtomics to allow writting out data inside the "
                      "vertex shader.");
        return;
    }

    // maxBoundDescriptorSets limit, but possibly adjusted
    const uint32_t adjusted_max_desc_sets_limit =
        std::min(kMaxAdjustedBoundDescriptorSet, phys_dev_props.limits.maxBoundDescriptorSets);
    // If gpu_validation_reserve_binding_slot: the max slot is where we reserved
    // else: always use the last possible set as least likely to be used
    instrumentation_desc_set_bind_index_ = adjusted_max_desc_sets_limit - 1;

    // We can't do anything if there is only one.
    // Device probably not a legit Vulkan device, since there should be at least 4. Protect ourselves.
    if (adjusted_max_desc_sets_limit == 1) {
        InternalError(device, loc, "Device can bind only a single descriptor set.");
        return;
    }

    const VkDescriptorSetLayoutCreateInfo debug_desc_layout_info = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, nullptr, 0,
                                                                    static_cast<uint32_t>(instrumentation_bindings_.size()),
                                                                    instrumentation_bindings_.data()};

    VkResult result = DispatchCreateDescriptorSetLayout(device, &debug_desc_layout_info, nullptr, &instrumentation_desc_layout_);
    if (result != VK_SUCCESS) {
        InternalError(device, loc, "vkCreateDescriptorSetLayout failed for internal descriptor set");
        Cleanup();
        return;
    }

    const VkDescriptorSetLayoutCreateInfo dummy_desc_layout_info = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, nullptr, 0,
                                                                    0, nullptr};
    result = DispatchCreateDescriptorSetLayout(device, &dummy_desc_layout_info, nullptr, &dummy_desc_layout_);
    if (result != VK_SUCCESS) {
        InternalError(device, loc, "vkCreateDescriptorSetLayout failed for internal dummy descriptor set");
        Cleanup();
        return;
    }

    std::vector<VkDescriptorSetLayout> debug_layouts;
    for (uint32_t j = 0; j < instrumentation_desc_set_bind_index_; ++j) {
        debug_layouts.push_back(dummy_desc_layout_);
    }
    debug_layouts.push_back(instrumentation_desc_layout_);

    const VkPipelineLayoutCreateInfo debug_pipeline_layout_info = {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
                                                                   nullptr,
                                                                   0u,
                                                                   static_cast<uint32_t>(debug_layouts.size()),
                                                                   debug_layouts.data(),
                                                                   0u,
                                                                   nullptr};
    result = DispatchCreatePipelineLayout(device, &debug_pipeline_layout_info, nullptr, &instrumentation_pipeline_layout_);
    if (result != VK_SUCCESS) {
        InternalError(device, loc, "vkCreateDescriptorSetLayout failed for internal pipeline layout");
        Cleanup();
        return;
    }
}

void GpuShaderInstrumentor::Cleanup() {
    if (instrumentation_desc_layout_) {
        DispatchDestroyDescriptorSetLayout(device, instrumentation_desc_layout_, nullptr);
        instrumentation_desc_layout_ = VK_NULL_HANDLE;
    }
    if (dummy_desc_layout_) {
        DispatchDestroyDescriptorSetLayout(device, dummy_desc_layout_, nullptr);
        dummy_desc_layout_ = VK_NULL_HANDLE;
    }

    if (instrumentation_pipeline_layout_) {
        DispatchDestroyPipelineLayout(device, instrumentation_pipeline_layout_, nullptr);
        instrumentation_pipeline_layout_ = VK_NULL_HANDLE;
    }
}

void GpuShaderInstrumentor::PreCallRecordDestroyDevice(VkDevice device, const VkAllocationCallbacks *pAllocator,
                                                       const RecordObject &record_obj) {
    Cleanup();
    BaseClass::PreCallRecordDestroyDevice(device, pAllocator, record_obj);
}

void GpuShaderInstrumentor::ReserveBindingSlot(VkPhysicalDevice physicalDevice, VkPhysicalDeviceLimits &limits,
                                               const Location &loc) {
    // There is an implicit layer that can cause this call to return 0 for maxBoundDescriptorSets - Ignore such calls
    if (limits.maxBoundDescriptorSets == 0) return;

    if (limits.maxBoundDescriptorSets > kMaxAdjustedBoundDescriptorSet) {
        std::stringstream ss;
        ss << "A descriptor binding slot is required to store GPU-side information, but the device maxBoundDescriptorSets is "
           << limits.maxBoundDescriptorSets << " which is too large, so we will be trying to use slot "
           << kMaxAdjustedBoundDescriptorSet;
        InternalWarning(physicalDevice, loc, ss.str().c_str());
    }

    if (enabled[gpu_validation_reserve_binding_slot]) {
        if (limits.maxBoundDescriptorSets > 1) {
            limits.maxBoundDescriptorSets -= 1;
        } else {
            InternalWarning(physicalDevice, loc, "Unable to reserve descriptor binding slot on a device with only one slot.");
        }
    }
}

void GpuShaderInstrumentor::PostCallRecordGetPhysicalDeviceProperties(VkPhysicalDevice physicalDevice,
                                                                      VkPhysicalDeviceProperties *device_props,
                                                                      const RecordObject &record_obj) {
    ReserveBindingSlot(physicalDevice, device_props->limits, record_obj.location);
}

void GpuShaderInstrumentor::PostCallRecordGetPhysicalDeviceProperties2(VkPhysicalDevice physicalDevice,
                                                                       VkPhysicalDeviceProperties2 *device_props2,
                                                                       const RecordObject &record_obj) {
    ReserveBindingSlot(physicalDevice, device_props2->properties.limits, record_obj.location);
}

// Just gives a warning about a possible deadlock.
bool GpuShaderInstrumentor::ValidateCmdWaitEvents(VkCommandBuffer command_buffer, VkPipelineStageFlags2 src_stage_mask,
                                                  const Location &loc) const {
    if (src_stage_mask & VK_PIPELINE_STAGE_2_HOST_BIT) {
        std::ostringstream error_msg;
        error_msg << loc.Message()
                  << ": recorded with VK_PIPELINE_STAGE_HOST_BIT set. GPU-Assisted validation waits on queue completion. This wait "
                     "could block the host's signaling of this event, resulting in deadlock.";
        InternalError(command_buffer, loc, error_msg.str().c_str());
    }
    return false;
}

bool GpuShaderInstrumentor::PreCallValidateCmdWaitEvents(
    VkCommandBuffer commandBuffer, uint32_t eventCount, const VkEvent *pEvents, VkPipelineStageFlags srcStageMask,
    VkPipelineStageFlags dstStageMask, uint32_t memoryBarrierCount, const VkMemoryBarrier *pMemoryBarriers,
    uint32_t bufferMemoryBarrierCount, const VkBufferMemoryBarrier *pBufferMemoryBarriers, uint32_t imageMemoryBarrierCount,
    const VkImageMemoryBarrier *pImageMemoryBarriers, const ErrorObject &error_obj) const {
    BaseClass::PreCallValidateCmdWaitEvents(commandBuffer, eventCount, pEvents, srcStageMask, dstStageMask, memoryBarrierCount,
                                            pMemoryBarriers, bufferMemoryBarrierCount, pBufferMemoryBarriers,
                                            imageMemoryBarrierCount, pImageMemoryBarriers, error_obj);
    return ValidateCmdWaitEvents(commandBuffer, static_cast<VkPipelineStageFlags2>(srcStageMask), error_obj.location);
}

bool GpuShaderInstrumentor::PreCallValidateCmdWaitEvents2KHR(VkCommandBuffer commandBuffer, uint32_t eventCount,
                                                             const VkEvent *pEvents, const VkDependencyInfoKHR *pDependencyInfos,
                                                             const ErrorObject &error_obj) const {
    return PreCallValidateCmdWaitEvents2(commandBuffer, eventCount, pEvents, pDependencyInfos, error_obj);
}

bool GpuShaderInstrumentor::PreCallValidateCmdWaitEvents2(VkCommandBuffer commandBuffer, uint32_t eventCount,
                                                          const VkEvent *pEvents, const VkDependencyInfo *pDependencyInfos,
                                                          const ErrorObject &error_obj) const {
    VkPipelineStageFlags2 src_stage_mask = 0;

    for (uint32_t i = 0; i < eventCount; i++) {
        auto stage_masks = sync_utils::GetGlobalStageMasks(pDependencyInfos[i]);
        src_stage_mask |= stage_masks.src;
    }

    BaseClass::PreCallValidateCmdWaitEvents2(commandBuffer, eventCount, pEvents, pDependencyInfos, error_obj);
    return ValidateCmdWaitEvents(commandBuffer, src_stage_mask, error_obj.location);
}

void GpuShaderInstrumentor::PreCallRecordCreatePipelineLayout(VkDevice device, const VkPipelineLayoutCreateInfo *pCreateInfo,
                                                              const VkAllocationCallbacks *pAllocator,
                                                              VkPipelineLayout *pPipelineLayout, const RecordObject &record_obj,
                                                              chassis::CreatePipelineLayout &chassis_state) {
    if (gpuav_settings.IsSpirvModified()) {
        if (chassis_state.modified_create_info.setLayoutCount > instrumentation_desc_set_bind_index_) {
            std::ostringstream strm;
            strm << "pCreateInfo::setLayoutCount (" << chassis_state.modified_create_info.setLayoutCount
                 << ") will conflicts with validation's descriptor set at slot " << instrumentation_desc_set_bind_index_ << ". "
                 << "This Pipeline Layout has too many descriptor sets that will not allow GPU shader instrumentation to be setup "
                    "for "
                    "pipelines created with it, therefor no validation error will be repored for them by GPU-AV at "
                    "runtime.";
            InternalWarning(device, record_obj.location, strm.str().c_str());
        } else {
            // Modify the pipeline layout by:
            // 1. Copying the caller's descriptor set desc_layouts
            // 2. Fill in dummy descriptor layouts up to the max binding
            // 3. Fill in with the debug descriptor layout at the max binding slot
            chassis_state.new_layouts.reserve(instrumentation_desc_set_bind_index_ + 1);
            chassis_state.new_layouts.insert(chassis_state.new_layouts.end(), &pCreateInfo->pSetLayouts[0],
                                             &pCreateInfo->pSetLayouts[pCreateInfo->setLayoutCount]);
            for (uint32_t i = pCreateInfo->setLayoutCount; i < instrumentation_desc_set_bind_index_; ++i) {
                chassis_state.new_layouts.push_back(dummy_desc_layout_);
            }
            chassis_state.new_layouts.push_back(instrumentation_desc_layout_);
            chassis_state.modified_create_info.pSetLayouts = chassis_state.new_layouts.data();
            chassis_state.modified_create_info.setLayoutCount = instrumentation_desc_set_bind_index_ + 1;
        }
    }
    BaseClass::PreCallRecordCreatePipelineLayout(device, pCreateInfo, pAllocator, pPipelineLayout, record_obj, chassis_state);
}

void GpuShaderInstrumentor::PostCallRecordCreatePipelineLayout(VkDevice device, const VkPipelineLayoutCreateInfo *pCreateInfo,
                                                               const VkAllocationCallbacks *pAllocator,
                                                               VkPipelineLayout *pPipelineLayout, const RecordObject &record_obj) {
    if (record_obj.result != VK_SUCCESS) {
        InternalError(device, record_obj.location, "Unable to create pipeline layout.");
        return;
    }
    BaseClass::PostCallRecordCreatePipelineLayout(device, pCreateInfo, pAllocator, pPipelineLayout, record_obj);
}

void GpuShaderInstrumentor::PostCallRecordCreateShaderModule(VkDevice device, const VkShaderModuleCreateInfo *pCreateInfo,
                                                             const VkAllocationCallbacks *pAllocator, VkShaderModule *pShaderModule,
                                                             const RecordObject &record_obj,
                                                             chassis::CreateShaderModule &chassis_state) {
    BaseClass::PostCallRecordCreateShaderModule(device, pCreateInfo, pAllocator, pShaderModule, record_obj, chassis_state);

    // By default, we instrument everything, but if the setting is enabled, we only will instrument the shaders the app picks
    if (gpuav_settings.select_instrumented_shaders && IsSelectiveInstrumentationEnabled(pCreateInfo->pNext)) {
        // If this is being filled up, likely only a few shaders and the app scope is narrowed down, so no need to spend time
        // removing these later
        selected_instrumented_shaders.insert(*pShaderModule);
    };
}

void GpuShaderInstrumentor::PreCallRecordShaderObjectInstrumentation(
    VkShaderCreateInfoEXT &create_info, const Location &create_info_loc,
    chassis::ShaderObjectInstrumentationData &instrumentation_data) {
    if (gpuav_settings.select_instrumented_shaders && !IsSelectiveInstrumentationEnabled(create_info.pNext)) return;
    uint32_t unique_shader_id = 0;
    bool cached = false;
    bool pass = false;
    std::vector<uint32_t> &instrumented_spirv = instrumentation_data.instrumented_spirv;
    if (gpuav_settings.cache_instrumented_shaders) {
        unique_shader_id = hash_util::ShaderHash(create_info.pCode, create_info.codeSize);
        if (const auto spirv = instrumented_shaders_cache_.Get(unique_shader_id)) {
            instrumented_spirv = *spirv;
            cached = true;
        }
    } else {
        unique_shader_id = unique_shader_module_id_++;
    }

    const bool has_bindless_descriptors = HasBindlessDescriptors(create_info);

    if (!cached) {
        pass = InstrumentShader(
            vvl::make_span(static_cast<const uint32_t *>(create_info.pCode), create_info.codeSize / sizeof(uint32_t)),
            unique_shader_id, has_bindless_descriptors, create_info_loc, instrumented_spirv);
    }

    if (cached || pass) {
        instrumentation_data.unique_shader_id = unique_shader_id;
        create_info.pCode = instrumented_spirv.data();
        create_info.codeSize = instrumented_spirv.size() * sizeof(uint32_t);
        if (gpuav_settings.cache_instrumented_shaders && !cached) {
            instrumented_shaders_cache_.Add(unique_shader_id, instrumented_spirv);
        }
    }
}

void GpuShaderInstrumentor::PreCallRecordCreateShadersEXT(VkDevice device, uint32_t createInfoCount,
                                                          const VkShaderCreateInfoEXT *pCreateInfos,
                                                          const VkAllocationCallbacks *pAllocator, VkShaderEXT *pShaders,
                                                          const RecordObject &record_obj, chassis::ShaderObject &chassis_state) {
    BaseClass::PreCallRecordCreateShadersEXT(device, createInfoCount, pCreateInfos, pAllocator, pShaders, record_obj,
                                             chassis_state);
    if (!gpuav_settings.IsSpirvModified()) return;

    chassis_state.modified_create_infos.reserve(createInfoCount);

    // Resize here so if using just CoreCheck we don't waste time allocating this
    chassis_state.instrumentations_data.resize(createInfoCount);

    for (uint32_t i = 0; i < createInfoCount; ++i) {
        VkShaderCreateInfoEXT new_create_info = pCreateInfos[i];
        auto &instrumentation_data = chassis_state.instrumentations_data[i];

        if (new_create_info.setLayoutCount > instrumentation_desc_set_bind_index_) {
            std::ostringstream strm;
            strm << "pCreateInfos[" << i << "]::setLayoutCount (" << new_create_info.setLayoutCount
                 << ") will conflicts with validation's descriptor set at slot " << instrumentation_desc_set_bind_index_ << ". "
                 << "This Shader Object has too many descriptor sets that will not allow GPU shader instrumentation to be setup "
                    "for VkShaderEXT created with it, therefor no validation error will be repored for them by GPU-AV at "
                    "runtime.";
            InternalWarning(device, record_obj.location, strm.str().c_str());
        } else {
            // Modify the pipeline layout by:
            // 1. Copying the caller's descriptor set desc_layouts
            // 2. Fill in dummy descriptor layouts up to the max binding
            // 3. Fill in with the debug descriptor layout at the max binding slot
            instrumentation_data.new_layouts.reserve(instrumentation_desc_set_bind_index_ + 1);
            instrumentation_data.new_layouts.insert(instrumentation_data.new_layouts.end(), pCreateInfos[i].pSetLayouts,
                                                    &pCreateInfos[i].pSetLayouts[pCreateInfos[i].setLayoutCount]);
            for (uint32_t j = pCreateInfos[i].setLayoutCount; j < instrumentation_desc_set_bind_index_; ++j) {
                instrumentation_data.new_layouts.push_back(dummy_desc_layout_);
            }
            instrumentation_data.new_layouts.push_back(instrumentation_desc_layout_);
            new_create_info.pSetLayouts = instrumentation_data.new_layouts.data();
            new_create_info.setLayoutCount = instrumentation_desc_set_bind_index_ + 1;
        }

        PreCallRecordShaderObjectInstrumentation(new_create_info, record_obj.location.dot(vvl::Field::pCreateInfos, i),
                                                 instrumentation_data);

        chassis_state.modified_create_infos.emplace_back(std::move(new_create_info));
    }

    chassis_state.pCreateInfos = reinterpret_cast<VkShaderCreateInfoEXT *>(chassis_state.modified_create_infos.data());
}

void GpuShaderInstrumentor::PostCallRecordCreateShadersEXT(VkDevice device, uint32_t createInfoCount,
                                                           const VkShaderCreateInfoEXT *pCreateInfos,
                                                           const VkAllocationCallbacks *pAllocator, VkShaderEXT *pShaders,
                                                           const RecordObject &record_obj, chassis::ShaderObject &chassis_state) {
    BaseClass::PostCallRecordCreateShadersEXT(device, createInfoCount, pCreateInfos, pAllocator, pShaders, record_obj,
                                              chassis_state);
    if (!gpuav_settings.IsSpirvModified()) return;

    for (uint32_t i = 0; i < createInfoCount; ++i) {
        auto &instrumentation_data = chassis_state.instrumentations_data[i];

        // if the shader for some reason was not instrumented, there is nothing to save
        if (!instrumentation_data.IsInstrumented()) {
            continue;
        }
        if (const auto &shader_object_state = Get<vvl::ShaderObject>(pShaders[i])) {
            shader_object_state->instrumentation_data.was_instrumented = true;
        }

        instrumented_shaders_map_.insert_or_assign(instrumentation_data.unique_shader_id, VK_NULL_HANDLE, VK_NULL_HANDLE,
                                                   pShaders[i], instrumentation_data.instrumented_spirv);
    }
}

void GpuShaderInstrumentor::PreCallRecordDestroyShaderEXT(VkDevice device, VkShaderEXT shader,
                                                          const VkAllocationCallbacks *pAllocator, const RecordObject &record_obj) {
    auto to_erase =
        instrumented_shaders_map_.snapshot([shader](const InstrumentedShader &entry) { return entry.shader_object == shader; });
    for (const auto &entry : to_erase) {
        instrumented_shaders_map_.erase(entry.first);
    }
    BaseClass::PreCallRecordDestroyShaderEXT(device, shader, pAllocator, record_obj);
}

void GpuShaderInstrumentor::PreCallRecordCreateGraphicsPipelines(VkDevice device, VkPipelineCache pipelineCache, uint32_t count,
                                                                 const VkGraphicsPipelineCreateInfo *pCreateInfos,
                                                                 const VkAllocationCallbacks *pAllocator, VkPipeline *pPipelines,
                                                                 const RecordObject &record_obj, PipelineStates &pipeline_states,
                                                                 chassis::CreateGraphicsPipelines &chassis_state) {
    BaseClass::PreCallRecordCreateGraphicsPipelines(device, pipelineCache, count, pCreateInfos, pAllocator, pPipelines, record_obj,
                                                    pipeline_states, chassis_state);
    if (!gpuav_settings.IsSpirvModified()) return;

    chassis_state.shader_instrumentations_metadata.resize(count);
    chassis_state.modified_create_infos.resize(count);

    for (uint32_t i = 0; i < count; ++i) {
        const auto &pipeline_state = pipeline_states[i];

        // Need to make a deep copy so if SPIR-V is inlined, user doesn't see it after the call
        auto &new_pipeline_ci = chassis_state.modified_create_infos[i];
        new_pipeline_ci.initialize(&pipeline_state->GraphicsCreateInfo());

        if (!NeedPipelineCreationShaderInstrumentation(*pipeline_state)) {
            continue;
        }

        const Location create_info_loc = record_obj.location.dot(vvl::Field::pCreateInfos, i);
        auto &shader_instrumentation_metadata = chassis_state.shader_instrumentations_metadata[i];

        if (pipeline_state->linking_shaders != 0) {
            PreCallRecordPipelineCreationShaderInstrumentationGPL(pAllocator, *pipeline_state, new_pipeline_ci, create_info_loc,
                                                                  shader_instrumentation_metadata);
        } else {
            PreCallRecordPipelineCreationShaderInstrumentation(pAllocator, *pipeline_state, new_pipeline_ci, create_info_loc,
                                                               shader_instrumentation_metadata);
        }
    }

    chassis_state.pCreateInfos = reinterpret_cast<VkGraphicsPipelineCreateInfo *>(chassis_state.modified_create_infos.data());
}

void GpuShaderInstrumentor::PreCallRecordCreateComputePipelines(VkDevice device, VkPipelineCache pipelineCache, uint32_t count,
                                                                const VkComputePipelineCreateInfo *pCreateInfos,
                                                                const VkAllocationCallbacks *pAllocator, VkPipeline *pPipelines,
                                                                const RecordObject &record_obj, PipelineStates &pipeline_states,
                                                                chassis::CreateComputePipelines &chassis_state) {
    BaseClass::PreCallRecordCreateComputePipelines(device, pipelineCache, count, pCreateInfos, pAllocator, pPipelines, record_obj,
                                                   pipeline_states, chassis_state);
    if (!gpuav_settings.IsSpirvModified()) return;

    chassis_state.shader_instrumentations_metadata.resize(count);
    chassis_state.modified_create_infos.resize(count);

    for (uint32_t i = 0; i < count; ++i) {
        const auto &pipeline_state = pipeline_states[i];

        // Need to make a deep copy so if SPIR-V is inlined, user doesn't see it after the call
        auto &new_pipeline_ci = chassis_state.modified_create_infos[i];
        new_pipeline_ci.initialize(&pipeline_state->ComputeCreateInfo());

        if (!NeedPipelineCreationShaderInstrumentation(*pipeline_state)) {
            continue;
        }

        const Location create_info_loc = record_obj.location.dot(vvl::Field::pCreateInfos, i);
        auto &shader_instrumentation_metadata = chassis_state.shader_instrumentations_metadata[i];

        PreCallRecordPipelineCreationShaderInstrumentation(pAllocator, *pipeline_state, new_pipeline_ci, create_info_loc,
                                                           shader_instrumentation_metadata);
    }

    chassis_state.pCreateInfos = reinterpret_cast<VkComputePipelineCreateInfo *>(chassis_state.modified_create_infos.data());
}

void GpuShaderInstrumentor::PreCallRecordCreateRayTracingPipelinesNV(VkDevice device, VkPipelineCache pipelineCache, uint32_t count,
                                                                     const VkRayTracingPipelineCreateInfoNV *pCreateInfos,
                                                                     const VkAllocationCallbacks *pAllocator,
                                                                     VkPipeline *pPipelines, const RecordObject &record_obj,
                                                                     PipelineStates &pipeline_states,
                                                                     chassis::CreateRayTracingPipelinesNV &chassis_state) {
    BaseClass::PreCallRecordCreateRayTracingPipelinesNV(device, pipelineCache, count, pCreateInfos, pAllocator, pPipelines,
                                                        record_obj, pipeline_states, chassis_state);
    if (!gpuav_settings.IsSpirvModified()) return;

    chassis_state.shader_instrumentations_metadata.resize(count);
    chassis_state.modified_create_infos.resize(count);

    for (uint32_t i = 0; i < count; ++i) {
        const auto &pipeline_state = pipeline_states[i];

        // Need to make a deep copy so if SPIR-V is inlined, user doesn't see it after the call
        auto &new_pipeline_ci = chassis_state.modified_create_infos[i];
        new_pipeline_ci = pipeline_state->RayTracingCreateInfo();  // use copy operation to fight the Common vs NV

        if (!NeedPipelineCreationShaderInstrumentation(*pipeline_state)) {
            continue;
        }

        const Location create_info_loc = record_obj.location.dot(vvl::Field::pCreateInfos, i);
        auto &shader_instrumentation_metadata = chassis_state.shader_instrumentations_metadata[i];

        PreCallRecordPipelineCreationShaderInstrumentation(pAllocator, *pipeline_state, new_pipeline_ci, create_info_loc,
                                                           shader_instrumentation_metadata);
    }

    chassis_state.pCreateInfos = reinterpret_cast<VkRayTracingPipelineCreateInfoNV *>(chassis_state.modified_create_infos.data());
}

void GpuShaderInstrumentor::PreCallRecordCreateRayTracingPipelinesKHR(
    VkDevice device, VkDeferredOperationKHR deferredOperation, VkPipelineCache pipelineCache, uint32_t count,
    const VkRayTracingPipelineCreateInfoKHR *pCreateInfos, const VkAllocationCallbacks *pAllocator, VkPipeline *pPipelines,
    const RecordObject &record_obj, PipelineStates &pipeline_states, chassis::CreateRayTracingPipelinesKHR &chassis_state) {
    BaseClass::PreCallRecordCreateRayTracingPipelinesKHR(device, deferredOperation, pipelineCache, count, pCreateInfos, pAllocator,
                                                         pPipelines, record_obj, pipeline_states, chassis_state);
    if (!gpuav_settings.IsSpirvModified()) return;

    chassis_state.shader_instrumentations_metadata.resize(count);
    chassis_state.modified_create_infos.resize(count);

    for (uint32_t i = 0; i < count; ++i) {
        const auto &pipeline_state = pipeline_states[i];

        // Need to make a deep copy so if SPIR-V is inlined, user doesn't see it after the call
        auto &new_pipeline_ci = chassis_state.modified_create_infos[i];
        new_pipeline_ci.initialize(&pipeline_state->RayTracingCreateInfo());

        if (!NeedPipelineCreationShaderInstrumentation(*pipeline_state)) {
            continue;
        }

        const Location create_info_loc = record_obj.location.dot(vvl::Field::pCreateInfos, i);
        auto &shader_instrumentation_metadata = chassis_state.shader_instrumentations_metadata[i];

        PreCallRecordPipelineCreationShaderInstrumentation(pAllocator, *pipeline_state, new_pipeline_ci, create_info_loc,
                                                           shader_instrumentation_metadata);
    }

    chassis_state.pCreateInfos = reinterpret_cast<VkRayTracingPipelineCreateInfoKHR *>(chassis_state.modified_create_infos.data());
}

template <typename CreateInfos, typename SafeCreateInfos>
static void UtilCopyCreatePipelineFeedbackData(CreateInfos &create_info, SafeCreateInfos &safe_create_info) {
    auto src_feedback_struct = vku::FindStructInPNextChain<VkPipelineCreationFeedbackCreateInfoEXT>(safe_create_info.pNext);
    if (!src_feedback_struct) return;
    auto dst_feedback_struct = const_cast<VkPipelineCreationFeedbackCreateInfoEXT *>(
        vku::FindStructInPNextChain<VkPipelineCreationFeedbackCreateInfoEXT>(create_info.pNext));
    *dst_feedback_struct->pPipelineCreationFeedback = *src_feedback_struct->pPipelineCreationFeedback;
    for (uint32_t j = 0; j < src_feedback_struct->pipelineStageCreationFeedbackCount; j++) {
        dst_feedback_struct->pPipelineStageCreationFeedbacks[j] = src_feedback_struct->pPipelineStageCreationFeedbacks[j];
    }
}

void GpuShaderInstrumentor::PostCallRecordCreateGraphicsPipelines(VkDevice device, VkPipelineCache pipelineCache, uint32_t count,
                                                                  const VkGraphicsPipelineCreateInfo *pCreateInfos,
                                                                  const VkAllocationCallbacks *pAllocator, VkPipeline *pPipelines,
                                                                  const RecordObject &record_obj, PipelineStates &pipeline_states,
                                                                  chassis::CreateGraphicsPipelines &chassis_state) {
    BaseClass::PostCallRecordCreateGraphicsPipelines(device, pipelineCache, count, pCreateInfos, pAllocator, pPipelines, record_obj,
                                                     pipeline_states, chassis_state);
    if (!gpuav_settings.IsSpirvModified()) return;
    for (uint32_t i = 0; i < count; ++i) {
        UtilCopyCreatePipelineFeedbackData(pCreateInfos[i], chassis_state.modified_create_infos[i]);

        auto pipeline_state = Get<vvl::Pipeline>(pPipelines[i]);
        ASSERT_AND_CONTINUE(pipeline_state);

        // Move all instrumentation until the final linking time
        if (pipeline_state->create_flags & VK_PIPELINE_CREATE_LIBRARY_BIT_KHR) continue;

        auto &shader_instrumentation_metadata = chassis_state.shader_instrumentations_metadata[i];
        if (pipeline_state->linking_shaders != 0) {
            PostCallRecordPipelineCreationShaderInstrumentationGPL(*pipeline_state, pAllocator, shader_instrumentation_metadata);
        } else {
            PostCallRecordPipelineCreationShaderInstrumentation(*pipeline_state, shader_instrumentation_metadata);
        }
    }
}

void GpuShaderInstrumentor::PostCallRecordCreateComputePipelines(VkDevice device, VkPipelineCache pipelineCache, uint32_t count,
                                                                 const VkComputePipelineCreateInfo *pCreateInfos,
                                                                 const VkAllocationCallbacks *pAllocator, VkPipeline *pPipelines,
                                                                 const RecordObject &record_obj, PipelineStates &pipeline_states,
                                                                 chassis::CreateComputePipelines &chassis_state) {
    BaseClass::PostCallRecordCreateComputePipelines(device, pipelineCache, count, pCreateInfos, pAllocator, pPipelines, record_obj,
                                                    pipeline_states, chassis_state);
    if (!gpuav_settings.IsSpirvModified()) return;
    for (uint32_t i = 0; i < count; ++i) {
        UtilCopyCreatePipelineFeedbackData(pCreateInfos[i], chassis_state.modified_create_infos[i]);

        auto pipeline_state = Get<vvl::Pipeline>(pPipelines[i]);
        ASSERT_AND_CONTINUE(pipeline_state);
        auto &shader_instrumentation_metadata = chassis_state.shader_instrumentations_metadata[i];
        PostCallRecordPipelineCreationShaderInstrumentation(*pipeline_state, shader_instrumentation_metadata);
    }
}

void GpuShaderInstrumentor::PostCallRecordCreateRayTracingPipelinesNV(
    VkDevice device, VkPipelineCache pipelineCache, uint32_t count, const VkRayTracingPipelineCreateInfoNV *pCreateInfos,
    const VkAllocationCallbacks *pAllocator, VkPipeline *pPipelines, const RecordObject &record_obj,
    PipelineStates &pipeline_states, chassis::CreateRayTracingPipelinesNV &chassis_state) {
    BaseClass::PostCallRecordCreateRayTracingPipelinesNV(device, pipelineCache, count, pCreateInfos, pAllocator, pPipelines,
                                                         record_obj, pipeline_states, chassis_state);
    if (!gpuav_settings.IsSpirvModified()) return;
    for (uint32_t i = 0; i < count; ++i) {
        UtilCopyCreatePipelineFeedbackData(pCreateInfos[i], chassis_state.modified_create_infos[i]);

        auto pipeline_state = Get<vvl::Pipeline>(pPipelines[i]);
        ASSERT_AND_CONTINUE(pipeline_state);
        auto &shader_instrumentation_metadata = chassis_state.shader_instrumentations_metadata[i];
        PostCallRecordPipelineCreationShaderInstrumentation(*pipeline_state, shader_instrumentation_metadata);
    }
}

void GpuShaderInstrumentor::PostCallRecordCreateRayTracingPipelinesKHR(
    VkDevice device, VkDeferredOperationKHR deferredOperation, VkPipelineCache pipelineCache, uint32_t count,
    const VkRayTracingPipelineCreateInfoKHR *pCreateInfos, const VkAllocationCallbacks *pAllocator, VkPipeline *pPipelines,
    const RecordObject &record_obj, PipelineStates &pipeline_states,
    std::shared_ptr<chassis::CreateRayTracingPipelinesKHR> chassis_state) {
    BaseClass::PostCallRecordCreateRayTracingPipelinesKHR(device, deferredOperation, pipelineCache, count, pCreateInfos, pAllocator,
                                                          pPipelines, record_obj, pipeline_states, chassis_state);
    if (!gpuav_settings.IsSpirvModified()) return;

    const bool is_operation_deferred = deferredOperation != VK_NULL_HANDLE && record_obj.result == VK_OPERATION_DEFERRED_KHR;

    auto layer_data = GetLayerDataPtr(GetDispatchKey(device), layer_data_map);
    if (is_operation_deferred) {
        for (uint32_t i = 0; i < count; ++i) {
            UtilCopyCreatePipelineFeedbackData(pCreateInfos[i], chassis_state->modified_create_infos[i]);
        }

        if (wrap_handles) {
            deferredOperation = layer_data->Unwrap(deferredOperation);
        }

        auto found = layer_data->deferred_operation_post_check.pop(deferredOperation);
        std::vector<std::function<void(const std::vector<VkPipeline> &)>> deferred_op_post_checks;
        if (found->first) {
            deferred_op_post_checks = std::move(found->second);
        } else {
            // ValidationStateTracker::PostCallRecordCreateRayTracingPipelinesKHR should have added a lambda in
            // deferred_operation_post_check for the current deferredOperation.
            // This lambda is responsible for initializing the pipeline state we maintain,
            // this state will be accessed in the following lambda.
            // Given how PostCallRecordCreateRayTracingPipelinesKHR is called in
            // GpuShaderInstrumentor::PostCallRecordCreateRayTracingPipelinesKHR
            // conditions holds as of writing. But it is something we need to be aware of.
            assert(false);
            return;
        }

        deferred_op_post_checks.emplace_back(
            [this, held_chassis_state = chassis_state](const std::vector<VkPipeline> &vk_pipelines) mutable {
                for (size_t i = 0; i < vk_pipelines.size(); ++i) {
                    std::shared_ptr<vvl::Pipeline> pipeline_state =
                        ((GpuShaderInstrumentor *)this)->Get<vvl::Pipeline>(vk_pipelines[i]);
                    ASSERT_AND_CONTINUE(pipeline_state);
                    auto &shader_instrumentation_metadata = held_chassis_state->shader_instrumentations_metadata[i];
                    PostCallRecordPipelineCreationShaderInstrumentation(*pipeline_state, shader_instrumentation_metadata);
                }
            });
        layer_data->deferred_operation_post_check.insert(deferredOperation, std::move(deferred_op_post_checks));
    } else {
        for (uint32_t i = 0; i < count; ++i) {
            UtilCopyCreatePipelineFeedbackData(pCreateInfos[i], chassis_state->modified_create_infos[i]);

            auto pipeline_state = Get<vvl::Pipeline>(pPipelines[i]);

            auto &shader_instrumentation_metadata = chassis_state->shader_instrumentations_metadata[i];
            PostCallRecordPipelineCreationShaderInstrumentation(*pipeline_state, shader_instrumentation_metadata);
        }
    }
}

// Remove all the shader trackers associated with this destroyed pipeline.
void GpuShaderInstrumentor::PreCallRecordDestroyPipeline(VkDevice device, VkPipeline pipeline,
                                                         const VkAllocationCallbacks *pAllocator, const RecordObject &record_obj) {
    auto to_erase =
        instrumented_shaders_map_.snapshot([pipeline](const InstrumentedShader &entry) { return entry.pipeline == pipeline; });
    for (const auto &entry : to_erase) {
        instrumented_shaders_map_.erase(entry.first);
    }

    if (auto pipeline_state = Get<vvl::Pipeline>(pipeline)) {
        for (auto shader_module : pipeline_state->instrumentation_data.instrumented_shader_module) {
            DispatchDestroyShaderModule(device, shader_module, pAllocator);
        }
        if (pipeline_state->instrumentation_data.pre_raster_lib != VK_NULL_HANDLE) {
            DispatchDestroyPipeline(device, pipeline_state->instrumentation_data.pre_raster_lib, pAllocator);
        }
        if (pipeline_state->instrumentation_data.frag_out_lib != VK_NULL_HANDLE) {
            DispatchDestroyPipeline(device, pipeline_state->instrumentation_data.frag_out_lib, pAllocator);
        }
    }

    BaseClass::PreCallRecordDestroyPipeline(device, pipeline, pAllocator, record_obj);
}

template <typename CreateInfo>
VkShaderModule GetShaderModule(const CreateInfo &create_info, VkShaderStageFlagBits stage) {
    for (uint32_t i = 0; i < create_info.stageCount; ++i) {
        if (create_info.pStages[i].stage == stage) {
            return create_info.pStages[i].module;
        }
    }
    return {};
}

template <>
VkShaderModule GetShaderModule(const VkComputePipelineCreateInfo &create_info, VkShaderStageFlagBits) {
    return create_info.stage.module;
}

template <typename SafeType>
void SetShaderModule(SafeType &create_info, const vku::safe_VkPipelineShaderStageCreateInfo &stage_info,
                     VkShaderModule shader_module, uint32_t stage_ci_index) {
    create_info.pStages[stage_ci_index] = stage_info;
    create_info.pStages[stage_ci_index].module = shader_module;
}

template <>
void SetShaderModule(vku::safe_VkComputePipelineCreateInfo &create_info,
                     const vku::safe_VkPipelineShaderStageCreateInfo &stage_info, VkShaderModule shader_module,
                     uint32_t stage_ci_index) {
    assert(stage_ci_index == 0);
    create_info.stage = stage_info;
    create_info.stage.module = shader_module;
}

template <typename CreateInfo, typename StageInfo>
StageInfo &GetShaderStageCI(CreateInfo &ci, VkShaderStageFlagBits stage) {
    static StageInfo null_stage{};
    for (uint32_t i = 0; i < ci.stageCount; ++i) {
        if (ci.pStages[i].stage == stage) {
            return ci.pStages[i];
        }
    }
    return null_stage;
}

template <>
vku::safe_VkPipelineShaderStageCreateInfo &GetShaderStageCI(vku::safe_VkComputePipelineCreateInfo &ci, VkShaderStageFlagBits) {
    return ci.stage;
}

bool GpuShaderInstrumentor::IsSelectiveInstrumentationEnabled(const void *pNext) {
    if (auto features = vku::FindStructInPNextChain<VkValidationFeaturesEXT>(pNext)) {
        for (uint32_t i = 0; i < features->enabledValidationFeatureCount; i++) {
            if (features->pEnabledValidationFeatures[i] == VK_VALIDATION_FEATURE_ENABLE_GPU_ASSISTED_EXT) {
                return true;
            }
        }
    }
    return false;
}

bool GpuShaderInstrumentor::NeedPipelineCreationShaderInstrumentation(vvl::Pipeline &pipeline_state) {
    // will hit with using GPL without shaders in them (ex. fragment output)
    if (pipeline_state.stage_states.empty()) return false;

    // Move all instrumentation until the final linking time
    // This still needs to create a copy of the create_info (we *could* have a mix of GPL and non-GPL)
    if (pipeline_state.create_flags & VK_PIPELINE_CREATE_LIBRARY_BIT_KHR) return false;

    // If the app requests all available sets, the pipeline layout was not modified at pipeline layout creation and the
    // already instrumented shaders need to be replaced with uninstrumented shaders
    if (pipeline_state.active_slots.find(instrumentation_desc_set_bind_index_) != pipeline_state.active_slots.end()) {
        return false;
    }
    const auto pipeline_layout = pipeline_state.PipelineLayoutState();
    if (pipeline_layout && pipeline_layout->set_layouts.size() > instrumentation_desc_set_bind_index_) {
        return false;
    }

    return true;
}

bool GpuShaderInstrumentor::HasBindlessDescriptors(vvl::Pipeline &pipeline_state) {
    const auto pipeline_layout = pipeline_state.PipelineLayoutState();
    if (!pipeline_layout) return false;

    for (const auto &set_layout : pipeline_layout->set_layouts) {
        if (set_layout) {
            for (uint32_t i = 0; i < set_layout->GetBindingCount(); i++) {
                const VkDescriptorBindingFlags flags = set_layout->GetDescriptorBindingFlagsFromIndex(i);
                if (vvl::IsBindless(flags)) {
                    return true;
                }
            }
        }
    }
    return false;
}

bool GpuShaderInstrumentor::HasBindlessDescriptors(VkShaderCreateInfoEXT &create_info) {
    for (const auto [layout_i, set_layout] : vvl::enumerate(create_info.pSetLayouts, create_info.setLayoutCount)) {
        if (auto set_layout_state = Get<vvl::DescriptorSetLayout>(*set_layout)) {
            for (uint32_t i = 0; i < set_layout_state->GetBindingCount(); i++) {
                const VkDescriptorBindingFlags flags = set_layout_state->GetDescriptorBindingFlagsFromIndex(i);
                if (vvl::IsBindless(flags)) {
                    return true;
                }
            }
        }
    }
    return false;
}

// Instrument all SPIR-V that is sent through pipeline. This can be done in various ways
// 1. VkCreateShaderModule and passed in VkShaderModule.
//    For this we create our own VkShaderModule with instrumented shader and manage it inside the pipeline state
// 2. GPL
//    We defer until linking time, otherwise we will instrument many libraries that might never be used.
//    (this also spreads the compile time cost evenly instead of a huge spike on startup)
// 3. Inlined via VkPipelineShaderStageCreateInfo pNext
//    We just instrument the shader and update the inlined SPIR-V
// 4. VK_EXT_shader_module_identifier
//    We will skip these as we don't know the incoming SPIR-V
// Note: Shader Objects are handled in their own path as they don't use pipelines
template <typename SafeCreateInfo>
void GpuShaderInstrumentor::PreCallRecordPipelineCreationShaderInstrumentation(
    const VkAllocationCallbacks *pAllocator, vvl::Pipeline &pipeline_state, SafeCreateInfo &new_pipeline_ci, const Location &loc,
    std::vector<chassis::ShaderInstrumentationMetadata> &shader_instrumentation_metadata) {
    // Init here instead of in chassis so we don't pay cost when GPU-AV is not used
    const size_t total_stages = pipeline_state.stage_states.size();
    shader_instrumentation_metadata.resize(total_stages);

    // TODO - measure and see if would be better to make a gpuav subclasses of pipeline layout and store this information once there
    // (not sure how much pipeline layout re-usage there is)
    const bool has_bindless_descriptors = HasBindlessDescriptors(pipeline_state);

    for (uint32_t i = 0; i < static_cast<uint32_t>(pipeline_state.stage_states.size()); ++i) {
        const auto &stage_state = pipeline_state.stage_states[i];
        auto module_state = std::const_pointer_cast<vvl::ShaderModule>(stage_state.module_state);
        ASSERT_AND_CONTINUE(module_state);
        auto &instrumentation_metadata = shader_instrumentation_metadata[i];

        const VkShaderStageFlagBits stage = stage_state.GetStage();

        // Check pNext for inlined SPIR-V
        auto &stage_ci = GetShaderStageCI<SafeCreateInfo, vku::safe_VkPipelineShaderStageCreateInfo>(new_pipeline_ci, stage);
        // We're modifying the copied, safe create info, which is ok to be non-const
        auto sm_ci = const_cast<vku::safe_VkShaderModuleCreateInfo *>(reinterpret_cast<const vku::safe_VkShaderModuleCreateInfo *>(
            vku::FindStructInPNextChain<VkShaderModuleCreateInfo>(stage_ci.pNext)));

        if (gpuav_settings.select_instrumented_shaders) {
            if (sm_ci && !IsSelectiveInstrumentationEnabled(sm_ci->pNext)) {
                continue;
            } else if (selected_instrumented_shaders.find(module_state->VkHandle()) == selected_instrumented_shaders.end()) {
                continue;
            }
        }

        uint32_t unique_shader_id = 0;
        bool cached = false;
        bool pass = false;
        std::vector<uint32_t> instrumented_spirv;
        if (gpuav_settings.cache_instrumented_shaders) {
            unique_shader_id =
                hash_util::ShaderHash(module_state->spirv->words_.data(), module_state->spirv->words_.size() * sizeof(uint32_t));
            if (const auto spirv = instrumented_shaders_cache_.Get(unique_shader_id)) {
                instrumented_spirv = *spirv;
                cached = true;
            }
        } else {
            unique_shader_id = unique_shader_module_id_++;
        }
        if (!cached) {
            pass =
                InstrumentShader(module_state->spirv->words_, unique_shader_id, has_bindless_descriptors, loc, instrumented_spirv);
        }
        if (cached || pass) {
            instrumentation_metadata.unique_shader_id = unique_shader_id;
            if (module_state->VkHandle() != VK_NULL_HANDLE) {
                // If the user used vkCreateShaderModule, we create a new VkShaderModule to replace with the instrumented
                // shader
                VkShaderModule instrumented_shader_module;
                VkShaderModuleCreateInfo create_info = vku::InitStructHelper();
                create_info.pCode = instrumented_spirv.data();
                create_info.codeSize = instrumented_spirv.size() * sizeof(uint32_t);
                VkResult result = DispatchCreateShaderModule(device, &create_info, pAllocator, &instrumented_shader_module);
                if (result == VK_SUCCESS) {
                    SetShaderModule(new_pipeline_ci, *stage_state.pipeline_create_info, instrumented_shader_module, i);
                    pipeline_state.instrumentation_data.instrumented_shader_module.emplace_back(instrumented_shader_module);
                } else {
                    InternalError(device, loc, "Unable to replace non-instrumented shader with instrumented one.");
                }
            } else if (sm_ci) {
                // The user is inlining the Shader Module into the pipeline, so just need to update the spirv
                instrumentation_metadata.passed_in_shader_stage_ci = true;
                // TODO - This makes a copy, but could save on Chassis stack instead (then remove function from VUL).
                // The core issue is we always use std::vector<uint32_t> but Safe Struct manages its own version of the pCode
                // memory. It would be much harder to change everything from std::vector and instead to adjust Safe Struct to not
                // double-free the memory on us. If making any changes, we have to consider a case where the user inlines the
                // fragment shader, but use a normal VkShaderModule in the vertex shader.
                sm_ci->SetCode(instrumented_spirv);
            } else {
                assert(false);
            }

            if (gpuav_settings.cache_instrumented_shaders && !cached) {
                instrumented_shaders_cache_.Add(unique_shader_id, instrumented_spirv);
            }
        }
    }
}

// Now that we have created the pipeline (and have its handle) build up the shader map for each shader we instrumented
void GpuShaderInstrumentor::PostCallRecordPipelineCreationShaderInstrumentation(
    vvl::Pipeline &pipeline_state, std::vector<chassis::ShaderInstrumentationMetadata> &shader_instrumentation_metadata) {
    // if we return early from NeedPipelineCreationShaderInstrumentation, will need to skip at this point in PostCall
    if (shader_instrumentation_metadata.empty()) return;

    for (uint32_t i = 0; i < static_cast<uint32_t>(pipeline_state.stage_states.size()); ++i) {
        auto &instrumentation_metadata = shader_instrumentation_metadata[i];

        // if the shader for some reason was not instrumented, there is nothing to save
        if (!instrumentation_metadata.IsInstrumented()) {
            continue;
        }
        pipeline_state.instrumentation_data.was_instrumented = true;

        const auto &stage_state = pipeline_state.stage_states[i];
        auto &module_state = stage_state.module_state;

        // We currently need to store a copy of the original, non-instrumented shader so if there is debug information,
        // we can reference it by the instruction number printed out in the shader. Since the application can destroy the
        // original VkShaderModule, there is a chance this will be gone, we need to copy it now.
        // TODO - in the instrumentation, instead of printing the instruction number only, if we print out debug info, we
        // can remove this copy
        std::vector<uint32_t> code;
        if (module_state && module_state->spirv) code = module_state->spirv->words_;

        VkShaderModule shader_module_handle = module_state->VkHandle();
        if (shader_module_handle == VK_NULL_HANDLE && instrumentation_metadata.passed_in_shader_stage_ci) {
            shader_module_handle = kPipelineStageInfoHandle;
        }

        instrumented_shaders_map_.insert_or_assign(instrumentation_metadata.unique_shader_id, pipeline_state.VkHandle(),
                                                   shader_module_handle, VK_NULL_HANDLE, std::move(code));
    }
}

// While have an almost duplicated funciton is not ideal, the core issue is we have a single, templated function designed for
// Graphics, Compute, and Ray Tracing. GPL is only for graphics, so we end up needing this "side code path" for graphics only and it
// doesn't fit in the "all pipeline" templated flow.
void GpuShaderInstrumentor::PreCallRecordPipelineCreationShaderInstrumentationGPL(
    const VkAllocationCallbacks *pAllocator, vvl::Pipeline &pipeline_state, vku::safe_VkGraphicsPipelineCreateInfo &new_pipeline_ci,
    const Location &loc, std::vector<chassis::ShaderInstrumentationMetadata> &shader_instrumentation_metadata) {
    // Init here instead of in chassis so we don't pay cost when GPU-AV is not used
    const size_t total_stages = pipeline_state.stage_states.size();
    shader_instrumentation_metadata.resize(total_stages);

    const bool has_bindless_descriptors = HasBindlessDescriptors(pipeline_state);

    auto library_create_info = const_cast<VkPipelineLibraryCreateInfoKHR *>(
        vku::FindStructInPNextChain<VkPipelineLibraryCreateInfoKHR>(new_pipeline_ci.pNext));

    // the "pStages[]" is spread across libraries, so build it up in the double for loop
    uint32_t shader_index = 0;

    // This outer loop is the main difference between the GPL and non-GPL version and why its hard to merge them
    for (uint32_t library_i = 0; library_i < library_create_info->libraryCount; ++library_i) {
        const auto lib = Get<vvl::Pipeline>(library_create_info->pLibraries[library_i]);
        if (!lib) continue;
        if (lib->stage_states.empty()) continue;

        vku::safe_VkGraphicsPipelineCreateInfo new_lib_pipeline_ci(lib->GraphicsCreateInfo());

        for (uint32_t stage_state_i = 0; stage_state_i < static_cast<uint32_t>(lib->stage_states.size()); ++stage_state_i) {
            const auto &stage_state = lib->stage_states[stage_state_i];
            auto module_state = std::const_pointer_cast<vvl::ShaderModule>(stage_state.module_state);
            ASSERT_AND_CONTINUE(module_state);
            auto &instrumentation_metadata = shader_instrumentation_metadata[shader_index++];

            const VkShaderStageFlagBits stage = stage_state.GetStage();

            vku::safe_VkPipelineShaderStageCreateInfo *stage_ci = nullptr;
            // Check pNext for inlined SPIR-V
            for (uint32_t i = 0; i < new_lib_pipeline_ci.stageCount; ++i) {
                if (new_lib_pipeline_ci.pStages[i].stage == stage) {
                    stage_ci = &new_lib_pipeline_ci.pStages[i];
                }
            }

            // We're modifying the copied, safe create info, which is ok to be non-const
            auto sm_ci =
                const_cast<vku::safe_VkShaderModuleCreateInfo *>(reinterpret_cast<const vku::safe_VkShaderModuleCreateInfo *>(
                    vku::FindStructInPNextChain<VkShaderModuleCreateInfo>(stage_ci->pNext)));

            if (gpuav_settings.select_instrumented_shaders) {
                if (sm_ci && !IsSelectiveInstrumentationEnabled(sm_ci->pNext)) {
                    continue;
                } else if (selected_instrumented_shaders.find(module_state->VkHandle()) == selected_instrumented_shaders.end()) {
                    continue;
                }
            }

            uint32_t unique_shader_id = 0;
            bool cached = false;
            bool pass = false;
            std::vector<uint32_t> instrumented_spirv;
            if (gpuav_settings.cache_instrumented_shaders) {
                unique_shader_id = hash_util::ShaderHash(module_state->spirv->words_.data(),
                                                         module_state->spirv->words_.size() * sizeof(uint32_t));
                if (const auto spirv = instrumented_shaders_cache_.Get(unique_shader_id)) {
                    instrumented_spirv = *spirv;
                    cached = true;
                }
            } else {
                unique_shader_id = unique_shader_module_id_++;
            }
            if (!cached) {
                pass = InstrumentShader(module_state->spirv->words_, unique_shader_id, has_bindless_descriptors, loc,
                                        instrumented_spirv);
            }
            if (cached || pass) {
                instrumentation_metadata.unique_shader_id = unique_shader_id;
                if (module_state->VkHandle() != VK_NULL_HANDLE) {
                    // If the user used vkCreateShaderModule, we create a new VkShaderModule to replace with the instrumented
                    // shader
                    VkShaderModule instrumented_shader_module;
                    VkShaderModuleCreateInfo create_info = vku::InitStructHelper();
                    create_info.pCode = instrumented_spirv.data();
                    create_info.codeSize = instrumented_spirv.size() * sizeof(uint32_t);
                    VkResult result = DispatchCreateShaderModule(device, &create_info, pAllocator, &instrumented_shader_module);
                    if (result == VK_SUCCESS) {
                        SetShaderModule(new_lib_pipeline_ci, *stage_state.pipeline_create_info, instrumented_shader_module,
                                        stage_state_i);
                        lib->instrumentation_data.instrumented_shader_module.emplace_back(instrumented_shader_module);
                    } else {
                        InternalError(device, loc, "Unable to replace non-instrumented shader with instrumented one.");
                    }
                } else if (sm_ci) {
                    // The user is inlining the Shader Module into the pipeline, so just need to update the spirv
                    instrumentation_metadata.passed_in_shader_stage_ci = true;
                    // TODO - This makes a copy, but could save on Chassis stack instead (then remove function from VUL).
                    // The core issue is we always use std::vector<uint32_t> but Safe Struct manages its own version of the pCode
                    // memory. It would be much harder to change everything from std::vector and instead to adjust Safe Struct to
                    // not double-free the memory on us. If making any changes, we have to consider a case where the user inlines
                    // the fragment shader, but use a normal VkShaderModule in the vertex shader.
                    sm_ci->SetCode(instrumented_spirv);
                } else {
                    assert(false);
                }

                if (gpuav_settings.cache_instrumented_shaders && !cached) {
                    instrumented_shaders_cache_.Add(unique_shader_id, instrumented_spirv);
                }
            }
        }

        VkPipeline new_lib_pipeline;
        DispatchCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, new_lib_pipeline_ci.ptr(), pAllocator, &new_lib_pipeline);

        if (lib->active_shaders & VK_SHADER_STAGE_FRAGMENT_BIT) {
            pipeline_state.instrumentation_data.frag_out_lib = new_lib_pipeline;
        } else {
            pipeline_state.instrumentation_data.pre_raster_lib = new_lib_pipeline;
        }

        const_cast<VkPipeline *>(library_create_info->pLibraries)[library_i] = new_lib_pipeline;
    }
}

void GpuShaderInstrumentor::PostCallRecordPipelineCreationShaderInstrumentationGPL(
    vvl::Pipeline &pipeline_state, const VkAllocationCallbacks *pAllocator,
    std::vector<chassis::ShaderInstrumentationMetadata> &shader_instrumentation_metadata) {
    // if we return early from NeedPipelineCreationShaderInstrumentation, will need to skip at this point in PostCall
    if (shader_instrumentation_metadata.empty()) return;

    uint32_t shader_index = 0;
    // This outer loop is the main difference between the GPL and non-GPL version and why its hard to merge them
    for (uint32_t library_i = 0; library_i < pipeline_state.library_create_info->libraryCount; ++library_i) {
        const auto lib = Get<vvl::Pipeline>(pipeline_state.library_create_info->pLibraries[library_i]);
        if (!lib) continue;
        if (lib->stage_states.empty()) continue;

        vku::safe_VkGraphicsPipelineCreateInfo new_lib_pipeline_ci(lib->GraphicsCreateInfo());

        for (uint32_t stage_state_i = 0; stage_state_i < static_cast<uint32_t>(lib->stage_states.size()); ++stage_state_i) {
            auto &instrumentation_metadata = shader_instrumentation_metadata[shader_index++];

            // if the shader for some reason was not instrumented, there is nothing to save
            if (!instrumentation_metadata.IsInstrumented()) continue;

            pipeline_state.instrumentation_data.was_instrumented = true;

            const auto &stage_state = lib->stage_states[stage_state_i];
            auto &module_state = stage_state.module_state;

            // We currently need to store a copy of the original, non-instrumented shader so if there is debug information,
            // we can reference it by the instruction number printed out in the shader. Since the application can destroy the
            // original VkShaderModule, there is a chance this will be gone, we need to copy it now.
            // TODO - in the instrumentation, instead of printing the instruction number only, if we print out debug info, we
            // can remove this copy
            std::vector<uint32_t> code;
            if (module_state && module_state->spirv) code = module_state->spirv->words_;

            VkShaderModule shader_module_handle = module_state->VkHandle();
            if (shader_module_handle == VK_NULL_HANDLE && instrumentation_metadata.passed_in_shader_stage_ci) {
                shader_module_handle = kPipelineStageInfoHandle;
            }

            instrumented_shaders_map_.insert_or_assign(instrumentation_metadata.unique_shader_id, lib->VkHandle(),
                                                       shader_module_handle, VK_NULL_HANDLE, std::move(code));
        }
    }
}

static bool GpuValidateShader(const std::vector<uint32_t> &input, bool SetRelaxBlockLayout, bool SetScalarBlockLayout,
                              spv_target_env target_env, std::string &error) {
    // Use SPIRV-Tools validator to try and catch any issues with the module
    spv_context ctx = spvContextCreate(target_env);
    spv_const_binary_t binary{input.data(), input.size()};
    spv_diagnostic diag = nullptr;
    spv_validator_options options = spvValidatorOptionsCreate();
    spvValidatorOptionsSetRelaxBlockLayout(options, SetRelaxBlockLayout);
    spvValidatorOptionsSetScalarBlockLayout(options, SetScalarBlockLayout);
    spv_result_t result = spvValidateWithOptions(ctx, options, &binary, &diag);
    if (result != SPV_SUCCESS && diag) error = diag->error;
    return (result == SPV_SUCCESS);
}

// Call the SPIR-V Optimizer to run the instrumentation pass on the shader.
bool GpuShaderInstrumentor::InstrumentShader(const vvl::span<const uint32_t> &input_spirv, uint32_t unique_shader_id,
                                             bool has_bindless_descriptors, const Location &loc,
                                             std::vector<uint32_t> &out_instrumented_spirv) {
    if (input_spirv[0] != spv::MagicNumber) return false;

    if (gpuav_settings.debug_dump_instrumented_shaders) {
        std::string file_name = "dump_" + std::to_string(unique_shader_id) + "_before.spv";
        std::ofstream debug_file(file_name, std::ios::out | std::ios::binary);
        debug_file.write(reinterpret_cast<const char *>(input_spirv.data()),
                         static_cast<std::streamsize>(input_spirv.size() * sizeof(uint32_t)));
    }

    spirv::Settings module_settings{};
    // Use the unique_shader_id as a shader ID so we can look up its handle later in the shader_map.
    module_settings.shader_id = unique_shader_id;
    module_settings.output_buffer_descriptor_set = instrumentation_desc_set_bind_index_;
    module_settings.print_debug_info = gpuav_settings.debug_print_instrumentation_info;
    module_settings.max_instrumentations_count = gpuav_settings.debug_max_instrumentations_count;
    module_settings.support_non_semantic_info = IsExtEnabled(device_extensions.vk_khr_shader_non_semantic_info);
    module_settings.support_int64 = enabled_features.shaderInt64;
    module_settings.support_memory_model_device_scope = enabled_features.vulkanMemoryModelDeviceScope;
    module_settings.has_bindless_descriptors = has_bindless_descriptors;

    spirv::Module module(input_spirv, debug_report, module_settings);

    bool modified = false;

    // If descriptor indexing is enabled, enable length checks and updated descriptor checks
    if (gpuav_settings.shader_instrumentation.bindless_descriptor) {
        modified |= module.RunPassBindlessDescriptor();
        modified |= module.RunPassNonBindlessOOBBuffer();
        modified |= module.RunPassNonBindlessOOBTexelBuffer();
    }

    if (gpuav_settings.shader_instrumentation.buffer_device_address) {
        modified |= module.RunPassBufferDeviceAddress();
    }

    if (gpuav_settings.shader_instrumentation.ray_query) {
        modified |= module.RunPassRayQuery();
    }

    // Post Process instrumentation passes assume the things inside are valid, but putting at the end, things above will wrap checks
    // in a if/else, this means they will be gaurded as if they were inside the above passes
    if (gpuav_settings.shader_instrumentation.post_process_descriptor_index) {
        modified |= module.RunPassPostProcessDescriptorIndexing();
    }

    // If there were GLSL written function injected, we will grab them and link them in here
    for (const auto &info : module.link_info_) {
        module.LinkFunction(info);
    }

    // DebugPrintf goes at the end for 2 reasons:
    // 1. We use buffer device address in it and we don't want to validate the inside of this pass
    // 2. We might want to debug the above passes and want to inject our own debug printf calls
    if (gpuav_settings.debug_printf_enabled) {
        modified |= module.RunPassDebugPrintf(glsl::kBindingInstDebugPrintf);
    }

    // If nothing was instrumented, leave early to save time
    if (!modified) {
        return false;
    }

    // some small cleanup to make sure SPIR-V is legal
    module.PostProcess();
    // translate internal representation of SPIR-V into legal SPIR-V binary
    module.ToBinary(out_instrumented_spirv);

    if (gpuav_settings.debug_dump_instrumented_shaders) {
        std::string file_name = "dump_" + std::to_string(unique_shader_id) + "_after.spv";
        std::ofstream debug_file(file_name, std::ios::out | std::ios::binary);
        debug_file.write(reinterpret_cast<char *>(out_instrumented_spirv.data()),
                         static_cast<std::streamsize>(out_instrumented_spirv.size() * sizeof(uint32_t)));
    }

    spv_target_env target_env = PickSpirvEnv(api_version, IsExtEnabled(device_extensions.vk_khr_spirv_1_4));
    // (Maybe) validate the instrumented and linked shader
    if (gpuav_settings.debug_validate_instrumented_shaders) {
        std::string instrumented_error;
        if (!GpuValidateShader(out_instrumented_spirv, device_extensions.vk_khr_relaxed_block_layout,
                               device_extensions.vk_ext_scalar_block_layout, target_env, instrumented_error)) {
            std::ostringstream strm;
            strm << "Instrumented shader (id " << unique_shader_id << ") is invalid, spirv-val error:\n"
                 << instrumented_error << " Proceeding with non instrumented shader.";
            InternalError(device, loc, strm.str().c_str());
            return false;
        }
    }

    // Run Dead Code elimination
    // If DebugPrintf is the only thing, there will be nothing to eliminate so don't waste time on it
    if (!gpuav_settings.debug_printf_only) {
        using namespace spvtools;
        OptimizerOptions opt_options;
        opt_options.set_run_validator(false);
        Optimizer dce_pass(target_env);

        const MessageConsumer gpu_console_message_consumer =
            [this, loc](spv_message_level_t level, const char *, const spv_position_t &position, const char *message) -> void {
            switch (level) {
                case SPV_MSG_FATAL:
                case SPV_MSG_INTERNAL_ERROR:
                case SPV_MSG_ERROR:
                    this->LogError("UNASSIGNED-GPU-Assisted", this->device, loc,
                                   "Error during shader instrumentation: line %zu: %s", position.index, message);
                    break;
                default:
                    break;
            }
        };

        dce_pass.SetMessageConsumer(gpu_console_message_consumer);
        // Call CreateAggressiveDCEPass with preserve_interface == true
        dce_pass.RegisterPass(CreateAggressiveDCEPass(true));
        if (!dce_pass.Run(out_instrumented_spirv.data(), out_instrumented_spirv.size(), &out_instrumented_spirv, opt_options)) {
            InternalError(device, loc,
                          "Failure to run spirv-opt DCE on instrumented shader. Proceeding with non-instrumented shader.");
            return false;
        }

        if (gpuav_settings.debug_dump_instrumented_shaders) {
            std::string file_name = "dump_" + std::to_string(unique_shader_id) + "_opt.spv";
            std::ofstream debug_file(file_name, std::ios::out | std::ios::binary);
            debug_file.write(reinterpret_cast<char *>(out_instrumented_spirv.data()),
                             static_cast<std::streamsize>(out_instrumented_spirv.size() * sizeof(uint32_t)));
        }
    }

    return true;
}

void GpuShaderInstrumentor::InternalError(LogObjectList objlist, const Location &loc, const char *const specific_message) const {
    aborted_ = true;
    std::string error_message = specific_message;

    char const *layer_name = gpuav_settings.debug_printf_only ? "DebugPrintf" : "GPU-AV";
    char const *vuid = gpuav_settings.debug_printf_only ? "UNASSIGNED-DEBUG-PRINTF" : "UNASSIGNED-GPU-Assisted-Validation";

    LogError(vuid, objlist, loc, "Internal Error, %s is being disabled. Details:\n%s", layer_name, error_message.c_str());

    // Once we encounter an internal issue disconnect everything.
    // This prevents need to check "if (aborted)" (which is awful when we easily forget to check somewhere and the user gets spammed
    // with errors making it hard to see the first error with the real source of the problem).
    ReleaseDeviceDispatchObject(LayerObjectTypeGpuAssisted);
}

void GpuShaderInstrumentor::InternalWarning(LogObjectList objlist, const Location &loc, const char *const specific_message) const {
    char const *vuid = gpuav_settings.debug_printf_only ? "WARNING-DEBUG-PRINTF" : "WARNING-GPU-Assisted-Validation";
    LogWarning(vuid, objlist, loc, "Internal Warning: %s", specific_message);
}

// The lock (debug_output_mutex) is held by the caller,
// because the latter has code paths that make multiple calls of this function,
// and all such calls have to access the same debug reporting state to ensure consistency of output information.
static std::string LookupDebugUtilsNameNoLock(const DebugReport *debug_report, const uint64_t object) {
    auto object_label = debug_report->GetUtilsObjectNameNoLock(object);
    if (object_label != "") {
        object_label = "(" + object_label + ")";
    }
    return object_label;
}

// Generate the stage-specific part of the message.
static void GenerateStageMessage(std::ostringstream &ss, uint32_t stage_id, uint32_t stage_info_0, uint32_t stage_info_1,
                                 uint32_t stage_info_2, const std::vector<Instruction> &instructions) {
    switch (stage_id) {
        case glsl::kHeaderStageIdMultiEntryPoint: {
            ss << "Stage has multiple OpEntryPoint (";
            bool first_stage = true;
            for (const auto &insn : instructions) {
                if (insn.Opcode() == spv::OpFunction) break;  // early exit when possible
                if (insn.Opcode() == spv::OpEntryPoint) {
                    if (first_stage) {
                        first_stage = false;
                    } else {
                        ss << ", ";
                    }
                    ss << string_SpvExecutionModel(insn.Word(1));
                }
            }
            ss << ") and could not detect stage. ";
        } break;
        case spv::ExecutionModelVertex: {
            ss << "Stage = Vertex. Vertex Index = " << stage_info_0 << " Instance Index = " << stage_info_1 << ". ";
        } break;
        case spv::ExecutionModelTessellationControl: {
            ss << "Stage = Tessellation Control.  Invocation ID = " << stage_info_0 << ", Primitive ID = " << stage_info_1;
        } break;
        case spv::ExecutionModelTessellationEvaluation: {
            ss << "Stage = Tessellation Eval.  Primitive ID = " << stage_info_0 << ", TessCoord (u, v) = (" << stage_info_1 << ", "
               << stage_info_2 << "). ";
        } break;
        case spv::ExecutionModelGeometry: {
            ss << "Stage = Geometry.  Primitive ID = " << stage_info_0 << " Invocation ID = " << stage_info_1 << ". ";
        } break;
        case spv::ExecutionModelFragment: {
            // Should use std::bit_cast but requires c++20
            float x_coord;
            float y_coord;
            std::memcpy(&x_coord, &stage_info_0, sizeof(float));
            std::memcpy(&y_coord, &stage_info_1, sizeof(float));
            ss << "Stage = Fragment.  Fragment coord (x,y) = (" << x_coord << ", " << y_coord << "). ";
        } break;
        case spv::ExecutionModelGLCompute: {
            ss << "Stage = Compute.  Global invocation ID (x, y, z) = (" << stage_info_0 << ", " << stage_info_1 << ", "
               << stage_info_2 << ")";
        } break;
        case spv::ExecutionModelRayGenerationKHR: {
            ss << "Stage = Ray Generation.  Global Launch ID (x,y,z) = (" << stage_info_0 << ", " << stage_info_1 << ", "
               << stage_info_2 << "). ";
        } break;
        case spv::ExecutionModelIntersectionKHR: {
            ss << "Stage = Intersection.  Global Launch ID (x,y,z) = (" << stage_info_0 << ", " << stage_info_1 << ", "
               << stage_info_2 << "). ";
        } break;
        case spv::ExecutionModelAnyHitKHR: {
            ss << "Stage = Any Hit.  Global Launch ID (x,y,z) = (" << stage_info_0 << ", " << stage_info_1 << ", " << stage_info_2
               << "). ";
        } break;
        case spv::ExecutionModelClosestHitKHR: {
            ss << "Stage = Closest Hit.  Global Launch ID (x,y,z) = (" << stage_info_0 << ", " << stage_info_1 << ", "
               << stage_info_2 << "). ";
        } break;
        case spv::ExecutionModelMissKHR: {
            ss << "Stage = Miss.  Global Launch ID (x,y,z) = (" << stage_info_0 << ", " << stage_info_1 << ", " << stage_info_2
               << "). ";
        } break;
        case spv::ExecutionModelCallableKHR: {
            ss << "Stage = Callable.  Global Launch ID (x,y,z) = (" << stage_info_0 << ", " << stage_info_1 << ", " << stage_info_2
               << "). ";
        } break;
        case spv::ExecutionModelTaskEXT: {
            ss << "Stage = TaskEXT. Global invocation ID (x, y, z) = (" << stage_info_0 << ", " << stage_info_1 << ", "
               << stage_info_2 << " )";
        } break;
        case spv::ExecutionModelMeshEXT: {
            ss << "Stage = MeshEXT. Global invocation ID (x, y, z) = (" << stage_info_0 << ", " << stage_info_1 << ", "
               << stage_info_2 << " )";
        } break;
        case spv::ExecutionModelTaskNV: {
            ss << "Stage = TaskNV. Global invocation ID (x, y, z) = (" << stage_info_0 << ", " << stage_info_1 << ", "
               << stage_info_2 << " )";
        } break;
        case spv::ExecutionModelMeshNV: {
            ss << "Stage = MeshNV. Global invocation ID (x, y, z) = (" << stage_info_0 << ", " << stage_info_1 << ", "
               << stage_info_2 << " )";
        } break;
        default: {
            ss << "Internal Error (unexpected stage = " << stage_id << "). ";
            assert(false);
        } break;
    }
    ss << '\n';
}

// There are 2 ways to inject source into a shader:
// 1. The "old" way using OpLine/OpSource
// 2. The "new" way using NonSemantic Shader DebugInfo
static std::string FindShaderSource(std::ostringstream &ss, const std::vector<Instruction> &instructions,
                                    uint32_t instruction_position, bool debug_printf_only) {
    ss << "SPIR-V Instruction Index = " << instruction_position << '\n';

    // Find the OpLine/DebugLine just before the failing instruction indicated by the debug info.
    // SPIR-V can only be iterated in the forward direction due to its opcode/length encoding.
    uint32_t index = 0;
    uint32_t shader_debug_info_set_id = 0;
    const Instruction *last_line_inst = nullptr;
    for (const auto &insn : instructions) {
        const uint32_t opcode = insn.Opcode();
        if (opcode == spv::OpExtInstImport) {
            if (strcmp(insn.GetAsString(2), "NonSemantic.Shader.DebugInfo.100") == 0) {
                shader_debug_info_set_id = insn.ResultId();
            }
        }

        if (opcode == spv::OpExtInst && insn.Word(3) == shader_debug_info_set_id &&
            insn.Word(4) == NonSemanticShaderDebugInfo100DebugLine) {
            last_line_inst = &insn;
        } else if (opcode == spv::OpLine) {
            last_line_inst = &insn;
        } else if (opcode == spv::OpFunctionEnd) {
            last_line_inst = nullptr;  // debug lines can't cross functions boundaries
        }

        if (index == instruction_position) {
            break;
        }
        index++;
    }

    if (last_line_inst) {
        ss << (debug_printf_only ? "Debug shader printf message generated " : "Shader validation error occurred ");
        GetShaderSourceInfo(ss, instructions, *last_line_inst);
    } else {
        ss << "Unable to source. Build shader with debug info to get source information.\n";
    }

    return ss.str();
}

// Where we build up the error message with all the useful debug information about where the error occured
std::string GpuShaderInstrumentor::GenerateDebugInfoMessage(
    VkCommandBuffer commandBuffer, const std::vector<Instruction> &instructions, uint32_t stage_id, uint32_t stage_info_0,
    uint32_t stage_info_1, uint32_t stage_info_2, uint32_t instruction_position, const InstrumentedShader *instrumented_shader,
    uint32_t shader_id, VkPipelineBindPoint pipeline_bind_point, uint32_t operation_index) const {
    std::ostringstream ss;
    if (instructions.empty() || !instrumented_shader) {
        ss << "[Internal Error] - Can't get instructions from shader_map\n";
        return ss.str();
    }

    GenerateStageMessage(ss, stage_id, stage_info_0, stage_info_1, stage_info_2, instructions);

    ss << std::hex << std::showbase;
    if (instrumented_shader->shader_module == VK_NULL_HANDLE && instrumented_shader->shader_object == VK_NULL_HANDLE) {
        std::unique_lock<std::mutex> lock(debug_report->debug_output_mutex);
        ss << "[Internal Error] - Unable to locate shader/pipeline handles used in command buffer "
           << LookupDebugUtilsNameNoLock(debug_report, HandleToUint64(commandBuffer)) << "(" << HandleToUint64(commandBuffer)
           << ")\n";
        assert(true);
    } else {
        std::unique_lock<std::mutex> lock(debug_report->debug_output_mutex);
        ss << "Command buffer " << LookupDebugUtilsNameNoLock(debug_report, HandleToUint64(commandBuffer)) << "("
           << HandleToUint64(commandBuffer) << ")\n";

        ss << std::dec << std::noshowbase;
        ss << '\t';  // helps to show that the index is expressed with respect to the command buffer
        if (pipeline_bind_point == VK_PIPELINE_BIND_POINT_GRAPHICS) {
            ss << "Draw ";
        } else if (pipeline_bind_point == VK_PIPELINE_BIND_POINT_COMPUTE) {
            ss << "Compute Dispatch ";
        } else if (pipeline_bind_point == VK_PIPELINE_BIND_POINT_RAY_TRACING_KHR) {
            ss << "Ray Trace ";
        } else {
            assert(false);
            ss << "Unknown Pipeline Operation ";
        }
        ss << "Index " << operation_index << '\n';
        ss << std::hex << std::noshowbase;

        if (instrumented_shader->shader_module == VK_NULL_HANDLE) {
            ss << "Shader Object " << LookupDebugUtilsNameNoLock(debug_report, HandleToUint64(instrumented_shader->shader_object))
               << "(" << HandleToUint64(instrumented_shader->shader_object) << ") (internal ID " << shader_id << ")\n";
        } else {
            ss << "Pipeline " << LookupDebugUtilsNameNoLock(debug_report, HandleToUint64(instrumented_shader->pipeline)) << "("
               << HandleToUint64(instrumented_shader->pipeline) << ")";
            if (instrumented_shader->shader_module == kPipelineStageInfoHandle) {
                ss << " (internal ID " << shader_id
                   << ")\nShader Module was passed in via VkPipelineShaderStageCreateInfo::pNext\n";
            } else {
                ss << "\nShader Module "
                   << LookupDebugUtilsNameNoLock(debug_report, HandleToUint64(instrumented_shader->shader_module)) << "("
                   << HandleToUint64(instrumented_shader->shader_module) << ") (internal ID " << shader_id << ")\n";
            }
        }
    }
    ss << std::dec << std::noshowbase;

    FindShaderSource(ss, instructions, instruction_position, gpuav_settings.debug_printf_only);

    return ss.str();
}

}  // namespace gpuav
