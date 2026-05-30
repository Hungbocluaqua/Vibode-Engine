#include "rtv/SkySystem.h"

#include "rtv/DescriptorAllocator.h"
#include "rtv/DescriptorLayoutCache.h"
#include "rtv/PipelineCache.h"
#include "rtv/ResourceAllocator.h"
#include "rtv/ShaderModule.h"

namespace rtv {

SkySystem::SkySystem(
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
    : lutSystem_(
        device,
        allocator,
        layoutCache,
        pipelineCache,
        transmittanceShader,
        multiScatterShader,
        skyViewShader,
        skyReprojectShader,
        aerialPerspectiveShader,
        skyCdfShader) {}

SkySystem::~SkySystem() = default;

void SkySystem::record(VkCommandBuffer commandBuffer, DescriptorAllocator& descriptors) {
    lutSystem_.record(commandBuffer, descriptors);
}

void SkySystem::setSunElevation(float elevation) {
    sunElevation_ = elevation;
    lutSystem_.setSkyParameters(sunElevation_, sunAzimuth_, skyIntensity_);
    temporalSystem_.markDirty(AtmosphereDirtyBit::SunDirection);
}

void SkySystem::setSunAzimuth(float azimuth) {
    sunAzimuth_ = azimuth;
    lutSystem_.setSkyParameters(sunElevation_, sunAzimuth_, skyIntensity_);
    temporalSystem_.markDirty(AtmosphereDirtyBit::SunDirection);
}

void SkySystem::setSkyIntensity(float intensity) {
    skyIntensity_ = intensity;
    lutSystem_.setSkyParameters(sunElevation_, sunAzimuth_, skyIntensity_);
}

void SkySystem::markDirty() {
    lutSystem_.markDirty();
    temporalSystem_.markDirtyAll();
}

} // namespace rtv
