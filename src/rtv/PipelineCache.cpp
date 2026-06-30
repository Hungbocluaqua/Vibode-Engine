#include "rtv/PipelineCache.h"

#include "rtv/Check.h"

#include <algorithm>
#include <cstdlib>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>

namespace rtv {

namespace {

struct CachedPipelineCacheFile {
    std::filesystem::file_time_type writeTime{};
    uintmax_t fileSize = 0;
    std::shared_ptr<const std::vector<uint8_t>> data;
};

std::mutex& pipelineCacheFileMutex() {
    static std::mutex mutex;
    return mutex;
}

std::unordered_map<std::string, CachedPipelineCacheFile>& pipelineCacheFiles() {
    static std::unordered_map<std::string, CachedPipelineCacheFile> files;
    return files;
}

std::string cacheFileKey(const std::filesystem::path& path) {
    return std::filesystem::absolute(path).lexically_normal().string();
}

std::shared_ptr<const std::vector<uint8_t>> emptyPipelineCacheData() {
    static const auto empty = std::make_shared<const std::vector<uint8_t>>();
    return empty;
}

std::string environmentValue(const char* name) {
#ifdef _WIN32
    char* value = nullptr;
    size_t valueSize = 0;
    if (_dupenv_s(&value, &valueSize, name) != 0 || value == nullptr) {
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

uint64_t pipelineCacheMaxPersistentBytes() {
    const std::string configured = environmentValue("RTV_PIPELINE_CACHE_MAX_MB");
    if (!configured.empty()) {
        try {
            const uint64_t mib = std::stoull(configured);
            if (mib == 0) {
                return 0;
            }
            return mib * 1024ull * 1024ull;
        } catch (const std::exception&) {
            std::cerr << "Ignoring invalid RTV_PIPELINE_CACHE_MAX_MB=" << configured << '\n';
        }
    }
    return 256ull * 1024ull * 1024ull;
}

double bytesToMiB(uint64_t bytes) {
    return static_cast<double>(bytes) / (1024.0 * 1024.0);
}

std::shared_ptr<const std::vector<uint8_t>> readCacheFileShared(const std::filesystem::path& path) {
    if (!std::filesystem::exists(path)) {
        return emptyPipelineCacheData();
    }

    const std::string key = cacheFileKey(path);
    const auto writeTime = std::filesystem::last_write_time(path);
    const uintmax_t fileSize = std::filesystem::file_size(path);
    if (fileSize == 0) {
        return emptyPipelineCacheData();
    }

    const uint64_t maxBytes = pipelineCacheMaxPersistentBytes();
    if (maxBytes != 0 && fileSize > maxBytes) {
        std::cout << "Ignoring oversized pipeline cache " << path.string()
                  << " (" << fileSize << " bytes, cap "
                  << maxBytes << " bytes / " << bytesToMiB(maxBytes)
                  << " MiB). Set RTV_PIPELINE_CACHE_MAX_MB to override.\n";
        return emptyPipelineCacheData();
    }

    {
        std::lock_guard lock(pipelineCacheFileMutex());
        const auto it = pipelineCacheFiles().find(key);
        if (it != pipelineCacheFiles().end() &&
            it->second.writeTime == writeTime &&
            it->second.fileSize == fileSize &&
            it->second.data != nullptr) {
            return it->second.data;
        }
    }

    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file) {
        return emptyPipelineCacheData();
    }
    const size_t size = static_cast<size_t>(file.tellg());
    if (size == 0) {
        return emptyPipelineCacheData();
    }
    file.seekg(0);

    auto data = std::make_shared<std::vector<uint8_t>>(size);
    file.read(reinterpret_cast<char*>(data->data()), static_cast<std::streamsize>(size));
    if (!file) {
        return emptyPipelineCacheData();
    }

    auto readonlyData = std::static_pointer_cast<const std::vector<uint8_t>>(data);
    {
        std::lock_guard lock(pipelineCacheFileMutex());
        pipelineCacheFiles()[key] = CachedPipelineCacheFile{
            .writeTime = writeTime,
            .fileSize = fileSize,
            .data = readonlyData,
        };
    }
    return readonlyData;
}

void forgetCacheFile(const std::filesystem::path& path) {
    std::lock_guard lock(pipelineCacheFileMutex());
    pipelineCacheFiles().erase(cacheFileKey(path));
}

bool cachedCacheFileMatches(const std::filesystem::path& path, const std::vector<uint8_t>& data) {
    const std::string key = cacheFileKey(path);
    std::lock_guard lock(pipelineCacheFileMutex());
    const auto it = pipelineCacheFiles().find(key);
    return it != pipelineCacheFiles().end() &&
        it->second.data != nullptr &&
        it->second.data->size() == data.size() &&
        std::equal(it->second.data->begin(), it->second.data->end(), data.begin());
}

void rememberCacheFile(const std::filesystem::path& path, std::vector<uint8_t> data) {
    if (!std::filesystem::exists(path)) {
        return;
    }
    const std::string key = cacheFileKey(path);
    const auto writeTime = std::filesystem::last_write_time(path);
    const uintmax_t fileSize = std::filesystem::file_size(path);
    auto readonlyData = std::static_pointer_cast<const std::vector<uint8_t>>(
        std::make_shared<std::vector<uint8_t>>(std::move(data)));

    std::lock_guard lock(pipelineCacheFileMutex());
    pipelineCacheFiles()[key] = CachedPipelineCacheFile{
        .writeTime = writeTime,
        .fileSize = fileSize,
        .data = std::move(readonlyData),
    };
}

} // namespace

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
    const uint64_t maxPersistentBytes = pipelineCacheMaxPersistentBytes();
    if (maxPersistentBytes != 0 && std::filesystem::exists(cachePath)) {
        const uintmax_t cacheBytes = std::filesystem::file_size(cachePath);
        if (cacheBytes > maxPersistentBytes) {
            persistentFileWritesEnabled_ = false;
        }
    }
    const auto initialData = readCacheFileShared(cachePath);
    if (!initialData->empty()) {
        createInfo.initialDataSize = initialData->size();
        createInfo.pInitialData = initialData->data();
        std::cout << "Loaded pipeline cache from " << cachePath.string()
                  << " (" << initialData->size() << " bytes)\n";
    }
    VkResult result = vkCreatePipelineCache(device_, &createInfo, nullptr, &cache_);
    if (result != VK_SUCCESS && !initialData->empty()) {
        std::cerr << "Pipeline cache rejected by driver from " << cachePath.string()
                  << " with VkResult " << result
                  << "; retrying with an empty cache\n";
        forgetCacheFile(cachePath);
        createInfo.initialDataSize = 0;
        createInfo.pInitialData = nullptr;
        result = vkCreatePipelineCache(device_, &createInfo, nullptr, &cache_);
    }
    checkVk(result, "vkCreatePipelineCache");
}

PipelineCache::~PipelineCache() {
    if (cache_ != VK_NULL_HANDLE) {
        vkDestroyPipelineCache(device_, cache_, nullptr);
    }
}

bool PipelineCache::mergeFrom(const PipelineCache& source) {
    if (cache_ == VK_NULL_HANDLE || source.cache_ == VK_NULL_HANDLE) {
        return false;
    }
    return vkMergePipelineCaches(device_, cache_, 1, &source.cache_) == VK_SUCCESS;
}

bool PipelineCache::saveToFile(const std::filesystem::path& path) const {
    if (cache_ == VK_NULL_HANDLE) {
        return false;
    }
    if (!persistentFileWritesEnabled_) {
        std::cout << "Skipping pipeline cache save for oversized persistent cache "
                  << path.string() << ". Delete the file or raise RTV_PIPELINE_CACHE_MAX_MB to re-enable writes.\n";
        return false;
    }

    size_t dataSize = 0;
    VkResult result = vkGetPipelineCacheData(device_, cache_, &dataSize, nullptr);
    if (result != VK_SUCCESS || dataSize == 0) {
        return false;
    }
    const uint64_t maxPersistentBytes = pipelineCacheMaxPersistentBytes();
    if (maxPersistentBytes != 0 && dataSize > maxPersistentBytes) {
        std::cout << "Skipping oversized pipeline cache save to " << path.string()
                  << " (" << dataSize << " bytes, cap "
                  << maxPersistentBytes << " bytes / " << bytesToMiB(maxPersistentBytes)
                  << " MiB). Set RTV_PIPELINE_CACHE_MAX_MB to override.\n";
        return false;
    }

    std::vector<uint8_t> data(dataSize);
    result = vkGetPipelineCacheData(device_, cache_, &dataSize, data.data());
    if (result != VK_SUCCESS) {
        return false;
    }

    if (cachedCacheFileMatches(path, data)) {
        std::cout << "Pipeline cache unchanged at " << path.string()
                  << " (" << dataSize << " bytes)\n";
        return true;
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
    rememberCacheFile(path, std::move(data));
    return true;
}

std::vector<uint8_t> PipelineCache::loadFromFile(const std::filesystem::path& path) {
    return readCacheFile(path);
}

std::vector<uint8_t> PipelineCache::readCacheFile(const std::filesystem::path& path) {
    const auto data = readCacheFileShared(path);
    return *data;
}

} // namespace rtv
