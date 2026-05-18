#include "rtv/ShaderModule.h"

#include "rtv/Check.h"

#include <utility>

namespace rtv {

ShaderModule::ShaderModule(VkDevice device, std::vector<uint32_t> spirv, const char*)
    : device_(device), reflection_(ShaderReflection::reflect(spirv)) {
    VkShaderModuleCreateInfo createInfo{};
    createInfo.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    createInfo.codeSize = spirv.size() * sizeof(uint32_t);
    createInfo.pCode = spirv.data();
    checkVk(vkCreateShaderModule(device_, &createInfo, nullptr, &module_), "vkCreateShaderModule");
}

ShaderModule::~ShaderModule() {
    if (module_ != VK_NULL_HANDLE) {
        vkDestroyShaderModule(device_, module_, nullptr);
    }
}

ShaderModule::ShaderModule(ShaderModule&& other) noexcept {
    *this = std::move(other);
}

ShaderModule& ShaderModule::operator=(ShaderModule&& other) noexcept {
    if (this != &other) {
        if (module_ != VK_NULL_HANDLE) {
            vkDestroyShaderModule(device_, module_, nullptr);
        }
        device_ = other.device_;
        module_ = other.module_;
        reflection_ = other.reflection_;
        other.device_ = VK_NULL_HANDLE;
        other.module_ = VK_NULL_HANDLE;
        other.reflection_ = {};
    }
    return *this;
}

VkPipelineShaderStageCreateInfo ShaderModule::stageInfo(const char* entryPoint) const {
    VkPipelineShaderStageCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    info.stage = stage();
    info.module = module_;
    info.pName = entryPoint;
    return info;
}

} // namespace rtv
