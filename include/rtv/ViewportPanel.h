#pragma once

#include "rtv/EditorPanels.h"

namespace rtv {

class ViewportPanel {
public:
    void draw(EditorRuntimeState& state, EditorRequests& requests);

    [[nodiscard]] VkExtent2D desiredRenderExtent(VkExtent2D fallback) const;
    [[nodiscard]] bool interactionActive() const { return focused_ || hovered_; }
    [[nodiscard]] bool hovered() const { return hovered_; }

private:
    VkExtent2D lastContentExtent_{};
    bool focused_ = false;
    bool hovered_ = false;
};

} // namespace rtv
