#include "rtv/Application.h"
#include "rtv/DiagnosticTools.h"
#include "rtv/HeadlessDiagnostics.h"
#include "rtv/PathTracerRenderer.h"
#include "rtv/RendererDebug.h"
#include "rtv/RendererSettings.h"
#include "rtv/RenderGraphDump.h"
#include "rtv/RenderGraph.h"
#include "rtv/GpuProfiler.h"

#include <exception>
#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>
#include <sstream>
#include <stdexcept>
#include <vector>

#ifdef RTV_HAS_RENDERDOC
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <renderdoc_app.h>
static RENDERDOC_API_1_6_0* rdocApi = nullptr;
static std::filesystem::path rdocCapturePath;
static uint32_t rdocCaptureFrame = 60;
static bool rdocCaptureRequested = false;
static std::optional<std::filesystem::path> rdocDllPathOverride;
static std::string rdocCaptureTemplate;

static std::optional<std::filesystem::path> renderDocEnvPath(const char* name) {
    char* value = nullptr;
    size_t length = 0;
    if (_dupenv_s(&value, &length, name) != 0 || value == nullptr) {
        return std::nullopt;
    }
    std::string result(value);
    std::free(value);
    if (result.empty()) {
        return std::nullopt;
    }
    return std::filesystem::path(result);
}

static void addRenderDocCandidate(std::vector<std::filesystem::path>& candidates, std::filesystem::path path) {
    if (path.empty()) {
        return;
    }
    if (path.filename() != "renderdoc.dll") {
        path /= "renderdoc.dll";
    }
    candidates.push_back(std::move(path));
}

static void addRenderDocEnvCandidate(std::vector<std::filesystem::path>& candidates, const char* name) {
    if (std::optional<std::filesystem::path> path = renderDocEnvPath(name)) {
        addRenderDocCandidate(candidates, *path);
        addRenderDocCandidate(candidates, *path / "api" / "app");
        addRenderDocCandidate(candidates, path->parent_path().parent_path());
    }
}

static HMODULE loadRenderDocDll() {
    if (HMODULE existing = GetModuleHandleA("renderdoc.dll")) {
        return existing;
    }

    std::vector<std::filesystem::path> candidates;
    if (rdocDllPathOverride.has_value()) {
        addRenderDocCandidate(candidates, *rdocDllPathOverride);
    }
    addRenderDocEnvCandidate(candidates, "RENDERDOC_DLL_PATH");
    addRenderDocEnvCandidate(candidates, "RENDERDOC_DIR");
    addRenderDocEnvCandidate(candidates, "RENDERDOC_SDK_DIR");
    addRenderDocCandidate(candidates, std::filesystem::current_path());
    if (std::optional<std::filesystem::path> programFiles = renderDocEnvPath("ProgramFiles")) {
        addRenderDocCandidate(candidates, *programFiles / "RenderDoc");
    }
    if (std::optional<std::filesystem::path> programFilesX86 = renderDocEnvPath("ProgramFiles(x86)")) {
        addRenderDocCandidate(candidates, *programFilesX86 / "RenderDoc");
    }

    for (const std::filesystem::path& candidate : candidates) {
        if (!std::filesystem::exists(candidate)) {
            continue;
        }
        if (HMODULE loaded = LoadLibraryA(candidate.string().c_str())) {
            std::cout << "Loaded RenderDoc DLL: " << candidate.string() << "\n";
            return loaded;
        }
    }

    if (HMODULE loaded = LoadLibraryA("renderdoc.dll")) {
        std::cout << "Loaded RenderDoc DLL from DLL search path\n";
        return loaded;
    }
    return nullptr;
}

static void initRenderDoc() {
    HMODULE mod = loadRenderDocDll();
    if (mod == nullptr) {
        std::cerr << "Warning: RenderDoc DLL not loaded. Set --renderdoc-dll <path>, RENDERDOC_DLL_PATH, or install RenderDoc.\n";
        return;
    }
    auto getApi = (pRENDERDOC_GetAPI)GetProcAddress(mod, "RENDERDOC_GetAPI");
    if (getApi != nullptr) {
        getApi(eRENDERDOC_API_Version_1_6_0, (void**)&rdocApi);
    }
    if (rdocApi == nullptr) {
        std::cerr << "Warning: RenderDoc API not available.\n";
    }
}
#endif

int main(int argc, char** argv) {
    try {
        uint32_t maxFrames = 0;
        rtv::RendererDebugView debugView = rtv::RendererDebugView::Beauty;
        bool debugViewProvided = false;
        std::optional<std::filesystem::path> gltfPath;
        std::optional<std::filesystem::path> hdrPath;
        std::optional<std::filesystem::path> scenePath;
        std::optional<bool> denoiserOverride;
        std::optional<rtv::RestirMode> restirModeOverride;
        std::optional<rtv::RenderPreset> renderPresetOverride;
        std::optional<bool> restirGiOverride;
        std::optional<bool> opacityMicromapOverride;
        std::optional<uint32_t> opacityMicromapSubdivisionOverride;
        std::optional<bool> wavefrontQueuesOverride;
        std::optional<bool> wavefrontPrimaryGenerateOverride;
        std::optional<bool> wavefrontTraceOverride;
        std::optional<bool> wavefrontShadeOverride;
        std::optional<bool> wavefrontShadowTraceOverride;
        std::optional<bool> wavefrontCompactOverride;
        std::optional<bool> wavefrontSortOverride;
        std::optional<bool> wavefrontFinalOutputOverride;
        std::optional<bool> shaderExecutionReorderingOverride;
        std::optional<float> dofApertureRadiusOverride;
        std::optional<float> dofFocusDistanceOverride;
        std::optional<uint32_t> dofBladeCountOverride;
        std::optional<float> dofBokehRotationOverride;
        std::optional<bool> motionBlurOverride;
        std::optional<float> motionBlurShutterOpenOverride;
        std::optional<float> motionBlurShutterCloseOverride;
        std::optional<bool> homogeneousVolumeOverride;
        std::optional<float> homogeneousVolumeScatteringOverride;
        std::optional<float> homogeneousVolumeAbsorptionOverride;
        std::optional<float> homogeneousVolumeAnisotropyOverride;
        std::optional<bool> mneeCausticsOverride;
        bool wavefrontValidationMode = false;
        std::optional<float> taaMotionFeedbackOverride;
        std::optional<float> taaReactiveFeedbackOverride;
        std::optional<uint32_t> samplesPerPixelOverride;
        std::optional<bool> sppLimiterOverride;
        bool validationCameraMotion = false;
        bool validationObjectMotion = false;

        rtv::HeadlessDiagnosticsConfig diagConfig;
        bool dumpRenderGraphDot = false;
        std::optional<std::filesystem::path> dotOutputPath;
        std::optional<std::filesystem::path> compareProfileOldPath;
        std::optional<std::filesystem::path> compareProfileNewPath;
        std::optional<std::filesystem::path> compareImageBaselinePath;
        std::optional<std::filesystem::path> compareImageCurrentPath;
        std::optional<std::filesystem::path> compareImageSequenceBaselinePath;
        std::optional<std::filesystem::path> compareImageSequenceCurrentPath;
        std::optional<std::filesystem::path> compareImageOutputPath;
        bool updateBaseline = false;
        bool checkBaseline = false;
        std::filesystem::path baselineRoot = "baselines";
        std::optional<std::filesystem::path> dumpMemoryPath;
        std::optional<std::filesystem::path> dumpFrameTimelinePath;
        std::optional<std::filesystem::path> dumpResourceLifetimesPath;
        std::optional<std::filesystem::path> dumpShaderReportPath;
        std::optional<std::filesystem::path> dumpBindingsPath;
        std::optional<std::filesystem::path> crashDumpPackageDir;
        std::optional<std::filesystem::path> checkBudgetPath;
        std::optional<std::filesystem::path> descriptorLifetimeStressPath;
        uint32_t descriptorLifetimeStressCycles = 12;
        uint32_t descriptorLifetimeStressFrames = 2;
        bool validateGpuLabels = false;
        bool shaderHotReloadReport = false;
        std::optional<std::string> cameraName;
        std::optional<uint32_t> frameIndex;
        std::vector<std::string> disabledPasses;
        std::vector<std::string> sequenceViewNames;

        auto splitCsv = [](std::string_view value) {
            std::vector<std::string> result;
            std::stringstream stream{std::string(value)};
            std::string item;
            while (std::getline(stream, item, ',')) {
                item.erase(item.begin(), std::find_if(item.begin(), item.end(), [](unsigned char ch) {
                    return !std::isspace(ch);
                }));
                item.erase(std::find_if(item.rbegin(), item.rend(), [](unsigned char ch) {
                    return !std::isspace(ch);
                }).base(), item.end());
                if (!item.empty()) {
                    result.push_back(item);
                }
            }
            return result;
        };

        for (int i = 1; i < argc; ++i) {
            std::string_view arg(argv[i]);

            if (arg == "--frames" && i + 1 < argc) {
                maxFrames = static_cast<uint32_t>(std::stoul(argv[++i]));
            } else if (arg == "--compare-profile" && i + 2 < argc) {
                compareProfileOldPath = std::filesystem::path(argv[++i]);
                compareProfileNewPath = std::filesystem::path(argv[++i]);
            } else if (arg == "--compare-image" && i + 2 < argc) {
                compareImageBaselinePath = std::filesystem::path(argv[++i]);
                compareImageCurrentPath = std::filesystem::path(argv[++i]);
            } else if (arg == "--compare-image-sequence" && i + 2 < argc) {
                compareImageSequenceBaselinePath = std::filesystem::path(argv[++i]);
                compareImageSequenceCurrentPath = std::filesystem::path(argv[++i]);
            } else if (arg == "--out" && i + 1 < argc) {
                compareImageOutputPath = std::filesystem::path(argv[++i]);
            } else if (arg == "--debug-view" && i + 1 < argc) {
                debugView = rtv::parseRendererDebugView(argv[++i]);
                debugViewProvided = true;
            } else if (arg == "--gltf" && i + 1 < argc) {
                gltfPath = std::filesystem::path(argv[++i]);
            } else if (arg == "--hdr" && i + 1 < argc) {
                hdrPath = std::filesystem::path(argv[++i]);
            } else if ((arg == "--scene" || arg == "--rtlevel") && i + 1 < argc) {
                scenePath = std::filesystem::path(argv[++i]);
            } else if (arg == "--denoiser" && i + 1 < argc) {
                const std::string_view value(argv[++i]);
                denoiserOverride = !(value == "off" || value == "false" || value == "0");
            } else if (arg == "--restir" && i + 1 < argc) {
                const std::string_view value(argv[++i]);
                if (value == "classic" || value == "off" || value == "nee") {
                    restirModeOverride = rtv::RestirMode::ClassicNee;
                } else if (value == "restir" || value == "on" || value == "only") {
                    restirModeOverride = rtv::RestirMode::RestirOnly;
                } else if (value == "hybrid" || value == "compare") {
                    restirModeOverride = rtv::RestirMode::HybridCompare;
                } else {
                    throw std::runtime_error("Unknown ReSTIR mode: " + std::string(value));
                }
            } else if (arg == "--render-preset" && i + 1 < argc) {
                renderPresetOverride = rtv::parseRenderPreset(argv[++i]);
            } else if (arg == "--restir-gi" && i + 1 < argc) {
                const std::string_view value(argv[++i]);
                restirGiOverride = !(value == "off" || value == "false" || value == "0");
            } else if ((arg == "--opacity-micromaps" || arg == "--omm") && i + 1 < argc) {
                const std::string_view value(argv[++i]);
                opacityMicromapOverride = !(value == "off" || value == "false" || value == "0");
            } else if ((arg == "--omm-subdivision" || arg == "--opacity-micromap-subdivision") && i + 1 < argc) {
                opacityMicromapSubdivisionOverride = static_cast<uint32_t>(std::stoul(argv[++i]));
            } else if (arg == "--wavefront-queues" && i + 1 < argc) {
                const std::string_view value(argv[++i]);
                wavefrontQueuesOverride = !(value == "off" || value == "false" || value == "0");
            } else if ((arg == "--wavefront-primary-generate" || arg == "--wavefront-generate") && i + 1 < argc) {
                const std::string_view value(argv[++i]);
                wavefrontPrimaryGenerateOverride = !(value == "off" || value == "false" || value == "0");
            } else if ((arg == "--wavefront-trace" || arg == "--wavefront-trace-wrapper") && i + 1 < argc) {
                const std::string_view value(argv[++i]);
                wavefrontTraceOverride = !(value == "off" || value == "false" || value == "0");
            } else if ((arg == "--wavefront-shade" || arg == "--wavefront-shade-compute") && i + 1 < argc) {
                const std::string_view value(argv[++i]);
                wavefrontShadeOverride = !(value == "off" || value == "false" || value == "0");
            } else if ((arg == "--wavefront-shadow-trace" || arg == "--wavefront-shadow") && i + 1 < argc) {
                const std::string_view value(argv[++i]);
                wavefrontShadowTraceOverride = !(value == "off" || value == "false" || value == "0");
            } else if ((arg == "--wavefront-compact" || arg == "--wavefront-queue-compact") && i + 1 < argc) {
                const std::string_view value(argv[++i]);
                wavefrontCompactOverride = !(value == "off" || value == "false" || value == "0");
            } else if ((arg == "--wavefront-sort" || arg == "--wavefront-ray-sort") && i + 1 < argc) {
                const std::string_view value(argv[++i]);
                wavefrontSortOverride = !(value == "off" || value == "false" || value == "0");
            } else if ((arg == "--wavefront-final-output" || arg == "--wavefront-renderer") && i + 1 < argc) {
                const std::string_view value(argv[++i]);
                wavefrontFinalOutputOverride = !(value == "off" || value == "false" || value == "0");
            } else if ((arg == "--ser" || arg == "--shader-execution-reordering") && i + 1 < argc) {
                const std::string_view value(argv[++i]);
                shaderExecutionReorderingOverride = !(value == "off" || value == "false" || value == "0");
            } else if (arg == "--dof-aperture-radius" && i + 1 < argc) {
                dofApertureRadiusOverride = std::stof(argv[++i]);
            } else if ((arg == "--dof-focus-distance" || arg == "--focus-distance") && i + 1 < argc) {
                dofFocusDistanceOverride = std::stof(argv[++i]);
            } else if ((arg == "--dof-blades" || arg == "--dof-blade-count") && i + 1 < argc) {
                dofBladeCountOverride = static_cast<uint32_t>(std::stoul(argv[++i]));
            } else if ((arg == "--dof-bokeh-rotation" || arg == "--bokeh-rotation") && i + 1 < argc) {
                dofBokehRotationOverride = std::stof(argv[++i]);
            } else if ((arg == "--motion-blur" || arg == "--rt-motion-blur") && i + 1 < argc) {
                const std::string_view value(argv[++i]);
                motionBlurOverride = !(value == "off" || value == "false" || value == "0");
            } else if (arg == "--motion-blur-shutter-open" && i + 1 < argc) {
                motionBlurShutterOpenOverride = std::stof(argv[++i]);
            } else if (arg == "--motion-blur-shutter-close" && i + 1 < argc) {
                motionBlurShutterCloseOverride = std::stof(argv[++i]);
            } else if ((arg == "--homogeneous-volume" || arg == "--volume") && i + 1 < argc) {
                const std::string_view value(argv[++i]);
                homogeneousVolumeOverride = !(value == "off" || value == "false" || value == "0");
            } else if ((arg == "--volume-scattering" || arg == "--homogeneous-volume-scattering") && i + 1 < argc) {
                homogeneousVolumeScatteringOverride = std::stof(argv[++i]);
            } else if ((arg == "--volume-absorption" || arg == "--homogeneous-volume-absorption") && i + 1 < argc) {
                homogeneousVolumeAbsorptionOverride = std::stof(argv[++i]);
            } else if ((arg == "--volume-anisotropy" || arg == "--homogeneous-volume-anisotropy") && i + 1 < argc) {
                homogeneousVolumeAnisotropyOverride = std::stof(argv[++i]);
            } else if ((arg == "--mnee-caustics" || arg == "--caustics") && i + 1 < argc) {
                const std::string_view value(argv[++i]);
                mneeCausticsOverride = !(value == "off" || value == "false" || value == "0");
            } else if (arg == "--wavefront-validation") {
                wavefrontValidationMode = true;
            } else if (arg == "--taa-motion-feedback" && i + 1 < argc) {
                taaMotionFeedbackOverride = std::stof(argv[++i]);
            } else if (arg == "--taa-reactive-feedback" && i + 1 < argc) {
                taaReactiveFeedbackOverride = std::stof(argv[++i]);
            } else if ((arg == "--spp" || arg == "--samples-per-pixel") && i + 1 < argc) {
                samplesPerPixelOverride = static_cast<uint32_t>(std::stoul(argv[++i]));
            } else if ((arg == "--spp-limit" || arg == "--limit-spp") && i + 1 < argc) {
                const std::string_view value(argv[++i]);
                sppLimiterOverride = !(value == "off" || value == "false" || value == "0");
            } else if (arg == "--validation-camera-motion") {
                validationCameraMotion = true;
            } else if (arg == "--validation-object-motion") {
                validationObjectMotion = true;
            } else if (arg == "--headless") {
                diagConfig.headless = true;
            } else if (arg == "--warmup-frames" && i + 1 < argc) {
                diagConfig.warmupFrames = static_cast<uint32_t>(std::stoul(argv[++i]));
            } else if (arg == "--fixed-seed" && i + 1 < argc) {
                diagConfig.fixedSeed = static_cast<uint32_t>(std::stoul(argv[++i]));
            } else if (arg == "--profile") {
                diagConfig.profile = true;
            } else if (arg == "--profile-json" && i + 1 < argc) {
                diagConfig.profileJsonPath = std::filesystem::path(argv[++i]);
            } else if (arg == "--dump-rendergraph" && i + 1 < argc) {
                diagConfig.dumpRenderGraphPath = std::filesystem::path(argv[++i]);
            } else if (arg == "--dump-rendergraph-dot" && i + 1 < argc) {
                dumpRenderGraphDot = true;
                dotOutputPath = std::filesystem::path(argv[++i]);
            } else if (arg == "--save-debug-views" && i + 1 < argc) {
                diagConfig.saveDebugViewsDir = std::filesystem::path(argv[++i]);
            } else if (arg == "--save-frame-sequence" && i + 1 < argc) {
                diagConfig.saveFrameSequenceDir = std::filesystem::path(argv[++i]);
            } else if (arg == "--sequence-views" && i + 1 < argc) {
                sequenceViewNames = splitCsv(argv[++i]);
            } else if (arg == "--sequence-start-frame" && i + 1 < argc) {
                diagConfig.sequenceStartFrame = static_cast<uint32_t>(std::stoul(argv[++i]));
            } else if (arg == "--sequence-frame-count" && i + 1 < argc) {
                diagConfig.sequenceFrameCount = static_cast<uint32_t>(std::stoul(argv[++i]));
            } else if (arg == "--sequence-step" && i + 1 < argc) {
                diagConfig.sequenceStep = std::max(1u, static_cast<uint32_t>(std::stoul(argv[++i])));
            } else if (arg == "--capture-renderdoc" && i + 1 < argc) {
                diagConfig.captureRenderDocPath = std::filesystem::path(argv[++i]);
            } else if (arg == "--capture-frame" && i + 1 < argc) {
                diagConfig.captureFrame = static_cast<uint32_t>(std::stoul(argv[++i]));
            } else if (arg == "--renderdoc-dll" && i + 1 < argc) {
#ifdef RTV_HAS_RENDERDOC
                rdocDllPathOverride = std::filesystem::path(argv[++i]);
#else
                ++i;
                std::cerr << "Warning: --renderdoc-dll ignored because this build was not configured with RENDERDOC_SDK_DIR.\n";
#endif
            } else if (arg == "--make-debug-package" && i + 1 < argc) {
                diagConfig.makeDebugPackageDir = std::filesystem::path(argv[++i]);
            } else if (arg == "--disable-async-compute") {
                diagConfig.disableAsyncCompute = true;
            } else if (arg == "--single-queue-fallback") {
                diagConfig.singleQueueFallback = true;
            } else if (arg == "--disable-resource-aliasing") {
                diagConfig.disableResourceAliasing = true;
            } else if (arg == "--run-validation-suite") {
                diagConfig.runValidationSuite = true;
            } else if (arg == "--validation-output" && i + 1 < argc) {
                diagConfig.validationOutputDir = std::filesystem::path(argv[++i]);
            } else if (arg == "--update-baseline") {
                updateBaseline = true;
            } else if (arg == "--check-baseline") {
                checkBaseline = true;
            } else if (arg == "--baseline-dir" && i + 1 < argc) {
                baselineRoot = std::filesystem::path(argv[++i]);
            } else if (arg == "--dump-memory" && i + 1 < argc) {
                dumpMemoryPath = std::filesystem::path(argv[++i]);
            } else if (arg == "--dump-frame-timeline" && i + 1 < argc) {
                dumpFrameTimelinePath = std::filesystem::path(argv[++i]);
            } else if (arg == "--dump-resource-lifetimes" && i + 1 < argc) {
                dumpResourceLifetimesPath = std::filesystem::path(argv[++i]);
            } else if (arg == "--dump-shader-report" && i + 1 < argc) {
                dumpShaderReportPath = std::filesystem::path(argv[++i]);
            } else if (arg == "--dump-bindings" && i + 1 < argc) {
                dumpBindingsPath = std::filesystem::path(argv[++i]);
            } else if (arg == "--crash-dump-package" && i + 1 < argc) {
                crashDumpPackageDir = std::filesystem::path(argv[++i]);
            } else if (arg == "--validate-gpu-labels") {
                validateGpuLabels = true;
            } else if (arg == "--check-budget" && i + 1 < argc) {
                checkBudgetPath = std::filesystem::path(argv[++i]);
            } else if (arg == "--descriptor-lifetime-stress" && i + 1 < argc) {
                descriptorLifetimeStressPath = std::filesystem::path(argv[++i]);
            } else if (arg == "--descriptor-lifetime-stress-cycles" && i + 1 < argc) {
                descriptorLifetimeStressCycles = std::max(1u, static_cast<uint32_t>(std::stoul(argv[++i])));
            } else if (arg == "--descriptor-lifetime-stress-frames" && i + 1 < argc) {
                descriptorLifetimeStressFrames = std::max(1u, static_cast<uint32_t>(std::stoul(argv[++i])));
            } else if (arg == "--shader-hot-reload-report") {
                shaderHotReloadReport = true;
            } else if (arg == "--disable-pass" && i + 1 < argc) {
                disabledPasses.push_back(argv[++i]);
            } else if (arg == "--camera" && i + 1 < argc) {
                cameraName = std::string(argv[++i]);
            } else if (arg == "--frame-index" && i + 1 < argc) {
                frameIndex = static_cast<uint32_t>(std::stoul(argv[++i]));
            }
        }

        if (compareProfileOldPath.has_value() || compareProfileNewPath.has_value()) {
            if (!compareProfileOldPath.has_value() || !compareProfileNewPath.has_value()) {
                throw std::runtime_error("--compare-profile requires old.json and new.json");
            }
            return rtv::compareProfileCommand(*compareProfileOldPath, *compareProfileNewPath);
        }
        if (compareImageBaselinePath.has_value() || compareImageCurrentPath.has_value()) {
            if (!compareImageBaselinePath.has_value() || !compareImageCurrentPath.has_value()) {
                throw std::runtime_error("--compare-image requires baseline.png and current.png");
            }
            return rtv::compareImageCommand(*compareImageBaselinePath, *compareImageCurrentPath, compareImageOutputPath);
        }
        if (compareImageSequenceBaselinePath.has_value() || compareImageSequenceCurrentPath.has_value()) {
            if (!compareImageSequenceBaselinePath.has_value() || !compareImageSequenceCurrentPath.has_value()) {
                throw std::runtime_error("--compare-image-sequence requires baseline_dir and current_dir");
            }
            return rtv::compareImageSequenceCommand(
                *compareImageSequenceBaselinePath,
                *compareImageSequenceCurrentPath,
                compareImageOutputPath,
                sequenceViewNames);
        }

        for (const std::string& viewName : sequenceViewNames) {
            diagConfig.sequenceViews.push_back(rtv::parseRendererDebugView(viewName));
        }

        if (diagConfig.headless && maxFrames == 0) {
            maxFrames = diagConfig.totalFrames;
        }
        if (maxFrames != 0) {
            diagConfig.totalFrames = maxFrames;
        }
        diagConfig.wavefrontValidationMode = wavefrontValidationMode;

        if (diagConfig.runValidationSuite) {
            rtv::HeadlessDiagnostics diag(diagConfig);
            const auto summary = diag.runValidationSuite();
            std::cout << "Validation suite: " << summary.totalPass << " passed, "
                      << summary.totalFail << " failed\n";
            return summary.totalFail > 0 ? 1 : 0;
        }

        const bool baselineMode = updateBaseline || checkBaseline;
        const bool needsProfile =
            diagConfig.profile ||
            diagConfig.profileJsonPath.has_value() ||
            baselineMode ||
            dumpMemoryPath.has_value() ||
            dumpFrameTimelinePath.has_value() ||
            checkBudgetPath.has_value() ||
            crashDumpPackageDir.has_value() ||
            wavefrontValidationMode;
        const bool needsRenderGraph =
            baselineMode ||
            dumpFrameTimelinePath.has_value() ||
            dumpResourceLifetimesPath.has_value() ||
            dumpBindingsPath.has_value() ||
            validateGpuLabels ||
            crashDumpPackageDir.has_value();
        const bool needsDebugViews = baselineMode || crashDumpPackageDir.has_value();
        if (needsProfile) {
            diagConfig.profile = true;
        }
        const std::filesystem::path diagnosticSourcePath = scenePath.value_or(gltfPath.value_or("scene"));
        const std::filesystem::path artifactBase =
            rtv::defaultDiagnosticArtifactDir(diagnosticSourcePath, "current");
        if (needsProfile && !diagConfig.profileJsonPath.has_value()) {
            diagConfig.profileJsonPath = artifactBase / "profile.json";
        }
        if (needsRenderGraph && !diagConfig.dumpRenderGraphPath.has_value()) {
            diagConfig.dumpRenderGraphPath = artifactBase / "rendergraph.json";
        }
        if (needsDebugViews && !diagConfig.saveDebugViewsDir.has_value()) {
            diagConfig.saveDebugViewsDir = artifactBase / "debug_views";
        }
        if (shaderHotReloadReport && !dumpShaderReportPath.has_value()) {
            dumpShaderReportPath = artifactBase / "shader_hot_reload_report.json";
        }
        if (checkBaseline) {
            const rtv::BaselinePaths paths = rtv::baselinePathsFor(diagnosticSourcePath, baselineRoot);
            if (std::filesystem::exists(paths.frameSequence) && !diagConfig.saveFrameSequenceDir.has_value()) {
                diagConfig.saveFrameSequenceDir = artifactBase / "frame_sequence";
            }
        }
        if (frameIndex.has_value() && !diagConfig.fixedSeed.has_value()) {
            diagConfig.fixedSeed = *frameIndex;
        }

        if (diagConfig.headless && !scenePath.has_value() && !gltfPath.has_value()) {
            throw std::runtime_error("--headless requires --scene <path> or --gltf <path>");
        }
        if (diagConfig.saveFrameSequenceDir.has_value() && !diagConfig.headless) {
            throw std::runtime_error("--save-frame-sequence requires --headless");
        }
        if (descriptorLifetimeStressPath.has_value() && !diagConfig.headless) {
            throw std::runtime_error("--descriptor-lifetime-stress requires --headless");
        }

#ifdef RTV_HAS_RENDERDOC
        if (diagConfig.captureRenderDocPath.has_value()) {
            rdocCaptureRequested = true;
            rdocCapturePath = *diagConfig.captureRenderDocPath;
            rdocCaptureFrame = std::max(1u, diagConfig.captureFrame);
            initRenderDoc();
        }
#else
        if (diagConfig.captureRenderDocPath.has_value()) {
            std::cerr << "Warning: RenderDoc capture requested, but this build was not configured with RENDERDOC_SDK_DIR.\n";
        }
#endif

        rtv::Application app(debugView, gltfPath, hdrPath, scenePath,
            denoiserOverride, restirModeOverride, renderPresetOverride, restirGiOverride,
            opacityMicromapOverride,
            opacityMicromapSubdivisionOverride,
            debugViewProvided, validationCameraMotion, validationObjectMotion,
            diagConfig.headless,
            diagConfig.disableAsyncCompute,
            diagConfig.singleQueueFallback,
            diagConfig.disableResourceAliasing);

        if (auto* renderer = app.pathTracer()) {
            renderer->setRayTracingDiagnosticCountersEnabled(diagConfig.profile);
            auto lower = [](std::string value) {
                std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
                    return static_cast<char>(std::tolower(ch));
                });
                return value;
            };
            if (diagConfig.fixedSeed.has_value()) {
                rtv::RendererSettings settings = renderer->settings();
                settings.fixedSeed = diagConfig.fixedSeed;
                renderer->applySettings(settings);
            }
            if (taaMotionFeedbackOverride.has_value() || taaReactiveFeedbackOverride.has_value()) {
                rtv::RendererSettings settings = renderer->settings();
                if (taaMotionFeedbackOverride.has_value()) {
                    settings.taaMotionFeedback = *taaMotionFeedbackOverride;
                }
                if (taaReactiveFeedbackOverride.has_value()) {
                    settings.taaReactiveFeedback = *taaReactiveFeedbackOverride;
                }
                settings.renderPreset = rtv::RenderPreset::Custom;
                renderer->applySettings(settings);
            }
            if (samplesPerPixelOverride.has_value() || sppLimiterOverride.has_value()) {
                rtv::RendererSettings settings = renderer->settings();
                if (samplesPerPixelOverride.has_value()) {
                    settings.samplesPerPixel = *samplesPerPixelOverride;
                }
                if (sppLimiterOverride.has_value()) {
                    settings.limitSamplesPerPixel = *sppLimiterOverride;
                }
                settings.renderPreset = rtv::RenderPreset::Custom;
                renderer->applySettings(settings);
            }
            if (wavefrontQueuesOverride.has_value()) {
                rtv::RendererSettings settings = renderer->settings();
                settings.wavefrontQueuesEnabled = *wavefrontQueuesOverride;
                settings.renderPreset = rtv::RenderPreset::Custom;
                renderer->applySettings(settings);
            }
            if (wavefrontPrimaryGenerateOverride.has_value()) {
                rtv::RendererSettings settings = renderer->settings();
                settings.wavefrontPrimaryGenerateEnabled = *wavefrontPrimaryGenerateOverride;
                if (*wavefrontPrimaryGenerateOverride) {
                    settings.wavefrontQueuesEnabled = true;
                }
                settings.renderPreset = rtv::RenderPreset::Custom;
                renderer->applySettings(settings);
            }
            if (wavefrontTraceOverride.has_value()) {
                rtv::RendererSettings settings = renderer->settings();
                settings.wavefrontTraceEnabled = *wavefrontTraceOverride;
                if (*wavefrontTraceOverride) {
                    settings.wavefrontPrimaryGenerateEnabled = true;
                    settings.wavefrontQueuesEnabled = true;
                }
                settings.renderPreset = rtv::RenderPreset::Custom;
                renderer->applySettings(settings);
            }
            if (wavefrontShadeOverride.has_value()) {
                rtv::RendererSettings settings = renderer->settings();
                settings.wavefrontShadeEnabled = *wavefrontShadeOverride;
                if (*wavefrontShadeOverride) {
                    settings.wavefrontTraceEnabled = true;
                    settings.wavefrontPrimaryGenerateEnabled = true;
                    settings.wavefrontQueuesEnabled = true;
                }
                settings.renderPreset = rtv::RenderPreset::Custom;
                renderer->applySettings(settings);
            }
            if (wavefrontShadowTraceOverride.has_value()) {
                rtv::RendererSettings settings = renderer->settings();
                settings.wavefrontShadowTraceEnabled = *wavefrontShadowTraceOverride;
                if (*wavefrontShadowTraceOverride) {
                    settings.wavefrontShadeEnabled = true;
                    settings.wavefrontTraceEnabled = true;
                    settings.wavefrontPrimaryGenerateEnabled = true;
                    settings.wavefrontQueuesEnabled = true;
                }
                settings.renderPreset = rtv::RenderPreset::Custom;
                renderer->applySettings(settings);
            }
            if (wavefrontCompactOverride.has_value()) {
                rtv::RendererSettings settings = renderer->settings();
                settings.wavefrontCompactEnabled = *wavefrontCompactOverride;
                if (*wavefrontCompactOverride) {
                    settings.wavefrontShadeEnabled = true;
                    settings.wavefrontTraceEnabled = true;
                    settings.wavefrontPrimaryGenerateEnabled = true;
                    settings.wavefrontQueuesEnabled = true;
                }
                settings.renderPreset = rtv::RenderPreset::Custom;
                renderer->applySettings(settings);
            }
            if (wavefrontSortOverride.has_value()) {
                rtv::RendererSettings settings = renderer->settings();
                settings.wavefrontSortEnabled = *wavefrontSortOverride;
                if (*wavefrontSortOverride) {
                    settings.wavefrontCompactEnabled = true;
                    settings.wavefrontShadeEnabled = true;
                    settings.wavefrontTraceEnabled = true;
                    settings.wavefrontPrimaryGenerateEnabled = true;
                    settings.wavefrontQueuesEnabled = true;
                }
                settings.renderPreset = rtv::RenderPreset::Custom;
                renderer->applySettings(settings);
            }
            if (wavefrontFinalOutputOverride.has_value()) {
                rtv::RendererSettings settings = renderer->settings();
                settings.wavefrontFinalOutputEnabled = *wavefrontFinalOutputOverride;
                if (*wavefrontFinalOutputOverride) {
                    settings.wavefrontShadowTraceEnabled = true;
                    settings.wavefrontCompactEnabled = true;
                    settings.wavefrontShadeEnabled = true;
                    settings.wavefrontTraceEnabled = true;
                    settings.wavefrontPrimaryGenerateEnabled = true;
                    settings.wavefrontQueuesEnabled = true;
                }
                settings.renderPreset = rtv::RenderPreset::Custom;
                renderer->applySettings(settings);
            }
            if (shaderExecutionReorderingOverride.has_value()) {
                rtv::RendererSettings settings = renderer->settings();
                settings.shaderExecutionReorderingEnabled = *shaderExecutionReorderingOverride;
                settings.renderPreset = rtv::RenderPreset::Custom;
                renderer->applySettings(settings);
            }
            if (dofApertureRadiusOverride.has_value() || dofFocusDistanceOverride.has_value() ||
                dofBladeCountOverride.has_value() || dofBokehRotationOverride.has_value()) {
                rtv::RendererSettings settings = renderer->settings();
                if (dofApertureRadiusOverride.has_value()) {
                    settings.dofApertureRadius = *dofApertureRadiusOverride;
                }
                if (dofFocusDistanceOverride.has_value()) {
                    settings.dofFocusDistance = *dofFocusDistanceOverride;
                }
                if (dofBladeCountOverride.has_value()) {
                    settings.dofBladeCount = *dofBladeCountOverride;
                }
                if (dofBokehRotationOverride.has_value()) {
                    settings.dofBokehRotation = *dofBokehRotationOverride;
                }
                settings.renderPreset = rtv::RenderPreset::Custom;
                renderer->applySettings(settings);
            }
            if (motionBlurOverride.has_value() || motionBlurShutterOpenOverride.has_value() || motionBlurShutterCloseOverride.has_value()) {
                rtv::RendererSettings settings = renderer->settings();
                if (motionBlurOverride.has_value()) {
                    settings.motionBlurEnabled = *motionBlurOverride;
                }
                if (motionBlurShutterOpenOverride.has_value()) {
                    settings.motionBlurShutterOpen = *motionBlurShutterOpenOverride;
                }
                if (motionBlurShutterCloseOverride.has_value()) {
                    settings.motionBlurShutterClose = *motionBlurShutterCloseOverride;
                }
                settings.renderPreset = rtv::RenderPreset::Custom;
                renderer->applySettings(settings);
            }
            if (homogeneousVolumeOverride.has_value() ||
                homogeneousVolumeScatteringOverride.has_value() ||
                homogeneousVolumeAbsorptionOverride.has_value() ||
                homogeneousVolumeAnisotropyOverride.has_value()) {
                rtv::RendererSettings settings = renderer->settings();
                if (homogeneousVolumeOverride.has_value()) {
                    settings.homogeneousVolumeEnabled = *homogeneousVolumeOverride;
                } else {
                    settings.homogeneousVolumeEnabled = true;
                }
                if (homogeneousVolumeScatteringOverride.has_value()) {
                    settings.homogeneousVolumeScattering = *homogeneousVolumeScatteringOverride;
                }
                if (homogeneousVolumeAbsorptionOverride.has_value()) {
                    settings.homogeneousVolumeAbsorption = *homogeneousVolumeAbsorptionOverride;
                }
                if (homogeneousVolumeAnisotropyOverride.has_value()) {
                    settings.homogeneousVolumeAnisotropy = *homogeneousVolumeAnisotropyOverride;
                }
                settings.renderPreset = rtv::RenderPreset::Custom;
                renderer->applySettings(settings);
            }
            if (mneeCausticsOverride.has_value()) {
                rtv::RendererSettings settings = renderer->settings();
                settings.mneeCausticsEnabled = *mneeCausticsOverride;
                settings.renderPreset = rtv::RenderPreset::Custom;
                renderer->applySettings(settings);
            }
            if (wavefrontValidationMode) {
                rtv::RendererSettings settings = renderer->settings();
                settings.restirMode = rtv::RestirMode::ClassicNee;
                settings.restirGiEnabled = false;
                settings.wavefrontQueuesEnabled = true;
                settings.wavefrontPrimaryGenerateEnabled = true;
                settings.wavefrontTraceEnabled = true;
                settings.wavefrontShadeEnabled = true;
                settings.wavefrontShadowTraceEnabled = true;
                settings.wavefrontCompactEnabled = true;
                settings.renderPreset = rtv::RenderPreset::Custom;
                renderer->applySettings(settings);
            }
            if (!disabledPasses.empty()) {
                rtv::RendererSettings settings = renderer->settings();
                for (const std::string& pass : disabledPasses) {
                    const std::string name = lower(pass);
                    if (name == "denoiser" || name == "temporaldenoiser") {
                        settings.denoiserEnabled = false;
                    } else if (name == "taa" || name == "tsr") {
                        settings.taaEnabled = false;
                    } else if (name == "restir") {
                        settings.restirMode = rtv::RestirMode::ClassicNee;
                        settings.restirGiEnabled = false;
                    } else if (name == "restirdi" || name == "restirspatial") {
                        settings.restirMode = rtv::RestirMode::ClassicNee;
                    } else if (name == "restirgi") {
                        settings.restirGiEnabled = false;
                    } else if (name == "autoexposure") {
                        settings.autoExposureEnabled = false;
                    } else {
                        std::cerr << "Warning: unknown --disable-pass value: " << pass << "\n";
                    }
                }
                renderer->applySettings(settings);
            }
            if (cameraName.has_value()) {
                if (!app.applyNamedCamera(*cameraName)) {
                    std::cerr << "Warning: --camera " << *cameraName
                              << " did not match a scene camera or built-in diagnostic camera; using the active scene camera.\n";
                }
            }
            if (diagConfig.dumpRenderGraphPath.has_value()) {
                renderer->setDumpRenderGraphPath(diagConfig.dumpRenderGraphPath);
            }
            if (dumpRenderGraphDot && dotOutputPath.has_value()) {
                renderer->setDumpRenderGraphDotPath(dotOutputPath);
            }
            if (diagConfig.disableAsyncCompute || diagConfig.singleQueueFallback) {
                rtv::RendererSettings settings = renderer->settings();
                settings.adaptiveQualityMode = rtv::AdaptiveQualityMode::Off;
                renderer->applySettings(settings);
            }
        }

        rtv::HeadlessDiagnostics diag(diagConfig);
        if (diagConfig.makeDebugPackageDir.has_value() || crashDumpPackageDir.has_value()) {
            diag.captureStdout();
        }

#ifdef RTV_HAS_RENDERDOC
        bool rdocCaptureStarted = false;
        bool rdocCaptureFinished = false;
        if (rdocCaptureRequested && rdocApi != nullptr) {
            const std::filesystem::path absoluteCapturePath = std::filesystem::absolute(rdocCapturePath);
            const auto captureDir = absoluteCapturePath.parent_path();
            if (!captureDir.empty()) {
                std::filesystem::create_directories(captureDir);
            }
            rdocCaptureTemplate = absoluteCapturePath.string();
            rdocApi->SetCaptureFilePathTemplate(rdocCaptureTemplate.c_str());
            app.setFrameCaptureCallbacks(
                [&](uint32_t frameNumber) {
                    if (!rdocCaptureStarted && frameNumber == rdocCaptureFrame) {
                        rdocApi->StartFrameCapture(nullptr, nullptr);
                        rdocCaptureStarted = true;
                        std::cout << "RenderDoc capture started at frame " << frameNumber << "\n";
                    }
                },
                [&](uint32_t frameNumber) {
                    if (rdocCaptureStarted && !rdocCaptureFinished && frameNumber == rdocCaptureFrame) {
                        const uint32_t captureSaved = rdocApi->EndFrameCapture(nullptr, nullptr);
                        rdocCaptureFinished = true;
                        if (captureSaved != 0u) {
                            std::cout << "RenderDoc capture saved to template: " << rdocCaptureTemplate << "\n";
                        } else {
                            std::cerr << "Warning: RenderDoc capture ended but was not saved.\n";
                        }
                    }
                });
        }
#endif

        if (diagConfig.headless) {
            app.runHeadless(diagConfig.warmupFrames, maxFrames);
        } else {
            app.run(maxFrames);
        }

        bool descriptorLifetimeStressPassed = true;
        if (descriptorLifetimeStressPath.has_value()) {
            descriptorLifetimeStressPassed = app.runDescriptorLifetimeStress(
                *descriptorLifetimeStressPath,
                descriptorLifetimeStressCycles,
                descriptorLifetimeStressFrames);
        }

#ifdef RTV_HAS_RENDERDOC
        if (rdocCaptureRequested && rdocApi != nullptr && !rdocCaptureFinished) {
            std::cerr << "Warning: RenderDoc capture frame " << rdocCaptureFrame
                      << " was not reached before shutdown.\n";
        }
#endif

        if (diagConfig.profile || diagConfig.saveDebugViewsDir.has_value() ||
            diagConfig.saveFrameSequenceDir.has_value() ||
            diagConfig.dumpRenderGraphPath.has_value() || diagConfig.makeDebugPackageDir.has_value() ||
            dumpMemoryPath.has_value() || dumpFrameTimelinePath.has_value() ||
            dumpResourceLifetimesPath.has_value() || dumpBindingsPath.has_value() ||
            dumpShaderReportPath.has_value() || crashDumpPackageDir.has_value() ||
            baselineMode || validateGpuLabels || checkBudgetPath.has_value()) {
            diag.run(app);

            if (diagConfig.profileJsonPath.has_value()) {
                diag.writeProfileJson(*diagConfig.profileJsonPath);
            }
            if (diagConfig.saveDebugViewsDir.has_value()) {
                diag.exportDebugViews(app, *diagConfig.saveDebugViewsDir);
            }
            if (diagConfig.saveFrameSequenceDir.has_value()) {
                diag.exportFrameSequence(app, *diagConfig.saveFrameSequenceDir);
            }
            if (diagConfig.makeDebugPackageDir.has_value()) {
                diag.makeDebugPackage(app, *diagConfig.makeDebugPackageDir, diagnosticSourcePath);
            }
            if (crashDumpPackageDir.has_value()) {
                diag.makeDebugPackage(app, *crashDumpPackageDir, diagnosticSourcePath);
            }
        }

        std::string capturedLog;
        if (crashDumpPackageDir.has_value()) {
            capturedLog = diag.releaseStdout();
        }

        int finalExitCode = 0;
        if (!descriptorLifetimeStressPassed) {
            finalExitCode = 1;
        }
        if (dumpMemoryPath.has_value()) {
            rtv::writeMemoryReport(*dumpMemoryPath, diag.profileReport(), diagConfig.dumpRenderGraphPath);
        }
        if (dumpFrameTimelinePath.has_value()) {
            rtv::writeFrameTimeline(*dumpFrameTimelinePath, diag.profileReport(), diagConfig.dumpRenderGraphPath);
        }
        if (dumpResourceLifetimesPath.has_value()) {
            rtv::writeResourceLifetimes(*dumpResourceLifetimesPath, diagConfig.dumpRenderGraphPath);
        }
        if (dumpShaderReportPath.has_value()) {
            const auto shaderDir = std::filesystem::current_path() / "shaders";
            const auto shaderOutDir = std::filesystem::current_path() / "build" / "shaders";
            rtv::writeShaderReport(*dumpShaderReportPath, shaderDir, shaderOutDir);
        }
        if (dumpBindingsPath.has_value()) {
            rtv::writeBindingsReport(*dumpBindingsPath, diagConfig.dumpRenderGraphPath);
        }
        if (validateGpuLabels) {
            finalExitCode = std::max(finalExitCode, rtv::validateGpuLabels(diagConfig.dumpRenderGraphPath));
        }
        if (checkBudgetPath.has_value()) {
            finalExitCode = std::max(finalExitCode, rtv::checkBudget(*checkBudgetPath, diag.profileReport()));
        }
        if (baselineMode) {
            const rtv::BaselinePaths paths = rtv::baselinePathsFor(diagnosticSourcePath, baselineRoot);
            if (!diagConfig.profileJsonPath.has_value() ||
                !diagConfig.dumpRenderGraphPath.has_value() ||
                !diagConfig.saveDebugViewsDir.has_value()) {
                throw std::runtime_error("Baseline mode requires profile, render graph, and debug view artifacts");
            }
            if (updateBaseline) {
                rtv::updateBaseline(paths, *diagConfig.profileJsonPath, *diagConfig.dumpRenderGraphPath,
                    *diagConfig.saveDebugViewsDir, diagConfig.saveFrameSequenceDir);
            }
            if (checkBaseline) {
                finalExitCode = std::max(finalExitCode,
                    rtv::checkBaseline(paths, *diagConfig.profileJsonPath, *diagConfig.dumpRenderGraphPath, *diagConfig.saveDebugViewsDir));
            }
        }
        if (crashDumpPackageDir.has_value()) {
            rtv::writeCrashDumpPackage(
                *crashDumpPackageDir,
                diagnosticSourcePath,
                diagConfig.profileJsonPath,
                diagConfig.dumpRenderGraphPath,
                diagConfig.saveDebugViewsDir,
                capturedLog);
        }

        return finalExitCode;
    } catch (const std::exception& error) {
        std::cerr << "Fatal error: " << error.what() << '\n';
        return 1;
    }
}
