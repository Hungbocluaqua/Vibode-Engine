#include "rtv/DiagnosticTools.h"

#include "rtv/DescriptorWriteDiagnostics.h"
#include "rtv/RenderGraphDump.h"
#include "rtv/ShaderReflection.h"

#include <nlohmann/json.hpp>
#include <stb_image.h>
#include <stb_image_write.h>

#include <algorithm>
#include <cmath>
#include <cctype>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <limits>
#include <numeric>
#include <sstream>
#include <stdexcept>
#include <set>
#include <unordered_map>
#include <vector>

namespace rtv {
namespace {

using json = nlohmann::json;

json readJsonFile(const std::filesystem::path& path) {
    std::ifstream file(path);
    if (!file.is_open()) {
        throw std::runtime_error("Failed to open JSON file: " + path.string());
    }
    json j;
    file >> j;
    return j;
}

void writeJsonFile(const std::filesystem::path& path, const json& j) {
    const auto dir = path.parent_path();
    if (!dir.empty()) {
        std::filesystem::create_directories(dir);
    }
    std::ofstream file(path);
    if (!file.is_open()) {
        throw std::runtime_error("Failed to open JSON output file: " + path.string());
    }
    file << j.dump(2);
}

double numberAt(const json& j, std::initializer_list<const char*> path) {
    const json* current = &j;
    for (const char* key : path) {
        if (!current->is_object() || !current->contains(key)) {
            return 0.0;
        }
        current = &(*current)[key];
    }
    return current->is_number() ? current->get<double>() : 0.0;
}

uint64_t uintAt(const json& j, std::initializer_list<const char*> path) {
    const json* current = &j;
    for (const char* key : path) {
        if (!current->is_object() || !current->contains(key)) {
            return 0;
        }
        current = &(*current)[key];
    }
    return current->is_number_unsigned() || current->is_number_integer()
        ? current->get<uint64_t>()
        : static_cast<uint64_t>(current->is_number() ? current->get<double>() : 0.0);
}

uint64_t memoryTotalBytes(const ProfileReport::MemoryReport& memory) {
    return memory.texturesBytes +
        memory.buffersBytes +
        memory.accelerationStructureBytes +
        memory.temporalHistoryBytes +
        memory.restirReservoirBytes +
        memory.stagingUploadPeakBytes;
}

uint64_t memoryTotalBytes(const json& memory) {
    return uintAt(memory, {"textures_bytes"}) +
        uintAt(memory, {"buffers_bytes"}) +
        uintAt(memory, {"acceleration_structure_bytes"}) +
        uintAt(memory, {"temporal_history_bytes"}) +
        uintAt(memory, {"restir_reservoir_bytes"}) +
        uintAt(memory, {"staging_upload_peak_bytes"});
}

std::map<std::string, uint64_t> profileMemoryBytes(const ProfileReport& profile) {
    const auto& memory = profile.memory;
    return {
        {"textures_bytes", memory.texturesBytes},
        {"buffers_bytes", memory.buffersBytes},
        {"acceleration_structure_bytes", memory.accelerationStructureBytes},
        {"temporal_history_bytes", memory.temporalHistoryBytes},
        {"restir_reservoir_bytes", memory.restirReservoirBytes},
        {"restir_di_current_bytes", memory.restirDiCurrentBytes},
        {"restir_di_initial_bytes", memory.restirDiInitialBytes},
        {"restir_di_temporal_bytes", memory.restirDiTemporalBytes},
        {"restir_di_previous_bytes", memory.restirDiPreviousBytes},
        {"restir_di_spatial_bytes", memory.restirDiSpatialBytes},
        {"restir_di_final_bytes", memory.restirDiFinalBytes},
        {"restir_di_receiver_bytes", memory.restirDiReceiverBytes},
        {"restir_di_previous_receiver_bytes", memory.restirDiPreviousReceiverBytes},
        {"restir_di_counters_bytes", memory.restirDiCountersBytes},
        {"restir_di_physical_bytes", memory.restirDiPhysicalBytes},
        {"restir_di_alias_savings_bytes", memory.restirDiAliasSavingsBytes},
        {"restir_gi_current_bytes", memory.restirGiCurrentBytes},
        {"restir_gi_previous_bytes", memory.restirGiPreviousBytes},
        {"restir_gi_spatial_bytes", memory.restirGiSpatialBytes},
        {"restir_gi_production_temporal_bytes", memory.restirGiProductionTemporalBytes},
        {"restir_gi_production_spatial_bytes", memory.restirGiProductionSpatialBytes},
        {"restir_gi_production_previous_bytes", memory.restirGiProductionPreviousBytes},
        {"restir_gi_production_upsampled_bytes", memory.restirGiProductionUpsampledBytes},
        {"restir_gi_active_tile_mask_bytes", memory.restirGiActiveTileMaskBytes},
        {"restir_gi_counters_bytes", memory.restirGiCountersBytes},
        {"restir_gi_receiver_bytes", memory.restirGiReceiverBytes},
        {"restir_gi_previous_receiver_bytes", memory.restirGiPreviousReceiverBytes},
        {"staging_upload_peak_bytes", memory.stagingUploadPeakBytes},
        {"staging_upload_total_bytes", memory.stagingUploadTotalBytes},
        {"total_bytes", memoryTotalBytes(memory)},
    };
}

std::map<std::string, double> passGpuMsMap(const ProfileReport::PerPassGpuMs& passes) {
    return {
        {"path_trace", passes.pathTrace},
        {"restir_history_clear", passes.restirHistoryClear},
        {"restir_gi_clear", passes.restirGiClear},
        {"restir_gi_temporal", passes.restirGiTemporal},
        {"restir_spatial", passes.restirSpatial},
        {"restir_spatial_copy", passes.restirSpatialCopy},
        {"restir_gi_spatial", passes.restirGiSpatial},
        {"restir_gi_upsample", passes.restirGiUpsample},
        {"restir_gi_final", passes.restirGiFinal},
        {"restir_gi_counters_readback", passes.restirGiCountersReadback},
        {"regir_build", passes.regirBuild},
        {"regir_spatial_reuse", passes.regirSpatialReuse},
        {"regir_temporal_reuse", passes.regirTemporalReuse},
        {"restir_di_temporal", passes.restirDiTemporal},
        {"restir_di_spatial", passes.restirDiSpatial},
        {"restir_di_final", passes.restirDiFinal},
        {"fog_integrate", passes.fogIntegrate},
        {"atmosphere", passes.atmosphere},
        {"atmosphere_transmittance", passes.atmosphereTransmittance},
        {"atmosphere_multi_scatter", passes.atmosphereMultiScatter},
        {"atmosphere_sky_view", passes.atmosphereSkyView},
        {"atmosphere_sky_reproject", passes.atmosphereSkyReproject},
        {"atmosphere_sky_cdf", passes.atmosphereSkyCdf},
        {"atmosphere_aerial_perspective", passes.atmosphereAerialPerspective},
        {"denoiser", passes.denoiser},
        {"moment_update", passes.momentUpdate},
        {"adaptive_sampling_diagnostics", passes.adaptiveSamplingDiagnostics},
        {"adaptive_sampling_fill", passes.adaptiveSamplingFill},
        {"history_copy", passes.historyCopy},
        {"skip_denoiser_copy", passes.skipDenoiserCopy},
        {"taa", passes.taa},
        {"taa_history_copy", passes.taaHistoryCopy},
        {"auto_exposure_histogram_clear", passes.autoExposureHistogramClear},
        {"auto_exposure_histogram", passes.autoExposureHistogram},
        {"auto_exposure_reduce", passes.autoExposureReduce},
        {"tone_map", passes.toneMap},
        {"selection_outline", passes.selectionOutline},
        {"fullscreen", passes.fullscreen},
        {"editor_presentation", passes.editorPresentation},
        {"wavefront_trace", passes.wavefrontTrace},
        {"wavefront_secondary_trace", passes.wavefrontSecondaryTrace},
        {"wavefront_sorted_trace", passes.wavefrontSortedTrace},
        {"wavefront_shadow_trace", passes.wavefrontShadowTrace},
        {"wavefront_shade", passes.wavefrontShade},
        {"wavefront_secondary_shade", passes.wavefrontSecondaryShade},
        {"wavefront_sorted_shade", passes.wavefrontSortedShade},
        {"wavefront_compact", passes.wavefrontCompact},
        {"wavefront_sort", passes.wavefrontSort},
    };
}

uint64_t bytesFromBudgetValue(const json& value) {
    if (value.is_number_unsigned() || value.is_number_integer()) {
        return value.get<uint64_t>();
    }
    if (value.is_number()) {
        return static_cast<uint64_t>(value.get<double>());
    }
    return 0;
}

json renderGraphOrEmpty(const std::optional<std::filesystem::path>& path);

struct RenderGraphTransientStats {
    uint64_t estimatedBytes = 0;
    uint64_t aliasEligibleBytes = 0;
    uint64_t activeAliasSavedBytes = 0;
    uint32_t transientResourceCount = 0;
    uint32_t aliasEligibleResourceCount = 0;
    uint32_t activeAliasCount = 0;
};

RenderGraphTransientStats transientStatsFromRenderGraph(const std::optional<std::filesystem::path>& renderGraphPath) {
    RenderGraphTransientStats stats{};
    const json graph = renderGraphOrEmpty(renderGraphPath);
    if (!graph.is_object()) {
        return stats;
    }

    for (const auto& resource : graph.value("resources", json::array())) {
        if (resource.value("lifetime", "") != "Transient") {
            continue;
        }
        ++stats.transientResourceCount;
        const json aliasing = resource.value("aliasing", json::object());
        const uint64_t estimatedBytes = uintAt(aliasing, {"estimated_bytes"});
        stats.estimatedBytes += estimatedBytes;
        if (aliasing.value("eligible", false)) {
            stats.aliasEligibleBytes += estimatedBytes;
            ++stats.aliasEligibleResourceCount;
        }
    }

    for (const auto& check : graph.value("alias_checks", json::array())) {
        if (!check.value("shared_physical_handle", false) || !check.value("physical_alias_candidate", false)) {
            continue;
        }
        ++stats.activeAliasCount;
        stats.activeAliasSavedBytes += bytesFromBudgetValue(check.value("estimated_saved_bytes", json(0)));
    }
    return stats;
}

double regressionPercent(double oldValue, double newValue) {
    if (std::abs(oldValue) <= 1.0e-9) {
        return std::abs(newValue) <= 1.0e-9 ? 0.0 : 100.0;
    }
    return ((newValue - oldValue) / oldValue) * 100.0;
}

json metricDelta(double oldValue, double newValue, const char* unit) {
    return {
        {"old", oldValue},
        {"new", newValue},
        {"delta", newValue - oldValue},
        {"regression_percent", regressionPercent(oldValue, newValue)},
        {"unit", unit},
    };
}

json comparePassTimingGroup(const json& oldProfile, const json& newProfile, const char* key) {
    json passJson = json::object();
    const json oldPasses = oldProfile.value(key, json::object());
    const json newPasses = newProfile.value(key, json::object());
    std::vector<std::string> passNames;
    for (auto it = oldPasses.begin(); it != oldPasses.end(); ++it) {
        passNames.push_back(it.key());
    }
    for (auto it = newPasses.begin(); it != newPasses.end(); ++it) {
        if (std::find(passNames.begin(), passNames.end(), it.key()) == passNames.end()) {
            passNames.push_back(it.key());
        }
    }
    std::sort(passNames.begin(), passNames.end());
    for (const std::string& name : passNames) {
        passJson[name] = metricDelta(oldPasses.value(name, 0.0), newPasses.value(name, 0.0), "ms");
    }
    return passJson;
}

json compareProfilesJson(const json& oldProfile, const json& newProfile) {
    json result;
    result["cpu_frame_ms"] = {
        {"avg", metricDelta(numberAt(oldProfile, {"cpu_frame_ms", "avg"}), numberAt(newProfile, {"cpu_frame_ms", "avg"}), "ms")},
        {"min", metricDelta(numberAt(oldProfile, {"cpu_frame_ms", "min"}), numberAt(newProfile, {"cpu_frame_ms", "min"}), "ms")},
        {"max", metricDelta(numberAt(oldProfile, {"cpu_frame_ms", "max"}), numberAt(newProfile, {"cpu_frame_ms", "max"}), "ms")},
        {"p95", metricDelta(numberAt(oldProfile, {"cpu_frame_ms", "p95"}), numberAt(newProfile, {"cpu_frame_ms", "p95"}), "ms")},
        {"p99", metricDelta(numberAt(oldProfile, {"cpu_frame_ms", "p99"}), numberAt(newProfile, {"cpu_frame_ms", "p99"}), "ms")},
    };
    result["gpu_frame_ms"] = {
        {"avg", metricDelta(numberAt(oldProfile, {"gpu_frame_ms", "avg"}), numberAt(newProfile, {"gpu_frame_ms", "avg"}), "ms")},
        {"min", metricDelta(numberAt(oldProfile, {"gpu_frame_ms", "min"}), numberAt(newProfile, {"gpu_frame_ms", "min"}), "ms")},
        {"max", metricDelta(numberAt(oldProfile, {"gpu_frame_ms", "max"}), numberAt(newProfile, {"gpu_frame_ms", "max"}), "ms")},
        {"p95", metricDelta(numberAt(oldProfile, {"gpu_frame_ms", "p95"}), numberAt(newProfile, {"gpu_frame_ms", "p95"}), "ms")},
        {"p99", metricDelta(numberAt(oldProfile, {"gpu_frame_ms", "p99"}), numberAt(newProfile, {"gpu_frame_ms", "p99"}), "ms")},
    };

    result["per_pass_gpu_ms"] = comparePassTimingGroup(oldProfile, newProfile, "per_pass_gpu_ms");
    if (oldProfile.contains("per_pass_gpu_ms_p95") || newProfile.contains("per_pass_gpu_ms_p95")) {
        result["per_pass_gpu_ms_p95"] = comparePassTimingGroup(oldProfile, newProfile, "per_pass_gpu_ms_p95");
    }
    if (oldProfile.contains("per_pass_gpu_ms_p99") || newProfile.contains("per_pass_gpu_ms_p99")) {
        result["per_pass_gpu_ms_p99"] = comparePassTimingGroup(oldProfile, newProfile, "per_pass_gpu_ms_p99");
    }

    json memoryJson = json::object();
    const json oldMemory = oldProfile.value("memory", json::object());
    const json newMemory = newProfile.value("memory", json::object());
    std::vector<std::string> memoryNames;
    for (auto it = oldMemory.begin(); it != oldMemory.end(); ++it) {
        memoryNames.push_back(it.key());
    }
    for (auto it = newMemory.begin(); it != newMemory.end(); ++it) {
        if (std::find(memoryNames.begin(), memoryNames.end(), it.key()) == memoryNames.end()) {
            memoryNames.push_back(it.key());
        }
    }
    std::sort(memoryNames.begin(), memoryNames.end());
    for (const std::string& name : memoryNames) {
        const json oldValueJson = oldMemory.value(name, json(0));
        const json newValueJson = newMemory.value(name, json(0));
        if (!oldValueJson.is_number() || !newValueJson.is_number()) {
            continue;
        }
        const uint64_t oldValue = oldValueJson.is_number_unsigned() || oldValueJson.is_number_integer()
            ? oldValueJson.get<uint64_t>()
            : static_cast<uint64_t>(oldValueJson.is_number() ? oldValueJson.get<double>() : 0.0);
        const uint64_t newValue = newValueJson.is_number_unsigned() || newValueJson.is_number_integer()
            ? newValueJson.get<uint64_t>()
            : static_cast<uint64_t>(newValueJson.is_number() ? newValueJson.get<double>() : 0.0);
        memoryJson[name] = metricDelta(static_cast<double>(oldValue), static_cast<double>(newValue), "bytes");
    }
    const uint64_t oldTotal = memoryTotalBytes(oldMemory);
    const uint64_t newTotal = memoryTotalBytes(newMemory);
    memoryJson["total_bytes"] = metricDelta(static_cast<double>(oldTotal), static_cast<double>(newTotal), "bytes");
    result["memory"] = memoryJson;
    result["summary"] = {
        {"gpu_avg_regression_percent", result["gpu_frame_ms"]["avg"]["regression_percent"]},
        {"cpu_avg_regression_percent", result["cpu_frame_ms"]["avg"]["regression_percent"]},
        {"memory_regression_percent", result["memory"]["total_bytes"]["regression_percent"]},
    };
    return result;
}

struct LoadedImage {
    int width = 0;
    int height = 0;
    std::vector<unsigned char> pixels;
};

LoadedImage loadImageRgba(const std::filesystem::path& path) {
    int width = 0;
    int height = 0;
    int channels = 0;
    unsigned char* data = stbi_load(path.string().c_str(), &width, &height, &channels, STBI_rgb_alpha);
    if (data == nullptr) {
        throw std::runtime_error("Failed to load image: " + path.string());
    }
    LoadedImage image;
    image.width = width;
    image.height = height;
    image.pixels.assign(data, data + static_cast<size_t>(width) * height * 4u);
    stbi_image_free(data);
    return image;
}

double luminance(unsigned char r, unsigned char g, unsigned char b) {
    return 0.2126 * static_cast<double>(r) + 0.7152 * static_cast<double>(g) + 0.0722 * static_cast<double>(b);
}

json imageMetricsJson(const ImageDiffMetrics& metrics) {
    return {
        {"width", metrics.width},
        {"height", metrics.height},
        {"mse", metrics.mse},
        {"psnr", metrics.psnr},
        {"ssim", metrics.ssim},
        {"max_error", metrics.maxError},
        {"changed_pixel_percentage", metrics.changedPixelPercentage},
        {"baseline_mean_luminance", metrics.baselineMeanLuminance},
        {"current_mean_luminance", metrics.currentMeanLuminance},
        {"mean_luminance_bias", metrics.meanLuminanceBias},
        {"mean_luminance_bias_percentage", metrics.meanLuminanceBiasPercentage},
        {"mean_red_bias", metrics.meanRedBias},
        {"mean_green_bias", metrics.meanGreenBias},
        {"mean_blue_bias", metrics.meanBlueBias},
        {"max_abs_mean_channel_bias", metrics.maxAbsMeanChannelBias},
    };
}

json sequenceMetricSummaryJson(const SequenceMetricSummary& metrics) {
    return {
        {"frame_count", metrics.frameCount},
        {"average_mse", metrics.averageMse},
        {"max_mse", metrics.maxMse},
        {"average_psnr", metrics.averagePsnr},
        {"worst_psnr", metrics.worstPsnr},
        {"average_ssim", metrics.averageSsim},
        {"best_ssim", metrics.bestSsim},
        {"worst_ssim", metrics.worstSsim},
        {"average_changed_pixel_percentage", metrics.averageChangedPixelPercentage},
        {"max_changed_pixel_percentage", metrics.maxChangedPixelPercentage},
        {"worst_frame", metrics.worstFrame},
    };
}

json sequenceTemporalMetricsJson(const SequenceTemporalMetrics& metrics) {
    return {
        {"pair_count", metrics.pairCount},
        {"average_frame_delta_mse", metrics.averageFrameDeltaMse},
        {"max_frame_delta_mse", metrics.maxFrameDeltaMse},
        {"average_frame_delta_changed_pixel_percentage", metrics.averageFrameDeltaChangedPixelPercentage},
        {"max_frame_delta_changed_pixel_percentage", metrics.maxFrameDeltaChangedPixelPercentage},
        {"temporal_variance_score", metrics.temporalVarianceScore},
        {"worst_pair_start_frame", metrics.worstPairStartFrame},
    };
}

struct SequenceFrameSample {
    std::string fileName;
    uint32_t frameIndex = 0;
    ImageDiffMetrics metrics{};
};

struct InternalSequenceViewComparison {
    SequenceViewComparison summary;
    std::vector<SequenceFrameSample> perFrame;
};

uint32_t frameIndexFromName(const std::string& name, uint32_t fallback) {
    const size_t dot = name.find_last_of('.');
    const size_t end = dot == std::string::npos ? name.size() : dot;
    size_t begin = end;
    while (begin > 0 && std::isdigit(static_cast<unsigned char>(name[begin - 1u]))) {
        --begin;
    }
    if (begin == end) {
        return fallback;
    }
    return static_cast<uint32_t>(std::stoul(name.substr(begin, end - begin)));
}

std::string sequenceFrameFileName(uint32_t frameIndex) {
    std::ostringstream stream;
    stream << "frame_" << std::setw(4) << std::setfill('0') << frameIndex << ".png";
    return stream.str();
}

std::vector<std::string> pngNamesInDir(const std::filesystem::path& dir) {
    std::vector<std::string> names;
    if (!std::filesystem::exists(dir)) {
        return names;
    }
    for (const auto& entry : std::filesystem::directory_iterator(dir)) {
        if (!entry.is_regular_file()) {
            continue;
        }
        if (entry.path().extension() == ".png") {
            names.push_back(entry.path().filename().string());
        }
    }
    std::sort(names.begin(), names.end());
    return names;
}

std::vector<std::string> viewNamesInSequenceDir(const std::filesystem::path& dir) {
    std::vector<std::string> views;
    const auto manifestPath = dir / "sequence_manifest.json";
    if (std::filesystem::exists(manifestPath)) {
        const json manifest = readJsonFile(manifestPath);
        for (const auto& view : manifest.value("views", json::array())) {
            views.push_back(view.get<std::string>());
        }
    }
    if (views.empty() && std::filesystem::exists(dir)) {
        for (const auto& entry : std::filesystem::directory_iterator(dir)) {
            if (entry.is_directory()) {
                views.push_back(entry.path().filename().string());
            }
        }
    }
    std::sort(views.begin(), views.end());
    views.erase(std::unique(views.begin(), views.end()), views.end());
    return views;
}

std::vector<std::string> selectedSequenceViews(
    const std::filesystem::path& baselineDir,
    const std::filesystem::path& currentDir,
    const std::vector<std::string>& requestedViews,
    std::vector<std::string>& warnings) {
    if (!requestedViews.empty()) {
        return requestedViews;
    }
    std::vector<std::string> baselineViews = viewNamesInSequenceDir(baselineDir);
    std::vector<std::string> currentViews = viewNamesInSequenceDir(currentDir);
    std::vector<std::string> common;
    std::set_intersection(
        baselineViews.begin(), baselineViews.end(),
        currentViews.begin(), currentViews.end(),
        std::back_inserter(common));
    if (common.empty()) {
        warnings.push_back("No common sequence views found in baseline and current directories");
    }
    return common;
}

SequenceMetricSummary summarizeFrameSamples(const std::vector<SequenceFrameSample>& samples) {
    SequenceMetricSummary summary;
    summary.frameCount = static_cast<uint32_t>(samples.size());
    if (samples.empty()) {
        return summary;
    }

    summary.maxMse = 0.0;
    summary.bestSsim = -std::numeric_limits<double>::infinity();
    summary.worstSsim = std::numeric_limits<double>::infinity();
    summary.worstPsnr = std::numeric_limits<double>::infinity();
    double sumMse = 0.0;
    double sumPsnr = 0.0;
    double sumSsim = 0.0;
    double sumChanged = 0.0;

    for (const auto& sample : samples) {
        const auto& m = sample.metrics;
        sumMse += m.mse;
        sumPsnr += m.psnr;
        sumSsim += m.ssim;
        sumChanged += m.changedPixelPercentage;
        summary.bestSsim = std::max(summary.bestSsim, m.ssim);
        summary.worstSsim = std::min(summary.worstSsim, m.ssim);
        summary.worstPsnr = std::min(summary.worstPsnr, m.psnr);
        summary.maxChangedPixelPercentage = std::max(summary.maxChangedPixelPercentage, m.changedPixelPercentage);
        if (m.mse > summary.maxMse) {
            summary.maxMse = m.mse;
            summary.worstFrame = sample.frameIndex;
        }
    }

    const double inv = 1.0 / static_cast<double>(samples.size());
    summary.averageMse = sumMse * inv;
    summary.averagePsnr = sumPsnr * inv;
    summary.averageSsim = sumSsim * inv;
    summary.averageChangedPixelPercentage = sumChanged * inv;
    return summary;
}

SequenceTemporalMetrics summarizeTemporalPairs(
    const std::filesystem::path& viewDir,
    const std::vector<std::string>& frameNames) {
    SequenceTemporalMetrics metrics;
    if (frameNames.size() < 2u) {
        return metrics;
    }

    double sumMse = 0.0;
    double sumChanged = 0.0;
    for (size_t i = 1; i < frameNames.size(); ++i) {
        const ImageDiffMetrics pair = compareImages(viewDir / frameNames[i - 1u], viewDir / frameNames[i], std::nullopt);
        ++metrics.pairCount;
        sumMse += pair.mse;
        sumChanged += pair.changedPixelPercentage;
        if (pair.mse > metrics.maxFrameDeltaMse) {
            metrics.maxFrameDeltaMse = pair.mse;
            metrics.worstPairStartFrame = frameIndexFromName(frameNames[i - 1u], static_cast<uint32_t>(i - 1u));
        }
        metrics.maxFrameDeltaChangedPixelPercentage = std::max(
            metrics.maxFrameDeltaChangedPixelPercentage,
            pair.changedPixelPercentage);
    }

    if (metrics.pairCount > 0u) {
        const double inv = 1.0 / static_cast<double>(metrics.pairCount);
        metrics.averageFrameDeltaMse = sumMse * inv;
        metrics.averageFrameDeltaChangedPixelPercentage = sumChanged * inv;
        metrics.temporalVarianceScore = metrics.averageFrameDeltaMse *
            (metrics.averageFrameDeltaChangedPixelPercentage / 100.0);
    }
    return metrics;
}

std::vector<double> temporalPairMseValues(
    const std::filesystem::path& viewDir,
    const std::vector<std::string>& frameNames) {
    std::vector<double> values;
    if (frameNames.size() < 2u) {
        return values;
    }
    values.reserve(frameNames.size() - 1u);
    for (size_t i = 1; i < frameNames.size(); ++i) {
        const ImageDiffMetrics pair = compareImages(viewDir / frameNames[i - 1u], viewDir / frameNames[i], std::nullopt);
        values.push_back(pair.mse);
    }
    return values;
}

double pearsonCorrelation(const std::vector<double>& a, const std::vector<double>& b) {
    if (a.size() != b.size() || a.size() < 2u) {
        return 0.0;
    }
    const double inv = 1.0 / static_cast<double>(a.size());
    const double meanA = std::accumulate(a.begin(), a.end(), 0.0) * inv;
    const double meanB = std::accumulate(b.begin(), b.end(), 0.0) * inv;
    double covariance = 0.0;
    double varA = 0.0;
    double varB = 0.0;
    for (size_t i = 0; i < a.size(); ++i) {
        const double da = a[i] - meanA;
        const double db = b[i] - meanB;
        covariance += da * db;
        varA += da * da;
        varB += db * db;
    }
    if (varA <= 1.0e-12 || varB <= 1.0e-12) {
        return 0.0;
    }
    return covariance / std::sqrt(varA * varB);
}

void writeContactSheet(
    const std::filesystem::path& diffDir,
    const std::filesystem::path& outputPath,
    std::vector<SequenceFrameSample> samples,
    uint32_t maxFrames) {
    if (samples.empty()) {
        return;
    }
    std::sort(samples.begin(), samples.end(), [](const auto& lhs, const auto& rhs) {
        return lhs.metrics.mse > rhs.metrics.mse;
    });
    if (samples.size() > maxFrames) {
        samples.resize(maxFrames);
    }

    std::vector<LoadedImage> images;
    images.reserve(samples.size());
    for (const auto& sample : samples) {
        const auto path = diffDir / sample.fileName;
        if (std::filesystem::exists(path)) {
            images.push_back(loadImageRgba(path));
        }
    }
    if (images.empty()) {
        return;
    }

    const int tileWidth = images.front().width;
    const int tileHeight = images.front().height;
    const int columns = std::min<int>(4, static_cast<int>(images.size()));
    const int rows = (static_cast<int>(images.size()) + columns - 1) / columns;
    std::vector<unsigned char> sheet(static_cast<size_t>(tileWidth) * tileHeight * columns * rows * 4u, 255u);
    const int sheetWidth = tileWidth * columns;

    for (size_t index = 0; index < images.size(); ++index) {
        const LoadedImage& image = images[index];
        if (image.width != tileWidth || image.height != tileHeight) {
            continue;
        }
        const int dstX = static_cast<int>(index % static_cast<size_t>(columns)) * tileWidth;
        const int dstY = static_cast<int>(index / static_cast<size_t>(columns)) * tileHeight;
        for (int y = 0; y < tileHeight; ++y) {
            const size_t srcOffset = static_cast<size_t>(y) * tileWidth * 4u;
            const size_t dstOffset = (static_cast<size_t>(dstY + y) * sheetWidth + dstX) * 4u;
            std::copy_n(image.pixels.data() + srcOffset, static_cast<size_t>(tileWidth) * 4u, sheet.data() + dstOffset);
        }
    }

    const auto dir = outputPath.parent_path();
    if (!dir.empty()) {
        std::filesystem::create_directories(dir);
    }
    stbi_write_png(outputPath.string().c_str(), sheetWidth, tileHeight * rows, 4, sheet.data(), sheetWidth * 4);
}

std::vector<std::string> passNamesFromRenderGraph(const json& graph) {
    std::vector<std::string> names;
    for (const auto& pass : graph.value("passes", json::array())) {
        names.push_back(pass.value("name", ""));
    }
    return names;
}

std::filesystem::path beautyPath(const std::filesystem::path& debugViewsDir) {
    return debugViewsDir / "beauty.png";
}

std::string hexHashBytes(const std::vector<unsigned char>& bytes) {
    uint64_t hash = 1469598103934665603ull;
    for (unsigned char byte : bytes) {
        hash ^= static_cast<uint64_t>(byte);
        hash *= 1099511628211ull;
    }
    std::ostringstream stream;
    stream << std::hex << std::setw(16) << std::setfill('0') << hash;
    return stream.str();
}

std::vector<unsigned char> readBinary(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file.is_open()) {
        throw std::runtime_error("Failed to open file: " + path.string());
    }
    return std::vector<unsigned char>(
        std::istreambuf_iterator<char>(file),
        std::istreambuf_iterator<char>());
}

std::vector<uint32_t> bytesToWords(const std::vector<unsigned char>& bytes) {
    if (bytes.size() % sizeof(uint32_t) != 0) {
        throw std::runtime_error("SPIR-V byte size is not aligned to 32-bit words");
    }
    std::vector<uint32_t> words(bytes.size() / sizeof(uint32_t));
    if (!words.empty()) {
        std::memcpy(words.data(), bytes.data(), bytes.size());
    }
    return words;
}

std::string descriptorTypeName(VkDescriptorType type) {
    switch (type) {
    case VK_DESCRIPTOR_TYPE_SAMPLER: return "sampler";
    case VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER: return "combined_image_sampler";
    case VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE: return "sampled_image";
    case VK_DESCRIPTOR_TYPE_STORAGE_IMAGE: return "storage_image";
    case VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER: return "uniform_texel_buffer";
    case VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER: return "storage_texel_buffer";
    case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER: return "uniform_buffer";
    case VK_DESCRIPTOR_TYPE_STORAGE_BUFFER: return "storage_buffer";
    case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC: return "uniform_buffer_dynamic";
    case VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC: return "storage_buffer_dynamic";
    case VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT: return "input_attachment";
    case VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR: return "acceleration_structure";
    default: return "unknown";
    }
}

std::string shaderStageFlagsName(VkShaderStageFlags stages) {
    std::vector<std::string> names;
    if ((stages & VK_SHADER_STAGE_VERTEX_BIT) != 0) {
        names.push_back("vertex");
    }
    if ((stages & VK_SHADER_STAGE_FRAGMENT_BIT) != 0) {
        names.push_back("fragment");
    }
    if ((stages & VK_SHADER_STAGE_COMPUTE_BIT) != 0) {
        names.push_back("compute");
    }
    if ((stages & VK_SHADER_STAGE_RAYGEN_BIT_KHR) != 0) {
        names.push_back("raygen");
    }
    if ((stages & VK_SHADER_STAGE_ANY_HIT_BIT_KHR) != 0) {
        names.push_back("any_hit");
    }
    if ((stages & VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR) != 0) {
        names.push_back("closest_hit");
    }
    if ((stages & VK_SHADER_STAGE_MISS_BIT_KHR) != 0) {
        names.push_back("miss");
    }
    if ((stages & VK_SHADER_STAGE_INTERSECTION_BIT_KHR) != 0) {
        names.push_back("intersection");
    }
    if ((stages & VK_SHADER_STAGE_CALLABLE_BIT_KHR) != 0) {
        names.push_back("callable");
    }
    if (names.empty()) {
        return "none";
    }
    std::ostringstream stream;
    for (size_t i = 0; i < names.size(); ++i) {
        if (i > 0) {
            stream << "|";
        }
        stream << names[i];
    }
    return stream.str();
}

struct DescriptorExpectation {
    const char* setName = "";
    uint32_t set = 0;
    uint32_t binding = 0;
    VkDescriptorType type = VK_DESCRIPTOR_TYPE_MAX_ENUM;
    const char* role = "";
};

std::vector<DescriptorExpectation> rendererDescriptorExpectations() {
    return {
        {"raytracing_set_0", 0, 38, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, "ReSTIR DI current reservoir storage buffer"},
        {"raytracing_set_0", 0, 39, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, "ReSTIR DI previous reservoir storage buffer"},
        {"raytracing_set_0", 0, 40, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, "ReGIR light BVH nodes storage buffer"},
        {"raytracing_set_0", 0, 42, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, "path data storage buffer"},
        {"raytracing_set_0", 0, 43, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, "ReSTIR GI current reservoir storage buffer"},
        {"raytracing_set_0", 0, 44, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, "ReSTIR GI previous reservoir storage buffer"},
        {"raytracing_set_0", 0, 45, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, "ReSTIR GI spatial reservoir storage buffer"},
        {"bindless_texture_heap_set_2", 2, 0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, "bindless combined image sampler heap"},
    };
}

json descriptorExpectationJson(const DescriptorExpectation& expectation) {
    return {
        {"set_name", expectation.setName},
        {"set", expectation.set},
        {"binding", expectation.binding},
        {"type", descriptorTypeName(expectation.type)},
        {"role", expectation.role},
        {"required", true},
    };
}

json descriptorExpectationsJson() {
    json rows = json::array();
    for (const DescriptorExpectation& expectation : rendererDescriptorExpectations()) {
        rows.push_back(descriptorExpectationJson(expectation));
    }
    return rows;
}

json reflectedBindingJson(const ReflectedBinding& binding) {
    return {
        {"set", binding.set},
        {"binding", binding.binding},
        {"count", binding.count},
        {"type", descriptorTypeName(binding.type)},
        {"stage_flags", binding.stages},
        {"stages", shaderStageFlagsName(binding.stages)},
    };
}

struct ReflectedBindingRecord {
    ReflectedBinding binding;
    std::string source;
};

struct ReflectedBindingSummary {
    uint32_t set = 0;
    uint32_t binding = 0;
    uint32_t minCount = 0;
    uint32_t maxCount = 0;
    uint32_t occurrenceCount = 0;
    std::set<std::string> types;
    std::set<std::string> stages;
    std::set<std::string> sources;
};

json reflectedBindingSummaryJson(const ReflectedBindingSummary& summary) {
    json sources = json::array();
    uint32_t sourceCount = 0;
    for (const std::string& source : summary.sources) {
        if (sourceCount >= 16) {
            break;
        }
        sources.push_back(source);
        ++sourceCount;
    }
    return {
        {"set", summary.set},
        {"binding", summary.binding},
        {"min_count", summary.minCount},
        {"max_count", summary.maxCount},
        {"occurrence_count", summary.occurrenceCount},
        {"types", summary.types},
        {"stages", summary.stages},
        {"sources", sources},
        {"truncated_sources", summary.sources.size() > sources.size()},
    };
}

json descriptorReflectionAuditJson(
    const std::vector<ReflectedBindingRecord>& reflectedBindings,
    bool reflectionPassed) {
    std::map<std::pair<uint32_t, uint32_t>, ReflectedBindingSummary> summaries;
    std::set<uint32_t> reflectedSets;
    for (const ReflectedBindingRecord& record : reflectedBindings) {
        reflectedSets.insert(record.binding.set);
        auto& summary = summaries[{record.binding.set, record.binding.binding}];
        if (summary.occurrenceCount == 0) {
            summary.set = record.binding.set;
            summary.binding = record.binding.binding;
            summary.minCount = record.binding.count;
            summary.maxCount = record.binding.count;
        } else {
            summary.minCount = std::min(summary.minCount, record.binding.count);
            summary.maxCount = std::max(summary.maxCount, record.binding.count);
        }
        ++summary.occurrenceCount;
        summary.types.insert(descriptorTypeName(record.binding.type));
        summary.stages.insert(shaderStageFlagsName(record.binding.stages));
        summary.sources.insert(record.source);
    }

    json reflectedBindingRows = json::array();
    for (const auto& [key, summary] : summaries) {
        (void)key;
        reflectedBindingRows.push_back(reflectedBindingSummaryJson(summary));
    }

    json expectations = json::array();
    json failures = json::array();
    for (const DescriptorExpectation& expectation : rendererDescriptorExpectations()) {
        json matches = json::array();
        bool hasBinding = false;
        bool hasTypeMatch = false;
        for (const ReflectedBindingRecord& record : reflectedBindings) {
            if (record.binding.set != expectation.set || record.binding.binding != expectation.binding) {
                continue;
            }
            hasBinding = true;
            const bool typeMatches = record.binding.type == expectation.type;
            hasTypeMatch = hasTypeMatch || typeMatches;
            matches.push_back({
                {"source", record.source},
                {"count", record.binding.count},
                {"type", descriptorTypeName(record.binding.type)},
                {"stages", shaderStageFlagsName(record.binding.stages)},
                {"type_matches", typeMatches},
            });
        }
        const bool passed = hasBinding && hasTypeMatch;
        json expectationJson = descriptorExpectationJson(expectation);
        expectationJson["reflected"] = hasBinding;
        expectationJson["type_matched"] = hasTypeMatch;
        expectationJson["match_count"] = matches.size();
        expectationJson["matches"] = matches;
        expectationJson["passed"] = passed;
        expectations.push_back(expectationJson);
        if (!passed) {
            failures.push_back({
                {"set_name", expectation.setName},
                {"set", expectation.set},
                {"binding", expectation.binding},
                {"expected_type", descriptorTypeName(expectation.type)},
                {"role", expectation.role},
                {"reason", hasBinding ? "descriptor type mismatch" : "descriptor binding not reflected by compiled shaders"},
            });
        }
    }

    return {
        {"schema_version", 1},
        {"reflection_passed", reflectionPassed},
        {"reflected_binding_occurrence_count", reflectedBindings.size()},
        {"reflected_unique_binding_count", summaries.size()},
        {"reflected_descriptor_set_count", reflectedSets.size()},
        {"reflected_descriptor_bindings", reflectedBindingRows},
        {"known_set_expectation_count", rendererDescriptorExpectations().size()},
        {"known_set_expectations", expectations},
        {"failure_count", failures.size()},
        {"failures", failures},
        {"passed", reflectionPassed && !reflectedBindings.empty() && failures.empty()},
        {"notes", json::array({
            "Descriptor count 0 is used by SPIR-V reflection for runtime-sized descriptor arrays.",
            "This audit verifies the renderer's documented ray tracing and bindless descriptor contracts against compiled SPIR-V.",
        })},
    };
}

json descriptorWriteRecordJson(const DescriptorWriteDiagnosticRecord& record) {
    return {
        {"sequence", record.sequence},
        {"descriptor_set", record.descriptorSet},
        {"descriptor_set_layout", record.descriptorSetLayout},
        {"binding", record.binding},
        {"array_element", record.arrayElement},
        {"count", record.count},
        {"type", descriptorTypeName(record.type)},
        {"kind", record.kind},
        {"source", record.source},
        {"owner", record.owner},
        {"pass", record.pass},
        {"set_name", record.setName},
        {"set_index", record.setIndex},
    };
}

json descriptorWriteAggregateJson(const DescriptorWriteDiagnosticAggregate& aggregate) {
    return {
        {"descriptor_set_layout", aggregate.descriptorSetLayout},
        {"binding", aggregate.binding},
        {"type", descriptorTypeName(aggregate.type)},
        {"kind", aggregate.kind},
        {"source", aggregate.source},
        {"owner", aggregate.owner},
        {"pass", aggregate.pass},
        {"set_name", aggregate.setName},
        {"set_index", aggregate.setIndex},
        {"min_count", aggregate.minCount},
        {"max_count", aggregate.maxCount},
        {"occurrence_count", aggregate.occurrenceCount},
    };
}

json descriptorWriteDiagnosticsJson(const DescriptorWriteDiagnosticsSnapshot& snapshot) {
    json recentWrites = json::array();
    for (const DescriptorWriteDiagnosticRecord& record : snapshot.recentWrites) {
        recentWrites.push_back(descriptorWriteRecordJson(record));
    }
    json aggregates = json::array();
    for (const DescriptorWriteDiagnosticAggregate& aggregate : snapshot.aggregates) {
        aggregates.push_back(descriptorWriteAggregateJson(aggregate));
    }
    return {
        {"schema_version", 1},
        {"update_call_count", snapshot.updateCallCount},
        {"write_count", snapshot.writeCount},
        {"unique_write_count", snapshot.aggregates.size()},
        {"recent_write_limit", snapshot.recentWriteLimit},
        {"dropped_recent_write_count", snapshot.droppedRecentWriteCount},
        {"recent_writes", recentWrites},
        {"unique_writes", aggregates},
    };
}

json actualDescriptorWriteAuditJson(const DescriptorWriteDiagnosticsSnapshot& snapshot) {
    json expectations = json::array();
    json failures = json::array();
    for (const DescriptorExpectation& expectation : rendererDescriptorExpectations()) {
        bool matched = false;
        uint64_t matchCount = 0;
        uint64_t occurrenceCount = 0;
        json matches = json::array();
        for (const DescriptorWriteDiagnosticAggregate& aggregate : snapshot.aggregates) {
            if (aggregate.binding != expectation.binding ||
                aggregate.type != expectation.type ||
                aggregate.setName != expectation.setName) {
                continue;
            }
            matched = true;
            ++matchCount;
            occurrenceCount += aggregate.occurrenceCount;
            matches.push_back(descriptorWriteAggregateJson(aggregate));
        }

        json expectationJson = descriptorExpectationJson(expectation);
        expectationJson["actual_write_matched"] = matched;
        expectationJson["match_count"] = matchCount;
        expectationJson["occurrence_count"] = occurrenceCount;
        expectationJson["matches"] = matches;
        expectationJson["passed"] = matched;
        expectations.push_back(expectationJson);
        if (!matched) {
            failures.push_back({
                {"set_name", expectation.setName},
                {"set", expectation.set},
                {"binding", expectation.binding},
                {"expected_type", descriptorTypeName(expectation.type)},
                {"role", expectation.role},
                {"reason", "expected descriptor binding was not observed in actual descriptor writes"},
            });
        }
    }
    return {
        {"schema_version", 1},
        {"write_count", snapshot.writeCount},
        {"unique_write_count", snapshot.aggregates.size()},
        {"known_set_expectation_count", rendererDescriptorExpectations().size()},
        {"known_set_expectations", expectations},
        {"failure_count", failures.size()},
        {"failures", failures},
        {"passed", snapshot.writeCount > 0 && failures.empty()},
        {"notes", json::array({
            "This audit is based on actual Vulkan descriptor updates recorded through DescriptorWriter and BindlessTextureHeap.",
            "Expected bindings must match explicit owner-provided descriptor set names.",
        })},
    };
}

json reflectedPushConstantJson(const ReflectedPushConstant& pushConstant) {
    return {
        {"offset", pushConstant.offset},
        {"size", pushConstant.size},
        {"stage_flags", pushConstant.stages},
        {"stages", shaderStageFlagsName(pushConstant.stages)},
    };
}

json renderGraphOrEmpty(const std::optional<std::filesystem::path>& path) {
    if (!path.has_value() || !std::filesystem::exists(*path)) {
        return json::object();
    }
    return readJsonFile(*path);
}

} // namespace

int compareProfileCommand(const std::filesystem::path& oldPath, const std::filesystem::path& newPath) {
    const json result = compareProfilesJson(readJsonFile(oldPath), readJsonFile(newPath));
    std::cout << result.dump(2) << "\n";
    return 0;
}

ImageDiffMetrics compareImages(
    const std::filesystem::path& baselinePath,
    const std::filesystem::path& currentPath,
    const std::optional<std::filesystem::path>& diffOutputPath) {
    const LoadedImage baseline = loadImageRgba(baselinePath);
    const LoadedImage current = loadImageRgba(currentPath);
    if (baseline.width != current.width || baseline.height != current.height) {
        throw std::runtime_error("Image dimensions differ: " + baselinePath.string() + " vs " + currentPath.string());
    }

    const size_t pixelCount = static_cast<size_t>(baseline.width) * baseline.height;
    std::vector<unsigned char> diff(pixelCount * 4u, 255u);
    double sumSquared = 0.0;
    double maxError = 0.0;
    uint64_t changedPixels = 0;
    double sumX = 0.0;
    double sumY = 0.0;
    double sumX2 = 0.0;
    double sumY2 = 0.0;
    double sumXY = 0.0;
    double sumRedDelta = 0.0;
    double sumGreenDelta = 0.0;
    double sumBlueDelta = 0.0;

    for (size_t pixel = 0; pixel < pixelCount; ++pixel) {
        const size_t offset = pixel * 4u;
        bool changed = false;
        uint32_t pixelMax = 0;
        for (size_t channel = 0; channel < 4u; ++channel) {
            const int delta = static_cast<int>(current.pixels[offset + channel]) - static_cast<int>(baseline.pixels[offset + channel]);
            const uint32_t absDelta = static_cast<uint32_t>(std::abs(delta));
            sumSquared += static_cast<double>(delta * delta);
            pixelMax = std::max(pixelMax, absDelta);
            maxError = std::max(maxError, static_cast<double>(absDelta));
            if (absDelta != 0u) {
                changed = true;
            }
            diff[offset + channel] = channel == 3u ? 255u : static_cast<unsigned char>(std::min<uint32_t>(absDelta * 4u, 255u));
        }
        if (changed) {
            ++changedPixels;
        }
        const double x = luminance(baseline.pixels[offset], baseline.pixels[offset + 1u], baseline.pixels[offset + 2u]);
        const double y = luminance(current.pixels[offset], current.pixels[offset + 1u], current.pixels[offset + 2u]);
        sumRedDelta += static_cast<double>(current.pixels[offset]) - static_cast<double>(baseline.pixels[offset]);
        sumGreenDelta += static_cast<double>(current.pixels[offset + 1u]) - static_cast<double>(baseline.pixels[offset + 1u]);
        sumBlueDelta += static_cast<double>(current.pixels[offset + 2u]) - static_cast<double>(baseline.pixels[offset + 2u]);
        sumX += x;
        sumY += y;
        sumX2 += x * x;
        sumY2 += y * y;
        sumXY += x * y;
        (void)pixelMax;
    }

    const double sampleCount = static_cast<double>(pixelCount);
    const double channelCount = sampleCount * 4.0;
    const double mse = channelCount > 0.0 ? sumSquared / channelCount : 0.0;
    const double psnr = mse <= 1.0e-12 ? 99.0 : 20.0 * std::log10(255.0 / std::sqrt(mse));
    const double meanX = sumX / sampleCount;
    const double meanY = sumY / sampleCount;
    const double varianceX = std::max(0.0, sumX2 / sampleCount - meanX * meanX);
    const double varianceY = std::max(0.0, sumY2 / sampleCount - meanY * meanY);
    const double covariance = sumXY / sampleCount - meanX * meanY;
    constexpr double c1 = 6.5025;
    constexpr double c2 = 58.5225;
    const double ssim = ((2.0 * meanX * meanY + c1) * (2.0 * covariance + c2)) /
        ((meanX * meanX + meanY * meanY + c1) * (varianceX + varianceY + c2));

    if (diffOutputPath.has_value()) {
        const auto dir = diffOutputPath->parent_path();
        if (!dir.empty()) {
            std::filesystem::create_directories(dir);
        }
        if (stbi_write_png(
                diffOutputPath->string().c_str(),
                baseline.width,
                baseline.height,
                4,
                diff.data(),
                baseline.width * 4) == 0) {
            throw std::runtime_error("Failed to write diff image: " + diffOutputPath->string());
        }
    }

    ImageDiffMetrics metrics;
    metrics.width = static_cast<uint32_t>(baseline.width);
    metrics.height = static_cast<uint32_t>(baseline.height);
    metrics.mse = mse;
    metrics.psnr = psnr;
    metrics.ssim = ssim;
    metrics.maxError = static_cast<uint32_t>(maxError);
    metrics.changedPixelPercentage = sampleCount > 0.0
        ? (static_cast<double>(changedPixels) / sampleCount) * 100.0
        : 0.0;
    metrics.baselineMeanLuminance = meanX;
    metrics.currentMeanLuminance = meanY;
    metrics.meanLuminanceBias = meanY - meanX;
    metrics.meanLuminanceBiasPercentage = std::abs(meanX) > 1.0e-8
        ? (metrics.meanLuminanceBias / meanX) * 100.0
        : 0.0;
    metrics.meanRedBias = sampleCount > 0.0 ? sumRedDelta / sampleCount : 0.0;
    metrics.meanGreenBias = sampleCount > 0.0 ? sumGreenDelta / sampleCount : 0.0;
    metrics.meanBlueBias = sampleCount > 0.0 ? sumBlueDelta / sampleCount : 0.0;
    metrics.maxAbsMeanChannelBias = std::max({
        std::abs(metrics.meanRedBias),
        std::abs(metrics.meanGreenBias),
        std::abs(metrics.meanBlueBias)});
    return metrics;
}

int compareImageCommand(
    const std::filesystem::path& baselinePath,
    const std::filesystem::path& currentPath,
    const std::optional<std::filesystem::path>& diffOutputPath,
    const ImageCompareThresholds& thresholds) {
    const ImageDiffMetrics metrics = compareImages(baselinePath, currentPath, diffOutputPath);
    json result = imageMetricsJson(metrics);
    if (diffOutputPath.has_value()) {
        result["diff_image"] = diffOutputPath->string();
    }
    std::vector<std::string> failures;
    if (thresholds.minPsnr.has_value() && metrics.psnr < *thresholds.minPsnr) {
        failures.push_back("psnr below threshold");
    }
    if (thresholds.minSsim.has_value() && metrics.ssim < *thresholds.minSsim) {
        failures.push_back("ssim below threshold");
    }
    if (thresholds.maxChangedPixelPercentage.has_value() &&
        metrics.changedPixelPercentage > *thresholds.maxChangedPixelPercentage) {
        failures.push_back("changed pixel percentage above threshold");
    }
    result["status"] = failures.empty() ? "pass" : "fail";
    result["failures"] = failures;
    result["thresholds"] = {
        {"min_psnr", thresholds.minPsnr.has_value() ? json(*thresholds.minPsnr) : json(nullptr)},
        {"min_ssim", thresholds.minSsim.has_value() ? json(*thresholds.minSsim) : json(nullptr)},
        {"max_changed_pixel_percentage", thresholds.maxChangedPixelPercentage.has_value()
            ? json(*thresholds.maxChangedPixelPercentage)
            : json(nullptr)},
    };
    std::cout << result.dump(2) << "\n";
    return failures.empty() ? 0 : 1;
}

SequenceComparisonReport compareImageSequences(
    const std::filesystem::path& baselineDir,
    const std::filesystem::path& currentDir,
    const std::optional<std::filesystem::path>& outputDir,
    const std::vector<std::string>& requestedViews) {
    SequenceComparisonReport report;
    if (!std::filesystem::exists(baselineDir)) {
        throw std::runtime_error("Baseline sequence directory does not exist: " + baselineDir.string());
    }
    if (!std::filesystem::exists(currentDir)) {
        throw std::runtime_error("Current sequence directory does not exist: " + currentDir.string());
    }

    const std::vector<std::string> views = selectedSequenceViews(baselineDir, currentDir, requestedViews, report.warnings);
    std::vector<InternalSequenceViewComparison> internalViews;
    bool anyComparableFrames = false;

    for (const std::string& view : views) {
        const std::filesystem::path baselineViewDir = baselineDir / view;
        const std::filesystem::path currentViewDir = currentDir / view;
        if (!std::filesystem::exists(baselineViewDir) || !std::filesystem::exists(currentViewDir)) {
            report.warnings.push_back("Skipping view with missing directory: " + view);
            continue;
        }

        const std::vector<std::string> baselineFrames = pngNamesInDir(baselineViewDir);
        const std::vector<std::string> currentFrames = pngNamesInDir(currentViewDir);
        std::vector<std::string> commonFrames;
        std::set_intersection(
            baselineFrames.begin(), baselineFrames.end(),
            currentFrames.begin(), currentFrames.end(),
            std::back_inserter(commonFrames));
        if (commonFrames.empty()) {
            report.warnings.push_back("Skipping view with no matching PNG frames: " + view);
            continue;
        }
        if (commonFrames.size() != baselineFrames.size() || commonFrames.size() != currentFrames.size()) {
            report.warnings.push_back("View has missing or extra frames: " + view);
        }

        anyComparableFrames = true;
        InternalSequenceViewComparison internal;
        internal.summary.view = view;
        const std::filesystem::path diffDir = outputDir.has_value() ? (*outputDir / "diffs" / view) : std::filesystem::path{};

        for (size_t index = 0; index < commonFrames.size(); ++index) {
            const std::string& frameName = commonFrames[index];
            const std::optional<std::filesystem::path> diffPath = outputDir.has_value()
                ? std::optional<std::filesystem::path>(diffDir / frameName)
                : std::nullopt;
            SequenceFrameSample sample;
            sample.fileName = frameName;
            sample.frameIndex = frameIndexFromName(frameName, static_cast<uint32_t>(index));
            sample.metrics = compareImages(baselineViewDir / frameName, currentViewDir / frameName, diffPath);
            internal.perFrame.push_back(sample);
        }

        internal.summary.baselineVsCurrent = summarizeFrameSamples(internal.perFrame);
        internal.summary.baselineTemporal = summarizeTemporalPairs(baselineViewDir, baselineFrames);
        internal.summary.currentTemporal = summarizeTemporalPairs(currentViewDir, currentFrames);
        report.views.push_back(internal.summary);
        internalViews.push_back(std::move(internal));
    }

    if (!anyComparableFrames) {
        throw std::runtime_error("No comparable sequence frames were found");
    }

    if (outputDir.has_value()) {
        std::filesystem::create_directories(*outputDir);
        json out;
        out["baseline_dir"] = baselineDir.string();
        out["current_dir"] = currentDir.string();
        out["warnings"] = report.warnings;
        out["views"] = json::array();
        std::map<std::string, std::vector<double>> mseByView;
        std::map<std::string, std::vector<double>> temporalMseByView;
        std::map<std::string, SequenceTemporalMetrics> currentTemporalByView;

        for (const auto& internal : internalViews) {
            json viewJson;
            viewJson["view"] = internal.summary.view;
            viewJson["baseline_vs_current"] = sequenceMetricSummaryJson(internal.summary.baselineVsCurrent);
            viewJson["baseline_temporal"] = sequenceTemporalMetricsJson(internal.summary.baselineTemporal);
            viewJson["current_temporal"] = sequenceTemporalMetricsJson(internal.summary.currentTemporal);
            currentTemporalByView[internal.summary.view] = internal.summary.currentTemporal;
            viewJson["frames"] = json::array();
            for (const auto& sample : internal.perFrame) {
                viewJson["frames"].push_back({
                    {"file", sample.fileName},
                    {"frame", sample.frameIndex},
                    {"metrics", imageMetricsJson(sample.metrics)},
                });
                mseByView[internal.summary.view].push_back(sample.metrics.mse);
            }
            const auto currentViewDir = currentDir / internal.summary.view;
            temporalMseByView[internal.summary.view] = temporalPairMseValues(currentViewDir, pngNamesInDir(currentViewDir));
            out["views"].push_back(viewJson);

            const auto diffDir = *outputDir / "diffs" / internal.summary.view;
            writeContactSheet(diffDir, *outputDir / ("contact_sheet_" + internal.summary.view + ".png"), internal.perFrame, 8);
            if (internal.summary.view == "beauty") {
                auto worstIt = std::find_if(internal.perFrame.begin(), internal.perFrame.end(), [&](const auto& sample) {
                    return sample.frameIndex == internal.summary.baselineVsCurrent.worstFrame;
                });
                const auto worstPath = diffDir / (worstIt != internal.perFrame.end() ? worstIt->fileName : sequenceFrameFileName(internal.summary.baselineVsCurrent.worstFrame));
                if (std::filesystem::exists(worstPath)) {
                    std::filesystem::copy_file(worstPath, *outputDir / "worst_beauty_diff.png", std::filesystem::copy_options::overwrite_existing);
                }
            } else if (internal.summary.view == "reprojection-confidence") {
                auto worstIt = std::find_if(internal.perFrame.begin(), internal.perFrame.end(), [&](const auto& sample) {
                    return sample.frameIndex == internal.summary.baselineVsCurrent.worstFrame;
                });
                const auto worstPath = diffDir / (worstIt != internal.perFrame.end() ? worstIt->fileName : sequenceFrameFileName(internal.summary.baselineVsCurrent.worstFrame));
                if (std::filesystem::exists(worstPath)) {
                    std::filesystem::copy_file(worstPath, *outputDir / "worst_reprojection_confidence_diff.png", std::filesystem::copy_options::overwrite_existing);
                }
            }
        }

        json correlations = json::object();
        if (mseByView.contains("beauty")) {
            for (const char* diagnosticView : {"motion-vectors", "reprojection-confidence", "temporal-history-weight"}) {
                if (mseByView.contains(diagnosticView)) {
                    correlations[diagnosticView] = pearsonCorrelation(mseByView["beauty"], mseByView[diagnosticView]);
                }
            }
        }
        out["motion_debug_correlations"] = correlations;

        json temporalCorrelations = json::object();
        double strongestTemporalCorrelation = 0.0;
        double strongestDenoiserTemporalCorrelation = 0.0;
        if (temporalMseByView.contains("beauty")) {
            for (const char* diagnosticView : {
                     "motion-vectors",
                     "reprojection-confidence",
                     "temporal-history-weight",
                     "variance",
                     "restir-gi-validity",
                     "restir-gi-final"}) {
                if (temporalMseByView.contains(diagnosticView)) {
                    const double correlation = pearsonCorrelation(temporalMseByView["beauty"], temporalMseByView[diagnosticView]);
                    temporalCorrelations[diagnosticView] = correlation;
                    const double absCorrelation = std::abs(correlation);
                    strongestTemporalCorrelation = std::max(strongestTemporalCorrelation, absCorrelation);
                    const std::string diagnosticName = diagnosticView;
                    if (diagnosticName == "reprojection-confidence" ||
                        diagnosticName == "temporal-history-weight" ||
                        diagnosticName == "variance") {
                        strongestDenoiserTemporalCorrelation = std::max(strongestDenoiserTemporalCorrelation, absCorrelation);
                    }
                }
            }
        }
        out["temporal_debug_correlations"] = temporalCorrelations;

        json stabilitySummary = json::object();
        if (currentTemporalByView.contains("beauty")) {
            const auto& beauty = currentTemporalByView["beauty"];
            stabilitySummary["beauty_average_frame_delta_mse"] = beauty.averageFrameDeltaMse;
            stabilitySummary["beauty_max_frame_delta_mse"] = beauty.maxFrameDeltaMse;
            stabilitySummary["beauty_temporal_variance_score"] = beauty.temporalVarianceScore;
            stabilitySummary["beauty_worst_pair_start_frame"] = beauty.worstPairStartFrame;
            stabilitySummary["strongest_temporal_diagnostic_correlation"] = strongestTemporalCorrelation;
            stabilitySummary["uncorrelated_temporal_noise_score"] = beauty.temporalVarianceScore * (1.0 - std::clamp(strongestTemporalCorrelation, 0.0, 1.0));
            stabilitySummary["strongest_denoiser_temporal_diagnostic_correlation"] = strongestDenoiserTemporalCorrelation;
            stabilitySummary["denoiser_residual_noise_score"] = beauty.temporalVarianceScore * (1.0 - std::clamp(strongestDenoiserTemporalCorrelation, 0.0, 1.0));
        }
        if (currentTemporalByView.contains("variance")) {
            stabilitySummary["variance_temporal_variance_score"] = currentTemporalByView["variance"].temporalVarianceScore;
        }
        if (currentTemporalByView.contains("temporal-history-weight")) {
            stabilitySummary["history_weight_temporal_variance_score"] = currentTemporalByView["temporal-history-weight"].temporalVarianceScore;
        }
        if (currentTemporalByView.contains("reprojection-confidence")) {
            stabilitySummary["reprojection_temporal_variance_score"] = currentTemporalByView["reprojection-confidence"].temporalVarianceScore;
        }
        out["stability_summary"] = stabilitySummary;
        writeJsonFile(*outputDir / "motion_stability_report.json", out);
    }

    return report;
}

int compareImageSequenceCommand(
    const std::filesystem::path& baselineDir,
    const std::filesystem::path& currentDir,
    const std::optional<std::filesystem::path>& outputDir,
    const std::vector<std::string>& requestedViews) {
    const SequenceComparisonReport report = compareImageSequences(baselineDir, currentDir, outputDir, requestedViews);
    json result;
    result["status"] = "pass";
    result["warnings"] = report.warnings;
    result["views"] = json::array();
    for (const auto& view : report.views) {
        result["views"].push_back({
            {"view", view.view},
            {"baseline_vs_current", sequenceMetricSummaryJson(view.baselineVsCurrent)},
            {"baseline_temporal", sequenceTemporalMetricsJson(view.baselineTemporal)},
            {"current_temporal", sequenceTemporalMetricsJson(view.currentTemporal)},
        });
    }
    if (outputDir.has_value()) {
        result["report"] = (*outputDir / "motion_stability_report.json").string();
    }
    std::cout << result.dump(2) << "\n";
    return 0;
}

BaselinePaths baselinePathsFor(const std::filesystem::path& scenePath, const std::filesystem::path& baselineRoot) {
    const std::string caseName = scenePath.empty() ? "default" : scenePath.stem().string();
    BaselinePaths paths;
    paths.root = baselineRoot;
    paths.caseDir = baselineRoot / caseName;
    paths.profile = paths.caseDir / "profile.json";
    paths.renderGraph = paths.caseDir / "rendergraph.json";
    paths.beautyImage = paths.caseDir / "beauty.png";
    paths.frameSequence = paths.caseDir / "frame_sequence";
    return paths;
}

void updateBaseline(
    const BaselinePaths& paths,
    const std::filesystem::path& profilePath,
    const std::filesystem::path& renderGraphPath,
    const std::filesystem::path& debugViewsDir,
    const std::optional<std::filesystem::path>& frameSequenceDir) {
    std::filesystem::create_directories(paths.caseDir);
    std::filesystem::copy_file(profilePath, paths.profile, std::filesystem::copy_options::overwrite_existing);
    std::filesystem::copy_file(renderGraphPath, paths.renderGraph, std::filesystem::copy_options::overwrite_existing);
    std::filesystem::copy_file(beautyPath(debugViewsDir), paths.beautyImage, std::filesystem::copy_options::overwrite_existing);
    const std::filesystem::path sequenceDir = frameSequenceDir.value_or(debugViewsDir.parent_path() / "frame_sequence");
    if (std::filesystem::exists(sequenceDir)) {
        if (std::filesystem::exists(paths.frameSequence)) {
            std::filesystem::remove_all(paths.frameSequence);
        }
        std::filesystem::copy(sequenceDir, paths.frameSequence,
            std::filesystem::copy_options::recursive | std::filesystem::copy_options::overwrite_existing);
    }
    json manifest = {
        {"profile", paths.profile.filename().string()},
        {"rendergraph", paths.renderGraph.filename().string()},
        {"beauty", paths.beautyImage.filename().string()},
        {"frame_sequence", std::filesystem::exists(paths.frameSequence) ? paths.frameSequence.filename().string() : ""},
    };
    writeJsonFile(paths.caseDir / "manifest.json", manifest);
    std::cout << "Updated baseline: " << paths.caseDir.string() << "\n";
}

int checkBaseline(
    const BaselinePaths& paths,
    const std::filesystem::path& profilePath,
    const std::filesystem::path& renderGraphPath,
    const std::filesystem::path& debugViewsDir) {
    if (!std::filesystem::exists(paths.profile) ||
        !std::filesystem::exists(paths.renderGraph) ||
        !std::filesystem::exists(paths.beautyImage)) {
        std::cerr << "Baseline is missing for " << paths.caseDir.string() << "\n";
        return 2;
    }

    const json profileComparison = compareProfilesJson(readJsonFile(paths.profile), readJsonFile(profilePath));
    const ImageDiffMetrics imageMetrics = compareImages(paths.beautyImage, beautyPath(debugViewsDir), std::nullopt);
    const json baselineGraph = readJsonFile(paths.renderGraph);
    const json currentGraph = readJsonFile(renderGraphPath);
    const std::vector<std::string> baselinePasses = passNamesFromRenderGraph(baselineGraph);
    const std::vector<std::string> currentPasses = passNamesFromRenderGraph(currentGraph);
    const bool renderGraphChanged = baselinePasses != currentPasses;

    const double gpuRegression = profileComparison["summary"].value("gpu_avg_regression_percent", 0.0);
    const double memoryRegression = profileComparison["summary"].value("memory_regression_percent", 0.0);
    const bool imageFailed = imageMetrics.changedPixelPercentage > 0.1 && imageMetrics.psnr < 50.0;
    const bool profileFailed = gpuRegression > 10.0 || memoryRegression > 10.0;
    bool sequenceFailed = false;
    json sequenceJson = nullptr;
    const std::filesystem::path currentSequence = debugViewsDir.parent_path() / "frame_sequence";
    if (std::filesystem::exists(paths.frameSequence)) {
        if (!std::filesystem::exists(currentSequence)) {
            sequenceFailed = true;
            sequenceJson = {{"status", "fail"}, {"reason", "current frame sequence is missing"}};
        } else {
            try {
                const SequenceComparisonReport sequenceReport = compareImageSequences(paths.frameSequence, currentSequence, std::nullopt, {});
                sequenceJson = {{"status", "pass"}, {"warnings", sequenceReport.warnings}, {"views", json::array()}};
                for (const auto& view : sequenceReport.views) {
                    sequenceJson["views"].push_back({
                        {"view", view.view},
                        {"baseline_vs_current", sequenceMetricSummaryJson(view.baselineVsCurrent)},
                        {"baseline_temporal", sequenceTemporalMetricsJson(view.baselineTemporal)},
                        {"current_temporal", sequenceTemporalMetricsJson(view.currentTemporal)},
                    });
                }
            } catch (const std::exception& error) {
                sequenceFailed = true;
                sequenceJson = {{"status", "fail"}, {"reason", error.what()}};
            }
        }
    }
    const bool failed = imageFailed || profileFailed || renderGraphChanged || sequenceFailed;

    json result = {
        {"status", failed ? "fail" : "pass"},
        {"profile", profileComparison},
        {"image", imageMetricsJson(imageMetrics)},
        {"rendergraph", {
            {"changed", renderGraphChanged},
            {"baseline_passes", baselinePasses},
            {"current_passes", currentPasses},
        }},
        {"frame_sequence", sequenceJson},
    };
    std::cout << result.dump(2) << "\n";
    return failed ? 1 : 0;
}

void writeMemoryReport(
    const std::filesystem::path& outputPath,
    const ProfileReport& profile,
    const std::optional<std::filesystem::path>& renderGraphPath) {
    const RenderGraphTransientStats transientStats = transientStatsFromRenderGraph(renderGraphPath);
    const uint64_t persistent =
        profile.memory.texturesBytes +
        profile.memory.buffersBytes +
        profile.memory.accelerationStructureBytes +
        profile.memory.temporalHistoryBytes +
        profile.memory.restirReservoirBytes +
        profile.memory.stagingUploadPeakBytes;
    const uint64_t totalBytes = persistent + transientStats.estimatedBytes;
    json heaps = json::array();
    for (const auto& heap : profile.memory.vmaBudget.heaps) {
        heaps.push_back({
            {"heap_index", heap.heapIndex},
            {"usage_bytes", heap.usageBytes},
            {"budget_bytes", heap.budgetBytes},
            {"allocation_bytes", heap.allocationBytes},
            {"block_bytes", heap.blockBytes},
            {"allocation_count", heap.allocationCount},
            {"block_count", heap.blockCount},
            {"usage_ratio", heap.usageRatio},
            {"pressure", heap.pressure},
        });
    }
    json vmaBudget = {
        {"supported", profile.memory.vmaBudget.supported},
        {"total_usage_bytes", profile.memory.vmaBudget.totalUsageBytes},
        {"total_budget_bytes", profile.memory.vmaBudget.totalBudgetBytes},
        {"total_allocation_bytes", profile.memory.vmaBudget.totalAllocationBytes},
        {"total_block_bytes", profile.memory.vmaBudget.totalBlockBytes},
        {"peak_usage_bytes", profile.memory.vmaBudget.peakUsageBytes},
        {"usage_delta_bytes", profile.memory.vmaBudget.usageDeltaBytes},
        {"allocation_count", profile.memory.vmaBudget.allocationCount},
        {"block_count", profile.memory.vmaBudget.blockCount},
        {"max_usage_ratio", profile.memory.vmaBudget.maxUsageRatio},
        {"pressure", profile.memory.vmaBudget.pressure},
        {"override_active", profile.memory.vmaBudget.overrideActive},
        {"heaps", heaps},
        {"warnings", profile.memory.vmaBudget.warnings},
    };
    json descriptorPools = {
        {"sets_per_pool", profile.memory.descriptors.setsPerPool},
        {"max_pools", profile.memory.descriptors.maxPools},
        {"used_pools", profile.memory.descriptors.usedPools},
        {"free_pools", profile.memory.descriptors.freePools},
        {"pool_count", profile.memory.descriptors.poolCount},
        {"capacity_sets", profile.memory.descriptors.capacitySets},
        {"allocated_sets", profile.memory.descriptors.allocatedSets},
        {"peak_allocated_sets", profile.memory.descriptors.peakAllocatedSets},
        {"failed_allocations", profile.memory.descriptors.failedAllocations},
        {"fragmented_pool_failures", profile.memory.descriptors.fragmentedPoolFailures},
        {"pool_growth_count", profile.memory.descriptors.poolGrowthCount},
    };
    json ui = {
        {"present", profile.memory.ui.present},
        {"descriptor_max_sets", profile.memory.ui.descriptorMaxSets},
        {"combined_image_sampler_descriptors", profile.memory.ui.combinedImageSamplerDescriptors},
        {"sampled_image_descriptors", profile.memory.ui.sampledImageDescriptors},
        {"sampler_descriptors", profile.memory.ui.samplerDescriptors},
        {"viewport_descriptor_allocated", profile.memory.ui.viewportDescriptorAllocated},
    };
    json j = {
        {"textures_bytes", profile.memory.texturesBytes},
        {"buffers_bytes", profile.memory.buffersBytes},
        {"acceleration_structure_bytes", profile.memory.accelerationStructureBytes},
        {"temporal_history_bytes", profile.memory.temporalHistoryBytes},
        {"restir_reservoir_bytes", profile.memory.restirReservoirBytes},
        {"restir_di_current_bytes", profile.memory.restirDiCurrentBytes},
        {"restir_di_initial_bytes", profile.memory.restirDiInitialBytes},
        {"restir_di_temporal_bytes", profile.memory.restirDiTemporalBytes},
        {"restir_di_previous_bytes", profile.memory.restirDiPreviousBytes},
        {"restir_di_spatial_bytes", profile.memory.restirDiSpatialBytes},
        {"restir_di_final_bytes", profile.memory.restirDiFinalBytes},
        {"restir_di_receiver_bytes", profile.memory.restirDiReceiverBytes},
        {"restir_di_previous_receiver_bytes", profile.memory.restirDiPreviousReceiverBytes},
        {"restir_di_counters_bytes", profile.memory.restirDiCountersBytes},
        {"staging_upload_total_bytes", profile.memory.stagingUploadTotalBytes},
        {"staging_upload_peak_bytes", profile.memory.stagingUploadPeakBytes},
        {"staging_upload_last_bytes", profile.memory.stagingUploadLastBytes},
        {"staging_upload_count", profile.memory.stagingUploadCount},
        {"staging_buffer_upload_count", profile.memory.stagingBufferUploadCount},
        {"staging_image_upload_count", profile.memory.stagingImageUploadCount},
        {"staging_batch_upload_count", profile.memory.stagingBatchUploadCount},
        {"transient_resources_bytes", transientStats.estimatedBytes},
        {"transient_alias_eligible_bytes", transientStats.aliasEligibleBytes},
        {"rendergraph_active_alias_saved_bytes", transientStats.activeAliasSavedBytes},
        {"transient_resource_count", transientStats.transientResourceCount},
        {"transient_alias_eligible_resource_count", transientStats.aliasEligibleResourceCount},
        {"rendergraph_active_alias_count", transientStats.activeAliasCount},
        {"persistent_resources_bytes", persistent},
        {"total_bytes", totalBytes},
        {"vma_budget", vmaBudget},
        {"descriptor_pools", descriptorPools},
        {"ui", ui},
        {"notes", json::array({"transient_resources_bytes is derived from rendergraph.json lifetime estimates when --dump-rendergraph is provided"})},
    };
    writeJsonFile(outputPath, j);
}

void writeFrameTimeline(
    const std::filesystem::path& outputPath,
    const ProfileReport& profile,
    const std::optional<std::filesystem::path>& renderGraphPath) {
    json graph = renderGraphOrEmpty(renderGraphPath);
    json gpuPasses = json::array();
    for (const auto& pass : graph.value("passes", json::array())) {
        gpuPasses.push_back({
            {"name", pass.value("name", "")},
            {"queue", pass.value("queue", "unknown")},
            {"gpu_ms", pass.value("gpu_ms", 0.0)},
            {"barrier_count", pass.value("barriers", json::array()).size()},
        });
    }
    json asyncCompute = {
        {"enabled", profile.asyncCompute.enabled},
        {"disabled_by_cli", profile.asyncCompute.disabledByCli},
        {"single_queue_fallback", profile.asyncCompute.singleQueueFallback},
        {"timeline_semaphore", profile.asyncCompute.timelineSemaphore},
        {"independent_queue", profile.asyncCompute.independentQueue},
        {"dedicated_compute_family", profile.asyncCompute.dedicatedComputeFamily},
        {"cross_family", profile.asyncCompute.crossFamily},
        {"graphics_family", profile.asyncCompute.graphicsFamily.has_value()
            ? json(*profile.asyncCompute.graphicsFamily)
            : json(nullptr)},
        {"compute_family", profile.asyncCompute.computeFamily.has_value()
            ? json(*profile.asyncCompute.computeFamily)
            : json(nullptr)},
        {"compute_queue_index", profile.asyncCompute.computeQueueIndex},
        {"resource_sharing_mode", profile.asyncCompute.resourceSharingMode},
        {"resource_sharing_queue_family_count", profile.asyncCompute.resourceSharingQueueFamilyCount},
        {"resource_sharing_queue_families", profile.asyncCompute.resourceSharingQueueFamilies},
    };
    json queueSubmits = json::array();
    if (profile.asyncCompute.enabled) {
        queueSubmits.push_back({{"queue", "graphics"}, {"count", 2}, {"role", "producer_and_post"}});
        queueSubmits.push_back({{"queue", "compute"}, {"count", 1}, {"role", "post_trace_compute"}});
    } else {
        queueSubmits.push_back({{"queue", "graphics"}, {"count", 1}, {"role", "single_queue"}});
    }
    json j = {
        {"cpu_events", {
            {{"name", "frame"}, {"min_ms", profile.cpuFrameMs.min}, {"avg_ms", profile.cpuFrameMs.avg}, {"max_ms", profile.cpuFrameMs.max}},
        }},
        {"gpu_passes", gpuPasses},
        {"queue_submits", queueSubmits},
        {"queue_lane_ms", {
            {"graphics", profile.queueLaneMs.graphics},
            {"ray_tracing", profile.queueLaneMs.rayTracing},
            {"compute", profile.queueLaneMs.compute},
            {"queue_wait", profile.queueLaneMs.queueWait},
        }},
        {"async_compute", asyncCompute},
        {"semaphores", json::array()},
        {"barriers", graph.value("barriers", json::array())},
        {"presentation", {{"headless", true}}},
    };
    writeJsonFile(outputPath, j);
}

void writeResourceLifetimes(
    const std::filesystem::path& outputPath,
    const std::optional<std::filesystem::path>& renderGraphPath) {
    json graph = renderGraphOrEmpty(renderGraphPath);
    std::unordered_map<std::string, json> resources;
    for (const auto& resource : graph.value("resources", json::array())) {
        const std::string name = resource.value("name", "unnamed");
        resources[name] = {
            {"name", name},
            {"type", resource.value("type", "unknown")},
            {"lifetime", resource.value("lifetime", "unknown")},
            {"index", resource.contains("index") ? resource["index"] : json(nullptr)},
            {"lifetime_interval", resource.value("lifetime_interval", json::object())},
            {"aliasing", resource.value("aliasing", json::object())},
            {"created_by", nullptr},
            {"destroyed_after", nullptr},
            {"reads", json::array()},
            {"writes", json::array()},
            {"aliases", json::array()},
            {"alias_rejections", json::array()},
        };
    }
    for (const auto& pass : graph.value("passes", json::array())) {
        const std::string passName = pass.value("name", "");
        for (const auto& input : pass.value("inputs", json::array())) {
            resources[input.get<std::string>()]["reads"].push_back(passName);
        }
        for (const auto& output : pass.value("outputs", json::array())) {
            auto& entry = resources[output.get<std::string>()];
            if (entry["created_by"].is_null()) {
                entry["created_by"] = passName;
            }
            entry["writes"].push_back(passName);
            entry["destroyed_after"] = passName;
        }
    }
    for (const auto& check : graph.value("alias_checks", json::array())) {
        const std::string a = check.value("resource_a", "");
        const std::string b = check.value("resource_b", "");
        if (a.empty() || b.empty()) {
            continue;
        }
        const bool candidate = check.value("schedule_candidate", false);
        json compact = {
            {"resource", b},
            {"lifetimes_overlap", check.value("lifetimes_overlap", true)},
            {"physical_alias_candidate", check.value("physical_alias_candidate", false)},
            {"estimated_saved_bytes", check.value("estimated_saved_bytes", 0)},
            {"reason", check.value("reason", "")},
        };
        json reverse = compact;
        reverse["resource"] = a;
        if (candidate) {
            resources[a]["aliases"].push_back(compact);
            resources[b]["aliases"].push_back(reverse);
        } else {
            resources[a]["alias_rejections"].push_back(compact);
            resources[b]["alias_rejections"].push_back(reverse);
        }
    }
    json out = json::array();
    for (auto& [name, resource] : resources) {
        (void)name;
        out.push_back(resource);
    }
    writeJsonFile(outputPath, {
        {"resources", out},
        {"alias_checks", graph.value("alias_checks", json::array())},
        {"resource_aliasing", graph.value("resource_aliasing", json::object())},
    });
}

void writeShaderReport(
    const std::filesystem::path& outputPath,
    const std::filesystem::path& shaderSourceDir,
    const std::filesystem::path& shaderOutputDir) {
    json shaders = json::array();
    json reflectionFailures = json::array();
    uint32_t shaderCount = 0;
    uint32_t spirvPresentCount = 0;
    uint32_t reflectedShaderCount = 0;
    uint32_t totalDescriptorBindingCount = 0;
    uint32_t totalPushConstantRangeCount = 0;
    std::vector<ReflectedBindingRecord> reflectedBindingRecords;
    if (std::filesystem::exists(shaderSourceDir)) {
        for (const auto& entry : std::filesystem::directory_iterator(shaderSourceDir)) {
            if (!entry.is_regular_file()) {
                continue;
            }
            const std::filesystem::path source = entry.path();
            const std::string ext = source.extension().string();
            if (ext != ".rgen" && ext != ".rchit" && ext != ".rahit" && ext != ".rmiss" && ext != ".comp" && ext != ".vert" && ext != ".frag") {
                continue;
            }
            ++shaderCount;
            const std::filesystem::path spirv = shaderOutputDir / (source.filename().string() + ".spv");
            json shader = {
                {"source", source.string()},
                {"spirv", spirv.string()},
                {"entry_point", "main"},
                {"pipeline_name", source.stem().string()},
                {"defines", {
                    {"RTV_USE_DIMENSIONED_SAMPLER", "env/default"},
                    {"RTV_DENOISER_SHARED_TILE", "env/default"},
                }},
                {"compile_time_ms", nullptr},
                {"hash", nullptr},
                {"spirv_size_bytes", 0},
                {"reflection", {
                    {"available", false},
                    {"descriptor_binding_count", 0},
                    {"push_constant_range_count", 0},
                    {"bindings", json::array()},
                    {"descriptor_sets", json::object()},
                    {"push_constants", json::array()},
                    {"error", nullptr},
                }},
            };
            if (std::filesystem::exists(spirv)) {
                const std::vector<unsigned char> bytes = readBinary(spirv);
                shader["hash"] = hexHashBytes(bytes);
                shader["spirv_size_bytes"] = bytes.size();
                ++spirvPresentCount;
                try {
                    const ShaderReflectionData reflection = ShaderReflection::reflect(bytesToWords(bytes));
                    json bindingRows = json::array();
                    json descriptorSets = json::object();
                    for (const ReflectedBinding& binding : reflection.bindings) {
                        reflectedBindingRecords.push_back({binding, source.string()});
                        json bindingRow = reflectedBindingJson(binding);
                        bindingRows.push_back(bindingRow);
                        descriptorSets[std::to_string(binding.set)].push_back(std::move(bindingRow));
                    }
                    json pushRows = json::array();
                    for (const ReflectedPushConstant& pushConstant : reflection.pushConstants) {
                        pushRows.push_back(reflectedPushConstantJson(pushConstant));
                    }
                    shader["reflection"] = {
                        {"available", true},
                        {"stage", shaderStageFlagsName(static_cast<VkShaderStageFlags>(reflection.stage))},
                        {"stage_flags", static_cast<uint32_t>(reflection.stage)},
                        {"descriptor_binding_count", reflection.bindings.size()},
                        {"push_constant_range_count", reflection.pushConstants.size()},
                        {"bindings", std::move(bindingRows)},
                        {"descriptor_sets", std::move(descriptorSets)},
                        {"push_constants", std::move(pushRows)},
                        {"error", nullptr},
                    };
                    ++reflectedShaderCount;
                    totalDescriptorBindingCount += static_cast<uint32_t>(reflection.bindings.size());
                    totalPushConstantRangeCount += static_cast<uint32_t>(reflection.pushConstants.size());
                } catch (const std::exception& e) {
                    shader["reflection"]["error"] = e.what();
                    reflectionFailures.push_back({
                        {"source", source.string()},
                        {"spirv", spirv.string()},
                        {"error", e.what()},
                    });
                }
            }
            shaders.push_back(shader);
        }
    }
    const json reflectionValidation = {
        {"schema_version", 1},
        {"shader_count", shaderCount},
        {"spirv_present_count", spirvPresentCount},
        {"reflected_shader_count", reflectedShaderCount},
        {"descriptor_binding_count", totalDescriptorBindingCount},
        {"push_constant_range_count", totalPushConstantRangeCount},
        {"failure_count", reflectionFailures.size()},
        {"failures", reflectionFailures},
        {"passed", shaderCount > 0 && spirvPresentCount > 0 && reflectedShaderCount == spirvPresentCount && reflectionFailures.empty()},
    };
    const json descriptorAudit = descriptorReflectionAuditJson(
        reflectedBindingRecords,
        reflectionValidation.value("passed", false));
    writeJsonFile(outputPath, {
        {"shaders", shaders},
        {"reflection_validation", reflectionValidation},
        {"descriptor_audit", descriptorAudit},
    });
}

void writeBindingsReport(
    const std::filesystem::path& outputPath,
    const std::optional<std::filesystem::path>& renderGraphPath) {
    json graph = renderGraphOrEmpty(renderGraphPath);
    json passes = json::array();
    for (const auto& pass : graph.value("passes", json::array())) {
        passes.push_back({
            {"pass", pass.value("name", "")},
            {"queue", pass.value("queue", "unknown")},
            {"inputs", pass.value("inputs", json::array())},
            {"outputs", pass.value("outputs", json::array())},
            {"resource_formats", pass.value("resource_formats", json::object())},
            {"resource_extents", pass.value("extents", json::object())},
        });
    }
    json knownSets = {
        {"raytracing_set_0", {
            {"binding_38", "restir reservoir storage buffer"},
            {"binding_39", "previous restir reservoir storage buffer"},
            {"binding_40", "light BVH nodes storage buffer"},
            {"binding_42", "path data storage buffer"},
            {"binding_43", "ReSTIR GI current reservoir storage buffer"},
            {"binding_44", "ReSTIR GI previous reservoir storage buffer"},
            {"binding_45", "ReSTIR GI spatial reservoir storage buffer"},
        }},
        {"bindless_texture_heap_set_2", {
            {"binding_0", "full bindless combined image sampler heap"},
        }},
    };
    const DescriptorWriteDiagnosticsSnapshot descriptorWrites = descriptorWriteDiagnosticsSnapshot();
    writeJsonFile(outputPath, {
        {"passes", passes},
        {"known_descriptor_sets", knownSets},
        {"known_descriptor_binding_expectations", descriptorExpectationsJson()},
        {"descriptor_write_diagnostics", descriptorWriteDiagnosticsJson(descriptorWrites)},
        {"actual_descriptor_write_audit", actualDescriptorWriteAuditJson(descriptorWrites)},
    });
}

void writeCrashDumpPackage(
    const std::filesystem::path& outputDir,
    const std::filesystem::path& scenePath,
    const std::optional<std::filesystem::path>& profilePath,
    const std::optional<std::filesystem::path>& renderGraphPath,
    const std::optional<std::filesystem::path>& debugViewsDir,
    const std::string& capturedLog) {
    std::filesystem::create_directories(outputDir);
    if (!scenePath.empty() && std::filesystem::exists(scenePath)) {
        std::filesystem::copy_file(scenePath, outputDir / "scene_copy.rtlevel", std::filesystem::copy_options::overwrite_existing);
    }
    if (profilePath.has_value() && std::filesystem::exists(*profilePath)) {
        std::filesystem::copy_file(*profilePath, outputDir / "last_profile.json", std::filesystem::copy_options::overwrite_existing);
    }
    if (renderGraphPath.has_value() && std::filesystem::exists(*renderGraphPath)) {
        std::filesystem::copy_file(*renderGraphPath, outputDir / "rendergraph.json", std::filesystem::copy_options::overwrite_existing);
    }
    if (debugViewsDir.has_value() && std::filesystem::exists(*debugViewsDir)) {
        std::filesystem::copy(*debugViewsDir, outputDir / "debug_views",
            std::filesystem::copy_options::recursive | std::filesystem::copy_options::overwrite_existing);
        const std::filesystem::path beauty = beautyPath(*debugViewsDir);
        if (std::filesystem::exists(beauty)) {
            std::filesystem::copy_file(beauty, outputDir / "last_frame.png", std::filesystem::copy_options::overwrite_existing);
        }
    }
    if (!capturedLog.empty()) {
        std::ofstream logFile(outputDir / "log.txt");
        if (logFile.is_open()) {
            logFile << capturedLog;
        }
    }
}

int validateGpuLabels(const std::optional<std::filesystem::path>& renderGraphPath) {
    json graph = renderGraphOrEmpty(renderGraphPath);
    std::vector<std::string> problems;
    for (const auto& pass : graph.value("passes", json::array())) {
        if (pass.value("name", "").empty()) {
            problems.push_back("Unnamed render graph pass");
        }
    }
    for (const auto& resource : graph.value("resources", json::array())) {
        const std::string name = resource.value("name", "");
        if (name.empty() || name == "unnamed") {
            problems.push_back("Unnamed render graph resource");
        }
    }
    json result = {{"status", problems.empty() ? "pass" : "fail"}, {"problems", problems}};
    std::cout << result.dump(2) << "\n";
    return problems.empty() ? 0 : 1;
}

int checkBudget(const std::filesystem::path& budgetPath, const ProfileReport& profile) {
    const json budget = readJsonFile(budgetPath);
    json failures = json::array();

    auto checkPassBudgets = [&failures](
                                const json& passBudgets,
                                const std::map<std::string, double>& actual,
                                const std::string& group) {
        for (auto it = passBudgets.begin(); it != passBudgets.end(); ++it) {
            const std::string pass = it.key();
            const double maxMs = it.value().get<double>();
            const double actualMs = actual.contains(pass) ? actual.at(pass) : 0.0;
            if (actualMs > maxMs) {
                failures.push_back({
                    {"metric", group + "." + pass},
                    {"pass", pass},
                    {"actual_ms", actualMs},
                    {"budget_ms", maxMs},
                });
            }
        }
    };

    checkPassBudgets(
        budget.value("per_pass_gpu_ms", json::object()),
        passGpuMsMap(profile.perPassGpuMs),
        "per_pass_gpu_ms");
    checkPassBudgets(
        budget.value("per_pass_gpu_ms_p95", json::object()),
        passGpuMsMap(profile.perPassGpuMsP95),
        "per_pass_gpu_ms_p95");
    checkPassBudgets(
        budget.value("per_pass_gpu_ms_p99", json::object()),
        passGpuMsMap(profile.perPassGpuMsP99),
        "per_pass_gpu_ms_p99");
    if (budget.contains("gpu_frame_ms")) {
        const double maxFrame = budget["gpu_frame_ms"].get<double>();
        if (profile.gpuFrameMs.avg > maxFrame) {
            failures.push_back({{"metric", "gpu_frame_ms.avg"}, {"actual_ms", profile.gpuFrameMs.avg}, {"budget_ms", maxFrame}});
        }
    }
    if (budget.contains("gpu_frame_ms_avg")) {
        const double maxFrame = budget["gpu_frame_ms_avg"].get<double>();
        if (profile.gpuFrameMs.avg > maxFrame) {
            failures.push_back({{"metric", "gpu_frame_ms.avg"}, {"actual_ms", profile.gpuFrameMs.avg}, {"budget_ms", maxFrame}});
        }
    }
    if (budget.contains("gpu_frame_ms_max")) {
        const double maxFrame = budget["gpu_frame_ms_max"].get<double>();
        if (profile.gpuFrameMs.max > maxFrame) {
            failures.push_back({{"metric", "gpu_frame_ms.max"}, {"actual_ms", profile.gpuFrameMs.max}, {"budget_ms", maxFrame}});
        }
    }
    if (budget.contains("gpu_frame_ms_p95")) {
        const double maxFrame = budget["gpu_frame_ms_p95"].get<double>();
        if (profile.gpuFrameMs.p95 > maxFrame) {
            failures.push_back({{"metric", "gpu_frame_ms.p95"}, {"actual_ms", profile.gpuFrameMs.p95}, {"budget_ms", maxFrame}});
        }
    }
    if (budget.contains("gpu_frame_ms_p99")) {
        const double maxFrame = budget["gpu_frame_ms_p99"].get<double>();
        if (profile.gpuFrameMs.p99 > maxFrame) {
            failures.push_back({{"metric", "gpu_frame_ms.p99"}, {"actual_ms", profile.gpuFrameMs.p99}, {"budget_ms", maxFrame}});
        }
    }
    if (budget.contains("validation_error_count")) {
        const uint64_t maxValidationErrors = bytesFromBudgetValue(budget["validation_error_count"]);
        if (!profile.validationEnabled) {
            failures.push_back({
                {"metric", "validation_enabled"},
                {"actual", false},
                {"budget", true},
            });
        } else if (profile.validationErrorCount > maxValidationErrors) {
            failures.push_back({
                {"metric", "validation_error_count"},
                {"actual", profile.validationErrorCount},
                {"budget", maxValidationErrors},
            });
        }
    }
    auto jsonUint = [](const json& object, const char* key) -> uint64_t {
        if (!object.is_object() || !object.contains(key)) {
            return 0ull;
        }
        const json& value = object.at(key);
        if (value.is_number_unsigned()) {
            return value.get<uint64_t>();
        }
        if (value.is_number_integer()) {
            return static_cast<uint64_t>(std::max<int64_t>(0, value.get<int64_t>()));
        }
        return 0ull;
    };
    auto jsonNestedUint = [&jsonUint](const json& object, const char* objectKey, const char* valueKey) -> uint64_t {
        if (!object.is_object() || !object.contains(objectKey) || !object.at(objectKey).is_object()) {
            return 0ull;
        }
        return jsonUint(object.at(objectKey), valueKey);
    };
    auto jsonBool = [](const json& object, const char* key) -> bool {
        if (!object.is_object() || !object.contains(key)) {
            return false;
        }
        const json& value = object.at(key);
        return value.is_boolean() ? value.get<bool>() : false;
    };
    auto requireBool = [&failures, &budget](const char* key, bool actual, const char* metric) {
        if (budget.value(key, false) && !actual) {
            failures.push_back({
                {"metric", metric},
                {"actual", actual},
                {"budget", true},
            });
        }
    };
    requireBool(
        "require_gpu_markers",
        profile.nvidiaIntegrations.vulkanDebugLabelsAvailable,
        "nvidia_integrations.vulkan_debug_labels_available");
    requireBool(
        "require_gpu_object_names",
        profile.nvidiaIntegrations.vulkanDebugObjectNamesAvailable,
        "nvidia_integrations.vulkan_debug_object_names_available");
    requireBool(
        "require_nsight_perf_sdk",
        profile.nvidiaIntegrations.nsightPerfSdk.reportGeneratorInitialized,
        "nvidia_integrations.nsight_perf_sdk.report_generator_initialized");
    requireBool(
        "require_nsight_perf_command_buffer_ranges",
        profile.nvidiaIntegrations.nsightPerfSdk.commandBufferRanges.commandBufferRangesAvailable,
        "nvidia_integrations.nsight_perf_sdk.command_buffer_ranges.available");
    requireBool(
        "require_nsight_perf_range_emission",
        profile.nvidiaIntegrations.nsightPerfSdk.commandBufferRanges.pushedRanges > 0ull &&
            profile.nvidiaIntegrations.nsightPerfSdk.commandBufferRanges.failedPushes == 0ull,
        "nvidia_integrations.nsight_perf_sdk.command_buffer_ranges.pushed_ranges");
    requireBool(
        "require_nsight_perf_report",
        profile.nvidiaIntegrations.nsightPerfSdk.commandBufferRanges.captureSucceeded,
        "nvidia_integrations.nsight_perf_sdk.command_buffer_ranges.capture_succeeded");
    requireBool(
        "require_gpu_crash_dump_build",
        profile.nvidiaIntegrations.gpuCrashDumps.buildAvailable,
        "nvidia_integrations.gpu_crash_dumps.build_available");
    requireBool(
        "require_gpu_crash_dumps_enabled",
        profile.nvidiaIntegrations.gpuCrashDumps.requested && profile.nvidiaIntegrations.gpuCrashDumps.enabled,
        "nvidia_integrations.gpu_crash_dumps.enabled");

    auto counterAt = [](const std::vector<uint64_t>& counters, size_t index) -> uint64_t {
        return index < counters.size() ? counters[index] : 0ull;
    };
    auto addReservoirContractFailure = [&failures](
                                           const char* metric,
                                           bool active,
                                           bool checked,
                                           uint64_t sourcePdfViolations,
                                           uint64_t targetPdfViolations,
                                           uint64_t sourcePdfParityMismatches,
                                           uint64_t targetPdfParityMismatches,
                                           uint64_t nonFiniteViolations,
                                           uint64_t budgetLimit) {
        const uint64_t violations =
            sourcePdfViolations +
            targetPdfViolations +
            sourcePdfParityMismatches +
            targetPdfParityMismatches +
            nonFiniteViolations;
        if ((active && !checked) || violations > budgetLimit) {
            failures.push_back({
                {"metric", metric},
                {"active", active},
                {"checked", checked},
                {"invalid_source_pdf_count", sourcePdfViolations},
                {"invalid_target_pdf_count", targetPdfViolations},
                {"source_pdf_parity_mismatch_count", sourcePdfParityMismatches},
                {"target_pdf_parity_mismatch_count", targetPdfParityMismatches},
                {"non_finite_count", nonFiniteViolations},
                {"actual", violations},
                {"budget", budgetLimit},
            });
        }
    };
    const bool requireAnyReservoirContract =
        budget.value("require_restir_reservoir_contract_validation", false);
    const bool diContractActive =
        profile.settings.restirDiMode == RestirDiMode::Production ||
        profile.settings.restirDiMode == RestirDiMode::ReferenceValidation ||
        profile.settings.restirDiMode == RestirDiMode::HybridCompare;
    const bool diContractChecked = diContractActive && !profile.restirDiCounters.empty();
    const uint64_t diContractInvalidSourcePdf = 0ull;
    const uint64_t diContractInvalidTargetPdf = counterAt(profile.restirDiCounters, 63);
    const uint64_t diSourcePdfParityMismatch = 0ull;
    const uint64_t diTargetPdfParityMismatch = 0ull;
    const uint64_t diContractNonFinite = 0ull;
    const bool giContractActive =
        !profile.settings.wavefrontFinalOutputEnabled &&
        (profile.settings.restirGiMode == RestirGiMode::Production ||
         profile.settings.restirGiMode == RestirGiMode::ReferenceValidation);
    const bool giContractChecked = giContractActive && !profile.restirGiCounters.empty();
    const uint64_t giContractInvalidSourcePdf = counterAt(profile.restirGiCounters, 48);
    const uint64_t giContractInvalidTargetPdf = counterAt(profile.restirGiCounters, 49);
    const uint64_t giSourcePdfParityMismatch = counterAt(profile.restirGiCounters, 50);
    const uint64_t giTargetPdfParityMismatch = counterAt(profile.restirGiCounters, 51);
    const uint64_t giContractNonFinite = counterAt(profile.restirGiCounters, 45);
    if (requireAnyReservoirContract || budget.value("require_restir_di_reservoir_contract_validation", false)) {
        addReservoirContractFailure(
            "restir_di.reservoir_contract_validation",
            diContractActive,
            diContractChecked,
            diContractInvalidSourcePdf,
            diContractInvalidTargetPdf,
            diSourcePdfParityMismatch,
            diTargetPdfParityMismatch,
            diContractNonFinite,
            0ull);
    }
    if (requireAnyReservoirContract || budget.value("require_restir_gi_reservoir_contract_validation", false)) {
        addReservoirContractFailure(
            "restir_gi.reservoir_contract_validation",
            giContractActive,
            giContractChecked,
            giContractInvalidSourcePdf,
            giContractInvalidTargetPdf,
            giSourcePdfParityMismatch,
            giTargetPdfParityMismatch,
            giContractNonFinite,
            0ull);
    }
    if (budget.contains("max_restir_di_reservoir_contract_violation_count")) {
        addReservoirContractFailure(
            "restir_di.reservoir_contract_validation.violation_count",
            diContractActive,
            diContractChecked,
            diContractInvalidSourcePdf,
            diContractInvalidTargetPdf,
            diSourcePdfParityMismatch,
            diTargetPdfParityMismatch,
            diContractNonFinite,
            bytesFromBudgetValue(budget["max_restir_di_reservoir_contract_violation_count"]));
    }
    if (budget.contains("max_restir_gi_reservoir_contract_violation_count")) {
        addReservoirContractFailure(
            "restir_gi.reservoir_contract_validation.violation_count",
            giContractActive,
            giContractChecked,
            giContractInvalidSourcePdf,
            giContractInvalidTargetPdf,
            giSourcePdfParityMismatch,
            giTargetPdfParityMismatch,
            giContractNonFinite,
            bytesFromBudgetValue(budget["max_restir_gi_reservoir_contract_violation_count"]));
    }
    if (budget.value("require_no_streaming_uploads", false)) {
        const uint64_t streamingUploads =
            profile.textureDiagnostics.is_object() &&
                profile.textureDiagnostics.contains("streaming_image_uploads_during_capture")
            ? jsonUint(profile.textureDiagnostics, "streaming_image_uploads_during_capture")
            : jsonUint(profile.textureDiagnostics, "streaming_upload_count");
        if (streamingUploads > 0ull) {
            failures.push_back({
                {"metric", "texture_cache_diagnostics.streaming_image_uploads_during_capture"},
                {"actual", streamingUploads},
                {"budget", 0},
            });
        }
    }
    if (budget.value("require_capture_ready_snapshot", false)) {
        const bool snapshotValid = jsonBool(profile.textureDiagnostics, "capture_upload_snapshot_valid");
        const uint64_t snapshotFrame = jsonUint(profile.textureDiagnostics, "capture_snapshot_frame");
        if (!snapshotValid || snapshotFrame == 0ull) {
            failures.push_back({
                {"metric", "texture_cache_diagnostics.capture_ready_snapshot"},
                {"capture_upload_snapshot_valid", snapshotValid},
                {"capture_snapshot_frame", snapshotFrame},
                {"budget", "capture-ready snapshot required"},
            });
        }
    }
    if (budget.contains("max_texture_warning_count")) {
        const uint64_t maxWarnings = bytesFromBudgetValue(budget["max_texture_warning_count"]);
        const uint64_t warningCount =
            profile.textureDiagnostics.is_object() && profile.textureDiagnostics.contains("warnings") &&
                profile.textureDiagnostics.at("warnings").is_array()
            ? profile.textureDiagnostics.at("warnings").size()
            : 0ull;
        if (warningCount > maxWarnings) {
            failures.push_back({
                {"metric", "texture_cache_diagnostics.warnings"},
                {"actual", warningCount},
                {"budget", maxWarnings},
            });
        }
    }
    if (budget.value("require_runtime_texture_residency", false)) {
        const uint64_t boundImported = jsonNestedUint(
            profile.textureDiagnostics,
            "runtime_residency",
            "bound_imported_texture_count");
        const uint64_t residentImported = jsonNestedUint(
            profile.textureDiagnostics,
            "runtime_residency",
            "resident_imported_texture_count");
        const uint64_t residentMips = jsonNestedUint(
            profile.textureDiagnostics,
            "runtime_residency",
            "resident_imported_mip_count");
        const uint64_t residentBytes = jsonNestedUint(
            profile.textureDiagnostics,
            "runtime_residency",
            "estimated_resident_imported_bytes");
        if (boundImported > 0ull && (residentImported == 0ull || residentMips == 0ull || residentBytes == 0ull)) {
            failures.push_back({
                {"metric", "texture_cache_diagnostics.runtime_residency.imported"},
                {"bound_imported_texture_count", boundImported},
                {"resident_imported_texture_count", residentImported},
                {"resident_imported_mip_count", residentMips},
                {"estimated_resident_imported_bytes", residentBytes},
                {"budget", "runtime residency required for imported textures"},
            });
        }
    }
    if (budget.contains("min_imported_resident_texture_count")) {
        const uint64_t minResident = bytesFromBudgetValue(budget["min_imported_resident_texture_count"]);
        const uint64_t residentImported = jsonNestedUint(
            profile.textureDiagnostics,
            "runtime_residency",
            "resident_imported_texture_count");
        if (residentImported < minResident) {
            failures.push_back({
                {"metric", "texture_cache_diagnostics.runtime_residency.resident_imported_texture_count"},
                {"actual", residentImported},
                {"budget", minResident},
            });
        }
    }
    if (budget.contains("max_imported_runtime_texture_bytes")) {
        const uint64_t maxBytes = bytesFromBudgetValue(budget["max_imported_runtime_texture_bytes"]);
        const uint64_t residentBytes = jsonNestedUint(
            profile.textureDiagnostics,
            "runtime_residency",
            "estimated_resident_imported_bytes");
        if (residentBytes > maxBytes) {
            failures.push_back({
                {"metric", "texture_cache_diagnostics.runtime_residency.estimated_resident_imported_bytes"},
                {"actual", residentBytes},
                {"budget", maxBytes},
            });
        }
    }
    if (budget.contains("max_high_optimization_hint_count")) {
        const uint64_t maxHints = bytesFromBudgetValue(budget["max_high_optimization_hint_count"]);
        uint64_t highHintCount = 0;
        if (profile.optimizationHints.is_array()) {
            for (const json& hint : profile.optimizationHints) {
                if (hint.value("severity", "") == "high") {
                    ++highHintCount;
                }
            }
        }
        if (highHintCount > maxHints) {
            failures.push_back({
                {"metric", "optimization_hints.high_count"},
                {"actual", highHintCount},
                {"budget", maxHints},
            });
        }
    }
    if (budget.contains("max_acceleration_structure_mb")) {
        const double maxAsMb = budget["max_acceleration_structure_mb"].get<double>();
        const double actualAsMb = static_cast<double>(profile.memory.accelerationStructureBytes) / (1024.0 * 1024.0);
        if (actualAsMb > maxAsMb) {
            failures.push_back({
                {"metric", "memory.acceleration_structure_mb"},
                {"actual_mb", actualAsMb},
                {"budget_mb", maxAsMb},
            });
        }
    }

    const std::map<std::string, uint64_t> memoryBytes = profileMemoryBytes(profile);
    if (budget.contains("memory_total_bytes")) {
        const uint64_t maxBytes = bytesFromBudgetValue(budget["memory_total_bytes"]);
        const uint64_t actualBytes = memoryBytes.at("total_bytes");
        if (actualBytes > maxBytes) {
            failures.push_back({{"metric", "memory.total_bytes"}, {"actual_bytes", actualBytes}, {"budget_bytes", maxBytes}});
        }
    }
    if (budget.contains("memory_total_mb")) {
        const double maxMb = budget["memory_total_mb"].get<double>();
        const double actualMb = static_cast<double>(memoryBytes.at("total_bytes")) / (1024.0 * 1024.0);
        if (actualMb > maxMb) {
            failures.push_back({{"metric", "memory.total_mb"}, {"actual_mb", actualMb}, {"budget_mb", maxMb}});
        }
    }
    const json memoryBudgets = budget.value("memory", json::object());
    for (auto it = memoryBudgets.begin(); it != memoryBudgets.end(); ++it) {
        const std::string metric = it.key();
        const uint64_t maxBytes = bytesFromBudgetValue(it.value());
        const uint64_t actualBytes = memoryBytes.contains(metric) ? memoryBytes.at(metric) : 0;
        if (actualBytes > maxBytes) {
            failures.push_back({{"metric", "memory." + metric}, {"actual_bytes", actualBytes}, {"budget_bytes", maxBytes}});
        }
    }
    json result = {{"status", failures.empty() ? "pass" : "fail"}, {"failures", failures}};
    std::cout << result.dump(2) << "\n";
    return failures.empty() ? 0 : 1;
}

int checkStreamingBudget(
    const std::filesystem::path& budgetPath,
    const ProfileReport& profile,
    const json& streamingReport) {
    const json budget = readJsonFile(budgetPath);
    json failures = json::array();
    json warnings = json::array();

    auto failMs = [&failures](std::string metric, double actual, double limit) {
        if (actual > limit) {
            failures.push_back({
                {"metric", std::move(metric)},
                {"actual_ms", actual},
                {"budget_ms", limit},
            });
        }
    };
    auto failBytes = [&failures](std::string metric, uint64_t actual, uint64_t limit) {
        if (actual > limit) {
            failures.push_back({
                {"metric", std::move(metric)},
                {"actual_bytes", actual},
                {"budget_bytes", limit},
            });
        }
    };
    auto failCount = [&failures](std::string metric, uint64_t actual, uint64_t limit) {
        if (actual > limit) {
            failures.push_back({
                {"metric", std::move(metric)},
                {"actual", actual},
                {"budget", limit},
            });
        }
    };

    const std::string schema = budget.value("schema", "");
    if (schema != "StreamingRegressionBudgetV1") {
        warnings.push_back({
            {"metric", "schema"},
            {"message", "budget schema is not StreamingRegressionBudgetV1; checking recognized streaming fields only"},
            {"actual", schema},
        });
    }

    if (budget.contains("max_cpu_frame_ms_p95")) {
        failMs("cpu_frame_ms.p95", profile.cpuFrameMs.p95, budget["max_cpu_frame_ms_p95"].get<double>());
    }
    if (budget.contains("max_cpu_frame_ms_p99")) {
        failMs("cpu_frame_ms.p99", profile.cpuFrameMs.p99, budget["max_cpu_frame_ms_p99"].get<double>());
    }
    if (budget.contains("max_gpu_frame_ms_p95")) {
        failMs("gpu_frame_ms.p95", profile.gpuFrameMs.p95, budget["max_gpu_frame_ms_p95"].get<double>());
    }
    if (budget.contains("max_gpu_frame_ms_p99")) {
        failMs("gpu_frame_ms.p99", profile.gpuFrameMs.p99, budget["max_gpu_frame_ms_p99"].get<double>());
    }

    const uint64_t residentCpuBytes = uintAt(streamingReport, {"native_gpu_asset_cache", "memory_pressure", "resident_cpu_bytes"});
    const uint64_t residentGpuBytes = uintAt(streamingReport, {"native_gpu_asset_cache", "memory_pressure", "resident_gpu_bytes"});
    const uint64_t retainedStagingBytes = uintAt(streamingReport, {"streaming_gpu_work_queue", "stats", "retained_staging_bytes"});
    const uint64_t peakRetainedStagingBytes = uintAt(streamingReport, {"streaming_gpu_work_queue", "stats", "peak_retained_staging_bytes"});
    const uint64_t inFlightUploadBytes = uintAt(streamingReport, {"native_gpu_asset_cache", "stats", "in_flight_upload_bytes"});
    const uint64_t requestedUploadBytesPerFrame = uintAt(streamingReport, {"options", "upload_bytes_per_frame"});
    const uint64_t submittedUploadBytesLastFrame = uintAt(streamingReport, {"streaming_gpu_work_queue", "hitch_summary", "last_frame", "submitted_bytes"});
    const uint64_t peakStreamingCpuBytes = residentCpuBytes + inFlightUploadBytes + std::max(retainedStagingBytes, peakRetainedStagingBytes);
    const uint64_t peakStreamingGpuBytes = residentGpuBytes + uintAt(streamingReport, {"native_gpu_asset_cache", "memory_pressure", "over_gpu_budget_bytes"});

    if (budget.contains("max_cpu_streaming_memory_mb")) {
        const uint64_t limit = bytesFromBudgetValue(budget["max_cpu_streaming_memory_mb"]) * 1024ull * 1024ull;
        failBytes("streaming.cpu_memory", peakStreamingCpuBytes, limit);
    }
    if (budget.contains("max_gpu_streaming_memory_mb")) {
        const uint64_t limit = bytesFromBudgetValue(budget["max_gpu_streaming_memory_mb"]) * 1024ull * 1024ull;
        failBytes("streaming.gpu_memory", peakStreamingGpuBytes, limit);
    }
    if (budget.contains("max_upload_bytes_per_frame")) {
        const uint64_t limit = bytesFromBudgetValue(budget["max_upload_bytes_per_frame"]);
        failBytes("streaming.upload_bytes_per_frame.configured", requestedUploadBytesPerFrame, limit);
        failBytes("streaming.upload_bytes_per_frame.last_frame", submittedUploadBytesLastFrame, limit);
    }

    if (budget.contains("max_hitch_count")) {
        const uint64_t limit = bytesFromBudgetValue(budget["max_hitch_count"]);
        const uint64_t stalledFrames = uintAt(streamingReport, {"streaming_gpu_work_queue", "hitch_summary", "stalled_frames"});
        const uint64_t failedAssets = uintAt(streamingReport, {"queue_snapshot", "failed"}) +
            uintAt(streamingReport, {"gpu_scene_streaming", "stats", "failed_instances"});
        const uint64_t hitchCount = stalledFrames + failedAssets;
        failCount("streaming.hitch_count", hitchCount, limit);
    }

    if (budget.contains("max_main_thread_apply_ms_p99")) {
        warnings.push_back({
            {"metric", "main_thread_apply_ms.p99"},
            {"message", "streaming runtime report does not yet expose main-thread apply p99; budget field recorded but not enforced"},
            {"budget_ms", budget["max_main_thread_apply_ms_p99"]},
        });
    }

    const json requiredMilestones = budget.value("minimum_progressive_visibility_milestones", json::array());
    for (const json& milestoneValue : requiredMilestones) {
        if (!milestoneValue.is_string()) {
            continue;
        }
        const std::string milestone = milestoneValue.get<std::string>();
        bool present = false;
        if (milestone == "metadata_shell") {
            present = uintAt(streamingReport, {"gpu_scene_streaming", "stats", "instance_count"}) > 0 ||
                uintAt(streamingReport, {"catalog", "asset_count"}) > 0 ||
                !streamingReport.value("asset_states", json::array()).empty();
        } else if (milestone == "proxy_visible") {
            present = uintAt(streamingReport, {"gpu_scene_streaming", "stats", "fallback_descriptor_instances"}) > 0 ||
                uintAt(streamingReport, {"native_gpu_asset_cache", "stats", "fallback_descriptor_count"}) > 0;
        } else if (milestone == "first_renderable_geometry" || milestone == "minimum_residency") {
            present = uintAt(streamingReport, {"gpu_scene_streaming", "stats", "renderable_instances"}) > 0 ||
                uintAt(streamingReport, {"native_gpu_asset_cache", "stats", "mesh_renderable_count"}) > 0;
        } else if (milestone == "full_quality_residency") {
            const uint64_t totalMips = uintAt(streamingReport, {"native_gpu_asset_cache", "stats", "total_texture_mip_count"});
            present = uintAt(streamingReport, {"native_gpu_asset_cache", "stats", "texture_fully_resident_count"}) > 0 ||
                (totalMips > 0 &&
                    uintAt(streamingReport, {"native_gpu_asset_cache", "stats", "resident_texture_mip_count"}) >= totalMips);
        } else {
            warnings.push_back({
                {"metric", "minimum_progressive_visibility_milestones." + milestone},
                {"message", "unknown streaming milestone; not enforced"},
            });
            continue;
        }

        if (!present) {
            failures.push_back({
                {"metric", "minimum_progressive_visibility_milestones." + milestone},
                {"actual", false},
                {"budget", true},
            });
        }
    }

    const json result = {
        {"schema", "StreamingRegressionBudgetCheckV1"},
        {"budget", budgetPath.generic_string()},
        {"budget_name", budget.value("name", budgetPath.stem().string())},
        {"status", failures.empty() ? "pass" : "fail"},
        {"observed", {
            {"cpu_frame_ms_p95", profile.cpuFrameMs.p95},
            {"cpu_frame_ms_p99", profile.cpuFrameMs.p99},
            {"gpu_frame_ms_p95", profile.gpuFrameMs.p95},
            {"gpu_frame_ms_p99", profile.gpuFrameMs.p99},
            {"streaming_cpu_memory_bytes", peakStreamingCpuBytes},
            {"streaming_gpu_memory_bytes", peakStreamingGpuBytes},
            {"configured_upload_bytes_per_frame", requestedUploadBytesPerFrame},
            {"last_frame_upload_bytes", submittedUploadBytesLastFrame},
            {"stalled_frames", uintAt(streamingReport, {"streaming_gpu_work_queue", "hitch_summary", "stalled_frames"})},
        }},
        {"warnings", warnings},
        {"failures", failures},
    };
    std::cout << result.dump(2) << "\n";
    return failures.empty() ? 0 : 1;
}

std::filesystem::path defaultDiagnosticArtifactDir(const std::filesystem::path& scenePath, std::string_view name) {
    const std::string caseName = scenePath.empty() ? "default" : scenePath.stem().string();
    return std::filesystem::path("out") / "diagnostics" / caseName / std::string(name);
}

} // namespace rtv
