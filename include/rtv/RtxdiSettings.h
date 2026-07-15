#pragma once

#include <cstdint>
#include <cctype>
#include <string>
#include <string_view>

namespace rtv {

enum class RendererPipelineMode : uint32_t {
    LegacyPathTracer = 0,
    HybridRtxdi = 1,
    PathTracerRtxdi = 2,
};

enum class RtxdiQualityPreset : uint32_t {
    Fast = 0,
    Medium = 1,
    Unbiased = 2,
    Ultra = 3,
    Reference = 4,
};

[[nodiscard]] constexpr const char* rendererPipelineModeName(RendererPipelineMode mode) {
    switch (mode) {
    case RendererPipelineMode::LegacyPathTracer: return "legacy-path-tracer";
    case RendererPipelineMode::HybridRtxdi: return "hybrid-rtxdi";
    case RendererPipelineMode::PathTracerRtxdi: return "path-tracer-rtxdi";
    }
    return "legacy-path-tracer";
}

[[nodiscard]] constexpr const char* rtxdiQualityPresetName(RtxdiQualityPreset preset) {
    switch (preset) {
    case RtxdiQualityPreset::Fast: return "fast";
    case RtxdiQualityPreset::Medium: return "medium";
    case RtxdiQualityPreset::Unbiased: return "unbiased";
    case RtxdiQualityPreset::Ultra: return "ultra";
    case RtxdiQualityPreset::Reference: return "reference";
    }
    return "medium";
}

[[nodiscard]] inline std::string normalizeRtxdiSettingName(std::string_view value) {
    std::string result;
    result.reserve(value.size());
    for (unsigned char ch : value) {
        if (std::isalnum(ch) != 0) {
            result.push_back(static_cast<char>(std::tolower(ch)));
        }
    }
    return result;
}

[[nodiscard]] inline RendererPipelineMode parseRendererPipelineMode(std::string_view value) {
    const std::string key = normalizeRtxdiSettingName(value);
    if (key == "hybrid" || key == "hybridrtxdi" || key == "rtxdi") {
        return RendererPipelineMode::HybridRtxdi;
    }
    if (key == "pathtracer" || key == "pathtracerrtxdi" || key == "restirpt" || key == "rtxdipt") {
        return RendererPipelineMode::PathTracerRtxdi;
    }
    return RendererPipelineMode::LegacyPathTracer;
}

[[nodiscard]] inline RtxdiQualityPreset parseRtxdiQualityPreset(std::string_view value) {
    const std::string key = normalizeRtxdiSettingName(value);
    if (key == "fast" || key == "performance") return RtxdiQualityPreset::Fast;
    if (key == "unbiased") return RtxdiQualityPreset::Unbiased;
    if (key == "ultra" || key == "quality") return RtxdiQualityPreset::Ultra;
    if (key == "reference" || key == "ref") return RtxdiQualityPreset::Reference;
    return RtxdiQualityPreset::Medium;
}

} // namespace rtv
