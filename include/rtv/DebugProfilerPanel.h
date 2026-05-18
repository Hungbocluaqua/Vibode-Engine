#pragma once

#include "rtv/EditorPanels.h"

namespace rtv {

class DebugProfilerPanel {
public:
    void draw(EditorRuntimeState& state, EditorRequests& requests);
};

} // namespace rtv
