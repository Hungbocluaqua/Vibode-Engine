#include "rtv/ShaderReflection.h"

#include <spirv-reflect/spirv_reflect.h>

#include <algorithm>
#include <map>
#include <stdexcept>

namespace rtv {

namespace {

VkShaderStageFlagBits toVkStage(SpvReflectShaderStageFlagBits stage) {
    switch (stage) {
    case SPV_REFLECT_SHADER_STAGE_VERTEX_BIT:
        return VK_SHADER_STAGE_VERTEX_BIT;
    case SPV_REFLECT_SHADER_STAGE_FRAGMENT_BIT:
        return VK_SHADER_STAGE_FRAGMENT_BIT;
    case SPV_REFLECT_SHADER_STAGE_COMPUTE_BIT:
        return VK_SHADER_STAGE_COMPUTE_BIT;
    case SPV_REFLECT_SHADER_STAGE_RAYGEN_BIT_KHR:
        return VK_SHADER_STAGE_RAYGEN_BIT_KHR;
    case SPV_REFLECT_SHADER_STAGE_MISS_BIT_KHR:
        return VK_SHADER_STAGE_MISS_BIT_KHR;
    case SPV_REFLECT_SHADER_STAGE_CLOSEST_HIT_BIT_KHR:
        return VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR;
    case SPV_REFLECT_SHADER_STAGE_ANY_HIT_BIT_KHR:
        return VK_SHADER_STAGE_ANY_HIT_BIT_KHR;
    default:
        throw std::runtime_error("Unsupported reflected shader stage");
    }
}

VkDescriptorType toVkDescriptorType(SpvReflectDescriptorType type) {
    switch (type) {
    case SPV_REFLECT_DESCRIPTOR_TYPE_SAMPLER:
        return VK_DESCRIPTOR_TYPE_SAMPLER;
    case SPV_REFLECT_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER:
        return VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    case SPV_REFLECT_DESCRIPTOR_TYPE_SAMPLED_IMAGE:
        return VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
    case SPV_REFLECT_DESCRIPTOR_TYPE_STORAGE_IMAGE:
        return VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    case SPV_REFLECT_DESCRIPTOR_TYPE_UNIFORM_BUFFER:
        return VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    case SPV_REFLECT_DESCRIPTOR_TYPE_STORAGE_BUFFER:
        return VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    case SPV_REFLECT_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR:
        return VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR;
    default:
        throw std::runtime_error("Unsupported reflected descriptor type");
    }
}

} // namespace

ShaderReflectionData ShaderReflection::reflect(const std::vector<uint32_t>& spirv) {
    SpvReflectShaderModule module{};
    SpvReflectResult result = spvReflectCreateShaderModule(spirv.size() * sizeof(uint32_t), spirv.data(), &module);
    if (result != SPV_REFLECT_RESULT_SUCCESS) {
        throw std::runtime_error("spvReflectCreateShaderModule failed");
    }

    ShaderReflectionData data;
    data.stage = toVkStage(module.shader_stage);

    uint32_t bindingCount = 0;
    result = spvReflectEnumerateDescriptorBindings(&module, &bindingCount, nullptr);
    if (result != SPV_REFLECT_RESULT_SUCCESS) {
        spvReflectDestroyShaderModule(&module);
        throw std::runtime_error("spvReflectEnumerateDescriptorBindings(count) failed");
    }

    std::vector<SpvReflectDescriptorBinding*> reflectedBindings(bindingCount);
    result = spvReflectEnumerateDescriptorBindings(&module, &bindingCount, reflectedBindings.data());
    if (result != SPV_REFLECT_RESULT_SUCCESS) {
        spvReflectDestroyShaderModule(&module);
        throw std::runtime_error("spvReflectEnumerateDescriptorBindings failed");
    }

    for (const SpvReflectDescriptorBinding* binding : reflectedBindings) {
        uint32_t count = 1;
        for (uint32_t i = 0; i < binding->array.dims_count; ++i) {
            count *= binding->array.dims[i];
        }
        data.bindings.push_back({
            .set = binding->set,
            .binding = binding->binding,
            .count = count,
            .type = toVkDescriptorType(binding->descriptor_type),
            .stages = static_cast<VkShaderStageFlags>(data.stage),
        });
    }

    uint32_t pushCount = 0;
    result = spvReflectEnumeratePushConstantBlocks(&module, &pushCount, nullptr);
    if (result != SPV_REFLECT_RESULT_SUCCESS) {
        spvReflectDestroyShaderModule(&module);
        throw std::runtime_error("spvReflectEnumeratePushConstantBlocks(count) failed");
    }

    std::vector<SpvReflectBlockVariable*> reflectedPush(pushCount);
    result = spvReflectEnumeratePushConstantBlocks(&module, &pushCount, reflectedPush.data());
    if (result != SPV_REFLECT_RESULT_SUCCESS) {
        spvReflectDestroyShaderModule(&module);
        throw std::runtime_error("spvReflectEnumeratePushConstantBlocks failed");
    }

    for (const SpvReflectBlockVariable* push : reflectedPush) {
        data.pushConstants.push_back({
            .offset = push->offset,
            .size = push->size,
            .stages = static_cast<VkShaderStageFlags>(data.stage),
        });
    }

    spvReflectDestroyShaderModule(&module);
    return data;
}

std::vector<VkDescriptorSetLayoutBinding> ShaderReflection::bindingsForSet(
    const std::vector<ShaderReflectionData>& modules,
    uint32_t set) {
    struct BindingKey {
        VkDescriptorType type = VK_DESCRIPTOR_TYPE_MAX_ENUM;
        uint32_t count = 0;
        VkShaderStageFlags stages = 0;
    };

    std::map<uint32_t, BindingKey> merged;
    for (const ShaderReflectionData& module : modules) {
        for (const ReflectedBinding& binding : module.bindings) {
            if (binding.set != set) {
                continue;
            }

            BindingKey& entry = merged[binding.binding];
            if (entry.count == 0) {
                entry.type = binding.type;
                entry.count = binding.count;
            } else if (entry.type != binding.type || entry.count != binding.count) {
                throw std::runtime_error("Incompatible descriptor binding reflected across shader stages");
            }
            entry.stages |= binding.stages;
        }
    }

    std::vector<VkDescriptorSetLayoutBinding> result;
    result.reserve(merged.size());
    for (const auto& [binding, info] : merged) {
        result.push_back({
            .binding = binding,
            .descriptorType = info.type,
            .descriptorCount = info.count,
            .stageFlags = info.stages,
        });
    }
    return result;
}

std::vector<VkPushConstantRange> ShaderReflection::mergePushConstants(const std::vector<ShaderReflectionData>& modules) {
    std::vector<VkPushConstantRange> ranges;
    for (const ShaderReflectionData& module : modules) {
        for (const ReflectedPushConstant& push : module.pushConstants) {
            ranges.push_back({
                .stageFlags = push.stages,
                .offset = push.offset,
                .size = push.size,
            });
        }
    }
    return ranges;
}

} // namespace rtv
