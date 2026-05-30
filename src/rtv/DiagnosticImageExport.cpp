#include "rtv/DiagnosticImageExport.h"

#include "rtv/Buffer.h"
#include "rtv/Check.h"
#include "rtv/Image.h"
#include "rtv/PathTracerRenderer.h"
#include "rtv/RendererDebug.h"
#include "rtv/ResourceAllocator.h"
#include "rtv/VulkanContext.h"

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include <stb_image_write.h>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cstring>
#include <fstream>
#include <stdexcept>
#include <vector>

namespace rtv {

DiagnosticImageExport::DiagnosticImageExport(const VulkanContext& context, ResourceAllocator& allocator)
    : context_(context), allocator_(allocator) {
}

DiagnosticImageExport::~DiagnosticImageExport() {
    destroy();
}

bool DiagnosticImageExport::initialize(VkFormat format, VkExtent2D extent) {
    format_ = format;
    extent_ = extent;

    const VkDeviceSize bufferSize = static_cast<VkDeviceSize>(extent.width) * extent.height * 4;
    readbackBuffer_ = std::make_unique<Buffer>();
    readbackBuffer_->create(allocator_, BufferDesc{
        .size = bufferSize,
        .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT,
        .memory = BufferMemory::Readback,
        .persistentMapped = true,
        .debugName = "diagnostic readback buffer",
    });

    VkCommandPoolCreateInfo poolInfo{};
    poolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    poolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    poolInfo.queueFamilyIndex = context_.queueFamilies().graphics.value();
    checkVk(vkCreateCommandPool(context_.device(), &poolInfo, nullptr, &commandPool_), "vkCreateCommandPool(export)");

    VkCommandBufferAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    allocInfo.commandPool = commandPool_;
    allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    allocInfo.commandBufferCount = 1;
    checkVk(vkAllocateCommandBuffers(context_.device(), &allocInfo, &commandBuffer_), "vkAllocateCommandBuffers(export)");

    VkFenceCreateInfo fenceInfo{};
    fenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    checkVk(vkCreateFence(context_.device(), &fenceInfo, nullptr, &completionFence_), "vkCreateFence(export)");

    initialized_ = true;
    return true;
}

void DiagnosticImageExport::destroy() {
    if (completionFence_ != VK_NULL_HANDLE) {
        vkDestroyFence(context_.device(), completionFence_, nullptr);
        completionFence_ = VK_NULL_HANDLE;
    }
    if (commandPool_ != VK_NULL_HANDLE) {
        vkDestroyCommandPool(context_.device(), commandPool_, nullptr);
        commandPool_ = VK_NULL_HANDLE;
    }
    readbackBuffer_.reset();
    initialized_ = false;
}

bool DiagnosticImageExport::exportView(
    PathTracerRenderer& renderer,
    RendererDebugView view,
    const std::filesystem::path& outputPath,
    uint32_t warmupFrames) {
    (void)view;
    (void)warmupFrames;
    if (!initialized_) {
        return false;
    }

    const VkImage presImage = renderer.presentationImage();
    if (presImage == VK_NULL_HANDLE) {
        return false;
    }

    VkCommandBufferBeginInfo beginInfo{};
    beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    checkVk(vkBeginCommandBuffer(commandBuffer_, &beginInfo), "vkBeginCommandBuffer(export)");

    VkImageMemoryBarrier2 preBarrier{};
    preBarrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
    preBarrier.srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
    preBarrier.srcAccessMask = VK_ACCESS_2_SHADER_WRITE_BIT;
    preBarrier.dstStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
    preBarrier.dstAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT;
    preBarrier.oldLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    preBarrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    preBarrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    preBarrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    preBarrier.image = presImage;
    preBarrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    preBarrier.subresourceRange.baseMipLevel = 0;
    preBarrier.subresourceRange.levelCount = 1;
    preBarrier.subresourceRange.baseArrayLayer = 0;
    preBarrier.subresourceRange.layerCount = 1;

    VkDependencyInfo preDependency{};
    preDependency.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
    preDependency.imageMemoryBarrierCount = 1;
    preDependency.pImageMemoryBarriers = &preBarrier;
    vkCmdPipelineBarrier2(commandBuffer_, &preDependency);

    VkBufferImageCopy region{};
    region.bufferOffset = 0;
    region.bufferRowLength = 0;
    region.bufferImageHeight = 0;
    region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    region.imageSubresource.mipLevel = 0;
    region.imageSubresource.baseArrayLayer = 0;
    region.imageSubresource.layerCount = 1;
    region.imageOffset = {0, 0, 0};
    region.imageExtent = {extent_.width, extent_.height, 1};

    vkCmdCopyImageToBuffer(commandBuffer_, presImage, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
        readbackBuffer_->handle(), 1, &region);

    VkImageMemoryBarrier2 postBarrier{};
    postBarrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
    postBarrier.srcStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
    postBarrier.srcAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT;
    postBarrier.dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
    postBarrier.dstAccessMask = VK_ACCESS_2_SHADER_WRITE_BIT;
    postBarrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    postBarrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    postBarrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    postBarrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    postBarrier.image = presImage;
    postBarrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    postBarrier.subresourceRange.baseMipLevel = 0;
    postBarrier.subresourceRange.levelCount = 1;
    postBarrier.subresourceRange.baseArrayLayer = 0;
    postBarrier.subresourceRange.layerCount = 1;

    VkDependencyInfo postDependency{};
    postDependency.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
    postDependency.imageMemoryBarrierCount = 1;
    postDependency.pImageMemoryBarriers = &postBarrier;
    vkCmdPipelineBarrier2(commandBuffer_, &postDependency);

    checkVk(vkEndCommandBuffer(commandBuffer_), "vkEndCommandBuffer(export)");

    VkCommandBufferSubmitInfo cmdInfo{};
    cmdInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO;
    cmdInfo.commandBuffer = commandBuffer_;

    VkSubmitInfo2 submitInfo{};
    submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2;
    submitInfo.commandBufferInfoCount = 1;
    submitInfo.pCommandBufferInfos = &cmdInfo;

    checkVk(vkQueueSubmit2(context_.graphicsQueue(), 1, &submitInfo, completionFence_), "vkQueueSubmit2(export)");
    checkVk(vkWaitForFences(context_.device(), 1, &completionFence_, VK_TRUE, UINT64_MAX), "vkWaitForFences(export)");
    checkVk(vkResetFences(context_.device(), 1, &completionFence_), "vkResetFences(export)");

    readbackBuffer_->invalidate(readbackBuffer_->size());

    const auto* data = static_cast<const unsigned char*>(readbackBuffer_->mappedData());
    if (data == nullptr) {
        return false;
    }

    const auto dir = outputPath.parent_path();
    if (!dir.empty() && !std::filesystem::exists(dir)) {
        std::filesystem::create_directories(dir);
    }

    int result = stbi_write_png(
        outputPath.string().c_str(),
        static_cast<int>(extent_.width),
        static_cast<int>(extent_.height),
        4,
        data,
        static_cast<int>(extent_.width * 4));

    return result != 0;
}

void DiagnosticImageExport::writeExportManifest(
    const std::filesystem::path& dir,
    const std::vector<std::string>& exported,
    uint32_t width,
    uint32_t height) {
    nlohmann::json manifest;
    manifest["exported"] = exported;
    manifest["missing_debug_views"] = nlohmann::json::array();
    manifest["resolution"] = { {"width", width}, {"height", height} };

    const auto manifestPath = dir / "export_manifest.json";
    std::ofstream file(manifestPath);
    if (!file.is_open()) {
        throw std::runtime_error("Failed to open export manifest: " + manifestPath.string());
    }
    file << manifest.dump(2);
}

std::vector<RendererDebugView> DiagnosticImageExport::allExportViews() {
    return {
        RendererDebugView::Beauty,
        RendererDebugView::Variance,
        RendererDebugView::DenoiserRejection,
        RendererDebugView::Albedo,
        RendererDebugView::MaterialOcclusion,
        RendererDebugView::Normals,
        RendererDebugView::ReprojectionConfidence,
        RendererDebugView::Depth,
        RendererDebugView::MotionVectors,
        RendererDebugView::Roughness,
        RendererDebugView::DirectLighting,
        RendererDebugView::IndirectLighting,
        RendererDebugView::EmissiveContribution,
        RendererDebugView::EnvironmentContribution,
        RendererDebugView::TraversalSteps,
        RendererDebugView::BvhDepth,
        RendererDebugView::InstanceId,
        RendererDebugView::MeshId,
        RendererDebugView::TlasSteps,
        RendererDebugView::TraversalMismatch,
        RendererDebugView::LightPdf,
        RendererDebugView::BsdfPdf,
        RendererDebugView::MisWeight,
        RendererDebugView::DirectSampleType,
        RendererDebugView::CausticVisibility,
        RendererDebugView::ClayMaterial,
        RendererDebugView::FirstBounceThroughput,
        RendererDebugView::SecondaryEnvironmentMiss,
        RendererDebugView::BounceCount,
        RendererDebugView::SecondaryEnvironmentRadiance,
        RendererDebugView::WhiteEnvironmentTransport,
        RendererDebugView::AtmosphereSkyView,
        RendererDebugView::AtmosphereTransmittance,
        RendererDebugView::AtmosphereAerialPerspective,
        RendererDebugView::AtmosphereMultiScatter,
        RendererDebugView::TemporalReactiveMask,
        RendererDebugView::TemporalHistoryWeight,
        RendererDebugView::RestirReservoirAge,
        RendererDebugView::RestirReservoirConfidence,
        RendererDebugView::RestirReservoirM,
        RendererDebugView::EmissiveContinuation,
        RendererDebugView::SunMisWeight,
        RendererDebugView::SunLightPdf,
        RendererDebugView::SunPreviousBsdfPdf,
        RendererDebugView::RisRawLightPdf,
        RendererDebugView::RisEffectiveLightPdf,
        RendererDebugView::RisPdfRatio,
        RendererDebugView::SampleDimension,
        RendererDebugView::SampleScramble,
        RendererDebugView::PathDirectDiffuse,
        RendererDebugView::PathDirectSpecular,
        RendererDebugView::PathIndirectDiffuse,
        RendererDebugView::PathIndirectSpecular,
        RendererDebugView::PathDataAlbedo,
        RendererDebugView::PathDataMetrics,
        RendererDebugView::DenoiserKernelRadius,
        RendererDebugView::DenoiserHitDistance,
        RendererDebugView::DenoiserVirtualMotion,
        RendererDebugView::DenoiserDiffuseDebug,
        RendererDebugView::DenoiserSpecularDebug,
        RendererDebugView::DenoiserEmissiveClamp,
        RendererDebugView::DenoiserVarianceConfidence,
        RendererDebugView::DenoiserDiffuseChannelConfidence,
        RendererDebugView::DenoiserFrameBlend,
        RendererDebugView::DenoiserMaxHitDistanceDelta,
        RendererDebugView::DenoiserDiffuseOnScreen,
        RendererDebugView::DenoiserBaseDisocclusion,
        RendererDebugView::DenoiserSpecularChannelConfidence,
        RendererDebugView::DenoiserSpecularHistoryWeight,
        RendererDebugView::DenoiserDirectDiffuseVariance,
        RendererDebugView::DenoiserDirectSpecularVariance,
        RendererDebugView::DenoiserIndirectDiffuseVariance,
        RendererDebugView::DenoiserIndirectSpecularVariance,
        RendererDebugView::DenoiserDiffuseHistoryLength,
        RendererDebugView::DenoiserSpecularHistoryLength,
        RendererDebugView::MomentUpdateValidity,
        RendererDebugView::MomentDisocclusionConfidence,
        RendererDebugView::MomentNormalCone,
        RendererDebugView::MomentDepthDelta,
        RendererDebugView::MomentHistoryKindValid,
        RendererDebugView::DenoiserDiffuseRawVariance,
        RendererDebugView::DenoiserSpecularRawVariance,
        RendererDebugView::RestirPairwiseMis,
        RendererDebugView::RestirGiValidity,
        RendererDebugView::RestirGiAge,
        RendererDebugView::RestirGiInitial,
        RendererDebugView::RestirGiTemporal,
        RendererDebugView::RestirGiSpatial,
        RendererDebugView::RestirGiFinal,
        RendererDebugView::RestirGiNormal,
        RendererDebugView::RestirGiHitDistance,
        RendererDebugView::WavefrontQueueOccupancy,
        RendererDebugView::WavefrontPathDepth,
        RendererDebugView::WavefrontLiveRays,
        RendererDebugView::WavefrontTerminatedRays,
        RendererDebugView::WavefrontMaterialBucket,
        RendererDebugView::WavefrontRestirDi,
        RendererDebugView::WavefrontRestirGi,
        RendererDebugView::WavefrontDirectLighting,
    };
}

} // namespace rtv
