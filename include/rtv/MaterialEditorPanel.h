#pragma once

#include "rtv/EditorPanels.h"

namespace rtv {

class MaterialEditorPanel {
public:
    void draw(const EditorRuntimeState& state, const EditorSelection& selection, EditorRequests& requests);
};

} // namespace rtv
