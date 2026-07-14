#include "rtv/StreamingGpuTransferExecutor.h"

#include "rtv/AccelerationStructure.h"
#include "rtv/Buffer.h"
#include "rtv/Check.h"
#include "rtv/GpuValidation.h"
#include "rtv/Image.h"
#include "rtv/ImageBarrier.h"
#include "rtv/ResourceAllocator.h"
#include "rtv/VulkanContext.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <cstring>
#include <vector>
#include <utility>

namespace rtv {

StreamingGpuTransferExecutor::~StreamingGpuTransferExecutor() {
    shutdown();
}

bool StreamingGpuTransferExecutor::initialize(const VulkanContext& context, ResourceAllocator& allocator, uint64_t stagingCapacityBytes) {
    if (device_ != VK_NULL_HANDLE) {
        return true;
    }
    if (context.device() == VK_NULL_HANDLE) {
        return false;
    }
    device_ = context.device();
    allocator_ = &allocator;

    const QueueFamilyIndices& families = context.queueFamilies();
    // Prefer a dedicated transfer queue; otherwise fall back to graphics so the
    // path always works (Phase 7 exit criterion: no hard dependency on a DMA queue).
    if (context.hasIndependentTransferQueue() && families.transfer.has_value()) {
        queue_ = context.transferQueue();
        queueFamily_ = families.transfer.value();
        dedicatedTransferQueue_ = true;
    } else {
        queue_ = context.graphicsQueue();
        queueFamily_ = families.graphics.value();
        dedicatedTransferQueue_ = false;
    }
    graphicsQueue_ = context.graphicsQueue();
    graphicsQueueFamily_ = families.graphics.value();
    accelerationStructureScratchAlignment_ = std::max<uint64_t>(
        1ull,
        context.rayTracingInfo().accelerationStructureProperties.minAccelerationStructureScratchOffsetAlignment);

    VkCommandPoolCreateInfo poolInfo{};
    poolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    poolInfo.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT | VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    poolInfo.queueFamilyIndex = queueFamily_;
    if (vkCreateCommandPool(device_, &poolInfo, nullptr, &commandPool_) != VK_SUCCESS) {
        device_ = VK_NULL_HANDLE;
        return false;
    }
    VkCommandPoolCreateInfo graphicsPoolInfo{};
    graphicsPoolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    graphicsPoolInfo.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT | VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    graphicsPoolInfo.queueFamilyIndex = graphicsQueueFamily_;
    if (vkCreateCommandPool(device_, &graphicsPoolInfo, nullptr, &graphicsCommandPool_) != VK_SUCCESS) {
        vkDestroyCommandPool(device_, commandPool_, nullptr);
        commandPool_ = VK_NULL_HANDLE;
        device_ = VK_NULL_HANDLE;
        return false;
    }

    VkSemaphoreTypeCreateInfo timelineInfo{};
    timelineInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO;
    timelineInfo.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
    timelineInfo.initialValue = 0;
    VkSemaphoreCreateInfo semInfo{};
    semInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
    semInfo.pNext = &timelineInfo;
    if (vkCreateSemaphore(device_, &semInfo, nullptr, &timeline_) != VK_SUCCESS) {
        vkDestroyCommandPool(device_, graphicsCommandPool_, nullptr);
        graphicsCommandPool_ = VK_NULL_HANDLE;
        vkDestroyCommandPool(device_, commandPool_, nullptr);
        commandPool_ = VK_NULL_HANDLE;
        device_ = VK_NULL_HANDLE;
        return false;
    }

    stagingRing_ = StreamingStagingRing(allocator, stagingCapacityBytes);
    return true;
}

void StreamingGpuTransferExecutor::shutdown() {
    if (device_ == VK_NULL_HANDLE) {
        return;
    }
    // Drain any in-flight submissions so command buffers/staging are safe to free.
    if (timeline_ != VK_NULL_HANDLE && submittedTimeline_ > 0) {
        VkSemaphoreWaitInfo waitInfo{};
        waitInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO;
        waitInfo.semaphoreCount = 1;
        waitInfo.pSemaphores = &timeline_;
        waitInfo.pValues = &submittedTimeline_;
        (void)vkWaitSemaphores(device_, &waitInfo, UINT64_MAX);
    }
    if (computeTimeline_ != VK_NULL_HANDLE && submittedComputeTimeline_ > 0) {
        VkSemaphoreWaitInfo waitInfo{};
        waitInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO;
        waitInfo.semaphoreCount = 1;
        waitInfo.pSemaphores = &computeTimeline_;
        waitInfo.pValues = &submittedComputeTimeline_;
        (void)vkWaitSemaphores(device_, &waitInfo, UINT64_MAX);
    }
    const bool computeSharesTransferTimeline = computeTimeline_ == timeline_;
    if (computeTimelineOwned_ && computeTimeline_ != VK_NULL_HANDLE && !computeSharesTransferTimeline) {
        vkDestroySemaphore(device_, computeTimeline_, nullptr);
    }
    computeTimeline_ = VK_NULL_HANDLE;
    computeTimelineOwned_ = false;
    if (timeline_ != VK_NULL_HANDLE) {
        vkDestroySemaphore(device_, timeline_, nullptr);
        timeline_ = VK_NULL_HANDLE;
    }
    for (const PendingCompactionQuery& query : openGraphicsCompactionQueries_) {
        if (query.queryPool != VK_NULL_HANDLE) {
            vkDestroyQueryPool(device_, query.queryPool, nullptr);
        }
    }
    openGraphicsCompactionQueries_.clear();
    for (const PendingCompactionQuery& query : pendingCompactionQueries_) {
        if (query.queryPool != VK_NULL_HANDLE) {
            vkDestroyQueryPool(device_, query.queryPool, nullptr);
        }
    }
    pendingCompactionQueries_.clear();
    compactedBlasSizes_.clear();
    imageLayouts_.clear();
    if (commandPool_ != VK_NULL_HANDLE) {
        vkDestroyCommandPool(device_, commandPool_, nullptr);
        commandPool_ = VK_NULL_HANDLE;
    }
    if (graphicsCommandPool_ != VK_NULL_HANDLE) {
        vkDestroyCommandPool(device_, graphicsCommandPool_, nullptr);
        graphicsCommandPool_ = VK_NULL_HANDLE;
    }
    stagingRing_ = StreamingStagingRing();
    inFlight_.clear();
    freeCommandBuffers_.clear();
    freeGraphicsCommandBuffers_.clear();
    openBatch_ = VK_NULL_HANDLE;
    openBatchCopies_ = 0;
    openGraphicsBatch_ = VK_NULL_HANDLE;
    openGraphicsBatchOps_ = 0;
    device_ = VK_NULL_HANDLE;
    allocator_ = nullptr;
    graphicsQueue_ = VK_NULL_HANDLE;
    graphicsQueueFamily_ = UINT32_MAX;
    computeQueue_ = VK_NULL_HANDLE;
    nextComputeTimelineValue_ = 1;
    submittedComputeTimeline_ = 0;
    completedComputeTimeline_ = 0;
}

VkCommandBuffer StreamingGpuTransferExecutor::beginBatch() {
    if (openBatch_ != VK_NULL_HANDLE) {
        return openBatch_;
    }
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    if (!freeCommandBuffers_.empty()) {
        cmd = freeCommandBuffers_.back();
        freeCommandBuffers_.pop_back();
        vkResetCommandBuffer(cmd, 0);
    } else {
        VkCommandBufferAllocateInfo allocInfo{};
        allocInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        allocInfo.commandPool = commandPool_;
        allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        allocInfo.commandBufferCount = 1;
        checkVk(vkAllocateCommandBuffers(device_, &allocInfo, &cmd), "vkAllocateCommandBuffers(streaming transfer)");
    }
    VkCommandBufferBeginInfo beginInfo{};
    beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    checkVk(vkBeginCommandBuffer(cmd, &beginInfo), "vkBeginCommandBuffer(streaming transfer)");
    openBatch_ = cmd;
    openBatchCopies_ = 0;
    return cmd;
}

VkCommandBuffer StreamingGpuTransferExecutor::beginGraphicsBatch() {
    if (openGraphicsBatch_ != VK_NULL_HANDLE) {
        return openGraphicsBatch_;
    }
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    if (!freeGraphicsCommandBuffers_.empty()) {
        cmd = freeGraphicsCommandBuffers_.back();
        freeGraphicsCommandBuffers_.pop_back();
        vkResetCommandBuffer(cmd, 0);
    } else {
        VkCommandBufferAllocateInfo allocInfo{};
        allocInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        allocInfo.commandPool = graphicsCommandPool_;
        allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        allocInfo.commandBufferCount = 1;
        checkVk(vkAllocateCommandBuffers(device_, &allocInfo, &cmd), "vkAllocateCommandBuffers(streaming graphics)");
    }
    VkCommandBufferBeginInfo beginInfo{};
    beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    checkVk(vkBeginCommandBuffer(cmd, &beginInfo), "vkBeginCommandBuffer(streaming graphics)");
    openGraphicsBatch_ = cmd;
    openGraphicsBatchOps_ = 0;
    return cmd;
}

bool StreamingGpuTransferExecutor::stageBufferUpload(Buffer& destination, const void* src, uint64_t bytes, uint64_t dstOffset) {
    if (device_ == VK_NULL_HANDLE || src == nullptr || bytes == 0) {
        return false;
    }
    // The staging range is consumed by the submission that signals the *next*
    // timeline value (the batch we are about to record into).
    const uint64_t targetTimeline = nextTimelineValue_;
    std::optional<StreamingStagingAllocation> alloc = stagingRing_.allocate(bytes, targetTimeline);
    if (!alloc.has_value()) {
        ++stagingAllocationFailures_;
        return false; // bounded back-pressure
    }
    if (alloc->mapped == nullptr || alloc->buffer == VK_NULL_HANDLE) {
        ++stagingAllocationFailures_;
        return false;
    }
    std::memcpy(alloc->mapped, src, static_cast<size_t>(bytes));
    stagingRing_.flush(alloc->offset, bytes);

    VkCommandBuffer cmd = beginBatch();
    VkBufferCopy copy{};
    copy.srcOffset = alloc->offset;
    copy.dstOffset = dstOffset;
    copy.size = bytes;
    vkCmdCopyBuffer(cmd, alloc->buffer, destination.handle(), 1, &copy);
    ++openBatchCopies_;
    ++totalBufferCopies_;
    totalUploadedBytes_ += bytes;
    return true;
}

bool StreamingGpuTransferExecutor::stageImageMipUpload(
    Image& destination,
    const void* src,
    uint64_t bytes,
    uint32_t mipLevel,
    uint32_t width,
    uint32_t height) {
    if (device_ == VK_NULL_HANDLE || src == nullptr || bytes == 0 || destination.handle() == VK_NULL_HANDLE || mipLevel >= destination.mipLevels()) {
        return false;
    }
    const uint64_t targetTimeline = nextTimelineValue_;
    std::optional<StreamingStagingAllocation> alloc = stagingRing_.allocate(bytes, targetTimeline);
    if (!alloc.has_value()) {
        ++stagingAllocationFailures_;
        return false;
    }
    if (alloc->mapped == nullptr || alloc->buffer == VK_NULL_HANDLE) {
        ++stagingAllocationFailures_;
        return false;
    }
    std::memcpy(alloc->mapped, src, static_cast<size_t>(bytes));
    stagingRing_.flush(alloc->offset, bytes);

    VkCommandBuffer cmd = beginBatch();
    TrackedImageLayouts& tracked = imageLayouts_[&destination];
    if (tracked.image != destination.handle() || tracked.mips.size() != destination.mipLevels()) {
        tracked.image = destination.handle();
        tracked.mips.assign(destination.mipLevels(), destination.layout());
    }
    const VkImageLayout oldMipLayout = tracked.mips[mipLevel];
    barrier::cmdTransitionImage(cmd, {
        .image = destination.handle(),
        .oldLayout = oldMipLayout,
        .newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        .range = barrier::colorRange(mipLevel, 1),
        .srcStage = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
        .srcAccess = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT,
        .dstStage = VK_PIPELINE_STAGE_2_COPY_BIT,
        .dstAccess = VK_ACCESS_2_TRANSFER_WRITE_BIT,
    });

    VkBufferImageCopy copy{};
    copy.bufferOffset = alloc->offset;
    copy.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    copy.imageSubresource.mipLevel = mipLevel;
    copy.imageSubresource.baseArrayLayer = 0;
    copy.imageSubresource.layerCount = 1;
    copy.imageExtent = {
        std::max(width, 1u),
        std::max(height, 1u),
        1u,
    };
    vkCmdCopyBufferToImage(
        cmd,
        alloc->buffer,
        destination.handle(),
        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        1,
        &copy);

    barrier::cmdTransitionImage(cmd, {
        .image = destination.handle(),
        .oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        .newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
        .range = barrier::colorRange(mipLevel, 1),
        .srcStage = VK_PIPELINE_STAGE_2_COPY_BIT,
        .srcAccess = VK_ACCESS_2_TRANSFER_WRITE_BIT,
        .dstStage = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
        .dstAccess = VK_ACCESS_2_MEMORY_READ_BIT,
    });
    tracked.mips[mipLevel] = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    destination.setLayout(VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);

    ++openBatchCopies_;
    ++totalImageCopies_;
    totalUploadedBytes_ += bytes;
    return true;
}

bool StreamingGpuTransferExecutor::stageBlasBuild(const BlasTriangleBuild& build) {
    if (device_ == VK_NULL_HANDLE ||
        build.destination == nullptr ||
        build.scratch == nullptr ||
        build.vertexBuffer == nullptr ||
        build.indexBuffer == nullptr ||
        build.destination->handle() == VK_NULL_HANDLE ||
        build.scratch->handle() == VK_NULL_HANDLE ||
        build.vertexBuffer->handle() == VK_NULL_HANDLE ||
        build.indexBuffer->handle() == VK_NULL_HANDLE ||
        build.vertexCount == 0 ||
        build.indexCount < 3 ||
        build.vertexStride == 0 ||
        !build.vertexBuffer->supportsDeviceAddress() ||
        !build.indexBuffer->supportsDeviceAddress()) {
        return false;
    }

    VkAccelerationStructureGeometryTrianglesDataKHR triangles{};
    triangles.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_TRIANGLES_DATA_KHR;
    triangles.vertexFormat = VK_FORMAT_R32G32B32_SFLOAT;
    triangles.vertexData.deviceAddress = build.vertexBuffer->deviceAddress() + build.vertexDataOffset;
    triangles.vertexStride = build.vertexStride;
    triangles.maxVertex = build.vertexCount - 1u;
    triangles.indexType = VK_INDEX_TYPE_UINT32;
    triangles.indexData.deviceAddress = build.indexBuffer->deviceAddress() + build.indexDataOffset;

    VkAccelerationStructureGeometryKHR geometry{};
    geometry.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR;
    geometry.geometryType = VK_GEOMETRY_TYPE_TRIANGLES_KHR;
    geometry.flags = VK_GEOMETRY_OPAQUE_BIT_KHR;
    geometry.geometry.triangles = triangles;

    VkAccelerationStructureBuildGeometryInfoKHR buildInfo{};
    buildInfo.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR;
    buildInfo.type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
    buildInfo.flags = build.flags;
    buildInfo.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
    buildInfo.geometryCount = 1;
    buildInfo.pGeometries = &geometry;
    buildInfo.dstAccelerationStructure = build.destination->handle();
    buildInfo.scratchData.deviceAddress = Buffer::alignUp(build.scratch->deviceAddress(), accelerationStructureScratchAlignment_);

    VkAccelerationStructureBuildRangeInfoKHR range{};
    range.primitiveCount = build.indexCount / 3u;
    range.primitiveOffset = 0;
    range.firstVertex = 0;
    range.transformOffset = 0;
    const VkAccelerationStructureBuildRangeInfoKHR* ranges[] = {&range};

    VkCommandBuffer cmd = beginGraphicsBatch();
    vkCmdBuildAccelerationStructuresKHR(cmd, 1, &buildInfo, ranges);

    VkMemoryBarrier2 barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2;
    barrier.srcStageMask = VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR;
    barrier.srcAccessMask = VK_ACCESS_2_ACCELERATION_STRUCTURE_WRITE_BIT_KHR;
    barrier.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    barrier.dstAccessMask = VK_ACCESS_2_ACCELERATION_STRUCTURE_READ_BIT_KHR |
        VK_ACCESS_2_ACCELERATION_STRUCTURE_WRITE_BIT_KHR |
        VK_ACCESS_2_MEMORY_READ_BIT |
        VK_ACCESS_2_MEMORY_WRITE_BIT;
    VkDependencyInfo dependency{};
    dependency.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
    dependency.memoryBarrierCount = 1;
    dependency.pMemoryBarriers = &barrier;
    recordManualBarrierEscape("StreamingGpuTransferExecutor", "streaming_blas_build", dependency);
    vkCmdPipelineBarrier2(cmd, &dependency);

    if ((build.flags & VK_BUILD_ACCELERATION_STRUCTURE_ALLOW_COMPACTION_BIT_KHR) != 0u) {
        VkQueryPoolCreateInfo queryInfo{};
        queryInfo.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO;
        queryInfo.queryType = VK_QUERY_TYPE_ACCELERATION_STRUCTURE_COMPACTED_SIZE_KHR;
        queryInfo.queryCount = 1;
        VkQueryPool queryPool = VK_NULL_HANDLE;
        if (vkCreateQueryPool(device_, &queryInfo, nullptr, &queryPool) == VK_SUCCESS) {
            vkCmdResetQueryPool(cmd, queryPool, 0, 1);
            VkAccelerationStructureKHR accelerationStructure = build.destination->handle();
            vkCmdWriteAccelerationStructuresPropertiesKHR(
                cmd,
                1,
                &accelerationStructure,
                VK_QUERY_TYPE_ACCELERATION_STRUCTURE_COMPACTED_SIZE_KHR,
                queryPool,
                0);
            openGraphicsCompactionQueries_.push_back(PendingCompactionQuery{
                .source = build.destination,
                .queryPool = queryPool,
            });
        }
    }

    ++openGraphicsBatchOps_;
    ++totalBlasBuilds_;
    return true;
}

bool StreamingGpuTransferExecutor::stageBlasCompaction(AccelerationStructure& source, AccelerationStructure& destination) {
    if (device_ == VK_NULL_HANDLE ||
        source.handle() == VK_NULL_HANDLE ||
        destination.handle() == VK_NULL_HANDLE ||
        source.type() != VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR ||
        destination.type() != VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR) {
        return false;
    }

    VkCopyAccelerationStructureInfoKHR copyInfo{};
    copyInfo.sType = VK_STRUCTURE_TYPE_COPY_ACCELERATION_STRUCTURE_INFO_KHR;
    copyInfo.src = source.handle();
    copyInfo.dst = destination.handle();
    copyInfo.mode = VK_COPY_ACCELERATION_STRUCTURE_MODE_COMPACT_KHR;

    VkCommandBuffer cmd = beginGraphicsBatch();
    vkCmdCopyAccelerationStructureKHR(cmd, &copyInfo);

    VkMemoryBarrier2 barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2;
    // The dedicated copy stage requires VK_KHR_ray_tracing_maintenance1. The
    // build stage also covers acceleration-structure copies on baseline KHR RT.
    barrier.srcStageMask = VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR;
    barrier.srcAccessMask = VK_ACCESS_2_ACCELERATION_STRUCTURE_WRITE_BIT_KHR;
    barrier.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    barrier.dstAccessMask = VK_ACCESS_2_ACCELERATION_STRUCTURE_READ_BIT_KHR |
        VK_ACCESS_2_ACCELERATION_STRUCTURE_WRITE_BIT_KHR |
        VK_ACCESS_2_MEMORY_READ_BIT |
        VK_ACCESS_2_MEMORY_WRITE_BIT;
    VkDependencyInfo dependency{};
    dependency.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
    dependency.memoryBarrierCount = 1;
    dependency.pMemoryBarriers = &barrier;
    recordManualBarrierEscape("StreamingGpuTransferExecutor", "streaming_blas_compaction", dependency);
    vkCmdPipelineBarrier2(cmd, &dependency);

    ++openGraphicsBatchOps_;
    ++totalBlasCompactions_;
    return true;
}

uint64_t StreamingGpuTransferExecutor::compactedBlasSize(const AccelerationStructure& source) const {
    for (const auto& [storedSource, bytes] : compactedBlasSizes_) {
        if (storedSource == &source) {
            return bytes;
        }
    }
    return 0;
}

uint64_t StreamingGpuTransferExecutor::consumeCompactedBlasSize(const AccelerationStructure& source) {
    for (auto it = compactedBlasSizes_.begin(); it != compactedBlasSizes_.end(); ++it) {
        if (it->first == &source) {
            const uint64_t bytes = it->second;
            compactedBlasSizes_.erase(it);
            return bytes;
        }
    }
    return 0;
}

bool StreamingGpuTransferExecutor::stageTlasBuild(const TlasBuild& build) {
    if (device_ == VK_NULL_HANDLE ||
        build.destination == nullptr ||
        build.scratch == nullptr ||
        build.instanceBuffer == nullptr ||
        build.destination->handle() == VK_NULL_HANDLE ||
        build.scratch->handle() == VK_NULL_HANDLE ||
        build.instanceBuffer->handle() == VK_NULL_HANDLE ||
        build.instanceCount == 0) {
        return false;
    }

    VkAccelerationStructureGeometryInstancesDataKHR instances{};
    instances.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_INSTANCES_DATA_KHR;
    instances.arrayOfPointers = VK_FALSE;
    instances.data.deviceAddress = build.instanceBuffer->deviceAddress() + build.instanceDataOffset;

    VkAccelerationStructureGeometryKHR geometry{};
    geometry.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR;
    geometry.geometryType = VK_GEOMETRY_TYPE_INSTANCES_KHR;
    geometry.geometry.instances = instances;

    VkAccelerationStructureBuildGeometryInfoKHR buildInfo{};
    buildInfo.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR;
    buildInfo.type = VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR;
    buildInfo.flags = build.flags;
    buildInfo.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
    buildInfo.geometryCount = 1;
    buildInfo.pGeometries = &geometry;
    buildInfo.dstAccelerationStructure = build.destination->handle();
    buildInfo.scratchData.deviceAddress = Buffer::alignUp(build.scratch->deviceAddress(), accelerationStructureScratchAlignment_);

    VkAccelerationStructureBuildRangeInfoKHR range{};
    range.primitiveCount = build.instanceCount;
    const VkAccelerationStructureBuildRangeInfoKHR* ranges[] = {&range};

    VkCommandBuffer cmd = beginGraphicsBatch();
    vkCmdBuildAccelerationStructuresKHR(cmd, 1, &buildInfo, ranges);

    VkMemoryBarrier2 barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2;
    barrier.srcStageMask = VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR;
    barrier.srcAccessMask = VK_ACCESS_2_ACCELERATION_STRUCTURE_WRITE_BIT_KHR;
    barrier.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    barrier.dstAccessMask = VK_ACCESS_2_ACCELERATION_STRUCTURE_READ_BIT_KHR |
        VK_ACCESS_2_ACCELERATION_STRUCTURE_WRITE_BIT_KHR |
        VK_ACCESS_2_MEMORY_READ_BIT |
        VK_ACCESS_2_MEMORY_WRITE_BIT;
    VkDependencyInfo dependency{};
    dependency.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
    dependency.memoryBarrierCount = 1;
    dependency.pMemoryBarriers = &barrier;
    recordManualBarrierEscape("StreamingGpuTransferExecutor", "streaming_tlas_build", dependency);
    vkCmdPipelineBarrier2(cmd, &dependency);

    ++openGraphicsBatchOps_;
    ++totalTlasBuilds_;
    return true;
}

uint64_t StreamingGpuTransferExecutor::submitFrame() {
    if (device_ == VK_NULL_HANDLE || openBatch_ == VK_NULL_HANDLE || openBatchCopies_ == 0) {
        // Nothing recorded: discard an empty open batch if present.
        if (openBatch_ != VK_NULL_HANDLE) {
            vkEndCommandBuffer(openBatch_);
            freeCommandBuffers_.push_back(openBatch_);
            openBatch_ = VK_NULL_HANDLE;
            openBatchCopies_ = 0;
        }
        return 0;
    }
    checkVk(vkEndCommandBuffer(openBatch_), "vkEndCommandBuffer(streaming transfer)");

    const uint64_t signalValue = nextTimelineValue_++;
    const uint64_t waitValue = submittedTimeline_;

    VkCommandBufferSubmitInfo cmdInfo{};
    cmdInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO;
    cmdInfo.commandBuffer = openBatch_;

    VkSemaphoreSubmitInfo signalInfo{};
    signalInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO;
    signalInfo.semaphore = timeline_;
    signalInfo.value = signalValue;
    signalInfo.stageMask = VK_PIPELINE_STAGE_2_COPY_BIT;

    VkSemaphoreSubmitInfo waitInfo{};
    waitInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO;
    waitInfo.semaphore = timeline_;
    waitInfo.value = waitValue;
    waitInfo.stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;

    VkSubmitInfo2 submit{};
    submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2;
    submit.commandBufferInfoCount = 1;
    submit.pCommandBufferInfos = &cmdInfo;
    if (waitValue != 0) {
        submit.waitSemaphoreInfoCount = 1;
        submit.pWaitSemaphoreInfos = &waitInfo;
    }
    submit.signalSemaphoreInfoCount = 1;
    submit.pSignalSemaphoreInfos = &signalInfo;
    checkVk(vkQueueSubmit2(queue_, 1, &submit, VK_NULL_HANDLE), "vkQueueSubmit2(streaming transfer)");

    inFlight_.push_back(InFlightBatch{.timelineValue = signalValue, .commandBuffer = openBatch_});
    submittedTimeline_ = signalValue;
    ++totalSubmissions_;
    openBatch_ = VK_NULL_HANDLE;
    openBatchCopies_ = 0;
    return signalValue;
}

uint64_t StreamingGpuTransferExecutor::submitGraphicsFrame(uint64_t waitTimelineValue) {
    if (device_ == VK_NULL_HANDLE || openGraphicsBatch_ == VK_NULL_HANDLE || openGraphicsBatchOps_ == 0) {
        if (openGraphicsBatch_ != VK_NULL_HANDLE) {
            vkEndCommandBuffer(openGraphicsBatch_);
            freeGraphicsCommandBuffers_.push_back(openGraphicsBatch_);
            openGraphicsBatch_ = VK_NULL_HANDLE;
            openGraphicsBatchOps_ = 0;
        }
        return 0;
    }
    checkVk(vkEndCommandBuffer(openGraphicsBatch_), "vkEndCommandBuffer(streaming graphics)");

    const uint64_t signalValue = nextTimelineValue_++;
    const uint64_t orderedWaitTimelineValue = std::max(waitTimelineValue, submittedTimeline_);

    VkCommandBufferSubmitInfo cmdInfo{};
    cmdInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO;
    cmdInfo.commandBuffer = openGraphicsBatch_;

    VkSemaphoreSubmitInfo waitInfo{};
    waitInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO;
    waitInfo.semaphore = timeline_;
    waitInfo.value = orderedWaitTimelineValue;
    waitInfo.stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;

    VkSemaphoreSubmitInfo signalInfo{};
    signalInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO;
    signalInfo.semaphore = timeline_;
    signalInfo.value = signalValue;
    signalInfo.stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;

    VkSubmitInfo2 submit{};
    submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2;
    submit.commandBufferInfoCount = 1;
    submit.pCommandBufferInfos = &cmdInfo;
    if (orderedWaitTimelineValue != 0) {
        submit.waitSemaphoreInfoCount = 1;
        submit.pWaitSemaphoreInfos = &waitInfo;
    }
    submit.signalSemaphoreInfoCount = 1;
    submit.pSignalSemaphoreInfos = &signalInfo;
    checkVk(vkQueueSubmit2(graphicsQueue_, 1, &submit, VK_NULL_HANDLE), "vkQueueSubmit2(streaming graphics)");

    inFlight_.push_back(InFlightBatch{.timelineValue = signalValue, .commandBuffer = openGraphicsBatch_, .graphics = true});
    for (PendingCompactionQuery& query : openGraphicsCompactionQueries_) {
        query.timelineValue = signalValue;
        pendingCompactionQueries_.push_back(query);
    }
    openGraphicsCompactionQueries_.clear();
    submittedTimeline_ = signalValue;
    ++totalSubmissions_;
    openGraphicsBatch_ = VK_NULL_HANDLE;
    openGraphicsBatchOps_ = 0;
    return signalValue;
}

uint64_t StreamingGpuTransferExecutor::submitTimelineMarker() {
    if (device_ == VK_NULL_HANDLE || timeline_ == VK_NULL_HANDLE) {
        return 0;
    }
    uint64_t waitForTransferTimeline = 0;
    if (openBatch_ != VK_NULL_HANDLE && openBatchCopies_ > 0) {
        waitForTransferTimeline = submitFrame();
    }
    if (openGraphicsBatch_ != VK_NULL_HANDLE && openGraphicsBatchOps_ > 0) {
        const uint64_t graphicsTimeline = submitGraphicsFrame(waitForTransferTimeline);
        if (graphicsTimeline != 0) {
            return graphicsTimeline;
        }
    }
    if (waitForTransferTimeline != 0) {
        return waitForTransferTimeline;
    }
    // Discard an empty open batch if one was begun but nothing recorded.
    if (openBatch_ != VK_NULL_HANDLE) {
        vkEndCommandBuffer(openBatch_);
        freeCommandBuffers_.push_back(openBatch_);
        openBatch_ = VK_NULL_HANDLE;
        openBatchCopies_ = 0;
    }
    if (openGraphicsBatch_ != VK_NULL_HANDLE) {
        vkEndCommandBuffer(openGraphicsBatch_);
        freeGraphicsCommandBuffers_.push_back(openGraphicsBatch_);
        openGraphicsBatch_ = VK_NULL_HANDLE;
        openGraphicsBatchOps_ = 0;
    }

    // Submit an empty batch whose only purpose is to advance the device timeline
    // so streaming completion can be gated on real GPU progress (never faked).
    const uint64_t signalValue = nextTimelineValue_++;
    const uint64_t waitValue = submittedTimeline_;

    VkSemaphoreSubmitInfo waitInfo{};
    waitInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO;
    waitInfo.semaphore = timeline_;
    waitInfo.value = waitValue;
    waitInfo.stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;

    VkSemaphoreSubmitInfo signalInfo{};
    signalInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO;
    signalInfo.semaphore = timeline_;
    signalInfo.value = signalValue;
    signalInfo.stageMask = VK_PIPELINE_STAGE_2_COPY_BIT;

    VkSubmitInfo2 submit{};
    submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2;
    if (waitValue != 0) {
        submit.waitSemaphoreInfoCount = 1;
        submit.pWaitSemaphoreInfos = &waitInfo;
    }
    submit.signalSemaphoreInfoCount = 1;
    submit.pSignalSemaphoreInfos = &signalInfo;
    checkVk(vkQueueSubmit2(queue_, 1, &submit, VK_NULL_HANDLE), "vkQueueSubmit2(streaming transfer marker)");

    submittedTimeline_ = signalValue;
    ++totalSubmissions_;
    return signalValue;
}

void StreamingGpuTransferExecutor::recycleCompleted(uint64_t completedTimeline) {
    while (!inFlight_.empty() && inFlight_.front().timelineValue <= completedTimeline) {
        if (inFlight_.front().commandBuffer != VK_NULL_HANDLE) {
            if (inFlight_.front().graphics) {
                freeGraphicsCommandBuffers_.push_back(inFlight_.front().commandBuffer);
            } else {
                freeCommandBuffers_.push_back(inFlight_.front().commandBuffer);
            }
        }
        inFlight_.pop_front();
    }
    (void)stagingRing_.retire(completedTimeline);
    auto queryOut = pendingCompactionQueries_.begin();
    for (auto queryIt = pendingCompactionQueries_.begin(); queryIt != pendingCompactionQueries_.end(); ++queryIt) {
        if (queryIt->timelineValue <= completedTimeline) {
            uint64_t compactedSize = 0;
            const VkResult queryResult = vkGetQueryPoolResults(
                device_,
                queryIt->queryPool,
                0,
                1,
                sizeof(compactedSize),
                &compactedSize,
                sizeof(compactedSize),
                VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT);
            if (queryResult == VK_SUCCESS && queryIt->source != nullptr && compactedSize != 0) {
                auto found = std::find_if(
                    compactedBlasSizes_.begin(),
                    compactedBlasSizes_.end(),
                    [&](const auto& stored) { return stored.first == queryIt->source; });
                if (found != compactedBlasSizes_.end()) {
                    found->second = compactedSize;
                } else {
                    compactedBlasSizes_.emplace_back(queryIt->source, compactedSize);
                }
            }
            if (queryIt->queryPool != VK_NULL_HANDLE) {
                vkDestroyQueryPool(device_, queryIt->queryPool, nullptr);
            }
            continue;
        }
        if (queryOut != queryIt) {
            *queryOut = *queryIt;
        }
        ++queryOut;
    }
    pendingCompactionQueries_.erase(queryOut, pendingCompactionQueries_.end());
}

uint64_t StreamingGpuTransferExecutor::poll() {
    if (device_ == VK_NULL_HANDLE || timeline_ == VK_NULL_HANDLE) {
        return 0;
    }
    uint64_t transferValue = 0;
    if (vkGetSemaphoreCounterValue(device_, timeline_, &transferValue) == VK_SUCCESS) {
        completedTimeline_ = transferValue;
        recycleCompleted(transferValue);
    }
    if (computeTimeline_ != VK_NULL_HANDLE) {
        uint64_t computeValue = 0;
        if (vkGetSemaphoreCounterValue(device_, computeTimeline_, &computeValue) == VK_SUCCESS) {
            completedComputeTimeline_ = computeValue;
        }
    }
    return completedTimeline_;
}

bool StreamingGpuTransferExecutor::setComputeQueue(
    VkQueue queue,
    VkSemaphore timelineSemaphore,
    ComputeTimelineOwnership ownership) {
    if ((queue == VK_NULL_HANDLE) != (timelineSemaphore == VK_NULL_HANDLE) ||
        (timelineSemaphore != VK_NULL_HANDLE && timelineSemaphore == timeline_) ||
        (device_ == VK_NULL_HANDLE && queue != VK_NULL_HANDLE)) {
        return false;
    }

    uint64_t newCompletedTimeline = 0;
    if (device_ != VK_NULL_HANDLE && timelineSemaphore != VK_NULL_HANDLE &&
        vkGetSemaphoreCounterValue(device_, timelineSemaphore, &newCompletedTimeline) != VK_SUCCESS) {
        return false;
    }

    if (device_ != VK_NULL_HANDLE && computeTimeline_ != VK_NULL_HANDLE &&
        computeTimeline_ != timelineSemaphore && submittedComputeTimeline_ > completedComputeTimeline_) {
        VkSemaphoreWaitInfo waitInfo{};
        waitInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO;
        waitInfo.semaphoreCount = 1;
        waitInfo.pSemaphores = &computeTimeline_;
        waitInfo.pValues = &submittedComputeTimeline_;
        if (vkWaitSemaphores(device_, &waitInfo, UINT64_MAX) != VK_SUCCESS) {
            return false;
        }
        completedComputeTimeline_ = submittedComputeTimeline_;
    }

    if (device_ != VK_NULL_HANDLE && computeTimelineOwned_ &&
        computeTimeline_ != VK_NULL_HANDLE && computeTimeline_ != timelineSemaphore &&
        computeTimeline_ != timeline_) {
        vkDestroySemaphore(device_, computeTimeline_, nullptr);
    }

    const bool sameTimeline = computeTimeline_ == timelineSemaphore && timelineSemaphore != VK_NULL_HANDLE;
    computeQueue_ = queue;
    computeTimeline_ = timelineSemaphore;
    computeTimelineOwned_ = ownership == ComputeTimelineOwnership::Owned &&
        timelineSemaphore != VK_NULL_HANDLE && timelineSemaphore != timeline_;
    if (sameTimeline) {
        completedComputeTimeline_ = std::max(completedComputeTimeline_, newCompletedTimeline);
        nextComputeTimelineValue_ = std::max(nextComputeTimelineValue_, completedComputeTimeline_ + 1);
    } else {
        completedComputeTimeline_ = newCompletedTimeline;
        submittedComputeTimeline_ = newCompletedTimeline;
        nextComputeTimelineValue_ = newCompletedTimeline + 1;
    }
    return true;
}

uint64_t StreamingGpuTransferExecutor::submitComputeTimelineMarker(uint64_t waitTimelineValue) {
    if (device_ == VK_NULL_HANDLE || computeQueue_ == VK_NULL_HANDLE ||
        computeTimeline_ == VK_NULL_HANDLE || waitTimelineValue > submittedTimeline_) {
        return 0;
    }

    uint64_t currentComputeTimeline = 0;
    if (vkGetSemaphoreCounterValue(device_, computeTimeline_, &currentComputeTimeline) != VK_SUCCESS) {
        return 0;
    }
    completedComputeTimeline_ = std::max(completedComputeTimeline_, currentComputeTimeline);
    const uint64_t signalValue = std::max(nextComputeTimelineValue_, currentComputeTimeline + 1);

    // Wait on the transfer timeline.
    VkSemaphoreSubmitInfo waitInfo{};
    waitInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO;
    waitInfo.semaphore = timeline_;
    waitInfo.value = waitTimelineValue;
    waitInfo.stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;

    // Signal compute timeline.
    VkSemaphoreSubmitInfo signalInfo{};
    signalInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO;
    signalInfo.semaphore = computeTimeline_;
    signalInfo.value = signalValue;
    signalInfo.stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;

    VkSubmitInfo2 submit{};
    submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2;
    if (waitTimelineValue > 0) {
        submit.waitSemaphoreInfoCount = 1;
        submit.pWaitSemaphoreInfos = &waitInfo;
    }
    submit.signalSemaphoreInfoCount = 1;
    submit.pSignalSemaphoreInfos = &signalInfo;

    const VkResult result = vkQueueSubmit2(computeQueue_, 1, &submit, VK_NULL_HANDLE);
    if (result != VK_SUCCESS) {
        return 0;
    }

    nextComputeTimelineValue_ = signalValue + 1;
    submittedComputeTimeline_ = signalValue;
    ++totalSubmissions_;

    return signalValue;
}

StreamingGpuTransferExecutor::Stats StreamingGpuTransferExecutor::stats() const {
    Stats out;
    out.deviceBacked = device_ != VK_NULL_HANDLE;
    out.dedicatedTransferQueue = dedicatedTransferQueue_;
    out.transferQueueFamily = queueFamily_;
    out.submittedTimeline = submittedTimeline_;
    out.completedTimeline = completedTimeline_;
    const uint64_t pendingComputeMarkers = submittedComputeTimeline_ > completedComputeTimeline_
        ? submittedComputeTimeline_ - completedComputeTimeline_
        : 0;
    out.inFlightSubmissions = static_cast<uint32_t>(inFlight_.size() + pendingComputeMarkers);
    out.totalSubmissions = totalSubmissions_;
    out.totalBufferCopies = totalBufferCopies_;
    out.totalImageCopies = totalImageCopies_;
    out.totalBlasBuilds = totalBlasBuilds_;
    out.totalBlasCompactions = totalBlasCompactions_;
    out.totalTlasBuilds = totalTlasBuilds_;
    out.totalUploadedBytes = totalUploadedBytes_;
    out.stagingAllocationFailures = stagingAllocationFailures_;
    out.staging = stagingRing_.stats();
    return out;
}

bool StreamingGpuTransferExecutor::runSelfTest(std::string& errorOut) {
    if (device_ == VK_NULL_HANDLE || allocator_ == nullptr) {
        errorOut = "executor not initialized";
        return false;
    }
    constexpr uint64_t kBytes = 64ull * 1024ull;
    std::vector<uint8_t> pattern(static_cast<size_t>(kBytes));
    for (size_t i = 0; i < pattern.size(); ++i) {
        pattern[i] = static_cast<uint8_t>((i * 131u + 7u) & 0xFFu);
    }

    Buffer dst(*allocator_, BufferDesc{
        .size = kBytes,
        .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
        .memory = BufferMemory::GpuOnly,
        .debugName = "streaming transfer selftest dst",
    });

    if (!stageBufferUpload(dst, pattern.data(), kBytes, 0)) {
        errorOut = "stageBufferUpload failed (staging ring full)";
        return false;
    }
    const uint64_t signaled = submitFrame();
    if (signaled == 0) {
        errorOut = "submitFrame recorded no work";
        return false;
    }

    // Wait on the real device timeline - no vkDeviceWaitIdle.
    VkSemaphoreWaitInfo waitInfo{};
    waitInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO;
    waitInfo.semaphoreCount = 1;
    waitInfo.pSemaphores = &timeline_;
    waitInfo.pValues = &signaled;
    if (vkWaitSemaphores(device_, &waitInfo, UINT64_MAX) != VK_SUCCESS) {
        errorOut = "vkWaitSemaphores failed";
        return false;
    }
    const uint64_t completed = poll();
    if (completed < signaled) {
        errorOut = "timeline did not reach signaled value after wait";
        return false;
    }

    // Read back the destination via a readback staging buffer to validate bytes.
    Buffer readback(*allocator_, BufferDesc{
        .size = kBytes,
        .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT,
        .memory = BufferMemory::Readback,
        .persistentMapped = true,
        .debugName = "streaming transfer selftest readback",
    });
    VkCommandBuffer cmd = beginBatch();
    VkBufferCopy copy{};
    copy.size = kBytes;
    vkCmdCopyBuffer(cmd, dst.handle(), readback.handle(), 1, &copy);
    ++openBatchCopies_;
    const uint64_t readbackTimeline = submitFrame();
    VkSemaphoreWaitInfo rbWait{};
    rbWait.sType = VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO;
    rbWait.semaphoreCount = 1;
    rbWait.pSemaphores = &timeline_;
    rbWait.pValues = &readbackTimeline;
    if (vkWaitSemaphores(device_, &rbWait, UINT64_MAX) != VK_SUCCESS) {
        errorOut = "vkWaitSemaphores(readback) failed";
        return false;
    }
    (void)poll();
    readback.invalidate(kBytes);
    const uint8_t* readBytes = static_cast<const uint8_t*>(readback.mappedData());
    if (readBytes == nullptr) {
        errorOut = "readback buffer not mapped";
        return false;
    }
    if (std::memcmp(readBytes, pattern.data(), pattern.size()) != 0) {
        errorOut = "readback bytes did not match uploaded pattern";
        return false;
    }

    struct SelfTestMipUpload {
        uint32_t mipLevel = 0;
        uint32_t width = 1;
        uint32_t height = 1;
        VkDeviceSize readbackOffset = 0;
        std::vector<uint8_t> bytes;
    };
    std::array<SelfTestMipUpload, 3> mipUploads{{
        {.mipLevel = 2, .width = 1, .height = 1, .bytes = std::vector<uint8_t>(1u * 1u * 4u)},
        {.mipLevel = 0, .width = 4, .height = 4, .bytes = std::vector<uint8_t>(4u * 4u * 4u)},
        {.mipLevel = 1, .width = 2, .height = 2, .bytes = std::vector<uint8_t>(2u * 2u * 4u)},
    }};
    for (SelfTestMipUpload& upload : mipUploads) {
        for (size_t i = 0; i < upload.bytes.size(); ++i) {
            upload.bytes[i] = static_cast<uint8_t>(
                (i * 17u + static_cast<size_t>(upload.mipLevel) * 53u + 3u) & 0xFFu);
        }
    }
    constexpr VkDeviceSize kImageCopyOffsetAlignment = 16;
    VkDeviceSize imageReadbackBytes = 0;
    for (SelfTestMipUpload& upload : mipUploads) {
        upload.readbackOffset = Buffer::alignUp(imageReadbackBytes, kImageCopyOffsetAlignment);
        imageReadbackBytes = upload.readbackOffset + static_cast<VkDeviceSize>(upload.bytes.size());
    }
    Image image(*allocator_, ImageDesc{
        .width = 4,
        .height = 4,
        .depth = 1,
        .mipLevels = 3,
        .format = VK_FORMAT_R8G8B8A8_UNORM,
        .usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT |
            VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
            VK_IMAGE_USAGE_SAMPLED_BIT,
        .aspect = VK_IMAGE_ASPECT_COLOR_BIT,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
        .createDefaultView = true,
        .debugName = "streaming transfer selftest image",
    });
    uint64_t imageTimeline = 0;
    for (const SelfTestMipUpload& upload : mipUploads) {
        if (!stageImageMipUpload(
                image,
                upload.bytes.data(),
                upload.bytes.size(),
                upload.mipLevel,
                upload.width,
                upload.height)) {
            errorOut = "stageImageMipUpload(multi-mip) failed";
            return false;
        }
        const uint64_t mipTimeline = submitFrame();
        if (mipTimeline == 0 || mipTimeline <= imageTimeline) {
            errorOut = "multi-mip image uploads did not produce increasing timeline values";
            return false;
        }
        imageTimeline = mipTimeline;
    }
    VkSemaphoreWaitInfo imageWait{};
    imageWait.sType = VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO;
    imageWait.semaphoreCount = 1;
    imageWait.pSemaphores = &timeline_;
    imageWait.pValues = &imageTimeline;
    if (vkWaitSemaphores(device_, &imageWait, UINT64_MAX) != VK_SUCCESS) {
        errorOut = "vkWaitSemaphores(multi-mip image uploads) failed";
        return false;
    }
    if (poll() < imageTimeline) {
        errorOut = "multi-mip image upload timeline did not complete after final wait";
        return false;
    }

    Buffer imageReadback(*allocator_, BufferDesc{
        .size = imageReadbackBytes,
        .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT,
        .memory = BufferMemory::Readback,
        .persistentMapped = true,
        .debugName = "streaming transfer selftest image readback",
    });
    VkCommandBuffer imageReadbackCmd = beginBatch();
    std::array<VkBufferImageCopy, 3> imageCopies{};
    TrackedImageLayouts& imageTrackedLayouts = imageLayouts_[&image];
    for (size_t i = 0; i < mipUploads.size(); ++i) {
        const SelfTestMipUpload& upload = mipUploads[i];
        barrier::cmdTransitionImage(imageReadbackCmd, {
            .image = image.handle(),
            .oldLayout = imageTrackedLayouts.mips[upload.mipLevel],
            .newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
            .range = barrier::colorRange(upload.mipLevel, 1),
            .srcStage = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
            .srcAccess = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT,
            .dstStage = VK_PIPELINE_STAGE_2_COPY_BIT,
            .dstAccess = VK_ACCESS_2_TRANSFER_READ_BIT,
        });
        imageTrackedLayouts.mips[upload.mipLevel] = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;

        VkBufferImageCopy& imageCopy = imageCopies[i];
        imageCopy.bufferOffset = upload.readbackOffset;
        imageCopy.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        imageCopy.imageSubresource.mipLevel = upload.mipLevel;
        imageCopy.imageSubresource.baseArrayLayer = 0;
        imageCopy.imageSubresource.layerCount = 1;
        imageCopy.imageExtent = {upload.width, upload.height, 1};
    }
    vkCmdCopyImageToBuffer(
        imageReadbackCmd,
        image.handle(),
        VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
        imageReadback.handle(),
        static_cast<uint32_t>(imageCopies.size()),
        imageCopies.data());
    for (const SelfTestMipUpload& upload : mipUploads) {
        barrier::cmdTransitionImage(imageReadbackCmd, {
            .image = image.handle(),
            .oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
            .newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            .range = barrier::colorRange(upload.mipLevel, 1),
            .srcStage = VK_PIPELINE_STAGE_2_COPY_BIT,
            .srcAccess = VK_ACCESS_2_TRANSFER_READ_BIT,
            .dstStage = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
            .dstAccess = VK_ACCESS_2_MEMORY_READ_BIT,
        });
        imageTrackedLayouts.mips[upload.mipLevel] = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    }
    image.setLayout(VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    openBatchCopies_ += static_cast<uint32_t>(imageCopies.size());
    totalImageCopies_ += static_cast<uint32_t>(imageCopies.size());

    const uint64_t imageReadbackTimeline = submitFrame();
    if (imageReadbackTimeline <= imageTimeline) {
        errorOut = "multi-mip image readback did not produce a later timeline value";
        return false;
    }
    VkSemaphoreWaitInfo imageReadbackWait{};
    imageReadbackWait.sType = VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO;
    imageReadbackWait.semaphoreCount = 1;
    imageReadbackWait.pSemaphores = &timeline_;
    imageReadbackWait.pValues = &imageReadbackTimeline;
    if (vkWaitSemaphores(device_, &imageReadbackWait, UINT64_MAX) != VK_SUCCESS) {
        errorOut = "vkWaitSemaphores(multi-mip image readback) failed";
        return false;
    }
    if (poll() < imageReadbackTimeline) {
        errorOut = "multi-mip image readback timeline did not complete after wait";
        return false;
    }
    imageReadback.invalidate(imageReadbackBytes);
    const uint8_t* imageReadbackData = static_cast<const uint8_t*>(imageReadback.mappedData());
    if (imageReadbackData == nullptr) {
        errorOut = "multi-mip image readback buffer not mapped";
        return false;
    }
    for (const SelfTestMipUpload& upload : mipUploads) {
        if (std::memcmp(
                imageReadbackData + upload.readbackOffset,
                upload.bytes.data(),
                upload.bytes.size()) != 0) {
            errorOut = "multi-mip image readback bytes did not match uploaded pattern";
            return false;
        }
    }

    struct SelfTestVertex {
        float x = 0.0f;
        float y = 0.0f;
        float z = 0.0f;
    };
    const std::array<SelfTestVertex, 3> vertices{{
        {.x = 0.0f, .y = 0.0f, .z = 0.0f},
        {.x = 1.0f, .y = 0.0f, .z = 0.0f},
        {.x = 0.0f, .y = 1.0f, .z = 0.0f},
    }};
    const std::array<uint32_t, 3> indices{{0u, 1u, 2u}};
    Buffer blasVertices(*allocator_, BufferDesc{
        .size = sizeof(SelfTestVertex) * vertices.size(),
        .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT |
            VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT |
            VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR,
        .memory = BufferMemory::GpuOnly,
        .debugName = "streaming transfer selftest BLAS vertices",
    });
    Buffer blasIndices(*allocator_, BufferDesc{
        .size = sizeof(uint32_t) * indices.size(),
        .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT |
            VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT |
            VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR,
        .memory = BufferMemory::GpuOnly,
        .debugName = "streaming transfer selftest BLAS indices",
    });
    if (!stageBufferUpload(blasVertices, vertices.data(), sizeof(SelfTestVertex) * vertices.size(), 0) ||
        !stageBufferUpload(blasIndices, indices.data(), sizeof(uint32_t) * indices.size(), 0)) {
        errorOut = "stageBufferUpload(BLAS inputs) failed";
        return false;
    }

    VkAccelerationStructureGeometryTrianglesDataKHR triangles{};
    triangles.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_TRIANGLES_DATA_KHR;
    triangles.vertexFormat = VK_FORMAT_R32G32B32_SFLOAT;
    triangles.vertexData.deviceAddress = blasVertices.deviceAddress();
    triangles.vertexStride = sizeof(SelfTestVertex);
    triangles.maxVertex = static_cast<uint32_t>(vertices.size() - 1u);
    triangles.indexType = VK_INDEX_TYPE_UINT32;
    triangles.indexData.deviceAddress = blasIndices.deviceAddress();

    VkAccelerationStructureGeometryKHR geometry{};
    geometry.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR;
    geometry.geometryType = VK_GEOMETRY_TYPE_TRIANGLES_KHR;
    geometry.flags = VK_GEOMETRY_OPAQUE_BIT_KHR;
    geometry.geometry.triangles = triangles;

    VkAccelerationStructureBuildGeometryInfoKHR blasSizeInfo{};
    blasSizeInfo.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR;
    blasSizeInfo.type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
    blasSizeInfo.flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR |
        VK_BUILD_ACCELERATION_STRUCTURE_ALLOW_COMPACTION_BIT_KHR;
    blasSizeInfo.geometryCount = 1;
    blasSizeInfo.pGeometries = &geometry;
    const uint32_t primitiveCount = 1;
    VkAccelerationStructureBuildSizesInfoKHR blasSizes{};
    blasSizes.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR;
    vkGetAccelerationStructureBuildSizesKHR(
        device_,
        VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR,
        &blasSizeInfo,
        &primitiveCount,
        &blasSizes);
    AccelerationStructure blas(device_, *allocator_, AccelerationStructureDesc{
        .type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR,
        .size = blasSizes.accelerationStructureSize,
        .debugName = "streaming transfer selftest BLAS",
    });
    Buffer blasScratch(*allocator_, BufferDesc{
        .size = blasSizes.buildScratchSize + accelerationStructureScratchAlignment_,
        .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
        .memory = BufferMemory::GpuOnly,
        .debugName = "streaming transfer selftest BLAS scratch",
    });
    if (!stageBlasBuild(BlasTriangleBuild{
            .destination = &blas,
            .scratch = &blasScratch,
            .vertexBuffer = &blasVertices,
            .indexBuffer = &blasIndices,
            .vertexCount = static_cast<uint32_t>(vertices.size()),
            .indexCount = static_cast<uint32_t>(indices.size()),
            .vertexStride = sizeof(SelfTestVertex),
            .flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR |
                VK_BUILD_ACCELERATION_STRUCTURE_ALLOW_COMPACTION_BIT_KHR,
        })) {
        errorOut = "stageBlasBuild failed";
        return false;
    }
    const uint64_t blasTimeline = submitTimelineMarker();
    if (blasTimeline == 0) {
        errorOut = "BLAS build did not produce a timeline value";
        return false;
    }

    std::array<uint8_t, 256> postBlasPattern{};
    for (size_t i = 0; i < postBlasPattern.size(); ++i) {
        postBlasPattern[i] = static_cast<uint8_t>((i * 29u + 11u) & 0xFFu);
    }
    Buffer postBlasTransfer(*allocator_, BufferDesc{
        .size = postBlasPattern.size(),
        .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT,
        .memory = BufferMemory::GpuOnly,
        .debugName = "streaming transfer selftest post-BLAS transfer",
    });
    if (!stageBufferUpload(postBlasTransfer, postBlasPattern.data(), postBlasPattern.size(), 0)) {
        errorOut = "stageBufferUpload(post-BLAS transfer) failed";
        return false;
    }
    const uint64_t orderedTransferTimeline = submitFrame();
    if (orderedTransferTimeline <= blasTimeline) {
        errorOut = "post-BLAS transfer did not follow the graphics timeline value";
        return false;
    }
    VkSemaphoreWaitInfo orderedWait{};
    orderedWait.sType = VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO;
    orderedWait.semaphoreCount = 1;
    orderedWait.pSemaphores = &timeline_;
    orderedWait.pValues = &orderedTransferTimeline;
    if (vkWaitSemaphores(device_, &orderedWait, UINT64_MAX) != VK_SUCCESS) {
        errorOut = "vkWaitSemaphores(transfer-BLAS-transfer chain) failed";
        return false;
    }
    if (poll() < orderedTransferTimeline) {
        errorOut = "transfer-BLAS-transfer timeline did not complete after final wait";
        return false;
    }
    const uint64_t compactedBlasBytes = consumeCompactedBlasSize(blas);
    if (compactedBlasBytes == 0) {
        errorOut = "BLAS compaction-size query was unavailable after the ordered final transfer";
        return false;
    }
    AccelerationStructure compactedBlas;
    const AccelerationStructure* tlasBlas = &blas;
    if (compactedBlasBytes != 0 && compactedBlasBytes < static_cast<uint64_t>(blas.size())) {
        compactedBlas.create(device_, *allocator_, AccelerationStructureDesc{
            .type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR,
            .size = compactedBlasBytes,
            .debugName = "streaming transfer selftest compacted BLAS",
        });
        if (!stageBlasCompaction(blas, compactedBlas)) {
            errorOut = "stageBlasCompaction failed";
            return false;
        }
        const uint64_t compactTimeline = submitTimelineMarker();
        VkSemaphoreWaitInfo compactWait{};
        compactWait.sType = VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO;
        compactWait.semaphoreCount = 1;
        compactWait.pSemaphores = &timeline_;
        compactWait.pValues = &compactTimeline;
        if (compactTimeline == 0 || vkWaitSemaphores(device_, &compactWait, UINT64_MAX) != VK_SUCCESS) {
            errorOut = "vkWaitSemaphores(BLAS compaction) failed";
            return false;
        }
        if (poll() < compactTimeline) {
            errorOut = "BLAS compaction timeline did not complete after wait";
            return false;
        }
        tlasBlas = &compactedBlas;
    }

    VkAccelerationStructureInstanceKHR instance{};
    instance.transform.matrix[0][0] = 1.0f;
    instance.transform.matrix[1][1] = 1.0f;
    instance.transform.matrix[2][2] = 1.0f;
    instance.instanceCustomIndex = 0;
    instance.mask = 0xFF;
    instance.instanceShaderBindingTableRecordOffset = 0;
    instance.flags = VK_GEOMETRY_INSTANCE_TRIANGLE_FACING_CULL_DISABLE_BIT_KHR;
    instance.accelerationStructureReference = tlasBlas->deviceAddress();

    Buffer instanceUpload(*allocator_, BufferDesc{
        .size = sizeof(VkAccelerationStructureInstanceKHR),
        .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT |
            VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT |
            VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR,
        .memory = BufferMemory::GpuOnly,
        .debugName = "streaming transfer selftest TLAS instance",
    });
    if (!stageBufferUpload(instanceUpload, &instance, sizeof(instance), 0)) {
        errorOut = "stageBufferUpload(TLAS instance) failed";
        return false;
    }

    VkAccelerationStructureGeometryInstancesDataKHR tlasInstances{};
    tlasInstances.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_INSTANCES_DATA_KHR;
    tlasInstances.arrayOfPointers = VK_FALSE;
    tlasInstances.data.deviceAddress = instanceUpload.deviceAddress();

    VkAccelerationStructureGeometryKHR tlasGeometry{};
    tlasGeometry.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR;
    tlasGeometry.geometryType = VK_GEOMETRY_TYPE_INSTANCES_KHR;
    tlasGeometry.geometry.instances = tlasInstances;

    VkAccelerationStructureBuildGeometryInfoKHR tlasSizeInfo{};
    tlasSizeInfo.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR;
    tlasSizeInfo.type = VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR;
    tlasSizeInfo.flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR |
        VK_BUILD_ACCELERATION_STRUCTURE_ALLOW_UPDATE_BIT_KHR;
    tlasSizeInfo.geometryCount = 1;
    tlasSizeInfo.pGeometries = &tlasGeometry;
    const uint32_t tlasPrimitiveCount = 1;
    VkAccelerationStructureBuildSizesInfoKHR tlasSizes{};
    tlasSizes.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR;
    vkGetAccelerationStructureBuildSizesKHR(
        device_,
        VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR,
        &tlasSizeInfo,
        &tlasPrimitiveCount,
        &tlasSizes);
    AccelerationStructure tlas(device_, *allocator_, AccelerationStructureDesc{
        .type = VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR,
        .size = tlasSizes.accelerationStructureSize,
        .allowUpdate = true,
        .debugName = "streaming transfer selftest TLAS",
    });
    Buffer tlasScratch(*allocator_, BufferDesc{
        .size = tlasSizes.buildScratchSize + accelerationStructureScratchAlignment_,
        .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
        .memory = BufferMemory::GpuOnly,
        .debugName = "streaming transfer selftest TLAS scratch",
    });
    if (!stageTlasBuild(TlasBuild{
            .destination = &tlas,
            .scratch = &tlasScratch,
            .instanceBuffer = &instanceUpload,
            .instanceCount = 1,
        })) {
        errorOut = "stageTlasBuild failed";
        return false;
    }
    const uint64_t tlasTimeline = submitTimelineMarker();
    VkSemaphoreWaitInfo tlasWait{};
    tlasWait.sType = VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO;
    tlasWait.semaphoreCount = 1;
    tlasWait.pSemaphores = &timeline_;
    tlasWait.pValues = &tlasTimeline;
    if (tlasTimeline == 0 || vkWaitSemaphores(device_, &tlasWait, UINT64_MAX) != VK_SUCCESS) {
        errorOut = "vkWaitSemaphores(TLAS build) failed";
        return false;
    }
    if (poll() < tlasTimeline) {
        errorOut = "TLAS build timeline did not complete after wait";
        return false;
    }

    const uint64_t markerTimeline = submitTimelineMarker();
    if (markerTimeline == 0) {
        errorOut = "submitTimelineMarker failed";
        return false;
    }
    VkSemaphoreWaitInfo markerWait{};
    markerWait.sType = VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO;
    markerWait.semaphoreCount = 1;
    markerWait.pSemaphores = &timeline_;
    markerWait.pValues = &markerTimeline;
    if (vkWaitSemaphores(device_, &markerWait, UINT64_MAX) != VK_SUCCESS) {
        errorOut = "vkWaitSemaphores(marker) failed";
        return false;
    }
    if (poll() < markerTimeline) {
        errorOut = "timeline marker did not complete after wait";
        return false;
    }

    if (!hasAsyncComputeQueue()) {
        VkSemaphoreTypeCreateInfo computeTimelineInfo{};
        computeTimelineInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO;
        computeTimelineInfo.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
        computeTimelineInfo.initialValue = 0;
        VkSemaphoreCreateInfo computeSemaphoreInfo{};
        computeSemaphoreInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
        computeSemaphoreInfo.pNext = &computeTimelineInfo;
        VkSemaphore selfTestComputeTimeline = VK_NULL_HANDLE;
        if (vkCreateSemaphore(device_, &computeSemaphoreInfo, nullptr, &selfTestComputeTimeline) != VK_SUCCESS) {
            errorOut = "vkCreateSemaphore(compute self-test timeline) failed";
            return false;
        }
        if (!setComputeQueue(
                graphicsQueue_,
                selfTestComputeTimeline,
                ComputeTimelineOwnership::Owned)) {
            vkDestroySemaphore(device_, selfTestComputeTimeline, nullptr);
            errorOut = "setComputeQueue(compute self-test timeline) failed";
            return false;
        }
    }
    const size_t transferInFlightBeforeCompute = inFlight_.size();
    const uint64_t computeTimelineValue = submitComputeTimelineMarker(markerTimeline);
    if (computeTimelineValue == 0) {
        errorOut = "submitComputeTimelineMarker failed";
        return false;
    }
    VkSemaphoreWaitInfo computeWait{};
    computeWait.sType = VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO;
    computeWait.semaphoreCount = 1;
    computeWait.pSemaphores = &computeTimeline_;
    computeWait.pValues = &computeTimelineValue;
    if (vkWaitSemaphores(device_, &computeWait, UINT64_MAX) != VK_SUCCESS) {
        errorOut = "vkWaitSemaphores(compute self-test) failed";
        return false;
    }
    (void)poll();
    if (completedComputeTimeline_ < computeTimelineValue) {
        errorOut = "compute timeline marker did not complete on its own semaphore";
        return false;
    }
    if (inFlight_.size() != transferInFlightBeforeCompute ||
        std::find(freeGraphicsCommandBuffers_.begin(), freeGraphicsCommandBuffers_.end(), VK_NULL_HANDLE) !=
            freeGraphicsCommandBuffers_.end()) {
        errorOut = "compute completion corrupted transfer/graphics in-flight tracking";
        return false;
    }

    StreamingStagingRing cpuRing(1024u, 1u);
    const std::optional<StreamingStagingAllocation> a = cpuRing.allocate(700, 1);
    const std::optional<StreamingStagingAllocation> b = cpuRing.allocate(200, 2);
    if (!a.has_value() || !b.has_value()) {
        errorOut = "CPU staging ring setup allocation failed";
        return false;
    }
    (void)cpuRing.retire(1);
    const std::optional<StreamingStagingAllocation> c = cpuRing.allocate(300, 3);
    if (!c.has_value() || c->offset != 0) {
        errorOut = "CPU staging ring wrap allocation failed";
        return false;
    }
    (void)cpuRing.retire(3);
    const StreamingStagingRingStats cpuStats = cpuRing.stats();
    if (cpuStats.inFlightBytes != 0 || cpuStats.freeBytes != cpuStats.capacityBytes || cpuStats.liveAllocationCount != 0) {
        errorOut = "CPU staging ring did not fully drain after wrapped allocations";
        return false;
    }
    return true;
}

nlohmann::json streamingGpuTransferExecutorStatsJson(const StreamingGpuTransferExecutor::Stats& stats) {
    return {
        {"device_backed", stats.deviceBacked},
        {"dedicated_transfer_queue", stats.dedicatedTransferQueue},
        {"transfer_queue_family", stats.transferQueueFamily == UINT32_MAX ? nlohmann::json(nullptr) : nlohmann::json(stats.transferQueueFamily)},
        {"submitted_timeline", stats.submittedTimeline},
        {"completed_timeline", stats.completedTimeline},
        {"in_flight_submissions", stats.inFlightSubmissions},
        {"total_submissions", stats.totalSubmissions},
        {"total_buffer_copies", stats.totalBufferCopies},
        {"total_image_copies", stats.totalImageCopies},
        {"total_blas_builds", stats.totalBlasBuilds},
        {"total_blas_compactions", stats.totalBlasCompactions},
        {"total_tlas_builds", stats.totalTlasBuilds},
        {"total_uploaded_bytes", stats.totalUploadedBytes},
        {"staging_allocation_failures", stats.stagingAllocationFailures},
        {"staging_ring", streamingStagingRingStatsJson(stats.staging)},
    };
}

} // namespace rtv
