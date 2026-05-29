#pragma once

#include "rtv/RendererDebug.h"
#include "rtv/RendererSettings.h"

#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

namespace rtv {

class Application;
class DiagnosticImageExport;
class PathTracerRenderer;

struct HeadlessDiagnosticsConfig {
    bool headless = false;
    uint32_t warmupFrames = 0;
    uint32_t totalFrames = 120;
    std::optional<uint32_t> fixedSeed;
    bool profile = false;
    bool runValidationSuite = false;
    std::optional<std::filesystem::path> profileJsonPath;
    std::optional<std::filesystem::path> dumpRenderGraphPath;
    std::optional<std::filesystem::path> saveDebugViewsDir;
    std::optional<std::filesystem::path> captureRenderDocPath;
    uint32_t captureFrame = 60;
    std::optional<std::filesystem::path> makeDebugPackageDir;
    std::optional<std::filesystem::path> validationOutputDir;
    bool disableAsyncCompute = false;
    bool singleQueueFallback = false;
    bool disableResourceAliasing = false;
};

struct ProfileReport {
    std::string engineVersion = "0.1.0";
    std::string gitCommit;
    std::string gpuName;
    std::string driverVersion;
    std::string vulkanVersion;
    std::string restirGiLayout = "compressed";

    struct Resolution {
        uint32_t renderWidth = 0;
        uint32_t renderHeight = 0;
        uint32_t displayWidth = 0;
        uint32_t displayHeight = 0;
        float renderScale = 1.0f;
    } resolution{};

    uint32_t frameCount = 0;
    uint32_t warmupFrames = 0;
    uint32_t profiledFrames = 0;

    struct MinMaxAvg {
        float min = 0.0f;
        float avg = 0.0f;
        float max = 0.0f;
    };
    MinMaxAvg cpuFrameMs{};
    MinMaxAvg gpuFrameMs{};

    struct PerPassGpuMs {
        float pathTrace = 0.0f;
        float restirHistoryClear = 0.0f;
        float restirGiClear = 0.0f;
        float restirSpatial = 0.0f;
        float restirSpatialCopy = 0.0f;
        float restirGiSpatial = 0.0f;
        float restirGiFinal = 0.0f;
        float fogIntegrate = 0.0f;
        float atmosphere = 0.0f;
        float atmosphereTransmittance = 0.0f;
        float atmosphereMultiScatter = 0.0f;
        float atmosphereSkyView = 0.0f;
        float atmosphereSkyReproject = 0.0f;
        float atmosphereSkyCdf = 0.0f;
        float atmosphereAerialPerspective = 0.0f;
        float denoiser = 0.0f;
        float momentUpdate = 0.0f;
        float historyCopy = 0.0f;
        float skipDenoiserCopy = 0.0f;
        float taa = 0.0f;
        float taaHistoryCopy = 0.0f;
        float autoExposureHistogramClear = 0.0f;
        float autoExposureHistogram = 0.0f;
        float autoExposureReduce = 0.0f;
        float toneMap = 0.0f;
        float selectionOutline = 0.0f;
        float fullscreen = 0.0f;
        float editorPresentation = 0.0f;
    } perPassGpuMs{};

    struct PipelineStatistics {
        uint64_t rayInvocations = 0;
        uint64_t triangleHits = 0;
        uint64_t aabbHits = 0;
    } pipelineStatistics{};

    struct MemoryReport {
        uint64_t texturesBytes = 0;
        uint64_t buffersBytes = 0;
        uint64_t accelerationStructureBytes = 0;
        uint64_t temporalHistoryBytes = 0;
        uint64_t restirReservoirBytes = 0;
        uint64_t restirDiCurrentBytes = 0;
        uint64_t restirDiPreviousBytes = 0;
        uint64_t restirDiSpatialBytes = 0;
        uint64_t restirGiCurrentBytes = 0;
        uint64_t restirGiPreviousBytes = 0;
        uint64_t restirGiSpatialBytes = 0;
    } memory{};

    uint32_t validationErrorCount = 0;
    std::vector<std::string> warnings;

    RendererSettings settings{};
};

struct ValidationSceneResult {
    std::string name;
    std::string status;
    float gpuMsTotal = 0.0f;
    uint32_t validationErrors = 0;
    uint32_t framesRendered = 0;
};

struct ValidationSuiteSummary {
    std::vector<ValidationSceneResult> scenes;
    uint32_t totalPass = 0;
    uint32_t totalFail = 0;
};

class HeadlessDiagnostics {
public:
    explicit HeadlessDiagnostics(const HeadlessDiagnosticsConfig& config);
    ~HeadlessDiagnostics();

    ProfileReport run(Application& app);
    void writeProfileJson(const std::filesystem::path& path) const;
    void writeRenderGraphJson(const std::filesystem::path& path);
    void exportDebugViews(Application& app, const std::filesystem::path& dir);
    void makeDebugPackage(Application& app, const std::filesystem::path& dir, const std::filesystem::path& scenePath);
    ValidationSuiteSummary runValidationSuite();

    void captureStdout();
    std::string releaseStdout();

    [[nodiscard]] const ProfileReport& profileReport() const { return profileReport_; }
    [[nodiscard]] ProfileReport& profileReport() { return profileReport_; }

    [[nodiscard]] const std::filesystem::path& profileJsonPath() const { return profileJsonPath_; }

private:
    void collectValidationLog(Application& app);

    HeadlessDiagnosticsConfig config_;
    ProfileReport profileReport_;
    std::filesystem::path profileJsonPath_;
    std::unique_ptr<std::ostringstream> logCapture_;
    std::streambuf* oldCout_ = nullptr;
    std::streambuf* oldCerr_ = nullptr;
};

} // namespace rtv
