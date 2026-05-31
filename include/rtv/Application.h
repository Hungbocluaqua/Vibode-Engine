#pragma once

#include "rtv/NonCopyable.h"
#include "rtv/RendererDebug.h"
#include "rtv/RendererSettings.h"
#include "rtv/AssetManager.h"
#include "rtv/CameraController.h"
#include "rtv/EditorPanels.h"
#include "rtv/GpuProfiler.h"
#include "rtv/SceneDocument.h"
#include "rtv/SceneEventBus.h"
#include "rtv/SceneToGpuSceneBuilder.h"
#include "rtv/NotificationManager.h"
#include "rtv/UndoStack.h"
#include "rtv/HeadlessDiagnostics.h"

#include <memory>
#include <array>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <future>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

struct GLFWwindow;

namespace rtv {

class CommandSystem;
class BufferUploader;
class ResourceAllocator;
class ResourceDemo;
class PipelineDemo;
class PathTracerRenderer;
class Swapchain;
class UiOverlay;
class UploadContext;
class VulkanContext;

class Application final : private NonCopyable {
public:
    explicit Application(
        RendererDebugView debugView = RendererDebugView::Beauty,
        std::optional<std::filesystem::path> gltfPath = std::nullopt,
        std::optional<std::filesystem::path> hdrPath = std::nullopt,
        std::optional<std::filesystem::path> scenePath = std::nullopt,
        std::optional<bool> denoiserOverride = std::nullopt,
        std::optional<RestirMode> restirModeOverride = std::nullopt,
        std::optional<RenderPreset> renderPresetOverride = std::nullopt,
        std::optional<bool> restirGiOverride = std::nullopt,
        std::optional<bool> opacityMicromapOverride = std::nullopt,
        std::optional<uint32_t> opacityMicromapSubdivisionOverride = std::nullopt,
        bool debugViewOverride = false,
        bool validationCameraMotion = false,
        bool validationObjectMotion = false,
        bool headless = false,
        bool disableAsyncCompute = false,
        bool singleQueueFallback = false,
        bool disableResourceAliasing = false);
    ~Application();

    void run(uint32_t maxFrames = 0);
    void runHeadless(uint32_t warmupFrames, uint32_t totalFrames);
    void renderFrames(uint32_t count);
    [[nodiscard]] bool runDescriptorLifetimeStress(
        const std::filesystem::path& outputPath,
        uint32_t cycles,
        uint32_t framesPerCycle);
    void resetDiagnosticFrameCounter(uint32_t frameIndex = 0);
    void setFrameCaptureCallbacks(std::function<void(uint32_t)> begin, std::function<void(uint32_t)> end);
    void resetAccumulation();
    void applyDebugView(RendererDebugView view);
    [[nodiscard]] bool applyNamedCamera(std::string_view cameraName);
    void onWindowFocusChanged(bool focused);
    void onFilesDropped(int count, const char** paths);

    [[nodiscard]] PathTracerRenderer* pathTracer() { return pathTracer_.get(); }
    [[nodiscard]] const VulkanContext* vulkanContext() const { return context_.get(); }
    [[nodiscard]] ResourceAllocator* resourceAllocator() { return allocator_.get(); }
    [[nodiscard]] BufferUploader* bufferUploader() { return uploader_.get(); }
    [[nodiscard]] UiOverlay* uiOverlay() { return uiOverlay_.get(); }
    [[nodiscard]] Swapchain* swapchain() { return swapchain_.get(); }
    [[nodiscard]] const std::vector<float>& cpuFrameTimings() const { return cpuFrameTimings_; }
    [[nodiscard]] const std::vector<float>& gpuFrameTimings() const { return gpuFrameTimings_; }
    [[nodiscard]] const std::vector<GpuFrameTimings>& perFrameGpuTimings() const { return perFrameGpuTimings_; }
    [[nodiscard]] uint32_t warmupFrameCount() const { return warmupFrameCount_; }

private:
    struct PendingSceneLoadResult {
        std::filesystem::path path;
        AssetManager assets;
        SceneAsset scene;
        std::string error;
    };
    struct RetiredPathTracer {
        std::unique_ptr<PathTracerRenderer> renderer;
        uint64_t releaseFrame = 0;
    };
    enum class SunDragPhase {
        Idle,
        Armed,
        Dragging,
    };
    enum class DirtyScenePromptResult {
        Save,
        Discard,
        Cancel,
    };
    struct SunDragState {
        SunDragPhase phase = SunDragPhase::Idle;
        EntityId entity{};
        Transform originalTransform{};
        std::optional<SceneDocument> beforeDocument;
        double startMouseX = 0.0;
        double startMouseY = 0.0;
        double lastMouseX = 0.0;
        double lastMouseY = 0.0;
        float elevation = 0.97f;
        float azimuth = 0.0f;
        int previousCursorMode = 0;
        double armedTimeSeconds = 0.0;
        bool dragEligible = false;
        bool suppressOpenLevel = false;
    };

    void initWindow();
    void initVulkan();
    void mainLoop(uint32_t maxFrames);
    void applyValidationCameraMotion(uint32_t frameIndex);
    void applyValidationObjectMotion(uint32_t frameIndex);
    void processRuntimeControls(float deltaSeconds);
    void processSunDragControls(bool shortcutsBlocked, bool viewportHovered, bool viewportInteraction, bool ctrlDown);
    void beginSunDragArm(bool dragEligible);
    void startSunDrag(double mouseX, double mouseY);
    void updateSunDrag(double mouseX, double mouseY);
    void finishSunDrag(bool cancel);
    void updateWindowTitle(float seconds);
    void toggleBorderlessFullscreen();
    void reloadGltfScene(const std::filesystem::path& path);
    void requestGltfSceneLoad(const std::filesystem::path& path);
    void pollAsyncSceneLoad();
    void commitLoadedGltfScene(PendingSceneLoadResult&& result);
    void applyEditorRequests(const EditorRequests& requests, bool allowResourceRebuild);
    [[nodiscard]] DirtyScenePromptResult promptDirtySceneBefore(std::string_view action) const;
    [[nodiscard]] bool saveCurrentSceneForDirtyPrompt();
    [[nodiscard]] bool confirmDestructiveSceneAction(std::string_view action);
    [[nodiscard]] bool createProjectFromRequest(const CreateProjectRequest& request);
    [[nodiscard]] bool openProjectFromFile(const std::filesystem::path& projectFile, bool promptForDirtyScene);
    [[nodiscard]] bool closeCurrentProject();
    [[nodiscard]] bool loadProjectStartupScene(const ProjectContext& project);
    [[nodiscard]] bool writeDefaultProjectScene(const ProjectContext& project, std::string_view templateName);
    bool applyPendingSceneUpdate(bool allowResourceRebuild);
    void applyRendererSettingsSafely(const RendererSettings& settings, bool allowRenderResolutionChange);
    void reloadShadersFromEditor();
    void retirePathTracer(std::unique_ptr<PathTracerRenderer> renderer);
    void releaseRetiredPathTracers();
    [[nodiscard]] std::unique_ptr<PathTracerRenderer> makePathTracer(
        const SceneAsset* sceneAsset,
        const AssetManager* assets,
        std::optional<std::filesystem::path> sceneCachePath,
        const RendererSettings* settingsToRestore);
    void createPathTracer(const RendererSettings* settingsToRestore = nullptr);
    void applyActiveSceneCamera();
    void syncActiveSceneCameraFromController();
    void rebuildGpuSceneAsset();
    void initializeFallbackSceneDocument();
    [[nodiscard]] bool pressedOnce(int key);

    GLFWwindow* window_ = nullptr;
    bool headless_ = false;
    uint32_t nextDiagnosticFrameIndex_ = 0;
    uint32_t warmupFrameCount_ = 0;
    uint32_t totalFrameCount_ = 0;
    std::vector<float> cpuFrameTimings_;
    std::vector<float> gpuFrameTimings_;
    std::vector<GpuFrameTimings> perFrameGpuTimings_;
    std::function<void(uint32_t)> beginFrameCapture_;
    std::function<void(uint32_t)> endFrameCapture_;
    RendererDebugView debugView_ = RendererDebugView::Beauty;
    std::optional<std::filesystem::path> gltfPath_;
    std::optional<std::filesystem::path> hdrPath_;
    std::optional<std::filesystem::path> scenePath_;
    bool sceneUnsavedDirty_ = false;
    std::optional<ProjectContext> project_;
    AssetRegistry assetRegistry_;
    std::optional<bool> denoiserOverride_;
    std::optional<RestirMode> restirModeOverride_;
    std::optional<RenderPreset> renderPresetOverride_;
    std::optional<bool> restirGiOverride_;
    std::optional<bool> opacityMicromapOverride_;
    std::optional<uint32_t> opacityMicromapSubdivisionOverride_;
    bool debugViewOverride_ = false;
    bool validationCameraMotion_ = false;
    bool validationObjectMotion_ = false;
    EntityId validationObjectMotionEntity_{};
    Transform validationObjectMotionBaseTransform_{};
    bool disableAsyncCompute_ = false;
    bool singleQueueFallback_ = false;
    bool disableResourceAliasing_ = false;
    bool pendingOpenLevel_ = false;
    bool pendingSaveLevel_ = false;
    bool pendingReloadShaders_ = false;
    AssetManager assets_;
    CameraController cameraController_;
    SunDragState sunDrag_{};
    std::array<unsigned char, 512> keyState_{};
    float lastFrameSeconds_ = 0.0f;
    uint64_t frameSerial_ = 0;
    float lastTitleUpdateSeconds_ = -1.0f;
    bool borderlessFullscreen_ = false;
    int windowedX_ = 100;
    int windowedY_ = 100;
    int windowedWidth_ = 1280;
    int windowedHeight_ = 720;
    std::optional<SceneAsset> importedScene_;
    SceneDocument sceneDocument_;
    SceneEventBus sceneEventBus_;
    NotificationManager notifications_;
    UndoStack undoStack_;
    SceneToGpuSceneBuilder sceneBuilder_;
    std::optional<SceneAsset> gpuSceneAsset_;
    std::vector<EntityId> gpuInstanceEntities_;
    std::unique_ptr<VulkanContext> context_;
    std::unique_ptr<ResourceAllocator> allocator_;
    std::unique_ptr<UploadContext> uploadContext_;
    std::unique_ptr<BufferUploader> uploader_;
    std::unique_ptr<Swapchain> swapchain_;
    std::unique_ptr<CommandSystem> commandSystem_;
    std::unique_ptr<UiOverlay> uiOverlay_;
    std::unique_ptr<ResourceDemo> resourceDemo_;
    std::unique_ptr<PipelineDemo> pipelineDemo_;
    std::unique_ptr<PathTracerRenderer> pathTracer_;
    std::vector<RetiredPathTracer> retiredPathTracers_;
    std::optional<RendererSettings> pendingPostFrameSettings_;
    std::future<PendingSceneLoadResult> pendingSceneLoad_;
    std::optional<std::filesystem::path> pendingSceneLoadPath_;
    std::string sceneLoadingStatus_;
};

} // namespace rtv
