#pragma once

#include "rtv/AtmosphereModel.h"
#include "rtv/Image.h"
#include "rtv/NonCopyable.h"

#include <Volk/volk.h>

#include <array>
#include <cstdint>
#include <cstddef>
#include <memory>

namespace rtv {

class ComputePipeline;
class DescriptorAllocator;
class DescriptorLayoutCache;
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
        const ShaderModule& aerialPerspectiveShader);
    ~AtmosphereLutSystem();

    void record(VkCommandBuffer commandBuffer, DescriptorAllocator& descriptors);
    void setSkyParameters(float sunElevation, float skyIntensity);
    void markDirty();

    [[nodiscard]] const Image& transmittanceLut() const { return transmittanceLut_; }
    [[nodiscard]] const Image& multiScatterLut() const { return multiScatterLut_; }
    [[nodiscard]] const Image& skyViewLut() const { return skyViewLut_; }
    [[nodiscard]] const Image& aerialPerspectiveLut() const { return aerialPerspectiveLut_; }
    [[nodiscard]] VkSampler sampler() const { return sampler_; }
    [[nodiscard]] bool transmittanceReady() const { return transmittanceReady_; }
    [[nodiscard]] bool skyViewReady() const { return skyViewReady_; }
    [[nodiscard]] AtmosphereLutStats stats() const { return stats_; }

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

    void recordTransmittance(VkCommandBuffer commandBuffer, DescriptorAllocator& descriptors);
    void recordMultiScatter(VkCommandBuffer commandBuffer, DescriptorAllocator& descriptors);
    void recordSkyView(VkCommandBuffer commandBuffer, DescriptorAllocator& descriptors);
    void recordAerialPerspective(VkCommandBuffer commandBuffer, DescriptorAllocator& descriptors);

    VkDevice device_ = VK_NULL_HANDLE;
    AtmosphereModel model_{};
    Image transmittanceLut_;
    Image multiScatterLut_;
    Image skyViewLut_;
    Image aerialPerspectiveLut_;
    VkSampler sampler_ = VK_NULL_HANDLE;
    VkDescriptorSetLayout transmittanceSetLayout_ = VK_NULL_HANDLE;
    VkDescriptorSetLayout multiScatterSetLayout_ = VK_NULL_HANDLE;
    VkDescriptorSetLayout skyViewSetLayout_ = VK_NULL_HANDLE;
    VkDescriptorSetLayout aerialPerspectiveSetLayout_ = VK_NULL_HANDLE;
    std::unique_ptr<ComputePipeline> transmittancePipeline_;
    std::unique_ptr<ComputePipeline> multiScatterPipeline_;
    std::unique_ptr<ComputePipeline> skyViewPipeline_;
    std::unique_ptr<ComputePipeline> aerialPerspectivePipeline_;
    AtmosphereLutStats stats_{};
    bool transmittanceReady_ = false;
    bool multiScatterReady_ = false;
    bool skyViewReady_ = false;
    float sunElevation_ = 0.97f;
    float skyIntensity_ = 0.8f;
};

} // namespace rtv
