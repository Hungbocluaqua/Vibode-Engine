#pragma once

#include <Volk/volk.h>

#include <filesystem>
#include <vector>

namespace rtv {

struct ReflectedBinding {
    uint32_t set = 0;
    uint32_t binding = 0;
    uint32_t count = 1;
    VkDescriptorType type = VK_DESCRIPTOR_TYPE_MAX_ENUM;
    VkShaderStageFlags stages = 0;
};

struct ReflectedPushConstant {
    uint32_t offset = 0;
    uint32_t size = 0;
    VkShaderStageFlags stages = 0;
};

struct ShaderReflectionData {
    VkShaderStageFlagBits stage = VK_SHADER_STAGE_ALL;
    std::vector<ReflectedBinding> bindings;
    std::vector<ReflectedPushConstant> pushConstants;
};

class ShaderReflection {
public:
    [[nodiscard]] static ShaderReflectionData reflect(const std::vector<uint32_t>& spirv);
    [[nodiscard]] static std::vector<VkDescriptorSetLayoutBinding> bindingsForSet(
        const std::vector<ShaderReflectionData>& modules,
        uint32_t set);
    [[nodiscard]] static std::vector<VkPushConstantRange> mergePushConstants(const std::vector<ShaderReflectionData>& modules);
};

} // namespace rtv
