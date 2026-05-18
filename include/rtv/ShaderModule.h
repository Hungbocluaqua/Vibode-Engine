#pragma once

#include "rtv/NonCopyable.h"
#include "rtv/ShaderReflection.h"

#include <Volk/volk.h>

#include <filesystem>
#include <vector>

namespace rtv {

class ShaderModule final : private NonCopyable {
public:
    ShaderModule(VkDevice device, std::vector<uint32_t> spirv, const char* debugName = nullptr);
    ~ShaderModule();

    ShaderModule(ShaderModule&& other) noexcept;
    ShaderModule& operator=(ShaderModule&& other) noexcept;

    [[nodiscard]] VkShaderModule handle() const { return module_; }
    [[nodiscard]] VkShaderStageFlagBits stage() const { return reflection_.stage; }
    [[nodiscard]] const ShaderReflectionData& reflection() const { return reflection_; }
    [[nodiscard]] VkPipelineShaderStageCreateInfo stageInfo(const char* entryPoint = "main") const;

private:
    VkDevice device_ = VK_NULL_HANDLE;
    VkShaderModule module_ = VK_NULL_HANDLE;
    ShaderReflectionData reflection_{};
};

} // namespace rtv
