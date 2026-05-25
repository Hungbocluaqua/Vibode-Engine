#pragma once

#include "rtv/AtmosphereLutSystem.h"
#include "rtv/AtmosphereModel.h"
#include "rtv/AtmosphereSamplingSystem.h"
#include "rtv/NonCopyable.h"

#include <glm/glm.hpp>
#include <Volk/volk.h>

#include <cstdint>
#include <memory>

namespace rtv {

class DescriptorAllocator;
class DescriptorLayoutCache;
class PipelineCache;
class ResourceAllocator;
class ShaderModule;

enum class AtmosphereDirtyBit : uint32_t {
    Rayleigh = 1 << 0,
    Mie = 1 << 1,
    Ozone = 1 << 2,
    SunDirection = 1 << 3,
    Exposure = 1 << 4,
    Quality = 1 << 5,
};

inline uint32_t operator|(AtmosphereDirtyBit a, AtmosphereDirtyBit b) {
    return static_cast<uint32_t>(a) | static_cast<uint32_t>(b);
}

inline uint32_t operator|(uint32_t a, AtmosphereDirtyBit b) {
    return a | static_cast<uint32_t>(b);
}

inline uint32_t operator&(uint32_t a, AtmosphereDirtyBit b) {
    return a & static_cast<uint32_t>(b);
}

class AtmosphereTemporalSystem final : private NonCopyable {
public:
    AtmosphereTemporalSystem() = default;

    void markDirty(AtmosphereDirtyBit bit) { dirtyBits_ |= static_cast<uint32_t>(bit); traverseDag(); }
    void markDirtyExposure() { dirtyBits_ |= static_cast<uint32_t>(AtmosphereDirtyBit::Exposure); }
    void markDirtyAll() { dirtyBits_ = ~0u; traverseDag(); }

    [[nodiscard]] bool needsTransmittanceUpdate() const { return lutDirty_[kTransmittance]; }
    [[nodiscard]] bool needsMultiScatterUpdate() const { return lutDirty_[kMultiScatter]; }
    [[nodiscard]] bool needsSkyViewUpdate() const { return lutDirty_[kSkyView]; }
    [[nodiscard]] bool needsAerialPerspectiveUpdate() const { return lutDirty_[kAerial]; }
    [[nodiscard]] bool needsCdfUpdate() const { return lutDirty_[kCdf]; }

    void onTransmittanceUpdated() { lutDirty_[kTransmittance] = false; }
    void onMultiScatterUpdated() { lutDirty_[kMultiScatter] = false; }
    void onSkyViewUpdated() { lutDirty_[kSkyView] = false; lutDirty_[kCdf] = true; }
    void onAerialPerspectiveUpdated() { lutDirty_[kAerial] = false; }
    void onCdfUpdated() { lutDirty_[kCdf] = false; }

    [[nodiscard]] bool sunMoved() const { return (dirtyBits_ & static_cast<uint32_t>(AtmosphereDirtyBit::SunDirection)) != 0; }
    [[nodiscard]] uint32_t dirtyBits() const { return dirtyBits_; }

    void setCameraPosition(glm::vec3 position) {
        static constexpr float kCameraMoveRecomputeThreshold = 100.0f;
        if (!previousCameraPosSet_) {
            previousCameraPos_ = position;
            previousCameraPosSet_ = true;
            return;
        }
        float delta = glm::length(position - previousCameraPos_);
        if (delta > kCameraMoveRecomputeThreshold) {
            lutDirty_[kSkyView] = true;
            lutDirty_[kCdf] = true;
        }
        previousCameraPos_ = position;
    }

    [[nodiscard]] glm::vec3 previousCameraPos() const { return previousCameraPos_; }

private:
    static constexpr size_t kTransmittance = 0;
    static constexpr size_t kMultiScatter = 1;
    static constexpr size_t kSkyView = 2;
    static constexpr size_t kAerial = 3;
    static constexpr size_t kCdf = 4;

    static constexpr uint32_t kScatterBits =
        static_cast<uint32_t>(AtmosphereDirtyBit::Rayleigh) |
        static_cast<uint32_t>(AtmosphereDirtyBit::Mie) |
        static_cast<uint32_t>(AtmosphereDirtyBit::Ozone);

    static constexpr uint32_t kSkyViewBits = kScatterBits |
        static_cast<uint32_t>(AtmosphereDirtyBit::SunDirection) |
        static_cast<uint32_t>(AtmosphereDirtyBit::Quality);

    void traverseDag() {
        lutDirty_[kTransmittance]  = lutDirty_[kTransmittance]  || (dirtyBits_ & kScatterBits) != 0;
        lutDirty_[kMultiScatter]   = lutDirty_[kMultiScatter]   || (dirtyBits_ & kScatterBits) != 0;
        lutDirty_[kSkyView]        = lutDirty_[kSkyView]        || (dirtyBits_ & kSkyViewBits) != 0;
        lutDirty_[kAerial]         = lutDirty_[kAerial]         || (dirtyBits_ & kScatterBits) != 0;
        lutDirty_[kCdf]            = lutDirty_[kSkyView];
    }

    uint32_t dirtyBits_ = ~0u;
    bool lutDirty_[5] = {true, true, true, true, true};
    glm::vec3 previousCameraPos_{};
    bool previousCameraPosSet_ = false;
};

class SkySystem final : private NonCopyable {
public:
    SkySystem(
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
    ~SkySystem();

    void record(VkCommandBuffer commandBuffer, DescriptorAllocator& descriptors);
    void setSunElevation(float elevation);
    void setSunAzimuth(float azimuth);
    void setSkyIntensity(float intensity);
    void markDirty();

    [[nodiscard]] AtmosphereModel& model() { return model_; }
    [[nodiscard]] const AtmosphereModel& model() const { return model_; }
    [[nodiscard]] AtmosphereLutSystem& lutSystem() { return lutSystem_; }
    [[nodiscard]] const AtmosphereLutSystem& lutSystem() const { return lutSystem_; }
    [[nodiscard]] AtmosphereTemporalSystem& temporalSystem() { return temporalSystem_; }
    [[nodiscard]] const AtmosphereTemporalSystem& temporalSystem() const { return temporalSystem_; }

private:
    AtmosphereModel model_{};
    AtmosphereTemporalSystem temporalSystem_{};
    AtmosphereLutSystem lutSystem_;
    float sunElevation_ = 0.97f;
    float sunAzimuth_ = 0.0f;
    float skyIntensity_ = 0.8f;
};

} // namespace rtv
