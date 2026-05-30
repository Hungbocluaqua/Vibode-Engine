#pragma once

#include "rtv/AtmosphereModel.h"
#include "rtv/Image.h"
#include "rtv/NonCopyable.h"

#include <glm/glm.hpp>
#include <Volk/volk.h>

#include <array>
#include <cstdint>
#include <cstddef>
#include <memory>

namespace rtv {

class AtmosphereSamplingSystem;
class ComputePipeline;
class DescriptorAllocator;
class DescriptorLayoutCache;
class GpuProfiler;
class PipelineCache;
class ResourceAllocator;
class ShaderModule;

struct AtmosphereLutStats {
    std::array<bool, 4> dirty{};
    std::array<bool, 4> generatedThisRecord{};
    std::array<uint64_t, 4> generationCounts{};
};

class AtmosphereLutSystem final : private NonCopyable {
public:
    AtmosphereLutSystem(
        VkDevice device,
        ResourceAllocator& allocator,
        DescriptorLayoutCache& layoutCache,
        PipelineCache& pipelineCache,
        const ShaderModule& transmittanceShader,
        const ShaderModule& multiScatterShader,
        const ShaderModule& skyViewShader,
        const ShaderModule& skyReprojectShader,
        const ShaderModule& aerialPerspectiveShader,
        const ShaderModule& skyCdfShader);
    ~AtmosphereLutSystem();

    void record(VkCommandBuffer commandBuffer, DescriptorAllocator& descriptors, GpuProfiler* profiler = nullptr);
    void setSkyParameters(float sunElevation, float sunAzimuth, float skyIntensity);
    void setSkyDirection(glm::vec3 sunDirection, float skyIntensity);
    void setAtmosphereParams(float rayleighScaleHeight, float mieScaleHeight, float mieAnisotropy, float groundAlbedo);
    void setQuality(AtmosphereQuality quality);
    void setCameraPosition(glm::vec3 position);
    void markDirty();

    [[nodiscard]] const Image& transmittanceLut() const { return transmittanceLut_; }
    [[nodiscard]] const Image& multiScatterLut() const { return multiScatterLut_; }
    [[nodiscard]] const Image& skyViewLut() const { return skyViewLut_; }
    [[nodiscard]] const Image& aerialPerspectiveLut() const { return aerialPerspectiveLut_; }
    [[nodiscard]] VkSampler sampler() const { return sampler_; }
    [[nodiscard]] bool transmittanceReady() const { return transmittanceReady_; }
    [[nodiscard]] bool skyViewReady() const { return skyViewReady_; }
    [[nodiscard]] AtmosphereLutStats stats() const { return stats_; }
    [[nodiscard]] const AtmosphereSamplingSystem* samplingSystem() const { return samplingSystem_.get(); }

private:
    enum class LutNode : uint8_t {
        Transmittance = 0,
        MultiScatter = 1,
        SkyView = 2,
        AerialPerspective = 3,
    };

    [[nodiscard]] static constexpr size_t lutNodeIndex(LutNode node) {
        return static_cast<size_t>(node);
    }
    void markDirty(LutNode node);
    [[nodiscard]] bool isDirty(LutNode node) const;
    void clearDirty(LutNode node);
    void markGenerated(LutNode node);

    void recordTransmittance(VkCommandBuffer commandBuffer, DescriptorAllocator& descriptors, GpuProfiler* profiler);
    void recordMultiScatter(VkCommandBuffer commandBuffer, DescriptorAllocator& descriptors, GpuProfiler* profiler);
    void recordSkyView(VkCommandBuffer commandBuffer, DescriptorAllocator& descriptors, GpuProfiler* profiler);
    void recordSkyViewReproject(VkCommandBuffer commandBuffer, DescriptorAllocator& descriptors, GpuProfiler* profiler);
    void recordAerialPerspective(VkCommandBuffer commandBuffer, DescriptorAllocator& descriptors, GpuProfiler* profiler);

    VkDevice device_ = VK_NULL_HANDLE;
    ResourceAllocator& allocator_;
    AtmosphereModel model_{};
    Image transmittanceLut_;
    Image multiScatterLut_;
    Image skyViewLut_;
    Image rawSkyViewLut_;
    Image previousSkyViewLut_;
    Image aerialPerspectiveLut_;
    VkSampler sampler_ = VK_NULL_HANDLE;
    VkDescriptorSetLayout transmittanceSetLayout_ = VK_NULL_HANDLE;
    VkDescriptorSetLayout multiScatterSetLayout_ = VK_NULL_HANDLE;
    VkDescriptorSetLayout skyViewSetLayout_ = VK_NULL_HANDLE;
    VkDescriptorSetLayout skyReprojectSetLayout_ = VK_NULL_HANDLE;
    VkDescriptorSetLayout aerialPerspectiveSetLayout_ = VK_NULL_HANDLE;
    std::unique_ptr<ComputePipeline> transmittancePipeline_;
    std::unique_ptr<ComputePipeline> multiScatterPipeline_;
    std::unique_ptr<ComputePipeline> skyViewPipeline_;
    std::unique_ptr<ComputePipeline> skyReprojectPipeline_;
    std::unique_ptr<ComputePipeline> aerialPerspectivePipeline_;
    std::unique_ptr<AtmosphereSamplingSystem> samplingSystem_;
    AtmosphereLutStats stats_{};
    bool transmittanceReady_ = false;
    bool multiScatterReady_ = false;
    bool skyViewReady_ = false;
    bool previousSkyViewReady_ = false;
    float sunElevation_ = 0.97f;
    float sunAzimuth_ = 0.0f;
    float skyIntensity_ = 0.8f;
    uint32_t skyViewWidth_ = 256;
    uint32_t skyViewHeight_ = 144;
    AtmosphereQuality quality_ = AtmosphereQuality::High;
    glm::vec3 previousCameraPos_{};
    bool previousCameraPosSet_ = false;
};

} // namespace rtv
