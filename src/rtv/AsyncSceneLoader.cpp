#include "rtv/AsyncSceneLoader.h"

#include "rtv/GltfLoader.h"

#include <chrono>
#include <exception>
#include <utility>

namespace rtv {

namespace {
bool cancelled(const std::shared_ptr<std::atomic_bool>& cancelFlag) {
    return cancelFlag != nullptr && cancelFlag->load();
}

SceneLoadResult makeCancelledResult(const SceneLoadRequest& request) {
    SceneLoadResult result;
    result.mode = request.mode;
    result.sourcePath = request.sourcePath;
    result.cancelled = true;
    result.errorMessage = "Scene load cancelled";
    return result;
}

} // namespace

const char* sceneLoadModeLabel(SceneLoadMode mode) {
    switch (mode) {
    case SceneLoadMode::OpenRtLevel:
        return "Open Scene";
    case SceneLoadMode::ImportSceneAsNewScene:
        return "Import Scene as New Scene";
    case SceneLoadMode::MergeSceneIntoCurrent:
        return "Merge Scene";
    case SceneLoadMode::LoadProjectStartupScene:
        return "Load Project Startup Scene";
    }
    return "Scene Load";
}

const char* sceneLoadStatusLabel(SceneLoadStatus status) {
    switch (status) {
    case SceneLoadStatus::Idle:
        return "Idle";
    case SceneLoadStatus::Queued:
        return "Queued";
    case SceneLoadStatus::Loading:
        return "Loading";
    case SceneLoadStatus::WaitingForApply:
        return "Waiting for apply";
    case SceneLoadStatus::Applying:
        return "Applying";
    case SceneLoadStatus::Completed:
        return "Completed";
    case SceneLoadStatus::Failed:
        return "Failed";
    case SceneLoadStatus::Cancelled:
        return "Cancelled";
    }
    return "Unknown";
}

AsyncSceneLoader::~AsyncSceneLoader() {
    requestCancel();
    wait();
}

bool AsyncSceneLoader::start(SceneLoadRequest request) {
    if (future_.valid() && future_.wait_for(std::chrono::seconds(0)) != std::future_status::ready) {
        return false;
    }
    if (future_.valid()) {
        (void)future_.get();
    }

    cancelRequested_ = std::make_shared<std::atomic_bool>(false);
    setStage("Queued", 0.0f, SceneLoadStatus::Queued);
    future_ = std::async(std::launch::async, [this, request = std::move(request), cancelFlag = cancelRequested_]() mutable {
        return load(std::move(request), cancelFlag);
    });
    return true;
}

void AsyncSceneLoader::requestCancel() {
    if (cancelRequested_ != nullptr) {
        cancelRequested_->store(true);
        if (status_.load() == SceneLoadStatus::Queued || status_.load() == SceneLoadStatus::Loading) {
            setStage("Cancellation requested", progress_.load(), SceneLoadStatus::Loading);
        }
    }
}

void AsyncSceneLoader::wait() {
    if (future_.valid()) {
        (void)future_.get();
    }
    cancelRequested_.reset();
    if (status_.load() == SceneLoadStatus::WaitingForApply) {
        setStage("Idle", 0.0f, SceneLoadStatus::Idle);
    }
}

bool AsyncSceneLoader::isRunning() {
    if (!future_.valid()) {
        return false;
    }
    return future_.wait_for(std::chrono::seconds(0)) != std::future_status::ready;
}

std::string AsyncSceneLoader::stage() const {
    std::lock_guard<std::mutex> lock(stageMutex_);
    return stage_;
}

bool AsyncSceneLoader::hasCompletedResult() {
    if (!future_.valid()) {
        return false;
    }
    return future_.wait_for(std::chrono::seconds(0)) == std::future_status::ready;
}

SceneLoadResult AsyncSceneLoader::takeCompletedResult() {
    SceneLoadResult result = future_.get();
    cancelRequested_.reset();
    if (result.cancelled) {
        setStage("Cancelled", 0.0f, SceneLoadStatus::Cancelled);
    } else if (!result.success) {
        setStage("Failed", 0.0f, SceneLoadStatus::Failed);
    } else {
        setStage("Waiting for main thread", 1.0f, SceneLoadStatus::WaitingForApply);
    }
    return result;
}

void AsyncSceneLoader::setStage(std::string stage, float progress, SceneLoadStatus status) {
    {
        std::lock_guard<std::mutex> lock(stageMutex_);
        stage_ = std::move(stage);
    }
    progress_.store(progress);
    status_.store(status);
}

SceneLoadResult AsyncSceneLoader::load(SceneLoadRequest request, const std::shared_ptr<std::atomic_bool>& cancelFlag) {
    SceneLoadResult result;
    result.mode = request.mode;
    result.sourcePath = request.sourcePath;

    try {
        setStage("Reading file", 0.05f, SceneLoadStatus::Loading);
        if (cancelled(cancelFlag)) {
            return makeCancelledResult(request);
        }

        switch (request.mode) {
        case SceneLoadMode::OpenRtLevel:
        case SceneLoadMode::LoadProjectStartupScene: {
            setStage("Parsing scene", 0.20f, SceneLoadStatus::Loading);
            auto document = std::make_unique<SceneDocument>();
            if (!document->loadJson(request.sourcePath)) {
                result.errorMessage = "Scene JSON load failed";
                return result;
            }
            if (cancelled(cancelFlag)) {
                return makeCancelledResult(request);
            }

            setStage("Resolving assets", 0.45f, SceneLoadStatus::Loading);
            if (document->sourceGltfPath().has_value() && std::filesystem::exists(*document->sourceGltfPath())) {
                try {
                    setStage("Loading referenced glTF", 0.60f, SceneLoadStatus::Loading);
                    GltfLoader loader(result.assets);
                    result.importedScene = loader.loadWithCache(*document->sourceGltfPath());
                } catch (const std::exception& error) {
                    result.warningMessage = "Referenced glTF load failed: " + std::string(error.what());
                }
            }
            if (cancelled(cancelFlag)) {
                return makeCancelledResult(request);
            }

            setStage("Building CPU scene", 0.85f, SceneLoadStatus::Loading);
            result.stagedScene = std::move(document);
            result.success = true;
            break;
        }
        case SceneLoadMode::ImportSceneAsNewScene: {
            setStage("Parsing glTF", 0.25f, SceneLoadStatus::Loading);
            GltfLoader loader(result.assets);
            result.importedScene = loader.loadWithCache(request.sourcePath);
            if (cancelled(cancelFlag)) {
                return makeCancelledResult(request);
            }

            setStage("Building CPU scene", 0.80f, SceneLoadStatus::Loading);
            auto document = std::make_unique<SceneDocument>();
            document->importSceneAsset(*result.importedScene);
            document->setSourceGltfPath(request.sourcePath);
            result.stagedScene = std::move(document);
            result.success = true;
            break;
        }
        case SceneLoadMode::MergeSceneIntoCurrent: {
            setStage("Parsing glTF", 0.25f, SceneLoadStatus::Loading);
            GltfLoader loader(result.assets);
            result.importedScene = loader.loadWithCache(request.sourcePath);
            if (cancelled(cancelFlag)) {
                return makeCancelledResult(request);
            }
            setStage("Building CPU scene", 0.85f, SceneLoadStatus::Loading);
            result.success = true;
            break;
        }
        }

        if (cancelled(cancelFlag)) {
            return makeCancelledResult(request);
        }
        setStage("Waiting for main thread", 1.0f, SceneLoadStatus::WaitingForApply);
    } catch (const std::exception& error) {
        result.success = false;
        result.errorMessage = error.what();
    }

    return result;
}

} // namespace rtv
