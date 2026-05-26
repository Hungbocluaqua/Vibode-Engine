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
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>
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

static void initRenderDoc() {
    HMODULE mod = GetModuleHandleA("renderdoc.dll");
    if (mod == nullptr) {
        std::cerr << "Warning: RenderDoc DLL not loaded. Run from RenderDoc or inject the layer.\n";
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
        bool validationCameraMotion = false;

        rtv::HeadlessDiagnosticsConfig diagConfig;
        bool dumpRenderGraphDot = false;
        std::optional<std::filesystem::path> dotOutputPath;
        std::optional<std::filesystem::path> compareProfileOldPath;
        std::optional<std::filesystem::path> compareProfileNewPath;
        std::optional<std::filesystem::path> compareImageBaselinePath;
        std::optional<std::filesystem::path> compareImageCurrentPath;
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
        bool validateGpuLabels = false;
        bool shaderHotReloadReport = false;
        std::optional<std::string> cameraName;
        std::optional<uint32_t> frameIndex;
        std::vector<std::string> disabledPasses;

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
            } else if (arg == "--validation-camera-motion") {
                validationCameraMotion = true;
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
            } else if (arg == "--capture-renderdoc" && i + 1 < argc) {
                diagConfig.captureRenderDocPath = std::filesystem::path(argv[++i]);
            } else if (arg == "--capture-frame" && i + 1 < argc) {
                diagConfig.captureFrame = static_cast<uint32_t>(std::stoul(argv[++i]));
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

        if (diagConfig.headless && maxFrames == 0) {
            maxFrames = diagConfig.totalFrames;
        }
        if (maxFrames != 0) {
            diagConfig.totalFrames = maxFrames;
        }

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
            crashDumpPackageDir.has_value();
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
        const std::filesystem::path artifactBase =
            rtv::defaultDiagnosticArtifactDir(scenePath.value_or("scene"), "current");
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
        if (frameIndex.has_value() && !diagConfig.fixedSeed.has_value()) {
            diagConfig.fixedSeed = *frameIndex;
        }

        if (diagConfig.headless && !scenePath.has_value()) {
            throw std::runtime_error("--headless requires --scene <path>");
        }

        rtv::Application app(debugView, gltfPath, hdrPath, scenePath,
            denoiserOverride, restirModeOverride, debugViewProvided, validationCameraMotion,
            diagConfig.headless);

        if (auto* renderer = app.pathTracer()) {
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
            if (!disabledPasses.empty()) {
                rtv::RendererSettings settings = renderer->settings();
                for (const std::string& pass : disabledPasses) {
                    const std::string name = lower(pass);
                    if (name == "denoiser" || name == "temporaldenoiser") {
                        settings.denoiserEnabled = false;
                    } else if (name == "taa" || name == "tsr") {
                        settings.taaEnabled = false;
                    } else if (name == "restir" || name == "restirdi" || name == "restirgi" || name == "restirspatial") {
                        settings.restirMode = rtv::RestirMode::ClassicNee;
                    } else if (name == "autoexposure") {
                        settings.autoExposureEnabled = false;
                    } else {
                        std::cerr << "Warning: unknown --disable-pass value: " << pass << "\n";
                    }
                }
                renderer->applySettings(settings);
            }
            if (cameraName.has_value()) {
                std::cerr << "Warning: --camera " << *cameraName
                          << " was parsed, but named camera selection is not exposed by Application yet; using the active scene camera.\n";
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
            rdocApi->SetCaptureFilePathTemplate(absoluteCapturePath.string().c_str());
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
                            std::cout << "RenderDoc capture saved to template: " << absoluteCapturePath.string() << "\n";
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

#ifdef RTV_HAS_RENDERDOC
        if (rdocCaptureRequested && rdocApi != nullptr && !rdocCaptureFinished) {
            std::cerr << "Warning: RenderDoc capture frame " << rdocCaptureFrame
                      << " was not reached before shutdown.\n";
        }
#endif

        if (diagConfig.profile || diagConfig.saveDebugViewsDir.has_value() ||
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
            if (diagConfig.makeDebugPackageDir.has_value()) {
                const std::filesystem::path scnPath = scenePath.value_or("");
                diag.makeDebugPackage(app, *diagConfig.makeDebugPackageDir, scnPath);
            }
            if (crashDumpPackageDir.has_value()) {
                const std::filesystem::path scnPath = scenePath.value_or("");
                diag.makeDebugPackage(app, *crashDumpPackageDir, scnPath);
            }
        }

        std::string capturedLog;
        if (crashDumpPackageDir.has_value()) {
            capturedLog = diag.releaseStdout();
        }

        int finalExitCode = 0;
        if (dumpMemoryPath.has_value()) {
            rtv::writeMemoryReport(*dumpMemoryPath, diag.profileReport());
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
            const rtv::BaselinePaths paths = rtv::baselinePathsFor(scenePath.value_or("scene"), baselineRoot);
            if (!diagConfig.profileJsonPath.has_value() ||
                !diagConfig.dumpRenderGraphPath.has_value() ||
                !diagConfig.saveDebugViewsDir.has_value()) {
                throw std::runtime_error("Baseline mode requires profile, render graph, and debug view artifacts");
            }
            if (updateBaseline) {
                rtv::updateBaseline(paths, *diagConfig.profileJsonPath, *diagConfig.dumpRenderGraphPath, *diagConfig.saveDebugViewsDir);
            }
            if (checkBaseline) {
                finalExitCode = std::max(finalExitCode,
                    rtv::checkBaseline(paths, *diagConfig.profileJsonPath, *diagConfig.dumpRenderGraphPath, *diagConfig.saveDebugViewsDir));
            }
        }
        if (crashDumpPackageDir.has_value()) {
            rtv::writeCrashDumpPackage(
                *crashDumpPackageDir,
                scenePath.value_or(""),
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
