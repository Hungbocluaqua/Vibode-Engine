#pragma once

#include "rtv/EditorPanels.h"

namespace rtv {

class ViewportPanel {
public:
    void draw(EditorRuntimeState& state, EditorSelection& selection, EditorRequests& requests);

    [[nodiscard]] VkExtent2D desiredRenderExtent(VkExtent2D fallback) const;
    [[nodiscard]] bool interactionActive() const { return focused_ || hovered_; }
    [[nodiscard]] bool hovered() const { return hovered_; }

private:
    struct SnapSettings {
        bool enabled = false;
        float translation = 0.25f;
        float rotation = 15.0f;
        float scale = 0.1f;
    };

    VkExtent2D lastContentExtent_{};
    int transformGizmoMode_ = 0;
    bool localGizmoMode_ = false;
    SnapSettings snap_{};
    bool focused_ = false;
    bool hovered_ = false;
};

} // namespace rtv
