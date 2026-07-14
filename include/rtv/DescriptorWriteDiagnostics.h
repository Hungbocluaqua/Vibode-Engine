#pragma once

#include <Volk/volk.h>

#include <cstdint>
#include <string>
#include <vector>

namespace rtv {

struct DescriptorWriteOwner {
    const char* owner = "";
    const char* pass = "";
    const char* setName = "";
    int32_t setIndex = -1;
};

struct DescriptorWriteDiagnosticEntry {
    VkDescriptorSet descriptorSet = VK_NULL_HANDLE;
    VkDescriptorSetLayout descriptorSetLayout = VK_NULL_HANDLE;
    uint32_t binding = 0;
    uint32_t arrayElement = 0;
    uint32_t count = 0;
    VkDescriptorType type = VK_DESCRIPTOR_TYPE_MAX_ENUM;
    const char* kind = "";
    const char* source = "";
    DescriptorWriteOwner owner;
};

struct DescriptorWriteDiagnosticRecord {
    uint64_t sequence = 0;
    uint64_t descriptorSet = 0;
    uint64_t descriptorSetLayout = 0;
    uint32_t binding = 0;
    uint32_t arrayElement = 0;
    uint32_t count = 0;
    VkDescriptorType type = VK_DESCRIPTOR_TYPE_MAX_ENUM;
    std::string kind;
    std::string source;
    std::string owner;
    std::string pass;
    std::string setName;
    int32_t setIndex = -1;
};

struct DescriptorWriteDiagnosticAggregate {
    uint64_t descriptorSetLayout = 0;
    uint32_t binding = 0;
    VkDescriptorType type = VK_DESCRIPTOR_TYPE_MAX_ENUM;
    std::string kind;
    std::string source;
    std::string owner;
    std::string pass;
    std::string setName;
    int32_t setIndex = -1;
    uint32_t minCount = 0;
    uint32_t maxCount = 0;
    uint64_t occurrenceCount = 0;
};

struct DescriptorWriteDiagnosticsSnapshot {
    uint64_t updateCallCount = 0;
    uint64_t writeCount = 0;
    uint64_t droppedRecentWriteCount = 0;
    uint32_t recentWriteLimit = 0;
    std::vector<DescriptorWriteDiagnosticRecord> recentWrites;
    std::vector<DescriptorWriteDiagnosticAggregate> aggregates;
};

void recordDescriptorWriteUpdate(const std::vector<DescriptorWriteDiagnosticEntry>& entries);
void resetDescriptorWriteDiagnostics();
[[nodiscard]] DescriptorWriteDiagnosticsSnapshot descriptorWriteDiagnosticsSnapshot();

} // namespace rtv
