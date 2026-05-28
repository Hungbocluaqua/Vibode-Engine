#include "rtv/DiagnosticTools.h"

#include "rtv/RenderGraphDump.h"

#include <nlohmann/json.hpp>
#include <stb_image.h>
#include <stb_image_write.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <numeric>
#include <sstream>
#include <stdexcept>
#include <unordered_map>

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

json compareProfilesJson(const json& oldProfile, const json& newProfile) {
    json result;
    result["cpu_frame_ms"] = {
        {"avg", metricDelta(numberAt(oldProfile, {"cpu_frame_ms", "avg"}), numberAt(newProfile, {"cpu_frame_ms", "avg"}), "ms")},
        {"min", metricDelta(numberAt(oldProfile, {"cpu_frame_ms", "min"}), numberAt(newProfile, {"cpu_frame_ms", "min"}), "ms")},
        {"max", metricDelta(numberAt(oldProfile, {"cpu_frame_ms", "max"}), numberAt(newProfile, {"cpu_frame_ms", "max"}), "ms")},
    };
    result["gpu_frame_ms"] = {
        {"avg", metricDelta(numberAt(oldProfile, {"gpu_frame_ms", "avg"}), numberAt(newProfile, {"gpu_frame_ms", "avg"}), "ms")},
        {"min", metricDelta(numberAt(oldProfile, {"gpu_frame_ms", "min"}), numberAt(newProfile, {"gpu_frame_ms", "min"}), "ms")},
        {"max", metricDelta(numberAt(oldProfile, {"gpu_frame_ms", "max"}), numberAt(newProfile, {"gpu_frame_ms", "max"}), "ms")},
    };

    json passJson = json::object();
    const json oldPasses = oldProfile.value("per_pass_gpu_ms", json::object());
    const json newPasses = newProfile.value("per_pass_gpu_ms", json::object());
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
    result["per_pass_gpu_ms"] = passJson;

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
    uint64_t oldTotal = 0;
    uint64_t newTotal = 0;
    for (const std::string& name : memoryNames) {
        const uint64_t oldValue = oldMemory.value(name, 0ull);
        const uint64_t newValue = newMemory.value(name, 0ull);
        oldTotal += oldValue;
        newTotal += newValue;
        memoryJson[name] = metricDelta(static_cast<double>(oldValue), static_cast<double>(newValue), "bytes");
    }
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
    };
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
    return metrics;
}

int compareImageCommand(
    const std::filesystem::path& baselinePath,
    const std::filesystem::path& currentPath,
    const std::optional<std::filesystem::path>& diffOutputPath) {
    const ImageDiffMetrics metrics = compareImages(baselinePath, currentPath, diffOutputPath);
    json result = imageMetricsJson(metrics);
    if (diffOutputPath.has_value()) {
        result["diff_image"] = diffOutputPath->string();
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
    return paths;
}

void updateBaseline(
    const BaselinePaths& paths,
    const std::filesystem::path& profilePath,
    const std::filesystem::path& renderGraphPath,
    const std::filesystem::path& debugViewsDir) {
    std::filesystem::create_directories(paths.caseDir);
    std::filesystem::copy_file(profilePath, paths.profile, std::filesystem::copy_options::overwrite_existing);
    std::filesystem::copy_file(renderGraphPath, paths.renderGraph, std::filesystem::copy_options::overwrite_existing);
    std::filesystem::copy_file(beautyPath(debugViewsDir), paths.beautyImage, std::filesystem::copy_options::overwrite_existing);
    json manifest = {
        {"profile", paths.profile.filename().string()},
        {"rendergraph", paths.renderGraph.filename().string()},
        {"beauty", paths.beautyImage.filename().string()},
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
    const bool failed = imageFailed || profileFailed || renderGraphChanged;

    json result = {
        {"status", failed ? "fail" : "pass"},
        {"profile", profileComparison},
        {"image", imageMetricsJson(imageMetrics)},
        {"rendergraph", {
            {"changed", renderGraphChanged},
            {"baseline_passes", baselinePasses},
            {"current_passes", currentPasses},
        }},
    };
    std::cout << result.dump(2) << "\n";
    return failed ? 1 : 0;
}

void writeMemoryReport(const std::filesystem::path& outputPath, const ProfileReport& profile) {
    const uint64_t persistent =
        profile.memory.texturesBytes +
        profile.memory.buffersBytes +
        profile.memory.accelerationStructureBytes +
        profile.memory.temporalHistoryBytes +
        profile.memory.restirReservoirBytes;
    json j = {
        {"textures_bytes", profile.memory.texturesBytes},
        {"buffers_bytes", profile.memory.buffersBytes},
        {"acceleration_structure_bytes", profile.memory.accelerationStructureBytes},
        {"temporal_history_bytes", profile.memory.temporalHistoryBytes},
        {"restir_reservoir_bytes", profile.memory.restirReservoirBytes},
        {"transient_resources_bytes", 0},
        {"persistent_resources_bytes", persistent},
        {"total_bytes", persistent},
        {"notes", json::array({"transient resource byte accounting is reported as 0 until RenderGraph allocator exposes per-frame alias pool totals"})},
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
    json j = {
        {"cpu_events", {
            {{"name", "frame"}, {"min_ms", profile.cpuFrameMs.min}, {"avg_ms", profile.cpuFrameMs.avg}, {"max_ms", profile.cpuFrameMs.max}},
        }},
        {"gpu_passes", gpuPasses},
        {"queue_submits", json::array({{{"queue", "graphics"}, {"count", 1}}})},
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
            {"created_by", nullptr},
            {"destroyed_after", nullptr},
            {"reads", json::array()},
            {"writes", json::array()},
            {"aliases", json::array()},
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
    json out = json::array();
    for (auto& [name, resource] : resources) {
        (void)name;
        out.push_back(resource);
    }
    writeJsonFile(outputPath, {{"resources", out}});
}

void writeShaderReport(
    const std::filesystem::path& outputPath,
    const std::filesystem::path& shaderSourceDir,
    const std::filesystem::path& shaderOutputDir) {
    json shaders = json::array();
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
            };
            if (std::filesystem::exists(spirv)) {
                const std::vector<unsigned char> bytes = readBinary(spirv);
                shader["hash"] = hexHashBytes(bytes);
                shader["spirv_size_bytes"] = bytes.size();
            }
            shaders.push_back(shader);
        }
    }
    writeJsonFile(outputPath, {{"shaders", shaders}});
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
            {"binding_41", "bindless combined image samplers"},
            {"binding_42", "path data storage buffer"},
            {"binding_43", "ReSTIR GI current reservoir storage buffer"},
            {"binding_44", "ReSTIR GI previous reservoir storage buffer"},
            {"binding_45", "ReSTIR GI spatial reservoir storage buffer"},
        }},
    };
    writeJsonFile(outputPath, {{"passes", passes}, {"known_descriptor_sets", knownSets}});
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
    const json passBudgets = budget.value("per_pass_gpu_ms", json::object());
    std::map<std::string, double> actual = {
        {"path_trace", profile.perPassGpuMs.pathTrace},
        {"restir_history_clear", profile.perPassGpuMs.restirHistoryClear},
        {"restir_gi_clear", profile.perPassGpuMs.restirGiClear},
        {"restir_spatial", profile.perPassGpuMs.restirSpatial},
        {"restir_spatial_copy", profile.perPassGpuMs.restirSpatialCopy},
        {"restir_gi_spatial", profile.perPassGpuMs.restirGiSpatial},
        {"restir_gi_final", profile.perPassGpuMs.restirGiFinal},
        {"fog_integrate", profile.perPassGpuMs.fogIntegrate},
        {"atmosphere", profile.perPassGpuMs.atmosphere},
        {"atmosphere_transmittance", profile.perPassGpuMs.atmosphereTransmittance},
        {"atmosphere_multi_scatter", profile.perPassGpuMs.atmosphereMultiScatter},
        {"atmosphere_sky_view", profile.perPassGpuMs.atmosphereSkyView},
        {"atmosphere_sky_reproject", profile.perPassGpuMs.atmosphereSkyReproject},
        {"atmosphere_sky_cdf", profile.perPassGpuMs.atmosphereSkyCdf},
        {"atmosphere_aerial_perspective", profile.perPassGpuMs.atmosphereAerialPerspective},
        {"denoiser", profile.perPassGpuMs.denoiser},
        {"history_copy", profile.perPassGpuMs.historyCopy},
        {"skip_denoiser_copy", profile.perPassGpuMs.skipDenoiserCopy},
        {"taa", profile.perPassGpuMs.taa},
        {"taa_history_copy", profile.perPassGpuMs.taaHistoryCopy},
        {"auto_exposure_histogram_clear", profile.perPassGpuMs.autoExposureHistogramClear},
        {"auto_exposure_histogram", profile.perPassGpuMs.autoExposureHistogram},
        {"auto_exposure_reduce", profile.perPassGpuMs.autoExposureReduce},
        {"tone_map", profile.perPassGpuMs.toneMap},
        {"selection_outline", profile.perPassGpuMs.selectionOutline},
        {"fullscreen", profile.perPassGpuMs.fullscreen},
        {"editor_presentation", profile.perPassGpuMs.editorPresentation},
    };

    json failures = json::array();
    for (auto it = passBudgets.begin(); it != passBudgets.end(); ++it) {
        const std::string pass = it.key();
        const double maxMs = it.value().get<double>();
        const double actualMs = actual.contains(pass) ? actual[pass] : 0.0;
        if (actualMs > maxMs) {
            failures.push_back({{"pass", pass}, {"actual_ms", actualMs}, {"budget_ms", maxMs}});
        }
    }
    if (budget.contains("gpu_frame_ms")) {
        const double maxFrame = budget["gpu_frame_ms"].get<double>();
        if (profile.gpuFrameMs.avg > maxFrame) {
            failures.push_back({{"pass", "gpu_frame"}, {"actual_ms", profile.gpuFrameMs.avg}, {"budget_ms", maxFrame}});
        }
    }
    json result = {{"status", failures.empty() ? "pass" : "fail"}, {"failures", failures}};
    std::cout << result.dump(2) << "\n";
    return failures.empty() ? 0 : 1;
}

std::filesystem::path defaultDiagnosticArtifactDir(const std::filesystem::path& scenePath, std::string_view name) {
    const std::string caseName = scenePath.empty() ? "default" : scenePath.stem().string();
    return std::filesystem::path("out") / "diagnostics" / caseName / std::string(name);
}

} // namespace rtv
