#include "rtv/PipelineCache.h"

#include "rtv/Check.h"

#include <cstdint>
#include <cstdio>
#include <fstream>
#include <iostream>

namespace rtv {

PipelineCache::PipelineCache(VkDevice device)
    : device_(device) {
    VkPipelineCacheCreateInfo createInfo{};
    createInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO;
    checkVk(vkCreatePipelineCache(device_, &createInfo, nullptr, &cache_), "vkCreatePipelineCache");
}

PipelineCache::PipelineCache(VkDevice device, const std::filesystem::path& cachePath)
    : device_(device) {
    VkPipelineCacheCreateInfo createInfo{};
    createInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO;
    const std::vector<uint8_t> initialData = readCacheFile(cachePath);
    if (!initialData.empty()) {
        createInfo.initialDataSize = initialData.size();
        createInfo.pInitialData = initialData.data();
        std::cout << "Loaded pipeline cache from " << cachePath.string()
                  << " (" << initialData.size() << " bytes)\n";
    }
    checkVk(vkCreatePipelineCache(device_, &createInfo, nullptr, &cache_), "vkCreatePipelineCache");
}

PipelineCache::~PipelineCache() {
    if (cache_ != VK_NULL_HANDLE) {
        vkDestroyPipelineCache(device_, cache_, nullptr);
    }
}

bool PipelineCache::saveToFile(const std::filesystem::path& path) const {
    if (cache_ == VK_NULL_HANDLE) {
        return false;
    }

    size_t dataSize = 0;
    VkResult result = vkGetPipelineCacheData(device_, cache_, &dataSize, nullptr);
    if (result != VK_SUCCESS || dataSize == 0) {
        return false;
    }

    std::vector<uint8_t> data(dataSize);
    result = vkGetPipelineCacheData(device_, cache_, &dataSize, data.data());
    if (result != VK_SUCCESS) {
        return false;
    }

    std::ofstream file(path, std::ios::binary);
    if (!file) {
        std::cerr << "Failed to write pipeline cache to " << path.string() << '\n';
        return false;
    }
    file.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()));
    if (!file) {
        std::cerr << "Failed to write pipeline cache data to " << path.string() << '\n';
        return false;
    }

    std::cout << "Saved pipeline cache to " << path.string()
              << " (" << dataSize << " bytes)\n";
    return true;
}

std::vector<uint8_t> PipelineCache::loadFromFile(const std::filesystem::path& path) {
    return readCacheFile(path);
}

std::vector<uint8_t> PipelineCache::readCacheFile(const std::filesystem::path& path) {
    std::vector<uint8_t> data;
    if (!std::filesystem::exists(path)) {
        return data;
    }
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file) {
        return data;
    }
    const size_t size = static_cast<size_t>(file.tellg());
    if (size == 0) {
        return data;
    }
    file.seekg(0);
    data.resize(size);
    file.read(reinterpret_cast<char*>(data.data()), static_cast<std::streamsize>(size));
    if (!file) {
        data.clear();
    }
    return data;
}

} // namespace rtv
