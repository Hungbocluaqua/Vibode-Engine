#include "rtv/CommandSystem.h"

#include "rtv/Check.h"
#include "rtv/PathTracerRenderer.h"
#include "rtv/PipelineDemo.h"
#include "rtv/ResourceDemo.h"
#include "rtv/Swapchain.h"
#include "rtv/UiOverlay.h"
#include "rtv/VulkanContext.h"

#include <array>
#include <algorithm>
#include <cmath>
#include <iostream>

namespace rtv {

CommandSystem::CommandSystem(const VulkanContext& context, Swapchain& swapchain, bool disableAsyncCompute, bool singleQueueFallback)
    : context_(context),
      swapchain_(swapchain),
      asyncComputeEnabled_(!disableAsyncCompute && !singleQueueFallback &&
          context.computeQueue() != VK_NULL_HANDLE &&
          context.hasIndependentComputeQueue() &&
          context.queueFamilies().compute.has_value() &&
          context.supportsTimelineSemaphore()) {
    createFrameResources();
    createPresentSemaphores();
    if (uiOverlay_ != nullptr) {
        uiOverlay_->onSwapchainRecreated(swapchain_);
    }
}

CommandSystem::~CommandSystem() {
    waitIdle();
    destroyPresentSemaphores();
    destroyFrameResources();
}

void CommandSystem::drawFrame(float clearPhase, float deltaSeconds) {
    FrameResources& frame = frames_[frameIndex_];
    checkVk(vkWaitForFences(context_.device(), 1, &frame.inFlight, VK_TRUE, UINT64_MAX), "vkWaitForFences");

    uint32_t imageIndex = 0;
    VkResult acquireResult = VK_SUCCESS;

    if (headless_) {
        imageIndex = headlessImageIndex_;
        headlessImageIndex_ = (headlessImageIndex_ + 1) % swapchain_.imageCount();
    } else {
        acquireResult = vkAcquireNextImageKHR(
            context_.device(),
            swapchain_.handle(),
            UINT64_MAX,
            frame.imageAvailable,
            VK_NULL_HANDLE,
            &imageIndex);

        if (acquireResult == VK_ERROR_OUT_OF_DATE_KHR) {
            recreateSwapchainResources();
            return;
        }
        checkVk(acquireResult, "vkAcquireNextImageKHR");
    }

    checkVk(vkResetFences(context_.device(), 1, &frame.inFlight), "vkResetFences");
    checkVk(vkResetCommandPool(context_.device(), frame.commandPool, 0), "vkResetCommandPool");
    if (frame.computeCommandPool != VK_NULL_HANDLE) {
        checkVk(vkResetCommandPool(context_.device(), frame.computeCommandPool, 0), "vkResetCommandPool(compute)");
    }
    if (pathTracer_ != nullptr) {
        pathTracer_->refreshMemoryPressureQuality();
        VkExtent2D displayExtent = swapchain_.extent();
        if (uiOverlay_ != nullptr) {
            displayExtent = uiOverlay_->desiredRenderExtent(displayExtent);
        }
        const float scale = pathTracer_->effectiveRenderResolutionScale();
        VkExtent2D renderExtent = displayExtent;
        renderExtent.width = std::max(1u, static_cast<uint32_t>(static_cast<float>(renderExtent.width) * scale));
        renderExtent.height = std::max(1u, static_cast<uint32_t>(static_cast<float>(renderExtent.height) * scale));
        const VkExtent2D currentRenderExtent = pathTracer_->renderExtent();
        const VkExtent2D currentDisplayExtent = pathTracer_->displayExtent();
        if (uiOverlay_ != nullptr &&
            (currentRenderExtent.width != 0 || currentDisplayExtent.width != 0) &&
            (currentRenderExtent.width != renderExtent.width ||
             currentRenderExtent.height != renderExtent.height ||
             currentDisplayExtent.width != displayExtent.width ||
             currentDisplayExtent.height != displayExtent.height)) {
            uiOverlay_->invalidateViewportTexture();
        }
        pathTracer_->setFrameDeltaSeconds(deltaSeconds);
        pathTracer_->beginFrame(frameIndex_, renderExtent, displayExtent);
    } else if (pipelineDemo_ != nullptr) {
        pipelineDemo_->beginFrame(frameIndex_);
    }
    recordWorkCommands(frame.commandBuffer, imageIndex, clearPhase);
    const bool asyncComputeRecorded = recordAsyncComputeCommands(frame);
    recordPresentationCommands(frame.postCommandBuffer, imageIndex, clearPhase);
    submitFrame(frame, imageIndex, asyncComputeRecorded);

    if (headless_) {
        frameIndex_ = (frameIndex_ + 1) % framesInFlight;
        return;
    }

    VkSwapchainKHR swapchainHandle = swapchain_.handle();
    VkPresentInfoKHR presentInfo{};
    presentInfo.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
    presentInfo.waitSemaphoreCount = 1;
    presentInfo.pWaitSemaphores = &imageRenderFinished_.at(imageIndex);
    presentInfo.swapchainCount = 1;
    presentInfo.pSwapchains = &swapchainHandle;
    presentInfo.pImageIndices = &imageIndex;

    VkResult presentResult = vkQueuePresentKHR(context_.presentQueue(), &presentInfo);
    if (presentResult == VK_ERROR_OUT_OF_DATE_KHR || presentResult == VK_SUBOPTIMAL_KHR) {
        recreateSwapchainResources();
    } else {
        checkVk(presentResult, "vkQueuePresentKHR");
    }

    frameIndex_ = (frameIndex_ + 1) % framesInFlight;
}

void CommandSystem::waitIdle() const {
    if (context_.device() != VK_NULL_HANDLE) {
        std::cerr << "Device idle wait: CommandSystem shutdown/diagnostic drain\n";
        checkVk(vkDeviceWaitIdle(context_.device()), "vkDeviceWaitIdle(CommandSystem)");
    }
}

void CommandSystem::waitForFrameFences() const {
    std::array<VkFence, framesInFlight> fences{};
    uint32_t count = 0;
    for (const FrameResources& frame : frames_) {
        if (frame.inFlight != VK_NULL_HANDLE) {
            fences[count++] = frame.inFlight;
        }
    }
    if (count > 0) {
        checkVk(vkWaitForFences(context_.device(), count, fences.data(), VK_TRUE, UINT64_MAX), "vkWaitForFences(CommandSystem frames)");
    }
}

void CommandSystem::createFrameResources() {
    for (FrameResources& frame : frames_) {
        VkCommandPoolCreateInfo poolInfo{};
        poolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
        poolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        poolInfo.queueFamilyIndex = context_.queueFamilies().graphics.value();
        checkVk(vkCreateCommandPool(context_.device(), &poolInfo, nullptr, &frame.commandPool), "vkCreateCommandPool");

        VkCommandBufferAllocateInfo allocateInfo{};
        allocateInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        allocateInfo.commandPool = frame.commandPool;
        allocateInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        allocateInfo.commandBufferCount = 1;
        checkVk(vkAllocateCommandBuffers(context_.device(), &allocateInfo, &frame.commandBuffer), "vkAllocateCommandBuffers");
        checkVk(vkAllocateCommandBuffers(context_.device(), &allocateInfo, &frame.postCommandBuffer), "vkAllocateCommandBuffers(post)");

        if (asyncComputeEnabled_) {
            VkCommandPoolCreateInfo computePoolInfo{};
            computePoolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
            computePoolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
            computePoolInfo.queueFamilyIndex = context_.queueFamilies().compute.value();
            checkVk(vkCreateCommandPool(context_.device(), &computePoolInfo, nullptr, &frame.computeCommandPool),
                    "vkCreateCommandPool(compute)");

            VkCommandBufferAllocateInfo computeAllocateInfo{};
            computeAllocateInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
            computeAllocateInfo.commandPool = frame.computeCommandPool;
            computeAllocateInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
            computeAllocateInfo.commandBufferCount = 1;
            checkVk(vkAllocateCommandBuffers(context_.device(), &computeAllocateInfo, &frame.computeCommandBuffer),
                    "vkAllocateCommandBuffers(compute)");
        }

        VkSemaphoreCreateInfo semaphoreInfo{};
        semaphoreInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
        checkVk(vkCreateSemaphore(context_.device(), &semaphoreInfo, nullptr, &frame.imageAvailable), "vkCreateSemaphore(imageAvailable)");

        VkFenceCreateInfo fenceInfo{};
        fenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
        fenceInfo.flags = VK_FENCE_CREATE_SIGNALED_BIT;
        checkVk(vkCreateFence(context_.device(), &fenceInfo, nullptr, &frame.inFlight), "vkCreateFence");
    }
}

void CommandSystem::createPresentSemaphores() {
    imageRenderFinished_.resize(swapchain_.imageCount());
    for (VkSemaphore& semaphore : imageRenderFinished_) {
        VkSemaphoreCreateInfo semaphoreInfo{};
        semaphoreInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
        checkVk(vkCreateSemaphore(context_.device(), &semaphoreInfo, nullptr, &semaphore), "vkCreateSemaphore(imageRenderFinished)");
    }
}

void CommandSystem::destroyFrameResources() {
    for (FrameResources& frame : frames_) {
        if (frame.inFlight != VK_NULL_HANDLE) {
            vkDestroyFence(context_.device(), frame.inFlight, nullptr);
        }
        if (frame.imageAvailable != VK_NULL_HANDLE) {
            vkDestroySemaphore(context_.device(), frame.imageAvailable, nullptr);
        }
        if (frame.commandPool != VK_NULL_HANDLE) {
            vkDestroyCommandPool(context_.device(), frame.commandPool, nullptr);
        }
        if (frame.computeCommandPool != VK_NULL_HANDLE) {
            vkDestroyCommandPool(context_.device(), frame.computeCommandPool, nullptr);
        }
        frame = {};
    }
}

void CommandSystem::destroyPresentSemaphores() {
    for (VkSemaphore semaphore : imageRenderFinished_) {
        if (semaphore != VK_NULL_HANDLE) {
            vkDestroySemaphore(context_.device(), semaphore, nullptr);
        }
    }
    imageRenderFinished_.clear();
}

void CommandSystem::recreateSwapchainResources() {
    if (headless_) {
        return;
    }
    waitForFrameFences();
    destroyPresentSemaphores();
    swapchain_.recreate();
    createPresentSemaphores();
}

void CommandSystem::recordWorkCommands(VkCommandBuffer commandBuffer, uint32_t, float clearPhase) const {
    VkCommandBufferBeginInfo beginInfo{};
    beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    checkVk(vkBeginCommandBuffer(commandBuffer, &beginInfo), "vkBeginCommandBuffer");

    if (pathTracer_ != nullptr) {
        pathTracer_->recordPathTrace(commandBuffer, canRecordAsyncCompute());
    } else if (pipelineDemo_ != nullptr) {
        pipelineDemo_->recordCompute(commandBuffer, clearPhase);
    }

    checkVk(vkEndCommandBuffer(commandBuffer), "vkEndCommandBuffer");
}

void CommandSystem::recordPresentationCommands(VkCommandBuffer commandBuffer, uint32_t imageIndex, float clearPhase) const {
    VkCommandBufferBeginInfo beginInfo{};
    beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    checkVk(vkBeginCommandBuffer(commandBuffer, &beginInfo), "vkBeginCommandBuffer(post)");

    const VkImage swapchainImage = swapchain_.image(imageIndex);
    transitionImage(
        commandBuffer,
        swapchainImage,
        VK_IMAGE_LAYOUT_UNDEFINED,
        VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
        VK_PIPELINE_STAGE_2_NONE,
        VK_ACCESS_2_NONE,
        VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
        VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);

    const float pulse = 0.5f + 0.5f * std::sin(clearPhase * 0.7f);
    VkClearValue clearValue{};
    clearValue.color.float32[0] = 0.02f;
    clearValue.color.float32[1] = 0.03f + 0.04f * pulse;
    clearValue.color.float32[2] = 0.05f;
    clearValue.color.float32[3] = 1.0f;

    VkRenderingAttachmentInfo colorAttachment{};
    colorAttachment.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
    colorAttachment.imageView = swapchain_.imageView(imageIndex);
    colorAttachment.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    colorAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    colorAttachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    colorAttachment.clearValue = clearValue;

    VkRenderingInfo renderingInfo{};
    renderingInfo.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
    renderingInfo.renderArea.offset = {0, 0};
    renderingInfo.renderArea.extent = swapchain_.extent();
    renderingInfo.layerCount = 1;
    renderingInfo.colorAttachmentCount = 1;
    renderingInfo.pColorAttachments = &colorAttachment;

    vkCmdBeginRendering(commandBuffer, &renderingInfo);
    const bool pathTracerPresentedByEditor = pathTracer_ != nullptr && uiOverlay_ != nullptr && uiOverlay_->rendersPathTracerInViewport();
    if (pathTracer_ != nullptr && !pathTracerPresentedByEditor) {
        pathTracer_->recordFullscreen(commandBuffer, swapchain_.extent());
    } else if (pipelineDemo_ != nullptr) {
        pipelineDemo_->recordFullscreen(commandBuffer, swapchain_.extent());
    }
    if (pathTracerPresentedByEditor) {
        pathTracer_->recordEditorPresentationStart(commandBuffer);
    }
    if (uiOverlay_ != nullptr) {
        uiOverlay_->record(commandBuffer);
    }
    if (pathTracerPresentedByEditor) {
        pathTracer_->recordEditorPresentationEnd(commandBuffer);
    }
    vkCmdEndRendering(commandBuffer);

    if (resourceDemo_ != nullptr && pipelineDemo_ == nullptr && pathTracer_ == nullptr) {
        transitionImage(
            commandBuffer,
            swapchainImage,
            VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
            VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
            VK_PIPELINE_STAGE_2_BLIT_BIT,
            VK_ACCESS_2_TRANSFER_WRITE_BIT);

        resourceDemo_->record(commandBuffer, swapchainImage, swapchain_.extent());

        transitionImage(
            commandBuffer,
            swapchainImage,
            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            !headless_ ? VK_IMAGE_LAYOUT_PRESENT_SRC_KHR : VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
            VK_PIPELINE_STAGE_2_BLIT_BIT,
            VK_ACCESS_2_TRANSFER_WRITE_BIT,
            VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
            VK_ACCESS_2_NONE);
    } else {
        transitionImage(
            commandBuffer,
            swapchainImage,
            VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
            !headless_ ? VK_IMAGE_LAYOUT_PRESENT_SRC_KHR : VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
            VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
            VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
            VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
            VK_ACCESS_2_NONE);
    }

    checkVk(vkEndCommandBuffer(commandBuffer), "vkEndCommandBuffer(post)");
}

bool CommandSystem::canRecordAsyncCompute() const {
    return asyncComputeEnabled_ &&
        pathTracer_ != nullptr &&
        context_.computeQueue() != VK_NULL_HANDLE &&
        context_.hasIndependentComputeQueue() &&
        context_.timelineSemaphore() != VK_NULL_HANDLE;
}

bool CommandSystem::recordAsyncComputeCommands(FrameResources& frame) const {
    if (!canRecordAsyncCompute() || frame.computeCommandBuffer == VK_NULL_HANDLE) {
        return false;
    }

    VkCommandBufferBeginInfo beginInfo{};
    beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    checkVk(vkBeginCommandBuffer(frame.computeCommandBuffer, &beginInfo), "vkBeginCommandBuffer(compute)");
    const bool recorded = pathTracer_->recordAsyncComputeWork(frame.computeCommandBuffer);
    checkVk(vkEndCommandBuffer(frame.computeCommandBuffer), "vkEndCommandBuffer(compute)");
    return recorded;
}

void CommandSystem::submitFrame(FrameResources& frame, uint32_t imageIndex, bool asyncComputeRecorded) const {
    VkCommandBufferSubmitInfo workCommand{};
    workCommand.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO;
    workCommand.commandBuffer = frame.commandBuffer;

    VkCommandBufferSubmitInfo postCommand{};
    postCommand.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO;
    postCommand.commandBuffer = frame.postCommandBuffer;

    std::array<VkSemaphoreSubmitInfo, 2> graphicsWaits{};
    uint32_t graphicsWaitCount = 0;
    if (asyncHistoryCompleteValue_ != 0) {
        VkSemaphoreSubmitInfo& wait = graphicsWaits[graphicsWaitCount++];
        wait.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO;
        wait.semaphore = context_.timelineSemaphore();
        wait.value = asyncHistoryCompleteValue_;
        wait.stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    }
    if (!headless_) {
        VkSemaphoreSubmitInfo& wait = graphicsWaits[graphicsWaitCount++];
        wait.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO;
        wait.semaphore = frame.imageAvailable;
        wait.stageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
    }

    std::array<VkSemaphoreSubmitInfo, 2> graphicsSignals{};
    uint32_t graphicsSignalCount = 0;
    uint64_t graphicsTimelineValue = 0;
    if (asyncComputeRecorded) {
        graphicsTimelineValue = ++asyncTimelineValue_;
        VkSemaphoreSubmitInfo& signal = graphicsSignals[graphicsSignalCount++];
        signal.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO;
        signal.semaphore = context_.timelineSemaphore();
        signal.value = graphicsTimelineValue;
        signal.stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    } else if (!headless_) {
        VkSemaphoreSubmitInfo& signal = graphicsSignals[graphicsSignalCount++];
        signal.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO;
        signal.semaphore = imageRenderFinished_.at(imageIndex);
        signal.stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    }

    std::array<VkCommandBufferSubmitInfo, 2> graphicsCommands{workCommand, postCommand};

    VkSubmitInfo2 graphicsSubmit{};
    graphicsSubmit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2;
    graphicsSubmit.waitSemaphoreInfoCount = graphicsWaitCount;
    graphicsSubmit.pWaitSemaphoreInfos = graphicsWaitCount > 0 ? graphicsWaits.data() : nullptr;
    graphicsSubmit.commandBufferInfoCount = asyncComputeRecorded ? 1u : static_cast<uint32_t>(graphicsCommands.size());
    graphicsSubmit.pCommandBufferInfos = graphicsCommands.data();
    graphicsSubmit.signalSemaphoreInfoCount = graphicsSignalCount;
    graphicsSubmit.pSignalSemaphoreInfos = graphicsSignalCount > 0 ? graphicsSignals.data() : nullptr;

    if (!asyncComputeRecorded) {
        checkVk(vkQueueSubmit2(context_.graphicsQueue(), 1, &graphicsSubmit, frame.inFlight), "vkQueueSubmit2(graphics)");
        return;
    }

    checkVk(vkQueueSubmit2(context_.graphicsQueue(), 1, &graphicsSubmit, VK_NULL_HANDLE), "vkQueueSubmit2(graphics async producer)");

    const uint64_t computeTimelineValue = ++asyncTimelineValue_;
    VkSemaphoreSubmitInfo computeWait{};
    computeWait.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO;
    computeWait.semaphore = context_.timelineSemaphore();
    computeWait.value = graphicsTimelineValue;
    computeWait.stageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;

    VkSemaphoreSubmitInfo computeSignal{};
    computeSignal.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO;
    computeSignal.semaphore = context_.timelineSemaphore();
    computeSignal.value = computeTimelineValue;
    computeSignal.stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;

    VkCommandBufferSubmitInfo computeCommand{};
    computeCommand.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO;
    computeCommand.commandBuffer = frame.computeCommandBuffer;

    VkSubmitInfo2 computeSubmit{};
    computeSubmit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2;
    computeSubmit.waitSemaphoreInfoCount = 1;
    computeSubmit.pWaitSemaphoreInfos = &computeWait;
    computeSubmit.commandBufferInfoCount = 1;
    computeSubmit.pCommandBufferInfos = &computeCommand;
    computeSubmit.signalSemaphoreInfoCount = 1;
    computeSubmit.pSignalSemaphoreInfos = &computeSignal;
    checkVk(vkQueueSubmit2(context_.computeQueue(), 1, &computeSubmit, VK_NULL_HANDLE), "vkQueueSubmit2(compute)");

    VkSemaphoreSubmitInfo postWait{};
    postWait.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO;
    postWait.semaphore = context_.timelineSemaphore();
    postWait.value = computeTimelineValue;
    postWait.stageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;

    VkSemaphoreSubmitInfo postSignal{};
    postSignal.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO;
    postSignal.semaphore = !headless_ ? imageRenderFinished_.at(imageIndex) : VK_NULL_HANDLE;
    postSignal.stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;

    VkSubmitInfo2 postSubmit{};
    postSubmit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2;
    postSubmit.waitSemaphoreInfoCount = 1;
    postSubmit.pWaitSemaphoreInfos = &postWait;
    postSubmit.commandBufferInfoCount = 1;
    postSubmit.pCommandBufferInfos = &postCommand;
    postSubmit.signalSemaphoreInfoCount = !headless_ ? 1u : 0u;
    postSubmit.pSignalSemaphoreInfos = !headless_ ? &postSignal : nullptr;
    checkVk(vkQueueSubmit2(context_.graphicsQueue(), 1, &postSubmit, frame.inFlight), "vkQueueSubmit2(graphics async post)");
    asyncHistoryCompleteValue_ = computeTimelineValue;
}

void CommandSystem::transitionImage(
    VkCommandBuffer commandBuffer,
    VkImage image,
    VkImageLayout oldLayout,
    VkImageLayout newLayout,
    VkPipelineStageFlags2 srcStage,
    VkAccessFlags2 srcAccess,
    VkPipelineStageFlags2 dstStage,
    VkAccessFlags2 dstAccess) const {
    VkImageMemoryBarrier2 barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
    barrier.srcStageMask = srcStage;
    barrier.srcAccessMask = srcAccess;
    barrier.dstStageMask = dstStage;
    barrier.dstAccessMask = dstAccess;
    barrier.oldLayout = oldLayout;
    barrier.newLayout = newLayout;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = image;
    barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    barrier.subresourceRange.baseMipLevel = 0;
    barrier.subresourceRange.levelCount = 1;
    barrier.subresourceRange.baseArrayLayer = 0;
    barrier.subresourceRange.layerCount = 1;

    VkDependencyInfo dependency{};
    dependency.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
    dependency.imageMemoryBarrierCount = 1;
    dependency.pImageMemoryBarriers = &barrier;

    vkCmdPipelineBarrier2(commandBuffer, &dependency);
}

} // namespace rtv
