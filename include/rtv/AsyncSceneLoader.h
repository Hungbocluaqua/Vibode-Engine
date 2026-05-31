#pragma once

#include "rtv/AssetManager.h"
#include "rtv/Project.h"
#include "rtv/SceneDocument.h"

#include <atomic>
#include <filesystem>
#include <future>
#include <memory>
#include <mutex>
#include <optional>
#include <string>

namespace rtv {

enum class SceneLoadMode {
    OpenRtLevel,
    ImportSceneAsNewScene,
    MergeSceneIntoCurrent,
    LoadProjectStartupScene,
};

enum class SceneLoadStatus {
    Idle,
    Queued,
    Loading,
    WaitingForApply,
    Applying,
    Completed,
    Failed,
    Cancelled,
};

struct SceneLoadRequest {
    SceneLoadMode mode = SceneLoadMode::OpenRtLevel;
    std::filesystem::path sourcePath;
    std::optional<ProjectContext> projectSnapshot;
    bool preserveHierarchy = true;
    bool importMaterials = true;
    bool importTextures = true;
    bool importLights = true;
    bool importCameras = true;
};

struct SceneLoadResult {
    SceneLoadMode mode = SceneLoadMode::OpenRtLevel;
    std::filesystem::path sourcePath;
    bool success = false;
    bool cancelled = false;
    std::string errorMessage;
    std::string warningMessage;
    std::unique_ptr<SceneDocument> stagedScene;
    AssetManager assets;
    std::optional<SceneAsset> importedScene;
    bool needsRendererRebuild = true;
    bool needsGpuSceneRebuild = true;
    bool needsTlasRebuild = true;
    bool needsEnvironmentCdfRebuild = false;
    bool needsAtmosphereLutRebuild = false;
};

class AsyncSceneLoader final {
public:
    AsyncSceneLoader() = default;
    ~AsyncSceneLoader();

    AsyncSceneLoader(const AsyncSceneLoader&) = delete;
    AsyncSceneLoader& operator=(const AsyncSceneLoader&) = delete;

    bool start(SceneLoadRequest request);
    void requestCancel();
    void wait();

    [[nodiscard]] bool isRunning();
    [[nodiscard]] SceneLoadStatus status() const { return status_.load(); }
    [[nodiscard]] float progress() const { return progress_.load(); }
    [[nodiscard]] std::string stage() const;
    [[nodiscard]] bool hasCompletedResult();
    [[nodiscard]] SceneLoadResult takeCompletedResult();

private:
    void setStage(std::string stage, float progress, SceneLoadStatus status);
    [[nodiscard]] SceneLoadResult load(SceneLoadRequest request, const std::shared_ptr<std::atomic_bool>& cancelFlag);

    std::atomic<SceneLoadStatus> status_{SceneLoadStatus::Idle};
    std::atomic<float> progress_{0.0f};
    std::shared_ptr<std::atomic_bool> cancelRequested_;
    mutable std::mutex stageMutex_;
    std::string stage_ = "Idle";
    std::future<SceneLoadResult> future_;
};

[[nodiscard]] const char* sceneLoadModeLabel(SceneLoadMode mode);
[[nodiscard]] const char* sceneLoadStatusLabel(SceneLoadStatus status);

} // namespace rtv
