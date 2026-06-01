#pragma once

#include "rtv/EditorCommands.h"
#include "rtv/EditorPanels.h"

#include <array>

namespace rtv {

enum class GizmoInteractionState : uint8_t {
    Idle,
    Hovered,
    DraggingTranslate,
    DraggingRotate,
    DraggingScale,
};

class ViewportPanel {
public:
    void draw(EditorRuntimeState& state, EditorSelection& selection, EditorRequests& requests);

    [[nodiscard]] VkExtent2D desiredRenderExtent(VkExtent2D fallback) const;
    [[nodiscard]] bool interactionActive() const { return focused_ || hovered_; }
    [[nodiscard]] bool hovered() const { return hovered_; }
    [[nodiscard]] GizmoInteractionState gizmoState() const { return gizmoState_; }

    void setShowGrid(bool show) { showGrid_ = show; }
    void setShowAxes(bool show) { showAxes_ = show; }
    [[nodiscard]] bool showGrid() const { return showGrid_; }
    [[nodiscard]] bool showAxes() const { return showAxes_; }

private:
    struct SnapSettings {
        bool enabled = false;
        float translation = 0.25f;
        float rotation = 15.0f;
        float scale = 0.1f;
    };

    void commitGizmoDrag(EditorRequests& requests, SceneDocument& document);
    void abortGizmoDrag();
    void executeCommand(EditorCommandId id);
    void updateGizmoState(bool isOver, bool isUsing, int gizmoMode);

    VkExtent2D lastContentExtent_{};
    int transformGizmoMode_ = 0;
    bool localGizmoMode_ = false;
    SnapSettings snap_{};
    bool focused_ = false;
    bool hovered_ = false;
    GizmoInteractionState gizmoState_ = GizmoInteractionState::Idle;

    bool gizmoDragActive_ = false;
    EntityId gizmoDragEntity_{};
    Transform gizmoDragOriginal_{};

    bool showGrid_ = false;
    bool showAxes_ = true;
    bool showSelectionOverlay_ = true;

    uint32_t lastSampleCount_ = 0;
};

} // namespace rtv
