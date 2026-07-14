#include "rtv/DescriptorWriter.h"

#include "rtv/DescriptorWriteDiagnostics.h"

#include <algorithm>
#include <cstring>
#include <deque>
#include <map>
#include <mutex>
#include <sstream>

namespace rtv {
namespace {

constexpr uint32_t kRecentDescriptorWriteLimit = 4096;

struct DescriptorWriteRecorderState {
    uint64_t nextSequence = 1;
    uint64_t updateCallCount = 0;
    uint64_t writeCount = 0;
    uint64_t droppedRecentWriteCount = 0;
    std::deque<DescriptorWriteDiagnosticRecord> recentWrites;
    std::map<std::string, DescriptorWriteDiagnosticAggregate> aggregates;
    std::mutex mutex;
};

DescriptorWriteRecorderState& descriptorWriteRecorderState() {
    static DescriptorWriteRecorderState state;
    return state;
}

template <typename Handle>
uint64_t handleToUint64(Handle handle) {
    uint64_t value = 0;
    static_assert(sizeof(handle) <= sizeof(value), "Vulkan handle is larger than diagnostic storage");
    std::memcpy(&value, &handle, sizeof(handle));
    return value;
}

std::string descriptorWriteAggregateKey(const DescriptorWriteDiagnosticRecord& record) {
    std::ostringstream key;
    key << record.descriptorSetLayout
        << ':' << record.binding
        << ':' << static_cast<uint32_t>(record.type)
        << ':' << record.kind
        << ':' << record.source
        << ':' << record.owner
        << ':' << record.pass
        << ':' << record.setName
        << ':' << record.setIndex;
    return key.str();
}

} // namespace

DescriptorWriter& DescriptorWriter::writeBuffer(uint32_t binding, VkDescriptorType type, const VkDescriptorBufferInfo& bufferInfo) {
    buffers_.push_back(bufferInfo);

    writes_.push_back({binding, type, static_cast<uint32_t>(buffers_.size() - 1), 1, PendingWrite::Kind::Buffer});
    return *this;
}

DescriptorWriter& DescriptorWriter::writeImage(uint32_t binding, VkDescriptorType type, const VkDescriptorImageInfo& imageInfo) {
    images_.push_back(imageInfo);

    writes_.push_back({binding, type, static_cast<uint32_t>(images_.size() - 1), 1, PendingWrite::Kind::Image});
    return *this;
}

DescriptorWriter& DescriptorWriter::writeImageArray(uint32_t binding, VkDescriptorType type, const std::vector<VkDescriptorImageInfo>& imageInfos) {
    const uint32_t first = static_cast<uint32_t>(images_.size());
    images_.insert(images_.end(), imageInfos.begin(), imageInfos.end());
    writes_.push_back({binding, type, first, static_cast<uint32_t>(imageInfos.size()), PendingWrite::Kind::Image});
    return *this;
}

DescriptorWriter& DescriptorWriter::writeAccelerationStructure(uint32_t binding, VkAccelerationStructureKHR accelerationStructure) {
    accelerationStructures_.push_back(accelerationStructure);
    writes_.push_back({
        binding,
        VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR,
        static_cast<uint32_t>(accelerationStructures_.size() - 1),
        1,
        PendingWrite::Kind::AccelerationStructure,
    });
    return *this;
}

void DescriptorWriter::update(VkDevice device, DescriptorSet set, DescriptorWriteOwner owner) const {
    auto descriptorWriteKindName = [](PendingWrite::Kind kind) {
        switch (kind) {
        case PendingWrite::Kind::Buffer: return "buffer";
        case PendingWrite::Kind::Image: return "image";
        case PendingWrite::Kind::AccelerationStructure: return "acceleration_structure";
        default: return "unknown";
        }
    };

    std::vector<VkWriteDescriptorSet> patched;
    patched.reserve(writes_.size());
    std::vector<DescriptorWriteDiagnosticEntry> diagnosticWrites;
    diagnosticWrites.reserve(writes_.size());
    std::vector<VkWriteDescriptorSetAccelerationStructureKHR> accelerationStructureWrites;
    accelerationStructureWrites.reserve(accelerationStructures_.size());
    for (const PendingWrite& pending : writes_) {
        VkWriteDescriptorSet write{};
        write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        write.dstSet = set.handle();
        write.dstBinding = pending.binding;
        write.descriptorCount = pending.count;
        write.descriptorType = pending.type;
        if (pending.kind == PendingWrite::Kind::Image) {
            write.pImageInfo = &images_.at(pending.index);
        } else if (pending.kind == PendingWrite::Kind::Buffer) {
            write.pBufferInfo = &buffers_.at(pending.index);
        } else {
            VkWriteDescriptorSetAccelerationStructureKHR asWrite{};
            asWrite.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET_ACCELERATION_STRUCTURE_KHR;
            asWrite.accelerationStructureCount = pending.count;
            asWrite.pAccelerationStructures = &accelerationStructures_.at(pending.index);
            accelerationStructureWrites.push_back(asWrite);
            write.pNext = &accelerationStructureWrites.back();
        }
        diagnosticWrites.push_back({
            .descriptorSet = set.handle(),
            .descriptorSetLayout = set.layout(),
            .binding = pending.binding,
            .arrayElement = 0,
            .count = pending.count,
            .type = pending.type,
            .kind = descriptorWriteKindName(pending.kind),
            .source = "DescriptorWriter",
            .owner = owner,
        });
        patched.push_back(write);
    }
    recordDescriptorWriteUpdate(diagnosticWrites);
    vkUpdateDescriptorSets(device, static_cast<uint32_t>(patched.size()), patched.data(), 0, nullptr);
}

void recordDescriptorWriteUpdate(const std::vector<DescriptorWriteDiagnosticEntry>& entries) {
    DescriptorWriteRecorderState& state = descriptorWriteRecorderState();
    std::lock_guard<std::mutex> lock(state.mutex);
    ++state.updateCallCount;
    for (const DescriptorWriteDiagnosticEntry& entry : entries) {
        DescriptorWriteDiagnosticRecord record{
            .sequence = state.nextSequence++,
            .descriptorSet = handleToUint64(entry.descriptorSet),
            .descriptorSetLayout = handleToUint64(entry.descriptorSetLayout),
            .binding = entry.binding,
            .arrayElement = entry.arrayElement,
            .count = entry.count,
            .type = entry.type,
            .kind = entry.kind != nullptr ? entry.kind : "",
            .source = entry.source != nullptr ? entry.source : "",
            .owner = entry.owner.owner != nullptr ? entry.owner.owner : "",
            .pass = entry.owner.pass != nullptr ? entry.owner.pass : "",
            .setName = entry.owner.setName != nullptr ? entry.owner.setName : "",
            .setIndex = entry.owner.setIndex,
        };
        ++state.writeCount;
        if (state.recentWrites.size() >= kRecentDescriptorWriteLimit) {
            state.recentWrites.pop_front();
            ++state.droppedRecentWriteCount;
        }
        state.recentWrites.push_back(record);

        auto& aggregate = state.aggregates[descriptorWriteAggregateKey(record)];
        if (aggregate.occurrenceCount == 0) {
            aggregate.descriptorSetLayout = record.descriptorSetLayout;
            aggregate.binding = record.binding;
            aggregate.type = record.type;
            aggregate.kind = record.kind;
            aggregate.source = record.source;
            aggregate.owner = record.owner;
            aggregate.pass = record.pass;
            aggregate.setName = record.setName;
            aggregate.setIndex = record.setIndex;
            aggregate.minCount = record.count;
            aggregate.maxCount = record.count;
        } else {
            aggregate.minCount = std::min(aggregate.minCount, record.count);
            aggregate.maxCount = std::max(aggregate.maxCount, record.count);
        }
        ++aggregate.occurrenceCount;
    }
}

void resetDescriptorWriteDiagnostics() {
    DescriptorWriteRecorderState& state = descriptorWriteRecorderState();
    std::lock_guard<std::mutex> lock(state.mutex);
    state.nextSequence = 1;
    state.updateCallCount = 0;
    state.writeCount = 0;
    state.droppedRecentWriteCount = 0;
    state.recentWrites.clear();
    state.aggregates.clear();
}

DescriptorWriteDiagnosticsSnapshot descriptorWriteDiagnosticsSnapshot() {
    DescriptorWriteRecorderState& state = descriptorWriteRecorderState();
    std::lock_guard<std::mutex> lock(state.mutex);
    DescriptorWriteDiagnosticsSnapshot snapshot;
    snapshot.updateCallCount = state.updateCallCount;
    snapshot.writeCount = state.writeCount;
    snapshot.droppedRecentWriteCount = state.droppedRecentWriteCount;
    snapshot.recentWriteLimit = kRecentDescriptorWriteLimit;
    snapshot.recentWrites.assign(state.recentWrites.begin(), state.recentWrites.end());
    for (const auto& [key, aggregate] : state.aggregates) {
        (void)key;
        snapshot.aggregates.push_back(aggregate);
    }
    return snapshot;
}

} // namespace rtv
