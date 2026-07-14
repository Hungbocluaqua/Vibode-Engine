#pragma once

#include "rtv/EditorPanels.h"

namespace rtv {

class SceneStatsPanel {
public:
    void draw(const EditorRuntimeState& state, EditorRequests& requests);
};

} // namespace rtv
