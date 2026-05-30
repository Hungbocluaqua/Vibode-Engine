#include "rtv/CameraBookmark.h"

#include "rtv/CameraController.h"
#include "rtv/PathTracerRenderer.h"
#include "rtv/SceneDocument.h"

#include <algorithm>
#include <fstream>

#include <nlohmann/json.hpp>

namespace rtv {

void CameraBookmarkManager::saveBookmark(
    const CameraController& camera,
    std::string name,
    const RendererSettings* settings) {
    CameraBookmark bookmark;
    bookmark.name = std::move(name);
    bookmark.position = camera.position();
    bookmark.forward = camera.direction();
    bookmark.fovY = 60.0f;
    if (settings != nullptr) {
        bookmark.exposure = settings->exposure;
        bookmark.envRotation = settings->environmentRotation;
        bookmark.debugView = static_cast<uint32_t>(settings->debugView);
    }
    bookmarks_.push_back(std::move(bookmark));
}

void CameraBookmarkManager::loadBookmark(
    const CameraBookmark& bookmark,
    CameraController& camera,
    PathTracerRenderer& renderer,
    RendererSettings* settings) const {
    camera.setPose(bookmark.position, bookmark.forward, renderer);
    renderer.setCameraFovY(bookmark.fovY);
    if (settings != nullptr) {
        if (bookmark.exposure.has_value()) {
            settings->exposure = *bookmark.exposure;
        }
        if (bookmark.envRotation.has_value()) {
            settings->environmentRotation = *bookmark.envRotation;
        }
        if (bookmark.debugView.has_value()) {
            settings->debugView = static_cast<RendererDebugView>(*bookmark.debugView);
        }
    }
}

void CameraBookmarkManager::deleteBookmark(size_t index) {
    if (index < bookmarks_.size()) {
        bookmarks_.erase(bookmarks_.begin() + static_cast<std::ptrdiff_t>(index));
    }
}

void CameraBookmarkManager::clear() {
    bookmarks_.clear();
}

void CameraBookmarkManager::serialize(SceneDocument& document) const {
    nlohmann::json arr = nlohmann::json::array();
    for (const CameraBookmark& bm : bookmarks_) {
        nlohmann::json entry;
        entry["name"] = bm.name;
        entry["position"] = nlohmann::json::array({bm.position.x, bm.position.y, bm.position.z});
        entry["forward"] = nlohmann::json::array({bm.forward.x, bm.forward.y, bm.forward.z});
        entry["fovY"] = bm.fovY;
        if (bm.exposure.has_value()) entry["exposure"] = *bm.exposure;
        if (bm.envRotation.has_value()) entry["envRotation"] = *bm.envRotation;
        if (bm.debugView.has_value()) entry["debugView"] = *bm.debugView;
        arr.push_back(std::move(entry));
    }
    document.setBookmarksJson(arr);
}

void CameraBookmarkManager::deserialize(const SceneDocument& document) {
    bookmarks_.clear();
    const auto& json = document.bookmarksJson();
    if (!json.has_value() || !json->is_array()) {
        return;
    }
    for (const auto& entry : *json) {
        CameraBookmark bm;
        bm.name = entry.value("name", std::string{});
        if (entry.contains("position") && entry["position"].is_array() && entry["position"].size() >= 3) {
            bm.position = glm::vec3(entry["position"][0].get<float>(), entry["position"][1].get<float>(), entry["position"][2].get<float>());
        }
        if (entry.contains("forward") && entry["forward"].is_array() && entry["forward"].size() >= 3) {
            bm.forward = glm::vec3(entry["forward"][0].get<float>(), entry["forward"][1].get<float>(), entry["forward"][2].get<float>());
        }
        bm.fovY = entry.value("fovY", 60.0f);
        if (entry.contains("exposure")) bm.exposure = entry["exposure"].get<float>();
        if (entry.contains("envRotation")) bm.envRotation = entry["envRotation"].get<float>();
        if (entry.contains("debugView")) bm.debugView = entry["debugView"].get<uint32_t>();
        bookmarks_.push_back(std::move(bm));
    }
}

} // namespace rtv
