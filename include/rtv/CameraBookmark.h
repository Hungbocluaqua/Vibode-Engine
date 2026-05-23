#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include <glm/glm.hpp>

namespace rtv {

class CameraController;
class PathTracerRenderer;
enum class RendererDebugView : uint32_t;
struct RendererSettings;

struct CameraBookmark {
    std::string name;
    glm::vec3 position{};
    glm::vec3 forward{};
    float fovY = 60.0f;
    std::optional<float> exposure;
    std::optional<float> envRotation;
    std::optional<uint32_t> debugView;
};

class CameraBookmarkManager {
public:
    void saveBookmark(const CameraController& camera, std::string name,
                      const RendererSettings* settings = nullptr);
    void loadBookmark(const CameraBookmark& bookmark,
                      CameraController& camera, class PathTracerRenderer& renderer,
                      RendererSettings* settings = nullptr) const;
    void deleteBookmark(size_t index);
    [[nodiscard]] const std::vector<CameraBookmark>& bookmarks() const { return bookmarks_; }
    void serialize(class SceneDocument& document) const;
    void deserialize(const class SceneDocument& document);
    void clear();

    [[nodiscard]] size_t count() const { return bookmarks_.size(); }

private:
    std::vector<CameraBookmark> bookmarks_;
};

} // namespace rtv
