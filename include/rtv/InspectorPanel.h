#pragma once

#include "rtv/EditorPanels.h"

namespace rtv {

class InspectorPanel {
public:
    void draw(const EditorRuntimeState& state, EditorSelection& selection, EditorRequests& requests);
};

} // namespace rtv
