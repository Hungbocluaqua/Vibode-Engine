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

struct BaselinePaths {
    std::filesystem::path root;
    std::filesystem::path caseDir;
    std::filesystem::path profile;
    std::filesystem::path renderGraph;
    std::filesystem::path beautyImage;
};

[[nodiscard]] int compareProfileCommand(
    const std::filesystem::path& oldPath,
    const std::filesystem::path& newPath);

[[nodiscard]] int compareImageCommand(
    const std::filesystem::path& baselinePath,
    const std::filesystem::path& currentPath,
    const std::optional<std::filesystem::path>& diffOutputPath);

[[nodiscard]] ImageDiffMetrics compareImages(
    const std::filesystem::path& baselinePath,
    const std::filesystem::path& currentPath,
    const std::optional<std::filesystem::path>& diffOutputPath);

[[nodiscard]] BaselinePaths baselinePathsFor(
    const std::filesystem::path& scenePath,
    const std::filesystem::path& baselineRoot);

void updateBaseline(
    const BaselinePaths& paths,
    const std::filesystem::path& profilePath,
    const std::filesystem::path& renderGraphPath,
    const std::filesystem::path& debugViewsDir);

[[nodiscard]] int checkBaseline(
    const BaselinePaths& paths,
    const std::filesystem::path& profilePath,
    const std::filesystem::path& renderGraphPath,
    const std::filesystem::path& debugViewsDir);

void writeMemoryReport(
    const std::filesystem::path& outputPath,
    const ProfileReport& profile);

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
