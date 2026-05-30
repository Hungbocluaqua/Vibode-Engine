#pragma once

#include "rtv/HeadlessDiagnostics.h"

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace rtv {

struct ImageDiffMetrics {
    uint32_t width = 0;
    uint32_t height = 0;
    double mse = 0.0;
    double psnr = 0.0;
    double ssim = 1.0;
    uint32_t maxError = 0;
    double changedPixelPercentage = 0.0;
};

struct SequenceMetricSummary {
    uint32_t frameCount = 0;
    double averageMse = 0.0;
    double maxMse = 0.0;
    double averagePsnr = 0.0;
    double worstPsnr = 99.0;
    double averageSsim = 1.0;
    double bestSsim = 1.0;
    double worstSsim = 1.0;
    double averageChangedPixelPercentage = 0.0;
    double maxChangedPixelPercentage = 0.0;
    uint32_t worstFrame = 0;
};

struct SequenceTemporalMetrics {
    uint32_t pairCount = 0;
    double averageFrameDeltaMse = 0.0;
    double maxFrameDeltaMse = 0.0;
    double averageFrameDeltaChangedPixelPercentage = 0.0;
    double maxFrameDeltaChangedPixelPercentage = 0.0;
    double temporalVarianceScore = 0.0;
    uint32_t worstPairStartFrame = 0;
};

struct SequenceViewComparison {
    std::string view;
    SequenceMetricSummary baselineVsCurrent;
    SequenceTemporalMetrics baselineTemporal;
    SequenceTemporalMetrics currentTemporal;
};

struct SequenceComparisonReport {
    std::vector<SequenceViewComparison> views;
    std::vector<std::string> warnings;
};

struct BaselinePaths {
    std::filesystem::path root;
    std::filesystem::path caseDir;
    std::filesystem::path profile;
    std::filesystem::path renderGraph;
    std::filesystem::path beautyImage;
    std::filesystem::path frameSequence;
};

[[nodiscard]] int compareProfileCommand(
    const std::filesystem::path& oldPath,
    const std::filesystem::path& newPath);

[[nodiscard]] int compareImageCommand(
    const std::filesystem::path& baselinePath,
    const std::filesystem::path& currentPath,
    const std::optional<std::filesystem::path>& diffOutputPath);

[[nodiscard]] int compareImageSequenceCommand(
    const std::filesystem::path& baselineDir,
    const std::filesystem::path& currentDir,
    const std::optional<std::filesystem::path>& outputDir,
    const std::vector<std::string>& requestedViews);

[[nodiscard]] ImageDiffMetrics compareImages(
    const std::filesystem::path& baselinePath,
    const std::filesystem::path& currentPath,
    const std::optional<std::filesystem::path>& diffOutputPath);

[[nodiscard]] SequenceComparisonReport compareImageSequences(
    const std::filesystem::path& baselineDir,
    const std::filesystem::path& currentDir,
    const std::optional<std::filesystem::path>& outputDir,
    const std::vector<std::string>& requestedViews);

[[nodiscard]] BaselinePaths baselinePathsFor(
    const std::filesystem::path& scenePath,
    const std::filesystem::path& baselineRoot);

void updateBaseline(
    const BaselinePaths& paths,
    const std::filesystem::path& profilePath,
    const std::filesystem::path& renderGraphPath,
    const std::filesystem::path& debugViewsDir,
    const std::optional<std::filesystem::path>& frameSequenceDir = std::nullopt);

[[nodiscard]] int checkBaseline(
    const BaselinePaths& paths,
    const std::filesystem::path& profilePath,
    const std::filesystem::path& renderGraphPath,
    const std::filesystem::path& debugViewsDir);

void writeMemoryReport(
    const std::filesystem::path& outputPath,
    const ProfileReport& profile,
    const std::optional<std::filesystem::path>& renderGraphPath = std::nullopt);

void writeFrameTimeline(
    const std::filesystem::path& outputPath,
    const ProfileReport& profile,
    const std::optional<std::filesystem::path>& renderGraphPath);

void writeResourceLifetimes(
    const std::filesystem::path& outputPath,
    const std::optional<std::filesystem::path>& renderGraphPath);

void writeShaderReport(
    const std::filesystem::path& outputPath,
    const std::filesystem::path& shaderSourceDir,
    const std::filesystem::path& shaderOutputDir);

void writeBindingsReport(
    const std::filesystem::path& outputPath,
    const std::optional<std::filesystem::path>& renderGraphPath);

void writeCrashDumpPackage(
    const std::filesystem::path& outputDir,
    const std::filesystem::path& scenePath,
    const std::optional<std::filesystem::path>& profilePath,
    const std::optional<std::filesystem::path>& renderGraphPath,
    const std::optional<std::filesystem::path>& debugViewsDir,
    const std::string& capturedLog);

[[nodiscard]] int validateGpuLabels(
    const std::optional<std::filesystem::path>& renderGraphPath);

[[nodiscard]] int checkBudget(
    const std::filesystem::path& budgetPath,
    const ProfileReport& profile);

[[nodiscard]] std::filesystem::path defaultDiagnosticArtifactDir(
    const std::filesystem::path& scenePath,
    std::string_view name);

} // namespace rtv
