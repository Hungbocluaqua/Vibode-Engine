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

namespace rtv {

CommandSystem::CommandSystem(const VulkanContext& context, Swapchain& swapchain)
    : context_(context), swapchain_(swapchain) {
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
    VkResult acquireResult = vkAcquireNextImageKHR(
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

    checkVk(vkResetFences(context_.device(), 1, &frame.inFlight), "vkResetFences");
    checkVk(vkResetCommandPool(context_.device(), frame.commandPool, 0), "vkResetCommandPool");
    if (pathTracer_ != nullptr) {
        VkExtent2D renderExtent = swapchain_.extent();
        if (uiOverlay_ != nullptr) {
            renderExtent = uiOverlay_->desiredRenderExtent(renderExtent);
        }
        const float scale = pathTracer_->settings().renderResolutionScale;
        renderExtent.width = std::max(1u, static_cast<uint32_t>(static_cast<float>(renderExtent.width) * scale));
        renderExtent.height = std::max(1u, static_cast<uint32_t>(static_cast<float>(renderExtent.height) * scale));
        const VkExtent2D currentRenderExtent = pathTracer_->renderExtent();
        if (uiOverlay_ != nullptr &&
            currentRenderExtent.width != 0 &&
            (currentRenderExtent.width != renderExtent.width || currentRenderExtent.height != renderExtent.height)) {
            checkVk(vkDeviceWaitIdle(context_.device()), "vkDeviceWaitIdle(editor viewport resize)");
            uiOverlay_->invalidateViewportTexture();
        }
        pathTracer_->setFrameDeltaSeconds(deltaSeconds);
        pathTracer_->beginFrame(frameIndex_, renderExtent);
    } else if (pipelineDemo_ != nullptr) {
        pipelineDemo_->beginFrame(frameIndex_);
    }
    recordClearCommands(frame.commandBuffer, imageIndex, clearPhase);

    VkCommandBufferSubmitInfo commandBufferInfo{};
    commandBufferInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO;
    commandBufferInfo.commandBuffer = frame.commandBuffer;

    VkSemaphoreSubmitInfo waitInfo{};
    waitInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO;
    waitInfo.semaphore = frame.imageAvailable;
    waitInfo.stageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;

    VkSemaphoreSubmitInfo signalInfo{};
    signalInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO;
    signalInfo.semaphore = imageRenderFinished_.at(imageIndex);
    signalInfo.stageMask = VK_PIPELINE_STAGE_2_ALL_GRAPHICS_BIT;

    VkSubmitInfo2 submitInfo{};
    submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2;
    submitInfo.waitSemaphoreInfoCount = 1;
    submitInfo.pWaitSemaphoreInfos = &waitInfo;
    submitInfo.commandBufferInfoCount = 1;
    submitInfo.pCommandBufferInfos = &commandBufferInfo;
    submitInfo.signalSemaphoreInfoCount = 1;
    submitInfo.pSignalSemaphoreInfos = &signalInfo;

    checkVk(vkQueueSubmit2(context_.graphicsQueue(), 1, &submitInfo, frame.inFlight), "vkQueueSubmit2");

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
        vkDeviceWaitIdle(context_.device());
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
    waitIdle();
    destroyPresentSemaphores();
    swapchain_.recreate();
    createPresentSemaphores();
}

void CommandSystem::recordClearCommands(VkCommandBuffer commandBuffer, uint32_t imageIndex, float clearPhase) const {
    VkCommandBufferBeginInfo beginInfo{};
    beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    checkVk(vkBeginCommandBuffer(commandBuffer, &beginInfo), "vkBeginCommandBuffer");

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

    if (pathTracer_ != nullptr) {
        pathTracer_->recordPathTrace(commandBuffer);
    } else if (pipelineDemo_ != nullptr) {
        pipelineDemo_->recordCompute(commandBuffer, clearPhase);
    }

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
            VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
            VK_PIPELINE_STAGE_2_BLIT_BIT,
            VK_ACCESS_2_TRANSFER_WRITE_BIT,
            VK_PIPELINE_STAGE_2_NONE,
            VK_ACCESS_2_NONE);
    } else {
        transitionImage(
            commandBuffer,
            swapchainImage,
            VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
            VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
            VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
            VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
            VK_PIPELINE_STAGE_2_NONE,
            VK_ACCESS_2_NONE);
    }

    checkVk(vkEndCommandBuffer(commandBuffer), "vkEndCommandBuffer");
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
