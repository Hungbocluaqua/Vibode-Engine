#include "rtv/StreamingGpuTransferExecutor.h"

#include "rtv/Buffer.h"
#include "rtv/Check.h"
#include "rtv/Image.h"
#include "rtv/ImageBarrier.h"
#include "rtv/ResourceAllocator.h"
#include "rtv/VulkanContext.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <cstring>
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

    VkCommandPoolCreateInfo poolInfo{};
    poolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    poolInfo.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT | VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    poolInfo.queueFamilyIndex = queueFamily_;
    if (vkCreateCommandPool(device_, &poolInfo, nullptr, &commandPool_) != VK_SUCCESS) {
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
    if (timeline_ != VK_NULL_HANDLE) {
        vkDestroySemaphore(device_, timeline_, nullptr);
        timeline_ = VK_NULL_HANDLE;
    }
    if (commandPool_ != VK_NULL_HANDLE) {
        vkDestroyCommandPool(device_, commandPool_, nullptr);
        commandPool_ = VK_NULL_HANDLE;
    }
    stagingRing_ = StreamingStagingRing();
    inFlight_.clear();
    freeCommandBuffers_.clear();
    openBatch_ = VK_NULL_HANDLE;
    openBatchCopies_ = 0;
    device_ = VK_NULL_HANDLE;
    allocator_ = nullptr;
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

    VkCommandBuffer cmd = beginBatch();
    barrier::cmdTransitionImage(cmd, {
        .image = destination.handle(),
        .oldLayout = destination.layout(),
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
    destination.setLayout(VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);

    ++openBatchCopies_;
    ++totalImageCopies_;
    totalUploadedBytes_ += bytes;
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

    VkCommandBufferSubmitInfo cmdInfo{};
    cmdInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO;
    cmdInfo.commandBuffer = openBatch_;

    VkSemaphoreSubmitInfo signalInfo{};
    signalInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO;
    signalInfo.semaphore = timeline_;
    signalInfo.value = signalValue;
    signalInfo.stageMask = VK_PIPELINE_STAGE_2_COPY_BIT;

    VkSubmitInfo2 submit{};
    submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2;
    submit.commandBufferInfoCount = 1;
    submit.pCommandBufferInfos = &cmdInfo;
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

uint64_t StreamingGpuTransferExecutor::submitTimelineMarker() {
    if (device_ == VK_NULL_HANDLE || timeline_ == VK_NULL_HANDLE) {
        return 0;
    }
    // If copies are already recorded, their submission timeline is the marker
    // for that batch. Callers that need a distinct no-op marker should call
    // again after the batch has been submitted.
    if (openBatch_ != VK_NULL_HANDLE && openBatchCopies_ > 0) {
        return submitFrame();
    }
    // Discard an empty open batch if one was begun but nothing recorded.
    if (openBatch_ != VK_NULL_HANDLE) {
        vkEndCommandBuffer(openBatch_);
        freeCommandBuffers_.push_back(openBatch_);
        openBatch_ = VK_NULL_HANDLE;
        openBatchCopies_ = 0;
    }

    // Submit an empty batch whose only purpose is to advance the device timeline
    // so streaming completion can be gated on real GPU progress (never faked).
    const uint64_t signalValue = nextTimelineValue_++;

    VkSemaphoreSubmitInfo signalInfo{};
    signalInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO;
    signalInfo.semaphore = timeline_;
    signalInfo.value = signalValue;
    signalInfo.stageMask = VK_PIPELINE_STAGE_2_COPY_BIT;

    VkSubmitInfo2 submit{};
    submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2;
    submit.signalSemaphoreInfoCount = 1;
    submit.pSignalSemaphoreInfos = &signalInfo;
    checkVk(vkQueueSubmit2(queue_, 1, &submit, VK_NULL_HANDLE), "vkQueueSubmit2(streaming transfer marker)");

    submittedTimeline_ = signalValue;
    ++totalSubmissions_;
    return signalValue;
}

void StreamingGpuTransferExecutor::recycleCompleted(uint64_t completedTimeline) {
    while (!inFlight_.empty() && inFlight_.front().timelineValue <= completedTimeline) {
        freeCommandBuffers_.push_back(inFlight_.front().commandBuffer);
        inFlight_.pop_front();
    }
    (void)stagingRing_.retire(completedTimeline);
}

uint64_t StreamingGpuTransferExecutor::poll() {
    if (device_ == VK_NULL_HANDLE || timeline_ == VK_NULL_HANDLE) {
        return 0;
    }
    uint64_t value = 0;
    if (vkGetSemaphoreCounterValue(device_, timeline_, &value) != VK_SUCCESS) {
        return completedTimeline_;
    }
    completedTimeline_ = value;
    recycleCompleted(value);
    return value;
}

StreamingGpuTransferExecutor::Stats StreamingGpuTransferExecutor::stats() const {
    Stats out;
    out.deviceBacked = device_ != VK_NULL_HANDLE;
    out.dedicatedTransferQueue = dedicatedTransferQueue_;
    out.transferQueueFamily = queueFamily_;
    out.submittedTimeline = submittedTimeline_;
    out.completedTimeline = completedTimeline_;
    out.inFlightSubmissions = static_cast<uint32_t>(inFlight_.size());
    out.totalSubmissions = totalSubmissions_;
    out.totalBufferCopies = totalBufferCopies_;
    out.totalImageCopies = totalImageCopies_;
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

    std::array<uint8_t, 4u * 4u * 4u> imagePattern{};
    for (size_t i = 0; i < imagePattern.size(); ++i) {
        imagePattern[i] = static_cast<uint8_t>((i * 17u + 3u) & 0xFFu);
    }
    Image image(*allocator_, ImageDesc{
        .width = 4,
        .height = 4,
        .depth = 1,
        .mipLevels = 1,
        .format = VK_FORMAT_R8G8B8A8_UNORM,
        .usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
        .aspect = VK_IMAGE_ASPECT_COLOR_BIT,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
        .createDefaultView = true,
        .debugName = "streaming transfer selftest image",
    });
    if (!stageImageMipUpload(image, imagePattern.data(), imagePattern.size(), 0, 4, 4)) {
        errorOut = "stageImageMipUpload failed";
        return false;
    }
    const uint64_t imageTimeline = submitFrame();
    VkSemaphoreWaitInfo imageWait{};
    imageWait.sType = VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO;
    imageWait.semaphoreCount = 1;
    imageWait.pSemaphores = &timeline_;
    imageWait.pValues = &imageTimeline;
    if (vkWaitSemaphores(device_, &imageWait, UINT64_MAX) != VK_SUCCESS) {
        errorOut = "vkWaitSemaphores(image upload) failed";
        return false;
    }
    if (poll() < imageTimeline) {
        errorOut = "image upload timeline did not complete after wait";
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
        {"total_uploaded_bytes", stats.totalUploadedBytes},
        {"staging_allocation_failures", stats.stagingAllocationFailures},
        {"staging_ring", streamingStagingRingStatsJson(stats.staging)},
    };
}

} // namespace rtv
