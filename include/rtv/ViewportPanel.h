#pragma once

#include "rtv/EditorCommands.h"
#include "rtv/EditorPanels.h"

#include <glm/mat4x4.hpp>

#include <array>
#include <filesystem>
#include <string>

namespace rtv {

enum class GizmoInteractionState : uint8_t {
    Idle,
    Hovered,
    DraggingTranslate,
    DraggingRotate,
    DraggingScale,
};

enum class ViewportPlacementBrushKind : uint8_t {
    None,
    Prefab,
    Mesh,
};

struct ViewportPlacementBrushState {
    ViewportPlacementBrushKind kind = ViewportPlacementBrushKind::None;
    AssetGuid guid;
    float yawRadians = 0.0f;
    int remainingPlacements = 0;
    bool multiPlace = false;

    [[nodiscard]] bool active() const { return kind != ViewportPlacementBrushKind::None && !guid.empty(); }
};

class ViewportPanel {
public:
    void draw(EditorRuntimeState& state, EditorSelection& selection, EditorRequests& requests);

    [[nodiscard]] VkExtent2D desiredRenderExtent(VkExtent2D fallback) const;
    [[nodiscard]] bool interactionActive() const { return focused_ || hovered_; }
    [[nodiscard]] bool hovered() const { return hovered_; }
    [[nodiscard]] GizmoInteractionState gizmoState() const { return gizmoState_; }
    void executeCommand(EditorCommandId id);

    void setShowGrid(bool show) { showGrid_ = show; }
    void setShowAxes(bool show) { showAxes_ = show; }
    [[nodiscard]] bool showGrid() const { return showGrid_; }
    [[nodiscard]] bool showAxes() const { return showAxes_; }
    void reloadViewportPreferences(const EditorPreferences& preferences);

private:
    struct SnapSettings {
        bool enabled = false;
        float translation = 0.25f;
        float rotation = 15.0f;
        float scale = 0.1f;
    };

    void commitGizmoDrag(EditorRequests& requests, SceneDocument& document);
    void abortGizmoDrag();
    void updateGizmoState(bool isOver, bool isUsing, int gizmoMode);
    void persistViewportPreferences(EditorPreferences& preferences, const std::filesystem::path& path) const;

    VkExtent2D lastContentExtent_{};
    int transformGizmoMode_ = 0;
    bool localGizmoMode_ = false;
    SnapSettings snap_{};
    bool viewportPreferencesLoaded_ = false;
    bool focused_ = false;
    bool hovered_ = false;
    bool rightMouseContextCandidate_ = false;
    bool rightMouseContextSuppressed_ = false;
    float rightMouseContextHoldSeconds_ = 0.0f;
    bool levelDropPopupOpen_ = false;
    std::filesystem::path pendingLevelDropPath_;
    std::string pendingLevelDropLabel_;
    bool materialDropPopupOpen_ = false;
    AssetGuid pendingMaterialDropGuid_;
    EntityId pendingMaterialDropEntity_{};
    std::string activePlacementPayloadKey_;
    float placementPreviewYawRadians_ = 0.0f;
    ViewportPlacementBrushState placementBrush_{};
    GizmoInteractionState gizmoState_ = GizmoInteractionState::Idle;

    bool gizmoDragActive_ = false;
    bool gizmoDragModified_ = false;
    EntityId gizmoDragEntity_{};
    Transform gizmoDragOriginal_{};
    glm::mat4 gizmoDragParentWorld_{1.0f};
    glm::mat4 gizmoDragOriginalWorld_{1.0f};

    bool showGrid_ = false;
    bool showAxes_ = true;
    bool showSelectionOverlay_ = true;
    bool showActorIcons_ = true;
    bool pickMeshEntities_ = true;
    bool pickActorIcons_ = true;

    uint32_t lastSampleCount_ = 0;
};

} // namespace rtv
