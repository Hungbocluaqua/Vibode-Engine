#include "rtv/ShaderReflection.h"

#include <spirv-reflect/spirv_reflect.h>

#include <algorithm>
#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <stdexcept>
#include <string>

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

uint64_t hashSpirv(const std::vector<uint32_t>& spirv) {
    uint64_t hash = 1469598103934665603ull;
    const auto* bytes = reinterpret_cast<const uint8_t*>(spirv.data());
    const size_t byteCount = spirv.size() * sizeof(uint32_t);
    for (size_t i = 0; i < byteCount; ++i) {
        hash ^= static_cast<uint64_t>(bytes[i]);
        hash *= 1099511628211ull;
    }
    return hash;
}

std::string environmentValue(const char* name) {
#if defined(_MSC_VER)
    char* value = nullptr;
    size_t length = 0;
    _dupenv_s(&value, &length, name);
    if (value == nullptr) {
        return {};
    }
    std::string result(value);
    std::free(value);
    return result;
#else
    const char* value = std::getenv(name);
    return value != nullptr ? std::string(value) : std::string{};
#endif
}

std::filesystem::path reflectionCacheDirectory() {
    const std::string configured = environmentValue("RTV_SHADER_REFLECTION_CACHE_DIR");
    if (!configured.empty()) {
        return configured;
    }
    return std::filesystem::temp_directory_path() / "rtvulkan_shader_reflection_v1";
}

std::filesystem::path reflectionCachePath(uint64_t spirvHash) {
    char name[64]{};
    std::snprintf(name, sizeof(name), "reflection_%016llx.bin", static_cast<unsigned long long>(spirvHash));
    return reflectionCacheDirectory() / name;
}

template <typename T>
bool readBinary(std::ifstream& file, T& value) {
    file.read(reinterpret_cast<char*>(&value), sizeof(T));
    return static_cast<bool>(file);
}

template <typename T>
void writeBinary(std::ofstream& file, const T& value) {
    file.write(reinterpret_cast<const char*>(&value), sizeof(T));
}

bool tryLoadReflectionCache(uint64_t spirvHash, ShaderReflectionData& data) {
    std::ifstream file(reflectionCachePath(spirvHash), std::ios::binary);
    if (!file) {
        return false;
    }

    uint32_t magic = 0;
    uint32_t version = 0;
    uint32_t stage = 0;
    uint32_t bindingCount = 0;
    uint32_t pushCount = 0;
    if (!readBinary(file, magic) ||
        !readBinary(file, version) ||
        !readBinary(file, stage) ||
        !readBinary(file, bindingCount) ||
        !readBinary(file, pushCount) ||
        magic != 0x52534631u ||
        version != 1u ||
        bindingCount > 4096u ||
        pushCount > 256u) {
        return false;
    }

    ShaderReflectionData loaded{};
    loaded.stage = static_cast<VkShaderStageFlagBits>(stage);
    loaded.bindings.resize(bindingCount);
    loaded.pushConstants.resize(pushCount);
    for (ReflectedBinding& binding : loaded.bindings) {
        uint32_t type = 0;
        if (!readBinary(file, binding.set) ||
            !readBinary(file, binding.binding) ||
            !readBinary(file, binding.count) ||
            !readBinary(file, type) ||
            !readBinary(file, binding.stages)) {
            return false;
        }
        binding.type = static_cast<VkDescriptorType>(type);
    }
    for (ReflectedPushConstant& push : loaded.pushConstants) {
        if (!readBinary(file, push.offset) ||
            !readBinary(file, push.size) ||
            !readBinary(file, push.stages)) {
            return false;
        }
    }
    data = std::move(loaded);
    return true;
}

void storeReflectionCache(uint64_t spirvHash, const ShaderReflectionData& data) {
    try {
        const std::filesystem::path directory = reflectionCacheDirectory();
        std::filesystem::create_directories(directory);
        const std::filesystem::path path = reflectionCachePath(spirvHash);
        const std::filesystem::path tmp = path.string() + ".tmp";
        std::ofstream file(tmp, std::ios::binary | std::ios::trunc);
        if (!file) {
            return;
        }
        const uint32_t magic = 0x52534631u;
        const uint32_t version = 1u;
        const uint32_t stage = static_cast<uint32_t>(data.stage);
        const uint32_t bindingCount = static_cast<uint32_t>(data.bindings.size());
        const uint32_t pushCount = static_cast<uint32_t>(data.pushConstants.size());
        writeBinary(file, magic);
        writeBinary(file, version);
        writeBinary(file, stage);
        writeBinary(file, bindingCount);
        writeBinary(file, pushCount);
        for (const ReflectedBinding& binding : data.bindings) {
            const uint32_t type = static_cast<uint32_t>(binding.type);
            writeBinary(file, binding.set);
            writeBinary(file, binding.binding);
            writeBinary(file, binding.count);
            writeBinary(file, type);
            writeBinary(file, binding.stages);
        }
        for (const ReflectedPushConstant& push : data.pushConstants) {
            writeBinary(file, push.offset);
            writeBinary(file, push.size);
            writeBinary(file, push.stages);
        }
        file.close();
        if (!file) {
            std::filesystem::remove(tmp);
            return;
        }
        std::filesystem::rename(tmp, path);
    } catch (const std::exception&) {
    }
}

} // namespace

ShaderReflectionData ShaderReflection::reflect(const std::vector<uint32_t>& spirv) {
    const uint64_t spirvHash = hashSpirv(spirv);
    ShaderReflectionData cached;
    if (tryLoadReflectionCache(spirvHash, cached)) {
        return cached;
    }

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
    storeReflectionCache(spirvHash, data);
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
