#include "rtv/TemporalSystem.h"

#include <algorithm>
#include <cmath>
#include <utility>

namespace rtv {

namespace {

uint32_t bytesPerPixel(VkFormat format) {
    switch (format) {
    case VK_FORMAT_R8_UNORM:
    case VK_FORMAT_R8_UINT:
        return 1;
    case VK_FORMAT_R16_SFLOAT:
    case VK_FORMAT_R16_UNORM:
    case VK_FORMAT_R16_UINT:
    case VK_FORMAT_R8G8_UNORM:
        return 2;
    case VK_FORMAT_R16G16_SFLOAT:
    case VK_FORMAT_R32_SFLOAT:
    case VK_FORMAT_R8G8B8A8_UNORM:
    case VK_FORMAT_B8G8R8A8_UNORM:
        return 4;
    case VK_FORMAT_R16G16B16A16_SFLOAT:
    case VK_FORMAT_R32G32_SFLOAT:
        return 8;
    case VK_FORMAT_R32G32B32A32_SFLOAT:
        return 16;
    default:
        return 4;
    }
}

uint64_t estimateHistoryBytes(VkFormat format, VkExtent2D extent, TemporalSystem::TemporalResidency residency) {
    uint64_t width = extent.width;
    uint64_t height = extent.height;
    if (residency == TemporalSystem::TemporalResidency::HalfResolution) {
        width = std::max<uint64_t>(1, (width + 1) / 2);
        height = std::max<uint64_t>(1, (height + 1) / 2);
    }
    return width * height * bytesPerPixel(format);
}

glm::vec3 rgbToYCoCg(glm::vec3 c) {
    const float y = c.r * 0.25f + c.g * 0.5f + c.b * 0.25f;
    const float co = c.r - c.b;
    const float cg = c.g - 0.5f * (c.r + c.b);
    return {y, co, cg};
}

glm::vec3 yCoCgToRgb(glm::vec3 c) {
    const float t = c.x - c.z * 0.5f;
    const float g = c.x + c.z * 0.5f;
    const float r = t + c.y * 0.5f;
    const float b = t - c.y * 0.5f;
    return {r, g, b};
}

} // namespace

TemporalSystem::ReprojectResult TemporalSystem::reproject(glm::vec2 uv, glm::vec2 velocityPixels, glm::vec2 renderSize) {
    if (renderSize.x <= 0.0f || renderSize.y <= 0.0f) {
        return {};
    }
    const glm::vec2 prevUv = uv - velocityPixels / renderSize;
    const bool valid = prevUv.x >= 0.0f && prevUv.y >= 0.0f && prevUv.x <= 1.0f && prevUv.y <= 1.0f;
    const float motionPixels = glm::length(velocityPixels);
    const float confidence = valid ? std::clamp(1.0f - motionPixels / 64.0f, 0.0f, 1.0f) : 0.0f;
    return ReprojectResult{
        .prevUv = prevUv,
        .valid = valid,
        .confidence = confidence,
    };
}

TemporalSystem::ConfidenceResult TemporalSystem::evaluateConfidence(const ConfidenceInput& input) {
    if (!input.historyValid) {
        return {};
    }
    const float varianceConfidence = std::exp(-std::max(input.luminanceVariance, 0.0f) * 2.0f);
    const float depthConfidence = std::exp(-std::max(input.depthDisocclusion, 0.0f) * 8.0f);
    const float normalConfidence = std::exp(-std::max(input.normalDisocclusion, 0.0f) * 4.0f);
    const float motionConfidence = std::clamp(1.0f - std::max(input.motionPixels, 0.0f) / 64.0f, 0.0f, 1.0f);
    const float confidence = std::clamp(varianceConfidence * depthConfidence * normalConfidence * motionConfidence, 0.0f, 1.0f);
    const float reactive = std::clamp((1.0f - varianceConfidence) + (1.0f - motionConfidence) * 0.5f, 0.0f, 1.0f);
    return ConfidenceResult{
        .confidence = confidence,
        .reactive = reactive,
        .blendFactor = std::clamp(1.0f - confidence, 0.04f, 0.92f),
    };
}

float TemporalSystem::reactiveWeight(float localContrast, float luminance) {
    const float normalizedContrast = localContrast / (std::max(luminance, 0.0f) + 0.02f);
    return std::clamp((normalizedContrast - 0.12f) * 2.5f, 0.0f, 1.0f);
}

glm::vec3 TemporalSystem::clampHistoryYCoCg(glm::vec3 history, glm::vec3 minColor, glm::vec3 maxColor, float sigmaLuminance) {
    const glm::vec3 historyYCoCg = rgbToYCoCg(history);
    const glm::vec3 minYCoCg = rgbToYCoCg(minColor);
    const glm::vec3 maxYCoCg = rgbToYCoCg(maxColor);
    const glm::vec3 lo = glm::min(minYCoCg, maxYCoCg) - glm::vec3(
        std::max(0.01f, sigmaLuminance * 0.25f),
        0.08f + sigmaLuminance * 0.20f,
        0.08f + sigmaLuminance * 0.20f);
    const glm::vec3 hi = glm::max(minYCoCg, maxYCoCg) + glm::vec3(
        std::max(0.01f, sigmaLuminance * 0.25f),
        0.08f + sigmaLuminance * 0.20f,
        0.08f + sigmaLuminance * 0.20f);
    return glm::max(yCoCgToRgb(glm::clamp(historyYCoCg, lo, hi)), glm::vec3(0.0f));
}

TemporalSystem::HistorySlot& TemporalSystem::createHistorySlot(
    std::string name,
    VkFormat format,
    VkExtent2D extent,
    TemporalResidency residency,
    float memoryBudgetWeight) {
    auto [it, inserted] = historySlots_.try_emplace(name);
    HistorySlot& slot = it->second;
    if (inserted) {
        slot.name = std::move(name);
    }
    slot.format = format;
    slot.extent = extent;
    slot.residency = residency;
    slot.memoryBudgetWeight = memoryBudgetWeight;
    slot.resident = true;
    slot.estimatedBytes = estimateHistoryBytes(format, extent, residency);
    recomputeHistoryMemory();
    return slot;
}

void TemporalSystem::markSlotWritten(const std::string& name) {
    auto it = historySlots_.find(name);
    if (it == historySlots_.end()) {
        return;
    }
    it->second.valid = true;
    it->second.resident = true;
    it->second.lastWrittenFrame = frameIndex_;
    recomputeHistoryMemory();
}

void TemporalSystem::invalidateSlot(const std::string& name) {
    auto it = historySlots_.find(name);
    if (it != historySlots_.end()) {
        it->second.valid = false;
    }
}

void TemporalSystem::invalidateAll() {
    for (auto& [name, slot] : historySlots_) {
        (void)name;
        slot.valid = false;
    }
}

void TemporalSystem::evictToBudget(uint64_t maxHistoryMemoryBytes) {
    recomputeHistoryMemory();
    if (totalHistoryMemoryBytes_ <= maxHistoryMemoryBytes) {
        return;
    }

    std::vector<HistorySlot*> candidates;
    candidates.reserve(historySlots_.size());
    for (auto& [name, slot] : historySlots_) {
        (void)name;
        if (slot.residency != TemporalResidency::Persistent && slot.resident) {
            candidates.push_back(&slot);
        }
    }
    std::sort(candidates.begin(), candidates.end(), [](const HistorySlot* a, const HistorySlot* b) {
        if (a->memoryBudgetWeight == b->memoryBudgetWeight) {
            return a->lastWrittenFrame < b->lastWrittenFrame;
        }
        return a->memoryBudgetWeight < b->memoryBudgetWeight;
    });
    for (HistorySlot* slot : candidates) {
        if (totalHistoryMemoryBytes_ <= maxHistoryMemoryBytes) {
            break;
        }
        slot->valid = false;
        slot->resident = false;
        recomputeHistoryMemory();
    }
}

void TemporalSystem::beginFrame(uint64_t frameIndex) {
    frameIndex_ = frameIndex;
}

void TemporalSystem::endFrame() {
    cameraCut_ = false;
}

void TemporalSystem::setCameraCut(bool cut, AccumulationResetReason reason) {
    cameraCut_ = cut;
    resetReason_ = reason;
    if (cut) {
        invalidateAll();
    }
}

void TemporalSystem::recomputeHistoryMemory() {
    totalHistoryMemoryBytes_ = 0;
    for (const auto& [name, slot] : historySlots_) {
        (void)name;
        if (slot.resident) {
            totalHistoryMemoryBytes_ += slot.estimatedBytes;
        }
    }
}

} // namespace rtv
