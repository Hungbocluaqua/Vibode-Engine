#include "rtv/AtmosphereLutSystem.h"

#include "rtv/AtmosphereSamplingSystem.h"
#include "rtv/ComputePipeline.h"
#include "rtv/DescriptorAllocator.h"
#include "rtv/DescriptorLayoutCache.h"
#include "rtv/DescriptorWriter.h"
#include "rtv/GpuProfiler.h"
#include "rtv/ImageBarrier.h"
#include "rtv/PipelineCache.h"
#include "rtv/ResourceAllocator.h"
#include "rtv/ShaderModule.h"
#include "rtv/ShaderReflection.h"
#include "rtv/Check.h"

#include <algorithm>
#include <cmath>

namespace rtv {

AtmosphereLutSystem::AtmosphereLutSystem(
    VkDevice device,
    ResourceAllocator& allocator,
    DescriptorLayoutCache& layoutCache,
    PipelineCache& pipelineCache,
    const ShaderModule& transmittanceShader,
    const ShaderModule& multiScatterShader,
    const ShaderModule& skyViewShader,
    const ShaderModule& skyReprojectShader,
    const ShaderModule& aerialPerspectiveShader,
    const ShaderModule& skyCdfShader)
    : device_(device), allocator_(allocator) {
    VkSamplerCreateInfo samplerInfo{};
    samplerInfo.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    samplerInfo.magFilter = VK_FILTER_LINEAR;
    samplerInfo.minFilter = VK_FILTER_LINEAR;
    samplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.maxLod = 1.0f;
    checkVk(vkCreateSampler(device_, &samplerInfo, nullptr, &sampler_), "vkCreateSampler(atmosphere LUT)");

    transmittanceLut_.create(allocator, ImageDesc{
        .width = 256,
        .height = 64,
        .format = VK_FORMAT_R16G16B16A16_SFLOAT,
        .usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
        .debugName = "atmosphere transmittance lut",
    });
    multiScatterLut_.create(allocator, ImageDesc{
        .width = 32,
        .height = 32,
        .format = VK_FORMAT_R16G16B16A16_SFLOAT,
        .usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
        .debugName = "atmosphere multi scatter lut",
    });
    skyViewLut_.create(allocator, ImageDesc{
        .width = 256,
        .height = 144,
        .format = VK_FORMAT_R16G16B16A16_SFLOAT,
        .usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
        .debugName = "atmosphere sky view lut",
    });
    rawSkyViewLut_.create(allocator, ImageDesc{
        .width = 256,
        .height = 144,
        .format = VK_FORMAT_R16G16B16A16_SFLOAT,
        .usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
        .debugName = "atmosphere raw sky view lut",
    });
    previousSkyViewLut_.create(allocator, ImageDesc{
        .width = 256,
        .height = 144,
        .format = VK_FORMAT_R16G16B16A16_SFLOAT,
        .usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
        .debugName = "atmosphere previous sky view lut",
    });
    aerialPerspectiveLut_.create(allocator, ImageDesc{
        .width = 96,
        .height = 96,
        .depth = 48,
        .format = VK_FORMAT_R16G16B16A16_SFLOAT,
        .usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
        .debugName = "atmosphere aerial perspective lut",
    });
    transmittanceSetLayout_ = layoutCache.createLayout(ShaderReflection::bindingsForSet({transmittanceShader.reflection()}, 0));
    multiScatterSetLayout_ = layoutCache.createLayout(ShaderReflection::bindingsForSet({multiScatterShader.reflection()}, 0));
    skyViewSetLayout_ = layoutCache.createLayout(ShaderReflection::bindingsForSet({skyViewShader.reflection()}, 0));
    skyReprojectSetLayout_ = layoutCache.createLayout(ShaderReflection::bindingsForSet({skyReprojectShader.reflection()}, 0));
    aerialPerspectiveSetLayout_ = layoutCache.createLayout(ShaderReflection::bindingsForSet({aerialPerspectiveShader.reflection()}, 0));
    transmittancePipeline_ = std::make_unique<ComputePipeline>(
        device_,
        transmittanceShader,
        std::vector<VkDescriptorSetLayout>{transmittanceSetLayout_},
        ShaderReflection::mergePushConstants({transmittanceShader.reflection()}),
        pipelineCache);
    multiScatterPipeline_ = std::make_unique<ComputePipeline>(
        device_,
        multiScatterShader,
        std::vector<VkDescriptorSetLayout>{multiScatterSetLayout_},
        ShaderReflection::mergePushConstants({multiScatterShader.reflection()}),
        pipelineCache);
    skyViewPipeline_ = std::make_unique<ComputePipeline>(
        device_,
        skyViewShader,
        std::vector<VkDescriptorSetLayout>{skyViewSetLayout_},
        ShaderReflection::mergePushConstants({skyViewShader.reflection()}),
        pipelineCache);
    skyReprojectPipeline_ = std::make_unique<ComputePipeline>(
        device_,
        skyReprojectShader,
        std::vector<VkDescriptorSetLayout>{skyReprojectSetLayout_},
        ShaderReflection::mergePushConstants({skyReprojectShader.reflection()}),
        pipelineCache);
    aerialPerspectivePipeline_ = std::make_unique<ComputePipeline>(
        device_,
        aerialPerspectiveShader,
        std::vector<VkDescriptorSetLayout>{aerialPerspectiveSetLayout_},
        ShaderReflection::mergePushConstants({aerialPerspectiveShader.reflection()}),
        pipelineCache);
    samplingSystem_ = std::make_unique<AtmosphereSamplingSystem>(
        device_,
        allocator_,
        layoutCache,
        pipelineCache,
        skyCdfShader,
        skyViewLut_,
        sampler_);
    markDirty(LutNode::Transmittance);
}

AtmosphereLutSystem::~AtmosphereLutSystem() {
    if (sampler_ != VK_NULL_HANDLE) {
        vkDestroySampler(device_, sampler_, nullptr);
    }
}

void AtmosphereLutSystem::setSkyParameters(float sunElevation, float sunAzimuth, float skyIntensity) {
    if (std::abs(sunElevation_ - sunElevation) > 0.0001f ||
        std::abs(sunAzimuth_ - sunAzimuth) > 0.0001f ||
        std::abs(skyIntensity_ - skyIntensity) > 0.0001f) {
        markDirty(LutNode::Transmittance);
        previousSkyViewReady_ = false;
    }
    sunElevation_ = sunElevation;
    sunAzimuth_ = sunAzimuth;
    skyIntensity_ = skyIntensity;
}

void AtmosphereLutSystem::setSkyDirection(glm::vec3 sunDirection, float skyIntensity) {
    const float len2 = glm::dot(sunDirection, sunDirection);
    if (len2 <= 1.0e-6f) {
        setSkyParameters(0.97f, 0.0f, skyIntensity);
        return;
    }
    sunDirection *= glm::inversesqrt(len2);
    const float elevation = std::asin(std::clamp(sunDirection.y, -1.0f, 1.0f));
    const float azimuth = std::atan2(sunDirection.x, sunDirection.z);
    setSkyParameters(elevation, azimuth, skyIntensity);
}

void AtmosphereLutSystem::setAtmosphereParams(float rayleighScaleHeight, float mieScaleHeight, float mieAnisotropy, float groundAlbedo) {
    const auto& p = model_.params();
    const bool changed =
        std::abs(p.rayleighScaleHeight - rayleighScaleHeight) > 0.5f ||
        std::abs(p.mieScaleHeight - mieScaleHeight) > 0.5f ||
        std::abs(p.miePhaseAnisotropy - mieAnisotropy) > 0.0001f ||
        std::abs(p.groundAlbedo - groundAlbedo) > 0.0001f;
    if (!changed) return;
    AtmosphereParams next = p;
    next.rayleighScaleHeight = rayleighScaleHeight;
    next.mieScaleHeight = mieScaleHeight;
    next.miePhaseAnisotropy = mieAnisotropy;
    next.groundAlbedo = groundAlbedo;
    model_.setParams(next);
    markDirty();
    previousSkyViewReady_ = false;
}

void AtmosphereLutSystem::setQuality(AtmosphereQuality quality) {
    if (quality_ == quality) {
        return;
    }
    quality_ = quality;
    markDirty();
    previousSkyViewReady_ = false;
}

void AtmosphereLutSystem::markDirty() {
    markDirty(LutNode::Transmittance);
    previousSkyViewReady_ = false;
    previousCameraPosSet_ = false;
}

void AtmosphereLutSystem::setCameraPosition(glm::vec3 position) {
    static constexpr float kCameraMoveRecomputeThreshold = 100.0f;
    if (!previousCameraPosSet_) {
        previousCameraPos_ = position;
        previousCameraPosSet_ = true;
        return;
    }
    float delta = glm::length(position - previousCameraPos_);
    if (delta > kCameraMoveRecomputeThreshold) {
        markDirty(LutNode::SkyView);
    }
    previousCameraPos_ = position;
}

void AtmosphereLutSystem::markDirty(LutNode node) {
    switch (node) {
    case LutNode::Transmittance:
        stats_.dirty[lutNodeIndex(LutNode::Transmittance)] = true;
        stats_.dirty[lutNodeIndex(LutNode::MultiScatter)] = true;
        stats_.dirty[lutNodeIndex(LutNode::SkyView)] = true;
        stats_.dirty[lutNodeIndex(LutNode::AerialPerspective)] = true;
        break;
    case LutNode::MultiScatter:
        stats_.dirty[lutNodeIndex(LutNode::MultiScatter)] = true;
        stats_.dirty[lutNodeIndex(LutNode::SkyView)] = true;
        stats_.dirty[lutNodeIndex(LutNode::AerialPerspective)] = true;
        break;
    case LutNode::SkyView:
        stats_.dirty[lutNodeIndex(LutNode::SkyView)] = true;
        stats_.dirty[lutNodeIndex(LutNode::AerialPerspective)] = true;
        break;
    case LutNode::AerialPerspective:
        stats_.dirty[lutNodeIndex(LutNode::AerialPerspective)] = true;
        break;
    }
}

bool AtmosphereLutSystem::isDirty(LutNode node) const {
    return stats_.dirty[lutNodeIndex(node)];
}

void AtmosphereLutSystem::clearDirty(LutNode node) {
    stats_.dirty[lutNodeIndex(node)] = false;
}

void AtmosphereLutSystem::markGenerated(LutNode node) {
    const size_t index = lutNodeIndex(node);
    stats_.generatedThisRecord[index] = true;
    ++stats_.generationCounts[index];
}

void AtmosphereLutSystem::record(VkCommandBuffer commandBuffer, DescriptorAllocator& descriptors, GpuProfiler* profiler) {
    std::fill(stats_.generatedThisRecord.begin(), stats_.generatedThisRecord.end(), false);
    recordTransmittance(commandBuffer, descriptors, profiler);
    recordMultiScatter(commandBuffer, descriptors, profiler);
    recordSkyView(commandBuffer, descriptors, profiler);
    recordAerialPerspective(commandBuffer, descriptors, profiler);
}

void AtmosphereLutSystem::recordTransmittance(VkCommandBuffer commandBuffer, DescriptorAllocator& descriptors, GpuProfiler* profiler) {
    if (!isDirty(LutNode::Transmittance) || transmittancePipeline_ == nullptr || transmittanceLut_.handle() == VK_NULL_HANDLE) {
        return;
    }
    if (profiler != nullptr) {
        profiler->write(commandBuffer, GpuProfiler::AtmosphereTransmittanceStart, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT);
    }

    barrier::cmdTransitionImage(commandBuffer, {
        .image = transmittanceLut_.handle(),
        .oldLayout = transmittanceLut_.layout(),
        .newLayout = VK_IMAGE_LAYOUT_GENERAL,
        .range = transmittanceLut_.fullRange(),
        .srcStage = transmittanceLut_.layout() == VK_IMAGE_LAYOUT_UNDEFINED ? VK_PIPELINE_STAGE_2_NONE : VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
        .srcAccess = transmittanceLut_.layout() == VK_IMAGE_LAYOUT_UNDEFINED ? VK_ACCESS_2_NONE : VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
        .dstStage = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
        .dstAccess = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
    });
    transmittanceLut_.setLayout(VK_IMAGE_LAYOUT_GENERAL);

    DescriptorSet set = descriptors.allocate(transmittanceSetLayout_);
    DescriptorWriter()
        .writeImage(0, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, transmittanceLut_.storageDescriptor())
        .update(device_, set);

    const AtmosphereUniform push = model_.uniform();
    transmittancePipeline_->bind(commandBuffer);
    vkCmdPushConstants(commandBuffer, transmittancePipeline_->layout(), VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(push), &push);
    const VkDescriptorSet descriptorSet = set.handle();
    vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, transmittancePipeline_->layout(), 0, 1, &descriptorSet, 0, nullptr);
    transmittancePipeline_->dispatch(commandBuffer, transmittanceLut_.width(), transmittanceLut_.height(), 8, 8);

    barrier::cmdTransitionImage(commandBuffer, {
        .image = transmittanceLut_.handle(),
        .oldLayout = transmittanceLut_.layout(),
        .newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
        .range = transmittanceLut_.fullRange(),
        .srcStage = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
        .srcAccess = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
        .dstStage = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
        .dstAccess = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
    });
    transmittanceLut_.setLayout(VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    clearDirty(LutNode::Transmittance);
    markGenerated(LutNode::Transmittance);
    transmittanceReady_ = true;
    if (profiler != nullptr) {
        profiler->write(commandBuffer, GpuProfiler::AtmosphereTransmittanceEnd, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT);
    }
}

void AtmosphereLutSystem::recordMultiScatter(VkCommandBuffer commandBuffer, DescriptorAllocator& descriptors, GpuProfiler* profiler) {
    if (!isDirty(LutNode::MultiScatter) || !transmittanceReady_ || multiScatterPipeline_ == nullptr || multiScatterLut_.handle() == VK_NULL_HANDLE) {
        return;
    }
    if (profiler != nullptr) {
        profiler->write(commandBuffer, GpuProfiler::AtmosphereMultiScatterStart, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT);
    }

    barrier::cmdTransitionImage(commandBuffer, {
        .image = multiScatterLut_.handle(),
        .oldLayout = multiScatterLut_.layout(),
        .newLayout = VK_IMAGE_LAYOUT_GENERAL,
        .range = multiScatterLut_.fullRange(),
        .srcStage = multiScatterLut_.layout() == VK_IMAGE_LAYOUT_UNDEFINED ? VK_PIPELINE_STAGE_2_NONE : VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
        .srcAccess = multiScatterLut_.layout() == VK_IMAGE_LAYOUT_UNDEFINED ? VK_ACCESS_2_NONE : VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
        .dstStage = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
        .dstAccess = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
    });
    multiScatterLut_.setLayout(VK_IMAGE_LAYOUT_GENERAL);

    DescriptorSet set = descriptors.allocate(multiScatterSetLayout_);
    DescriptorWriter()
        .writeImage(0, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, transmittanceLut_.sampledDescriptor(VK_NULL_HANDLE))
        .writeImage(1, VK_DESCRIPTOR_TYPE_SAMPLER, VkDescriptorImageInfo{.sampler = sampler_})
        .writeImage(2, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, multiScatterLut_.storageDescriptor())
        .update(device_, set);

    struct MultiScatterPush {
        glm::vec4 planetRadiusAtmosphereRadius{};
        glm::vec4 rayleighScattering{};
        glm::vec4 mieScatteringAnisotropy{};
        glm::vec4 absorptionGroundAlbedo{};
        glm::vec4 scaleHeights{};
        glm::vec4 sunElevationIntensity{};
    };

    const AtmosphereUniform atmosphere = model_.uniform();
    const MultiScatterPush push{
        .planetRadiusAtmosphereRadius = atmosphere.planetRadiusAtmosphereRadius,
        .rayleighScattering = atmosphere.rayleighScattering,
        .mieScatteringAnisotropy = atmosphere.mieScatteringAnisotropy,
        .absorptionGroundAlbedo = atmosphere.absorptionGroundAlbedo,
        .scaleHeights = atmosphere.scaleHeights,
        .sunElevationIntensity = glm::vec4(sunElevation_, skyIntensity_, sunAzimuth_, 0.0f),
    };

    multiScatterPipeline_->bind(commandBuffer);
    vkCmdPushConstants(commandBuffer, multiScatterPipeline_->layout(), VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(push), &push);
    const VkDescriptorSet descriptorSet = set.handle();
    vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, multiScatterPipeline_->layout(), 0, 1, &descriptorSet, 0, nullptr);
    multiScatterPipeline_->dispatch(commandBuffer, multiScatterLut_.width(), multiScatterLut_.height(), 8, 8);

    barrier::cmdTransitionImage(commandBuffer, {
        .image = multiScatterLut_.handle(),
        .oldLayout = multiScatterLut_.layout(),
        .newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
        .range = multiScatterLut_.fullRange(),
        .srcStage = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
        .srcAccess = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
        .dstStage = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
        .dstAccess = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
    });
    multiScatterLut_.setLayout(VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    clearDirty(LutNode::MultiScatter);
    markGenerated(LutNode::MultiScatter);
    multiScatterReady_ = true;
    if (profiler != nullptr) {
        profiler->write(commandBuffer, GpuProfiler::AtmosphereMultiScatterEnd, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT);
    }
}

void AtmosphereLutSystem::recordSkyView(VkCommandBuffer commandBuffer, DescriptorAllocator& descriptors, GpuProfiler* profiler) {
    if (!isDirty(LutNode::SkyView) || !transmittanceReady_ || !multiScatterReady_ || skyViewPipeline_ == nullptr || rawSkyViewLut_.handle() == VK_NULL_HANDLE) {
        return;
    }
    if (profiler != nullptr) {
        profiler->write(commandBuffer, GpuProfiler::AtmosphereSkyViewStart, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT);
    }

    barrier::cmdTransitionImage(commandBuffer, {
        .image = rawSkyViewLut_.handle(),
        .oldLayout = rawSkyViewLut_.layout(),
        .newLayout = VK_IMAGE_LAYOUT_GENERAL,
        .range = rawSkyViewLut_.fullRange(),
        .srcStage = rawSkyViewLut_.layout() == VK_IMAGE_LAYOUT_UNDEFINED ? VK_PIPELINE_STAGE_2_NONE : VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
        .srcAccess = rawSkyViewLut_.layout() == VK_IMAGE_LAYOUT_UNDEFINED ? VK_ACCESS_2_NONE : VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
        .dstStage = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
        .dstAccess = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
    });
    rawSkyViewLut_.setLayout(VK_IMAGE_LAYOUT_GENERAL);

    DescriptorSet set = descriptors.allocate(skyViewSetLayout_);
    DescriptorWriter()
        .writeImage(0, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, transmittanceLut_.sampledDescriptor(VK_NULL_HANDLE))
        .writeImage(1, VK_DESCRIPTOR_TYPE_SAMPLER, VkDescriptorImageInfo{.sampler = sampler_})
        .writeImage(2, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, rawSkyViewLut_.storageDescriptor())
        .writeImage(3, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, multiScatterLut_.sampledDescriptor(VK_NULL_HANDLE))
        .update(device_, set);

    struct SkyViewPush {
        glm::vec4 planetRadiusAtmosphereRadius{};
        glm::vec4 rayleighScattering{};
        glm::vec4 mieScatteringAnisotropy{};
        glm::vec4 absorptionGroundAlbedo{};
        glm::vec4 scaleHeights{};
        glm::vec4 sunElevationIntensity{};
    };

    const AtmosphereUniform atmosphere = model_.uniform();
    const SkyViewPush push{
        .planetRadiusAtmosphereRadius = atmosphere.planetRadiusAtmosphereRadius,
        .rayleighScattering = atmosphere.rayleighScattering,
        .mieScatteringAnisotropy = atmosphere.mieScatteringAnisotropy,
        .absorptionGroundAlbedo = atmosphere.absorptionGroundAlbedo,
        .scaleHeights = atmosphere.scaleHeights,
        .sunElevationIntensity = glm::vec4(sunElevation_, skyIntensity_, sunAzimuth_, 0.0f),
    };

    skyViewPipeline_->bind(commandBuffer);
    vkCmdPushConstants(commandBuffer, skyViewPipeline_->layout(), VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(push), &push);
    const VkDescriptorSet descriptorSet = set.handle();
    vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, skyViewPipeline_->layout(), 0, 1, &descriptorSet, 0, nullptr);
    skyViewPipeline_->dispatch(commandBuffer, rawSkyViewLut_.width(), rawSkyViewLut_.height(), 8, 8);

    barrier::cmdTransitionImage(commandBuffer, {
        .image = rawSkyViewLut_.handle(),
        .oldLayout = rawSkyViewLut_.layout(),
        .newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
        .range = rawSkyViewLut_.fullRange(),
        .srcStage = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
        .srcAccess = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
        .dstStage = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
        .dstAccess = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
    });
    rawSkyViewLut_.setLayout(VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    if (profiler != nullptr) {
        profiler->write(commandBuffer, GpuProfiler::AtmosphereSkyViewEnd, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT);
    }
    recordSkyViewReproject(commandBuffer, descriptors, profiler);
    if (samplingSystem_ != nullptr) {
        samplingSystem_->record(commandBuffer, descriptors, profiler);
    }
    clearDirty(LutNode::SkyView);
    markGenerated(LutNode::SkyView);
    skyViewReady_ = true;
}

void AtmosphereLutSystem::recordSkyViewReproject(VkCommandBuffer commandBuffer, DescriptorAllocator& descriptors, GpuProfiler* profiler) {
    if (skyReprojectPipeline_ == nullptr || skyViewLut_.handle() == VK_NULL_HANDLE || rawSkyViewLut_.handle() == VK_NULL_HANDLE) {
        return;
    }
    if (profiler != nullptr) {
        profiler->write(commandBuffer, GpuProfiler::AtmosphereSkyReprojectStart, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT);
    }

    if (previousSkyViewReady_) {
        barrier::cmdTransitionImage(commandBuffer, {
            .image = previousSkyViewLut_.handle(),
            .oldLayout = previousSkyViewLut_.layout(),
            .newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            .range = previousSkyViewLut_.fullRange(),
            .srcStage = previousSkyViewLut_.layout() == VK_IMAGE_LAYOUT_UNDEFINED ? VK_PIPELINE_STAGE_2_NONE : VK_PIPELINE_STAGE_2_TRANSFER_BIT,
            .srcAccess = previousSkyViewLut_.layout() == VK_IMAGE_LAYOUT_UNDEFINED ? VK_ACCESS_2_NONE : VK_ACCESS_2_TRANSFER_WRITE_BIT,
            .dstStage = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
            .dstAccess = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
        });
        previousSkyViewLut_.setLayout(VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    }

    barrier::cmdTransitionImage(commandBuffer, {
        .image = skyViewLut_.handle(),
        .oldLayout = skyViewLut_.layout(),
        .newLayout = VK_IMAGE_LAYOUT_GENERAL,
        .range = skyViewLut_.fullRange(),
        .srcStage = skyViewLut_.layout() == VK_IMAGE_LAYOUT_UNDEFINED ? VK_PIPELINE_STAGE_2_NONE : VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
        .srcAccess = skyViewLut_.layout() == VK_IMAGE_LAYOUT_UNDEFINED ? VK_ACCESS_2_NONE : VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
        .dstStage = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
        .dstAccess = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
    });
    skyViewLut_.setLayout(VK_IMAGE_LAYOUT_GENERAL);

    DescriptorSet set = descriptors.allocate(skyReprojectSetLayout_);
    DescriptorWriter()
        .writeImage(0, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, rawSkyViewLut_.sampledDescriptor(VK_NULL_HANDLE))
        .writeImage(1, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, previousSkyViewReady_ ? previousSkyViewLut_.sampledDescriptor(VK_NULL_HANDLE) : rawSkyViewLut_.sampledDescriptor(VK_NULL_HANDLE))
        .writeImage(2, VK_DESCRIPTOR_TYPE_SAMPLER, VkDescriptorImageInfo{.sampler = sampler_})
        .writeImage(3, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, skyViewLut_.storageDescriptor())
        .update(device_, set);

    struct SkyReprojectPush {
        uint32_t hasHistory = 0;
        float historyWeight = 0.9f;
        float varianceGamma = 1.2f;
        uint32_t padding = 0;
    };
    const SkyReprojectPush push{
        .hasHistory = previousSkyViewReady_ ? 1u : 0u,
    };

    skyReprojectPipeline_->bind(commandBuffer);
    vkCmdPushConstants(commandBuffer, skyReprojectPipeline_->layout(), VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(push), &push);
    const VkDescriptorSet descriptorSet = set.handle();
    vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, skyReprojectPipeline_->layout(), 0, 1, &descriptorSet, 0, nullptr);
    skyReprojectPipeline_->dispatch(commandBuffer, skyViewLut_.width(), skyViewLut_.height(), 8, 8);

    barrier::cmdTransitionImage(commandBuffer, {
        .image = skyViewLut_.handle(),
        .oldLayout = skyViewLut_.layout(),
        .newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
        .range = skyViewLut_.fullRange(),
        .srcStage = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
        .srcAccess = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
        .dstStage = VK_PIPELINE_STAGE_2_TRANSFER_BIT,
        .dstAccess = VK_ACCESS_2_TRANSFER_READ_BIT,
    });
    skyViewLut_.setLayout(VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    barrier::cmdTransitionImage(commandBuffer, {
        .image = previousSkyViewLut_.handle(),
        .oldLayout = previousSkyViewLut_.layout(),
        .newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        .range = previousSkyViewLut_.fullRange(),
        .srcStage = previousSkyViewLut_.layout() == VK_IMAGE_LAYOUT_UNDEFINED ? VK_PIPELINE_STAGE_2_NONE : VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
        .srcAccess = previousSkyViewLut_.layout() == VK_IMAGE_LAYOUT_UNDEFINED ? VK_ACCESS_2_NONE : VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
        .dstStage = VK_PIPELINE_STAGE_2_TRANSFER_BIT,
        .dstAccess = VK_ACCESS_2_TRANSFER_WRITE_BIT,
    });
    previousSkyViewLut_.setLayout(VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    VkImageCopy copy{};
    copy.srcSubresource = VkImageSubresourceLayers{VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    copy.dstSubresource = VkImageSubresourceLayers{VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    copy.extent = skyViewLut_.extent();
    vkCmdCopyImage(
        commandBuffer,
        skyViewLut_.handle(),
        VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
        previousSkyViewLut_.handle(),
        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        1,
        &copy);
    previousSkyViewReady_ = true;
    barrier::cmdTransitionImage(commandBuffer, {
        .image = skyViewLut_.handle(),
        .oldLayout = skyViewLut_.layout(),
        .newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
        .range = skyViewLut_.fullRange(),
        .srcStage = VK_PIPELINE_STAGE_2_TRANSFER_BIT,
        .srcAccess = VK_ACCESS_2_TRANSFER_READ_BIT,
        .dstStage = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
        .dstAccess = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
    });
    skyViewLut_.setLayout(VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    if (profiler != nullptr) {
        profiler->write(commandBuffer, GpuProfiler::AtmosphereSkyReprojectEnd, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT);
    }
}

void AtmosphereLutSystem::recordAerialPerspective(VkCommandBuffer commandBuffer, DescriptorAllocator& descriptors, GpuProfiler* profiler) {
    if (!isDirty(LutNode::AerialPerspective) || !transmittanceReady_ || !skyViewReady_ || aerialPerspectivePipeline_ == nullptr || aerialPerspectiveLut_.handle() == VK_NULL_HANDLE) {
        return;
    }
    if (profiler != nullptr) {
        profiler->write(commandBuffer, GpuProfiler::AtmosphereAerialPerspectiveStart, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT);
    }

    barrier::cmdTransitionImage(commandBuffer, {
        .image = aerialPerspectiveLut_.handle(),
        .oldLayout = aerialPerspectiveLut_.layout(),
        .newLayout = VK_IMAGE_LAYOUT_GENERAL,
        .range = aerialPerspectiveLut_.fullRange(),
        .srcStage = aerialPerspectiveLut_.layout() == VK_IMAGE_LAYOUT_UNDEFINED ? VK_PIPELINE_STAGE_2_NONE : VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
        .srcAccess = aerialPerspectiveLut_.layout() == VK_IMAGE_LAYOUT_UNDEFINED ? VK_ACCESS_2_NONE : VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
        .dstStage = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
        .dstAccess = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
    });
    aerialPerspectiveLut_.setLayout(VK_IMAGE_LAYOUT_GENERAL);

    DescriptorSet set = descriptors.allocate(aerialPerspectiveSetLayout_);
    DescriptorWriter()
        .writeImage(0, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, transmittanceLut_.sampledDescriptor(VK_NULL_HANDLE))
        .writeImage(1, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, skyViewLut_.sampledDescriptor(VK_NULL_HANDLE))
        .writeImage(2, VK_DESCRIPTOR_TYPE_SAMPLER, VkDescriptorImageInfo{.sampler = sampler_})
        .writeImage(3, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, aerialPerspectiveLut_.storageDescriptor())
        .update(device_, set);

    struct AerialPush {
        glm::vec4 planetRadiusAtmosphereRadius{};
        glm::vec4 rayleighScattering{};
        glm::vec4 mieScatteringAnisotropy{};
        glm::vec4 absorptionGroundAlbedo{};
        glm::vec4 scaleHeights{};
        glm::vec4 sunElevationIntensity{};
    };

    const AtmosphereUniform atmosphere = model_.uniform();
    const AerialPush push{
        .planetRadiusAtmosphereRadius = atmosphere.planetRadiusAtmosphereRadius,
        .rayleighScattering = atmosphere.rayleighScattering,
        .mieScatteringAnisotropy = atmosphere.mieScatteringAnisotropy,
        .absorptionGroundAlbedo = atmosphere.absorptionGroundAlbedo,
        .scaleHeights = atmosphere.scaleHeights,
        .sunElevationIntensity = glm::vec4(sunElevation_, skyIntensity_, sunAzimuth_, 0.0f),
    };

    aerialPerspectivePipeline_->bind(commandBuffer);
    vkCmdPushConstants(commandBuffer, aerialPerspectivePipeline_->layout(), VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(push), &push);
    const VkDescriptorSet descriptorSet = set.handle();
    vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, aerialPerspectivePipeline_->layout(), 0, 1, &descriptorSet, 0, nullptr);
    const uint32_t groupsX = (aerialPerspectiveLut_.width() + 7) / 8;
    const uint32_t groupsY = (aerialPerspectiveLut_.height() + 7) / 8;
    const uint32_t groupsZ = (aerialPerspectiveLut_.extent().depth + 3) / 4;
    vkCmdDispatch(commandBuffer, groupsX, groupsY, groupsZ);

    barrier::cmdTransitionImage(commandBuffer, {
        .image = aerialPerspectiveLut_.handle(),
        .oldLayout = aerialPerspectiveLut_.layout(),
        .newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
        .range = aerialPerspectiveLut_.fullRange(),
        .srcStage = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
        .srcAccess = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
        .dstStage = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
        .dstAccess = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
    });
    aerialPerspectiveLut_.setLayout(VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    clearDirty(LutNode::AerialPerspective);
    markGenerated(LutNode::AerialPerspective);
    if (profiler != nullptr) {
        profiler->write(commandBuffer, GpuProfiler::AtmosphereAerialPerspectiveEnd, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT);
    }
}

} // namespace rtv
