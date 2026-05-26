#pragma once

#include "rtv/PathTracerRenderer.h"

#include <Volk/volk.h>
#include <glm/glm.hpp>

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace rtv {

class TemporalSystem {
public:
    enum class TemporalResidency : uint8_t {
        Persistent,
        Evictable,
        HalfResolution,
        DynamicResolution,
    };

    struct ReprojectResult {
        glm::vec2 prevUv{};
        bool valid = false;
        float confidence = 0.0f;
    };

    struct HistorySlot {
        std::string name;
        VkFormat format = VK_FORMAT_UNDEFINED;
        VkExtent2D extent{};
        bool valid = false;
        bool resident = true;
        uint64_t lastWrittenFrame = 0;
        uint64_t estimatedBytes = 0;
        TemporalResidency residency = TemporalResidency::Persistent;
        float memoryBudgetWeight = 1.0f;
    };

    struct ConfidenceInput {
        float luminanceVariance = 0.0f;
        float depthDisocclusion = 0.0f;
        float normalDisocclusion = 0.0f;
        float motionPixels = 0.0f;
        bool historyValid = true;
    };

    struct ConfidenceResult {
        float confidence = 0.0f;
        float reactive = 0.0f;
        float blendFactor = 1.0f;
    };

    struct SpecularVirtualMotionInput {
        glm::vec3 surfacePosition{};
        glm::vec3 surfaceNormal{0.0f, 1.0f, 0.0f};
        glm::vec3 cameraPosition{};
        glm::mat4 currentViewProj{1.0f};
        glm::mat4 previousViewProj{1.0f};
        glm::vec2 currentPixel{};
        glm::vec2 renderSize{};
        glm::vec2 surfaceVelocityPixels{};
        float secondaryHitDistance = 0.0f;
        float roughness = 1.0f;
        float specularSignal = 0.0f;
    };

    struct SpecularVirtualMotionResult {
        glm::vec2 velocityPixels{};
        glm::vec2 historyPixel{};
        bool valid = false;
        float confidence = 0.0f;
    };

    [[nodiscard]] static ReprojectResult reproject(glm::vec2 uv, glm::vec2 velocityPixels, glm::vec2 renderSize);
    [[nodiscard]] static ConfidenceResult evaluateConfidence(const ConfidenceInput& input);
    [[nodiscard]] static SpecularVirtualMotionResult estimateSpecularVirtualMotion(const SpecularVirtualMotionInput& input);
    [[nodiscard]] static float effectiveHistoryLength(float historyWeight);
    [[nodiscard]] static float reactiveWeight(float localContrast, float luminance);
    [[nodiscard]] static glm::vec3 clampHistoryYCoCg(glm::vec3 history, glm::vec3 minColor, glm::vec3 maxColor, float sigmaLuminance);

    HistorySlot& createHistorySlot(
        std::string name,
        VkFormat format,
        VkExtent2D extent,
        TemporalResidency residency = TemporalResidency::Persistent,
        float memoryBudgetWeight = 1.0f);
    void markSlotWritten(const std::string& name);
    void invalidateSlot(const std::string& name);
    void invalidateAll();
    void evictToBudget(uint64_t maxHistoryMemoryBytes);

    void beginFrame(uint64_t frameIndex);
    void endFrame();
    void setCameraCut(bool cut, AccumulationResetReason reason = AccumulationResetReason::Manual);

    [[nodiscard]] bool isCameraCut() const { return cameraCut_; }
    [[nodiscard]] AccumulationResetReason lastResetReason() const { return resetReason_; }
    [[nodiscard]] uint64_t frameIndex() const { return frameIndex_; }
    [[nodiscard]] uint64_t totalHistoryMemoryBytes() const { return totalHistoryMemoryBytes_; }
    [[nodiscard]] const std::unordered_map<std::string, HistorySlot>& historySlots() const { return historySlots_; }

private:
    void recomputeHistoryMemory();

    std::unordered_map<std::string, HistorySlot> historySlots_;
    bool cameraCut_ = true;
    AccumulationResetReason resetReason_ = AccumulationResetReason::Startup;
    uint64_t frameIndex_ = 0;
    uint64_t totalHistoryMemoryBytes_ = 0;
};

} // namespace rtv
