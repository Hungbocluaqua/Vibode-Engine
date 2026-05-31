#include "rtv/Application.h"

#include "rtv/CommandSystem.h"
#include "rtv/BufferUploader.h"
#include "rtv/FileDialog.h"
#include "rtv/GltfLoader.h"
#include "rtv/PathTracerRenderer.h"
#include "rtv/PipelineDemo.h"
#include "rtv/ResourceAllocator.h"
#include "rtv/ResourceDemo.h"
#include "rtv/SceneOperations.h"
#include "rtv/SceneUpdateRouter.h"
#include "rtv/SunController.h"
#include "rtv/Swapchain.h"
#include "rtv/UiOverlay.h"
#include "rtv/UploadContext.h"
#include "rtv/VulkanContext.h"

#include <Volk/volk.h>
#include <GLFW/glfw3.h>

#include <nlohmann/json.hpp>

#include <chrono>
#include <algorithm>
#include <cmath>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string_view>
#include <utility>

#include <glm/glm.hpp>
#include <glm/gtc/constants.hpp>
#include <glm/gtc/quaternion.hpp>

#if defined(_WIN32)
#include <Windows.h>
#endif

namespace rtv {

namespace {
constexpr int initialWidth = 1280;
constexpr int initialHeight = 720;
constexpr uint64_t largeSceneTriangleThreshold = 1'000'000ull;
constexpr float defaultMaxFrameDeltaSeconds = 1.0f / 30.0f;

constexpr RendererDebugView intermediateViews[] = {
    RendererDebugView::Beauty,
    RendererDebugView::DirectLighting,
    RendererDebugView::IndirectLighting,
    RendererDebugView::Variance,
    RendererDebugView::Normals,
    RendererDebugView::Depth,
    RendererDebugView::MotionVectors,
};

RendererDebugView nextDebugView(RendererDebugView view) {
    const auto& views = editorDebugViews();
    for (size_t i = 0; i < views.size(); ++i) {
        if (views[i] == view) {
            return views[(i + 1u) % views.size()];
        }
    }
    return RendererDebugView::Beauty;
}

std::string normalizedCameraName(std::string_view name) {
    std::string result{name};
    std::transform(result.begin(), result.end(), result.begin(), [](unsigned char ch) {
        return ch == '_' ? '-' : static_cast<char>(std::tolower(ch));
    });
    return result;
}

float clampFrameDeltaSeconds(float rawDeltaSeconds, const PathTracerRenderer* renderer) {
    const float maxDelta = renderer != nullptr
        ? std::max(0.001f, renderer->settings().maxFrameDeltaSeconds)
        : defaultMaxFrameDeltaSeconds;
    return std::clamp(std::isfinite(rawDeltaSeconds) ? rawDeltaSeconds : 0.0f, 0.0f, maxDelta);
}

uint64_t countSceneTriangles(const SceneAsset& scene, const AssetManager& assets) {
    uint64_t triangles = 0;
    for (MeshAssetHandle handle : scene.meshes) {
        if (const MeshAsset* mesh = assets.mesh(handle)) {
            triangles += mesh->indices.size() / 3u;
        }
    }
    return triangles;
}

RendererSettings interactiveSettingsForScene(RendererSettings settings, const SceneAsset& scene, const AssetManager& assets, bool& changed) {
    changed = false;
    const uint64_t triangleCount = countSceneTriangles(scene, assets);
    if (triangleCount < largeSceneTriangleThreshold) {
        return settings;
    }

    const uint32_t cappedBounces = std::min(settings.maxBounces, 4u);
    const float cappedScale = std::min(settings.renderResolutionScale, 0.5f);
    changed = cappedBounces != settings.maxBounces ||
        std::abs(cappedScale - settings.renderResolutionScale) > 0.0001f ||
        !settings.denoiserEnabled;
    settings.maxBounces = cappedBounces;
    settings.renderResolutionScale = cappedScale;
    settings.denoiserEnabled = true;
    settings.renderPreset = RenderPreset::Custom;
    if (changed) {
        std::cout << "Large glTF scene detected (" << triangleCount
                  << " triangles); using interactive defaults: bounces="
                  << settings.maxBounces << " resolution_scale="
                  << settings.renderResolutionScale << ". Raise these in Render Settings for final-quality renders.\n";
    }
    return settings;
}

glm::mat4 entityWorldMatrix(const SceneRegistry& registry, const Entity& entity);

std::wstring widenAscii(std::string_view value) {
    return std::wstring(value.begin(), value.end());
}

void syncDocumentRenderSettings(SceneDocument& document, const RendererSettings& settings) {
    RenderSettings& render = document.renderSettings();
    render.renderPreset = settings.renderPreset;
    render.pathTracingEnabled = settings.pathTracingEnabled;
    render.cameraJitterEnabled = settings.cameraJitterEnabled;
    render.directLightingEnabled = settings.directLightingEnabled;
    render.maxBounces = settings.maxBounces;
    render.environmentDirectSamples = settings.environmentDirectSamples;
    render.toneMapper = settings.toneMapper;
    render.exposure = settings.exposure;
    render.gamma = settings.gamma;
    render.contrast = settings.contrast;
    render.saturation = settings.saturation;
    render.brightness = settings.brightness;
    render.whitePoint = settings.whitePoint;
    render.autoExposureEnabled = settings.autoExposureEnabled;
    render.targetLuminance = settings.targetLuminance;
    render.minExposure = settings.minExposure;
    render.maxExposure = settings.maxExposure;
    render.adaptationSpeed = settings.adaptationSpeed;
    render.histogramMinLogLuminance = settings.histogramMinLogLuminance;
    render.histogramMaxLogLuminance = settings.histogramMaxLogLuminance;
    render.histogramLowPercentile = settings.histogramLowPercentile;
    render.histogramHighPercentile = settings.histogramHighPercentile;
    render.histogramTargetPercentile = settings.histogramTargetPercentile;
    render.skyIntensity = settings.skyIntensity;
    render.indirectStrength = settings.indirectStrength;
    render.restirMode = settings.restirMode;
    render.restirGiEnabled = settings.restirGiEnabled;
    render.denoiserEnabled = settings.denoiserEnabled;
    render.denoiseWhileMoving = settings.denoiseWhileMoving;
    render.samplesPerPixel = settings.samplesPerPixel;
    render.limitSamplesPerPixel = settings.limitSamplesPerPixel;
    render.atrousIterations = settings.atrousIterations;
    render.denoiserStrength = settings.denoiserStrength;
    render.denoiserMaxHistoryLength = settings.denoiserMaxHistoryLength;
    render.momentValidityThreshold = settings.momentValidityThreshold;
    render.taaEnabled = settings.taaEnabled;
    render.taaFeedback = settings.taaFeedback;
    render.taaMotionFeedback = settings.taaMotionFeedback;
    render.taaReactiveFeedback = settings.taaReactiveFeedback;
    render.taaSharpeningStrength = settings.taaSharpeningStrength;
    render.debugView = settings.debugView;
    render.resolutionScale = settings.renderResolutionScale;
    render.materialTextureAnisotropy = settings.materialTextureAnisotropy;
    render.specularAaEnabled = settings.specularAaEnabled;
    render.opacityMicromapsEnabled = settings.opacityMicromapsEnabled;
    render.shadowRayBias = settings.shadowRayBias;
    render.shadowDistanceBias = settings.shadowDistanceBias;
    render.fireflyClamp = settings.fireflyClamp;
    render.restirGiTemporalMaxAge = settings.restirGiTemporalMaxAge;
    render.restirGiSpatialRounds = settings.restirGiSpatialRounds;
    render.restirGiSpatialRadius = settings.restirGiSpatialRadius;
    render.restirGiDepthThresholdScale = settings.restirGiDepthThresholdScale;
    render.restirGiSpatialCompatibilityThreshold = settings.restirGiSpatialCompatibilityThreshold;
    render.restirGiHalfResolution = settings.restirGiHalfResolution;
    render.restirGiVisibilityRayBudget = settings.restirGiVisibilityRayBudget;
    render.adaptiveQualityMode = settings.adaptiveQualityMode;
    render.adaptiveGpuFrameTargetMs = settings.adaptiveGpuFrameTargetMs;
    render.usePhysicalCamera = settings.usePhysicalCamera;
    render.physicalAperture = settings.physicalAperture;
    render.physicalShutterSeconds = settings.physicalShutterSeconds;
    render.physicalIso = settings.physicalIso;
    render.physicalExposureCompensation = settings.physicalExposureCompensation;
    render.dofApertureRadius = settings.dofApertureRadius;
    render.dofFocusDistance = settings.dofFocusDistance;
    render.dofBladeCount = settings.dofBladeCount;
    render.dofBokehRotation = settings.dofBokehRotation;
    render.motionBlurEnabled = settings.motionBlurEnabled;
    render.motionBlurShutterOpen = settings.motionBlurShutterOpen;
    render.motionBlurShutterClose = settings.motionBlurShutterClose;
    render.homogeneousVolumeEnabled = settings.homogeneousVolumeEnabled;
    render.homogeneousVolumeScattering = settings.homogeneousVolumeScattering;
    render.homogeneousVolumeAbsorption = settings.homogeneousVolumeAbsorption;
    render.homogeneousVolumeAnisotropy = settings.homogeneousVolumeAnisotropy;
    render.mneeCausticsEnabled = settings.mneeCausticsEnabled;
    Environment& environment = document.environment();
    environment.enabled = settings.environmentEnabled;
    environment.intensity = settings.environmentIntensity;
    environment.rotation = settings.environmentRotation;
    environment.backgroundIntensity = settings.environmentBackgroundIntensity;
    document.markDirty(SceneUpdateKind::RendererSettingsOnly);
}

RendererSettings rendererSettingsFromDocument(const SceneDocument& document, RendererSettings settings) {
    const RenderSettings& render = document.renderSettings();
    const Environment& environment = document.environment();
    settings.renderPreset = render.renderPreset;
    settings.pathTracingEnabled = render.pathTracingEnabled;
    settings.cameraJitterEnabled = render.cameraJitterEnabled;
    settings.directLightingEnabled = render.directLightingEnabled;
    settings.maxBounces = render.maxBounces;
    settings.environmentDirectSamples = render.environmentDirectSamples;
    settings.toneMapper = render.toneMapper;
    settings.exposure = render.exposure;
    settings.gamma = render.gamma;
    settings.contrast = render.contrast;
    settings.saturation = render.saturation;
    settings.brightness = render.brightness;
    settings.whitePoint = render.whitePoint;
    settings.autoExposureEnabled = render.autoExposureEnabled;
    settings.targetLuminance = render.targetLuminance;
    settings.minExposure = render.minExposure;
    settings.maxExposure = render.maxExposure;
    settings.adaptationSpeed = render.adaptationSpeed;
    settings.histogramMinLogLuminance = render.histogramMinLogLuminance;
    settings.histogramMaxLogLuminance = render.histogramMaxLogLuminance;
    settings.histogramLowPercentile = render.histogramLowPercentile;
    settings.histogramHighPercentile = render.histogramHighPercentile;
    settings.histogramTargetPercentile = render.histogramTargetPercentile;
    settings.skyIntensity = render.skyIntensity;
    settings.indirectStrength = render.indirectStrength;
    settings.restirMode = render.restirMode;
    settings.restirGiEnabled = render.restirGiEnabled;
    settings.denoiserEnabled = render.denoiserEnabled;
    settings.denoiseWhileMoving = render.denoiseWhileMoving;
    settings.samplesPerPixel = render.samplesPerPixel;
    settings.limitSamplesPerPixel = render.limitSamplesPerPixel;
    settings.atrousIterations = render.atrousIterations;
    settings.denoiserStrength = render.denoiserStrength;
    settings.denoiserMaxHistoryLength = render.denoiserMaxHistoryLength;
    settings.momentValidityThreshold = render.momentValidityThreshold;
    settings.taaEnabled = render.taaEnabled;
    settings.taaFeedback = render.taaFeedback;
    settings.taaMotionFeedback = render.taaMotionFeedback;
    settings.taaReactiveFeedback = render.taaReactiveFeedback;
    settings.taaSharpeningStrength = render.taaSharpeningStrength;
    settings.debugView = render.debugView;
    settings.renderResolutionScale = render.resolutionScale;
    settings.materialTextureAnisotropy = render.materialTextureAnisotropy;
    settings.specularAaEnabled = render.specularAaEnabled;
    settings.opacityMicromapsEnabled = render.opacityMicromapsEnabled;
    settings.shadowRayBias = render.shadowRayBias;
    settings.shadowDistanceBias = render.shadowDistanceBias;
    settings.fireflyClamp = render.fireflyClamp;
    settings.restirGiTemporalMaxAge = render.restirGiTemporalMaxAge;
    settings.restirGiSpatialRounds = render.restirGiSpatialRounds;
    settings.restirGiSpatialRadius = render.restirGiSpatialRadius;
    settings.restirGiDepthThresholdScale = render.restirGiDepthThresholdScale;
    settings.restirGiSpatialCompatibilityThreshold = render.restirGiSpatialCompatibilityThreshold;
    settings.restirGiHalfResolution = render.restirGiHalfResolution;
    settings.restirGiVisibilityRayBudget = render.restirGiVisibilityRayBudget;
    settings.adaptiveQualityMode = render.adaptiveQualityMode;
    settings.adaptiveGpuFrameTargetMs = render.adaptiveGpuFrameTargetMs;
    settings.usePhysicalCamera = render.usePhysicalCamera;
    settings.physicalAperture = render.physicalAperture;
    settings.physicalShutterSeconds = render.physicalShutterSeconds;
    settings.physicalIso = render.physicalIso;
    settings.physicalExposureCompensation = render.physicalExposureCompensation;
    settings.dofApertureRadius = render.dofApertureRadius;
    settings.dofFocusDistance = render.dofFocusDistance;
    settings.dofBladeCount = render.dofBladeCount;
    settings.dofBokehRotation = render.dofBokehRotation;
    settings.motionBlurEnabled = render.motionBlurEnabled;
    settings.motionBlurShutterOpen = render.motionBlurShutterOpen;
    settings.motionBlurShutterClose = render.motionBlurShutterClose;
    settings.homogeneousVolumeEnabled = render.homogeneousVolumeEnabled;
    settings.homogeneousVolumeScattering = render.homogeneousVolumeScattering;
    settings.homogeneousVolumeAbsorption = render.homogeneousVolumeAbsorption;
    settings.homogeneousVolumeAnisotropy = render.homogeneousVolumeAnisotropy;
    settings.mneeCausticsEnabled = render.mneeCausticsEnabled;
    settings.environmentEnabled = environment.enabled;
    settings.environmentIntensity = environment.intensity;
    settings.environmentRotation = environment.rotation;
    settings.environmentBackgroundIntensity = environment.backgroundIntensity;
    SunController::applyToRendererSettings(document, settings);
    return settings;
}

void applyDocumentMaterialAssignments(const SceneDocument& document, AssetManager& assets) {
    for (const Entity* entity : document.registry().entities()) {
        if (!entity->meshRenderer.has_value()) {
            continue;
        }
        MeshAsset* mesh = assets.mesh(entity->meshRenderer->mesh);
        if (mesh == nullptr) {
            continue;
        }
        const std::vector<MaterialSlot>& slots = entity->meshRenderer->materialSlots;
        for (size_t i = 0; i < slots.size() && i < mesh->primitives.size(); ++i) {
            const MaterialAssetHandle material = slots[i].resolvedMaterial();
            if (material.valid()) {
                mesh->primitives[i].material = material;
                updatePrimitiveAlphaClassification(mesh->primitives[i], assets.material(material));
            }
        }
    }
}

std::filesystem::path resolveProjectRoot() {
    std::filesystem::path candidate = std::filesystem::current_path();
    while (!candidate.empty()) {
        auto shadersDir = candidate / "native" / "vulkan" / "shaders";
        if (std::filesystem::exists(shadersDir / "pathtrace.rgen")) {
            return candidate;
        }
        candidate = candidate.parent_path();
    }
    return std::filesystem::current_path();
}

glm::mat4 entityWorldMatrix(const SceneRegistry& registry, const Entity& entity) {
    const Entity* current = &entity;
    glm::mat4 result(1.0f);
    constexpr int maxDepth = 512;
    for (int depth = 0; depth < maxDepth && current != nullptr; ++depth) {
        result = current->transform.localMatrix() * result;
        if (!current->parent.valid()) {
            break;
        }
        const Entity* parent = registry.entity(current->parent);
        if (parent == nullptr) {
            break;
        }
        if (parent == current) {
            break;
        }
        current = parent;
    }
    return result;
}

EntityId duplicateEntityRecursive(SceneRegistry& registry, Entity source, EntityId parent) {
    const EntityId copyId = registry.createEntity(source.name.empty() ? "Entity Copy" : source.name + " Copy");
    Entity* copy = registry.entity(copyId);
    if (copy == nullptr) {
        return {};
    }

    copy->transform = source.transform;
    copy->transform.dirty = true;
    copy->meshRenderer = source.meshRenderer;
    copy->light = source.light;
    copy->sun = source.sun;
    copy->camera = source.camera;
    if (copy->camera.has_value()) {
        copy->camera->active = false;
    }
    copy->parent = parent;
    copy->children.clear();
    if (Entity* parentEntity = registry.entity(parent)) {
        parentEntity->children.push_back(copyId);
    }

    const std::vector<EntityId> children = source.children;
    for (EntityId childId : children) {
        if (const Entity* child = registry.entity(childId)) {
            (void)duplicateEntityRecursive(registry, *child, copyId);
        }
    }
    return copyId;
}

void windowFocusCallback(GLFWwindow* window, int focused) {
    auto* app = static_cast<Application*>(glfwGetWindowUserPointer(window));
    if (app != nullptr) {
        app->onWindowFocusChanged(focused == GLFW_TRUE);
    }
}

void fileDropCallback(GLFWwindow* window, int count, const char** paths) {
    auto* app = static_cast<Application*>(glfwGetWindowUserPointer(window));
    if (app != nullptr) {
        app->onFilesDropped(count, paths);
    }
}
}

Application::Application(
    RendererDebugView debugView,
    std::optional<std::filesystem::path> gltfPath,
    std::optional<std::filesystem::path> hdrPath,
    std::optional<std::filesystem::path> scenePath,
    std::optional<bool> denoiserOverride,
    std::optional<RestirMode> restirModeOverride,
    std::optional<RenderPreset> renderPresetOverride,
    std::optional<bool> restirGiOverride,
    std::optional<bool> opacityMicromapOverride,
    std::optional<uint32_t> opacityMicromapSubdivisionOverride,
    bool debugViewOverride,
    bool validationCameraMotion,
    bool validationObjectMotion,
    bool headless,
    bool disableAsyncCompute,
    bool singleQueueFallback,
    bool disableResourceAliasing)
    : debugView_(debugView),
      gltfPath_(std::move(gltfPath)),
      hdrPath_(std::move(hdrPath)),
      scenePath_(std::move(scenePath)),
      denoiserOverride_(denoiserOverride),
      restirModeOverride_(restirModeOverride),
      renderPresetOverride_(renderPresetOverride),
      restirGiOverride_(restirGiOverride),
      opacityMicromapOverride_(opacityMicromapOverride),
      opacityMicromapSubdivisionOverride_(opacityMicromapSubdivisionOverride),
      debugViewOverride_(debugViewOverride),
      validationCameraMotion_(validationCameraMotion),
      validationObjectMotion_(validationObjectMotion),
      disableAsyncCompute_(disableAsyncCompute),
      singleQueueFallback_(singleQueueFallback),
      disableResourceAliasing_(disableResourceAliasing),
      headless_(headless) {
    if (!headless_) {
        initWindow();
    }
    initVulkan();
}

Application::~Application() {
    if (pendingSceneLoad_.valid()) {
        pendingSceneLoad_.wait();
    }
    if (commandSystem_) {
        commandSystem_->waitIdle();
    }

    if (uiOverlay_) {
        uiOverlay_->editor().editorPrefs().save(EditorPreferences::defaultPath());
    }
    commandSystem_.reset();
    uiOverlay_.reset();
    pathTracer_.reset();
    pipelineDemo_.reset();
    resourceDemo_.reset();
    swapchain_.reset();
    uploader_.reset();
    uploadContext_.reset();
    allocator_.reset();
    context_.reset();

    if (window_ != nullptr) {
        glfwDestroyWindow(window_);
    }
    if (!headless_) {
        glfwTerminate();
    }
}

void Application::run(uint32_t maxFrames) {
    mainLoop(maxFrames);
}

void Application::runHeadless(uint32_t warmupFrames, uint32_t totalFrames) {
    warmupFrameCount_ = warmupFrames;
    totalFrameCount_ = totalFrames;
    nextDiagnosticFrameIndex_ = 0;
    cpuFrameTimings_.clear();
    gpuFrameTimings_.clear();
    perFrameGpuTimings_.clear();
    cpuFrameTimings_.reserve(totalFrames);
    gpuFrameTimings_.reserve(totalFrames);
    perFrameGpuTimings_.reserve(totalFrames);

    const uint32_t renderedFrames = std::max(warmupFrames, totalFrames);
    const auto start = std::chrono::steady_clock::now();
    float seconds = 0.0f;

    for (uint32_t frameCount = 0; frameCount < renderedFrames; ++frameCount) {
        const auto frameStart = std::chrono::steady_clock::now();

        const float rawDeltaSeconds = 1.0f / 60.0f;
        const float deltaSeconds = clampFrameDeltaSeconds(rawDeltaSeconds, pathTracer_.get());
        lastFrameSeconds_ = seconds;
        applyValidationObjectMotion(nextDiagnosticFrameIndex_);
        applyValidationCameraMotion(nextDiagnosticFrameIndex_++);
        if (beginFrameCapture_) {
            beginFrameCapture_(frameCount + 1u);
        }
        commandSystem_->drawFrame(seconds, deltaSeconds);
        ++frameSerial_;
        releaseRetiredPathTracers();
        if (endFrameCapture_) {
            endFrameCapture_(frameCount + 1u);
        }
        seconds += deltaSeconds;

        const auto frameEnd = std::chrono::steady_clock::now();
        const float cpuMs = std::chrono::duration<float, std::milli>(frameEnd - frameStart).count();
        cpuFrameTimings_.push_back(cpuMs);

        if (pathTracer_) {
            const auto& timings = pathTracer_->timings();
            gpuFrameTimings_.push_back(timings.totalMs());
            perFrameGpuTimings_.push_back(timings);
        }
    }
    commandSystem_->waitIdle();
}

void Application::renderFrames(uint32_t count) {
    float seconds = lastFrameSeconds_ + 1.0f / 60.0f;
    for (uint32_t i = 0; i < count; ++i) {
        const float rawDeltaSeconds = 1.0f / 60.0f;
        const float deltaSeconds = clampFrameDeltaSeconds(rawDeltaSeconds, pathTracer_.get());
        lastFrameSeconds_ = seconds;
        applyValidationObjectMotion(nextDiagnosticFrameIndex_);
        applyValidationCameraMotion(nextDiagnosticFrameIndex_++);
        if (beginFrameCapture_) {
            beginFrameCapture_(i + 1u);
        }
        commandSystem_->drawFrame(seconds, deltaSeconds);
        ++frameSerial_;
        releaseRetiredPathTracers();
        if (endFrameCapture_) {
            endFrameCapture_(i + 1u);
        }
        seconds += deltaSeconds;
    }
    commandSystem_->waitIdle();
}

bool Application::runDescriptorLifetimeStress(
    const std::filesystem::path& outputPath,
    uint32_t cycles,
    uint32_t framesPerCycle) {
    if (!headless_) {
        throw std::runtime_error("--descriptor-lifetime-stress requires --headless");
    }
    if (!pathTracer_ || !allocator_ || !commandSystem_) {
        throw std::runtime_error("Descriptor lifetime stress requires an initialized renderer");
    }

    cycles = std::max(1u, cycles);
    framesPerCycle = std::max(1u, framesPerCycle);

    const RendererSettings originalSettings = pathTracer_->settings();
    const auto initialDescriptorStats = pathTracer_->descriptorPoolStats();
    const auto initialBudget = allocator_->memoryBudgetReport();

    uint32_t maxPoolCount = initialDescriptorStats.poolCount;
    uint32_t maxCapacitySets = initialDescriptorStats.capacitySets;
    uint32_t maxAllocatedSets = initialDescriptorStats.allocatedSets;
    uint32_t maxPeakAllocatedSets = initialDescriptorStats.peakAllocatedSets;
    uint32_t maxFailedAllocations = initialDescriptorStats.failedAllocations;
    uint32_t maxFragmentedPoolFailures = initialDescriptorStats.fragmentedPoolFailures;
    uint64_t maxVmaUsageBytes = initialBudget.totalUsageBytes;
    uint64_t maxVmaAllocationBytes = initialBudget.totalAllocationBytes;

    nlohmann::json samples = nlohmann::json::array();

    auto descriptorJson = [](const DescriptorAllocator::Stats& stats) {
        return nlohmann::json{
            {"sets_per_pool", stats.setsPerPool},
            {"max_pools", stats.maxPools},
            {"used_pools", stats.usedPools},
            {"free_pools", stats.freePools},
            {"pool_count", stats.poolCount},
            {"capacity_sets", stats.capacitySets},
            {"allocated_sets", stats.allocatedSets},
            {"peak_allocated_sets", stats.peakAllocatedSets},
            {"failed_allocations", stats.failedAllocations},
            {"fragmented_pool_failures", stats.fragmentedPoolFailures},
            {"pool_growth_count", stats.poolGrowthCount},
        };
    };
    auto budgetJson = [](const ResourceAllocator::MemoryBudgetReport& budget) {
        nlohmann::json heaps = nlohmann::json::array();
        for (const auto& heap : budget.heaps) {
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
        return nlohmann::json{
            {"supported", budget.supported},
            {"total_usage_bytes", budget.totalUsageBytes},
            {"total_budget_bytes", budget.totalBudgetBytes},
            {"total_allocation_bytes", budget.totalAllocationBytes},
            {"total_block_bytes", budget.totalBlockBytes},
            {"peak_usage_bytes", budget.peakUsageBytes},
            {"usage_delta_bytes", budget.usageDeltaBytes},
            {"allocation_count", budget.allocationCount},
            {"block_count", budget.blockCount},
            {"max_usage_ratio", budget.maxUsageRatio},
            {"pressure", budget.pressure},
            {"override_active", budget.overrideActive},
            {"warnings", budget.warnings},
            {"heaps", heaps},
        };
    };
    auto takeSample = [&](uint32_t cycle, const char* phase) {
        const auto descriptorStats = pathTracer_->descriptorPoolStats();
        const auto budget = allocator_->memoryBudgetReport();
        maxPoolCount = std::max(maxPoolCount, descriptorStats.poolCount);
        maxCapacitySets = std::max(maxCapacitySets, descriptorStats.capacitySets);
        maxAllocatedSets = std::max(maxAllocatedSets, descriptorStats.allocatedSets);
        maxPeakAllocatedSets = std::max(maxPeakAllocatedSets, descriptorStats.peakAllocatedSets);
        maxFailedAllocations = std::max(maxFailedAllocations, descriptorStats.failedAllocations);
        maxFragmentedPoolFailures = std::max(maxFragmentedPoolFailures, descriptorStats.fragmentedPoolFailures);
        maxVmaUsageBytes = std::max(maxVmaUsageBytes, budget.totalUsageBytes);
        maxVmaAllocationBytes = std::max(maxVmaAllocationBytes, budget.totalAllocationBytes);
        samples.push_back({
            {"cycle", cycle},
            {"phase", phase},
            {"frame_serial", frameSerial_},
            {"retired_renderer_count", retiredPathTracers_.size()},
            {"descriptors", descriptorJson(descriptorStats)},
            {"vma_budget", budgetJson(budget)},
        });
    };

    takeSample(0, "initial");
    const float originalScale = std::clamp(originalSettings.renderResolutionScale, 0.1f, 1.0f);
    const float stressScale = std::max(0.5f, std::min(originalScale, 0.75f));

    for (uint32_t cycle = 0; cycle < cycles; ++cycle) {
        renderFrames(framesPerCycle);
        takeSample(cycle, "steady_frame");

        RendererSettings resizedSettings = pathTracer_->settings();
        resizedSettings.renderResolutionScale = (cycle % 2u == 0u) ? stressScale : originalScale;
        resizedSettings.renderPreset = RenderPreset::Custom;
        applyRendererSettingsSafely(resizedSettings, true);
        pathTracer_->resetAccumulation(AccumulationResetReason::RenderSettingsChanged);
        renderFrames(framesPerCycle);
        takeSample(cycle, "render_scale_toggle");

        if (gpuSceneAsset_.has_value() && !gpuSceneAsset_->meshes.empty()) {
            if (!pathTracer_->updateMaterials(*gpuSceneAsset_, assets_)) {
                pathTracer_->resetAccumulation(AccumulationResetReason::MaterialChanged);
            }
            renderFrames(framesPerCycle);
            takeSample(cycle, "material_update");
        }

        if (scenePath_.has_value()) {
            const RendererSettings previousSettings = pathTracer_->settings();
            if (!sceneDocument_.loadJson(*scenePath_)) {
                throw std::runtime_error("Descriptor lifetime stress scene reload failed: " + scenePath_->string());
            }
            gltfPath_ = sceneDocument_.sourceGltfPath();
            hdrPath_ = sceneDocument_.sourceHdrPath();
            if (gltfPath_.has_value() && std::filesystem::exists(*gltfPath_)) {
                assets_.clear();
                GltfLoader loader(assets_);
                importedScene_ = loader.loadWithCache(*gltfPath_);
            }
            rebuildGpuSceneAsset();
            std::unique_ptr<PathTracerRenderer> nextPathTracer = makePathTracer(
                gpuSceneAsset_.has_value() && !gpuSceneAsset_->meshes.empty() ? &*gpuSceneAsset_ : nullptr,
                gpuSceneAsset_.has_value() && !gpuSceneAsset_->meshes.empty() ? &assets_ : nullptr,
                gltfPath_.has_value() ? SceneCache::cachePathFor(*gltfPath_) : std::optional<std::filesystem::path>{},
                &previousSettings);
            retirePathTracer(std::move(pathTracer_));
            pathTracer_ = std::move(nextPathTracer);
            applyActiveSceneCamera();
            pathTracer_->resetAccumulation(AccumulationResetReason::SceneChanged);
            commandSystem_->setPathTracer(pathTracer_.get());
            renderFrames(CommandSystem::framesInFlight + framesPerCycle + 1u);
            takeSample(cycle, "scene_reload");
        }

        reloadShadersFromEditor();
        takeSample(cycle, "renderer_recreated");
        renderFrames(CommandSystem::framesInFlight + framesPerCycle + 1u);
        takeSample(cycle, "retirement_drained");
    }

    applyRendererSettingsSafely(originalSettings, true);
    renderFrames(CommandSystem::framesInFlight + framesPerCycle + 1u);
    takeSample(cycles, "final");

    const auto finalDescriptorStats = pathTracer_->descriptorPoolStats();
    const auto finalBudget = allocator_->memoryBudgetReport();
    std::vector<std::string> failures;
    if (maxFailedAllocations > initialDescriptorStats.failedAllocations) {
        failures.push_back("descriptor allocation failures increased");
    }
    if (maxFragmentedPoolFailures > initialDescriptorStats.fragmentedPoolFailures) {
        failures.push_back("fragmented descriptor-pool failures increased");
    }
    if (!retiredPathTracers_.empty()) {
        failures.push_back("retired renderer queue was not drained");
    }
    const bool passed = failures.empty();

    nlohmann::json report;
    report["schema"] = "rtv_descriptor_lifetime_stress_v1";
    report["passed"] = passed;
    report["cycles"] = cycles;
    report["frames_per_cycle"] = framesPerCycle;
    report["operations"] = {
        "steady_frame",
        "render_scale_toggle",
        "material_update_when_scene_assets_exist",
        "scene_reload_when_rtlevel_path_exists",
        "renderer_recreated_via_shader_reload_path",
        "retirement_drained"
    };
    report["initial_descriptors"] = descriptorJson(initialDescriptorStats);
    report["final_descriptors"] = descriptorJson(finalDescriptorStats);
    report["max_descriptors"] = {
        {"pool_count", maxPoolCount},
        {"capacity_sets", maxCapacitySets},
        {"allocated_sets", maxAllocatedSets},
        {"peak_allocated_sets", maxPeakAllocatedSets},
        {"failed_allocations", maxFailedAllocations},
        {"fragmented_pool_failures", maxFragmentedPoolFailures},
    };
    report["initial_vma_budget"] = budgetJson(initialBudget);
    report["final_vma_budget"] = budgetJson(finalBudget);
    report["max_vma_usage_bytes"] = maxVmaUsageBytes;
    report["max_vma_allocation_bytes"] = maxVmaAllocationBytes;
    report["retired_renderer_count_final"] = retiredPathTracers_.size();
    report["failure_reasons"] = failures;
    report["samples"] = std::move(samples);

    const auto parent = outputPath.parent_path();
    if (!parent.empty()) {
        std::filesystem::create_directories(parent);
    }
    std::ofstream file(outputPath);
    if (!file.is_open()) {
        throw std::runtime_error("Failed to open descriptor lifetime stress output: " + outputPath.string());
    }
    file << report.dump(2);
    return passed;
}

void Application::resetDiagnosticFrameCounter(uint32_t frameIndex) {
    nextDiagnosticFrameIndex_ = frameIndex;
}

void Application::setFrameCaptureCallbacks(std::function<void(uint32_t)> begin, std::function<void(uint32_t)> end) {
    beginFrameCapture_ = std::move(begin);
    endFrameCapture_ = std::move(end);
}

void Application::resetAccumulation() {
    if (pathTracer_) {
        pathTracer_->resetAccumulation(AccumulationResetReason::Manual);
    }
}

bool Application::applyNamedCamera(std::string_view cameraName) {
    if (pathTracer_ == nullptr || cameraName.empty()) {
        return false;
    }

    const std::string normalized = normalizedCameraName(cameraName);
    auto applyPose = [&](glm::vec3 position, glm::vec3 target, float fovY, std::string_view label) {
        glm::vec3 forward = target - position;
        if (glm::dot(forward, forward) <= 1.0e-8f) {
            return false;
        }
        pathTracer_->setCameraFovY(fovY);
        cameraController_.setPose(position, glm::normalize(forward), *pathTracer_);
        std::cout << "Applied camera: " << label << '\n';
        return true;
    };

    if (normalized == "sponza-foliage" || normalized == "lightweight-sponza-foliage") {
        return applyPose(
            glm::vec3{2.0f, 1.05f, 1.55f},
            glm::vec3{-0.52f, 0.82f, -0.30f},
            1.15f,
            "sponza-foliage");
    }
    if (normalized == "sponza-courtyard" || normalized == "lightweight-sponza-courtyard") {
        return applyPose(
            glm::vec3{0.0f, 1.25f, 6.0f},
            glm::vec3{0.0f, 0.95f, 0.0f},
            0.872665f,
            "sponza-courtyard");
    }

    for (const Entity* entity : sceneDocument_.registry().entities()) {
        if (entity == nullptr || !entity->camera.has_value()) {
            continue;
        }
        if (normalizedCameraName(entity->name) != normalized) {
            continue;
        }
        const glm::mat4 transform = entityWorldMatrix(sceneDocument_.registry(), *entity);
        const glm::vec3 position = glm::vec3(transform[3]);
        glm::vec3 forward = glm::mat3(transform) * glm::vec3(0.0f, 0.0f, -1.0f);
        if (glm::dot(forward, forward) <= 1.0e-8f) {
            forward = glm::vec3{0.0f, 0.0f, -1.0f};
        }
        pathTracer_->setCameraFovY(entity->camera->verticalFovRadians);
        cameraController_.setPose(position, glm::normalize(forward), *pathTracer_);
        std::cout << "Applied scene camera: " << entity->name << '\n';
        return true;
    }

    return false;
}

void Application::applyDebugView(RendererDebugView view) {
    if (pathTracer_) {
        RendererSettings settings = pathTracer_->settings();
        settings.debugView = view;
        pathTracer_->applySettings(settings);
        pathTracer_->resetAccumulation(AccumulationResetReason::DebugViewChanged);
    }
}

void Application::initWindow() {
    if (glfwInit() != GLFW_TRUE) {
        throw std::runtime_error("glfwInit failed");
    }

    glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
    glfwWindowHint(GLFW_RESIZABLE, GLFW_TRUE);
    window_ = glfwCreateWindow(initialWidth, initialHeight, "Ray Tracing Engine - Vulkan", nullptr, nullptr);
    if (window_ == nullptr) {
        throw std::runtime_error("glfwCreateWindow failed");
    }
    glfwSetWindowUserPointer(window_, this);
    glfwSetWindowFocusCallback(window_, windowFocusCallback);
    glfwSetDropCallback(window_, fileDropCallback);
    glfwGetWindowPos(window_, &windowedX_, &windowedY_);
    glfwGetWindowSize(window_, &windowedWidth_, &windowedHeight_);

    std::cout << "Controls: hold right mouse in the viewport to look/move, Ctrl+L drag rotates sun, WASD move, QE/Space/Ctrl vertical, Shift fast, F11 borderless fullscreen.\n"
              << "Settings: F1 debug view, F2 denoiser, F3 denoise while moving, F4 sun, F5 env, F6 direct light, R reset.\n"
              << "Adjust: +/- exposure, 1-5 tone mapper, 6 auto exposure, </> env intensity, [/ ] env rotation, PageUp/PageDown bounces, Home/End a-trous.\n"
              << "Files: drop .hdr for environment maps or .gltf/.glb for scene reload.\n";
}

void Application::initVulkan() {
    if (headless_) {
        context_ = VulkanContext::createHeadless();
    } else {
        context_ = std::make_unique<VulkanContext>(window_);
    }
    allocator_ = std::make_unique<ResourceAllocator>(*context_);
    uploadContext_ = std::make_unique<UploadContext>(context_->device(), context_->graphicsQueue(), context_->queueFamilies().graphics.value());
    uploader_ = std::make_unique<BufferUploader>(*allocator_, *uploadContext_);
    if (headless_) {
        constexpr VkExtent2D defaultExtent{1280, 720};
        swapchain_ = std::make_unique<Swapchain>(*context_, defaultExtent);
    } else {
        swapchain_ = std::make_unique<Swapchain>(*context_, window_);
    }
    commandSystem_ = std::make_unique<CommandSystem>(*context_, *swapchain_, disableAsyncCompute_, singleQueueFallback_);
    commandSystem_->setHeadless(headless_);
    const auto projectRoot = resolveProjectRoot();
    const auto shaderDir = projectRoot / "native" / "vulkan" / "shaders";
    bool loadedSceneDocument = false;
    if (scenePath_.has_value()) {
        if (!sceneDocument_.loadJson(*scenePath_)) {
            throw std::runtime_error("Scene JSON load failed: " + scenePath_->string());
        }
        loadedSceneDocument = true;
        gltfPath_ = sceneDocument_.sourceGltfPath();
        if (!hdrPath_.has_value()) {
            hdrPath_ = sceneDocument_.sourceHdrPath();
        }
        if (gltfPath_.has_value() && std::filesystem::exists(*gltfPath_)) {
            GltfLoader loader(assets_);
            importedScene_ = loader.loadWithCache(*gltfPath_);
        }
        undoStack_.clear();
        std::cout << "Loaded scene JSON: " << scenePath_->string() << '\n';
    } else if (gltfPath_.has_value()) {
        GltfLoader loader(assets_);
        importedScene_ = loader.loadWithCache(*gltfPath_);
        sceneDocument_.importSceneAsset(*importedScene_);
        sceneDocument_.setSourceGltfPath(gltfPath_);
        undoStack_.clear();
        std::cout << "Loaded glTF: " << gltfPath_->string()
                  << " meshes=" << importedScene_->meshes.size()
                  << " materials=" << importedScene_->materials.size()
                  << " textures=" << importedScene_->textures.size()
                  << " nodes=" << importedScene_->nodes.size() << '\n';
    } else if (!loadedSceneDocument) {
        initializeFallbackSceneDocument();
    }
    sceneUnsavedDirty_ = false;
    sceneDocument_.setSourceHdrPath(hdrPath_);
    rebuildGpuSceneAsset();
    RendererSettings startupSettings{};
    startupSettings.debugView = debugView_;
    startupSettings = rendererSettingsFromDocument(sceneDocument_, startupSettings);
    if (debugViewOverride_) {
        startupSettings.debugView = debugView_;
    } else if (loadedSceneDocument) {
        debugView_ = startupSettings.debugView;
    }
    bool largeSceneSettingsChanged = false;
    if (importedScene_.has_value()) {
        startupSettings = interactiveSettingsForScene(startupSettings, *importedScene_, assets_, largeSceneSettingsChanged);
        if (largeSceneSettingsChanged) {
            syncDocumentRenderSettings(sceneDocument_, startupSettings);
        }
    }
    if (denoiserOverride_.has_value()) {
        startupSettings.denoiserEnabled = *denoiserOverride_;
        startupSettings.renderPreset = RenderPreset::Custom;
    }
    if (renderPresetOverride_.has_value()) {
        applyRenderPreset(startupSettings, *renderPresetOverride_);
        syncDocumentRenderSettings(sceneDocument_, startupSettings);
    }
    if (restirModeOverride_.has_value()) {
        startupSettings.restirMode = *restirModeOverride_;
        startupSettings.renderPreset = RenderPreset::Custom;
        syncDocumentRenderSettings(sceneDocument_, startupSettings);
    }
    if (restirGiOverride_.has_value()) {
        startupSettings.restirGiEnabled = *restirGiOverride_;
        startupSettings.renderPreset = RenderPreset::Custom;
        syncDocumentRenderSettings(sceneDocument_, startupSettings);
    }
    if (opacityMicromapOverride_.has_value()) {
        startupSettings.opacityMicromapsEnabled = *opacityMicromapOverride_;
        startupSettings.renderPreset = RenderPreset::Custom;
        syncDocumentRenderSettings(sceneDocument_, startupSettings);
    }
    if (opacityMicromapSubdivisionOverride_.has_value()) {
        startupSettings.opacityMicromapSubdivisionLevel = *opacityMicromapSubdivisionOverride_;
        startupSettings.renderPreset = RenderPreset::Custom;
    }
    createPathTracer(&startupSettings);
    syncDocumentRenderSettings(sceneDocument_, pathTracer_->settings());
    applyActiveSceneCamera();
    sceneDocument_.clearDirty();
    if (!headless_) {
        uiOverlay_ = std::make_unique<UiOverlay>(window_, *context_, *swapchain_);
        if (loadedSceneDocument) {
            uiOverlay_->editor().cameraBookmarks().deserialize(sceneDocument_);
        }
        commandSystem_->setUiOverlay(uiOverlay_.get());
        uiOverlay_->editor().editorPrefs().load(EditorPreferences::defaultPath());
        EditorPreferences& prefs = uiOverlay_->editor().editorPrefs();
        if (!scenePath_.has_value() && !gltfPath_.has_value() && prefs.openLastProject && !prefs.lastOpenedProject.empty() &&
            std::filesystem::exists(prefs.lastOpenedProject)) {
            if (openProjectFromFile(prefs.lastOpenedProject, false)) {
                (void)applyPendingSceneUpdate(true);
            }
        }
    }
    commandSystem_->setPathTracer(pathTracer_.get());
}

void Application::mainLoop(uint32_t maxFrames) {
    if (headless_) {
        runHeadless(0, maxFrames > 0 ? maxFrames : 120);
        return;
    }

    const auto start = std::chrono::steady_clock::now();
    uint32_t frameCount = 0;
    const uint32_t totalMaxFrames = (headless_ && maxFrames == 0) ? 120u : maxFrames;

    if (headless_) {
        while (totalMaxFrames == 0 || frameCount < totalMaxFrames) {
            const auto now = std::chrono::steady_clock::now();
            const float seconds = std::chrono::duration<float>(now - start).count();
            const float rawDeltaSeconds = std::max(0.0f, seconds - lastFrameSeconds_);
            const float deltaSeconds = clampFrameDeltaSeconds(rawDeltaSeconds, pathTracer_.get());
            lastFrameSeconds_ = seconds;

            commandSystem_->drawFrame(seconds, deltaSeconds);
            ++frameSerial_;
            releaseRetiredPathTracers();
            ++frameCount;
        }
        commandSystem_->waitIdle();
        return;
    }

    while (glfwWindowShouldClose(window_) == GLFW_FALSE) {
        glfwPollEvents();

        const auto now = std::chrono::steady_clock::now();
        const float seconds = std::chrono::duration<float>(now - start).count();
        const float rawDeltaSeconds = std::max(0.0f, seconds - lastFrameSeconds_);
        const float deltaSeconds = clampFrameDeltaSeconds(rawDeltaSeconds, pathTracer_.get());
        lastFrameSeconds_ = seconds;

        if (uiOverlay_) {
            uiOverlay_->beginFrame();
        }
        processRuntimeControls(deltaSeconds);
        applyValidationObjectMotion(frameCount);
        applyValidationCameraMotion(frameCount);
        notifications_.update(deltaSeconds);
        EditorRequests editorRequests;
        if (pendingOpenLevel_) {
            pendingOpenLevel_ = false;
            if (auto path = openSceneJsonFileDialog()) {
                editorRequests.loadSceneJson = *path;
                editorRequests.resetAccumulation = AccumulationResetReason::SceneChanged;
            }
        }
        if (pendingSaveLevel_) {
            pendingSaveLevel_ = false;
            if (auto path = saveSceneJsonFileDialog()) {
                editorRequests.saveSceneJson = *path;
            }
        }
        if (pendingReloadShaders_) {
            pendingReloadShaders_ = false;
            editorRequests.reloadShaders = true;
            editorRequests.resetAccumulation = AccumulationResetReason::ShaderReloaded;
        }
        if (pathTracer_ && pathTracer_->shadersNeedReload()) {
            editorRequests.reloadShaders = true;
            editorRequests.resetAccumulation = AccumulationResetReason::ShaderReloaded;
        }
        if (uiOverlay_ && pathTracer_) {
            editorRequests = uiOverlay_->build(
                *pathTracer_,
                swapchain_->extent(),
                importedScene_ ? &*importedScene_ : nullptr,
                &sceneDocument_,
                importedScene_ ? &assets_ : nullptr,
                gltfPath_,
                hdrPath_,
                scenePath_,
                project_ ? &*project_ : nullptr,
                project_ ? &assetRegistry_ : nullptr,
                sceneUnsavedDirty_,
                &gpuInstanceEntities_,
                sceneLoadingStatus_,
                &cameraController_,
                rawDeltaSeconds * 1000.0f,
                &notifications_,
                sunDrag_.phase != SunDragPhase::Idle);
        }
        applyEditorRequests(editorRequests, false);
        if (beginFrameCapture_) {
            beginFrameCapture_(frameCount + 1u);
        }
        commandSystem_->drawFrame(seconds, deltaSeconds);
        ++frameSerial_;
        releaseRetiredPathTracers();
        if (endFrameCapture_) {
            endFrameCapture_(frameCount + 1u);
        }
        applyEditorRequests(editorRequests, true);
        pollAsyncSceneLoad();
        updateWindowTitle(seconds);

        ++frameCount;
        if (maxFrames > 0 && frameCount >= maxFrames) {
            break;
        }
    }
}

void Application::onWindowFocusChanged(bool focused) {
    if (!focused && window_ != nullptr) {
        cameraController_.releaseMouse(window_);
        finishSunDrag(true);
    }
}

void Application::onFilesDropped(int count, const char** paths) {
    if (pathTracer_ == nullptr || paths == nullptr || count <= 0) {
        return;
    }

    for (int i = 0; i < count; ++i) {
        if (paths[i] == nullptr) {
            continue;
        }
        std::filesystem::path path{paths[i]};
        std::string extension = path.extension().string();
        std::transform(extension.begin(), extension.end(), extension.begin(), [](unsigned char ch) {
            return static_cast<char>(std::tolower(ch));
        });
        if (extension != ".hdr") {
            if (extension == ".gltf" || extension == ".glb") {
                if (!confirmDestructiveSceneAction("importing a scene as a new scene")) {
                    std::cout << "Dropped scene import cancelled: " << path.string() << '\n';
                    continue;
                }
                requestGltfSceneLoad(path);
            } else {
                std::cout << "Dropped file ignored: " << path.string() << " (supported: .hdr, .gltf, .glb)\n";
            }
            continue;
        }

        try {
            pathTracer_->loadEnvironment(path);
            hdrPath_ = path;
            sceneDocument_.setSourceHdrPath(hdrPath_);
            sceneDocument_.markDirty(SceneUpdateKind::EnvironmentOnly);
            RendererSettings settings = pathTracer_->settings();
            settings.environmentEnabled = true;
            applyRendererSettingsSafely(settings, true);
            notifications_.notify("HDR environment loaded", NotificationType::Success);
            std::cout << "Loaded dropped HDR environment: " << path.string() << '\n';
        } catch (const std::exception& error) {
            notifications_.notify("HDR environment load failed", NotificationType::Error);
            std::cerr << "Dropped HDR load failed: " << error.what() << '\n';
        }
    }
}

void Application::reloadGltfScene(const std::filesystem::path& path) {
    PendingSceneLoadResult result;
    result.path = path;
    try {
        GltfLoader loader(result.assets);
        result.scene = loader.loadWithCache(path);
    } catch (const std::exception& error) {
        result.error = error.what();
    }
    commitLoadedGltfScene(std::move(result));
}

void Application::requestGltfSceneLoad(const std::filesystem::path& path) {
    if (pendingSceneLoad_.valid() &&
        pendingSceneLoad_.wait_for(std::chrono::seconds(0)) != std::future_status::ready) {
        sceneLoadingStatus_ = "Scene load already running: " + pendingSceneLoadPath_->string();
        notifications_.notify("Scene load already running", NotificationType::Warning);
        return;
    }

    if (pendingSceneLoad_.valid()) {
        (void)pendingSceneLoad_.get();
    }

    pendingSceneLoadPath_ = path;
    sceneLoadingStatus_ = "Importing scene as new scene in background: " + path.string();
    notifications_.notify("Importing scene as new scene", NotificationType::Info);
    std::cout << sceneLoadingStatus_ << '\n';
    pendingSceneLoad_ = std::async(std::launch::async, [path]() {
        PendingSceneLoadResult result;
        result.path = path;
        try {
            GltfLoader loader(result.assets);
            result.scene = loader.loadWithCache(path);
        } catch (const std::exception& error) {
            result.error = error.what();
        }
        return result;
    });
}

void Application::pollAsyncSceneLoad() {
    if (!pendingSceneLoad_.valid()) {
        return;
    }
    if (pendingSceneLoad_.wait_for(std::chrono::seconds(0)) != std::future_status::ready) {
        return;
    }

    PendingSceneLoadResult result = pendingSceneLoad_.get();
    pendingSceneLoadPath_.reset();
    if (!result.error.empty()) {
        sceneLoadingStatus_ = "Import scene failed: " + result.error;
        notifications_.notify("Import scene failed", NotificationType::Error);
        std::cerr << sceneLoadingStatus_ << '\n';
        return;
    }

    sceneLoadingStatus_ = "Finalizing GPU scene: " + result.path.string();
    commitLoadedGltfScene(std::move(result));
}

void Application::commitLoadedGltfScene(PendingSceneLoadResult&& result) {
    if (!context_ || !allocator_ || !uploader_ || !swapchain_ || !commandSystem_) {
        return;
    }
    if (!result.error.empty()) {
        sceneLoadingStatus_ = "Import scene failed: " + result.error;
        notifications_.notify("Import scene failed", NotificationType::Error);
        std::cerr << sceneLoadingStatus_ << '\n';
        return;
    }

    const RendererSettings previousSettings = pathTracer_ != nullptr ? pathTracer_->settings() : RendererSettings{};
    bool largeSceneSettingsChanged = false;
    RendererSettings reloadSettings = interactiveSettingsForScene(previousSettings, result.scene, result.assets, largeSceneSettingsChanged);

    try {
        SceneDocument nextDocument;
        nextDocument.importSceneAsset(result.scene);
        nextDocument.setSourceGltfPath(result.path);
        nextDocument.setSourceHdrPath(hdrPath_);
        syncDocumentRenderSettings(nextDocument, reloadSettings);
        (void)SunController::migrateLegacyDirectionalSun(nextDocument);
        (void)SunController::repairPrimarySunTransform(nextDocument);
        reloadSettings = rendererSettingsFromDocument(nextDocument, reloadSettings);
        applyDocumentMaterialAssignments(nextDocument, result.assets);

        const SceneGpuBuildResult build = sceneBuilder_.build(nextDocument, &result.assets, reloadSettings);
        const std::optional<std::filesystem::path> cachePath = SceneCache::cachePathFor(result.path);

        std::unique_ptr<PathTracerRenderer> nextPathTracer = makePathTracer(
            build.sceneAsset.meshes.empty() ? nullptr : &build.sceneAsset,
            build.sceneAsset.meshes.empty() ? nullptr : &result.assets,
            cachePath,
            &reloadSettings);

        if (uiOverlay_) {
            uiOverlay_->invalidateViewportTexture();
        }
        cameraController_.releaseMouse(window_);

        assets_ = std::move(result.assets);
        importedScene_ = std::move(result.scene);
        gltfPath_ = result.path;
        scenePath_.reset();
        sceneDocument_ = std::move(nextDocument);
        sceneDocument_.clearDirty();
        sceneUnsavedDirty_ = true;
        undoStack_.clear();
        gpuSceneAsset_ = std::move(build.sceneAsset);
        gpuInstanceEntities_ = std::move(build.instanceEntities);
        retirePathTracer(std::move(pathTracer_));
        pathTracer_ = std::move(nextPathTracer);
        applyActiveSceneCamera();
        commandSystem_->setPathTracer(pathTracer_.get());
    } catch (const std::exception& error) {
        sceneLoadingStatus_ = "Import scene GPU finalization failed: " + std::string(error.what());
        notifications_.notify("Import scene GPU finalization failed", NotificationType::Error);
        std::cerr << sceneLoadingStatus_ << '\n';
        return;
    }

    sceneLoadingStatus_ = "Imported scene as new scene: " + result.path.string();
    if (uiOverlay_) {
        uiOverlay_->editor().editorPrefs().addRecentFile(result.path);
    }
    notifications_.notify("Scene imported as new scene", NotificationType::Success);
    std::cout << "Imported scene as new scene: " << result.path.string()
              << " meshes=" << importedScene_->meshes.size()
              << " materials=" << importedScene_->materials.size()
               << " textures=" << importedScene_->textures.size()
               << " nodes=" << importedScene_->nodes.size() << '\n';
}

Application::DirtyScenePromptResult Application::promptDirtySceneBefore(std::string_view action) const {
    if (!sceneUnsavedDirty_) {
        return DirtyScenePromptResult::Discard;
    }

    const std::string sceneName = scenePath_.has_value()
        ? scenePath_->filename().string()
        : (gltfPath_.has_value() ? gltfPath_->filename().string() : std::string("Untitled Scene"));
    const std::string message = "Save changes to " + sceneName + " before " + std::string(action) + "?\n\n"
        "Unsaved changes will be lost if you choose Do Not Save.";

#if defined(_WIN32)
    const std::wstring wideMessage = widenAscii(message);
    const int result = MessageBoxW(
        nullptr,
        wideMessage.c_str(),
        L"Unsaved Scene Changes",
        MB_ICONWARNING | MB_YESNOCANCEL | MB_DEFBUTTON1 | MB_APPLMODAL);
    if (result == IDYES) {
        return DirtyScenePromptResult::Save;
    }
    if (result == IDNO) {
        return DirtyScenePromptResult::Discard;
    }
#else
    std::cerr << message << "\nAction cancelled because modal dirty-scene prompts are only implemented on this platform.\n";
#endif
    return DirtyScenePromptResult::Cancel;
}

bool Application::saveCurrentSceneForDirtyPrompt() {
    std::filesystem::path savePath;
    if (scenePath_.has_value()) {
        savePath = *scenePath_;
    } else if (auto selected = saveSceneJsonFileDialog()) {
        savePath = *selected;
    }

    if (savePath.empty()) {
        notifications_.notify("Scene save cancelled", NotificationType::Warning);
        return false;
    }

    if (uiOverlay_ != nullptr) {
        uiOverlay_->editor().cameraBookmarks().serialize(sceneDocument_);
    }
    if (!sceneDocument_.saveJson(savePath)) {
        notifications_.notify("Scene save failed", NotificationType::Error);
        std::cerr << "Scene save failed: " << savePath.string() << '\n';
        return false;
    }

    scenePath_ = savePath;
    sceneDocument_.clearDirty();
    sceneUnsavedDirty_ = false;
    notifications_.notify("Scene saved", NotificationType::Success);
    std::cout << "Saved scene: " << savePath.string() << '\n';
    return true;
}

bool Application::confirmDestructiveSceneAction(std::string_view action) {
    switch (promptDirtySceneBefore(action)) {
    case DirtyScenePromptResult::Discard:
        return true;
    case DirtyScenePromptResult::Save:
        return saveCurrentSceneForDirtyPrompt();
    case DirtyScenePromptResult::Cancel:
        notifications_.notify("Scene action cancelled", NotificationType::Warning);
        return false;
    }
    return false;
}

bool Application::writeDefaultProjectScene(const ProjectContext& project, std::string_view templateName) {
    SceneDocument document;
    EntityId camera = document.registry().createEntity("Camera");
    Camera cameraComponent;
    cameraComponent.active = true;
    document.registry().addCamera(camera, cameraComponent);
    document.setActiveCamera(camera);

    EntityId sun = document.registry().createEntity("Sun Light");
    if (Entity* sunEntity = document.registry().entity(sun)) {
        sunEntity->sun = Sun{};
        sunEntity->transform = SunController::transformFromWorldAngles(
            document.registry(), *sunEntity, sunEntity->transform, 0.85f, glm::pi<float>());
    }
    document.setPrimarySun(sun);

    (void)document.registry().createEntity("Environment Light");
    if (templateName == "Lighting Test Scene") {
        EntityId light = document.registry().createEntity("Area Light");
        if (Entity* entity = document.registry().entity(light)) {
            entity->light = Light{};
            entity->transform.position = glm::vec3(0.0f, 2.2f, 0.0f);
        }
        (void)document.registry().createEntity("Post Process Volume");
    }

    document.clearDirty();
    return document.saveJson(project.startupScene);
}

bool Application::loadProjectStartupScene(const ProjectContext& project) {
    if (!std::filesystem::exists(project.startupScene)) {
        initializeFallbackSceneDocument();
        scenePath_.reset();
        gltfPath_.reset();
        importedScene_.reset();
        assets_.clear();
        undoStack_.clear();
        sceneUnsavedDirty_ = false;
        notifications_.notify("Project startup scene missing", NotificationType::Warning);
        return true;
    }

    if (!sceneDocument_.loadJson(project.startupScene)) {
        notifications_.notify("Project startup scene load failed", NotificationType::Error);
        return false;
    }

    scenePath_ = project.startupScene;
    gltfPath_ = sceneDocument_.sourceGltfPath();
    hdrPath_ = sceneDocument_.sourceHdrPath();
    assets_.clear();
    importedScene_.reset();
    if (gltfPath_.has_value() && std::filesystem::exists(*gltfPath_)) {
        try {
            GltfLoader loader(assets_);
            importedScene_ = loader.loadWithCache(*gltfPath_);
        } catch (const std::exception& error) {
            std::cerr << "Referenced glTF load failed for project startup scene: " << error.what() << '\n';
        }
    }
    (void)SunController::migrateLegacyDirectionalSun(sceneDocument_);
    (void)SunController::repairPrimarySunTransform(sceneDocument_);
    undoStack_.clear();
    sceneUnsavedDirty_ = false;
    return true;
}

bool Application::openProjectFromFile(const std::filesystem::path& projectFile, bool promptForDirtyScene) {
    if (promptForDirtyScene && !confirmDestructiveSceneAction("opening a project")) {
        return false;
    }

    ProjectContext project;
    std::string error;
    if (!loadProjectFile(projectFile, project, &error)) {
        notifications_.notify("Project open failed", NotificationType::Error);
        std::cerr << "Project open failed: " << projectFile.string() << " " << error << '\n';
        return false;
    }
    if (!createProjectFolders(project, true, &error)) {
        notifications_.notify("Project folder validation failed", NotificationType::Error);
        std::cerr << "Project folder validation failed: " << error << '\n';
        return false;
    }
    if (!assetRegistry_.load(project.assetRegistryPath, &error)) {
        notifications_.notify("Asset registry load failed", NotificationType::Error);
        std::cerr << "Asset registry load failed: " << error << '\n';
        return false;
    }

    project_ = project;
    if (uiOverlay_ != nullptr) {
        uiOverlay_->editor().editorPrefs().addRecentProject(project.projectFile);
        uiOverlay_->editor().editorPrefs().save(EditorPreferences::defaultPath());
    }

    if (!loadProjectStartupScene(*project_)) {
        project_.reset();
        return false;
    }

    sceneDocument_.markDirty(SceneUpdateKind::TopologyChanged);
    notifications_.notify("Project opened", NotificationType::Success);
    std::cout << "Opened project: " << project.projectFile.string() << '\n';
    return true;
}

bool Application::createProjectFromRequest(const CreateProjectRequest& request) {
    if (request.name.empty()) {
        notifications_.notify("Project name is required", NotificationType::Error);
        return false;
    }
    if (!confirmDestructiveSceneAction("creating a project")) {
        return false;
    }

    const std::filesystem::path projectRoot = request.location / request.name;
    ProjectContext project = makeProjectContext(request.name, projectRoot, projectRoot / (request.name + ".rtproject"));
    std::string error;
    if (!createProjectFolders(project, request.createDefaultContentFolders, &error)) {
        notifications_.notify("Project folder creation failed", NotificationType::Error);
        std::cerr << "Project folder creation failed: " << error << '\n';
        return false;
    }
    if (request.createDefaultScene && !writeDefaultProjectScene(project, request.templateName)) {
        notifications_.notify("Default scene creation failed", NotificationType::Error);
        return false;
    }
    if (!saveProjectFile(project)) {
        notifications_.notify("Project file save failed", NotificationType::Error);
        return false;
    }
    return openProjectFromFile(project.projectFile, false);
}

bool Application::closeCurrentProject() {
    if (!project_.has_value()) {
        return true;
    }
    if (!confirmDestructiveSceneAction("closing the project")) {
        return false;
    }
    if (assetRegistry_.dirty() && !assetRegistry_.save()) {
        notifications_.notify("Asset registry save failed", NotificationType::Error);
        return false;
    }
    if (uiOverlay_ != nullptr) {
        uiOverlay_->editor().editorPrefs().save(EditorPreferences::defaultPath());
    }
    project_.reset();
    assetRegistry_.clear();
    initializeFallbackSceneDocument();
    scenePath_.reset();
    gltfPath_.reset();
    importedScene_.reset();
    assets_.clear();
    undoStack_.clear();
    sceneUnsavedDirty_ = false;
    notifications_.notify("Project closed", NotificationType::Info);
    return true;
}

void Application::applyEditorRequests(const EditorRequests& requests, bool allowResourceRebuild) {
    if (!pathTracer_) {
        return;
    }

    if (!allowResourceRebuild) {
        if (requests.undo) {
            if (undoStack_.undo()) {
                sceneUnsavedDirty_ = true;
                notifications_.notify("Undo", NotificationType::Info);
            }
        }
        if (requests.redo) {
            if (undoStack_.redo()) {
                sceneUnsavedDirty_ = true;
                notifications_.notify("Redo", NotificationType::Info);
            }
        }
        if (requests.settings.has_value()) {
            sceneUnsavedDirty_ = true;
            applyRendererSettingsSafely(*requests.settings, false);
        }
        if (requests.previewEntityTransform.has_value()) {
            sceneUnsavedDirty_ = true;
            if (Entity* entity = sceneDocument_.registry().entity(requests.previewEntityTransform->entity)) {
                entity->transform = requests.previewEntityTransform->transform;
                entity->transform.dirty = true;
                sceneDocument_.markDirty(requests.previewEntityTransform->updateKind);
            }
        }
        (void)applyPendingSceneUpdate(false);
        if (requests.toggleDenoiser) {
            RendererSettings settings = pathTracer_->settings();
            settings.denoiserEnabled = !settings.denoiserEnabled;
            applyRendererSettingsSafely(settings, false);
        }
        if (requests.toggleDebugView) {
            RendererSettings settings = pathTracer_->settings();
            settings.debugView = nextDebugView(settings.debugView);
            applyRendererSettingsSafely(settings, false);
        }
        if (requests.cycleIntermediateView) {
            RendererSettings settings = pathTracer_->settings();
            constexpr int count = sizeof(intermediateViews) / sizeof(intermediateViews[0]);
            int idx = 0;
            for (int i = 0; i < count; ++i) {
                if (intermediateViews[i] == settings.debugView) { idx = (i + 1) % count; break; }
            }
            settings.debugView = intermediateViews[idx];
            applyRendererSettingsSafely(settings, false);
        }
        if (requests.resetAccumulation.has_value()) {
            pathTracer_->resetAccumulation(*requests.resetAccumulation);
        }
        if (requests.cameraMoveSpeed.has_value()) {
            cameraController_.setMoveSpeed(*requests.cameraMoveSpeed);
        }
        if (requests.resetCamera) {
            cameraController_.reset(*pathTracer_);
        }
        if (requests.sceneUpdate.has_value()) {
            sceneDocument_.markDirty(*requests.sceneUpdate);
        }
        return;
    }

    if (pendingPostFrameSettings_.has_value()) {
        RendererSettings pending = *pendingPostFrameSettings_;
        pendingPostFrameSettings_.reset();
        applyRendererSettingsSafely(pending, true);
    }

    if (requests.createProject.has_value()) {
        (void)createProjectFromRequest(*requests.createProject);
    }
    if (requests.openProject.has_value()) {
        (void)openProjectFromFile(requests.openProject->projectFile, true);
    }
    if (requests.saveProjectSettings && project_.has_value()) {
        const bool projectSaved = saveProjectFile(*project_);
        const bool registrySaved = !assetRegistry_.dirty() || assetRegistry_.save();
        if (projectSaved && registrySaved) {
            assetRegistry_.clearDirty();
            notifications_.notify("Project settings saved", NotificationType::Success);
        } else {
            notifications_.notify("Project settings save failed", NotificationType::Error);
        }
    }
    if (requests.closeProject) {
        (void)closeCurrentProject();
    }

    if (requests.loadHdr.has_value()) {
        try {
            pathTracer_->loadEnvironment(*requests.loadHdr);
            hdrPath_ = *requests.loadHdr;
            sceneUnsavedDirty_ = true;
            uiOverlay_->editor().editorPrefs().addRecentFile(*requests.loadHdr);
            sceneDocument_.setSourceHdrPath(hdrPath_);
            RendererSettings settings = pathTracer_->settings();
            settings.environmentEnabled = true;
            applyRendererSettingsSafely(settings, true);
            std::cout << "Loaded HDR from editor: " << requests.loadHdr->string() << '\n';
        } catch (const std::exception& error) {
            std::cerr << "Editor HDR load failed: " << error.what() << '\n';
        }
    }

    const std::optional<std::filesystem::path>& saveScenePath = requests.saveSceneAs.has_value()
        ? requests.saveSceneAs
        : (requests.saveScene.has_value() ? requests.saveScene : requests.saveSceneJson);
    if (saveScenePath.has_value()) {
        if (uiOverlay_ != nullptr) {
            uiOverlay_->editor().cameraBookmarks().serialize(sceneDocument_);
        }
        if (sceneDocument_.saveJson(*saveScenePath)) {
            scenePath_ = *saveScenePath;
            sceneDocument_.clearDirty();
            sceneUnsavedDirty_ = false;
            std::cout << "Saved scene: " << saveScenePath->string() << '\n';
        } else {
            std::cerr << "Scene save failed: " << saveScenePath->string() << '\n';
        }
    }

    const std::optional<std::filesystem::path>& openScenePath = requests.openScene.has_value()
        ? requests.openScene
        : requests.loadSceneJson;
    if (openScenePath.has_value()) {
        if (!confirmDestructiveSceneAction("opening another scene")) {
            std::cout << "Open scene cancelled: " << openScenePath->string() << '\n';
        } else {
            if (sceneDocument_.loadJson(*openScenePath)) {
                if (uiOverlay_ != nullptr) {
                    uiOverlay_->editor().cameraBookmarks().deserialize(sceneDocument_);
                }
                scenePath_ = *openScenePath;
                gltfPath_ = sceneDocument_.sourceGltfPath();
                hdrPath_ = sceneDocument_.sourceHdrPath();
                if (gltfPath_.has_value() && std::filesystem::exists(*gltfPath_)) {
                    try {
                        assets_.clear();
                        GltfLoader loader(assets_);
                        importedScene_ = loader.loadWithCache(*gltfPath_);
                    } catch (const std::exception& error) {
                        std::cerr << "Referenced glTF load failed for level: " << error.what() << '\n';
                    }
                }
                (void)SunController::migrateLegacyDirectionalSun(sceneDocument_);
                (void)SunController::repairPrimarySunTransform(sceneDocument_);
                undoStack_.clear();
                sceneUnsavedDirty_ = false;
                std::cout << "Opened scene: " << openScenePath->string() << '\n';
            } else {
                std::cerr << "Open scene failed: " << openScenePath->string() << '\n';
            }
        }
    }

    if (requests.newScene) {
        if (!confirmDestructiveSceneAction("creating a new scene")) {
            std::cout << "New scene cancelled\n";
        } else {
            initializeFallbackSceneDocument();
            scenePath_.reset();
            gltfPath_.reset();
            importedScene_.reset();
            assets_.clear();
            undoStack_.clear();
            sceneDocument_.markDirty(SceneUpdateKind::TopologyChanged);
            sceneUnsavedDirty_ = true;
            notifications_.notify("New scene created", NotificationType::Info);
        }
    }

    if (requests.materialUpdate.has_value()) {
        MaterialAsset* material = assets_.material(MaterialAssetHandle{requests.materialUpdate->materialId});
        if (material != nullptr) {
            *material = requests.materialUpdate->material;
            for (uint32_t meshIndex = 0; meshIndex < assets_.meshes().size(); ++meshIndex) {
                MeshAsset* mesh = assets_.mesh(MeshAssetHandle{meshIndex});
                if (mesh == nullptr) {
                    continue;
                }
                for (MeshPrimitiveAsset& primitive : mesh->primitives) {
                    if (primitive.material.index == requests.materialUpdate->materialId) {
                        updatePrimitiveAlphaClassification(primitive, material);
                    }
                }
            }
            bool gpuUpdated = false;
            if (gpuSceneAsset_.has_value()) {
                gpuUpdated = pathTracer_->updateMaterials(*gpuSceneAsset_, assets_);
            }
            if (!gpuUpdated) {
                pathTracer_->resetAccumulation(AccumulationResetReason::MaterialChanged);
            }
            sceneDocument_.markDirty(SceneUpdateKind::MaterialOnly);
            sceneUnsavedDirty_ = true;
        }
    }

    if (requests.materialAssignment.has_value()) {
        MeshAsset* mesh = assets_.mesh(requests.materialAssignment->mesh);
        if (mesh != nullptr && requests.materialAssignment->primitiveIndex < mesh->primitives.size()) {
            mesh->primitives[requests.materialAssignment->primitiveIndex].material = requests.materialAssignment->material;
            updatePrimitiveAlphaClassification(
                mesh->primitives[requests.materialAssignment->primitiveIndex],
                assets_.material(requests.materialAssignment->material));
            sceneDocument_.markDirty(SceneUpdateKind::TopologyChanged);
            sceneUnsavedDirty_ = true;
        }
    }

    SceneOperations sceneOps(sceneDocument_, &sceneEventBus_);
    sceneOps.setUndoStack(&undoStack_);
    if (requests.createEntity.has_value()) {
        const EditorEntityCreateRequest& create = *requests.createEntity;
        EntityId created{};
        switch (create.kind) {
        case EditorEntityCreateKind::Empty:
            created = sceneOps.createEntity("Entity", create.parent);
            break;
        case EditorEntityCreateKind::Camera:
            created = sceneOps.createEntity("Camera", create.parent);
            if (created.valid()) {
                Camera camera;
                camera.active = true;
                (void)sceneOps.addCameraComponent(created, camera);
            }
            break;
        case EditorEntityCreateKind::Light:
            created = sceneOps.createEntity("Point Light", create.parent);
            if (created.valid()) {
                (void)sceneOps.addLightComponent(created, Light{});
            }
            break;
        }
        if (created.valid()) {
            sceneUnsavedDirty_ = true;
            if (Entity* entity = sceneDocument_.registry().entity(created)) {
                glm::vec3 forward = cameraController_.direction();
                if (glm::dot(forward, forward) <= 0.0f) {
                    forward = glm::vec3(0.0f, 0.0f, -1.0f);
                } else {
                    forward = glm::normalize(forward);
                }
                entity->transform.position = cameraController_.position() + forward * 2.5f;
                entity->transform.dirty = true;
                sceneDocument_.markDirty(SceneUpdateKind::TopologyChanged);
            }
        }
    }

    if (requests.ensurePrimarySun) {
        (void)SunController::ensurePrimarySun(sceneDocument_);
        sceneUnsavedDirty_ = true;
    }

    if (requests.duplicateEntity.has_value()) {
        (void)sceneOps.duplicateEntity(*requests.duplicateEntity);
        sceneUnsavedDirty_ = true;
    }

    if (requests.deleteEntity.has_value()) {
        (void)sceneOps.deleteEntity(*requests.deleteEntity);
        sceneUnsavedDirty_ = true;
    }

    if (requests.reparentEntity.has_value()) {
        const auto [child, newParent] = *requests.reparentEntity;
        (void)sceneOps.reparentEntity(child, newParent);
        sceneUnsavedDirty_ = true;
    }

    if (requests.setEntityVisibility.has_value()) {
        (void)sceneOps.setVisibility(requests.setEntityVisibility->entity, requests.setEntityVisibility->value);
        sceneUnsavedDirty_ = true;
    }

    if (requests.setEntityLocked.has_value()) {
        (void)sceneOps.setLocked(requests.setEntityLocked->entity, requests.setEntityLocked->value);
        sceneUnsavedDirty_ = true;
    }

    if (requests.setEntityTransform.has_value()) {
        sceneOps.setTransformGizmoDrag(
            requests.setEntityTransform->entity,
            requests.setEntityTransform->oldTransform,
            requests.setEntityTransform->newTransform);
        sceneUnsavedDirty_ = true;
    }

    if (requests.addComponent.has_value()) {
        switch (requests.addComponent->kind) {
        case EditorComponentKind::Light:
            (void)sceneOps.addLightComponent(requests.addComponent->entity, Light{});
            break;
        case EditorComponentKind::Sun:
            (void)sceneOps.addSunComponent(requests.addComponent->entity, Sun{});
            break;
        case EditorComponentKind::Camera:
            (void)sceneOps.addCameraComponent(requests.addComponent->entity, Camera{});
            break;
        case EditorComponentKind::MeshRenderer:
            (void)sceneOps.addMeshRendererComponent(requests.addComponent->entity, MeshRenderer{});
            break;
        }
        sceneUnsavedDirty_ = true;
    }

    if (requests.setLight.has_value()) {
        (void)sceneOps.setLight(
            requests.setLight->entity,
            requests.setLight->oldLight,
            requests.setLight->newLight);
        sceneUnsavedDirty_ = true;
    }

    if (requests.setSun.has_value()) {
        (void)sceneOps.setSun(
            requests.setSun->entity,
            requests.setSun->oldSun,
            requests.setSun->newSun);
        sceneUnsavedDirty_ = true;
    }

    if (requests.setCamera.has_value()) {
        (void)sceneOps.setCamera(
            requests.setCamera->entity,
            requests.setCamera->oldCamera,
            requests.setCamera->newCamera,
            requests.setCamera->oldActiveCamera,
            requests.setCamera->newActiveCamera);
        sceneUnsavedDirty_ = true;
    }

    if (requests.focusOnEntity.has_value()) {
        const Entity* entity = sceneDocument_.registry().entity(*requests.focusOnEntity);
        if (entity != nullptr) {
            const glm::vec3 target = glm::vec3(entityWorldMatrix(sceneDocument_.registry(), *entity)[3]);
            const glm::vec3 position = cameraController_.position();
            glm::vec3 direction = target - position;
            if (glm::dot(direction, direction) > 0.0001f) {
                cameraController_.setPose(position, glm::normalize(direction), *pathTracer_);
            }
        }
    }

    if (requests.saveCameraBookmark.has_value()) {
        uiOverlay_->editor().cameraBookmarks().saveBookmark(
            cameraController_, *requests.saveCameraBookmark, &pathTracer_->settings());
    }
    if (requests.loadCameraBookmarkIndex.has_value()) {
        const auto& bookmarks = uiOverlay_->editor().cameraBookmarks().bookmarks();
        const size_t index = *requests.loadCameraBookmarkIndex;
        if (index < bookmarks.size()) {
            RendererSettings settings = pathTracer_->settings();
            uiOverlay_->editor().cameraBookmarks().loadBookmark(
                bookmarks[index], cameraController_, *pathTracer_, &settings);
            applyRendererSettingsSafely(settings, true);
        }
    }
    if (requests.deleteCameraBookmarkIndex.has_value()) {
        uiOverlay_->editor().cameraBookmarks().deleteBookmark(*requests.deleteCameraBookmarkIndex);
    }
    if (requests.removeFavorite.has_value()) {
        uiOverlay_->editor().editorPrefs().removeFavorite(*requests.removeFavorite);
    }

    (void)applyPendingSceneUpdate(allowResourceRebuild);

    if (requests.reloadShaders) {
        reloadShadersFromEditor();
    }

    const std::optional<std::filesystem::path>& importScenePath = requests.importSceneAsNewScene.has_value()
        ? requests.importSceneAsNewScene
        : requests.loadGltf;
    if (importScenePath.has_value()) {
        if (!confirmDestructiveSceneAction("importing a scene as a new scene")) {
            std::cout << "Import Scene as New Scene cancelled: " << importScenePath->string() << '\n';
        } else {
            requestGltfSceneLoad(*importScenePath);
        }
    }

    if (requests.importAsset.has_value()) {
        notifications_.notify("Import Asset is not implemented yet", NotificationType::Warning);
        std::cout << "Import Asset requested (non-mutating skeleton): " << requests.importAsset->string() << '\n';
    }

    if (requests.importAndPlace.has_value()) {
        notifications_.notify("Import and Place is not implemented yet", NotificationType::Warning);
        std::cout << "Import and Place requested: " << requests.importAndPlace->string() << '\n';
    }

    if (requests.mergeScene.has_value()) {
        notifications_.notify("Merge Scene is not implemented yet", NotificationType::Warning);
        std::cout << "Merge Scene requested: " << requests.mergeScene->string() << '\n';
    }

    if (requests.exit && window_ != nullptr && confirmDestructiveSceneAction("exiting")) {
        glfwSetWindowShouldClose(window_, GLFW_TRUE);
    }
}

void Application::applyValidationCameraMotion(uint32_t frameIndex) {
    if (!validationCameraMotion_ || pathTracer_ == nullptr) {
        return;
    }
    const float angle = static_cast<float>(frameIndex) * 0.035f;
    const float radius = 4.2f;
    const glm::vec3 target{0.0f, 0.55f, 0.0f};
    const glm::vec3 position{
        std::sin(angle) * radius,
        0.75f + std::sin(angle * 0.37f) * 0.25f,
        std::cos(angle) * radius};
    cameraController_.setPose(position, glm::normalize(target - position), *pathTracer_);
}

void Application::applyValidationObjectMotion(uint32_t frameIndex) {
    if (!validationObjectMotion_ || pathTracer_ == nullptr) {
        return;
    }

    Entity* entity = validationObjectMotionEntity_.valid()
        ? sceneDocument_.registry().entity(validationObjectMotionEntity_)
        : nullptr;
    if (entity == nullptr || !entity->meshRenderer.has_value()) {
        validationObjectMotionEntity_ = {};
        for (Entity* candidate : sceneDocument_.registry().entities()) {
            if (candidate != nullptr && candidate->meshRenderer.has_value()) {
                validationObjectMotionEntity_ = candidate->id;
                validationObjectMotionBaseTransform_ = candidate->transform;
                entity = candidate;
                break;
            }
        }
    }
    if (entity == nullptr || !entity->meshRenderer.has_value()) {
        return;
    }

    constexpr float kAmplitude = 0.65f;
    constexpr float kAngularStep = 0.42f;
    const float phase = static_cast<float>(frameIndex) * kAngularStep;
    entity->transform = validationObjectMotionBaseTransform_;
    entity->transform.position.x += std::sin(phase) * kAmplitude;
    entity->transform.rotationEuler.y += phase * 0.35f;
    entity->transform.dirty = true;
    const RendererSettings currentSettings = pathTracer_->settings();
    RenderSettings& documentSettings = sceneDocument_.renderSettings();
    documentSettings.motionBlurEnabled = currentSettings.motionBlurEnabled;
    documentSettings.motionBlurShutterOpen = currentSettings.motionBlurShutterOpen;
    documentSettings.motionBlurShutterClose = currentSettings.motionBlurShutterClose;
    sceneDocument_.markDirty(SceneUpdateKind::TransformOnly);
    (void)applyPendingSceneUpdate(false);
}

void Application::beginSunDragArm(bool dragEligible) {
    if (window_ == nullptr || sunDrag_.phase != SunDragPhase::Idle) {
        return;
    }
    glfwGetCursorPos(window_, &sunDrag_.startMouseX, &sunDrag_.startMouseY);
    sunDrag_.lastMouseX = sunDrag_.startMouseX;
    sunDrag_.lastMouseY = sunDrag_.startMouseY;
    sunDrag_.armedTimeSeconds = glfwGetTime();
    sunDrag_.dragEligible = dragEligible;
    sunDrag_.suppressOpenLevel = false;
    if (pathTracer_ != nullptr) {
        const RendererSettings settings = pathTracer_->settings();
        sunDrag_.elevation = settings.sunElevation;
        sunDrag_.azimuth = settings.sunAzimuth;
    }
    sunDrag_.phase = SunDragPhase::Armed;
}

void Application::startSunDrag(double mouseX, double mouseY) {
    if (window_ == nullptr || pathTracer_ == nullptr || sunDrag_.phase != SunDragPhase::Armed) {
        return;
    }

    cameraController_.releaseMouse(window_);
    sunDrag_.beforeDocument = sceneDocument_;
    sunDrag_.entity = SunController::ensurePrimarySun(sceneDocument_);

    Entity* sun = sceneDocument_.registry().entity(sunDrag_.entity);
    if (sun == nullptr || !sun->sun.has_value()) {
        finishSunDrag(true);
        return;
    }

    sunDrag_.originalTransform = sun->transform;
    SunController::anglesFromWorldTransform(sceneDocument_.registry(), *sun, sunDrag_.elevation, sunDrag_.azimuth);
    sunDrag_.lastMouseX = mouseX;
    sunDrag_.lastMouseY = mouseY;
    sunDrag_.previousCursorMode = glfwGetInputMode(window_, GLFW_CURSOR);
    glfwSetInputMode(window_, GLFW_CURSOR, GLFW_CURSOR_DISABLED);
    sunDrag_.phase = SunDragPhase::Dragging;
}

void Application::updateSunDrag(double mouseX, double mouseY) {
    if (sunDrag_.phase != SunDragPhase::Dragging) {
        return;
    }
    Entity* sun = sceneDocument_.registry().entity(sunDrag_.entity);
    if (sun == nullptr || !sun->sun.has_value()) {
        finishSunDrag(true);
        return;
    }

    constexpr float sensitivity = 0.0035f;
    const double dx = mouseX - sunDrag_.lastMouseX;
    const double dy = mouseY - sunDrag_.lastMouseY;
    sunDrag_.lastMouseX = mouseX;
    sunDrag_.lastMouseY = mouseY;
    if (dx == 0.0 && dy == 0.0) {
        return;
    }

    sunDrag_.azimuth -= static_cast<float>(dx) * sensitivity;
    sunDrag_.elevation = std::clamp(
        sunDrag_.elevation - static_cast<float>(dy) * sensitivity,
        -0.20f,
        1.45f);
    sun->transform = SunController::transformFromWorldAngles(sceneDocument_.registry(), *sun, sun->transform, sunDrag_.elevation, sunDrag_.azimuth);
    sceneDocument_.markDirty(SceneUpdateKind::LightOnly);
    (void)applyPendingSceneUpdate(false);
}

void Application::finishSunDrag(bool cancel) {
    if (sunDrag_.phase == SunDragPhase::Idle) {
        return;
    }
    const SunDragPhase phase = sunDrag_.phase;
    if (phase == SunDragPhase::Dragging && window_ != nullptr) {
        glfwSetInputMode(window_, GLFW_CURSOR, sunDrag_.previousCursorMode);
    }

    if (phase == SunDragPhase::Dragging) {
        if (cancel && sunDrag_.beforeDocument.has_value()) {
            sceneDocument_ = *sunDrag_.beforeDocument;
            sceneDocument_.markDirty(SceneUpdateKind::LightOnly);
            (void)applyPendingSceneUpdate(false);
        } else if (sunDrag_.beforeDocument.has_value()) {
            SceneOperations sceneOps(sceneDocument_, &sceneEventBus_);
            sceneOps.setUndoStack(&undoStack_);
            sceneOps.commitSunDrag(std::move(*sunDrag_.beforeDocument), SceneUpdateKind::LightOnly);
        }
    }

    sunDrag_ = SunDragState{};
}

void Application::processSunDragControls(bool shortcutsBlocked, bool viewportHovered, bool viewportInteraction, bool ctrlDown) {
    if (window_ == nullptr || pathTracer_ == nullptr) {
        return;
    }
    const bool lDown = glfwGetKey(window_, GLFW_KEY_L) == GLFW_PRESS;
    const bool escapeDown = glfwGetKey(window_, GLFW_KEY_ESCAPE) == GLFW_PRESS;
    const bool focused = glfwGetWindowAttrib(window_, GLFW_FOCUSED) == GLFW_TRUE;

    if (sunDrag_.phase == SunDragPhase::Idle) {
        if (!shortcutsBlocked && ctrlDown && pressedOnce(GLFW_KEY_L)) {
            beginSunDragArm(viewportHovered || viewportInteraction);
        }
        return;
    }

    if (sunDrag_.phase == SunDragPhase::Armed) {
        if (escapeDown || !focused) {
            finishSunDrag(true);
            return;
        }
        double mouseX = 0.0;
        double mouseY = 0.0;
        glfwGetCursorPos(window_, &mouseX, &mouseY);
        constexpr double dragThresholdPixels = 4.0;
        const double dx = mouseX - sunDrag_.startMouseX;
        const double dy = mouseY - sunDrag_.startMouseY;
        const double dragDistanceSq = dx * dx + dy * dy;
        if (!ctrlDown || !lDown) {
            constexpr double openLevelTapSeconds = 0.25;
            const bool quickTap = (glfwGetTime() - sunDrag_.armedTimeSeconds) <= openLevelTapSeconds &&
                                  dragDistanceSq < dragThresholdPixels * dragThresholdPixels;
            if (!sunDrag_.suppressOpenLevel && quickTap) {
                pendingOpenLevel_ = true;
            }
            finishSunDrag(false);
            return;
        }
        if (dragDistanceSq >= dragThresholdPixels * dragThresholdPixels) {
            if (sunDrag_.dragEligible) {
                startSunDrag(mouseX, mouseY);
            } else {
                sunDrag_.suppressOpenLevel = true;
            }
        }
        return;
    }

    if (sunDrag_.phase == SunDragPhase::Dragging) {
        if (escapeDown || !focused) {
            finishSunDrag(true);
            return;
        }
        if (!ctrlDown || !lDown) {
            finishSunDrag(false);
            return;
        }
        double mouseX = 0.0;
        double mouseY = 0.0;
        glfwGetCursorPos(window_, &mouseX, &mouseY);
        updateSunDrag(mouseX, mouseY);
    }
}

bool Application::applyPendingSceneUpdate(bool allowResourceRebuild) {
    if (!pathTracer_ || !sceneDocument_.dirty()) {
        return false;
    }

    const SceneUpdateKind pendingKind = sceneDocument_.pendingUpdate();
    SceneUpdateRoute route = SceneUpdateRouter::route(pendingKind);
    pathTracer_->validationLog().recordSceneUpdateRoute(
        sceneUpdateKindName(route.kind),
        sceneUpdateGpuActionName(route.action));
    if (route.action == SceneUpdateGpuAction::None) {
        sceneDocument_.clearDirty();
        return true;
    }
    if (!allowResourceRebuild &&
        route.requiresRendererRebuild) {
        return false;
    }

    std::optional<SceneGpuBuildResult> build;
    auto ensureBuild = [&]() -> SceneGpuBuildResult& {
        if (!build.has_value()) {
            build = sceneBuilder_.build(sceneDocument_, &assets_, pathTracer_->settings());
        }
        return *build;
    };
    if (route.requiresGpuSceneBuild) {
        applyRendererSettingsSafely(ensureBuild().rendererSettings, allowResourceRebuild);
    }

    auto rebuildRenderer = [&]() {
        SceneGpuBuildResult& sceneBuild = ensureBuild();
        const RendererSettings previousSettings = pathTracer_->settings();
        if (uiOverlay_) {
            uiOverlay_->invalidateViewportTexture();
        }
        gpuSceneAsset_ = sceneBuild.sceneAsset;
        gpuInstanceEntities_ = sceneBuild.instanceEntities;
        rebuildGpuSceneAsset();
        std::unique_ptr<PathTracerRenderer> nextPathTracer = makePathTracer(
            gpuSceneAsset_.has_value() && !gpuSceneAsset_->meshes.empty() ? &*gpuSceneAsset_ : nullptr,
            gpuSceneAsset_.has_value() && !gpuSceneAsset_->meshes.empty() ? &assets_ : nullptr,
            gltfPath_.has_value() ? SceneCache::cachePathFor(*gltfPath_) : std::optional<std::filesystem::path>{},
            &previousSettings);
        retirePathTracer(std::move(pathTracer_));
        pathTracer_ = std::move(nextPathTracer);
        applyActiveSceneCamera();
        pathTracer_->resetAccumulation(route.resetReason);
        commandSystem_->setPathTracer(pathTracer_.get());
    };

    switch (route.action) {
    case SceneUpdateGpuAction::UpdateCamera:
        applyActiveSceneCamera();
        break;
    case SceneUpdateGpuAction::UpdateLights:
        gpuSceneAsset_ = ensureBuild().sceneAsset;
        gpuInstanceEntities_ = ensureBuild().instanceEntities;
        applyRendererSettingsSafely(ensureBuild().rendererSettings, allowResourceRebuild);
        if (!pathTracer_->updateSceneLights(*gpuSceneAsset_)) {
            pathTracer_->resetAccumulation(route.resetReason);
        }
        break;
    case SceneUpdateGpuAction::UpdateMaterials:
        gpuSceneAsset_ = ensureBuild().sceneAsset;
        gpuInstanceEntities_ = ensureBuild().instanceEntities;
        if (!pathTracer_->updateMaterials(*gpuSceneAsset_, assets_)) {
            pathTracer_->resetAccumulation(route.resetReason);
        }
        break;
    case SceneUpdateGpuAction::UpdateEnvironment:
        gpuSceneAsset_ = ensureBuild().sceneAsset;
        gpuInstanceEntities_ = ensureBuild().instanceEntities;
        if (route.resetsAccumulation) {
            pathTracer_->resetAccumulation(route.resetReason);
        }
        break;
    case SceneUpdateGpuAction::ApplyRendererSettings:
        applyRendererSettingsSafely(rendererSettingsFromDocument(sceneDocument_, pathTracer_->settings()), allowResourceRebuild);
        if (route.resetsAccumulation) {
            pathTracer_->resetAccumulation(route.resetReason);
        }
        break;
    case SceneUpdateGpuAction::UpdateVisibility:
        gpuSceneAsset_ = ensureBuild().sceneAsset;
        gpuInstanceEntities_ = ensureBuild().instanceEntities;
        if (!pathTracer_->updateSceneVisibility(*gpuSceneAsset_, assets_) && allowResourceRebuild) {
            rebuildRenderer();
        }
        break;
    case SceneUpdateGpuAction::UpdateTransforms:
        gpuSceneAsset_ = ensureBuild().sceneAsset;
        gpuInstanceEntities_ = ensureBuild().instanceEntities;
        if (!pathTracer_->updateSceneTransforms(*gpuSceneAsset_, assets_)) {
            if (!allowResourceRebuild) {
                return false;
            }
            rebuildRenderer();
        }
        break;
    case SceneUpdateGpuAction::RebuildTopology:
        if (!allowResourceRebuild) {
            return false;
        }
        rebuildRenderer();
        break;
    case SceneUpdateGpuAction::None:
        break;
    }

    sceneDocument_.clearDirty();
    if (route.action == SceneUpdateGpuAction::RebuildTopology) {
        notifications_.notify("Scene topology rebuilt", NotificationType::Info);
    }
    return true;
}

void Application::applyRendererSettingsSafely(const RendererSettings& settings, bool allowRenderResolutionChange) {
    if (pathTracer_ == nullptr) {
        return;
    }

    const RendererSettings current = pathTracer_->settings();
    const bool renderResolutionChanged =
        std::abs(settings.renderResolutionScale - current.renderResolutionScale) > 0.0001f;
    if (!renderResolutionChanged || allowRenderResolutionChange) {
        if (pathTracer_->applySettings(settings)) {
            syncDocumentRenderSettings(sceneDocument_, settings);
        }
        return;
    }

    RendererSettings immediate = settings;
    immediate.renderResolutionScale = current.renderResolutionScale;
    if (pathTracer_->applySettings(immediate)) {
        syncDocumentRenderSettings(sceneDocument_, settings);
    }
    pendingPostFrameSettings_ = settings;
}

void Application::reloadShadersFromEditor() {
    if (!pathTracer_ || !commandSystem_) {
        return;
    }
    const RendererSettings previousSettings = pathTracer_->settings();
    std::unique_ptr<PathTracerRenderer> nextPathTracer = makePathTracer(
        gpuSceneAsset_.has_value() && !gpuSceneAsset_->meshes.empty() ? &*gpuSceneAsset_ : nullptr,
        gpuSceneAsset_.has_value() && !gpuSceneAsset_->meshes.empty() ? &assets_ : nullptr,
        gltfPath_.has_value() ? SceneCache::cachePathFor(*gltfPath_) : std::optional<std::filesystem::path>{},
        &previousSettings);
    if (uiOverlay_) {
        uiOverlay_->invalidateViewportTexture();
        uiOverlay_->editor().invalidateAssetThumbnails();
    }
    retirePathTracer(std::move(pathTracer_));
    pathTracer_ = std::move(nextPathTracer);
    applyActiveSceneCamera();
    pathTracer_->resetAccumulation(AccumulationResetReason::ShaderReloaded);
    commandSystem_->setPathTracer(pathTracer_.get());
    notifications_.notify("Shaders reloaded", NotificationType::Success);
    std::cout << "Reloaded shaders from editor.\n";
}

void Application::retirePathTracer(std::unique_ptr<PathTracerRenderer> renderer) {
    if (renderer == nullptr) {
        return;
    }
    retiredPathTracers_.push_back(RetiredPathTracer{
        .renderer = std::move(renderer),
        .releaseFrame = frameSerial_ + CommandSystem::framesInFlight + 1u,
    });
}

void Application::releaseRetiredPathTracers() {
    retiredPathTracers_.erase(
        std::remove_if(
            retiredPathTracers_.begin(),
            retiredPathTracers_.end(),
            [this](const RetiredPathTracer& retired) {
                return frameSerial_ >= retired.releaseFrame;
            }),
        retiredPathTracers_.end());
}

std::unique_ptr<PathTracerRenderer> Application::makePathTracer(
    const SceneAsset* sceneAsset,
    const AssetManager* assets,
    std::optional<std::filesystem::path> sceneCachePath,
    const RendererSettings* settingsToRestore) {
    const auto projectRoot = resolveProjectRoot();
    const auto shaderDir = projectRoot / "native" / "vulkan" / "shaders";
    const auto shaderOutDir = projectRoot / "native" / "vulkan" / "build" / "shaders";
    auto renderer = std::make_unique<PathTracerRenderer>(
        *context_,
        *allocator_,
        *uploader_,
        swapchain_->format(),
        shaderDir,
        shaderOutDir,
        debugView_,
        sceneAsset,
        sceneAsset != nullptr ? assets : nullptr,
        hdrPath_,
        std::move(sceneCachePath),
        !disableResourceAliasing_,
        settingsToRestore);
    if (settingsToRestore != nullptr) {
        renderer->applySettings(*settingsToRestore);
    }
    return renderer;
}

void Application::createPathTracer(const RendererSettings* settingsToRestore) {
    std::optional<std::filesystem::path> cachePath;
    if (gltfPath_.has_value()) {
        cachePath = SceneCache::cachePathFor(*gltfPath_);
    }
    const SceneAsset* sceneAsset = gpuSceneAsset_.has_value() && !gpuSceneAsset_->meshes.empty() ? &*gpuSceneAsset_ : nullptr;
    pathTracer_ = makePathTracer(sceneAsset, sceneAsset != nullptr ? &assets_ : nullptr, std::move(cachePath), settingsToRestore);
}

void Application::applyActiveSceneCamera() {
    if (pathTracer_ == nullptr || !sceneDocument_.activeCamera().valid()) {
        return;
    }

    const Entity* cameraEntity = sceneDocument_.registry().entity(sceneDocument_.activeCamera());
    if (cameraEntity == nullptr || !cameraEntity->camera.has_value()) {
        return;
    }

    const glm::mat4 transform = entityWorldMatrix(sceneDocument_.registry(), *cameraEntity);
    const glm::vec3 position = glm::vec3(transform[3]);
    glm::vec3 forward = glm::mat3(transform) * glm::vec3(0.0f, 0.0f, -1.0f);
    if (glm::dot(forward, forward) <= 0.0f) {
        forward = glm::vec3(0.0f, 0.0f, -1.0f);
    }
    pathTracer_->setCameraFovY(cameraEntity->camera->verticalFovRadians);
    cameraController_.setPose(position, forward, *pathTracer_);
}

void Application::syncActiveSceneCameraFromController() {
    const EntityId active = sceneDocument_.activeCamera();
    if (!active.valid()) {
        return;
    }
    Entity* cameraEntity = sceneDocument_.registry().entity(active);
    if (cameraEntity == nullptr || !cameraEntity->camera.has_value()) {
        return;
    }

    glm::vec3 localPosition = cameraController_.position();
    glm::vec3 localForward = cameraController_.direction();
    if (cameraEntity->parent.valid()) {
        if (const Entity* parent = sceneDocument_.registry().entity(cameraEntity->parent)) {
            const glm::mat4 invParent = glm::inverse(entityWorldMatrix(sceneDocument_.registry(), *parent));
            localPosition = glm::vec3(invParent * glm::vec4(localPosition, 1.0f));
            localForward = glm::normalize(glm::mat3(invParent) * localForward);
        }
    }

    const glm::vec3 zAxis = -glm::normalize(localForward);
    glm::vec3 xAxis = glm::cross(glm::vec3(0.0f, 1.0f, 0.0f), zAxis);
    if (glm::dot(xAxis, xAxis) <= 1.0e-6f) {
        xAxis = glm::cross(glm::vec3(0.0f, 0.0f, 1.0f), zAxis);
    }
    xAxis = glm::normalize(xAxis);
    const glm::vec3 yAxis = glm::normalize(glm::cross(zAxis, xAxis));
    const glm::mat3 rotation(xAxis, yAxis, zAxis);
    cameraEntity->transform.position = localPosition;
    cameraEntity->transform.rotationEuler = glm::eulerAngles(glm::normalize(glm::quat_cast(rotation)));
    cameraEntity->transform.dirty = true;
    sceneDocument_.markDirty(SceneUpdateKind::CameraOnly);
}

void Application::rebuildGpuSceneAsset() {
    const RendererSettings settings = pathTracer_ != nullptr ? pathTracer_->settings() : RendererSettings{};
    (void)SunController::migrateLegacyDirectionalSun(sceneDocument_);
    (void)SunController::repairPrimarySunTransform(sceneDocument_);
    applyDocumentMaterialAssignments(sceneDocument_, assets_);
    SceneGpuBuildResult build = sceneBuilder_.build(sceneDocument_, &assets_, settings);
    gpuSceneAsset_ = std::move(build.sceneAsset);
    gpuInstanceEntities_ = std::move(build.instanceEntities);
}

void Application::initializeFallbackSceneDocument() {
    sceneDocument_ = SceneDocument{};
    EntityId camera = sceneDocument_.registry().createEntity("Camera");
    Camera cameraComponent;
    cameraComponent.active = true;
    sceneDocument_.registry().addCamera(camera, cameraComponent);
    sceneDocument_.setActiveCamera(camera);
    (void)sceneDocument_.registry().createEntity("Cornell Fallback");
    EntityId sun = sceneDocument_.registry().createEntity("Sun");
    if (Entity* sunEntity = sceneDocument_.registry().entity(sun)) {
        sunEntity->sun = Sun{};
        sunEntity->transform = SunController::transformFromWorldAngles(
            sceneDocument_.registry(),
            *sunEntity,
            sunEntity->transform,
            0.97f,
            glm::pi<float>());
    }
    sceneDocument_.setPrimarySun(sun);
    sceneDocument_.clearDirty();
    sceneDocument_.markDirty(SceneUpdateKind::TopologyChanged);
}

void Application::processRuntimeControls(float deltaSeconds) {
    if (!pathTracer_) {
        return;
    }

    const bool uiWantsTextInput = uiOverlay_ != nullptr && uiOverlay_->wantsTextInput();
    const bool shortcutsBlocked = uiWantsTextInput;
    const bool viewportInteraction = uiOverlay_ != nullptr && uiOverlay_->viewportInteractionActive();
    const bool viewportHovered = uiOverlay_ != nullptr && uiOverlay_->viewportHovered();
    const bool cameraCaptured = cameraController_.mouseCaptured();
    const bool ctrlDown =
        glfwGetKey(window_, GLFW_KEY_LEFT_CONTROL) == GLFW_PRESS ||
        glfwGetKey(window_, GLFW_KEY_RIGHT_CONTROL) == GLFW_PRESS;
    processSunDragControls(shortcutsBlocked, viewportHovered, viewportInteraction, ctrlDown);
    const bool sunDragCapturing = sunDrag_.phase != SunDragPhase::Idle;
    const bool cameraMoved = cameraController_.update(
        window_,
        deltaSeconds,
        *pathTracer_,
        !sunDragCapturing && (viewportHovered || cameraCaptured),
        !sunDragCapturing && (viewportInteraction || cameraCaptured) && !uiWantsTextInput);
    if (cameraMoved) {
        syncActiveSceneCameraFromController();
    }

    RendererSettings settings = pathTracer_->settings();
    bool changed = false;
    if (!shortcutsBlocked && ctrlDown && pressedOnce(GLFW_KEY_Z)) {
        if (undoStack_.undo()) {
            notifications_.notify("Undo", NotificationType::Info);
            (void)applyPendingSceneUpdate(true);
        }
    }
    if (!shortcutsBlocked && ctrlDown && pressedOnce(GLFW_KEY_Y)) {
        if (undoStack_.redo()) {
            notifications_.notify("Redo", NotificationType::Info);
            (void)applyPendingSceneUpdate(true);
        }
    }
    if (!shortcutsBlocked && pressedOnce(GLFW_KEY_F1)) {
        settings.debugView = nextDebugView(settings.debugView);
        changed = true;
    }
    if (!shortcutsBlocked && pressedOnce(GLFW_KEY_0)) {
        settings.debugView = RendererDebugView::Beauty;
        changed = true;
    }
    if (!shortcutsBlocked && pressedOnce(GLFW_KEY_1)) {
        settings.toneMapper = ToneMapper::Linear;
        changed = true;
    }
    if (!shortcutsBlocked && pressedOnce(GLFW_KEY_2)) {
        settings.toneMapper = ToneMapper::Reinhard;
        changed = true;
    }
    if (!shortcutsBlocked && pressedOnce(GLFW_KEY_3)) {
        settings.toneMapper = ToneMapper::ACES;
        changed = true;
    }
    if (!shortcutsBlocked && pressedOnce(GLFW_KEY_4)) {
        settings.toneMapper = ToneMapper::PBRNeutral;
        changed = true;
    }
    if (!shortcutsBlocked && pressedOnce(GLFW_KEY_5)) {
        settings.toneMapper = ToneMapper::AgX;
        changed = true;
    }
    if (!shortcutsBlocked && pressedOnce(GLFW_KEY_6)) {
        settings.autoExposureEnabled = !settings.autoExposureEnabled;
        changed = true;
    }
    if (!shortcutsBlocked && pressedOnce(GLFW_KEY_F11)) {
        toggleBorderlessFullscreen();
    }
    if (!shortcutsBlocked && pressedOnce(GLFW_KEY_F2)) {
        settings.denoiserEnabled = !settings.denoiserEnabled;
        changed = true;
    }
    if (!shortcutsBlocked && pressedOnce(GLFW_KEY_F3)) {
        settings.denoiseWhileMoving = !settings.denoiseWhileMoving;
        changed = true;
    }
    if (!shortcutsBlocked && pressedOnce(GLFW_KEY_F4)) {
        const bool hadPrimarySun = SunController::primarySunEntity(sceneDocument_).valid();
        EntityId sunId = SunController::ensurePrimarySun(sceneDocument_);
        if (Entity* sun = sceneDocument_.registry().entity(sunId); sun != nullptr && sun->sun.has_value()) {
            sun->sun->enabled = hadPrimarySun ? !sun->sun->enabled : true;
            sceneDocument_.markDirty(SceneUpdateKind::LightOnly);
            (void)applyPendingSceneUpdate(false);
            settings = pathTracer_->settings();
        }
    }
    if (!shortcutsBlocked && pressedOnce(GLFW_KEY_F5)) {
        settings.environmentEnabled = !settings.environmentEnabled;
        changed = true;
    }
    if (!shortcutsBlocked && pressedOnce(GLFW_KEY_F6)) {
        settings.directLightingEnabled = !settings.directLightingEnabled;
        changed = true;
    }
    if (!shortcutsBlocked && pressedOnce(GLFW_KEY_F7)) {
        constexpr int count = sizeof(intermediateViews) / sizeof(intermediateViews[0]);
        int idx = 0;
        for (int i = 0; i < count; ++i) {
            if (intermediateViews[i] == settings.debugView) { idx = (i + 1) % count; break; }
        }
        settings.debugView = intermediateViews[idx];
        changed = true;
    }
    if (!shortcutsBlocked && ctrlDown && pressedOnce(GLFW_KEY_R)) {
        pendingReloadShaders_ = true;
    }
    if (!shortcutsBlocked && ctrlDown && pressedOnce(GLFW_KEY_S)) {
        pendingSaveLevel_ = true;
    }
    const bool resetAccumulationPressed = !shortcutsBlocked && !ctrlDown && pressedOnce(GLFW_KEY_R);
    if (resetAccumulationPressed && !viewportInteraction) {
        pathTracer_->resetAccumulation();
    }

    const float exposureRate = 0.9f * deltaSeconds;
    if (!shortcutsBlocked && (glfwGetKey(window_, GLFW_KEY_EQUAL) == GLFW_PRESS || glfwGetKey(window_, GLFW_KEY_KP_ADD) == GLFW_PRESS)) {
        settings.exposure += exposureRate;
        changed = true;
    }
    if (!shortcutsBlocked && (glfwGetKey(window_, GLFW_KEY_MINUS) == GLFW_PRESS || glfwGetKey(window_, GLFW_KEY_KP_SUBTRACT) == GLFW_PRESS)) {
        settings.exposure = std::max(0.05f, settings.exposure - exposureRate);
        changed = true;
    }

    const float envRate = 1.2f * deltaSeconds;
    if (!shortcutsBlocked && glfwGetKey(window_, GLFW_KEY_PERIOD) == GLFW_PRESS) {
        settings.environmentIntensity += envRate;
        changed = true;
    }
    if (!shortcutsBlocked && glfwGetKey(window_, GLFW_KEY_COMMA) == GLFW_PRESS) {
        settings.environmentIntensity = std::max(0.0f, settings.environmentIntensity - envRate);
        changed = true;
    }
    const float rotationRate = 1.4f * deltaSeconds;
    if (!shortcutsBlocked && glfwGetKey(window_, GLFW_KEY_RIGHT_BRACKET) == GLFW_PRESS) {
        settings.environmentRotation += rotationRate;
        changed = true;
    }
    if (!shortcutsBlocked && glfwGetKey(window_, GLFW_KEY_LEFT_BRACKET) == GLFW_PRESS) {
        settings.environmentRotation -= rotationRate;
        changed = true;
    }

    if (!shortcutsBlocked && pressedOnce(GLFW_KEY_PAGE_UP)) {
        ++settings.maxBounces;
        changed = true;
    }
    if (!shortcutsBlocked && pressedOnce(GLFW_KEY_PAGE_DOWN) && settings.maxBounces > 1) {
        --settings.maxBounces;
        changed = true;
    }
    if (!shortcutsBlocked && pressedOnce(GLFW_KEY_HOME)) {
        ++settings.atrousIterations;
        changed = true;
    }
    if (!shortcutsBlocked && pressedOnce(GLFW_KEY_END) && settings.atrousIterations > 1) {
        --settings.atrousIterations;
        changed = true;
    }

    if (changed) {
        applyRendererSettingsSafely(settings, false);
        settings = pathTracer_->settings();
        std::cout << "Settings changed: debug=" << rendererDebugViewName(settings.debugView)
                  << " tone=" << toneMapperName(settings.toneMapper)
                  << " autoExposure=" << (settings.autoExposureEnabled ? "on" : "off")
                  << " denoiser=" << (settings.denoiserEnabled ? "on" : "off")
                  << " sun=" << (settings.sunlightEnabled ? "on" : "off")
                  << " env=" << (settings.environmentEnabled ? "on" : "off")
                  << " bounces=" << settings.maxBounces << '\n';
    }
}

void Application::updateWindowTitle(float seconds) {
    if (!pathTracer_ || seconds - lastTitleUpdateSeconds_ < 0.25f) {
        return;
    }
    lastTitleUpdateSeconds_ = seconds;

    const RendererSettings& settings = pathTracer_->settings();
    const GpuFrameTimings& timings = pathTracer_->timings();
    std::ostringstream title;
    if (sceneDocument_.dirty()) {
        title << "[Modified] ";
    }
    title << (gltfPath_.has_value() ? gltfPath_->stem().string() : "Untitled")
          << " - Ray Tracing Engine"
          << " | samples " << pathTracer_->sampleCount()
          << " | " << rendererDebugViewName(settings.debugView)
          << " | denoise " << (settings.denoiserEnabled ? "on" : "off")
          << " | sun " << (settings.sunlightEnabled ? "on" : "off")
          << " | env " << (settings.environmentEnabled ? "on" : "off")
          << " | GPU "
          << std::fixed << std::setprecision(2)
          << timings.totalMs() << " ms";
    glfwSetWindowTitle(window_, title.str().c_str());
}

void Application::toggleBorderlessFullscreen() {
    if (window_ == nullptr) {
        return;
    }

    cameraController_.releaseMouse(window_);
    if (!borderlessFullscreen_) {
        glfwGetWindowPos(window_, &windowedX_, &windowedY_);
        glfwGetWindowSize(window_, &windowedWidth_, &windowedHeight_);

        GLFWmonitor* monitor = glfwGetPrimaryMonitor();
        const GLFWvidmode* mode = monitor != nullptr ? glfwGetVideoMode(monitor) : nullptr;
        if (monitor == nullptr || mode == nullptr) {
            return;
        }

        borderlessFullscreen_ = true;
        glfwSetWindowMonitor(window_, monitor, 0, 0, mode->width, mode->height, mode->refreshRate);
    } else {
        borderlessFullscreen_ = false;
        glfwSetWindowMonitor(window_, nullptr, windowedX_, windowedY_, windowedWidth_, windowedHeight_, 0);
    }
}

bool Application::pressedOnce(int key) {
    if (key < 0 || static_cast<size_t>(key) >= keyState_.size()) {
        return false;
    }
    const bool down = glfwGetKey(window_, key) == GLFW_PRESS;
    const bool wasDown = keyState_[static_cast<size_t>(key)] != 0;
    keyState_[static_cast<size_t>(key)] = down ? 1u : 0u;
    return down && !wasDown;
}

} // namespace rtv
