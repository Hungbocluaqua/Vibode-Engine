#include "rtv/HeadlessDiagnostics.h"

#include "rtv/Application.h"
#include "rtv/DiagnosticImageExport.h"
#include "rtv/GpuProfiler.h"
#include "rtv/GpuValidation.h"
#include "rtv/PathTracerRenderer.h"
#include "rtv/RenderGraphDump.h"
#include "rtv/RenderGraph.h"
#include "rtv/ResourceAllocator.h"
#include "rtv/Swapchain.h"
#include "rtv/VulkanContext.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <numeric>
#include <sstream>
#include <stdexcept>
#include <vector>

namespace rtv {

void to_json(nlohmann::json& j, const ProfileReport::Resolution& r) {
    j["render_extent"] = {{"width", r.renderWidth}, {"height", r.renderHeight}};
    j["display_extent"] = {{"width", r.displayWidth}, {"height", r.displayHeight}};
    j["render_scale"] = r.renderScale;
}

void to_json(nlohmann::json& j, const ProfileReport::MinMaxAvg& m) {
    j["min"] = m.min;
    j["avg"] = m.avg;
    j["max"] = m.max;
    j["p95"] = m.p95;
    j["p99"] = m.p99;
}

void to_json(nlohmann::json& j, const ProfileReport::PerPassGpuMs& p) {
    j["path_trace"] = p.pathTrace;
    j["restir_history_clear"] = p.restirHistoryClear;
    j["restir_gi_clear"] = p.restirGiClear;
    j["restir_spatial"] = p.restirSpatial;
    j["restir_spatial_copy"] = p.restirSpatialCopy;
    j["restir_gi_spatial"] = p.restirGiSpatial;
    j["restir_gi_final"] = p.restirGiFinal;
    j["fog_integrate"] = p.fogIntegrate;
    j["atmosphere"] = p.atmosphere;
    j["atmosphere_transmittance"] = p.atmosphereTransmittance;
    j["atmosphere_multi_scatter"] = p.atmosphereMultiScatter;
    j["atmosphere_sky_view"] = p.atmosphereSkyView;
    j["atmosphere_sky_reproject"] = p.atmosphereSkyReproject;
    j["atmosphere_sky_cdf"] = p.atmosphereSkyCdf;
    j["atmosphere_aerial_perspective"] = p.atmosphereAerialPerspective;
    j["denoiser"] = p.denoiser;
    j["moment_update"] = p.momentUpdate;
    j["history_copy"] = p.historyCopy;
    j["skip_denoiser_copy"] = p.skipDenoiserCopy;
    j["taa"] = p.taa;
    j["taa_history_copy"] = p.taaHistoryCopy;
    j["auto_exposure_histogram_clear"] = p.autoExposureHistogramClear;
    j["auto_exposure_histogram"] = p.autoExposureHistogram;
    j["auto_exposure_reduce"] = p.autoExposureReduce;
    j["tone_map"] = p.toneMap;
    j["selection_outline"] = p.selectionOutline;
    j["fullscreen"] = p.fullscreen;
    j["editor_presentation"] = p.editorPresentation;
}

void to_json(nlohmann::json& j, const ProfileReport::PipelineStatistics& s) {
    j["ray_invocations"] = s.rayInvocations;
    j["triangle_hits"] = s.triangleHits;
    j["aabb_hits"] = s.aabbHits;
}

void to_json(nlohmann::json& j, const ProfileReport::MemoryReport& m) {
    j["textures_bytes"] = m.texturesBytes;
    j["buffers_bytes"] = m.buffersBytes;
    j["acceleration_structure_bytes"] = m.accelerationStructureBytes;
    j["temporal_history_bytes"] = m.temporalHistoryBytes;
    j["restir_reservoir_bytes"] = m.restirReservoirBytes;
    j["restir_di_current_bytes"] = m.restirDiCurrentBytes;
    j["restir_di_previous_bytes"] = m.restirDiPreviousBytes;
    j["restir_di_spatial_bytes"] = m.restirDiSpatialBytes;
    j["restir_gi_current_bytes"] = m.restirGiCurrentBytes;
    j["restir_gi_previous_bytes"] = m.restirGiPreviousBytes;
    j["restir_gi_spatial_bytes"] = m.restirGiSpatialBytes;
}

void to_json(nlohmann::json& j, const ProfileReport::AdaptiveQualityReport& a) {
    j["smoothed_gpu_ms"] = a.smoothedGpuMs;
    j["tier"] = a.tier;
    j["over_budget_frames"] = a.overBudgetFrames;
    j["effective_max_bounces"] = a.effectiveMaxBounces;
    j["effective_environment_samples"] = a.effectiveEnvironmentSamples;
    j["effective_atrous_iterations"] = a.effectiveAtrousIterations;
    j["skip_restir_spatial"] = a.skipRestirSpatial;
    j["skip_denoiser"] = a.skipDenoiser;
}

void to_json(nlohmann::json& j, const RendererSettings& s) {
    j["render_preset"] = renderPresetName(s.renderPreset);
    j["path_tracing_enabled"] = s.pathTracingEnabled;
    j["denoiser_enabled"] = s.denoiserEnabled;
    j["max_bounces"] = s.maxBounces;
    j["atrous_iterations"] = s.atrousIterations;
    j["restir_mode"] = restirModeName(s.restirMode);
    j["restir_gi_enabled"] = s.restirGiEnabled;
    j["tone_mapper"] = toneMapperName(s.toneMapper);
    j["exposure"] = s.exposure;
    j["render_resolution_scale"] = s.renderResolutionScale;
    j["specular_aa_enabled"] = s.specularAaEnabled;
    j["camera_jitter_enabled"] = s.cameraJitterEnabled;
    j["denoise_while_moving"] = s.denoiseWhileMoving;
    j["samples_per_pixel"] = s.samplesPerPixel;
    j["limit_samples_per_pixel"] = s.limitSamplesPerPixel;
    j["effective_samples_per_pixel"] = s.limitSamplesPerPixel ? 1u : s.samplesPerPixel;
    j["taa_enabled"] = s.taaEnabled;
    j["taa_feedback"] = s.taaFeedback;
    j["taa_motion_feedback"] = s.taaMotionFeedback;
    j["taa_reactive_feedback"] = s.taaReactiveFeedback;
    j["taa_sharpening_strength"] = s.taaSharpeningStrength;
    j["sunlight_enabled"] = s.sunlightEnabled;
    j["direct_lighting_enabled"] = s.directLightingEnabled;
    j["environment_enabled"] = s.environmentEnabled;
    j["environment_direct_samples"] = s.environmentDirectSamples;
    j["denoiser_strength"] = s.denoiserStrength;
    j["denoiser_max_history_length"] = s.denoiserMaxHistoryLength;
    j["moment_validity_threshold"] = s.momentValidityThreshold;
    j["sun_intensity"] = s.sunIntensity;
    j["sun_elevation"] = s.sunElevation;
    j["sun_azimuth"] = s.sunAzimuth;
    j["gamma"] = s.gamma;
    j["contrast"] = s.contrast;
    j["saturation"] = s.saturation;
    j["brightness"] = s.brightness;
    j["white_point"] = s.whitePoint;
    j["auto_exposure_enabled"] = s.autoExposureEnabled;
    j["debug_view"] = rendererDebugViewName(s.debugView);
    j["restir_gi_temporal_max_age"] = s.restirGiTemporalMaxAge;
    j["restir_gi_spatial_rounds"] = s.restirGiSpatialRounds;
    j["restir_gi_spatial_radius"] = s.restirGiSpatialRadius;
    j["restir_gi_depth_threshold_scale"] = s.restirGiDepthThresholdScale;
    j["restir_gi_spatial_compatibility_threshold"] = s.restirGiSpatialCompatibilityThreshold;
    j["restir_gi_half_resolution"] = s.restirGiHalfResolution;
    j["restir_gi_visibility_ray_budget"] = s.restirGiVisibilityRayBudget;
    j["adaptive_quality_mode"] = s.adaptiveQualityMode == AdaptiveQualityMode::Off ? "off"
        : s.adaptiveQualityMode == AdaptiveQualityMode::Conservative ? "conservative"
        : s.adaptiveQualityMode == AdaptiveQualityMode::Balanced ? "balanced" : "aggressive";
    j["adaptive_gpu_frame_target_ms"] = s.adaptiveGpuFrameTargetMs;
}

namespace {

std::string formatVulkanVersion(uint32_t version) {
    std::ostringstream ss;
    ss << VK_VERSION_MAJOR(version) << "." << VK_VERSION_MINOR(version) << "." << VK_VERSION_PATCH(version);
    return ss.str();
}

float percentileOfSorted(const std::vector<float>& sorted, float p) {
    if (sorted.empty()) {
        return 0.0f;
    }
    const float scaled = std::clamp(p, 0.0f, 1.0f) * static_cast<float>(sorted.size() - 1u);
    const size_t lower = static_cast<size_t>(std::floor(scaled));
    const size_t upper = std::min(sorted.size() - 1u, lower + 1u);
    const float t = scaled - static_cast<float>(lower);
    return sorted[lower] * (1.0f - t) + sorted[upper] * t;
}

ProfileReport::MinMaxAvg computeMinMaxAvg(const std::vector<float>& values, uint32_t warmupFrames) {
    ProfileReport::MinMaxAvg result{};
    if (values.empty()) return result;
    size_t startIdx = std::min(static_cast<size_t>(warmupFrames), values.size());
    if (startIdx >= values.size()) { startIdx = 0; }
    size_t count = values.size() - startIdx;
    if (count == 0) { count = values.size(); startIdx = 0; }
    result.min = *std::min_element(values.begin() + startIdx, values.end());
    result.max = *std::max_element(values.begin() + startIdx, values.end());
    double sum = 0.0;
    for (size_t i = startIdx; i < values.size(); ++i) sum += values[i];
    result.avg = static_cast<float>(sum / count);

    std::vector<float> sorted(values.begin() + startIdx, values.end());
    std::sort(sorted.begin(), sorted.end());
    result.p95 = percentileOfSorted(sorted, 0.95f);
    result.p99 = percentileOfSorted(sorted, 0.99f);
    return result;
}

float percentileGpuTiming(
    const std::vector<GpuFrameTimings>& values,
    uint32_t warmupFrames,
    float GpuFrameTimings::*member,
    float percentile) {
    if (values.empty()) {
        return 0.0f;
    }

    size_t startIdx = std::min(static_cast<size_t>(warmupFrames), values.size());
    if (startIdx >= values.size()) {
        startIdx = 0;
    }
    if (startIdx >= values.size()) {
        return 0.0f;
    }

    std::vector<float> sorted;
    sorted.reserve(values.size() - startIdx);
    for (size_t i = startIdx; i < values.size(); ++i) {
        sorted.push_back(values[i].*member);
    }
    std::sort(sorted.begin(), sorted.end());
    return percentileOfSorted(sorted, percentile);
}

GpuFrameTimings percentileGpuTimings(
    const std::vector<GpuFrameTimings>& values,
    uint32_t warmupFrames,
    float percentile) {
    GpuFrameTimings result{};
    result.pathTraceMs = percentileGpuTiming(values, warmupFrames, &GpuFrameTimings::pathTraceMs, percentile);
    result.restirHistoryClearMs = percentileGpuTiming(values, warmupFrames, &GpuFrameTimings::restirHistoryClearMs, percentile);
    result.restirGiClearMs = percentileGpuTiming(values, warmupFrames, &GpuFrameTimings::restirGiClearMs, percentile);
    result.restirSpatialMs = percentileGpuTiming(values, warmupFrames, &GpuFrameTimings::restirSpatialMs, percentile);
    result.restirSpatialCopyMs = percentileGpuTiming(values, warmupFrames, &GpuFrameTimings::restirSpatialCopyMs, percentile);
    result.restirGiSpatialMs = percentileGpuTiming(values, warmupFrames, &GpuFrameTimings::restirGiSpatialMs, percentile);
    result.restirGiFinalMs = percentileGpuTiming(values, warmupFrames, &GpuFrameTimings::restirGiFinalMs, percentile);
    result.fogIntegrateMs = percentileGpuTiming(values, warmupFrames, &GpuFrameTimings::fogIntegrateMs, percentile);
    result.atmosphereMs = percentileGpuTiming(values, warmupFrames, &GpuFrameTimings::atmosphereMs, percentile);
    result.atmosphereTransmittanceMs = percentileGpuTiming(values, warmupFrames, &GpuFrameTimings::atmosphereTransmittanceMs, percentile);
    result.atmosphereMultiScatterMs = percentileGpuTiming(values, warmupFrames, &GpuFrameTimings::atmosphereMultiScatterMs, percentile);
    result.atmosphereSkyViewMs = percentileGpuTiming(values, warmupFrames, &GpuFrameTimings::atmosphereSkyViewMs, percentile);
    result.atmosphereSkyReprojectMs = percentileGpuTiming(values, warmupFrames, &GpuFrameTimings::atmosphereSkyReprojectMs, percentile);
    result.atmosphereSkyCdfMs = percentileGpuTiming(values, warmupFrames, &GpuFrameTimings::atmosphereSkyCdfMs, percentile);
    result.atmosphereAerialPerspectiveMs = percentileGpuTiming(values, warmupFrames, &GpuFrameTimings::atmosphereAerialPerspectiveMs, percentile);
    result.denoiserMs = percentileGpuTiming(values, warmupFrames, &GpuFrameTimings::denoiserMs, percentile);
    result.momentUpdateMs = percentileGpuTiming(values, warmupFrames, &GpuFrameTimings::momentUpdateMs, percentile);
    result.historyCopyMs = percentileGpuTiming(values, warmupFrames, &GpuFrameTimings::historyCopyMs, percentile);
    result.skipDenoiserCopyMs = percentileGpuTiming(values, warmupFrames, &GpuFrameTimings::skipDenoiserCopyMs, percentile);
    result.taaMs = percentileGpuTiming(values, warmupFrames, &GpuFrameTimings::taaMs, percentile);
    result.taaHistoryCopyMs = percentileGpuTiming(values, warmupFrames, &GpuFrameTimings::taaHistoryCopyMs, percentile);
    result.autoExposureMs = percentileGpuTiming(values, warmupFrames, &GpuFrameTimings::autoExposureMs, percentile);
    result.autoExposureHistogramClearMs = percentileGpuTiming(values, warmupFrames, &GpuFrameTimings::autoExposureHistogramClearMs, percentile);
    result.autoExposureHistogramMs = percentileGpuTiming(values, warmupFrames, &GpuFrameTimings::autoExposureHistogramMs, percentile);
    result.autoExposureReduceMs = percentileGpuTiming(values, warmupFrames, &GpuFrameTimings::autoExposureReduceMs, percentile);
    result.toneMapMs = percentileGpuTiming(values, warmupFrames, &GpuFrameTimings::toneMapMs, percentile);
    result.selectionOutlineMs = percentileGpuTiming(values, warmupFrames, &GpuFrameTimings::selectionOutlineMs, percentile);
    result.fullscreenMs = percentileGpuTiming(values, warmupFrames, &GpuFrameTimings::fullscreenMs, percentile);
    result.editorPresentationMs = percentileGpuTiming(values, warmupFrames, &GpuFrameTimings::editorPresentationMs, percentile);
    return result;
}

void assignPerPassGpuMs(ProfileReport::PerPassGpuMs& out, const GpuFrameTimings& timings) {
    out.pathTrace = timings.pathTraceMs;
    out.restirHistoryClear = timings.restirHistoryClearMs;
    out.restirGiClear = timings.restirGiClearMs;
    out.restirSpatial = timings.restirSpatialMs;
    out.restirSpatialCopy = timings.restirSpatialCopyMs;
    out.restirGiSpatial = timings.restirGiSpatialMs;
    out.restirGiFinal = timings.restirGiFinalMs;
    out.fogIntegrate = timings.fogIntegrateMs;
    out.atmosphere = timings.atmosphereMs;
    out.atmosphereTransmittance = timings.atmosphereTransmittanceMs;
    out.atmosphereMultiScatter = timings.atmosphereMultiScatterMs;
    out.atmosphereSkyView = timings.atmosphereSkyViewMs;
    out.atmosphereSkyReproject = timings.atmosphereSkyReprojectMs;
    out.atmosphereSkyCdf = timings.atmosphereSkyCdfMs;
    out.atmosphereAerialPerspective = timings.atmosphereAerialPerspectiveMs;
    out.denoiser = timings.denoiserMs;
    out.momentUpdate = timings.momentUpdateMs;
    out.historyCopy = timings.historyCopyMs;
    out.skipDenoiserCopy = timings.skipDenoiserCopyMs;
    out.taa = timings.taaMs;
    out.taaHistoryCopy = timings.taaHistoryCopyMs;
    out.autoExposureHistogramClear = timings.autoExposureHistogramClearMs;
    out.autoExposureHistogram = timings.autoExposureHistogramMs;
    out.autoExposureReduce = timings.autoExposureReduceMs;
    out.toneMap = timings.toneMapMs;
    out.selectionOutline = timings.selectionOutlineMs;
    out.fullscreen = timings.fullscreenMs;
    out.editorPresentation = timings.editorPresentationMs;
}

GpuFrameTimings averageGpuTimings(const std::vector<GpuFrameTimings>& values, uint32_t warmupFrames) {
    GpuFrameTimings result{};
    if (values.empty()) {
        return result;
    }

    size_t startIdx = std::min(static_cast<size_t>(warmupFrames), values.size());
    if (startIdx >= values.size()) {
        startIdx = 0;
    }
    const size_t count = values.size() - startIdx;
    if (count == 0) {
        return result;
    }

    for (size_t i = startIdx; i < values.size(); ++i) {
        result.pathTraceMs += values[i].pathTraceMs;
        result.restirHistoryClearMs += values[i].restirHistoryClearMs;
        result.restirGiClearMs += values[i].restirGiClearMs;
        result.restirSpatialMs += values[i].restirSpatialMs;
        result.restirSpatialCopyMs += values[i].restirSpatialCopyMs;
        result.restirGiSpatialMs += values[i].restirGiSpatialMs;
        result.restirGiFinalMs += values[i].restirGiFinalMs;
        result.fogIntegrateMs += values[i].fogIntegrateMs;
        result.atmosphereMs += values[i].atmosphereMs;
        result.atmosphereTransmittanceMs += values[i].atmosphereTransmittanceMs;
        result.atmosphereMultiScatterMs += values[i].atmosphereMultiScatterMs;
        result.atmosphereSkyViewMs += values[i].atmosphereSkyViewMs;
        result.atmosphereSkyReprojectMs += values[i].atmosphereSkyReprojectMs;
        result.atmosphereSkyCdfMs += values[i].atmosphereSkyCdfMs;
        result.atmosphereAerialPerspectiveMs += values[i].atmosphereAerialPerspectiveMs;
        result.denoiserMs += values[i].denoiserMs;
        result.momentUpdateMs += values[i].momentUpdateMs;
        result.historyCopyMs += values[i].historyCopyMs;
        result.skipDenoiserCopyMs += values[i].skipDenoiserCopyMs;
        result.taaMs += values[i].taaMs;
        result.taaHistoryCopyMs += values[i].taaHistoryCopyMs;
        result.autoExposureMs += values[i].autoExposureMs;
        result.autoExposureHistogramClearMs += values[i].autoExposureHistogramClearMs;
        result.autoExposureHistogramMs += values[i].autoExposureHistogramMs;
        result.autoExposureReduceMs += values[i].autoExposureReduceMs;
        result.toneMapMs += values[i].toneMapMs;
        result.selectionOutlineMs += values[i].selectionOutlineMs;
        result.fullscreenMs += values[i].fullscreenMs;
        result.editorPresentationMs += values[i].editorPresentationMs;
    }

    const float invCount = 1.0f / static_cast<float>(count);
    result.pathTraceMs *= invCount;
    result.restirHistoryClearMs *= invCount;
    result.restirGiClearMs *= invCount;
    result.restirSpatialMs *= invCount;
    result.restirSpatialCopyMs *= invCount;
    result.restirGiSpatialMs *= invCount;
    result.restirGiFinalMs *= invCount;
    result.fogIntegrateMs *= invCount;
    result.atmosphereMs *= invCount;
    result.atmosphereTransmittanceMs *= invCount;
    result.atmosphereMultiScatterMs *= invCount;
    result.atmosphereSkyViewMs *= invCount;
    result.atmosphereSkyReprojectMs *= invCount;
    result.atmosphereSkyCdfMs *= invCount;
    result.atmosphereAerialPerspectiveMs *= invCount;
    result.denoiserMs *= invCount;
    result.momentUpdateMs *= invCount;
    result.historyCopyMs *= invCount;
    result.skipDenoiserCopyMs *= invCount;
    result.taaMs *= invCount;
    result.taaHistoryCopyMs *= invCount;
    result.autoExposureMs *= invCount;
    result.autoExposureHistogramClearMs *= invCount;
    result.autoExposureHistogramMs *= invCount;
    result.autoExposureReduceMs *= invCount;
    result.toneMapMs *= invCount;
    result.selectionOutlineMs *= invCount;
    result.fullscreenMs *= invCount;
    result.editorPresentationMs *= invCount;
    return result;
}

void writeValidationLog(const RendererValidationLog& log, const std::filesystem::path& path) {
    std::ofstream file(path);
    if (!file.is_open()) return;
    file << "=== Validation Log ===\n\n";
    file << "--- Pass Events (" << log.passEvents().size() << ") ---\n";
    for (const auto& ev : log.passEvents()) { file << "  " << ev << "\n"; }
    file << "\n--- Barrier Events (" << log.barrierEvents().size() << ") ---\n";
    for (const auto& ev : log.barrierEvents()) { file << "  " << ev << "\n"; }
    file << "\n--- Accumulation Invalidations (" << log.invalidations().size() << ") ---\n";
    for (const auto& ev : log.invalidations()) { file << "  frame=" << ev.frame << " reason=" << ev.reason << "\n"; }
    file << "\n--- Scene Update Routes (" << log.sceneUpdateRoutes().size() << ") ---\n";
    for (const auto& ev : log.sceneUpdateRoutes()) { file << "  " << ev.kind << ": " << ev.action << " (count=" << ev.count << ")\n"; }
    file << "\n--- Resource States (" << log.resourceStateEvents().size() << ") ---\n";
    for (const auto& ev : log.resourceStateEvents()) {
        file << "  " << ev.resource << " " << ev.beforePass << " -> " << ev.afterPass
             << " [layout: " << static_cast<int>(ev.beforeLayout) << " -> " << static_cast<int>(ev.afterLayout)
             << ", stage: " << static_cast<uint64_t>(ev.beforeStage) << " -> " << static_cast<uint64_t>(ev.afterStage)
             << ", access: " << static_cast<uint64_t>(ev.beforeAccess) << " -> " << static_cast<uint64_t>(ev.afterAccess) << "]\n";
    }
}

void writeSettingsJson(const RendererSettings& settings, const std::filesystem::path& path) {
    nlohmann::json j;
    to_json(j, settings);
    std::ofstream file(path);
    if (file.is_open()) { file << j.dump(2); }
}

std::string sanitizeSceneName(std::string name) {
    for (char& ch : name) {
        if (std::isalnum(static_cast<unsigned char>(ch))) {
            ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
        } else {
            ch = '_';
        }
    }
    return name;
}

std::string sequenceFrameFileName(uint32_t frameIndex) {
    std::ostringstream stream;
    stream << "frame_" << std::setw(4) << std::setfill('0') << frameIndex << ".png";
    return stream.str();
}

} // namespace

HeadlessDiagnostics::HeadlessDiagnostics(const HeadlessDiagnosticsConfig& config)
    : config_(config) {
    if (config.profileJsonPath.has_value()) {
        profileJsonPath_ = *config.profileJsonPath;
    }
}

HeadlessDiagnostics::~HeadlessDiagnostics() {
    if (logCapture_) {
        (void)releaseStdout();
    }
}

ProfileReport HeadlessDiagnostics::run(Application& app) {
    auto* renderer = app.pathTracer();
    auto* context = app.vulkanContext();

    if (renderer == nullptr || context == nullptr) { return profileReport_; }
    profileReport_ = ProfileReport{};

    VkPhysicalDeviceProperties props = context->physicalDeviceProperties();
    profileReport_.gpuName = props.deviceName;
    profileReport_.vulkanVersion = formatVulkanVersion(props.apiVersion);
    profileReport_.driverVersion = std::to_string(props.driverVersion);
    profileReport_.resolution.renderWidth = renderer->renderExtent().width;
    profileReport_.resolution.renderHeight = renderer->renderExtent().height;
    profileReport_.resolution.displayWidth = renderer->displayExtent().width;
    profileReport_.resolution.displayHeight = renderer->displayExtent().height;
    profileReport_.resolution.renderScale = renderer->settings().renderResolutionScale;
    profileReport_.warmupFrames = config_.warmupFrames;
    profileReport_.frameCount = config_.totalFrames;
    profileReport_.profiledFrames = config_.totalFrames > config_.warmupFrames
        ? config_.totalFrames - config_.warmupFrames : 0;

    const auto& cpuTimings = app.cpuFrameTimings();
    const auto& gpuTimingsVec = app.gpuFrameTimings();
    uint32_t warmup = app.warmupFrameCount();
    profileReport_.cpuFrameMs = computeMinMaxAvg(cpuTimings, warmup);
    profileReport_.gpuFrameMs = computeMinMaxAvg(gpuTimingsVec, warmup);

    const auto timings = averageGpuTimings(app.perFrameGpuTimings(), warmup);
    assignPerPassGpuMs(profileReport_.perPassGpuMs, timings);
    assignPerPassGpuMs(profileReport_.perPassGpuMsP95, percentileGpuTimings(app.perFrameGpuTimings(), warmup, 0.95f));
    assignPerPassGpuMs(profileReport_.perPassGpuMsP99, percentileGpuTimings(app.perFrameGpuTimings(), warmup, 0.99f));

    const auto& stats = renderer->pipelineStats();
    profileReport_.pipelineStatistics.rayInvocations = stats.rayInvocations;
    profileReport_.pipelineStatistics.triangleHits = stats.triangleHits;
    profileReport_.pipelineStatistics.aabbHits = stats.aabbHits;

    const auto& rtStats = renderer->rayTracingStats();
    profileReport_.memory.accelerationStructureBytes = static_cast<uint64_t>(rtStats.accelerationStructureBytes);
    profileReport_.memory.texturesBytes = static_cast<uint64_t>(renderer->estimatedTextureMemory());
    profileReport_.memory.buffersBytes = static_cast<uint64_t>(renderer->estimatedBufferMemory());
    profileReport_.memory.temporalHistoryBytes = static_cast<uint64_t>(renderer->temporalHistoryMemory());
    profileReport_.memory.restirReservoirBytes = static_cast<uint64_t>(renderer->restirReservoirMemory());
    const auto reservoirBreakdown = renderer->restirReservoirMemoryBreakdown();
    profileReport_.memory.restirDiCurrentBytes = static_cast<uint64_t>(reservoirBreakdown.diCurrentBytes);
    profileReport_.memory.restirDiPreviousBytes = static_cast<uint64_t>(reservoirBreakdown.diPreviousBytes);
    profileReport_.memory.restirDiSpatialBytes = static_cast<uint64_t>(reservoirBreakdown.diSpatialBytes);
    profileReport_.memory.restirGiCurrentBytes = static_cast<uint64_t>(reservoirBreakdown.giCurrentBytes);
    profileReport_.memory.restirGiPreviousBytes = static_cast<uint64_t>(reservoirBreakdown.giPreviousBytes);
    profileReport_.memory.restirGiSpatialBytes = static_cast<uint64_t>(reservoirBreakdown.giSpatialBytes);
    profileReport_.restirGiLayout = renderer->restirGiReservoirLayoutName();

    const auto adaptiveState = renderer->adaptiveQualityState();
    profileReport_.adaptiveQuality.smoothedGpuMs = adaptiveState.smoothedGpuMs;
    profileReport_.adaptiveQuality.tier = adaptiveState.tier;
    profileReport_.adaptiveQuality.overBudgetFrames = adaptiveState.overBudgetFrames;
    profileReport_.adaptiveQuality.effectiveMaxBounces = adaptiveState.effectiveMaxBounces;
    profileReport_.adaptiveQuality.effectiveEnvironmentSamples = adaptiveState.effectiveEnvironmentSamples;
    profileReport_.adaptiveQuality.effectiveAtrousIterations = adaptiveState.effectiveAtrousIterations;
    profileReport_.adaptiveQuality.skipRestirSpatial = adaptiveState.skipRestirSpatial;
    profileReport_.adaptiveQuality.skipDenoiser = adaptiveState.skipDenoiser;

    profileReport_.validationErrorCount = 0;

    profileReport_.settings = renderer->settings();

    return profileReport_;
}

void HeadlessDiagnostics::writeProfileJson(const std::filesystem::path& path) const {
    nlohmann::json j;
    j["engine_version"] = profileReport_.engineVersion;
    j["git_commit"] = profileReport_.gitCommit;
    j["gpu_name"] = profileReport_.gpuName;
    j["driver_version"] = profileReport_.driverVersion;
    j["vulkan_version"] = profileReport_.vulkanVersion;
    j["restir_gi_layout"] = profileReport_.restirGiLayout;
    j["resolution"] = profileReport_.resolution;
    j["frame_count"] = profileReport_.frameCount;
    j["warmup_frames"] = profileReport_.warmupFrames;
    j["profiled_frames"] = profileReport_.profiledFrames;
    j["cpu_frame_ms"] = profileReport_.cpuFrameMs;
    j["gpu_frame_ms"] = profileReport_.gpuFrameMs;
    j["per_pass_gpu_ms"] = profileReport_.perPassGpuMs;
    j["per_pass_gpu_ms_p95"] = profileReport_.perPassGpuMsP95;
    j["per_pass_gpu_ms_p99"] = profileReport_.perPassGpuMsP99;
    j["pipeline_statistics"] = profileReport_.pipelineStatistics;
    const uint64_t hitCount = profileReport_.pipelineStatistics.triangleHits + profileReport_.pipelineStatistics.aabbHits;
    j["gpu_debug_counters"] = {
        {"ray_count", profileReport_.pipelineStatistics.rayInvocations},
        {"shadow_ray_count", nullptr},
        {"hit_count", hitCount},
        {"miss_count", profileReport_.pipelineStatistics.rayInvocations > hitCount ? profileReport_.pipelineStatistics.rayInvocations - hitCount : 0},
        {"path_length_histogram", nlohmann::json::array()},
        {"restir_accepted_count", nullptr},
        {"restir_rejected_count", nullptr},
        {"taa_history_accepted_count", nullptr},
        {"taa_history_rejected_count", nullptr},
        {"denoiser_history_accepted_count", nullptr},
        {"denoiser_history_rejected_count", nullptr},
        {"notes", nlohmann::json::array({
            "ray_count/hit_count/miss_count come from Vulkan pipeline statistics when available",
            "remaining counters require shader atomic instrumentation and are intentionally null until instrumented"
        })},
    };
    j["memory"] = profileReport_.memory;
    j["adaptive_quality"] = profileReport_.adaptiveQuality;
    j["validation_error_count"] = profileReport_.validationErrorCount;
    j["warnings"] = profileReport_.warnings;
    j["settings"] = profileReport_.settings;
    const auto dir = path.parent_path();
    if (!dir.empty() && !std::filesystem::exists(dir)) { std::filesystem::create_directories(dir); }
    std::ofstream file(path);
    if (!file.is_open()) { throw std::runtime_error("Failed to open profile JSON: " + path.string()); }
    file << j.dump(2);
}

void HeadlessDiagnostics::writeRenderGraphJson(const std::filesystem::path& path) {
    const auto dir = path.parent_path();
    if (!dir.empty() && !std::filesystem::exists(dir)) { std::filesystem::create_directories(dir); }
    GpuFrameTimings t{};
    RenderGraph g;
    dumpRenderGraphJson(g, t, path);
}

void HeadlessDiagnostics::exportDebugViews(Application& app, const std::filesystem::path& dir) {
    if (!config_.saveDebugViewsDir.has_value()) return;

    auto* renderer = app.pathTracer();
    auto* allocator = app.resourceAllocator();
    auto* context = app.vulkanContext();
    if (!renderer || !allocator || !context) return;

    std::filesystem::create_directories(dir);

    DiagnosticImageExport exporter(*context, *allocator);
    VkExtent2D displayExtent = renderer->displayExtent();
    if (!exporter.initialize(VK_FORMAT_R8G8B8A8_UNORM, displayExtent)) {
        std::cerr << "Warning: Failed to initialize image export\n";
        return;
    }

    const auto views = DiagnosticImageExport::allExportViews();
    std::vector<std::string> exported;
    const uint32_t kWarmupFrames = config_.warmupFrames > 0 ? config_.warmupFrames : 4;

    for (auto view : views) {
        std::string viewName = rendererDebugViewName(view);
        auto outputPath = dir / (viewName + ".png");
        std::cout << "Exporting debug view: " << viewName << "...\n";

        app.applyDebugView(view);
        app.renderFrames(kWarmupFrames + 1);

        if (exporter.exportView(*renderer, view, outputPath, 0)) {
            exported.push_back(viewName);
        } else {
            std::cerr << "Warning: Failed to export debug view: " << viewName << "\n";
        }
    }

    std::vector<std::string> missing;
    if (std::find(exported.begin(), exported.end(), "metallic") == exported.end()) {
        missing.push_back("metallic");
    }

    exporter.writeExportManifest(dir, exported, displayExtent.width, displayExtent.height);
    if (!missing.empty()) {
        auto manifestPath = dir / "export_manifest.json";
        if (std::filesystem::exists(manifestPath)) {
            std::ifstream in(manifestPath);
            nlohmann::json manifest;
            in >> manifest;
            manifest["missing_debug_views"] = missing;
            std::ofstream out(manifestPath);
            out << manifest.dump(2);
        }
    }
    std::cout << "Exported " << exported.size() << " debug views to " << dir.string() << "\n";
}

void HeadlessDiagnostics::exportFrameSequence(Application& app, const std::filesystem::path& dir) {
    if (!config_.saveFrameSequenceDir.has_value()) return;

    auto* renderer = app.pathTracer();
    auto* allocator = app.resourceAllocator();
    auto* context = app.vulkanContext();
    if (!renderer || !allocator || !context) return;

    std::filesystem::create_directories(dir);

    DiagnosticImageExport exporter(*context, *allocator);
    VkExtent2D displayExtent = renderer->displayExtent();
    if (!exporter.initialize(VK_FORMAT_R8G8B8A8_UNORM, displayExtent)) {
        std::cerr << "Warning: Failed to initialize frame sequence export\n";
        return;
    }

    std::vector<RendererDebugView> views = config_.sequenceViews;
    if (views.empty()) {
        views.push_back(RendererDebugView::Beauty);
    }

    const uint32_t warmupFrames = config_.warmupFrames;
    const uint32_t step = std::max(1u, config_.sequenceStep);
    const uint32_t profiledFrames = config_.totalFrames > warmupFrames
        ? config_.totalFrames - warmupFrames
        : config_.totalFrames;
    const uint32_t framesToExport = config_.sequenceFrameCount.value_or(std::max(1u, profiledFrames));

    nlohmann::json manifest;
    manifest["views"] = nlohmann::json::array();
    manifest["warmup_frames"] = warmupFrames;
    manifest["sequence_start_frame"] = config_.sequenceStartFrame;
    manifest["sequence_frame_count"] = framesToExport;
    manifest["sequence_step"] = step;
    manifest["resolution"] = { {"width", displayExtent.width}, {"height", displayExtent.height} };
    manifest["frames"] = nlohmann::json::object();

    for (RendererDebugView view : views) {
        const std::string viewName = rendererDebugViewName(view);
        const std::filesystem::path viewDir = dir / viewName;
        std::filesystem::create_directories(viewDir);
        manifest["views"].push_back(viewName);
        manifest["frames"][viewName] = nlohmann::json::array();

        std::cout << "Exporting frame sequence view: " << viewName << "...\n";
        app.applyDebugView(view);
        app.resetDiagnosticFrameCounter(0);
        if (warmupFrames > 0u) {
            app.renderFrames(warmupFrames);
        }
        if (config_.sequenceStartFrame > 0u) {
            app.renderFrames(config_.sequenceStartFrame);
        }

        uint32_t sequenceFrame = config_.sequenceStartFrame;
        for (uint32_t exported = 0; exported < framesToExport; ++exported) {
            app.renderFrames(1);
            const std::string fileName = sequenceFrameFileName(sequenceFrame);
            const std::filesystem::path outputPath = viewDir / fileName;
            if (exporter.exportView(*renderer, view, outputPath, 0)) {
                manifest["frames"][viewName].push_back(fileName);
            } else {
                std::cerr << "Warning: Failed to export sequence frame " << fileName
                          << " for view " << viewName << "\n";
            }
            if (step > 1u && exported + 1u < framesToExport) {
                app.renderFrames(step - 1u);
            }
            sequenceFrame += step;
        }
    }

    std::ofstream file(dir / "sequence_manifest.json");
    if (!file.is_open()) {
        throw std::runtime_error("Failed to open sequence manifest: " + (dir / "sequence_manifest.json").string());
    }
    file << manifest.dump(2);
    std::cout << "Exported frame sequence to " << dir.string() << "\n";
}

void HeadlessDiagnostics::makeDebugPackage(Application& app, const std::filesystem::path& dir, const std::filesystem::path& scenePath) {
    if (!std::filesystem::exists(dir)) { std::filesystem::create_directories(dir); }

    auto copyIf = [](const auto& src, const auto& dest) {
        if (src.has_value() && std::filesystem::exists(*src)) {
            std::filesystem::copy_file(*src, dest, std::filesystem::copy_options::overwrite_existing);
        }
    };
    copyIf(config_.profileJsonPath, dir / "profile.json");
    copyIf(config_.dumpRenderGraphPath, dir / "rendergraph.json");

    if (config_.saveDebugViewsDir.has_value() && std::filesystem::exists(*config_.saveDebugViewsDir)) {
        try {
            std::filesystem::copy(*config_.saveDebugViewsDir, dir / "debug_views",
                std::filesystem::copy_options::recursive | std::filesystem::copy_options::overwrite_existing);
        } catch (...) {}
    }

    if (config_.saveFrameSequenceDir.has_value() && std::filesystem::exists(*config_.saveFrameSequenceDir)) {
        try {
            std::filesystem::copy(*config_.saveFrameSequenceDir, dir / "frame_sequence",
                std::filesystem::copy_options::recursive | std::filesystem::copy_options::overwrite_existing);
        } catch (...) {}
    }

    if (!scenePath.empty() && std::filesystem::exists(scenePath)) {
        std::filesystem::copy_file(scenePath, dir / "scene_copy.rtlevel",
            std::filesystem::copy_options::overwrite_existing);
    }

    if (auto* renderer = app.pathTracer()) {
        writeValidationLog(renderer->validationLog(), dir / "validation.txt");
        writeSettingsJson(renderer->settings(), dir / "settings.json");
    }

    if (logCapture_) {
        std::string logText = releaseStdout();
        std::ofstream logFile(dir / "log.txt");
        if (logFile.is_open()) { logFile << logText; }
    }

    if (config_.captureRenderDocPath.has_value() && std::filesystem::exists(*config_.captureRenderDocPath)) {
        std::filesystem::copy_file(*config_.captureRenderDocPath, dir / "capture.rdc",
            std::filesystem::copy_options::overwrite_existing);
    }
}

ValidationSuiteSummary HeadlessDiagnostics::runValidationSuite() {
    ValidationSuiteSummary summary;

    struct SceneConfig {
        std::string name;
        std::filesystem::path path;
        uint32_t frames;
        uint32_t warmup;
    };

    std::vector<SceneConfig> scenes;
    const std::filesystem::path validationDir = "scenes/validation";
    const std::filesystem::path manifestPath = validationDir / "manifest.json";
    if (std::filesystem::exists(manifestPath)) {
        std::ifstream manifestFile(manifestPath);
        nlohmann::json manifest;
        manifestFile >> manifest;
        for (const auto& scene : manifest.value("scenes", nlohmann::json::array())) {
            const std::string fileName = scene.value("path", "");
            if (fileName.empty()) {
                continue;
            }
            const std::filesystem::path scenePath = validationDir / fileName;
            scenes.push_back(SceneConfig{
                .name = sanitizeSceneName(scenePath.stem().string()),
                .path = scenePath,
                .frames = 120,
                .warmup = 30,
            });
        }
    }
    if (scenes.empty()) {
        scenes = {
            {"material_grid", validationDir / "material_grid.rtlevel", 120, 30},
            {"transform_stress", validationDir / "transform_stress.rtlevel", 120, 30},
        };
    }

    std::filesystem::path outputBase = config_.validationOutputDir.value_or("validation_output");
    std::filesystem::create_directories(outputBase);

    for (const auto& scene : scenes) {
        ValidationSceneResult result;
        result.name = scene.name;
        result.framesRendered = scene.frames;

        auto sceneOutDir = outputBase / scene.name;
        try {
            std::filesystem::create_directories(sceneOutDir);
            if (!std::filesystem::exists(scene.path)) {
                throw std::runtime_error("Missing validation scene: " + scene.path.string());
            }

            HeadlessDiagnosticsConfig sceneConfig = config_;
            sceneConfig.headless = true;
            sceneConfig.profile = true;
            sceneConfig.runValidationSuite = false;
            sceneConfig.totalFrames = scene.frames;
            sceneConfig.warmupFrames = scene.warmup;
            sceneConfig.fixedSeed = sceneConfig.fixedSeed.value_or(1u);
            sceneConfig.profileJsonPath = sceneOutDir / "profile.json";
            sceneConfig.saveDebugViewsDir = sceneOutDir / "debug_views";
            sceneConfig.makeDebugPackageDir.reset();

            HeadlessDiagnostics sceneDiagnostics(sceneConfig);
            sceneDiagnostics.captureStdout();
            Application app(
                RendererDebugView::Beauty,
                std::nullopt,
                std::nullopt,
                scene.path,
                std::nullopt,
                std::nullopt,
                std::nullopt,
                std::nullopt,
                false,
                false,
                true);
            if (auto* renderer = app.pathTracer()) {
                RendererSettings settings = renderer->settings();
                settings.fixedSeed = sceneConfig.fixedSeed;
                renderer->applySettings(settings);
            }

            app.runHeadless(scene.warmup, scene.frames);
            const ProfileReport profile = sceneDiagnostics.run(app);
            sceneDiagnostics.writeProfileJson(*sceneConfig.profileJsonPath);
            sceneDiagnostics.exportDebugViews(app, *sceneConfig.saveDebugViewsDir);
            if (auto* renderer = app.pathTracer()) {
                writeValidationLog(renderer->validationLog(), sceneOutDir / "validation.txt");
                writeSettingsJson(renderer->settings(), sceneOutDir / "settings.json");
            }
            std::filesystem::copy_file(scene.path, sceneOutDir / "scene_copy.rtlevel",
                std::filesystem::copy_options::overwrite_existing);

            std::ofstream logFile(sceneOutDir / "log.txt");
            if (logFile.is_open()) {
                logFile << sceneDiagnostics.releaseStdout();
            } else {
                (void)sceneDiagnostics.releaseStdout();
            }

            result.gpuMsTotal = profile.gpuFrameMs.avg;
            result.validationErrors = profile.validationErrorCount;
            result.status = result.validationErrors == 0 ? "pass" : "fail";
            summary.scenes.push_back(result);
            if (result.status == "pass") {
                summary.totalPass++;
            } else {
                summary.totalFail++;
            }
        } catch (const std::exception& error) {
            result.status = "fail";
            std::ofstream logFile(sceneOutDir / "log.txt", std::ios::app);
            if (logFile.is_open()) {
                logFile << "Fatal error: " << error.what() << "\n";
            }
            summary.scenes.push_back(result);
            summary.totalFail++;
        } catch (...) {
            result.status = "fail";
            summary.scenes.push_back(result);
            summary.totalFail++;
        }
    }

    auto summaryPath = outputBase / "summary.json";
    nlohmann::json j;
    nlohmann::json scenesJson = nlohmann::json::array();
    for (const auto& s : summary.scenes) {
        nlohmann::json sj;
        sj["name"] = s.name;
        sj["status"] = s.status;
        sj["gpu_ms_total"] = s.gpuMsTotal;
        sj["validation_errors"] = s.validationErrors;
        sj["frames_rendered"] = s.framesRendered;
        scenesJson.push_back(sj);
    }
    j["scenes"] = scenesJson;
    j["total_pass"] = summary.totalPass;
    j["total_fail"] = summary.totalFail;
    std::ofstream file(summaryPath);
    if (file.is_open()) { file << j.dump(2); }

    return summary;
}

void HeadlessDiagnostics::captureStdout() {
    logCapture_ = std::make_unique<std::ostringstream>();
    oldCout_ = std::cout.rdbuf(logCapture_->rdbuf());
    oldCerr_ = std::cerr.rdbuf(logCapture_->rdbuf());
}

std::string HeadlessDiagnostics::releaseStdout() {
    const std::string text = logCapture_ ? logCapture_->str() : std::string{};
    if (oldCout_ != nullptr) {
        std::cout.rdbuf(oldCout_);
        oldCout_ = nullptr;
    }
    if (oldCerr_ != nullptr) {
        std::cerr.rdbuf(oldCerr_);
        oldCerr_ = nullptr;
    }
    logCapture_.reset();
    return text;
}

void HeadlessDiagnostics::collectValidationLog(Application& app) {
    auto* renderer = app.pathTracer();
    if (renderer == nullptr) return;
    const auto& log = renderer->validationLog();
    (void)log;
}

} // namespace rtv
