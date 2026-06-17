#pragma once

#include "rtv/NonCopyable.h"
#include "rtv/StreamingStagingRing.h"

#include <Volk/volk.h>

#include <nlohmann/json_fwd.hpp>

#include <cstdint>
#include <deque>
#include <vector>

namespace rtv {

class VulkanContext;
class ResourceAllocator;
class Buffer;
class Image;
class AccelerationStructure;

// Real device-backed transfer executor for streaming uploads.
//
// Records buffer/image copies into command buffers submitted on the transfer
// queue (falling back to graphics when no dedicated transfer family exists),
// each submission signaling a monotonically increasing value on a dedicated
// timeline semaphore. Staging memory is sourced from a bounded StreamingStagingRing
// and reclaimed only once the device has signaled the timeline value that
// consumed it, so uploads never call vkDeviceWaitIdle and staging memory stays
// bounded.
//
// Completion is polled from vkGetSemaphoreCounterValue, never faked.
class StreamingGpuTransferExecutor final : private NonCopyable {
public:
    struct Stats {
        bool deviceBacked = false;
        bool dedicatedTransferQueue = false;
        uint32_t transferQueueFamily = UINT32_MAX;
        uint64_t submittedTimeline = 0;     // highest value ever signaled-for
        uint64_t completedTimeline = 0;     // last polled completed value
        uint32_t inFlightSubmissions = 0;
        uint32_t totalSubmissions = 0;
        uint32_t totalBufferCopies = 0;
        uint32_t totalImageCopies = 0;
        uint32_t totalBlasBuilds = 0;
        uint32_t totalBlasCompactions = 0;
        uint32_t totalTlasBuilds = 0;
        uint64_t totalUploadedBytes = 0;
        uint32_t stagingAllocationFailures = 0;
        StreamingStagingRingStats staging{};
    };

    StreamingGpuTransferExecutor() = default;
    ~StreamingGpuTransferExecutor();

    // Initialize against a live device. Safe to call once. capacityBytes bounds
    // the staging ring. Returns false if device-backed init failed.
    bool initialize(const VulkanContext& context, ResourceAllocator& allocator, uint64_t stagingCapacityBytes);
    void shutdown();

    [[nodiscard]] bool initialized() const { return device_ != VK_NULL_HANDLE; }

    // Stage `bytes` from `src` into the ring and record a copy into `destination`
    // at dstOffset. The copy is recorded into the open frame batch; call
    // submitFrame() to flush. Returns false if the staging ring is full (bounded
    // back-pressure) - the caller should retry next frame.
    [[nodiscard]] bool stageBufferUpload(Buffer& destination, const void* src, uint64_t bytes, uint64_t dstOffset = 0);
    [[nodiscard]] bool stageImageMipUpload(Image& destination, const void* src, uint64_t bytes, uint32_t mipLevel, uint32_t width, uint32_t height);

    struct BlasTriangleBuild {
        AccelerationStructure* destination = nullptr;
        Buffer* scratch = nullptr;
        Buffer* vertexBuffer = nullptr;
        Buffer* indexBuffer = nullptr;
        uint64_t vertexDataOffset = 0;
        uint64_t indexDataOffset = 0;
        uint32_t vertexCount = 0;
        uint32_t indexCount = 0;
        uint32_t vertexStride = 0;
        VkBuildAccelerationStructureFlagsKHR flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR;
    };

    [[nodiscard]] bool stageBlasBuild(const BlasTriangleBuild& build);
    [[nodiscard]] bool stageBlasCompaction(AccelerationStructure& source, AccelerationStructure& destination);
    [[nodiscard]] uint64_t compactedBlasSize(const AccelerationStructure& source) const;
    [[nodiscard]] uint64_t consumeCompactedBlasSize(const AccelerationStructure& source);

    struct TlasBuild {
        AccelerationStructure* destination = nullptr;
        Buffer* scratch = nullptr;
        Buffer* instanceBuffer = nullptr;
        uint64_t instanceDataOffset = 0;
        uint32_t instanceCount = 0;
        VkBuildAccelerationStructureFlagsKHR flags =
            VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR |
            VK_BUILD_ACCELERATION_STRUCTURE_ALLOW_UPDATE_BIT_KHR;
    };

    [[nodiscard]] bool stageTlasBuild(const TlasBuild& build);

    // Flush all copies recorded since the last submitFrame() as one transfer
    // submission that signals the next timeline value. Returns the signaled
    // timeline value (0 if nothing was recorded).
    uint64_t submitFrame();

    // Signal a timeline value for the current transfer batch. If copies are
    // recorded, submitFrame() is used and that submission's timeline gates the
    // batch. Otherwise, submit a no-op timeline marker. Returns the signaled
    // value, or 0 if the executor is unavailable.
    uint64_t submitTimelineMarker();

    // Poll the device timeline and reclaim staging for completed submissions.
    // Returns the completed timeline value.
    uint64_t poll();

    [[nodiscard]] uint64_t nextTimelineValue() const { return nextTimelineValue_; }
    [[nodiscard]] Stats stats() const;

    // Headless self-test: stages a known byte pattern through the ring, submits a
    // real transfer, waits on the timeline, reads it back, and validates bytes.
    // Returns true if the round-trip matched. Used by --selftest-streaming-transfer.
    [[nodiscard]] bool runSelfTest(std::string& errorOut);

private:
    struct InFlightBatch {
        uint64_t timelineValue = 0;
        VkCommandBuffer commandBuffer = VK_NULL_HANDLE;
        bool graphics = false;
    };
    struct PendingCompactionQuery {
        const AccelerationStructure* source = nullptr;
        VkQueryPool queryPool = VK_NULL_HANDLE;
        uint64_t timelineValue = 0;
    };

    [[nodiscard]] VkCommandBuffer beginBatch();
    [[nodiscard]] VkCommandBuffer beginGraphicsBatch();
    uint64_t submitGraphicsFrame(uint64_t waitTimelineValue = 0);
    void recycleCompleted(uint64_t completedTimeline);

    VkDevice device_ = VK_NULL_HANDLE;
    ResourceAllocator* allocator_ = nullptr;
    VkQueue queue_ = VK_NULL_HANDLE;
    uint32_t queueFamily_ = UINT32_MAX;
    bool dedicatedTransferQueue_ = false;
    VkQueue graphicsQueue_ = VK_NULL_HANDLE;
    uint32_t graphicsQueueFamily_ = UINT32_MAX;
    VkCommandPool commandPool_ = VK_NULL_HANDLE;
    VkCommandPool graphicsCommandPool_ = VK_NULL_HANDLE;
    VkSemaphore timeline_ = VK_NULL_HANDLE;
    uint64_t nextTimelineValue_ = 1;
    uint64_t submittedTimeline_ = 0;
    uint64_t completedTimeline_ = 0;
    uint64_t accelerationStructureScratchAlignment_ = 256;

    StreamingStagingRing stagingRing_;
    VkCommandBuffer openBatch_ = VK_NULL_HANDLE;
    uint32_t openBatchCopies_ = 0;
    VkCommandBuffer openGraphicsBatch_ = VK_NULL_HANDLE;
    uint32_t openGraphicsBatchOps_ = 0;
    std::vector<PendingCompactionQuery> openGraphicsCompactionQueries_;
    std::vector<PendingCompactionQuery> pendingCompactionQueries_;
    std::vector<std::pair<const AccelerationStructure*, uint64_t>> compactedBlasSizes_;
    std::deque<InFlightBatch> inFlight_;
    std::vector<VkCommandBuffer> freeCommandBuffers_;
    std::vector<VkCommandBuffer> freeGraphicsCommandBuffers_;

    uint32_t totalSubmissions_ = 0;
    uint32_t totalBufferCopies_ = 0;
    uint32_t totalImageCopies_ = 0;
    uint32_t totalBlasBuilds_ = 0;
    uint32_t totalBlasCompactions_ = 0;
    uint32_t totalTlasBuilds_ = 0;
    uint64_t totalUploadedBytes_ = 0;
    uint32_t stagingAllocationFailures_ = 0;
};

[[nodiscard]] nlohmann::json streamingGpuTransferExecutorStatsJson(const StreamingGpuTransferExecutor::Stats& stats);

} // namespace rtv
